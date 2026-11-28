/**
 * @file state.cpp
 * @brief Global state storage, FreeRTOS synchronization objects, and evaluation helpers.
 *
 * Responsibilities:
 * 1. Storage of FreeRTOS IPC handles (sensorQueue, samplingQueue, bedQueue, displayMutex,
 *    alertSemaphore, heartbeatMutex).
 * 2. Storage and critical-section synchronization of global telemetry structs
 *    (telemetry, facilityTelemetry, medicationDosageControl, samplingPeriods).
 * 3. Clinical and facility condition evaluation logic:
 *    - isAbnormal(): Evaluates patient medical vitals against clinical thresholds.
 *    - isFacilityAbnormal(): Evaluates room environment against facility thresholds.
 * 4. Atomic snapshot getters:
 *    - getTelemetrySnapshot(), getFacilitySnapshot(), getMedicationDosageSnapshot().
 * 5. Rolling statistical aggregation for periodic 60 s hospital summary reports.
 */

#include "state.h"

// ============================================================================
// FREERTOS SYNCHRONIZATION OBJECTS
// ============================================================================

QueueHandle_t sensorQueue;
QueueHandle_t samplingQueue;
SemaphoreHandle_t displayMutex;
SemaphoreHandle_t alertSemaphore;
SemaphoreHandle_t heartbeatMutex;
portMUX_TYPE telemetryMux = portMUX_INITIALIZER_UNLOCKED;
QueueHandle_t bedQueue;


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

/**
 * @brief Retrieves a thread-safe atomic copy of current medication control state.
 *
 * @return MedicationDosageControl Snapshot copy.
 */
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
 * @brief Evaluates overall clinical severity of the patient (NORMAL, WARNING, CRITICAL).
 */
AlertSeverity evaluateClinicalSeverity(const Telemetry &data)
{
    AlertCandidate alert = evaluateMedicalAlert(data);
    return alert.active ? alert.severity : ALERT_SEVERITY_NORMAL;
}

/**
 * @brief Adds one valid medical telemetry snapshot to the summary window.
 *
 * The snapshot is supplied by mqttTask while MQTT is connected.
 * Decoupled from temperature validity: Heart Rate and SpO2 accumulate even if the
 * DS18B20 1-Wire temperature sensor is disconnected.
 */
void updateMedicalSummaryStats(const Telemetry &snapshot)
{
    if (!snapshot.initialVitalsProcessed) {
        return;
    }

    static uint32_t lastProcessedSampleCount = 0;
    if (snapshot.vitalSampleCount == lastProcessedSampleCount) {
        return;
    }
    lastProcessedSampleCount = snapshot.vitalSampleCount;

    medicalSummaryStats.sampleCount += 1;
    medicalSummaryStats.heartRateTotal += snapshot.heartRate;
    medicalSummaryStats.minSpo2 = min(medicalSummaryStats.minSpo2, snapshot.spo2);

    if (snapshot.temperatureValid) {
        if (medicalSummaryStats.temperatureSampleCount == 0) {
            medicalSummaryStats.maxBodyTemperature = snapshot.temperature;
        } else {
            medicalSummaryStats.maxBodyTemperature = max(medicalSummaryStats.maxBodyTemperature, snapshot.temperature);
        }
        medicalSummaryStats.temperatureSampleCount += 1;
        medicalSummaryStats.bodyTemperatureTotal += snapshot.temperature;
    }

    AlertCandidate alert = evaluateMedicalAlert(snapshot);
    if (alert.active) {
        medicalSummaryStats.abnormalCount += 1;
        if (alert.severity == ALERT_SEVERITY_CRITICAL) {
            medicalSummaryStats.criticalCount += 1;
        }
    }
}



/**
 * @brief Adds one facility telemetry snapshot to the summary window.
 *
 * The snapshot is supplied by mqttTask while MQTT is connected.
 */
void updateFacilitySummaryStats(const FacilityTelemetry &snapshot)
{
    if (!snapshot.initialFacilityProcessed) {
        return;
    }

    facilitySummaryStats.sampleCount += 1;
    facilitySummaryStats.roomTemperatureTotal += snapshot.roomTemperature;
    facilitySummaryStats.oxygenTotal += snapshot.oxygenLevel;
    facilitySummaryStats.aqiTotal += snapshot.aqi;

    AlertCandidate alert = evaluateFacilityAlert(snapshot);
    if (alert.active) {
        facilitySummaryStats.abnormalCount += 1;
        if (alert.severity == ALERT_SEVERITY_CRITICAL) {
            facilitySummaryStats.criticalCount += 1;
        }
    }
}





    