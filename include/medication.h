#ifndef MEDICATION_H
#define MEDICATION_H

#include <Arduino.h>
#include "state.h"

/**
 * @file medication.h
 * @brief Smart Medication Dosage safety controller and physical interlock declarations (Task 3).
 *
 * This module manages the simulated medication dosage safety subsystem, enforcing dosage
 * limits, ramping rate limits, and a physical pushbutton confirmation interlock for critical dosages.
 *
 * CRITICAL ARCHITECTURAL SEPARATION:
 * - Patient Medical Vitals (HR, SpO2, Temp, ECG) and Medication Dosage Safety are
 *   COMPLETELY INDEPENDENT subsystems.
 * - Abnormal patient vitals do NOT alter medication dosage or trigger medication alarms.
 * - Medication dosage status does NOT alter patient telemetry, sampling rate, or bed angle.
 * - The physical LEDs (Red/Yellow/Green) reflect infusion dosage risk rather than patient medical vitals.
 * - The acoustic buzzer is shared with Task 6 (Offline network cadences) via priority arbitration;
 *   medication critical alarms take absolute priority over offline signals.
 */

/**
 * @brief Configures GPIO 13 with internal pull-up resistor for the physical confirmation button.
 */
void medicationInit();

/**
 * @brief FreeRTOS task running the medication safety state machine and hardware interlock.
 *
 * Runs at Priority 3 with a 20 ms cycle (50 Hz), executing:
 * 1. Debounced button sampling on GPIO 13 for critical confirmation.
 * 2. Rate-limited dosage ramping (max +5.0 mg/hr per second).
 * 3. Physical safety output updates (LEDs on GPIO 25, 26, 33 and Buzzer on GPIO 14).
 *
 * @param parameter Unused FreeRTOS task parameter pointer (NULL).
 */
void medicationTask(void *parameter);

/**
 * @brief Constrains a requested dosage value to the valid infusion scale [0.0 - 100.0 mg/hr].
 *
 * Replaces NaN or non-finite values with 0.0 mg/hr.
 *
 * @param value Raw dosage input in mg/hr.
 * @return float Clamped dosage in mg/hr within [0.0, 100.0].
 */
float clampMedicationDosage(float value);

/**
 * @brief Classifies an applied dosage into its clinical risk tier.
 *
 * Thresholds:
 * - Normal:   0.0 to 79.9 mg/hr
 * - Warning:  80.0 mg/hr (exact boundary)
 * - Critical: > 80.0 mg/hr
 *
 * @param dosage Infusion rate in mg/hr.
 * @return MedicationSafetyLevel Enum representing NORMAL, WARNING, or CRITICAL.
 */
MedicationSafetyLevel getMedicationSafetyLevel(float dosage);

/**
 * @brief Returns the human-readable text label for a dosage safety tier.
 *
 * @param dosage Infusion rate in mg/hr.
 * @return const char* String literal: "NORMAL", "WARNING", or "CRITICAL".
 */
const char *getMedicationSafetyLabel(float dosage);

#endif
