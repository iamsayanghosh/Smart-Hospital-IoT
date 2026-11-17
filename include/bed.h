#pragma once

/**
 * @file bed.h
 * @brief Intelligent Remote Bed Elevation task declaration (Task 4).
 *
 * This module declares bedTask, which controls an automated hospital bed servo
 * using clinical-condition-first automatic rules with doctor manual override.
 */

/**
 * @brief FreeRTOS task controlling the bed servo actuator based on patient vitals and remote commands.
 *
 * Operational Details:
 * - Hardware: Drives servo on GPIO 18 (50 Hz PWM, 500-2400 us pulse width).
 * - Startup: Starts at 0° until initial vitals and ECG are processed.
 * - Automatic Clinical Modes:
 *     - Emergency (90°, 5.0°/s): Critical vitals (HR <45 or >120, SpO2 <88%, ECG <45 or >120, Temp <34°C or >39.5°C).
 *     - Breathing Support (45°, 1.5°/s): Hypoxia support (SpO2 88-91%).
 *     - Sleeping (10°, 1.5°/s): Normal stable vitals or non-critical anomalies.
 * - Clinician-in-the-Loop Override Primacy: Ingests manual angle targets [0°-90°] via bedQueue
 *   from MQTT feed "bed-angle". The attending physician retains absolute human-in-the-loop authority
 *   to command any bed angle at any time (e.g. flattening the bed to 0° supine for emergency CPR
 *   chest compressions or surgical access during a cardiac arrest). Any subsequent transition in
 *   the patient's clinical state automatically cancels manual override and re-evaluates posture.
 * - Kinematic Smoothing: Updates at 50 Hz (20 ms dt) with velocity-clamped increments (max 0.1° per 20 ms step = 5.0°/s maximum emergency slew rate, 1.5°/s nominal) to guarantee smooth, non-abrupt motion for patient comfort.
 *
 * @param parameter Unused FreeRTOS task parameter pointer (NULL).
 */
void bedTask(void *parameter);
