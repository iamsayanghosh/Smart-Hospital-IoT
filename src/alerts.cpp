/**
 * @file alerts.cpp
 * @brief Implementation of asynchronous medical and facility anomaly event logging.
 *
 * Responsibilities:
 * - Awaits alertSemaphore signaling from telemetry update points (updateTelemetry
 *   for patient vitals and simulateFacilityData called from environmentTask for facility anomalies).
 * - Collects atomic, mutex-protected snapshots of both medical and facility telemetry.
 * - Logs diagnostic details over Serial for debugging and auditing.
 *
 * Hardware & Subsystem Separation:
 * - This task ONLY performs diagnostic Serial logging.
 * - Physical alarm hardware (Buzzer, Red/Yellow/Green LEDs) is managed by
 *   Task 3 (Medication Dosage Safety) and Task 6 (Offline Detection) inside outputs.cpp.
 * - Patient emergency alerts on the cloud are dispatched asynchronously via MQTT
 *   inside mqtt.cpp (publishMedicalAlertIfNeeded -> "hospital-events").
 */

#include "alerts.h"
#include <Arduino.h>
#include "state.h"
#include "watchdog.h"
#include "esp_task_wdt.h"

/**
 * @brief Responds to newly detected medical or facility abnormal states.
 *
 * Execution Flow:
 * 1. Registers task with ESP32 Task Watchdog Timer (TWDT).
 * 2. Continuously reports heartbeat to the software watchdog supervisor.
 * 3. Blocks on alertSemaphore with a 500 ms timeout to ensure regular heartbeat reporting.
 * 4. When signaled (upon abnormal condition transition), safely copies telemetry snapshots
 *    under mutex protection and prints vital parameters to Serial.
 *
 * @param parameter Unused FreeRTOS task parameter pointer (NULL).
 */
void alertTask(void *parameter)
{
    (void)parameter;
    if (!registerTaskWithWatchdog(TASK_ALERT, "ALERT")) {
        return;
    }

    while (true) {
        reportTaskHeartbeat(TASK_ALERT);
        if (xSemaphoreTake(alertSemaphore, pdMS_TO_TICKS(500)) == pdTRUE) {
            Telemetry snapshot = getTelemetrySnapshot();
            FacilityTelemetry facilitySnapshot = getFacilitySnapshot();

            const bool medAbnormal = isAbnormal(snapshot);
            const bool facAbnormal = isFacilityAbnormal(facilitySnapshot);

            if (medAbnormal) {
                Serial.print("[ALERT TASK] Medical anomaly detected: HR=");
                Serial.print(snapshot.heartRate);
                Serial.print(" BPM | SpO2=");
                Serial.print(snapshot.spo2);
                Serial.print(" % | Temp=");
                if (snapshot.temperatureValid) {
                    Serial.print(snapshot.temperature, 1);
                    Serial.println(" C");
                } else {
                    Serial.println("N/A");
                }
            }

            if (facAbnormal) {
                Serial.print("[ALERT TASK] Facility anomaly detected: Room=");
                Serial.print(facilitySnapshot.roomTemperature, 1);
                Serial.print(" C | Oxygen=");
                Serial.print(facilitySnapshot.oxygenLevel, 1);
                Serial.print(" % | AQI=");
                Serial.println(facilitySnapshot.aqi);
            }

            if (!medAbnormal && !facAbnormal) {
                if (snapshot.temperatureValid) {
                    Serial.println("[ALERT TASK] Alert status cleared: All clinical and facility parameters nominal");
                } else {
                    Serial.println("[ALERT TASK] Alert status: Body temperature sensor unavailable; cannot confirm nominal status");
                }
            }
        }
    }
}
