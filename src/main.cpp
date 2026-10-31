/**
 * @file main.cpp
 * @brief ESP32 Smart Hospital IoT monitoring system firmware entry point.
 *
 * Implements a concurrent, multi-task medical and facility supervisory system
 * fulfilling all six course tasks:
 * - Task 1: Multi-Sensor Expansion Using FreeRTOS (DS18B20 temp, PIR motion, PPG vitals, ECG waveform).
 * - Task 2: Dual-Role IoT Monitoring System (Separate clinical vitals and facility environment feeds).
 * - Task 3: Smart Medication Dosage Safety (Rate-limited infusion pump with two-factor physical confirmation).
 * - Task 4: Intelligent Remote Bed Elevation (Automatic emergency/breathing elevation with doctor override).
 * - Task 5: Smart Dynamic Sampling Rate (60 s stable vs 5 s abnormal adaptive monitoring with doctor slider).
 * - Task 6: Advanced Offline Detection (Network failure sensing, local queue buffering, and acoustic alerts).
 *
 * Architectural Design:
 * - Core 1 Execution: All application tasks are pinned to Core 1 via FreeRTOS.
 * - Hardware Watchdog: Protected by ESP32 Task Watchdog Timer (TWDT) with 8-second hardware timeout.
 * - Minimal Loop: Arduino loop() is dedicated to serial command handling and periodic 1000 ms yields.
 */


// ============================================================================
// LIBRARIES
// ============================================================================

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <Wire.h>
#include <math.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "adafruit_io_config.h"
#include "app.h"
#include "display.h"
#include "environment.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "medication.h"
#include "outputs.h"
#include "sensors.h"
#include "state.h"

#define OLED_SDA 21 ///< I2C Data line for SSD1306 OLED display
#define OLED_SCL 22 ///< I2C Clock line for SSD1306 OLED display

// ============================================================================
// SETUP
// ============================================================================

/**
 * @brief Initializes hardware, synchronization objects, watchdog, and tasks.
 *
 * The sequence is serial, GPIO and sensor setup, I2C/OLED setup, RTOS object
 * creation, telemetry initialization, watchdog configuration, and task start.
 */
void setup() {
  // Initialize serial communication for startup, sensor, MQTT, and watchdog
  // logs.
  Serial.begin(115200);
  vTaskDelay(pdMS_TO_TICKS(500));
  Serial.println("\n================================");
  Serial.println("       SMART HOSPITAL IoT       ");
  Serial.println("================================");
  Serial.println(
      "[SETUP] Adafruit IO credentials loaded from "
      "adafruit_io_config.h or adafruit_io_config.local.h");

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
void loop() {
  handleSerialCommands();
  vTaskDelay(pdMS_TO_TICKS(1000));
}
