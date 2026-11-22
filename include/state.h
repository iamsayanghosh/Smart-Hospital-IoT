#pragma once

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

/**
 * @file state.h
 * @brief Centralized data structures, FreeRTOS synchronization objects, and state helper declarations.
 *
 * This module defines the system-wide shared data representations and concurrency primitives:
 * - IPC Queues: sensorQueue (sensor messages), samplingQueue (dynamic rate updates), bedQueue (bed angle).
 * - Mutexes & Semaphores: displayMutex (I2C display protection), heartbeatMutex (watchdog table), alertSemaphore (event notification).
 * - Spinlock: telemetryMux (atomic critical sections for multi-task telemetry access).
 *
 * Subsystem Separation Ground Rules:
 * - Telemetry vs MedicationDosageControl: Medical patient vitals and medication dosage safety
 *   are maintained in separate structs and evaluated with completely independent logic.
 */

// ============================================================================
// FREERTOS SYNCHRONIZATION OBJECTS
// ============================================================================

extern QueueHandle_t sensorQueue;         ///< Queue (depth 16) transporting SensorMessage from sensors to displayTask
extern QueueHandle_t samplingQueue;       ///< Queue (depth 1) delivering doctor sampling rate overrides from MQTT
extern SemaphoreHandle_t displayMutex;    ///< Mutex serializing I2C transactions to the SSD1306 OLED display
extern SemaphoreHandle_t alertSemaphore;  ///< Binary semaphore unblocking alertTask on clinical/environmental transitions
extern SemaphoreHandle_t heartbeatMutex;  ///< Mutex protecting taskHeartbeats[] table modifications
extern portMUX_TYPE telemetryMux;         ///< Spinlock critical section protecting global telemetry and dosage structures
extern QueueHandle_t bedQueue;            ///< Queue (depth 1) delivering remote bed angle commands from MQTT


// ============================================================================
// SHARED TELEMETRY STATE
// ============================================================================

enum SensorType {
    TEMP_DATA,
    MOTION_DATA,
    VITAL_DATA,
    ECG_DATA
};

enum MonitoredTaskId {
    TASK_TEMP,
    TASK_MOTION,
    TASK_VITAL,
    TASK_ECG,
    TASK_MQTT,
    TASK_DISPLAY,
    TASK_ALERT,
    TASK_MEDICATION,
    TASK_SAMPLING,
    TASK_BED,
    TASK_OFFLINE,
    TASK_ENVIRONMENT,
    TASK_MONITORED_COUNT
};

// Named clinical critical threshold constants
constexpr float HEART_RATE_CRITICAL_LOW = 45.0f;
constexpr float HEART_RATE_CRITICAL_HIGH = 120.0f;
constexpr float ECG_RATE_CRITICAL_LOW = 45.0f;
constexpr float ECG_RATE_CRITICAL_HIGH = 120.0f;
constexpr float SPO2_CRITICAL_LOW = 88.0f;
constexpr float BODY_TEMP_CRITICAL_LOW = 34.0f;
constexpr float BODY_TEMP_CRITICAL_HIGH = 39.5f;

#ifndef DEVICE_DISCONNECTED_C
#define DEVICE_DISCONNECTED_C -127
#endif

// Named facility critical threshold constants
constexpr float ROOM_TEMP_CRITICAL_LOW = 16.0f;
constexpr float ROOM_TEMP_CRITICAL_HIGH = 30.0f;
constexpr float OXYGEN_CRITICAL_LOW = 18.0f;
constexpr float OXYGEN_CRITICAL_HIGH = 24.5f;
constexpr float AQI_CRITICAL_HIGH = 150.0f;

enum AlertSeverity {
    ALERT_SEVERITY_NORMAL = 0,
    ALERT_SEVERITY_WARNING = 1,
    ALERT_SEVERITY_CRITICAL = 2
};

enum MedicationSafetyLevel {
    MEDICATION_SAFETY_NORMAL = 0,
    MEDICATION_SAFETY_WARNING = 1,
    MEDICATION_SAFETY_CRITICAL = 2
};

struct TaskHeartbeat {
    uint32_t lastHeartbeatMs = 0;
    bool active = false;
};

struct TieredThreshold {
    float low;           ///< Nominal/Warning lower boundary (< triggers warning)
    float high;          ///< Nominal/Warning upper boundary (> triggers warning)
    float criticalLow;   ///< Critical lower boundary (< triggers critical)
    float criticalHigh;  ///< Critical upper boundary (> triggers critical)
};

// Alias for backwards compatibility
using ThresholdRange = TieredThreshold;

struct MedicalThresholdConfig {
    TieredThreshold heartRate = {50.0f, 100.0f, HEART_RATE_CRITICAL_LOW, HEART_RATE_CRITICAL_HIGH};
    TieredThreshold ecgRate = {50.0f, 100.0f, ECG_RATE_CRITICAL_LOW, ECG_RATE_CRITICAL_HIGH};
    TieredThreshold spo2 = {92.0f, 100.0f, SPO2_CRITICAL_LOW, 100.0f};
    TieredThreshold bodyTemperature = {36.0f, 38.0f, BODY_TEMP_CRITICAL_LOW, BODY_TEMP_CRITICAL_HIGH};
};

struct FacilityThresholdConfig {
    TieredThreshold roomTemperature = {18.0f, 28.0f, ROOM_TEMP_CRITICAL_LOW, ROOM_TEMP_CRITICAL_HIGH};
    TieredThreshold oxygen = {19.5f, 24.0f, OXYGEN_CRITICAL_LOW, OXYGEN_CRITICAL_HIGH};
    TieredThreshold aqi = {0.0f, 100.0f, 0.0f, AQI_CRITICAL_HIGH};
};

struct SensorMessage {
    SensorType type;
    float temperature;
    int motion;
    int heartRate;
    int spo2;
    int ecgRate;
    float ecgSample;
};

struct Telemetry {
    float temperature = 0.0;
    bool temperatureValid = false;
    int motion = 0;
    int heartRate = 75;
    int spo2 = 97;
    int ecgRate = 72;
    float ecgSample = 0.0f;
    bool alertActive = false;
    bool initialVitalsProcessed = false;
    bool initialEcgProcessed = false;
    bool initialTempProcessed = false;
    uint32_t vitalSampleCount = 0;
};

struct FacilityTelemetry {
    float roomTemperature = 22.0f;
    float oxygenLevel = 20.8f;
    int aqi = 35;
    bool alertActive = false;
    bool initialFacilityProcessed = false;
};

struct MedicationDosageControl {
    float requested = 0.0f;
    float applied = 0.0f;
    bool received = false;
    uint32_t lastReceivedMs = 0;
    uint32_t lastRateLimitUpdateMs = 0;
    bool criticalConfirmationPending = false;
    bool criticalConfirmed = false;
    bool buttonPressed = false;
    uint32_t lastButtonTransitionMs = 0;
};

struct AlertCandidate {
    bool active = false;
    const char *parameter = nullptr;
    float value = 0.0f;
    const char *message = nullptr;
    AlertSeverity severity = ALERT_SEVERITY_NORMAL;
};

struct MedicalSummaryStats {
    uint32_t sampleCount = 0;
    float heartRateTotal = 0.0f;
    float bodyTemperatureTotal = 0.0f;
    int minSpo2 = 100;
    float maxBodyTemperature = 0.0f;
    uint32_t abnormalCount = 0;
    uint32_t criticalCount = 0;
    uint32_t temperatureSampleCount = 0;
};

struct FacilitySummaryStats {
    uint32_t sampleCount = 0;
    float roomTemperatureTotal = 0.0f;
    float oxygenTotal = 0.0f;
    float aqiTotal = 0.0f;
    uint32_t abnormalCount = 0;
    uint32_t criticalCount = 0;
};

struct SamplingPeriods {
    uint32_t temperatureMs = 2000;
    uint32_t vitalMs = 2000;
    uint32_t ecgMs = 200;
};

extern TaskHandle_t monitoredHandles[TASK_MONITORED_COUNT];
extern TaskHandle_t watchdogHandle;

extern Telemetry telemetry;
extern FacilityTelemetry facilityTelemetry;
extern MedicationDosageControl medicationDosageControl;
extern MedicalThresholdConfig medicalThresholds;
extern FacilityThresholdConfig facilityThresholds;
extern MedicalSummaryStats medicalSummaryStats;
extern FacilitySummaryStats facilitySummaryStats;
extern SamplingPeriods samplingPeriods;
extern bool medicalAlertPublished;
extern bool facilityAlertPublished;
extern bool medicalNormalPending;
extern bool facilityNormalPending;
extern String lastMedicalAlertParameter;
extern String lastFacilityAlertParameter;
extern AlertSeverity lastMedicalAlertSeverity;
extern AlertSeverity lastFacilityAlertSeverity;
extern uint32_t lastMedicalAlertPublishMs;
extern uint32_t lastFacilityAlertPublishMs;
extern bool medicalAlertStateKnown;
extern bool facilityAlertStateKnown;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief Evaluates whether a patient's vitals violate medical threshold limits.
 *
 * Checks heart rate [50 - 100 BPM], SpO2 [>= 92%], ECG rate [50 - 100 BPM], and body temperature [36.0 - 38.0 °C].
 *
 * @param data Telemetry snapshot to assess.
 * @return true if any vital is outside safe physiological range; false if normal.
 */
bool isAbnormal(const Telemetry &data);

/**
 * @brief Evaluates whether room environmental parameters breach facility standards.
 *
 * Checks room temperature [18.0 - 28.0 °C], oxygen [19.5 - 24.0 %], and AQI [<= 100].
 *
 * @param data Facility telemetry snapshot to assess.
 * @return true if environmental parameters exceed bounds; false if nominal.
 */
bool isFacilityAbnormal(const FacilityTelemetry &data);

/**
 * @brief Retrieves a thread-safe atomic copy of current medication control state.
 *
 * @return MedicationDosageControl Snapshot copy.
 */
MedicationDosageControl getMedicationDosageSnapshot();

/**
 * @brief Retrieves a thread-safe atomic copy of current patient medical telemetry.
 *
 * @return Telemetry Snapshot copy.
 */
Telemetry getTelemetrySnapshot();

/**
 * @brief Retrieves a thread-safe atomic copy of current facility environmental telemetry.
 *
 * @return FacilityTelemetry Snapshot copy.
 */
FacilityTelemetry getFacilitySnapshot();

/**
 * @brief Dispatches a SensorMessage to sensorQueue non-blockingly (0 timeout).
 *
 * @param message Sensor reading payload to enqueue.
 * @param taskName Diagnostic task name string for queue overflow warning logs.
 * @return true if enqueued; false if queue was full (sample dropped).
 */
bool sendSensorMessage(const SensorMessage &message, const char *taskName);

/**
 * @brief Evaluates overall clinical severity of the patient (NORMAL, WARNING, CRITICAL).
 *
 * @param data Patient telemetry snapshot.
 * @return AlertSeverity Highest severity among all valid patient vital parameters.
 */
AlertSeverity evaluateClinicalSeverity(const Telemetry &data);

/**
 * @brief Assesses whether the system is still waiting for initial telemetry from any medical sensor.
 *
 * Distinguishes startup latency (INITIALIZING) from a confirmed hardware failure (SENSOR FAULT).
 *
 * @param data Patient telemetry snapshot.
 * @return true if any required medical sensor has not yet completed its first acquisition attempt.
 */
inline bool isMedicalTelemetryInitializing(const Telemetry &data)
{
    return !data.initialVitalsProcessed ||
           !data.initialEcgProcessed ||
           !data.initialTempProcessed;
}

/**
 * @brief Assesses whether a confirmed hardware fault exists for the temperature sensor.
 *
 * Returns true only after a measurement attempt has occurred and confirmed a disconnected
 * or invalid sensor reading (e.g. DEVICE_DISCONNECTED_C or runtime 85°C reset).
 *
 * @param data Patient telemetry snapshot.
 * @return true if temperature acquisition was attempted and failed; false otherwise.
 */
inline bool isMedicalTemperatureFault(const Telemetry &data)
{
    return data.initialTempProcessed && !data.temperatureValid;
}

/**
 * @brief Assesses whether all mandatory medical vitals are available for full clinical clearance (READY).
 *
 * Requires that vitals, ECG, and temperature have all completed initialization and that
 * temperature is currently valid. An unavailable sensor precludes asserting a definitive NORMAL status.
 *
 * @param data Patient telemetry snapshot.
 * @return true if all required measurements are available and valid; false otherwise.
 */
inline bool isMedicalTelemetryFullyValid(const Telemetry &data)
{
    return data.initialVitalsProcessed &&
           data.initialEcgProcessed &&
           data.initialTempProcessed &&
           data.temperatureValid;
}

enum class SensorFaultTransition {
    NONE,
    TRIGGER_FAULT,
    TRIGGER_RECOVERY
};

/**
 * @brief Evaluates whether a sensor availability state transition is required.
 *
 * Distinguishes confirmed sensor failures from startup initialization:
 * - When isTempFault is true and faultActive is false, triggers a FAULT transition.
 * - When isTempFault is false, faultActive is true, and isFullyValid is true, triggers a RECOVERY transition.
 *
 * @param isTempFault True if confirmed temperature hardware fault.
 * @param isFullyValid True if all required telemetry channels are initialized and valid.
 * @param faultActive Current sensor fault state.
 * @return SensorFaultTransition State transition candidate (NONE, TRIGGER_FAULT, TRIGGER_RECOVERY).
 */
inline SensorFaultTransition evaluateSensorFaultTransition(bool isTempFault, bool isFullyValid, bool faultActive)
{
    if (isTempFault && !faultActive) {
        return SensorFaultTransition::TRIGGER_FAULT;
    }
    if (!isTempFault && faultActive && isFullyValid) {
        return SensorFaultTransition::TRIGGER_RECOVERY;
    }
    return SensorFaultTransition::NONE;
}

/**
 * @brief Applies sensor fault state machine mutations upon successful event commit.
 *
 * Invoked ONLY after safePublish() confirms MQTT dispatch or FIFO buffer acceptance.
 *
 * @param transition State transition to apply.
 * @param faultActive In/out reference to active sensor fault flag.
 * @param normalPending In/out reference to pending normal evaluation flag.
 * @return true if state was mutated; false otherwise.
 */
inline bool applySensorFaultTransition(SensorFaultTransition transition, bool &faultActive, bool &normalPending)
{
    if (transition == SensorFaultTransition::TRIGGER_FAULT) {
        faultActive = true;
        return true;
    }
    if (transition == SensorFaultTransition::TRIGGER_RECOVERY) {
        faultActive = false;
        normalPending = true;
        return true;
    }
    return false;
}

/**
 * @brief Updates queue retry pending state based on transmission outcome.
 *
 * Manages FIFO queue retry bookkeeping for critical telemetry frames:
 * - If send fails (e.g. queue full), retains or sets pending flag.
 * - If send succeeds, clears pending flag.
 *
 * @param sendSuccess True if queue send operation succeeded.
 * @param pendingState In/out reference to pending retry flag.
 * @return true if pending state changed; false otherwise.
 */
inline bool updateQueueRetryState(bool sendSuccess, bool &pendingState)
{
    bool prevState = pendingState;
    pendingState = !sendSuccess;
    return prevState != pendingState;
}

enum TemperatureScratchpadType {
    TEMP_READING_VALID,
    TEMP_READING_DISCONNECTED,
    TEMP_READING_COLD_BOOT_SCRATCHPAD,
    TEMP_READING_PERSISTENT_SCRATCHPAD,
    TEMP_READING_RUNTIME_RESET
};

/**
 * @brief Classifies a DS18B20 raw temperature reading to distinguish
 * cold-boot power-on scratchpad (+85.0 C), persistent failure, and runtime reset.
 *
 * @param rawTemp Raw reading from DallasTemperature.
 * @param hasReadValidPhysicalTemperature True if physical sensor previously produced a valid conversion.
 * @param consecutiveScratchpadCount In/out counter for consecutive 85C readings.
 * @return TemperatureScratchpadType Classification result.
 */
inline TemperatureScratchpadType classifyDs18b20Reading(float rawTemp,
                                                        bool hasReadValidPhysicalTemperature,
                                                        uint8_t &consecutiveScratchpadCount)
{
    if (rawTemp == DEVICE_DISCONNECTED_C || rawTemp <= -55.0f || rawTemp > 125.0f) {
        consecutiveScratchpadCount = 0;
        return TEMP_READING_DISCONNECTED;
    }
    if (abs(rawTemp - 85.0f) <= 0.05f) {
        if (!hasReadValidPhysicalTemperature) {
            consecutiveScratchpadCount++;
            if (consecutiveScratchpadCount < 3) {
                return TEMP_READING_COLD_BOOT_SCRATCHPAD;
            }
            return TEMP_READING_PERSISTENT_SCRATCHPAD;
        }
        return TEMP_READING_RUNTIME_RESET;
    }
    // Valid physical reading
    consecutiveScratchpadCount = 0;
    return TEMP_READING_VALID;
}

/**
 * @brief Retries dispatch of a pending temperature disconnect message.
 *
 * If send succeeds, clears pendingDisconnect and latches faultReported=true.
 * If send fails, preserves pendingDisconnect=true.
 *
 * @param sendSuccess True if queue send operation succeeded.
 * @param pendingDisconnect In/out reference to pending disconnect flag.
 * @param faultReported In/out reference to one-shot fault reported latch.
 * @return true if retry was pending and successfully cleared; false otherwise.
 */
inline bool retryTemperatureDisconnect(bool sendSuccess, bool &pendingDisconnect, bool &faultReported)
{
    if (!pendingDisconnect) {
        return false;
    }
    if (sendSuccess) {
        pendingDisconnect = false;
        faultReported = true;
        return true;
    }
    return false;
}

/**
 * @brief Manages one-shot fault notification latch and recovery transitions for sensor outages.
 *
 * Ensures:
 * 1. Exactly one fault notification is published per outage.
 * 2. If a fault fails to enqueue (queue full), pendingDisconnect is set and preserved.
 * 3. Recovery with a valid reading while pendingDisconnect is true NEVER silently drops the pending fault.
 * 4. faultReported latch is cleared ONLY after a recovered valid reading is successfully accepted into the queue.
 *
 * @param isFault True if evaluating a fault condition (persistent 85C or disconnect); false for valid reading recovery.
 * @param sendSuccess True if queue send operation succeeded.
 * @param pendingDisconnect In/out reference to pending disconnect flag.
 * @param faultReported In/out reference to one-shot fault reported latch.
 * @return true if state transitioned; false if duplicate or suppressed.
 */
inline bool updateTemperatureFaultReportState(bool isFault, bool sendSuccess,
                                              bool &pendingDisconnect, bool &faultReported)
{
    if (isFault) {
        if (faultReported || pendingDisconnect) {
            return false; // Duplicate suppressed: already reported or already pending retry
        }
        if (sendSuccess) {
            faultReported = true;
            pendingDisconnect = false;
        } else {
            faultReported = false;
            pendingDisconnect = true;
        }
        return true;
    } else {
        // Recovery / valid temperature reading
        if (pendingDisconnect) {
            // Cannot clear fault state or deliver recovery while disconnect notification is still pending
            return false;
        }
        if (sendSuccess) {
            bool wasReported = faultReported;
            faultReported = false; // Latch cleared only after successful queue acceptance of valid reading
            return wasReported;
        }
        return false;
    }
}

/**
 * @brief Analyzes patient telemetry and constructs an alert candidate if a threshold is violated.
 *
 * Evaluates physiological parameters in priority order: Heart Rate (<45 or >120 BPM Critical,
 * 45-49 or 101-120 BPM Warning), SpO2 (<88% Critical, 88-91% Warning), ECG Rate (<45 or >120 BPM Critical,
 * 45-49 or 101-120 BPM Warning), and Body Temperature (<34°C or >39.5°C Critical, 34-36°C or 38-39.5°C Warning).
 *
 * @param snapshot Evaluated patient vitals snapshot.
 * @param thresholds Clinical threshold configuration (defaults to the global medicalThresholds instance).
 * @return AlertCandidate Populated candidate descriptor.
 */
inline AlertCandidate evaluateMedicalAlert(const Telemetry &snapshot,
                                           const MedicalThresholdConfig &thresholds = medicalThresholds)
{
    AlertCandidate bestCandidate;

    auto checkCandidate = [&](const char* param, float val, const char* msg, AlertSeverity sev) {
        if (!bestCandidate.active || (sev > bestCandidate.severity)) {
            bestCandidate.active = true;
            bestCandidate.parameter = param;
            bestCandidate.value = val;
            bestCandidate.message = msg;
            bestCandidate.severity = sev;
        }
    };

    // 1. SpO2 (Hypoxia / critical airway priority)
    if (snapshot.spo2 < thresholds.spo2.low) {
        AlertSeverity sev = (snapshot.spo2 < thresholds.spo2.criticalLow) 
                            ? ALERT_SEVERITY_CRITICAL : ALERT_SEVERITY_WARNING;
        checkCandidate("spo2", static_cast<float>(snapshot.spo2), "Low SpO2", sev);
    }

    // 2. Heart Rate
    if (snapshot.heartRate < thresholds.heartRate.low || snapshot.heartRate > thresholds.heartRate.high) {
        AlertSeverity sev = (snapshot.heartRate < thresholds.heartRate.criticalLow || snapshot.heartRate > thresholds.heartRate.criticalHigh) 
                            ? ALERT_SEVERITY_CRITICAL : ALERT_SEVERITY_WARNING;
        const char *msg = snapshot.heartRate < thresholds.heartRate.low ? "Low heart rate" : "High heart rate";
        checkCandidate("heartRate", static_cast<float>(snapshot.heartRate), msg, sev);
    }

    // 3. ECG Rate
    if (snapshot.ecgRate < thresholds.ecgRate.low || snapshot.ecgRate > thresholds.ecgRate.high) {
        AlertSeverity sev = (snapshot.ecgRate < thresholds.ecgRate.criticalLow || snapshot.ecgRate > thresholds.ecgRate.criticalHigh) 
                            ? ALERT_SEVERITY_CRITICAL : ALERT_SEVERITY_WARNING;
        const char *msg = snapshot.ecgRate < thresholds.ecgRate.low ? "Low ECG rate" : "High ECG rate";
        checkCandidate("ecgRate", static_cast<float>(snapshot.ecgRate), msg, sev);
    }

    // 4. Body Temperature (only evaluated if sensor is valid/connected)
    if (snapshot.temperatureValid) {
        if (snapshot.temperature < thresholds.bodyTemperature.low || snapshot.temperature > thresholds.bodyTemperature.high) {
            AlertSeverity sev = (snapshot.temperature < thresholds.bodyTemperature.criticalLow || snapshot.temperature > thresholds.bodyTemperature.criticalHigh) 
                                ? ALERT_SEVERITY_CRITICAL : ALERT_SEVERITY_WARNING;
            const char *msg = snapshot.temperature < thresholds.bodyTemperature.low ? "Low body temperature" : "High body temperature";
            checkCandidate("bodyTemperature", snapshot.temperature, msg, sev);
        }
    }

    return bestCandidate;
}

/**
 * @brief Analyzes room telemetry and constructs an alert candidate if environmental limits fail.
 *
 * Evaluates AQI (>100), Room Oxygen (<19.5 or >24 %), and Room Temperature (<18 or >28 °C).
 *
 * @param snapshot Evaluated facility telemetry snapshot.
 * @return AlertCandidate Populated candidate descriptor.
 */
AlertCandidate evaluateFacilityAlert(const FacilityTelemetry &snapshot);

/**
 * @brief Accumulates valid patient vitals into rolling medical statistics window.
 *
 * Updates running sums, sample counts, minimum SpO2, and maximum body temperature.
 *
 * @param snapshot Current patient telemetry snapshot.
 */
void updateMedicalSummaryStats(const Telemetry &snapshot);

/**
 * @brief Accumulates facility telemetry into rolling environmental statistics window.
 *
 * Updates running sums and sample counts for room temperature, oxygen, and AQI.
 *
 * @param snapshot Current facility telemetry snapshot.
 */
void updateFacilitySummaryStats(const FacilityTelemetry &snapshot);
