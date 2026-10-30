#ifndef OUTPUTS_H
#define OUTPUTS_H

#include <Arduino.h>
#include "offline.h"

/**
 * @file outputs.h
 * @brief Physical indicator LEDs and acoustic buzzer actuator driver declarations.
 *
 * Hardware Assignments:
 * - Red LED:    GPIO 25 (Medication Dosage Safety - Critical [>80 mg/hr] Solid / Blinking)
 * - Yellow LED: GPIO 26 (Medication Dosage Safety - Warning [80 mg/hr] Solid)
 * - Green LED:  GPIO 33 (Medication Dosage Safety - Normal Infusion [0-79 mg/hr] Solid)
 * - Buzzer:     GPIO 14 (Shared acoustic transducer driven at 1200 Hz)
 *
 * Subsystem Boundaries & Priority Arbitration:
 * - The three LEDs are owned SOLELY by Task 3 (Medication Dosage Safety).
 * - Patient medical vitals (HR, SpO2, Temp, ECG) NEVER activate these LEDs or buzzer.
 * - The Buzzer is shared between Task 3 (Medication Emergency Alarm) and Task 6
 *   (Offline Connectivity Failure Beeps). Medication critical alarms have absolute
 *   priority and override any connectivity beeping patterns.
 */

/**
 * @brief Configures GPIO pins for the status LEDs and acoustic buzzer as outputs.
 *
 * Initializes Red, Yellow, Green LEDs to LOW (OFF) and silences the buzzer.
 */
void outputsInit();

/**
 * @brief Sets status LED states and registers medication alarm intent on the buzzer.
 *
 * Called at 50 Hz from medicationTask (src/medication.cpp).
 *
 * @param red State for Red LED on GPIO 25.
 * @param yellow State for Yellow LED on GPIO 26.
 * @param green State for Green LED on GPIO 33.
 * @param buzzer True if medication safety requires acoustic alarm tone.
 */
void setSafetyOutputs(bool red, bool yellow, bool green, bool buzzer);

/**
 * @brief Updates acoustic buzzer state, arbitrating between medication and offline patterns.
 *
 * Called from offlineTask (src/offline.cpp).
 * If medicationBuzzerActive is true, medication alarm takes absolute precedence.
 * Otherwise, generates cadence beeps for Wi-Fi failure (double pip) or MQTT failure (slow pip).
 *
 * @param mode Offline buzzer cadence mode (NONE, WIFI_FAIL, or MQTT_FAIL).
 */
void updateOfflineBuzzer(OfflineBuzzerMode mode);

#endif
