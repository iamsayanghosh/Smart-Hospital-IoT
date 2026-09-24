#include "state.h"

// ============================================================================
// FREERTOS SYNCHRONIZATION OBJECTS
// ============================================================================

QueueHandle_t sensorQueue;
SemaphoreHandle_t displayMutex;
SemaphoreHandle_t alertSemaphore;
SemaphoreHandle_t heartbeatMutex;
portMUX_TYPE telemetryMux = portMUX_INITIALIZER_UNLOCKED;

// ============================================================================
// SHARED TELEMETRY STATE
// ============================================================================

TaskHandle_t monitoredHandles[TASK_MONITORED_COUNT] = {nullptr};
TaskHandle_t watchdogHandle = nullptr;

Telemetry telemetry;
FacilityTelemetry facilityTelemetry;
MedicationDosageControl medicationDosageControl;
MedicalThresholdConfig medicalThresholds;
FacilityThresholdConfig facilityThresholds;
MedicalSummaryStats medicalSummaryStats;
FacilitySummaryStats facilitySummaryStats;
SamplingPeriods samplingPeriods;

bool medicalAlertPublished = false;
bool facilityAlertPublished = false;
String lastMedicalAlertParameter = "";
String lastFacilityAlertParameter = "";
AlertSeverity lastMedicalAlertSeverity = ALERT_SEVERITY_NORMAL;
AlertSeverity lastFacilityAlertSeverity = ALERT_SEVERITY_NORMAL;
uint32_t lastMedicalAlertPublishMs = 0;
uint32_t lastFacilityAlertPublishMs = 0;
bool medicalAlertStateKnown = false;
bool facilityAlertStateKnown = false;



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

MedicationDosageControl getMedicationDosageSnapshot()
{
    MedicationDosageControl snapshot;
    portENTER_CRITICAL(&telemetryMux);
    snapshot = medicationDosageControl;
    portEXIT_CRITICAL(&telemetryMux);
    return snapshot;
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





    