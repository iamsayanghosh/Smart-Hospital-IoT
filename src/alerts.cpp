#include "alerts.h"
#include <Arduino.h>
#include "state.h"
#include "app.h"
#include "watchdog.h"
#include "esp_task_wdt.h"

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
