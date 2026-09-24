#include "display.h"
#include "state.h"
#include "watchdog.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app.h" // For failSetup
#include <DallasTemperature.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

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
    } else if (message.type == ECG_DATA) {
        telemetry.ecgRate = message.ecgRate;
        telemetry.ecgSample = message.ecgSample;
    }
    bool abnormal = isAbnormal(telemetry);
    bool wasAbnormal = telemetry.alertActive;
    telemetry.alertActive = abnormal;
    portEXIT_CRITICAL(&telemetryMux);

    if (abnormal && !wasAbnormal) {
        xSemaphoreGive(alertSemaphore);
    }
}

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
            display.clearDisplay();
            display.setTextColor(SSD1306_WHITE);
            display.setTextSize(1);
            if (dosageSnapshot.criticalConfirmationPending) {
                display.setCursor(0, 0);
                display.println("CRITICAL DOSAGE");
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
                display.println("SMART HOSPITAL");
                display.setCursor(0, 12);
                display.print("Dose: ");
                display.print(dosageSnapshot.applied, 1);
                display.println(" mg/hr");
                display.setCursor(0, 30);
                display.print("Status: ");
                display.println(getMedicationSafetyLabel(dosageSnapshot.applied));
            }
            display.display();
            xSemaphoreGive(displayMutex);
        }
        reportTaskHeartbeat(TASK_DISPLAY);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
