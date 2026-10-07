#include "sampling.h"
#include "state.h"
#include "app.h"
#include "watchdog.h"
#include "esp_task_wdt.h"

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

    uint32_t selectedIntervalMs = 5000; // Default to 5 seconds

    while (true) {
        uint32_t newIntervalSec;
        if (xQueueReceive(samplingQueue, &newIntervalSec, 0) == pdTRUE) {
            selectedIntervalMs = newIntervalSec * 1000;
            Serial.print("[SAMPLING TASK] Base interval updated to ");
            Serial.print(newIntervalSec);
            Serial.println(" s");
        }

        Telemetry snapshot = getTelemetrySnapshot();
        bool abnormal = isAbnormal(snapshot);
        uint32_t effectiveIntervalMs = abnormal ? 5000 : selectedIntervalMs;

        portENTER_CRITICAL(&telemetryMux);
        samplingPeriods.temperatureMs = effectiveIntervalMs;
        samplingPeriods.vitalMs = effectiveIntervalMs;
        portEXIT_CRITICAL(&telemetryMux);

        reportTaskHeartbeat(TASK_SAMPLING);
        vTaskDelay(pdMS_TO_TICKS(1000)); // Evaluate once per second
    }
}
