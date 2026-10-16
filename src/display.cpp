/**
 * @file display.cpp
 * @brief SSD1306 OLED rendering and centralized sensorQueue message processing.
 *
 * Responsibilities:
 * 1. Hardware Control: Drives the 128x64 monochrome OLED via I2C at 400 kHz.
 * 2. Central Telemetry Aggregator: Drains sensorQueue (carrying TEMP, MOTION, VITAL,
 *    and ECG messages from sensor tasks) and writes atomically into the global
 *    telemetry structure under telemetryMux critical section.
 * 3. Alert Triggering: Detects rising-edge clinical abnormalities (abnormal && !wasAbnormal)
 *    and gives alertSemaphore to awaken alertTask.
 * 4. UI Ownership: Exclusively displays Task 3 Medication Dosage status (applied dose,
 *    safety label, confirmation prompt) or Task 6 offline logging banner. Patient vitals
 *    are intentionally NOT rendered on this display.
 */

#include "display.h"
#include "state.h"
#include "medication.h"
#include "watchdog.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app.h" // For failSetup
#include <DallasTemperature.h>
#include "offline.h"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

/**
 * @brief Configures I2C bus clock and initializes SSD1306 display hardware.
 *
 * @param sda GPIO pin for I2C data line.
 * @param scl GPIO pin for I2C clock line.
 */
void displayInit(uint8_t sda, uint8_t scl)
{
    Wire.begin(sda, scl);
    Wire.setClock(400000);
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        failSetup("[SETUP] OLED initialization failed");
    }
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("SYSTEM START");
    display.display();
}

/**
 * @brief Ingests a single SensorMessage and merges it into the shared telemetry state.
 *
 * Concurrency & Synchronization:
 * - Executes within a spinlock critical section (portENTER_CRITICAL / portEXIT_CRITICAL)
 *   protecting the global telemetry structure.
 * - Marks initial validity flags (temperatureValid, initialVitalsProcessed, initialEcgProcessed).
 * - Evaluates isAbnormal(telemetry). If the clinical condition transitions from normal to abnormal,
 *   it signals alertSemaphore to wake up alertTask asynchronously.
 *
 * @param message Sensor message popped from sensorQueue.
 */
void updateTelemetry(const SensorMessage &message)
{
    portENTER_CRITICAL(&telemetryMux);
    if (message.type == TEMP_DATA) {
        telemetry.temperature = message.temperature;
        telemetry.temperatureValid = isfinite(message.temperature) &&
                                     message.temperature != DEVICE_DISCONNECTED_C;
    } else if (message.type == MOTION_DATA) {
        telemetry.motion = message.motion;
    } else if (message.type == VITAL_DATA) {
        telemetry.heartRate = message.heartRate;
        telemetry.spo2 = message.spo2;
        telemetry.initialVitalsProcessed = true;
    } else if (message.type == ECG_DATA) {
        telemetry.ecgRate = message.ecgRate;
        telemetry.ecgSample = message.ecgSample;
        telemetry.initialEcgProcessed = true;
    }
    bool abnormal = isAbnormal(telemetry);
    bool wasAbnormal = telemetry.alertActive;
    telemetry.alertActive = abnormal;
    portEXIT_CRITICAL(&telemetryMux);

    if (abnormal && !wasAbnormal) {
        xSemaphoreGive(alertSemaphore);
    }
}

/**
 * @brief FreeRTOS task responsible for draining sensorQueue and refreshing OLED UI.
 *
 * @param parameter Unused FreeRTOS task parameter pointer (NULL).
 */
void displayTask(void *parameter)
{
    (void)parameter;
    esp_err_t wdtResult = esp_task_wdt_add(NULL);
    if (wdtResult != ESP_OK) {
        Serial.print("[WATCHDOG] DISPLAY registration failed: ");
        Serial.println(esp_err_to_name(wdtResult));
        return;
    }
    Serial.println("[WATCHDOG] DISPLAY task registered with TWDT");

    SensorMessage message;
    while (true) {
        if (xQueueReceive(sensorQueue, &message, pdMS_TO_TICKS(100)) == pdTRUE) {
            updateTelemetry(message);
            while (xQueueReceive(sensorQueue, &message, 0) == pdTRUE) {
                updateTelemetry(message);
            }
        }

        MedicationDosageControl dosageSnapshot = getMedicationDosageSnapshot();
        if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            SystemHealthState health = getSystemHealthState();
            if (health == SYSTEM_OFFLINE) {
                display.clearDisplay();
                display.setTextColor(SSD1306_WHITE);
                display.setTextSize(1);
                display.setCursor(0, 0);
                display.println("LOGGING OFFLINE");
            } else {
                display.clearDisplay();
                display.setTextColor(SSD1306_WHITE);
                display.setTextSize(1);
                if (dosageSnapshot.criticalConfirmationPending) {
                    display.setCursor(0, 0);
                    display.println("CRITICAL REQUEST");
                    display.setCursor(0, 12);
                    display.print("Req: ");
                    display.print(dosageSnapshot.requested, 1);
                    display.println(" mg/hr");
                    display.setCursor(0, 30);
                    display.print("Now: ");
                    display.print(dosageSnapshot.applied, 1);
                    display.println(" mg/hr");
                    display.setCursor(0, 45);
                    display.println("PRESS BUTTON");
                } else {
                    display.setCursor(0, 0);
                    display.print("Dose: ");
                    display.print(dosageSnapshot.applied, 1);
                    display.println(" mg/hr");
                    display.setCursor(0, 12);
                    display.print("Status: ");
                    if (dosageSnapshot.criticalConfirmed) {
                        display.println("CRITICAL");
                    } else {
                        display.println(getMedicationSafetyLabel(dosageSnapshot.applied));
                    }
                    if (dosageSnapshot.criticalConfirmed) {
                        display.setCursor(0, 24);
                        display.println("CONFIRMED");
                    }
                }
            }
            display.display();
            xSemaphoreGive(displayMutex);
        }
        reportTaskHeartbeat(TASK_DISPLAY);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
