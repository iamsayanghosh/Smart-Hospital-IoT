#pragma once

#include "state.h"

/**
 * @file bed.h
 * @brief Intelligent Remote Bed Elevation task declaration (Task 4).
 *
 * This module declares bedTask, which controls an automated hospital bed servo
 * using clinical-condition-first automatic rules with doctor manual override.
 */

/**
 * @brief Discrete operational modes for hospital bed elevation control.
 */
enum BedMode {
    BED_MODE_SLEEPING,          ///< Rest angle (10°) during stable condition
    BED_MODE_BREATHING_SUPPORT, ///< Elevated angle (45°) for respiratory support (SpO2 88-91%)
    BED_MODE_EMERGENCY,         ///< Upright angle (90°) during acute physiological crises
};

/**
 * @brief Evaluates appropriate bed elevation mode from telemetry with fault-latch protection.
 *
 * If an emergency was triggered by an acute vital (e.g. extreme body temperature) and that
 * measurement subsequently becomes invalid/disconnected, the emergency posture is latched and
 * retained until valid non-critical data is restored.
 *
 * @param data Patient telemetry snapshot.
 * @param tempTriggeredEmergency In/out reference tracking latched emergency state across sensor disconnects.
 * @param thresholds Medical threshold configuration.
 * @return BedMode Evaluated bed mode (SLEEPING, BREATHING_SUPPORT, or EMERGENCY).
 */
inline BedMode evaluateBedMode(const Telemetry &data, bool &tempTriggeredEmergency, const MedicalThresholdConfig &thresholds = medicalThresholds)
{
    const bool vitalsCritical = (data.heartRate < thresholds.heartRate.criticalLow ||
                                 data.heartRate > thresholds.heartRate.criticalHigh ||
                                 data.spo2 < thresholds.spo2.criticalLow ||
                                 data.ecgRate < thresholds.ecgRate.criticalLow ||
                                 data.ecgRate > thresholds.ecgRate.criticalHigh);

    const bool tempCritical = data.temperatureValid &&
                              (data.temperature < thresholds.bodyTemperature.criticalLow ||
                               data.temperature > thresholds.bodyTemperature.criticalHigh);

    if (tempCritical) {
        tempTriggeredEmergency = true;
    } else if (data.temperatureValid) {
        // Confirmed valid and non-critical: clear the latch
        tempTriggeredEmergency = false;
    }

    if (vitalsCritical || tempCritical || (tempTriggeredEmergency && !data.temperatureValid)) {
        return BED_MODE_EMERGENCY;
    } else if (data.spo2 < thresholds.spo2.low) {
        return BED_MODE_BREATHING_SUPPORT;
    }
    return BED_MODE_SLEEPING;
}

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

/**
 * @brief Detaches bed servo PWM and drives control line to safe unpowered state.
 *
 * Called during startup failure rollback or shutdown to prevent floating PWM lines.
 */
void bedDetachServo();
