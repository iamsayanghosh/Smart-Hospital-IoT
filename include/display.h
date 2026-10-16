#pragma once
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

/**
 * @file display.h
 * @brief SSD1306 OLED local display and centralized sensorQueue consumer declarations.
 *
 * This module manages the 128x64 I2C OLED display (SSD1306) and hosts the primary
 * consumer loop for sensorQueue messages emitted by physical and simulated sensor tasks.
 *
 * Subsystem Separation & Display Ownership:
 * - Patient Medical Telemetry is NOT displayed on the OLED screen.
 * - The OLED display is dedicated to:
 *     1. Task 3 Medication Dosage Safety: current applied dosage, safety status label,
 *        "CRITICAL REQUEST" interlock prompts, and "PRESS BUTTON" confirmation cues.
 *     2. Task 6 Offline Logging State: displays "LOGGING OFFLINE" banner when connectivity
 *        is lost and telemetry is diverted to the circular RAM buffer.
 */

/**
 * @brief Initializes the I2C bus and SSD1306 OLED hardware at address 0x3C.
 *
 * @param sda GPIO pin assigned for I2C SDA (default GPIO 21).
 * @param scl GPIO pin assigned for I2C SCL (default GPIO 22).
 */
void displayInit(uint8_t sda, uint8_t scl);

/**
 * @brief FreeRTOS task handling sensorQueue message processing and OLED rendering.
 *
 * Operational Responsibilities:
 * 1. Drains incoming SensorMessage packets from sensorQueue and atomically updates
 *    the global telemetry structure under telemetryMux.
 * 2. Evaluates isAbnormal() transitions and signals alertSemaphore when the patient's
 *    clinical condition becomes newly abnormal.
 * 3. Acquires displayMutex and renders medication safety status or offline banners
 *    onto the SSD1306 OLED.
 * 4. Feeds its watchdog heartbeat and runs at Priority 1 with non-blocking delays.
 *
 * @param parameter Unused FreeRTOS task parameter pointer (NULL).
 */
void displayTask(void *parameter);
