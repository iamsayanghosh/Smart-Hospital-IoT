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
 *    safety label, confirmation prompt) plus, when offline, the Task 6 logging banner on the
 *    bottom row (debounced; never replaces the dose). Patient vitals
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
#include "offline_banner_gate.h"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// Offline banner: one inverted 10 px bar on the bottom row (y 54..63).
// Medication content never goes below y = 44, so both are always visible together.
#define BANNER_BAR_Y 54
#define BANNER_BAR_H 10

/** @brief Draws the Task 3 medication screen (dose, status, confirmation prompt). */
static void drawMedicationScreen(const MedicationDosageControl &dose)
{
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    if (dose.criticalConfirmationPending) {
        display.setCursor(0, 0);
        display.println("CRITICAL REQUEST");
        display.setCursor(0, 11);
        display.print("Req: ");
        display.print(dose.requested, 1);
        display.println(" mg/hr");
        display.setCursor(0, 22);
        display.print("Now: ");
        display.print(dose.applied, 1);
        display.println(" mg/hr");
        display.setCursor(0, 36);
        display.println("PRESS BUTTON");
    } else {
        display.setCursor(0, 0);
        display.print("Dose: ");
        display.print(dose.applied, 1);
        display.println(" mg/hr");
        display.setCursor(0, 12);
        display.print("Status: ");
        if (dose.criticalConfirmed) {
            display.println("CRITICAL");
            display.setCursor(0, 24);
            display.println("CONFIRMED");
        } else {
            display.println(getMedicationSafetyLabel(dose.applied));
        }
    }
}

/** @brief Draws the Task 6 "LOGGING OFFLINE" banner as an inverted bar on the bottom row. */
static void drawOfflineBanner()
{
    display.fillRect(0, BANNER_BAR_Y, SCREEN_WIDTH, BANNER_BAR_H, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setTextSize(1);
    display.setCursor(2, BANNER_BAR_Y + 1);
    display.print("LOGGING OFFLINE");
}

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
        bool showOfflineBanner = isOfflineAlertConfirmed();
        if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            display.clearDisplay();
            drawMedicationScreen(dosageSnapshot); // always visible, online or offline
            if (showOfflineBanner) {
                drawOfflineBanner();
            }
            display.display();
            xSemaphoreGive(displayMutex);
        }
        reportTaskHeartbeat(TASK_DISPLAY);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
