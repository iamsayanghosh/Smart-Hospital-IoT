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


const float MEDICATION_DOSAGE_MIN = 0.0f;

const float MEDICATION_DOSAGE_MAX = 100.0f;

const float MEDICATION_CRITICAL_DOSAGE_THRESHOLD = 80.0f;

const float MEDICATION_RATE_LIMIT_STEP_MG_PER_SEC = 5.0f;

    