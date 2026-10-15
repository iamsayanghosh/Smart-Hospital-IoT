/**
 * @file main.cpp
 * @brief ESP32 Smart Hospital IoT monitoring prototype.
 *
 * Implements a FreeRTOS-based concurrent monitoring system with physical
 * DS18B20 temperature and PIR motion inputs; simulated heart rate, SpO2, ECG,
 * room temperature, oxygen, and AQI data; local OLED display; Adafruit IO MQTT
 * communication; alert handling; telemetry aggregation; and task monitoring.
 */

// ============================================================================
// LIBRARIES
// ============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <math.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "freertos/semphr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_task_wdt.h"
#include "adafruit_io_config.h"
#include "display.h"
#include "state.h"
#include "outputs.h"
#include "medication.h"
#include "sensors.h"
#include "environment.h"
#include "app.h"

#define OLED_SDA 21
#define OLED_SCL 22

// ============================================================================
// SETUP
// ============================================================================

/**
 * @brief Initializes hardware, synchronization objects, watchdog, and tasks.
 *
 * The sequence is serial, GPIO and sensor setup, I2C/OLED setup, RTOS object
 * creation, telemetry initialization, watchdog configuration, and task start.
 */
void setup()
{
    // Initialize serial communication for startup, sensor, MQTT, and watchdog logs.
    Serial.begin(115200);
    vTaskDelay(pdMS_TO_TICKS(500));
    Serial.println("\n================================");
    Serial.println("       SMART HOSPITAL IoT       ");
    Serial.println("================================");
    Serial.println("[SETUP] Adafruit IO credentials loaded from include/adafruit_io_config.h or include/adafruit_io_config.local.h");

    outputsInit();
    medicationInit();
    sensorsInit();

    // Initialize I2C and the 128x64 OLED at the configured 0x3C address.
    displayInit(OLED_SDA, OLED_SCL);

    appInit();
    resetSummaryStats();
    appStartTasks();
}

// ============================================================================
// LOOP
// ============================================================================

/**
 * @brief Processes serial simulation commands while FreeRTOS tasks run.
 *
 * The Arduino loop does not handle sensors or MQTT directly; it remains
 * lightweight and yields for 1000 ms after command processing.
 */
void loop()
{
    handleSerialCommands();
    vTaskDelay(pdMS_TO_TICKS(1000));
}