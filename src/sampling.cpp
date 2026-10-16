#include "sampling.h"
#include "state.h"
#include "app.h"
#include "watchdog.h"
#include "esp_task_wdt.h"

enum SamplingAutoMode {
    SAMPLING_MODE_UNKNOWN,
    SAMPLING_MODE_STABLE,
    SAMPLING_MODE_ABNORMAL
};

static const uint32_t AUTO_INTERVAL_STABLE_MS = 60000;         // 60 seconds
static const uint32_t AUTO_INTERVAL_ABNORMAL_MS = 5000;         // 5 seconds
static const uint32_t STARTUP_CONSERVATIVE_INTERVAL_MS = 5000; // 5 seconds

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
