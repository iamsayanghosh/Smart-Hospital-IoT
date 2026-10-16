/**
 * @file sampling.cpp
 * @brief Implementation of Smart Dynamic Sampling Rate (Task 5).
 *
 * Responsibilities & Operating Principles:
 * 1. Clinical State Arbitration:
 *    - Clinically Stable: Automatically selects 60 s (60000 ms) acquisition interval,
 *      minimizing 1-Wire and MAX30102 bus activity and processing overhead.
 *    - Clinically Abnormal: Automatically selects 5 s (5000 ms) high-frequency interval
 *      to closely track physiological deterioration.
 * 2. Doctor Manual Override & Automatic-First Hierarchy:
 *    - The clinician may specify a manual sampling interval between 5 s and 60 s
 *      via the dashboard slider published to MQTT feed "sampling-rate".
 *    - Manual override is preserved ONLY as long as the patient's clinical state remains constant.
 *    - Any clinical transition (STABLE -> ABNORMAL or ABNORMAL -> STABLE) automatically cancels
 *      the manual override and restores the corresponding automatic interval (5 s or 60 s).
 * 3. Startup Grace Period:
 *    - Prior to processing initial vitals and ECG readings, the system applies a conservative
 *      5 s interval to accelerate initial baseline convergence.
 * 4. Separate Schedules:
 *    - Vital and Temperature tasks use this dynamic interval.
 *    - ECG task runs at a fixed 200 ms schedule to maintain diagnostic waveform morphology.
 *    - PIR motion task runs at a fixed 1000 ms schedule to detect room occupancy.
 * 5. Acquisition vs. Transmission Frequency:
 *    - Sensor acquisition occurs every 5 s to 60 s as dictated by samplingPeriods.
 *    - MQTT transmission occurs every 3 s (mqttTask), publishing one changed feed per cycle.
 * 6. Sample-and-Hold Latency Bound:
 *    - In 60 s stable mode, sensor tasks block on vTaskDelay() for up to 60 s. Consequently,
 *      an acute condition arising mid-interval will be detected at the next scheduled acquisition.
 */

#include "sampling.h"
#include "state.h"
#include "app.h"
#include "watchdog.h"
#include "esp_task_wdt.h"

/**
 * @brief Discrete clinical monitoring states for automatic rate selection.
 */
enum SamplingAutoMode {
    SAMPLING_MODE_UNKNOWN,  ///< Initial unassessed clinical condition
    SAMPLING_MODE_STABLE,   ///< All patient vitals within normal clinical limits (60 s target)
    SAMPLING_MODE_ABNORMAL  ///< One or more vitals breaching medical thresholds (5 s target)
};

static const uint32_t AUTO_INTERVAL_STABLE_MS = 60000;         ///< Nominal acquisition period during stable condition (60 s)
static const uint32_t AUTO_INTERVAL_ABNORMAL_MS = 5000;         ///< Rapid acquisition period during abnormal condition (5 s)
static const uint32_t STARTUP_CONSERVATIVE_INTERVAL_MS = 5000; ///< Fallback period prior to initial sensor readiness (5 s)

/**
 * @brief FreeRTOS task executing adaptive sampling rate supervisory arbitration.
 *
 * @param parameter Unused FreeRTOS task parameter pointer (NULL).
 */
void samplingTask(void *parameter)
{
    (void)parameter;
    esp_err_t wdtResult = esp_task_wdt_add(NULL);
    if (wdtResult != ESP_OK) {
        Serial.print("[WATCHDOG] SAMPLING registration failed: ");
        Serial.println(esp_err_to_name(wdtResult));
        return;
    }
    Serial.println("[WATCHDOG] SAMPLING task registered with TWDT");

    SamplingAutoMode lastAutoMode = SAMPLING_MODE_UNKNOWN;
    bool isManualOverride = false;
    uint32_t manualIntervalMs = AUTO_INTERVAL_STABLE_MS;
    uint32_t effectiveIntervalMs = STARTUP_CONSERVATIVE_INTERVAL_MS;
    bool initialEval = true;

    while (true) {
        // 1. Process manual override commands from Adafruit IO via samplingQueue
        uint32_t newIntervalSec = 0;
        if (xQueueReceive(samplingQueue, &newIntervalSec, 0) == pdTRUE) {
            if (newIntervalSec < 5) newIntervalSec = 5;
            if (newIntervalSec > 60) newIntervalSec = 60;
            manualIntervalMs = newIntervalSec * 1000UL;
            isManualOverride = true;
            effectiveIntervalMs = manualIntervalMs;
            Serial.print("[SAMPLING TASK] Manual override applied: ");
            Serial.print(newIntervalSec);
            Serial.println(" s");
        }

        // 2. Assess patient telemetry readiness
        Telemetry snapshot = getTelemetrySnapshot();
        bool patientDataReady = snapshot.initialVitalsProcessed && snapshot.initialEcgProcessed;
        if (!patientDataReady) {
            // Patient condition is not yet known.
            // Maintain conservative high-frequency interval unless doctor explicitly overrode it.
            if (!isManualOverride) {
                effectiveIntervalMs = STARTUP_CONSERVATIVE_INTERVAL_MS;
            }
            portENTER_CRITICAL(&telemetryMux);
            samplingPeriods.temperatureMs = effectiveIntervalMs;
            samplingPeriods.vitalMs = effectiveIntervalMs;
            portEXIT_CRITICAL(&telemetryMux);

            reportTaskHeartbeat(TASK_SAMPLING);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        // 3. Evaluate clinical status using existing clinical thresholds
        SamplingAutoMode currentAutoMode = isAbnormal(snapshot) ? SAMPLING_MODE_ABNORMAL : SAMPLING_MODE_STABLE;

        // 4. State transition and priority resolution:
        // When clinical condition changes (or on initial evaluation), automatic mode
        // takes priority, clearing any active manual override and establishing the auto target.
        if (currentAutoMode != lastAutoMode || initialEval) {
            lastAutoMode = currentAutoMode;
            initialEval = false;
            isManualOverride = false;
            effectiveIntervalMs = (currentAutoMode == SAMPLING_MODE_ABNORMAL) ? AUTO_INTERVAL_ABNORMAL_MS : AUTO_INTERVAL_STABLE_MS;

            Serial.print("[SAMPLING TASK] Clinical state transition: ");
            Serial.println(currentAutoMode == SAMPLING_MODE_ABNORMAL ? "ABNORMAL (Auto 5 s)" : "STABLE (Auto 60 s)");
        } else {
            // Clinical state unchanged: retain active manual override if present, else auto target
            if (isManualOverride) {
                effectiveIntervalMs = manualIntervalMs;
            } else {
                effectiveIntervalMs = (currentAutoMode == SAMPLING_MODE_ABNORMAL) ? AUTO_INTERVAL_ABNORMAL_MS : AUTO_INTERVAL_STABLE_MS;
            }
        }

        // 5. Propagate effective interval to shared state under critical section
        portENTER_CRITICAL(&telemetryMux);
        samplingPeriods.temperatureMs = effectiveIntervalMs;
        samplingPeriods.vitalMs = effectiveIntervalMs;
        portEXIT_CRITICAL(&telemetryMux);

        // 6. Report heartbeat to watchdog and delay for next evaluation iteration
        reportTaskHeartbeat(TASK_SAMPLING);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
