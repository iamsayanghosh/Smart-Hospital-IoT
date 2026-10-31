/**
 * @file bed.cpp
 * @brief Implementation of Intelligent Remote Bed Elevation (Task 4).
 *
 * Responsibilities:
 * - Controls hospital bed elevation angle via a servo actuator on GPIO 18.
 * - Enforces automatic-first clinical safety priorities:
 *     1. Emergency Mode (90°, 5.0°/s): Automatically commanded when any vital reaches critical bounds.
 *     2. Breathing Support Mode (45°, 1.5°/s): Automatically commanded when SpO2 drops into hypoxia (88-91%).
 *     3. Sleeping Mode (10°, 1.5°/s): Default rest position for stable vitals or non-critical anomalies.
 * - Manual Doctor Override:
 *     Accepts manual angle commands [0°-90°] via bedQueue from MQTT feed "bed-angle".
 *     Manual override is strictly subordinate to clinical transitions: if the patient's
 *     clinical condition changes, manual mode is immediately cancelled and the automatic
 *     clinical position is restored.
 * - Actuator Safety & Kinematics:
 *     Runs a 50 Hz control loop (20 ms timestep) to smoothly ramp servo position, preventing
 *     abrupt physical movement that could distress a patient.
 */

#include "bed.h"
#include "state.h"
#include "watchdog.h"
#include <ESP32Servo.h>
#include "esp_task_wdt.h"
#include <cmath>

static const int BED_SERVO_PIN = 18;          ///< GPIO pin connected to bed elevation servo signal line
static const int BED_SERVO_MIN_PULSE = 500;   ///< Minimum pulse width in microseconds (corresponds to 0°)
static const int BED_SERVO_MAX_PULSE = 2400;  ///< Maximum pulse width in microseconds (corresponds to 180°)
static const float BED_SERVO_MAX_ANGLE = 180.0f; ///< Physical servo full travel scale in degrees

/**
 * @brief Discrete operational modes for hospital bed elevation control.
 */
enum BedMode {
    BED_MODE_SLEEPING,          ///< Rest angle (10°) during stable condition
    BED_MODE_BREATHING_SUPPORT, ///< Elevated angle (45°) for respiratory support (SpO2 88-91%)
    BED_MODE_EMERGENCY,         ///< Upright angle (90°) during acute physiological crises
    BED_MODE_MANUAL             ///< Clinician-defined angle commanded via dashboard
};

/**
 * @brief FreeRTOS task implementing bed elevation kinematic control and clinical arbitration.
 *
 * @param parameter Unused FreeRTOS task parameter pointer (NULL).
 */
void bedTask(void *parameter)
{
    (void)parameter;
    esp_err_t wdtResult = esp_task_wdt_add(NULL);
    if (wdtResult != ESP_OK) {
        Serial.print("[WATCHDOG] BED registration failed: ");
        Serial.println(esp_err_to_name(wdtResult));
        return;
    }
    Serial.println("[WATCHDOG] BED task registered with TWDT");

    Servo bedServo;
    bedServo.setPeriodHertz(50);
    bedServo.attach(BED_SERVO_PIN, BED_SERVO_MIN_PULSE, BED_SERVO_MAX_PULSE);

    // Startup requirement: Servo starts at exactly 0°.
    float currentAngle = 0.0f;
    bedServo.writeMicroseconds(BED_SERVO_MIN_PULSE + (int)((currentAngle / BED_SERVO_MAX_ANGLE) * (BED_SERVO_MAX_PULSE - BED_SERVO_MIN_PULSE)));

    BedMode lastAutoMode = BED_MODE_SLEEPING;
    bool isManualOverride = false;
    float targetAngle = 0.0f;
    float speedPerSecond = 1.5f;

    // Force an initial update to evaluate telemetry and move from 0°
    bool initialEval = true;

    while (true) {
        Telemetry data = getTelemetrySnapshot();
        if (initialEval && (!data.initialVitalsProcessed || !data.initialEcgProcessed)) {
            reportTaskHeartbeat(TASK_BED);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        // 1. Check for manual override from MQTT
        float manualTarget = -1.0f;
        if (xQueueReceive(bedQueue, &manualTarget, 0) == pdTRUE) {
            targetAngle = manualTarget;
            if (targetAngle < 0.0f) targetAngle = 0.0f;
            if (targetAngle > 90.0f) targetAngle = 90.0f;
            
            isManualOverride = true;
            speedPerSecond = 2.0f; // Manual: 2°/second
            Serial.print("[BED] Manual mode activated, target angle: ");
            Serial.println(targetAngle);
        }

        // 2. Automatic condition logic
        BedMode currentAutoMode = BED_MODE_SLEEPING;

        // CRITICAL CONDITION -> EMERGENCY (90°)
        // Critical conditions: Severe bradycardia (<45) or severe tachycardia (>120), severe hypoxia (<88%), or extreme temp (<34 or >39.5)
        if (data.heartRate < HEART_RATE_CRITICAL_LOW || data.heartRate > HEART_RATE_CRITICAL_HIGH ||
            data.spo2 < 88 ||
            data.ecgRate < ECG_RATE_CRITICAL_LOW || data.ecgRate > ECG_RATE_CRITICAL_HIGH ||
            (data.temperatureValid && (data.temperature < 34.0f || data.temperature > 39.5f))) {
            currentAutoMode = BED_MODE_EMERGENCY;
        } 
        // BREATHING ISSUE -> BREATHING_SUPPORT (45°)
        else if (data.spo2 < medicalThresholds.spo2.low) { // default 92, meaning 88 to 91 inclusive
            currentAutoMode = BED_MODE_BREATHING_SUPPORT;
        }
        // OTHER NON-CRITICAL ABNORMALITIES or PATIENT OK / STABLE -> SLEEPING (10°)
        // The assignment does not define a separate bed response for every possible abnormal vital.
        // Therefore, we use SLEEPING as the safest existing/default bed behavior for any other abnormalities (e.g. Temperature = 38.5).
        else {
            currentAutoMode = BED_MODE_SLEEPING;
        }

        // Apply if the auto mode changed OR if it's the very first evaluation.
        // This implicitly breaks out of manual override if the patient's condition changes.
        if (currentAutoMode != lastAutoMode || initialEval) {
            lastAutoMode = currentAutoMode;
            initialEval = false;
            isManualOverride = false;

            if (currentAutoMode == BED_MODE_SLEEPING) {
                targetAngle = 10.0f;
                speedPerSecond = 1.5f;
            } else if (currentAutoMode == BED_MODE_BREATHING_SUPPORT) {
                targetAngle = 45.0f;
                speedPerSecond = 1.5f;
            } else if (currentAutoMode == BED_MODE_EMERGENCY) {
                targetAngle = 90.0f;
                speedPerSecond = 5.0f;
            }

            Serial.print("[BED] Automatic state change, mode: ");
            Serial.print(currentAutoMode);
            Serial.print(", target angle: ");
            Serial.println(targetAngle);
        }

        // 3. Smooth movement
        // Loop runs at 50Hz, so dt is 0.02s
        float stepSize = speedPerSecond * 0.02f;
        
        if (fabsf(currentAngle - targetAngle) > stepSize) {
            if (currentAngle < targetAngle) {
                currentAngle += stepSize;
            } else {
                currentAngle -= stepSize;
            }
            int pulse = BED_SERVO_MIN_PULSE + (int)((currentAngle / BED_SERVO_MAX_ANGLE) * (BED_SERVO_MAX_PULSE - BED_SERVO_MIN_PULSE));
            bedServo.writeMicroseconds(pulse);
        } else if (currentAngle != targetAngle) {
            currentAngle = targetAngle;
            int pulse = BED_SERVO_MIN_PULSE + (int)((currentAngle / BED_SERVO_MAX_ANGLE) * (BED_SERVO_MAX_PULSE - BED_SERVO_MIN_PULSE));
            bedServo.writeMicroseconds(pulse);
        }

        reportTaskHeartbeat(TASK_BED);
        vTaskDelay(pdMS_TO_TICKS(20)); // 50 Hz update
    }
}
