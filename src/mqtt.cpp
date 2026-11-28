/**
 * @file mqtt.cpp
 * @brief Adafruit IO MQTT communications, subscription handlers, and publication coordinator.
 *
 * Handles Wi-Fi/MQTT reconnection with exponential backoff, inbound command subscriptions,
 * round-robin sensor publishing (3s period), clinical and facility event publishing,
 * 60s summary reports, and local 64-record FIFO queuing and replay during transport drop.
 */

#include "mqtt.h"
#include "app.h"
#include "state.h"
#include "parser_utils.h"
#include "medication.h"
#include "connectivity.h"
#include "watchdog.h"
#include "esp_task_wdt.h"
#include "adafruit_io_config.h"
#include <PubSubClient.h>
#include <atomic>
#include "environment.h"
#include "offline.h"

PubSubClient mqttClient(wifiClient);

static std::atomic<bool> mqttConnectedFlag{false};
static std::atomic<bool> simForceWifiOffline{false};
static std::atomic<bool> simForceMqttOffline{false};
constexpr uint32_t ALERT_ESCALATION_COOLDOWN_MS = 2000;

/**
 * @brief Sets simulated Wi-Fi disconnect flag for Task 6 demonstration in Wokwi.
 *
 * This function only mutates the atomic flag; actual link disconnect and buffering
 * are handled within mqttTask to preserve thread safety.
 *
 * @param enable True to simulate Wi-Fi disconnect; false to restore normal operation.
 */
void setSimulateOfflineWifi(bool enable) {
    simForceWifiOffline = enable;
}

/**
 * @brief Sets simulated MQTT broker disconnect flag for Task 6 demonstration in Wokwi.
 *
 * This function only mutates the atomic flag; actual broker teardown and buffering
 * are handled within mqttTask to preserve thread safety.
 *
 * @param enable True to simulate broker disconnect; false to restore normal operation.
 */
void setSimulateOfflineMqtt(bool enable) {
    simForceMqttOffline = enable;
}

/**
 * @brief Queries whether simulated Wi-Fi disconnect flag is set.
 *
 * @return true if simulated Wi-Fi drop is active; false otherwise.
 */
bool isSimulateOfflineWifi() {
    return simForceWifiOffline;
}

/**
 * @brief Thread-safe query of active MQTT connection status.
 *
 * Reflects broker connection status filtered through simulation override flags.
 *
 * @return true if connected to broker and not in simulated offline mode.
 */
bool isMqttClientConnected() {
    return mqttConnectedFlag && !simForceMqttOffline && !simForceWifiOffline;
}

const uint32_t MQTT_TASK_INTERVAL_MS = 3000;          ///< Telemetry and replay dispatch period (3 s = max 20 live/replay slots/min)
const uint32_t MQTT_LOOP_INTERVAL_MS = 50;            ///< Underlying client loop tick rate (50 ms)
const uint32_t MQTT_INITIAL_RETRY_DELAY_MS = 1000;    ///< Initial retry delay for exponential backoff (1 s)
const uint32_t MQTT_MAX_RETRY_DELAY_MS = 30000;       ///< Cap for exponential backoff retry delay (30 s)
const uint32_t SUMMARY_INTERVAL_MS = 60000;           ///< Aggregated hospital summary reporting cadence (60 s)
const uint32_t ALERT_PUBLISH_INTERVAL_MS = 60000;     ///< Rate limiter for repeated alert publications (60 s)
const uint16_t MQTT_SUMMARY_BUFFER_MARGIN = 64;       ///< Additional headroom bytes for PubSubClient packet buffer


#define MEDICAL_HEART_RATE_FEED "medical-heart-rate"
#define MEDICAL_SPO2_FEED "medical-spo2"
#define MEDICAL_BODY_TEMPERATURE_FEED "medical-body-temperature"
#define FACILITY_ROOM_TEMPERATURE_FEED "facility-room-temperature"
#define FACILITY_OXYGEN_FEED "facility-oxygen"
#define FACILITY_AQI_FEED "facility-aqi"
#define HOSPITAL_EVENTS_FEED "hospital-events"
#define MEDICATION_DOSAGE_FEED "medication-dosage"
#define SAMPLING_RATE_FEED "sampling-rate"
#define BED_ANGLE_FEED "bed-angle"

enum LiveFeedIndex {
    LIVE_MEDICAL_HEART_RATE,
    LIVE_MEDICAL_SPO2,
    LIVE_MEDICAL_BODY_TEMPERATURE,
    LIVE_FACILITY_ROOM_TEMPERATURE,
    LIVE_FACILITY_OXYGEN,
    LIVE_FACILITY_AQI,
    LIVE_FEED_COUNT
};

bool liveFeedPublished[LIVE_FEED_COUNT] = {false};
int lastPublishedHeartRate = 0;
int lastPublishedSpo2 = 0;
float lastPublishedBodyTemperature = 0.0f;
float lastPublishedRoomTemperature = 0.0f;
float lastPublishedOxygen = 0.0f;
int lastPublishedAqi = 0;

/**
 * @brief Builds the Adafruit IO MQTT topic for a configured feed name.
 */
String getAioFeedTopic(const char *feedName)
{
    return String(AIO_USERNAME) + "/feeds/" + String(feedName);
}

/**
 * @brief Formats a medical or facility alert for the hospital-events feed.
 */
String buildAlertPayload(const char *role, const char *parameter, float value, AlertSeverity severity, const char *message)
{
    const char *severityText = "NORMAL";
    if (severity == ALERT_SEVERITY_WARNING) {
        severityText = "WARNING";
    } else if (severity == ALERT_SEVERITY_CRITICAL) {
        severityText = "CRITICAL";
    }

    char payload[260];
    snprintf(payload, sizeof(payload),
             "%s ALERT\nParameter: %s\nValue: %.2f\nSeverity: %s\nMessage: %s",
             role, parameter, value, severityText, message);
    return String(payload);
}

/**
 * @brief Combines aggregated medical and facility snapshots into one payload.
 *
 * The resulting message is published periodically through hospital-events.
 */
String buildHospitalSummaryPayload(const char *averageHeartRateStr, const char *minimumSpo2Str, const char *maxBodyTempStr,
                                  uint32_t medicalSampleCount, uint32_t medicalAbnormalCount, uint32_t medicalCriticalCount,
                                  const char *averageRoomTempStr, const char *averageOxygenStr, const char *averageAqiStr,
                                  uint32_t facilitySampleCount, uint32_t facilityAbnormalCount, uint32_t facilityCriticalCount,
                                  const char *medicalStatusOverride = nullptr)
{
    const char *medicalStatus = (medicalStatusOverride != nullptr)
                                    ? medicalStatusOverride
                                    : ((medicalSampleCount == 0 && medicalCriticalCount == 0 && medicalAbnormalCount == 0)
                                           ? "N/A"
                                           : ((medicalCriticalCount > 0) ? "CRITICAL" : (medicalAbnormalCount > 0 ? "WARNING" : "NORMAL")));
    const char *facilityStatus = (facilitySampleCount == 0 && facilityCriticalCount == 0 && facilityAbnormalCount == 0) ? "N/A" : ((facilityCriticalCount > 0) ? "CRITICAL" : (facilityAbnormalCount > 0 ? "WARNING" : "NORMAL"));

    char payload[620];
    snprintf(payload, sizeof(payload),
             "HOSPITAL SUMMARY\n==============================\n\nMEDICAL SUMMARY\n\nAverage HR: %s\nMinimum SpO2: %s\nMaximum Body Temperature: %s\n\nSamples: %lu\nAbnormal Readings: %lu\n\nStatus: %s\n\n\nFACILITY SUMMARY\n\nAverage Room Temperature: %s\nAverage Oxygen: %s\nAverage AQI: %s\n\nSnapshots: %lu\nAbnormal Readings: %lu\n\nStatus: %s",
             averageHeartRateStr, minimumSpo2Str, maxBodyTempStr,
             static_cast<unsigned long>(medicalSampleCount), static_cast<unsigned long>(medicalAbnormalCount),
             medicalStatus,
             averageRoomTempStr, averageOxygenStr, averageAqiStr,
             static_cast<unsigned long>(facilitySampleCount), static_cast<unsigned long>(facilityAbnormalCount),
             facilityStatus);
    return String(payload);
}

/**
 * @brief Builds a condensed summary payload (up to 220-byte buffer) when full summary cannot fit.
 */
String buildCompactHospitalSummaryPayload(const char *averageHeartRateStr, const char *minimumSpo2Str, const char *maxBodyTempStr,
                                         uint32_t medicalAbnormalCount, uint32_t medicalCriticalCount,
                                         const char *averageOxygenStr, uint32_t facilityAbnormalCount, uint32_t facilityCriticalCount,
                                         const char *medicalStatusOverride = nullptr,
                                         const char *facilityStatusOverride = nullptr)
{
    const char *medStat = (medicalStatusOverride != nullptr)
                              ? medicalStatusOverride
                              : ((medicalCriticalCount > 0) ? "CRIT" : (medicalAbnormalCount > 0 ? "WARN" : "NORM"));
    const char *facStat = (facilityStatusOverride != nullptr)
                              ? facilityStatusOverride
                              : ((facilityCriticalCount > 0) ? "CRIT" : (facilityAbnormalCount > 0 ? "WARN" : "NORM"));

    char payload[220];
    snprintf(payload, sizeof(payload),
             "SUMMARY: Med[HR:%s,SpO2:%s,Temp:%s,%s] Fac[O2:%s,%s]",
             averageHeartRateStr, minimumSpo2Str, maxBodyTempStr, medStat,
             averageOxygenStr, facStat);
    return String(payload);
}

/**
 * @brief Sizes the MQTT packet buffer for the combined hospital summary.
 *
 * The combined summary can exceed PubSubClient's default packet buffer. The
 * code therefore measures the topic and worst-case payload sizes and adds a
 * safety margin before configuring the client buffer.
 *
 * @return true if buffer allocation succeeded or already large enough; false otherwise.
 */
bool configureMqttBuffer()
{
    String sizingPayload = buildHospitalSummaryPayload(
        "999.99 BPM", "100 %", "999.99 °C", UINT32_MAX, UINT32_MAX, UINT32_MAX,
        "999.99 °C", "999.99 %", "999.99", UINT32_MAX, UINT32_MAX, UINT32_MAX);
    String hospitalEventsTopic = getAioFeedTopic(HOSPITAL_EVENTS_FEED);
    const uint16_t requiredPacketSize = MQTT_MAX_HEADER_SIZE + 2 + hospitalEventsTopic.length() + sizingPayload.length();
    const uint16_t configuredBufferSize = requiredPacketSize + MQTT_SUMMARY_BUFFER_MARGIN;
    const uint16_t bufferSizeBefore = mqttClient.getBufferSize();

    bool success = true;
    if (bufferSizeBefore < configuredBufferSize) {
        success = mqttClient.setBufferSize(configuredBufferSize);
        if (!success) {
            Serial.println("[MQTT] Warning: Failed to allocate enlarged MQTT packet buffer; compact fallback active");
        }
    }

    Serial.print("[MQTT] MQTT buffer size before fix: ");
    Serial.println(bufferSizeBefore);
    Serial.print("[MQTT] MQTT buffer size: ");
    Serial.println(mqttClient.getBufferSize());
    return success;
}

// MQTT TASK SUPPORT & MESSAGE ROUTING
// ============================================================================

/**
 * @brief Function pointer type for inbound MQTT subscription payload handlers.
 */
using MqttMessageHandler = void (*)(const char *, size_t);

/**
 * @brief Association between an Adafruit IO subscription feed topic and its parser handler.
 */
struct MqttMessageRoute {
    const char *topic;
    MqttMessageHandler handler;
};

String medicationDosageTopic;
String samplingRateTopic;
String bedAngleTopic;

/**
 * @brief Parses and validates incoming dosage setpoints from "medication-dosage" feed.
 *
 * Enforces safety clamping [0.0 - 100.0 mg/hr] and commits requested value into
 * medicationDosageControl under spinlock critical section.
 *
 * @param payload Null-terminated string payload from broker.
 * @param length Byte length of payload.
 */
void handleMedicationDosagePayload(const char *payload, size_t length)
{
    if (length == 0) {
        return;
    }

    float parsedDosage = 0.0f;
    if (!parseFloatStrict(payload, 0.0f, 500.0f, parsedDosage)) {
        Serial.println("[MQTT TASK] Invalid medication-dosage string payload ignored");
        return;
    }
    const float clampedDosage = clampMedicationDosage(parsedDosage);

    portENTER_CRITICAL(&telemetryMux);
    medicationDosageControl.requested = clampedDosage;
    medicationDosageControl.received = true;
    medicationDosageControl.lastReceivedMs = millis();
    portEXIT_CRITICAL(&telemetryMux);
    Serial.print("[MQTT TASK] medication-dosage received: ");
    Serial.print(clampedDosage, 1);
    Serial.println(" mg/hr");
}

/**
 * @brief Parses and clamps doctor dynamic sampling rate commands from "sampling-rate" feed.
 *
 * Validates range between 5 and 60 seconds and overwrites the single-element samplingQueue.
 *
 * @param payload Null-terminated string payload from broker.
 * @param length Byte length of payload.
 */
void handleSamplingRatePayload(const char *payload, size_t length)
{
    if (length == 0) {
        return;
    }
    
    long parsedRate = 0;
    if (!parseLongStrict(payload, 5, 60, parsedRate)) {
        Serial.println("[MQTT TASK] Invalid sampling-rate string payload ignored");
        return;
    }
    
    uint32_t rate = static_cast<uint32_t>(parsedRate);
    xQueueOverwrite(samplingQueue, &rate);
    Serial.print("[MQTT TASK] sampling-rate received: ");
    Serial.print(rate);
    Serial.println(" s");
}

/**
 * @brief Parses and validates clinician bed elevation commands from "bed-angle" feed.
 *
 * Overwrites the single-element bedQueue for consumption by bedTask.
 *
 * @param payload Null-terminated string payload from broker.
 * @param length Byte length of payload.
 */
void handleBedAnglePayload(const char *payload, size_t length)
{
    if (length == 0) return;
    float angle = 0.0f;
    if (!parseFloatStrict(payload, 0.0f, 90.0f, angle)) {
        Serial.println("[MQTT TASK] Invalid bed-angle string payload ignored");
        return;
    }
    xQueueOverwrite(bedQueue, &angle);
    Serial.print("[MQTT TASK] bed-angle received: ");
    Serial.println(angle);
}

MqttMessageRoute mqttRoutes[] = {
    {nullptr, handleMedicationDosagePayload},
    {nullptr, handleSamplingRatePayload},
    {nullptr, handleBedAnglePayload}
};

/**
 * @brief PubSubClient packet reception callback.
 *
 * Safely copies payload into local null-terminated buffer and matches topic
 * against the registered dispatch table in mqttRoutes[].
 *
 * @param topic MQTT topic string.
 * @param payload Raw payload byte buffer.
 * @param length Byte length of received packet payload.
 */
void mqttMessageCallback(char *topic, byte *payload, unsigned int length)
{
    char payloadBuffer[32];
    if (topic == nullptr || payload == nullptr || length == 0 || length >= sizeof(payloadBuffer)) {
        return;
    }
    memcpy(payloadBuffer, payload, length);
    payloadBuffer[length] = '\0';

    for (MqttMessageRoute &route : mqttRoutes) {
        if (route.topic != nullptr && strcmp(topic, route.topic) == 0) {
            route.handler(payloadBuffer, length);
            return;
        }
    }
}

/**
 * @brief Connects MQTT client to Adafruit IO, configures subscriptions, and registers callbacks.
 *
 * Connection Preconditions:
 * - Requires active Wi-Fi link (WiFi.status() == WL_CONNECTED).
 * - Generates unique client ID based on ESP32 factory eFuse MAC address.
 * - Subscribes to: medication-dosage, sampling-rate, and bed-angle.
 *
 * @return true if successfully connected and subscribed; false otherwise.
 */
bool connectMqtt()
{
    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }

    IPAddress brokerIp;
    reportTaskHeartbeat(TASK_MQTT);
    if (WiFi.hostByName(mqttBroker, brokerIp) != 1) {
        reportTaskHeartbeat(TASK_MQTT);
        Serial.println("[MQTT TASK] DNS lookup failed for MQTT broker");
        return false;
    }
    reportTaskHeartbeat(TASK_MQTT);

    mqttClient.setServer(brokerIp, mqttPort);
    String clientId = "smart-hospital-" + String(static_cast<uint32_t>(ESP.getEfuseMac()), HEX);
    const bool connected = mqttClient.connect(clientId.c_str(), AIO_USERNAME, AIO_KEY);
    reportTaskHeartbeat(TASK_MQTT);
    if (connected) {
        medicationDosageTopic = getAioFeedTopic(MEDICATION_DOSAGE_FEED);
        samplingRateTopic = getAioFeedTopic(SAMPLING_RATE_FEED);
        bedAngleTopic = getAioFeedTopic(BED_ANGLE_FEED);
        
        mqttRoutes[0].topic = medicationDosageTopic.c_str();
        mqttRoutes[1].topic = samplingRateTopic.c_str();
        mqttRoutes[2].topic = bedAngleTopic.c_str();
        
        mqttClient.setCallback(mqttMessageCallback);
        bool sub1 = mqttClient.subscribe(medicationDosageTopic.c_str());
        bool sub2 = mqttClient.subscribe(samplingRateTopic.c_str());
        bool sub3 = mqttClient.subscribe(bedAngleTopic.c_str());
        reportTaskHeartbeat(TASK_MQTT);
        mqttClient.loop();
        reportTaskHeartbeat(TASK_MQTT);
        if (!sub1 || !sub2 || !sub3) {
            Serial.println("[MQTT TASK] Warning: One or more topic subscriptions failed; disconnecting socket to retry cleanly");
            mqttClient.disconnect();
            return false;
        }
        Serial.println("[MQTT TASK] Connected and subscribed to Adafruit IO broker");
        return true;
    }
    Serial.print("[MQTT TASK] Connection failed, state=");
    Serial.println(mqttClient.state());
    return false;
}

/**
 * @brief Attempts live MQTT publication or diverts to offline FIFO queue.
 *
 * Operational Logic:
 * - If offline is true: Enqueues directly to offline buffer via enqueueBufferedTelemetry().
 * - If online: Attempts mqttClient.publish(). If the TCP transport fails, diverts the message
 *   to the bounded offline FIFO buffer (capacity 64) for best-effort delivery under QoS 0.
 *
 * @param topic Destination Adafruit IO feed topic string.
 * @param payload Telemetry or event message payload.
 * @param offline True if Wi-Fi or MQTT broker connection is known to be down.
 * @return true if published live or successfully buffered; false if offline queue was full.
 */
bool safePublish(const String& topic, const String& payload, bool offline) {
    if (offline) {
        return enqueueBufferedTelemetry(topic.c_str(), payload.c_str());
    } else {
        if (mqttClient.publish(topic.c_str(), payload.c_str())) {
            return true;
        } else {
            Serial.println("[MQTT TASK] Publish failed; buffering telemetry");
            return enqueueBufferedTelemetry(topic.c_str(), payload.c_str());
        }
    }
}

/**
 * @brief Publishes one changed medical or facility live feed value.
 *
 * Feed names are mapped to the six existing medical and facility Adafruit IO
 * topics. Unchanged values are suppressed, one slot is attempted per MQTT
 * task cycle, and resetLiveFeedPublishState() forces the first value after a
 * connection or reconnection to be sent.
 */
bool publishLiveFeed(const Telemetry &snapshot, const FacilityTelemetry &facilitySnapshot, LiveFeedIndex feedIndex, bool offline)
{
    String feedName;
    String payload;
    bool changed = false;

    switch (feedIndex) {
        case LIVE_MEDICAL_HEART_RATE:
            feedName = MEDICAL_HEART_RATE_FEED;
            payload = String(snapshot.heartRate);
            changed = !liveFeedPublished[feedIndex] || lastPublishedHeartRate != snapshot.heartRate;
            break;
        case LIVE_MEDICAL_SPO2:
            feedName = MEDICAL_SPO2_FEED;
            payload = String(snapshot.spo2);
            changed = !liveFeedPublished[feedIndex] || lastPublishedSpo2 != snapshot.spo2;
            break;
        case LIVE_MEDICAL_BODY_TEMPERATURE:
            if (!snapshot.temperatureValid) {
                return false;
            }
            feedName = MEDICAL_BODY_TEMPERATURE_FEED;
            payload = String(snapshot.temperature, 1);
            changed = !liveFeedPublished[feedIndex] || lastPublishedBodyTemperature != snapshot.temperature;
            break;
        case LIVE_FACILITY_ROOM_TEMPERATURE:
            feedName = FACILITY_ROOM_TEMPERATURE_FEED;
            payload = String(facilitySnapshot.roomTemperature, 1);
            changed = !liveFeedPublished[feedIndex] || lastPublishedRoomTemperature != facilitySnapshot.roomTemperature;
            break;
        case LIVE_FACILITY_OXYGEN:
            feedName = FACILITY_OXYGEN_FEED;
            payload = String(facilitySnapshot.oxygenLevel, 1);
            changed = !liveFeedPublished[feedIndex] || lastPublishedOxygen != facilitySnapshot.oxygenLevel;
            break;
        case LIVE_FACILITY_AQI:
            feedName = FACILITY_AQI_FEED;
            payload = String(facilitySnapshot.aqi);
            changed = !liveFeedPublished[feedIndex] || lastPublishedAqi != facilitySnapshot.aqi;
            break;
        default:
            return false;
    }

    if (!changed) {
        return false;
    }

    if (safePublish(getAioFeedTopic(feedName.c_str()), payload, offline)) {
        liveFeedPublished[feedIndex] = true;
        switch (feedIndex) {
            case LIVE_MEDICAL_HEART_RATE:
                lastPublishedHeartRate = snapshot.heartRate;
                break;
            case LIVE_MEDICAL_SPO2:
                lastPublishedSpo2 = snapshot.spo2;
                break;
            case LIVE_MEDICAL_BODY_TEMPERATURE:
                lastPublishedBodyTemperature = snapshot.temperature;
                break;
            case LIVE_FACILITY_ROOM_TEMPERATURE:
                lastPublishedRoomTemperature = facilitySnapshot.roomTemperature;
                break;
            case LIVE_FACILITY_OXYGEN:
                lastPublishedOxygen = facilitySnapshot.oxygenLevel;
                break;
            case LIVE_FACILITY_AQI:
                lastPublishedAqi = facilitySnapshot.aqi;
                break;
            default:
                break;
        }
        return true;
    }
    return false;
}

/**
 * @brief Marks all six live feeds as pending after an MQTT reconnection.
 */
void resetLiveFeedPublishState()
{
    for (int feedIndex = 0; feedIndex < LIVE_FEED_COUNT; ++feedIndex) {
        liveFeedPublished[feedIndex] = false;
    }
}

static bool medicalSensorFaultActive = false;
static bool medicalAlertPublished = false;
static bool facilityAlertPublished = false;
static bool medicalNormalPending = false;
static bool facilityNormalPending = false;
static String lastMedicalAlertParameter = "";
static String lastFacilityAlertParameter = "";
static String lastMedicalAlertMessage = "";
static String lastFacilityAlertMessage = "";
static AlertSeverity lastMedicalAlertSeverity = ALERT_SEVERITY_NORMAL;
static AlertSeverity lastFacilityAlertSeverity = ALERT_SEVERITY_NORMAL;
static uint32_t lastMedicalAlertPublishMs = 0;
static uint32_t lastFacilityAlertPublishMs = 0;
static bool medicalAlertStateKnown = false;
static bool facilityAlertStateKnown = false;

/**
 * @brief Publishes medical alert or normal-status events when state changes.
 *
 * Both abnormal and normal-status messages use the shared hospital-events
 * feed. Duplicate states are suppressed. Abnormal alert re-publishes are
 * rate-limited by ALERT_PUBLISH_INTERVAL_MS; NORMAL is only published on
 * first ready state or on alert clearance transitions.
 */
void publishMedicalAlertIfNeeded(const Telemetry &snapshot, bool offline)
{
    const uint32_t now = millis();

    // 1. Independent Sensor Availability State Machine
    // Evaluates confirmed hardware faults independently of the overall channel initialization state (P2 fix).
    // State transitions are committed only upon successful MQTT delivery or FIFO buffering (Point C).
    const bool isTempFault = isMedicalTemperatureFault(snapshot);
    const bool isFullyValid = isMedicalTelemetryFullyValid(snapshot);
    const bool isInitializing = isMedicalTelemetryInitializing(snapshot);

    SensorFaultTransition trans = evaluateSensorFaultTransition(
        isTempFault,
        isFullyValid,
        medicalSensorFaultActive
    );

    if (trans == SensorFaultTransition::TRIGGER_FAULT) {
        const char *faultPayload = "MEDICAL STATUS: SENSOR FAULT\nBody temperature sensor unavailable; clinical status unknown.";
        if (safePublish(getAioFeedTopic(HOSPITAL_EVENTS_FEED), String(faultPayload), offline)) {
            applySensorFaultTransition(trans, medicalSensorFaultActive, medicalNormalPending);
        }
    } else if (trans == SensorFaultTransition::TRIGGER_RECOVERY) {
        const char *restoredPayload = "MEDICAL STATUS: SENSOR RESTORED\nBody temperature sensor operational; clinical telemetry restored.";
        if (safePublish(getAioFeedTopic(HOSPITAL_EVENTS_FEED), String(restoredPayload), offline)) {
            applySensorFaultTransition(trans, medicalSensorFaultActive, medicalNormalPending);
        }
    }

    // 2. Clinical Vitals Evaluation
    AlertCandidate candidate = evaluateMedicalAlert(snapshot);

    if (!candidate.active) {
        // Clear any active-alert latch when vitals normalize even during a sensor fault or startup init
        // so a recurring vital crisis is not suppressed and NORMAL publishes once fully valid.
        if (medicalAlertPublished) {
            clearActiveAlertLatch(medicalAlertPublished,
                                  medicalNormalPending,
                                  lastMedicalAlertParameter,
                                  lastMedicalAlertMessage,
                                  lastMedicalAlertSeverity);
        }

        // Suppress false NORMAL if sensor fault is active, system is initializing, or telemetry is not fully valid
        if (medicalSensorFaultActive || isInitializing || !isFullyValid) {
            return;
        }

        if (!medicalAlertStateKnown || medicalNormalPending) {
            const char *normalPayload = "MEDICAL STATUS: NORMAL\nNo active medical alerts.";
            if (safePublish(getAioFeedTopic(HOSPITAL_EVENTS_FEED), String(normalPayload), offline)) {
                lastMedicalAlertPublishMs = now;
                medicalAlertStateKnown = true;
                medicalNormalPending = false;
            }
        }
        return;
    }

    // Cancel pending normal if an acute abnormality emerges
    medicalNormalPending = false;

    if (shouldSuppressAlertPublish(candidate,
                                   medicalAlertPublished,
                                   medicalAlertStateKnown,
                                   lastMedicalAlertParameter,
                                   lastMedicalAlertMessage,
                                   lastMedicalAlertSeverity,
                                   lastMedicalAlertPublishMs,
                                   now,
                                   ALERT_PUBLISH_INTERVAL_MS,
                                   ALERT_ESCALATION_COOLDOWN_MS)) {
        return;
    }

    String alertPayload = buildAlertPayload("MEDICAL", candidate.parameter, candidate.value, candidate.severity, candidate.message);
    if (safePublish(getAioFeedTopic(HOSPITAL_EVENTS_FEED), alertPayload, offline)) {
        medicalAlertPublished = true;
        lastMedicalAlertParameter = candidate.parameter;
        lastMedicalAlertMessage = (candidate.message != nullptr) ? candidate.message : "";
        lastMedicalAlertSeverity = candidate.severity;
        lastMedicalAlertPublishMs = now;
        medicalAlertStateKnown = true;
    }
}

/**
 * @brief Publishes facility alert or normal-status events when state changes.
 *
 * Both abnormal and normal-status messages use the shared hospital-events
 * feed. Duplicate states are suppressed. Abnormal alert re-publishes are
 * rate-limited by ALERT_PUBLISH_INTERVAL_MS; NORMAL is only published on
 * first ready state or on alert clearance transitions.
 */
void publishFacilityAlertIfNeeded(const FacilityTelemetry &snapshot, bool offline)
{
    if (!snapshot.initialFacilityProcessed) {
        return;
    }
    AlertCandidate candidate = evaluateFacilityAlert(snapshot);
    const uint32_t now = millis();
    if (!candidate.active) {
        if (facilityAlertPublished) {
            clearActiveAlertLatch(facilityAlertPublished,
                                  facilityNormalPending,
                                  lastFacilityAlertParameter,
                                  lastFacilityAlertMessage,
                                  lastFacilityAlertSeverity);
        }

        if (!facilityAlertStateKnown || facilityNormalPending) {
            const char *normalPayload = "FACILITY STATUS: NORMAL\nNo active facility alerts.";
            if (safePublish(getAioFeedTopic(HOSPITAL_EVENTS_FEED), String(normalPayload), offline)) {
                lastFacilityAlertPublishMs = now;
                facilityAlertStateKnown = true;
                facilityNormalPending = false;
            }
        }
        return;
    }

    // Cancel pending normal if a facility anomaly emerges
    facilityNormalPending = false;

    if (shouldSuppressAlertPublish(candidate,
                                   facilityAlertPublished,
                                   facilityAlertStateKnown,
                                   lastFacilityAlertParameter,
                                   lastFacilityAlertMessage,
                                   lastFacilityAlertSeverity,
                                   lastFacilityAlertPublishMs,
                                   now,
                                   ALERT_PUBLISH_INTERVAL_MS,
                                   ALERT_ESCALATION_COOLDOWN_MS)) {
        return;
    }

    String alertPayload = buildAlertPayload("FACILITY", candidate.parameter, candidate.value, candidate.severity, candidate.message);
    if (safePublish(getAioFeedTopic(HOSPITAL_EVENTS_FEED), alertPayload, offline)) {
        facilityAlertPublished = true;
        lastFacilityAlertParameter = candidate.parameter;
        lastFacilityAlertMessage = (candidate.message != nullptr) ? candidate.message : "";
        lastFacilityAlertSeverity = candidate.severity;
        lastFacilityAlertPublishMs = now;
        facilityAlertStateKnown = true;
    }
}

/**
 * @brief Aggregates and publishes the periodic hospital summary.
 *
 * Medical and facility telemetry snapshots are accumulated independently in
 * mqttTask, combined into one payload, and published through hospital-events
 * every 60000 ms. The accumulators are reset after the publish attempt.
 */
void publishSummaryData(bool offline)
{
    Telemetry snapshot = getTelemetrySnapshot();
    FacilityTelemetry facilitySnapshot = getFacilitySnapshot();

    if (medicalSummaryStats.sampleCount == 0 && snapshot.initialVitalsProcessed) {
        medicalSummaryStats.minSpo2 = snapshot.spo2;
    }

    if (facilitySummaryStats.sampleCount == 0 && facilitySnapshot.initialFacilityProcessed) {
        updateFacilitySummaryStats(facilitySnapshot);
    }

    char hrBuf[32];
    char minSpo2Buf[32];
    if (medicalSummaryStats.sampleCount > 0) {
        snprintf(hrBuf, sizeof(hrBuf), "%.2f BPM", medicalSummaryStats.heartRateTotal / medicalSummaryStats.sampleCount);
        snprintf(minSpo2Buf, sizeof(minSpo2Buf), "%d %%", medicalSummaryStats.minSpo2);
    } else if (snapshot.initialVitalsProcessed) {
        snprintf(hrBuf, sizeof(hrBuf), "%.2f BPM", static_cast<float>(snapshot.heartRate));
        snprintf(minSpo2Buf, sizeof(minSpo2Buf), "%d %%", snapshot.spo2);
    } else {
        snprintf(hrBuf, sizeof(hrBuf), "N/A");
        snprintf(minSpo2Buf, sizeof(minSpo2Buf), "N/A");
    }

    char maxTempBuf[32];
    if (medicalSummaryStats.temperatureSampleCount > 0) {
        snprintf(maxTempBuf, sizeof(maxTempBuf), "%.2f °C", medicalSummaryStats.maxBodyTemperature);
    } else if (snapshot.temperatureValid) {
        snprintf(maxTempBuf, sizeof(maxTempBuf), "%.2f °C", snapshot.temperature);
    } else {
        snprintf(maxTempBuf, sizeof(maxTempBuf), "N/A");
    }

    char roomTempBuf[32];
    char oxygenBuf[32];
    char aqiBuf[32];
    if (facilitySummaryStats.sampleCount > 0) {
        snprintf(roomTempBuf, sizeof(roomTempBuf), "%.2f °C", facilitySummaryStats.roomTemperatureTotal / facilitySummaryStats.sampleCount);
        snprintf(oxygenBuf, sizeof(oxygenBuf), "%.2f %%", facilitySummaryStats.oxygenTotal / facilitySummaryStats.sampleCount);
        snprintf(aqiBuf, sizeof(aqiBuf), "%.2f", facilitySummaryStats.aqiTotal / facilitySummaryStats.sampleCount);
    } else if (facilitySnapshot.initialFacilityProcessed) {
        snprintf(roomTempBuf, sizeof(roomTempBuf), "%.2f °C", facilitySnapshot.roomTemperature);
        snprintf(oxygenBuf, sizeof(oxygenBuf), "%.2f %%", facilitySnapshot.oxygenLevel);
        snprintf(aqiBuf, sizeof(aqiBuf), "%d", facilitySnapshot.aqi);
    } else {
        snprintf(roomTempBuf, sizeof(roomTempBuf), "N/A");
        snprintf(oxygenBuf, sizeof(oxygenBuf), "N/A");
        snprintf(aqiBuf, sizeof(aqiBuf), "N/A");
    }

    const char *fallbackFullMedicalStatus = nullptr;
    const char *fallbackCompactMedicalStatus = nullptr;
    if (medicalSummaryStats.sampleCount == 0 &&
        medicalSummaryStats.abnormalCount == 0 &&
        medicalSummaryStats.criticalCount == 0) {
        if (snapshot.initialVitalsProcessed) {
            AlertCandidate fallbackAlert = evaluateMedicalAlert(snapshot);
            if (!fallbackAlert.active) {
                fallbackFullMedicalStatus = "NORMAL";
                fallbackCompactMedicalStatus = "NORM";
            } else if (fallbackAlert.severity == ALERT_SEVERITY_CRITICAL) {
                fallbackFullMedicalStatus = "CRITICAL";
                fallbackCompactMedicalStatus = "CRIT";
            } else {
                fallbackFullMedicalStatus = "WARNING";
                fallbackCompactMedicalStatus = "WARN";
            }
        } else {
            fallbackCompactMedicalStatus = "N/A";
        }
    }

    const char *fallbackCompactFacilityStatus =
        (facilitySummaryStats.sampleCount == 0 && !facilitySnapshot.initialFacilityProcessed)
            ? "N/A"
            : nullptr;

    auto buildCompactPayload = [&]() {
        return buildCompactHospitalSummaryPayload(
            hrBuf,
            minSpo2Buf,
            maxTempBuf,
            medicalSummaryStats.abnormalCount,
            medicalSummaryStats.criticalCount,
            oxygenBuf,
            facilitySummaryStats.abnormalCount,
            facilitySummaryStats.criticalCount,
            fallbackCompactMedicalStatus,
            fallbackCompactFacilityStatus);
    };

    String hospitalEventsTopic = getAioFeedTopic(HOSPITAL_EVENTS_FEED);
    String hospitalSummaryPayload;
    if (mqttClient.getBufferSize() >= 400) {
        hospitalSummaryPayload = buildHospitalSummaryPayload(
            hrBuf,
            minSpo2Buf,
            maxTempBuf,
            medicalSummaryStats.sampleCount,
            medicalSummaryStats.abnormalCount,
            medicalSummaryStats.criticalCount,
            roomTempBuf,
            oxygenBuf,
            aqiBuf,
            facilitySummaryStats.sampleCount,
            facilitySummaryStats.abnormalCount,
            facilitySummaryStats.criticalCount,
            fallbackFullMedicalStatus);
        const size_t requiredPacketSize = MQTT_MAX_HEADER_SIZE + 2 + hospitalEventsTopic.length() + hospitalSummaryPayload.length();
        if (offline && hospitalSummaryPayload.length() >= sizeof(BufferedTelemetryMessage::payload)) {
            Serial.println("[MQTT] Offline summary exceeds FIFO payload slot; switching to compact payload");
            hospitalSummaryPayload = buildCompactPayload();
        } else if (!offline && requiredPacketSize > mqttClient.getBufferSize()) {
            Serial.println("[MQTT] Online summary exceeds MQTT packet buffer; switching to compact payload");
            hospitalSummaryPayload = buildCompactPayload();
        }
    } else {
        hospitalSummaryPayload = buildCompactPayload();
    }

    Serial.print("[MQTT] Hospital summary payload length: ");
    Serial.print(hospitalSummaryPayload.length());
    Serial.println(" bytes");
    Serial.println("[MQTT] HOSPITAL SUMMARY PAYLOAD:");
    Serial.println(hospitalSummaryPayload);
    const bool hospitalSummaryPublishResult = safePublish(hospitalEventsTopic, hospitalSummaryPayload, offline);
    Serial.print("[MQTT] hospital summary publish result: ");
    Serial.println(hospitalSummaryPublishResult ? "SUCCESS" : "FAILURE");
    if (!hospitalSummaryPublishResult) {
        Serial.println("[MQTT TASK] Failed to deliver/buffer hospital summary; retaining statistics for next window");
    } else {
        resetSummaryStats();
    }
}

// ============================================================================
// MQTT TASK
// ============================================================================

/**
 * @brief Maintains Wi-Fi/MQTT connectivity and publishes telemetry.
 *
 * Uses the configured Adafruit IO MQTT credentials, maintains Wi-Fi and MQTT,
 * publishes changed live feeds and shared events, and combines medical plus
 * facility summaries on hospital-events. Medical and facility feeds provide
 * logical dashboard separation, not staff authentication or authorization.
 * The task services MQTT every 50 ms and schedules telemetry publication every
 * 3 seconds. Live feeds and replayed messages share the 3-second slot, while
 * summaries and alerts have independent pacing (up to 4 consecutive publishes in one iteration).
 */
void mqttTask(void *parameter)
{
    (void)parameter;
    if (!registerTaskWithWatchdog(TASK_MQTT, "MQTT")) {
        return;
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSsid, "");
    wifiClient.setConnectionTimeout(1500);
    wifiClient.setWriteTimeoutMs(1500);
    mqttClient.setServer(mqttBroker, mqttPort);
    mqttClient.setSocketTimeout(2);
    configureMqttBuffer();
    uint32_t nextWifiRetryMs = millis() + MQTT_INITIAL_RETRY_DELAY_MS;
    uint32_t wifiRetryDelayMs = MQTT_INITIAL_RETRY_DELAY_MS;
    uint32_t lastSummaryPublishMs = 0;
    uint32_t lastPublishMs = 0;
    uint32_t nextMqttRetryMs = 0;
    uint32_t mqttRetryDelayMs = MQTT_INITIAL_RETRY_DELAY_MS;
    int nextLiveFeed = LIVE_MEDICAL_HEART_RATE;

    while (true) {
        const uint32_t now = millis();
        if (simForceWifiOffline) {
            const wl_status_t wifiStatus = WiFi.status();
            if (wifiStatus != WL_DISCONNECTED && wifiStatus != WL_NO_SHIELD) {
                WiFi.disconnect();
                Serial.println("[MQTT TASK] WiFi disconnected by SIM_OFFLINE WIFI");
            }
            if (mqttClient.connected()) {
                mqttClient.disconnect();
            }
            nextWifiRetryMs = now;
            wifiRetryDelayMs = MQTT_INITIAL_RETRY_DELAY_MS;
            nextMqttRetryMs = now;
            mqttRetryDelayMs = MQTT_INITIAL_RETRY_DELAY_MS;
            mqttConnectedFlag = false;
        } else if (simForceMqttOffline) {
            if (mqttClient.connected()) {
                mqttClient.disconnect();
                Serial.println("[MQTT TASK] Disconnected by SIM_OFFLINE MQTT; buffering activated");
            }
            nextWifiRetryMs = now;
            nextMqttRetryMs = now;
            mqttRetryDelayMs = MQTT_INITIAL_RETRY_DELAY_MS;
            mqttConnectedFlag = false;
        } else if (WiFi.status() != WL_CONNECTED) {
            if ((int32_t)(now - nextWifiRetryMs) >= 0) {
                Serial.println("[MQTT TASK] WiFi unavailable; retrying...");
                WiFi.disconnect();
                WiFi.begin(wifiSsid, "");
                nextWifiRetryMs = now + wifiRetryDelayMs;
                wifiRetryDelayMs = min(wifiRetryDelayMs * 2U, MQTT_MAX_RETRY_DELAY_MS);
            }
            // Reset MQTT backoff since it depends on Wi-Fi
            mqttRetryDelayMs = MQTT_INITIAL_RETRY_DELAY_MS;
            nextMqttRetryMs = now;
        } else {
            wifiRetryDelayMs = MQTT_INITIAL_RETRY_DELAY_MS; // reset on success
            nextWifiRetryMs = now;
            
            if (!mqttClient.connected()) {
                if ((int32_t)(now - nextMqttRetryMs) >= 0) {
                    reportTaskHeartbeat(TASK_MQTT);
                    if (connectMqtt()) {
                        resetLiveFeedPublishState();
                        lastPublishMs = now - MQTT_TASK_INTERVAL_MS;
                        mqttRetryDelayMs = MQTT_INITIAL_RETRY_DELAY_MS;
                        nextMqttRetryMs = now;
                    } else {
                        nextMqttRetryMs = now + mqttRetryDelayMs;
                        mqttRetryDelayMs = min(mqttRetryDelayMs * 2U, MQTT_MAX_RETRY_DELAY_MS);
                    }
                }
            } else {
                nextMqttRetryMs = now;
                mqttClient.loop();
            }
        }

        mqttConnectedFlag = mqttClient.connected() && !simForceMqttOffline && !simForceWifiOffline;

        if (now - lastPublishMs >= MQTT_TASK_INTERVAL_MS) {
            lastPublishMs = now;
            bool offline = (WiFi.status() != WL_CONNECTED || !mqttConnectedFlag);
            
            Telemetry snapshot = getTelemetrySnapshot();
            FacilityTelemetry facilitySnapshot = getFacilitySnapshot();

            updateMedicalSummaryStats(snapshot);
            updateFacilitySummaryStats(facilitySnapshot);

            publishMedicalAlertIfNeeded(snapshot, offline);
            reportTaskHeartbeat(TASK_MQTT);
            publishFacilityAlertIfNeeded(facilitySnapshot, offline);
            reportTaskHeartbeat(TASK_MQTT);

            if (now - lastSummaryPublishMs >= SUMMARY_INTERVAL_MS) {
                Serial.println("[MQTT] Summary interval reached");
                lastSummaryPublishMs = now;
                publishSummaryData(offline);
                reportTaskHeartbeat(TASK_MQTT);
            }

            static bool syncTurn = false;
            static bool wasReplaying = false;
            size_t bufferedCount = getBufferedTelemetryCount();
            if (bufferedCount > 0) {
                wasReplaying = true;
            } else if (wasReplaying) {
                // Replay queue drained completely; resynchronize live feed states
                resetLiveFeedPublishState();
                wasReplaying = false;
                Serial.println("[MQTT TASK] Offline replay drained; live feeds resynchronized");
            }
            
            if (!offline && bufferedCount > 0 && syncTurn) {
                BufferedTelemetryMessage msg;
                if (peekBufferedTelemetry(&msg)) {
                    if (mqttClient.publish(msg.topic, msg.payload)) {
                        dequeueBufferedTelemetry(&msg);
                        Serial.println("[MQTT TASK] Synced buffered item");
                    }
                }
                syncTurn = false;
            } else {
                bool livePublished = publishLiveFeed(snapshot, facilitySnapshot, static_cast<LiveFeedIndex>(nextLiveFeed), offline);
                nextLiveFeed = (nextLiveFeed + 1) % LIVE_FEED_COUNT;
                
                if (!livePublished && !offline && getBufferedTelemetryCount() > 0) {
                    BufferedTelemetryMessage msg;
                    if (peekBufferedTelemetry(&msg)) {
                        if (mqttClient.publish(msg.topic, msg.payload)) {
                            dequeueBufferedTelemetry(&msg);
                            Serial.println("[MQTT TASK] Synced buffered item (idle slot)");
                        }
                    }
                } else if (getBufferedTelemetryCount() > 0) {
                    syncTurn = true;
                }
            }
        }
        reportTaskHeartbeat(TASK_MQTT);
        vTaskDelay(pdMS_TO_TICKS(MQTT_LOOP_INTERVAL_MS));
    }
}
