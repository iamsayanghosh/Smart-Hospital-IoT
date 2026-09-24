#include "mqtt.h"
#include "app.h"
#include "state.h"
#include "medication.h"
#include "connectivity.h"
#include "watchdog.h"
#include "esp_task_wdt.h"
#include "adafruit_io_config.h"
#include <PubSubClient.h>

const uint32_t MQTT_TASK_INTERVAL_MS = 3000;
const uint32_t MQTT_LOOP_INTERVAL_MS = 50;
const uint32_t MQTT_INITIAL_RETRY_DELAY_MS = 1000;
const uint32_t MQTT_MAX_RETRY_DELAY_MS = 30000;
const uint32_t SUMMARY_INTERVAL_MS = 60000;
const uint32_t ALERT_PUBLISH_INTERVAL_MS = 60000;
const uint16_t MQTT_SUMMARY_BUFFER_MARGIN = 64;

#define MEDICAL_HEART_RATE_FEED "medical-heart-rate"
#define MEDICAL_SPO2_FEED "medical-spo2"
#define MEDICAL_BODY_TEMPERATURE_FEED "medical-body-temperature"
#define FACILITY_ROOM_TEMPERATURE_FEED "facility-room-temperature"
#define FACILITY_OXYGEN_FEED "facility-oxygen"
#define FACILITY_AQI_FEED "facility-aqi"
#define HOSPITAL_EVENTS_FEED "hospital-events"
#define MEDICATION_DOSAGE_FEED "medication-dosage"

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
 * @brief Formats accumulated medical readings for summary reporting.
 */
String buildMedicalSummaryPayload(float averageHeartRate, int minimumSpo2, float maximumBodyTemperature, uint32_t sampleCount, uint32_t abnormalCount)
{
    char payload[280];
    snprintf(payload, sizeof(payload),
             "MEDICAL SUMMARY\n\nAverage HR: %.2f BPM\nMinimum SpO2: %d %%\nMaximum Body Temperature: %.2f °C\n\nSamples: %lu\nAbnormal Readings: %lu\n\nStatus: %s",
             averageHeartRate, minimumSpo2, maximumBodyTemperature,
             static_cast<unsigned long>(sampleCount), static_cast<unsigned long>(abnormalCount),
             abnormalCount > 0 ? "WARNING" : "NORMAL");
    return String(payload);
}

/**
 * @brief Formats accumulated facility readings for summary reporting.
 */
String buildFacilitySummaryPayload(float averageRoomTemperature, float averageOxygen, float averageAqi, uint32_t sampleCount, uint32_t abnormalCount)
{
    char payload[280];
    snprintf(payload, sizeof(payload),
             "FACILITY SUMMARY\n\nAverage Room Temperature: %.2f °C\nAverage Oxygen: %.2f %%\nAverage AQI: %.2f\n\nSamples: %lu\nAbnormal Readings: %lu\n\nStatus: %s",
             averageRoomTemperature, averageOxygen, averageAqi,
             static_cast<unsigned long>(sampleCount), static_cast<unsigned long>(abnormalCount),
             abnormalCount > 0 ? "WARNING" : "NORMAL");
    return String(payload);
}

/**
 * @brief Combines aggregated medical and facility snapshots into one payload.
 *
 * The resulting message is published periodically through hospital-events.
 */
String buildHospitalSummaryPayload(float averageHeartRate, int minimumSpo2, float maximumBodyTemperature,
                                  uint32_t medicalSampleCount, uint32_t medicalAbnormalCount,
                                  float averageRoomTemperature, float averageOxygen, float averageAqi,
                                  uint32_t facilitySampleCount, uint32_t facilityAbnormalCount)
{
    char payload[620];
    snprintf(payload, sizeof(payload),
             "HOSPITAL SUMMARY\n==============================\n\nMEDICAL SUMMARY\n\nAverage HR: %.2f BPM\nMinimum SpO2: %d %%\nMaximum Body Temperature: %.2f °C\n\nSamples: %lu\nAbnormal Readings: %lu\n\nStatus: %s\n\n\nFACILITY SUMMARY\n\nAverage Room Temperature: %.2f °C\nAverage Oxygen: %.2f %%\nAverage AQI: %.2f\n\nSamples: %lu\nAbnormal Readings: %lu\n\nStatus: %s",
             averageHeartRate, minimumSpo2, maximumBodyTemperature,
             static_cast<unsigned long>(medicalSampleCount), static_cast<unsigned long>(medicalAbnormalCount),
             medicalAbnormalCount > 0 ? "WARNING" : "NORMAL",
             averageRoomTemperature, averageOxygen, averageAqi,
             static_cast<unsigned long>(facilitySampleCount), static_cast<unsigned long>(facilityAbnormalCount),
             facilityAbnormalCount > 0 ? "WARNING" : "NORMAL");
    return String(payload);
}

/**
 * @brief Sizes the MQTT packet buffer for the combined hospital summary.
 *
 * The combined summary can exceed PubSubClient's default packet buffer. The
 * code therefore measures the topic and worst-case payload sizes and adds a
 * safety margin before configuring the client buffer.
 */
void configureMqttBuffer()
{
    String sizingPayload = buildHospitalSummaryPayload(
        999.99f, 100, 999.99f, UINT32_MAX, UINT32_MAX,
        999.99f, 999.99f, 999.99f, UINT32_MAX, UINT32_MAX);
    String hospitalEventsTopic = getAioFeedTopic(HOSPITAL_EVENTS_FEED);
    const uint16_t requiredPacketSize = MQTT_MAX_HEADER_SIZE + 2 + hospitalEventsTopic.length() + sizingPayload.length();
    const uint16_t configuredBufferSize = requiredPacketSize + MQTT_SUMMARY_BUFFER_MARGIN;
    const uint16_t bufferSizeBefore = mqttClient.getBufferSize();

    if (bufferSizeBefore < configuredBufferSize) {
        mqttClient.setBufferSize(configuredBufferSize);
    }

    Serial.print("[MQTT] MQTT buffer size before fix: ");
    Serial.println(bufferSizeBefore);
    Serial.print("[MQTT] MQTT buffer size: ");
    Serial.println(mqttClient.getBufferSize());
}

// MQTT TASK SUPPORT
// ============================================================================

/**
 * @brief Connects the MQTT client to Adafruit IO using configured credentials.
 *
 * The connection is attempted only after Wi-Fi is available. A failed attempt
 * is reported and the MQTT task retries during its normal periodic loop.
 */
using MqttMessageHandler = void (*)(const char *, size_t);

struct MqttMessageRoute {
    const char *topic;
    MqttMessageHandler handler;
};

String medicationDosageTopic;

void handleMedicationDosagePayload(const char *payload, size_t length)
{
    if (length == 0) {
        return;
    }

    const float parsedDosage = strtof(payload, nullptr);
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

MqttMessageRoute mqttRoutes[] = {
    {nullptr, handleMedicationDosagePayload}
};

void mqttMessageCallback(char *topic, byte *payload, unsigned int length)
{
    char payloadBuffer[32];
    if (length == 0 || length >= sizeof(payloadBuffer)) {
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

bool connectMqtt()
{
    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }
    String clientId = "smart-hospital-" + String(static_cast<uint32_t>(ESP.getEfuseMac()), HEX);
    if (mqttClient.connect(clientId.c_str(), AIO_USERNAME, AIO_KEY)) {
        medicationDosageTopic = getAioFeedTopic(MEDICATION_DOSAGE_FEED);
        mqttRoutes[0].topic = medicationDosageTopic.c_str();
        mqttClient.setCallback(mqttMessageCallback);
        mqttClient.subscribe(medicationDosageTopic.c_str());
        mqttClient.loop();
        Serial.println("[MQTT TASK] Connected to Adafruit IO broker");
        return true;
    }
    Serial.print("[MQTT TASK] Connection failed, state=");
    Serial.println(mqttClient.state());
    return false;
}

/**
 * @brief Publishes one changed medical or facility live feed value.
 *
 * Feed names are mapped to the six existing medical and facility Adafruit IO
 * topics. Unchanged values are suppressed, one slot is attempted per MQTT
 * task cycle, and resetLiveFeedPublishState() forces the first value after a
 * connection or reconnection to be sent.
 */
bool publishLiveFeed(const Telemetry &snapshot, const FacilityTelemetry &facilitySnapshot, LiveFeedIndex feedIndex)
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

    if (mqttClient.publish(getAioFeedTopic(feedName.c_str()).c_str(), payload.c_str())) {
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

/**
 * @brief Publishes medical alert or normal-status events when state changes.
 *
 * Both abnormal and normal-status messages use the shared hospital-events
 * feed. Repeated states are suppressed and messages are rate-limited by
 * ALERT_PUBLISH_INTERVAL_MS.
 */
void publishMedicalAlertIfNeeded(const Telemetry &snapshot)
{
    AlertCandidate candidate = evaluateMedicalAlert(snapshot);
    const uint32_t now = millis();
    if (!candidate.active) {
        if (!medicalAlertStateKnown || medicalAlertPublished) {
            if (medicalAlertStateKnown && now - lastMedicalAlertPublishMs < ALERT_PUBLISH_INTERVAL_MS) {
                return;
            }
            const char *normalPayload = "MEDICAL STATUS: NORMAL\nNo active medical alerts.";
            if (mqttClient.publish(getAioFeedTopic(HOSPITAL_EVENTS_FEED).c_str(), normalPayload)) {
                lastMedicalAlertPublishMs = now;
                medicalAlertStateKnown = true;
            }
        }
        medicalAlertPublished = false;
        lastMedicalAlertParameter = "";
        return;
    }

    if (medicalAlertPublished && lastMedicalAlertParameter == candidate.parameter && lastMedicalAlertSeverity == candidate.severity) {
        return;
    }
    if (medicalAlertStateKnown && now - lastMedicalAlertPublishMs < ALERT_PUBLISH_INTERVAL_MS) {
        return;
    }

    String alertPayload = buildAlertPayload("MEDICAL", candidate.parameter, candidate.value, candidate.severity, candidate.message);
    if (mqttClient.publish(getAioFeedTopic(HOSPITAL_EVENTS_FEED).c_str(), alertPayload.c_str())) {
        medicalAlertPublished = true;
        lastMedicalAlertParameter = candidate.parameter;
        lastMedicalAlertSeverity = candidate.severity;
        lastMedicalAlertPublishMs = now;
        medicalAlertStateKnown = true;
    }
}

/**
 * @brief Publishes facility alert or normal-status events when state changes.
 *
 * Both abnormal and normal-status messages use the shared hospital-events
 * feed. Repeated states are suppressed and messages are rate-limited by
 * ALERT_PUBLISH_INTERVAL_MS.
 */
void publishFacilityAlertIfNeeded(const FacilityTelemetry &snapshot)
{
    AlertCandidate candidate = evaluateFacilityAlert(snapshot);
    const uint32_t now = millis();
    if (!candidate.active) {
        if (!facilityAlertStateKnown || facilityAlertPublished) {
            if (facilityAlertStateKnown && now - lastFacilityAlertPublishMs < ALERT_PUBLISH_INTERVAL_MS) {
                return;
            }
            const char *normalPayload = "FACILITY STATUS: NORMAL\nNo active facility alerts.";
            if (mqttClient.publish(getAioFeedTopic(HOSPITAL_EVENTS_FEED).c_str(), normalPayload)) {
                lastFacilityAlertPublishMs = now;
                facilityAlertStateKnown = true;
            }
        }
        facilityAlertPublished = false;
        lastFacilityAlertParameter = "";
        return;
    }

    if (facilityAlertPublished && lastFacilityAlertParameter == candidate.parameter && lastFacilityAlertSeverity == candidate.severity) {
        return;
    }
    if (facilityAlertStateKnown && now - lastFacilityAlertPublishMs < ALERT_PUBLISH_INTERVAL_MS) {
        return;
    }

    String alertPayload = buildAlertPayload("FACILITY", candidate.parameter, candidate.value, candidate.severity, candidate.message);
    if (mqttClient.publish(getAioFeedTopic(HOSPITAL_EVENTS_FEED).c_str(), alertPayload.c_str())) {
        facilityAlertPublished = true;
        lastFacilityAlertParameter = candidate.parameter;
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
void publishSummaryData()
{
    Telemetry snapshot = getTelemetrySnapshot();
    FacilityTelemetry facilitySnapshot = getFacilitySnapshot();

    if (medicalSummaryStats.sampleCount == 0) {
        medicalSummaryStats.minSpo2 = snapshot.spo2;
    }

    if (facilitySummaryStats.sampleCount == 0) {
        facilitySummaryStats.sampleCount = 1;
        facilitySummaryStats.roomTemperatureTotal = facilitySnapshot.roomTemperature;
        facilitySummaryStats.oxygenTotal = facilitySnapshot.oxygenLevel;
        facilitySummaryStats.aqiTotal = facilitySnapshot.aqi;
    }

    const float medicalAverageHeartRate = medicalSummaryStats.sampleCount > 0 ? medicalSummaryStats.heartRateTotal / medicalSummaryStats.sampleCount : static_cast<float>(snapshot.heartRate);
    const int medicalMinSpo2 = medicalSummaryStats.sampleCount > 0 ? medicalSummaryStats.minSpo2 : snapshot.spo2;
    const float medicalMaxBodyTemperature = medicalSummaryStats.sampleCount > 0 ? medicalSummaryStats.maxBodyTemperature : snapshot.temperature;

    const float facilityAverageRoomTemperature = facilitySummaryStats.sampleCount > 0 ? facilitySummaryStats.roomTemperatureTotal / facilitySummaryStats.sampleCount : facilitySnapshot.roomTemperature;
    const float facilityAverageOxygen = facilitySummaryStats.sampleCount > 0 ? facilitySummaryStats.oxygenTotal / facilitySummaryStats.sampleCount : facilitySnapshot.oxygenLevel;
    const float facilityAverageAqi = facilitySummaryStats.sampleCount > 0 ? facilitySummaryStats.aqiTotal / facilitySummaryStats.sampleCount : static_cast<float>(facilitySnapshot.aqi);

    String hospitalSummaryPayload = buildHospitalSummaryPayload(
        medicalAverageHeartRate,
        medicalMinSpo2,
        medicalMaxBodyTemperature,
        medicalSummaryStats.sampleCount,
        medicalSummaryStats.abnormalCount,
        facilityAverageRoomTemperature,
        facilityAverageOxygen,
        facilityAverageAqi,
        facilitySummaryStats.sampleCount,
        facilitySummaryStats.abnormalCount);

    Serial.print("[MQTT] Hospital summary payload length: ");
    Serial.print(hospitalSummaryPayload.length());
    Serial.println(" bytes");
    Serial.println("[MQTT] HOSPITAL SUMMARY PAYLOAD:");
    Serial.println(hospitalSummaryPayload);
    const bool hospitalSummaryPublishResult = mqttClient.publish(getAioFeedTopic(HOSPITAL_EVENTS_FEED).c_str(), hospitalSummaryPayload.c_str());
    Serial.print("[MQTT] hospital summary publish result: ");
    Serial.println(hospitalSummaryPublishResult ? "SUCCESS" : "FAILURE");
    if (!hospitalSummaryPublishResult) {
        Serial.println("[MQTT TASK] Failed to publish hospital summary");
    }

    resetSummaryStats();
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
 * 3 seconds.
 */
void mqttTask(void *parameter)
{
    (void)parameter;
    esp_err_t wdtResult = esp_task_wdt_add(NULL);
    if (wdtResult != ESP_OK) {
        Serial.print("[WATCHDOG] MQTT registration failed: ");
        Serial.println(esp_err_to_name(wdtResult));
        return;
    }
    Serial.println("[WATCHDOG] MQTT task registered with TWDT");

    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSsid, "");
    mqttClient.setServer(mqttBroker, mqttPort);
    mqttClient.setSocketTimeout(3);
    configureMqttBuffer();
    uint32_t lastWifiAttempt = 0;
    uint32_t lastSummaryPublishMs = 0;
    uint32_t lastPublishMs = 0;
    uint32_t nextMqttRetryMs = 0;
    uint32_t mqttRetryDelayMs = MQTT_INITIAL_RETRY_DELAY_MS;
    int nextLiveFeed = LIVE_MEDICAL_HEART_RATE;

    while (true) {
        const uint32_t now = millis();
        if (WiFi.status() != WL_CONNECTED) {
            // Elapsed-time retry avoids repeatedly blocking the MQTT task on Wi-Fi setup.
            if (now - lastWifiAttempt > 10000) {
                lastWifiAttempt = now;
                Serial.println("[MQTT TASK] WiFi unavailable; retrying later");
                WiFi.disconnect();
                WiFi.begin(wifiSsid, "");
            }
        } else if (!mqttClient.connected()) {
            if (now >= nextMqttRetryMs) {
                if (connectMqtt()) {
                    resetLiveFeedPublishState();
                    lastPublishMs = now - MQTT_TASK_INTERVAL_MS;
                    lastSummaryPublishMs = now;
                    mqttRetryDelayMs = MQTT_INITIAL_RETRY_DELAY_MS;
                    nextMqttRetryMs = now;
                } else {
                    nextMqttRetryMs = now + mqttRetryDelayMs;
                    mqttRetryDelayMs = min(mqttRetryDelayMs * 2U, MQTT_MAX_RETRY_DELAY_MS);
                }
            }
        } else {
            mqttClient.loop();

            if (now - lastPublishMs >= MQTT_TASK_INTERVAL_MS) {
                lastPublishMs = now;
                Telemetry snapshot = getTelemetrySnapshot();
                FacilityTelemetry facilitySnapshot = getFacilitySnapshot();

                if (snapshot.temperatureValid) {
                    updateMedicalSummaryStats(snapshot);
                }
                updateFacilitySummaryStats(facilitySnapshot);

                publishLiveFeed(snapshot, facilitySnapshot, static_cast<LiveFeedIndex>(nextLiveFeed));
                nextLiveFeed = (nextLiveFeed + 1) % LIVE_FEED_COUNT;

                publishMedicalAlertIfNeeded(snapshot);
                publishFacilityAlertIfNeeded(facilitySnapshot);

                // Elapsed-time scheduling keeps periodic summary work inside the task loop
                // without adding a separate blocking wait for the 60-second interval.
                if (now - lastSummaryPublishMs >= SUMMARY_INTERVAL_MS) {
                    Serial.println("[MQTT] Summary interval reached");
                    lastSummaryPublishMs = now;
                    publishSummaryData();
                }
            }
        }
        reportTaskHeartbeat(TASK_MQTT);
        vTaskDelay(pdMS_TO_TICKS(MQTT_LOOP_INTERVAL_MS));
    }
}
