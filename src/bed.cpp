#include "bed.h"
#include "state.h"
#include "watchdog.h"
#include <ESP32Servo.h>
#include "esp_task_wdt.h"

static const int BED_SERVO_PIN = 18;

void bedTask(void *parameter)
{
    (void)parameter;
    esp_err_t wdtResult = esp_task_wdt_add(NULL);
    if (wdtResult != ESP_OK) {
        Serial.print("[WATCHDOG] BED registration failed: ");
        Serial.println(esp_err_to_name(wdtResult));
        return;
    }
    Serial.println("[WATCHDOG] BED task registered with TWDT");

    Servo bedServo;
    bedServo.setPeriodHertz(50);
    bedServo.attach(BED_SERVO_PIN, 500, 2400);

    float currentAngle = 10.0f;
    float targetAngle = 10.0f;
    bedServo.write(currentAngle);

    AlertSeverity lastSeverity = ALERT_SEVERITY_NORMAL;
    bool forceUpdate = false;

    while (true) {
        // 1. Check for manual override from MQTT
        float manualTarget = -1.0f;
        if (xQueueReceive(bedQueue, &manualTarget, 0) == pdTRUE) {
            targetAngle = manualTarget;
            if (targetAngle < 0.0f) targetAngle = 0.0f;
            if (targetAngle > 90.0f) targetAngle = 90.0f;
            Serial.print("[BED] Manual target angle received: ");
            Serial.println(targetAngle);
        }

        // 2. Check for automatic state change
        Telemetry snapshot = getTelemetrySnapshot();
        AlertCandidate alert = evaluateMedicalAlert(snapshot);
        if (alert.severity != lastSeverity || forceUpdate) {
            lastSeverity = alert.severity;
            forceUpdate = false;
            
            if (lastSeverity == ALERT_SEVERITY_NORMAL) {
                targetAngle = 10.0f;
            } else if (lastSeverity == ALERT_SEVERITY_WARNING) {
                targetAngle = 45.0f;
            } else if (lastSeverity == ALERT_SEVERITY_CRITICAL) {
                targetAngle = 90.0f;
            }
            Serial.print("[BED] Automatic state change, target angle: ");
            Serial.println(targetAngle);
        }

        // 3. Smooth movement
        if (abs(currentAngle - targetAngle) > 0.5f) {
            if (currentAngle < targetAngle) {
                currentAngle += 1.0f;
            } else {
                currentAngle -= 1.0f;
            }
            bedServo.write(currentAngle);
        }

        reportTaskHeartbeat(TASK_BED);
        vTaskDelay(pdMS_TO_TICKS(20)); // 50 Hz update for smooth movement
    }
}
