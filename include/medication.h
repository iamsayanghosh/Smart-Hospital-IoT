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

constexpr float MEDICATION_DOSAGE_MIN = 0.0f;                    ///< Minimum valid dosage (0 mg/hr = infusion stopped)
constexpr float MEDICATION_DOSAGE_MAX = 100.0f;                  ///< Maximum hardware delivery limit (100 mg/hr)
constexpr float MEDICATION_CRITICAL_DOSAGE_THRESHOLD = 80.0f;    ///< Boundary above which physical confirmation is mandatory
constexpr float MEDICATION_RATE_LIMIT_STEP_MG_PER_SEC = 5.0f;   ///< Maximum allowable dosage slew rate (+5.0 mg/hr per second)

/**
 * @brief Constrains a requested dosage value to the valid infusion scale [0.0 - 100.0 mg/hr].
 *
 * Replaces NaN or non-finite values with 0.0 mg/hr.
 *
 * @param value Raw dosage input in mg/hr.
 * @return float Clamped dosage in mg/hr within [0.0, 100.0].
 */
inline float clampMedicationDosage(float value)
{
    if (!isfinite(value) || value < MEDICATION_DOSAGE_MIN) {
        return MEDICATION_DOSAGE_MIN;
    }
    if (value > MEDICATION_DOSAGE_MAX) {
        return MEDICATION_DOSAGE_MAX;
    }
    return value;
}

/**
 * @brief Computes rate-limited dosage progression toward a goal.
 *
 * Implements asymmetric rate limiting:
 * - Downward adjustments are applied immediately.
 * - Upward adjustments are constrained to MEDICATION_RATE_LIMIT_STEP_MG_PER_SEC (+5.0 mg/hr per sec).
 * - Initial call or zero timestamp establishes the baseline timestamp with zero initial step.
 * - Interruptions >= 60,000 ms re-anchor the timestamp without catch-up increase to prevent runaway dosing.
 * - Unsigned 32-bit math safely handles millis() rollover at ~49.7 days.
 *
 * @param currentApplied Current delivered dosage in mg/hr.
 * @param goal Target dosage in mg/hr.
 * @param now Current timestamp in milliseconds.
 * @param lastRateLimitUpdateMs Reference to baseline timestamp (updated on progression/re-anchor).
 * @return float New applied dosage in mg/hr.
 */
inline float stepMedicationSlewRate(float currentApplied, float goal, uint32_t now, uint32_t &lastRateLimitUpdateMs)
{
    if (goal > currentApplied) {
        float elapsedSeconds = 0.0f;
        if (lastRateLimitUpdateMs != 0U) {
            uint32_t deltaMs = now - lastRateLimitUpdateMs;
            if (deltaMs > 0U && deltaMs < 60000U) {
                elapsedSeconds = static_cast<float>(deltaMs) / 1000.0f;
            } else if (deltaMs >= 60000U) {
                // Task was delayed or suspended >= 60s: refresh baseline timestamp without catch-up spike
                lastRateLimitUpdateMs = now;
            }
        } else {
            // First upward adjustment initializes baseline timestamp without an instantaneous step
            lastRateLimitUpdateMs = now;
        }

        if (elapsedSeconds > 0.0f) {
            const float maxIncrease = MEDICATION_RATE_LIMIT_STEP_MG_PER_SEC * elapsedSeconds;
            const float allowedIncrease = min(maxIncrease, goal - currentApplied);
            currentApplied += allowedIncrease;
            lastRateLimitUpdateMs = now;
        }
    } else {
        currentApplied = goal;
        lastRateLimitUpdateMs = now;
    }

    return constrain(currentApplied, MEDICATION_DOSAGE_MIN, MEDICATION_DOSAGE_MAX);
}

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
inline MedicationSafetyLevel getMedicationSafetyLevel(float dosage)
{
    if (dosage > MEDICATION_CRITICAL_DOSAGE_THRESHOLD) {
        return MEDICATION_SAFETY_CRITICAL;
    }
    if (fabsf(dosage - MEDICATION_CRITICAL_DOSAGE_THRESHOLD) < 0.001f) {
        return MEDICATION_SAFETY_WARNING;
    }
    return MEDICATION_SAFETY_NORMAL;
}

/**
 * @brief Returns the human-readable text label for a dosage safety tier.
 *
 * @param dosage Infusion rate in mg/hr.
 * @return const char* String literal: "NORMAL", "WARNING", or "CRITICAL".
 */
inline const char *getMedicationSafetyLabel(float dosage)
{
    switch (getMedicationSafetyLevel(dosage)) {
        case MEDICATION_SAFETY_WARNING:
            return "WARNING";
        case MEDICATION_SAFETY_CRITICAL:
            return "CRITICAL";
        default:
            return "NORMAL";
    }
}

#endif
