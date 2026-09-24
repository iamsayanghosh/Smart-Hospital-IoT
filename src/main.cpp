/**
 * @file main.cpp
 * @brief ESP32 Smart Hospital IoT monitoring prototype.
 *
 * Implements a FreeRTOS-based concurrent monitoring system with physical
 * DS18B20 temperature and PIR motion inputs; simulated heart rate, SpO2, ECG,
 * room temperature, oxygen, and AQI data; local OLED display; Adafruit IO MQTT
 * communication; alert handling; telemetry aggregation; and task monitoring.
 */

// ============================================================================
// LIBRARIES
// ============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <math.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#include "freertos/semphr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_task_wdt.h"
#include "adafruit_io_config.h"

// ============================================================================
// HARDWARE PIN DEFINITIONS
// ============================================================================

#define DS18B20_PIN 4
#define PIR_PIN 27
#define OLED_SDA 21
#define OLED_SCL 22
#define RED_LED_PIN 25
#define YELLOW_LED_PIN 26
#define GREEN_LED_PIN 33
#define BUZZER_PIN 14
#define PUSHBUTTON_PIN 13
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

// DS18B20 temperature input: GPIO 4. PIR motion input: GPIO 27.
// OLED uses I2C on SDA GPIO 21 and SCL GPIO 22 at 128x64 resolution.
// Red alert LED: GPIO 25. Yellow LED: GPIO 26. Green LED: GPIO 33.
// Buzzer: GPIO 14. Pushbutton: GPIO 13 for medication confirmation.

// ============================================================================
// MQTT / ADAFRUIT IO CONFIGURATION
// ============================================================================

#define MEDICAL_HEART_RATE_FEED "medical-heart-rate"
#define MEDICAL_SPO2_FEED "medical-spo2"
#define MEDICAL_BODY_TEMPERATURE_FEED "medical-body-temperature"
#define FACILITY_ROOM_TEMPERATURE_FEED "facility-room-temperature"
#define FACILITY_OXYGEN_FEED "facility-oxygen"
#define FACILITY_AQI_FEED "facility-aqi"
#define HOSPITAL_EVENTS_FEED "hospital-events"
#define MEDICATION_DOSAGE_FEED "medication-dosage"

const float MEDICATION_DOSAGE_MIN = 0.0f;
const float MEDICATION_DOSAGE_MAX = 100.0f;
const float MEDICATION_CRITICAL_DOSAGE_THRESHOLD = 80.0f;
const float MEDICATION_RATE_LIMIT_STEP_MG_PER_SEC = 5.0f;
const uint32_t MEDICATION_BUTTON_DEBOUNCE_MS = 50;

const char *wifiSsid = "Wokwi-GUEST";
const char *mqttBroker = AIO_MQTT_SERVER;
const uint16_t mqttPort = AIO_MQTT_PORT;

// ============================================================================
// HARDWARE INTERFACES
// ============================================================================

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
OneWire oneWire(DS18B20_PIN);
DallasTemperature temperatureSensor(&oneWire);
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

// ============================================================================
// FREERTOS SYNCHRONIZATION OBJECTS
// ============================================================================

#include "state.h"
#include "app.h"

// ============================================================================
// WATCHDOG CONFIGURATION
// ============================================================================

// Worker tasks subscribe to the ESP Task Watchdog and feed it through
// reportTaskHeartbeat(). The heartbeat timeout is diagnostic logging; the TWDT
// timeout is the enforcement mechanism for a subscribed task that stops feeding.
const uint32_t TWDT_TIMEOUT_MS = 8000;
const uint32_t WATCHDOG_TIMEOUT_MS = 6000; // Diagnostic heartbeat age limit, below the 8-second TWDT limit.
const uint32_t WATCHDOG_CHECK_INTERVAL_MS = 1000; // Heartbeat scan interval.
const uint32_t HEARTBEAT_MUTEX_WAIT_MS = 50; // Maximum wait to access heartbeat state.
const uint32_t MQTT_TASK_INTERVAL_MS = 3000;
const uint32_t MQTT_LOOP_INTERVAL_MS = 50;
const uint32_t MQTT_INITIAL_RETRY_DELAY_MS = 1000;
const uint32_t MQTT_MAX_RETRY_DELAY_MS = 30000;
const uint32_t STARTUP_GRACE_PERIOD_MS = 2000; // Allows workers to report their first heartbeat.
const uint32_t SUMMARY_INTERVAL_MS = 60000;
const uint32_t ALERT_PUBLISH_INTERVAL_MS = 60000;
const uint16_t MQTT_SUMMARY_BUFFER_MARGIN = 64;




// Tracks whether each live feed has been published after connection or
// reconnection, allowing the first value through even when it has not changed.
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

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================



/**
 * @brief Determines whether medical telemetry is outside configured limits.
 *
 * Requires a valid temperature and evaluates temperature, heart rate, SpO2,
 * and ECG rate against the implemented medical limits.
 */
bool isAbnormal(const Telemetry &data)
{
    if (!data.temperatureValid) {
        return false;
    }
    return data.temperature < medicalThresholds.bodyTemperature.low ||
           data.temperature > medicalThresholds.bodyTemperature.high ||
           data.heartRate < medicalThresholds.heartRate.low ||
           data.heartRate > medicalThresholds.heartRate.high ||
           data.spo2 < medicalThresholds.spo2.low ||
           data.ecgRate < 50 || data.ecgRate > 110;
}

/**
 * @brief Determines whether facility telemetry is outside configured limits.
 *
 * Evaluates the simulated room temperature, oxygen level, and AQI values.
 */
bool isFacilityAbnormal(const FacilityTelemetry &data)
{
    return data.roomTemperature < facilityThresholds.roomTemperature.low ||
           data.roomTemperature > facilityThresholds.roomTemperature.high ||
           data.oxygenLevel < facilityThresholds.oxygen.low ||
           data.oxygenLevel > facilityThresholds.oxygen.high ||
           data.aqi < facilityThresholds.aqi.low ||
           data.aqi > facilityThresholds.aqi.high;
}

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

float clampMedicationDosage(float value)
{
    if (!isfinite(value)) {
        return MEDICATION_DOSAGE_MIN;
    }
    return constrain(value, MEDICATION_DOSAGE_MIN, MEDICATION_DOSAGE_MAX);
}

MedicationDosageControl getMedicationDosageSnapshot()
{
    MedicationDosageControl snapshot;
    portENTER_CRITICAL(&telemetryMux);
    snapshot = medicationDosageControl;
    portEXIT_CRITICAL(&telemetryMux);
    return snapshot;
}

MedicationSafetyLevel getMedicationSafetyLevel(float dosage)
{
    if (dosage > MEDICATION_CRITICAL_DOSAGE_THRESHOLD) {
        return MEDICATION_SAFETY_CRITICAL;
    }
    if (dosage == MEDICATION_CRITICAL_DOSAGE_THRESHOLD) {
        return MEDICATION_SAFETY_WARNING;
    }
    return MEDICATION_SAFETY_NORMAL;
}

const char *getMedicationSafetyLabel(float dosage)
{
    switch (getMedicationSafetyLevel(dosage)) {
        case MEDICATION_SAFETY_WARNING:
            return "WARNING";
        case MEDICATION_SAFETY_CRITICAL:
            return "CRITICAL";
        default:
            return "NORMAL";
    }
}

void updateMedicationDosageFromRequest()
{
    const uint32_t now = millis();
    MedicationDosageControl state;
    portENTER_CRITICAL(&telemetryMux);
    state = medicationDosageControl;
    portEXIT_CRITICAL(&telemetryMux);

    const float targetDosage = clampMedicationDosage(state.requested);

    if (targetDosage <= MEDICATION_CRITICAL_DOSAGE_THRESHOLD) {
        state.criticalConfirmed = false;
    }

    if (state.criticalConfirmationPending && targetDosage <= MEDICATION_CRITICAL_DOSAGE_THRESHOLD) {
        state.criticalConfirmationPending = false;
        state.criticalConfirmed = false;
        state.lastRateLimitUpdateMs = now;
    }

    if (!state.criticalConfirmationPending && !state.criticalConfirmed && targetDosage > MEDICATION_CRITICAL_DOSAGE_THRESHOLD) {
        state.criticalConfirmationPending = true;
        state.applied = min(state.applied, MEDICATION_CRITICAL_DOSAGE_THRESHOLD);
        state.lastRateLimitUpdateMs = now;
    }

    const float goal = state.criticalConfirmationPending ? MEDICATION_CRITICAL_DOSAGE_THRESHOLD : targetDosage;
    if (goal > state.applied) {
        float elapsedSeconds = 1.0f;
        if (state.lastRateLimitUpdateMs != 0U && now > state.lastRateLimitUpdateMs) {
            elapsedSeconds = static_cast<float>(now - state.lastRateLimitUpdateMs) / 1000.0f;
        }
        if (elapsedSeconds <= 0.0f) {
            elapsedSeconds = 1.0f;
        }

        const float maxIncrease = MEDICATION_RATE_LIMIT_STEP_MG_PER_SEC * elapsedSeconds;
        const float allowedIncrease = min(maxIncrease, goal - state.applied);
        state.applied += allowedIncrease;
        state.lastRateLimitUpdateMs = now;
    } else {
        state.applied = goal;
        state.lastRateLimitUpdateMs = now;
    }

    state.applied = constrain(state.applied, MEDICATION_DOSAGE_MIN, MEDICATION_DOSAGE_MAX);

    portENTER_CRITICAL(&telemetryMux);
    medicationDosageControl.applied = state.applied;
    medicationDosageControl.criticalConfirmationPending = state.criticalConfirmationPending;
    medicationDosageControl.criticalConfirmed = state.criticalConfirmed;
    medicationDosageControl.lastRateLimitUpdateMs = state.lastRateLimitUpdateMs;
    portEXIT_CRITICAL(&telemetryMux);
}

void handleMedicationConfirmationButton()
{
    const uint32_t now = millis();
    const bool rawPressed = (digitalRead(PUSHBUTTON_PIN) == LOW);

    portENTER_CRITICAL(&telemetryMux);
    const bool previousPressed = medicationDosageControl.buttonPressed;
    const uint32_t lastTransitionMs = medicationDosageControl.lastButtonTransitionMs;
    portEXIT_CRITICAL(&telemetryMux);

    if (rawPressed != previousPressed && (now - lastTransitionMs) >= MEDICATION_BUTTON_DEBOUNCE_MS) {
        portENTER_CRITICAL(&telemetryMux);
        medicationDosageControl.buttonPressed = rawPressed;
        medicationDosageControl.lastButtonTransitionMs = now;
        if (rawPressed && medicationDosageControl.criticalConfirmationPending) {
            medicationDosageControl.criticalConfirmationPending = false;
            medicationDosageControl.criticalConfirmed = true;
            medicationDosageControl.lastRateLimitUpdateMs = now;
        }
        portEXIT_CRITICAL(&telemetryMux);
    }

}

void updateMedicationSafetyOutputs()
{
    static bool buzzerOn = false;
    MedicationDosageControl state = getMedicationDosageSnapshot();
    const uint32_t now = millis();
    const bool criticalPending = state.criticalConfirmationPending && state.requested > MEDICATION_CRITICAL_DOSAGE_THRESHOLD;
    const bool criticalActive = criticalPending || getMedicationSafetyLevel(state.applied) == MEDICATION_SAFETY_CRITICAL;
    bool redLedOn = false;
    bool yellowLedOn = false;
    bool greenLedOn = false;
    bool desiredBuzzerOn = false;

    if (criticalActive) {
        const bool redBlinkOn = ((now / 150U) % 2U) == 0U;
        redLedOn = redBlinkOn;
        desiredBuzzerOn = redBlinkOn;
    } else if (getMedicationSafetyLevel(state.applied) == MEDICATION_SAFETY_WARNING) {
        yellowLedOn = true;
    } else {
        greenLedOn = true;
    }

    digitalWrite(RED_LED_PIN, redLedOn ? HIGH : LOW);
    digitalWrite(YELLOW_LED_PIN, yellowLedOn ? HIGH : LOW);
    digitalWrite(GREEN_LED_PIN, greenLedOn ? HIGH : LOW);

    if (desiredBuzzerOn != buzzerOn) {
        buzzerOn = desiredBuzzerOn;
        if (buzzerOn) {
            tone(BUZZER_PIN, 1200);
        } else {
            noTone(BUZZER_PIN);
        }
    }
}

// ============================================================================
// MEDICATION TASK
// ============================================================================

/**
 * @brief Runs medication confirmation, dosage control, and safety outputs.
 *
 * The task remains responsive to the button and independent of MQTT timing.
 */
void medicationTask(void *parameter)
{
    (void)parameter;
    esp_err_t wdtResult = esp_task_wdt_add(NULL);
    if (wdtResult != ESP_OK) {
        Serial.print("[WATCHDOG] MEDICATION registration failed: ");
        Serial.println(esp_err_to_name(wdtResult));
        return;
    }
    Serial.println("[WATCHDOG] MEDICATION task registered with TWDT");

    while (true) {
        handleMedicationConfirmationButton();
        updateMedicationDosageFromRequest();
        updateMedicationSafetyOutputs();
        reportTaskHeartbeat(TASK_MEDICATION);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
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

/**
 * @brief Copies medical telemetry while holding the telemetry critical section.
 */
Telemetry getTelemetrySnapshot()
{
    Telemetry snapshot;
    portENTER_CRITICAL(&telemetryMux);
    snapshot = telemetry;
    portEXIT_CRITICAL(&telemetryMux);
    return snapshot;
}

/**
 * @brief Copies facility telemetry while holding the telemetry critical section.
 */
FacilityTelemetry getFacilitySnapshot()
{
    FacilityTelemetry snapshot;
    portENTER_CRITICAL(&telemetryMux);
    snapshot = facilityTelemetry;
    portEXIT_CRITICAL(&telemetryMux);
    return snapshot;
}

/**
 * @brief Enqueues a sensor sample without blocking the producing task.
 *
 * A full queue drops the current sample and reports the condition over serial.
 */
bool sendSensorMessage(const SensorMessage &message, const char *taskName)
{
    if (xQueueSend(sensorQueue, &message, 0) != pdTRUE) {
        Serial.print("[");
        Serial.print(taskName);
        Serial.println("] Queue full; sample skipped");
        return false;
    }
    return true;
}

/**
 * @brief Selects the first active medical alert in a telemetry snapshot.
 *
 * Supported candidates are heart rate, SpO2, and body temperature. ECG rate
 * contributes to isAbnormal(), but is not a separate candidate here.
 */
AlertCandidate evaluateMedicalAlert(const Telemetry &snapshot)
{
    AlertCandidate candidate;
    if (!snapshot.temperatureValid) {
        return candidate;
    }

    if (snapshot.heartRate < medicalThresholds.heartRate.low || snapshot.heartRate > medicalThresholds.heartRate.high) {
        candidate.active = true;
        candidate.parameter = "heartRate";
        candidate.value = static_cast<float>(snapshot.heartRate);
        candidate.message = snapshot.heartRate < medicalThresholds.heartRate.low ? "Low heart rate" : "High heart rate";
        candidate.severity = snapshot.heartRate < 60 || snapshot.heartRate > 110 ? ALERT_SEVERITY_CRITICAL : ALERT_SEVERITY_WARNING;
        return candidate;
    }

    if (snapshot.spo2 < medicalThresholds.spo2.low) {
        candidate.active = true;
        candidate.parameter = "spo2";
        candidate.value = static_cast<float>(snapshot.spo2);
        candidate.message = "Low SpO2";
        candidate.severity = snapshot.spo2 < 88 ? ALERT_SEVERITY_CRITICAL : ALERT_SEVERITY_WARNING;
        return candidate;
    }

    if (snapshot.temperature < medicalThresholds.bodyTemperature.low || snapshot.temperature > medicalThresholds.bodyTemperature.high) {
        candidate.active = true;
        candidate.parameter = "bodyTemperature";
        candidate.value = snapshot.temperature;
        candidate.message = snapshot.temperature < medicalThresholds.bodyTemperature.low ? "Low body temperature" : "High body temperature";
        candidate.severity = snapshot.temperature < 34.0f || snapshot.temperature > 39.5f ? ALERT_SEVERITY_CRITICAL : ALERT_SEVERITY_WARNING;
        return candidate;
    }

    return candidate;
}

/**
 * @brief Selects the first active facility alert in a telemetry snapshot.
 */
AlertCandidate evaluateFacilityAlert(const FacilityTelemetry &snapshot)
{
    AlertCandidate candidate;

    if (snapshot.roomTemperature < facilityThresholds.roomTemperature.low || snapshot.roomTemperature > facilityThresholds.roomTemperature.high) {
        candidate.active = true;
        candidate.parameter = "roomTemperature";
        candidate.value = snapshot.roomTemperature;
        candidate.message = snapshot.roomTemperature < facilityThresholds.roomTemperature.low ? "Low room temperature" : "High room temperature";
        candidate.severity = snapshot.roomTemperature < 16.0f || snapshot.roomTemperature > 30.0f ? ALERT_SEVERITY_CRITICAL : ALERT_SEVERITY_WARNING;
        return candidate;
    }

    if (snapshot.oxygenLevel < facilityThresholds.oxygen.low || snapshot.oxygenLevel > facilityThresholds.oxygen.high) {
        candidate.active = true;
        candidate.parameter = "oxygenLevel";
        candidate.value = snapshot.oxygenLevel;
        candidate.message = snapshot.oxygenLevel < facilityThresholds.oxygen.low ? "Low oxygen level" : "High oxygen level";
        candidate.severity = snapshot.oxygenLevel < 18.0f || snapshot.oxygenLevel > 24.5f ? ALERT_SEVERITY_CRITICAL : ALERT_SEVERITY_WARNING;
        return candidate;
    }

    if (snapshot.aqi < facilityThresholds.aqi.low || snapshot.aqi > facilityThresholds.aqi.high) {
        candidate.active = true;
        candidate.parameter = "aqi";
        candidate.value = static_cast<float>(snapshot.aqi);
        candidate.message = snapshot.aqi > facilityThresholds.aqi.high ? "AQI elevated" : "AQI unusually low";
        candidate.severity = snapshot.aqi > 150 ? ALERT_SEVERITY_CRITICAL : ALERT_SEVERITY_WARNING;
        return candidate;
    }

    return candidate;
}

/**
 * @brief Generates simulated room temperature, oxygen, and AQI values.
 *
 * Values are stored under telemetryMux. A transition into an abnormal facility
 * state signals alertSemaphore for alertTask; these are not physical sensors.
 */
void simulateFacilityData()
{
    FacilityTelemetry nextState = getFacilitySnapshot();

    const float roomDelta = random(-4, 5) * 0.4f;
    const float oxygenDelta = random(-3, 3) * 0.2f;
    const int aqiDelta = random(-12, 18);

    nextState.roomTemperature = constrain(22.0f + roomDelta, 18.0f, 30.0f);
    nextState.oxygenLevel = constrain(20.8f + oxygenDelta, 18.0f, 24.0f);
    nextState.aqi = constrain(35 + aqiDelta, 0, 200);

    portENTER_CRITICAL(&telemetryMux);
    facilityTelemetry = nextState;
    const bool wasAbnormal = facilityTelemetry.alertActive;
    facilityTelemetry.alertActive = isFacilityAbnormal(facilityTelemetry);
    portEXIT_CRITICAL(&telemetryMux);

    if (facilityTelemetry.alertActive && !wasAbnormal) {
        xSemaphoreGive(alertSemaphore);
    }
}

/**
 * @brief Adds one valid medical telemetry snapshot to the summary window.
 *
 * The snapshot is supplied by mqttTask while MQTT is connected.
 */
void updateMedicalSummaryStats(const Telemetry &snapshot)
{
    if (!snapshot.temperatureValid) {
        return;
    }

    medicalSummaryStats.sampleCount += 1;
    medicalSummaryStats.heartRateTotal += snapshot.heartRate;
    medicalSummaryStats.bodyTemperatureTotal += snapshot.temperature;
    medicalSummaryStats.minSpo2 = min(medicalSummaryStats.minSpo2, snapshot.spo2);
    medicalSummaryStats.maxBodyTemperature = max(medicalSummaryStats.maxBodyTemperature, snapshot.temperature);

    AlertCandidate alert = evaluateMedicalAlert(snapshot);
    if (alert.active) {
        medicalSummaryStats.abnormalCount += 1;
    }
}

/**
 * @brief Adds one facility telemetry snapshot to the summary window.
 *
 * The snapshot is supplied by mqttTask while MQTT is connected.
 */
void updateFacilitySummaryStats(const FacilityTelemetry &snapshot)
{
    facilitySummaryStats.sampleCount += 1;
    facilitySummaryStats.roomTemperatureTotal += snapshot.roomTemperature;
    facilitySummaryStats.oxygenTotal += snapshot.oxygenLevel;
    facilitySummaryStats.aqiTotal += snapshot.aqi;

    if (isFacilityAbnormal(snapshot)) {
        facilitySummaryStats.abnormalCount += 1;
    }
}

/**
 * @brief Clears medical and facility accumulators for the next summary window.
 */
void resetSummaryStats()
{
    medicalSummaryStats = MedicalSummaryStats();
    facilitySummaryStats = FacilitySummaryStats();
    medicalSummaryStats.minSpo2 = 100;
}

// ============================================================================
// TEMPERATURE TASK
// ============================================================================

/**
 * @brief Reads the physical DS18B20 temperature sensor.
 *
 * Enqueues each reading for the display task to apply to shared telemetry.
 * It subscribes to the TWDT, reports its heartbeat, and samples every 2 seconds.
 */
void temperatureTask(void *parameter)
{
    (void)parameter;
    esp_err_t wdtResult = esp_task_wdt_add(NULL);
    if (wdtResult != ESP_OK) {
        Serial.print("[WATCHDOG] TEMP registration failed: ");
        Serial.println(esp_err_to_name(wdtResult));
        return;
    }
    Serial.println("[WATCHDOG] TEMP task registered with TWDT");

    while (true) {
        temperatureSensor.requestTemperatures();
        SensorMessage message = {};
        message.type = TEMP_DATA;
        message.temperature = temperatureSensor.getTempCByIndex(0);
        sendSensorMessage(message, "TEMP TASK");

        Serial.print("[TEMP TASK] Temperature: ");
        Serial.print(message.temperature, 1);
        Serial.println(" C");
        reportTaskHeartbeat(TASK_TEMP);
        vTaskDelay(pdMS_TO_TICKS(getSamplePeriodMs(TASK_TEMP)));
    }
}

// ============================================================================
// MOTION TASK
// ============================================================================

/**
 * @brief Reads the physical PIR motion input.
 *
 * Enqueues the digital motion state for the display task. The task feeds its
 * watchdog heartbeat after TWDT registration and samples every 1 second.
 */
void motionTask(void *parameter)
{
    (void)parameter;
    esp_err_t wdtResult = esp_task_wdt_add(NULL);
    if (wdtResult != ESP_OK) {
        Serial.print("[WATCHDOG] MOTION registration failed: ");
        Serial.println(esp_err_to_name(wdtResult));
        return;
    }
    Serial.println("[WATCHDOG] MOTION task registered with TWDT");

    while (true) {
        SensorMessage message = {};
        message.type = MOTION_DATA;
        message.motion = digitalRead(PIR_PIN);
        sendSensorMessage(message, "MOTION TASK");

        Serial.print("[MOTION TASK] Motion: ");
        Serial.println(message.motion ? "DETECTED" : "NO MOTION");
        reportTaskHeartbeat(TASK_MOTION);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ============================================================================
// VITAL SIGNS TASK
// ============================================================================

/**
 * @brief Generates simulated heart-rate and SpO2 values.
 *
 * Vital samples are sent to the display task, while simulated facility values
 * are updated under telemetryMux. The task feeds its watchdog heartbeat and
 * runs every 2 seconds after registering with the TWDT.
 */
void vitalTask(void *parameter)
{
    (void)parameter;
    esp_err_t wdtResult = esp_task_wdt_add(NULL);
    if (wdtResult != ESP_OK) {
        Serial.print("[WATCHDOG] VITAL registration failed: ");
        Serial.println(esp_err_to_name(wdtResult));
        return;
    }
    Serial.println("[WATCHDOG] VITAL task registered with TWDT");

    while (true) {
        SensorMessage message = {};
        message.type = VITAL_DATA;
        message.heartRate = random(68, 81);
        message.spo2 = random(95, 99);
        sendSensorMessage(message, "VITAL TASK");

        Serial.print("[VITAL TASK] HR: ");
        Serial.print(message.heartRate);
        Serial.print(" BPM | SpO2: ");
        Serial.print(message.spo2);
        Serial.println(" %");

        simulateFacilityData();
        reportTaskHeartbeat(TASK_VITAL);
        vTaskDelay(pdMS_TO_TICKS(getSamplePeriodMs(TASK_VITAL)));
    }
}

/**
 * @brief Generates the simulated P/Q/R/S/T ECG waveform used by the prototype.
 */
static float generateEcgWaveform(float phase)
{
    const float pWave = 0.15f * expf(-28.0f * (phase - 0.18f) * (phase - 0.18f));
    const float qWave = -0.12f * expf(-96.0f * (phase - 0.26f) * (phase - 0.26f));
    const float rWave = 1.30f * expf(-180.0f * (phase - 0.33f) * (phase - 0.33f));
    const float sWave = -0.38f * expf(-180.0f * (phase - 0.40f) * (phase - 0.40f));
    const float tWave = 0.18f * expf(-20.0f * (phase - 0.72f) * (phase - 0.72f));
    return pWave + qWave + rWave + sWave + tWave;
}

// ============================================================================
// ECG TASK
// ============================================================================

/**
 * @brief Generates and queues a simulated ECG waveform sample.
 *
 * The waveform and heart rate are passed to displayTask through sensorQueue.
 * The task registers with the TWDT, reports its heartbeat, and samples every
 * 200 ms; it does not acquire physical ECG hardware.
 */
void ecgTask(void *parameter)
{
    (void)parameter;
    esp_err_t wdtResult = esp_task_wdt_add(NULL);
    if (wdtResult != ESP_OK) {
        Serial.print("[WATCHDOG] ECG registration failed: ");
        Serial.println(esp_err_to_name(wdtResult));
        return;
    }
    Serial.println("[WATCHDOG] ECG task registered with TWDT");

    float phase = 0.0f;
    int ecgRate = 72;

    while (true) {
        const float cycleDuration = 60.0f / static_cast<float>(ecgRate);
        const float normalizedPhase = fmodf(phase, cycleDuration) / cycleDuration;
        SensorMessage message = {};
        message.type = ECG_DATA;
        message.ecgRate = ecgRate;
        message.ecgSample = generateEcgWaveform(normalizedPhase);
        sendSensorMessage(message, "ECG TASK");

        if ((millis() / 1000) % 5 == 0 && message.ecgSample > 0.8f) {
            Serial.print("[ECG TASK] Simulated rhythm: ");
            Serial.print(message.ecgRate);
            Serial.print(" BPM | sample=");
            Serial.println(message.ecgSample, 2);
        }

        const uint32_t sampleMs = getSamplePeriodMs(TASK_ECG);
        phase += static_cast<float>(sampleMs) / 1000.0f;
        if (phase >= cycleDuration) {
            phase -= cycleDuration;
            ecgRate = random(70, 79);
        }

        reportTaskHeartbeat(TASK_ECG);
        vTaskDelay(pdMS_TO_TICKS(sampleMs));
    }
}

/**
 * @brief Applies one queued sensor message to shared medical telemetry.
 *
 * Updates are protected by telemetryMux. A transition into an abnormal state
 * signals alertSemaphore so the alert task can report it.
 */
void updateTelemetry(const SensorMessage &message)
{
    portENTER_CRITICAL(&telemetryMux);
    if (message.type == TEMP_DATA) {
        telemetry.temperature = message.temperature;
        telemetry.temperatureValid = isfinite(message.temperature) &&
                                     message.temperature != DEVICE_DISCONNECTED_C;
    } else if (message.type == MOTION_DATA) {
        telemetry.motion = message.motion;
    } else if (message.type == VITAL_DATA) {
        telemetry.heartRate = message.heartRate;
        telemetry.spo2 = message.spo2;
    } else if (message.type == ECG_DATA) {
        telemetry.ecgRate = message.ecgRate;
        telemetry.ecgSample = message.ecgSample;
    }
    bool abnormal = isAbnormal(telemetry);
    bool wasAbnormal = telemetry.alertActive;
    telemetry.alertActive = abnormal;
    portEXIT_CRITICAL(&telemetryMux);

    if (abnormal && !wasAbnormal) {
        xSemaphoreGive(alertSemaphore);
    }
}

// ============================================================================
// DISPLAY TASK
// ============================================================================

/**
 * @brief Consumes sensor messages and refreshes the OLED status display.
 *
 * Drains sensorQueue, updates shared telemetry under its critical section, and
 * serializes OLED access with displayMutex. The task feeds its watchdog
 * heartbeat and refreshes every 100 ms, with a queue wait of up to 100 ms.
 */
void displayTask(void *parameter)
{
    (void)parameter;
    esp_err_t wdtResult = esp_task_wdt_add(NULL);
    if (wdtResult != ESP_OK) {
        Serial.print("[WATCHDOG] DISPLAY registration failed: ");
        Serial.println(esp_err_to_name(wdtResult));
        return;
    }
    Serial.println("[WATCHDOG] DISPLAY task registered with TWDT");

    SensorMessage message;
    while (true) {
        if (xQueueReceive(sensorQueue, &message, pdMS_TO_TICKS(100)) == pdTRUE) {
            updateTelemetry(message);
            while (xQueueReceive(sensorQueue, &message, 0) == pdTRUE) {
                updateTelemetry(message);
            }
        }

        MedicationDosageControl dosageSnapshot = getMedicationDosageSnapshot();
        if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            display.clearDisplay();
            display.setTextColor(SSD1306_WHITE);
            display.setTextSize(1);
            if (dosageSnapshot.criticalConfirmationPending) {
                display.setCursor(0, 0);
                display.println("CRITICAL DOSAGE");
                display.setCursor(0, 12);
                display.print("Req: ");
                display.print(dosageSnapshot.requested, 1);
                display.println(" mg/hr");
                display.setCursor(0, 30);
                display.print("Now: ");
                display.print(dosageSnapshot.applied, 1);
                display.println(" mg/hr");
                display.setCursor(0, 45);
                display.println("PRESS BUTTON");
            } else {
                display.setCursor(0, 0);
                display.println("SMART HOSPITAL");
                display.setCursor(0, 12);
                display.print("Dose: ");
                display.print(dosageSnapshot.applied, 1);
                display.println(" mg/hr");
                display.setCursor(0, 30);
                display.print("Status: ");
                display.println(getMedicationSafetyLabel(dosageSnapshot.applied));
            }
            display.display();
            xSemaphoreGive(displayMutex);
        }
        reportTaskHeartbeat(TASK_DISPLAY);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ============================================================================
// ALERT TASK
// ============================================================================

/**
 * @brief Responds to newly detected medical or facility abnormal states.
 *
 * Reads protected telemetry snapshots after alertSemaphore is signaled and
 * logs the event. It feeds its watchdog heartbeat after TWDT registration
 * while waiting for alerts, using a 500 ms semaphore wait and 50 ms retry
 * delay.
 */
void alertTask(void *parameter)
{
    (void)parameter;
    esp_err_t wdtResult = esp_task_wdt_add(NULL);
    if (wdtResult != ESP_OK) {
        Serial.print("[WATCHDOG] ALERT registration failed: ");
        Serial.println(esp_err_to_name(wdtResult));
        return;
    }
    Serial.println("[WATCHDOG] ALERT task registered with TWDT");

    while (true) {
        reportTaskHeartbeat(TASK_ALERT);
        if (xSemaphoreTake(alertSemaphore, pdMS_TO_TICKS(500)) == pdTRUE) {
            Telemetry snapshot = getTelemetrySnapshot();
            FacilityTelemetry facilitySnapshot = getFacilitySnapshot();
            Serial.print("[ALERT TASK] Medical abnormal: HR=");
            Serial.print(snapshot.heartRate);
            Serial.print(" SpO2=");
            Serial.print(snapshot.spo2);
            Serial.print(" Temp=");
            Serial.print(snapshot.temperature, 1);
            Serial.print(" | Facility abnormal: room=");
            Serial.print(facilitySnapshot.roomTemperature, 1);
            Serial.print("C oxygen=");
            Serial.print(facilitySnapshot.oxygenLevel, 1);
            Serial.print("% AQI=");
            Serial.println(facilitySnapshot.aqi);

        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

// ============================================================================
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

// ============================================================================
// WATCHDOG TASK
// ============================================================================

/**
 * @brief Supervises worker heartbeats and feeds its own watchdog subscription.
 *
 * Monitors temperature, motion, vital, ECG, MQTT, display, alert, and medication tasks.
 * Each task registers with the ESP Task Watchdog and reports through
 * reportTaskHeartbeat(); this supervisor checks active state and heartbeat age
 * under heartbeatMutex. The eight workers are checked every 1 second against
 * the 6-second diagnostic limit. The supervisor itself is subscribed to the
 * 8-second TWDT and feeds it each loop; a 2-second startup grace period allows
 * workers to report their first heartbeat.
 */
void watchdogTask(void *parameter)
{
    (void)parameter;
    esp_err_t addResult = esp_task_wdt_add(NULL);
    if (addResult != ESP_OK && addResult != ESP_ERR_INVALID_STATE) {
        Serial.print("[WATCHDOG] Failed to subscribe watchdog to self: ");
        Serial.println(esp_err_to_name(addResult));
    }

    Serial.println("[WATCHDOG] Task health monitor active");
    const uint32_t monitoringStartMs = millis();
    uint32_t lastHealthyLogMs = 0;
    uint32_t lastStackLogMs = 0;

    while (true) {
        uint32_t now = millis();
        bool unhealthyFound = false;

        const bool startupGraceActive =
            (now - monitoringStartMs) < STARTUP_GRACE_PERIOD_MS;

        for (int taskIndex = 0; taskIndex < TASK_MONITORED_COUNT; ++taskIndex) {
            TaskHeartbeat heartbeat;
            if (xSemaphoreTake(heartbeatMutex, pdMS_TO_TICKS(HEARTBEAT_MUTEX_WAIT_MS)) == pdTRUE) {
                heartbeat = taskHeartbeats[taskIndex];
                xSemaphoreGive(heartbeatMutex);
            } else {
                Serial.println("[WATCHDOG] Could not read task heartbeat state");
                unhealthyFound = true;
                continue;
            }

            if (!heartbeat.active) {
                if (startupGraceActive) {
                    continue;
                }
                unhealthyFound = true;
                Serial.print("[WATCHDOG] Heartbeat timeout: ");
                Serial.print(taskNameFromId(static_cast<MonitoredTaskId>(taskIndex)));
                Serial.println(" has not reported a heartbeat yet");
                continue;
            }

            if ((now - heartbeat.lastHeartbeatMs) > WATCHDOG_TIMEOUT_MS) {
                unhealthyFound = true;
                Serial.print("[WATCHDOG] Heartbeat timeout: ");
                Serial.print(taskNameFromId(static_cast<MonitoredTaskId>(taskIndex)));
                Serial.print(" missed last heartbeat by ");
                Serial.print(now - heartbeat.lastHeartbeatMs);
                Serial.println(" ms");
            }
        }

        if (!unhealthyFound) {
            if (millis() - lastHealthyLogMs > 10000) {
                Serial.println("[WATCHDOG] All monitored tasks healthy");
                lastHealthyLogMs = millis();
            }
        }

        if (now - lastStackLogMs >= 30000U) {
            lastStackLogMs = now;
            Serial.print("[WATCHDOG] Free heap: ");
            Serial.println(ESP.getFreeHeap());
            Serial.print("[WATCHDOG] Minimum free heap: ");
            Serial.println(ESP.getMinFreeHeap());
            for (int taskIndex = 0; taskIndex < TASK_MONITORED_COUNT; ++taskIndex) {
                if (monitoredHandles[taskIndex] != nullptr) {
                    Serial.print("[WATCHDOG] ");
                    Serial.print(taskNameFromId(static_cast<MonitoredTaskId>(taskIndex)));
                    Serial.print(" stack high-water mark: ");
                    Serial.println(uxTaskGetStackHighWaterMark(monitoredHandles[taskIndex]));
                }
            }
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(WATCHDOG_CHECK_INTERVAL_MS));
    }
}

/**
 * @brief Logs a fatal initialization error and holds the system in a safe loop.
 */


/**
 * @brief Processes serial commands for facility telemetry simulation/testing.
 *
 * Supported commands are SET_ROOM <C>, SET_OXYGEN <percent>, SET_AQI <value>,
 * RESET_FACILITY, and SHOW_TASK2; these are simulation controls, not
 * production facility-management commands.
 */
void handleSerialCommands()
{
    static char lineBuffer[96];
    static size_t lineLength = 0;

    while (Serial.available() > 0) {
        const char character = static_cast<char>(Serial.read());
        if (character == '\n' || character == '\r') {
            if (lineLength == 0) {
                continue;
            }
            lineBuffer[lineLength] = '\0';
            String command(lineBuffer);
            lineLength = 0;

            if (command.startsWith("SET_ROOM ")) {
                String value = command.substring(9);
                portENTER_CRITICAL(&telemetryMux);
                facilityTelemetry.roomTemperature = value.toFloat();
                const float roomTemperature = facilityTelemetry.roomTemperature;
                portEXIT_CRITICAL(&telemetryMux);
                Serial.print("[SIM] Room temperature set to ");
                Serial.println(roomTemperature, 1);
            } else if (command.startsWith("SET_OXYGEN ")) {
                String value = command.substring(11);
                const float oxygenLevel = constrain(value.toFloat(), 16.0f, 25.0f);
                portENTER_CRITICAL(&telemetryMux);
                facilityTelemetry.oxygenLevel = oxygenLevel;
                portEXIT_CRITICAL(&telemetryMux);
                Serial.print("[SIM] Oxygen set to ");
                Serial.println(oxygenLevel, 1);
            } else if (command.startsWith("SET_AQI ")) {
                String value = command.substring(8);
                const int aqi = constrain(value.toInt(), 0, 250);
                portENTER_CRITICAL(&telemetryMux);
                facilityTelemetry.aqi = aqi;
                portEXIT_CRITICAL(&telemetryMux);
                Serial.print("[SIM] AQI set to ");
                Serial.println(aqi);
            } else if (command == "RESET_FACILITY") {
                portENTER_CRITICAL(&telemetryMux);
                facilityTelemetry.roomTemperature = 22.0f;
                facilityTelemetry.oxygenLevel = 20.8f;
                facilityTelemetry.aqi = 35;
                portEXIT_CRITICAL(&telemetryMux);
                Serial.println("[SIM] Facility simulation reset to normal range");
            } else if (command == "SHOW_TASK2") {
                Serial.println("[TASK2] Medical feeds: medical-heart-rate | medical-spo2 | medical-body-temperature");
                Serial.println("[TASK2] Facility feeds: facility-room-temperature | facility-oxygen | facility-aqi");
                Serial.println("[TASK2] Event feed: hospital-events (alerts + summaries + status)");
            } else {
                Serial.println("[SIM] Commands: SET_ROOM <C>, SET_OXYGEN <percent>, SET_AQI <value>, RESET_FACILITY, SHOW_TASK2");
            }
            continue;
        }

        if (lineLength < sizeof(lineBuffer) - 1) {
            lineBuffer[lineLength++] = character;
        }
    }
}

// ============================================================================
// SETUP
// ============================================================================

/**
 * @brief Initializes hardware, synchronization objects, watchdog, and tasks.
 *
 * The sequence is serial, GPIO and sensor setup, I2C/OLED setup, RTOS object
 * creation, telemetry initialization, watchdog configuration, and task start.
 */
void setup()
{
    // Initialize serial communication for startup, sensor, MQTT, and watchdog logs.
    Serial.begin(115200);
    vTaskDelay(pdMS_TO_TICKS(500));
    Serial.println("\n================================");
    Serial.println(" SMART HOSPITAL IoT - TASK 3");
    Serial.println(" REMOTE MEDICATION DOSAGE CONTROL");
    Serial.println("================================");
    Serial.println("[TASK3] Adafruit IO credentials loaded from include/adafruit_io_config.h or include/adafruit_io_config.local.h");

    // Initialize the PIR GPIO and physical DS18B20 temperature interface.
    pinMode(PIR_PIN, INPUT);
    pinMode(RED_LED_PIN, OUTPUT);
    pinMode(YELLOW_LED_PIN, OUTPUT);
    pinMode(GREEN_LED_PIN, OUTPUT);
    pinMode(BUZZER_PIN, OUTPUT);
    pinMode(PUSHBUTTON_PIN, INPUT_PULLUP);
    digitalWrite(RED_LED_PIN, LOW);
    digitalWrite(YELLOW_LED_PIN, LOW);
    digitalWrite(GREEN_LED_PIN, LOW);
    noTone(BUZZER_PIN);
    temperatureSensor.begin();

    // Initialize I2C and the 128x64 OLED at the configured 0x3C address.
    Wire.begin(OLED_SDA, OLED_SCL);
    Wire.setClock(400000);
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        failSetup("[SETUP] OLED initialization failed");
    }
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(20, 10);
    display.println("SMART HOSPITAL");
    display.setCursor(10, 30);
    display.println("Initializing...");
    display.display();

    appInit();
    resetSummaryStats();
    appStartTasks();
}

// ============================================================================
// LOOP
// ============================================================================

/**
 * @brief Processes serial simulation commands while FreeRTOS tasks run.
 *
 * The Arduino loop does not handle sensors or MQTT directly; it remains
 * lightweight and yields for 1000 ms after command processing.
 */
void loop()
{
    handleSerialCommands();
    vTaskDelay(pdMS_TO_TICKS(1000));
}