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
 *     - Emergency (90°, 5.0°/s): Critical vitals (HR <50 or >100, SpO2 <88%, ECG <50 or >110, Temp <34°C or >39.5°C).
 *     - Breathing Support (45°, 1.5°/s): Hypoxia support (SpO2 88-91%).
 *     - Sleeping (10°, 1.5°/s): Normal stable vitals or non-critical anomalies.
 * - Manual Override: Ingests manual angle targets [0°-90°] via bedQueue from MQTT feed "bed-angle".
 *   Manual override is preserved ONLY until the patient's clinical state changes. Any change in
 *   auto mode immediately cancels manual override and restores clinical automatic angle.
 * - Kinematic Smoothing: Updates at 50 Hz (20 ms dt) with velocity-clamped increments (max 1.0° per 20 ms step = 50.0°/s maximum slew rate) to guarantee smooth, non-abrupt motion for patient comfort.
 *
 * @param parameter Unused FreeRTOS task parameter pointer (NULL).
 */
void bedTask(void *parameter);
