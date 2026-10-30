/**
 * @file outputs.cpp
 * @brief Physical indicator LEDs and acoustic buzzer actuator driver implementation.
 *
 * Responsibilities:
 * - Controls GPIO 25 (Red), 26 (Yellow), and 33 (Green) indicator LEDs.
 * - Drives GPIO 14 acoustic buzzer with 1200 Hz tone.
 * - Enforces Strict Priority Arbitration on Buzzer:
 *     Priority 1 (Highest): Medication critical alarm (continuous or 1 Hz pulse for unconfirmed request).
 *     Priority 2 (Lower): Offline connectivity failure cadences (Wi-Fi fail or MQTT fail beeps).
 *
 * Separation Note:
 * - All physical LEDs in this system are controlled strictly by Medication Dosage Safety (Task 3).
 * - Patient medical vital anomalies (Task 1 & Task 2) do NOT activate these LEDs or buzzer.
 */

#include "outputs.h"

#define RED_LED_PIN 25     ///< Red indicator LED pin (Critical dosage >80 mg/hr)
#define YELLOW_LED_PIN 26  ///< Yellow indicator LED pin (Warning dosage 80 mg/hr)
#define GREEN_LED_PIN 33   ///< Green indicator LED pin (Normal safe dosage 0-79 mg/hr)
#define BUZZER_PIN 14      ///< Piezo buzzer pin (driven via Arduino tone at 1200 Hz)

/**
 * @brief Initializes GPIO output modes and sets all actuators to silent/inactive state.
 */
void outputsInit()
{
    pinMode(RED_LED_PIN, OUTPUT);
    pinMode(YELLOW_LED_PIN, OUTPUT);
    pinMode(GREEN_LED_PIN, OUTPUT);
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(RED_LED_PIN, LOW);
    digitalWrite(YELLOW_LED_PIN, LOW);
    digitalWrite(GREEN_LED_PIN, LOW);
    noTone(BUZZER_PIN);
}

static bool medicationBuzzerActive = false; ///< Holds active acoustic alarm request from medicationTask
static bool actualBuzzerState = false;      ///< Current physical tone state of the buzzer transducer

/**
 * @brief Applies visual LED indicators and registers acoustic alarm intent for medication safety.
 *
 * @param red Turn Red LED ON/OFF.
 * @param yellow Turn Yellow LED ON/OFF.
 * @param green Turn Green LED ON/OFF.
 * @param buzzer True if medication safety requests buzzer sounding.
 */
void setSafetyOutputs(bool red, bool yellow, bool green, bool buzzer)
{
    digitalWrite(RED_LED_PIN, red ? HIGH : LOW);
    digitalWrite(YELLOW_LED_PIN, yellow ? HIGH : LOW);
    digitalWrite(GREEN_LED_PIN, green ? HIGH : LOW);

    medicationBuzzerActive = buzzer;
}

/**
 * @brief Drives buzzer beeping cadences for network failure unless overridden by medication alarms.
 *
 * Cadence Specifications (1000 ms period):
 * - BUZZER_MODE_WIFI_FAIL: Double pip pattern (0-100 ms ON, 200-300 ms ON, remainder OFF).
 * - BUZZER_MODE_MQTT_FAIL: Slow single beep (0-500 ms ON, 500-1000 ms OFF).
 * - Priority: If medicationBuzzerActive is asserted, offline cadences are superseded.
 *
 * @param mode Offline cadence enum (BUZZER_MODE_NONE, BUZZER_MODE_WIFI_FAIL, or BUZZER_MODE_MQTT_FAIL).
 */
void updateOfflineBuzzer(OfflineBuzzerMode mode) {
    bool shouldBuzz = medicationBuzzerActive;
    
    if (!shouldBuzz) {
        if (mode == BUZZER_MODE_WIFI_FAIL) {
            uint32_t ms = millis() % 1000;
            shouldBuzz = (ms < 100) || (ms > 200 && ms < 300);
        } else if (mode == BUZZER_MODE_MQTT_FAIL) {
            uint32_t ms = millis() % 1000;
            shouldBuzz = (ms < 500);
        }
    }

    if (shouldBuzz != actualBuzzerState) {
        actualBuzzerState = shouldBuzz;
        if (actualBuzzerState) {
            tone(BUZZER_PIN, 1200);
        } else {
            noTone(BUZZER_PIN);
        }
    }
}
