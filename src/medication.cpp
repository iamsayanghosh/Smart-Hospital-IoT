/**
 * @file medication.cpp
 * @brief Implementation of Smart Medication Dosage safety controller and physical interlock (Task 3).
 *
 * Responsibilities:
 * 1. Dosage Safety Validation & Clamping: Ensures dosage requests stay in [0.0 - 100.0 mg/hr].
 * 2. Asymmetric Rate Limiting: Infusion increases are capped at +5.0 mg/hr per second to prevent
 *    bolus shock, while dosage decreases apply immediately for rapid emergency mitigation.
 * 3. Bedside Pushbutton Interlock: Any requested dose > 80.0 mg/hr is held at 80.0 mg/hr until
 *    a nurse or physician physically presses the confirmation pushbutton (GPIO 13).
 * 4. Hardware Outputs Ownership: Manages the status LEDs (Red=25, Yellow=26, Green=33)
 *    and drives the acoustic alarm buzzer (GPIO 14) via priority arbitration (shared with Task 6).
 *
 * SUBSYSTEM INDEPENDENCE:
 * - This module operates completely independently of patient medical vitals (heart rate, SpO2, temp).
 * - Abnormal vitals do NOT change medication limits, and medication states do NOT affect vitals.
 */

#include "medication.h"
#include "outputs.h"
#include "watchdog.h"
#include "app.h"
#include <math.h>
#include "esp_task_wdt.h"

static constexpr float MEDICATION_DOSAGE_MIN = 0.0f;                    ///< Minimum valid dosage (0 mg/hr = infusion stopped)
static constexpr float MEDICATION_DOSAGE_MAX = 100.0f;                  ///< Maximum permissible dosage (100 mg/hr)
static constexpr float MEDICATION_CRITICAL_DOSAGE_THRESHOLD = 80.0f;    ///< Boundary above which physical confirmation is mandatory
static constexpr float MEDICATION_RATE_LIMIT_STEP_MG_PER_SEC = 5.0f;   ///< Maximum allowable dosage slew rate (+5.0 mg/hr per second)
static constexpr uint32_t MEDICATION_BUTTON_DEBOUNCE_MS = 50;           ///< Pushbutton contact debounce interval in milliseconds
#define PUSHBUTTON_PIN 13                                               ///< GPIO pin for active-low confirmation pushbutton

/**
 * @brief Initializes GPIO 13 with internal pullup for physical dosage confirmation.
 */
void medicationInit()
{
    pinMode(PUSHBUTTON_PIN, INPUT_PULLUP);
}

/**
 * @brief Validates and constrains medication dosage within safe physical boundaries.
 *
 * @param value Desired dosage in mg/hr.
 * @return float Clamped value between 0.0 and 100.0 mg/hr (non-finite values return 0.0).
 */
float clampMedicationDosage(float value)
{
    if (!isfinite(value)) {
        return MEDICATION_DOSAGE_MIN;
    }
    return constrain(value, MEDICATION_DOSAGE_MIN, MEDICATION_DOSAGE_MAX);
}

/**
 * @brief Categorizes dosage rate into discrete safety levels.
 *
 * @param dosage Current infusion rate in mg/hr.
 * @return MedicationSafetyLevel NORMAL (0-79), WARNING (80), or CRITICAL (>80).
 */
MedicationSafetyLevel getMedicationSafetyLevel(float dosage)
{
    if (dosage > MEDICATION_CRITICAL_DOSAGE_THRESHOLD) {
        return MEDICATION_SAFETY_CRITICAL;
    }
    if (fabsf(dosage - MEDICATION_CRITICAL_DOSAGE_THRESHOLD) < 0.1f) {
        return MEDICATION_SAFETY_WARNING;
    }
    return MEDICATION_SAFETY_NORMAL;
}

/**
 * @brief Converts safety level enum to human-readable string.
 *
 * @param dosage Current infusion rate in mg/hr.
 * @return const char* "NORMAL", "WARNING", or "CRITICAL".
 */
const char *getMedicationSafetyLabel(float dosage)
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


/**
 * @brief Advances dosage rate towards requested target subject to safety limits and confirmation.
 *
 * Algorithm Details:
 * 1. Reads current control state under spinlock critical section.
 * 2. Clamps requested target between 0.0 and 100.0 mg/hr.
 * 3. Enforces Critical Confirmation Interlock:
 *    - If target > 80.0 mg/hr and not yet confirmed, state.criticalConfirmationPending is set true
 *      and effective goal is capped at 80.0 mg/hr.
 *    - If target is reduced <= 80.0 mg/hr, pending status is automatically cleared.
 * 4. Asymmetric Rate Limiting:
 *    - Upward adjustments are rate-limited to +5.0 mg/hr per second based on elapsed time.
 *    - Downward adjustments occur immediately to allow instant cessation or reduction of infusion.
 * 5. Commits updated applied dosage and control flags back under spinlock critical section.
 */
static void updateMedicationDosageFromRequest()
{
    const uint32_t now = millis();
    MedicationDosageControl state;
    portENTER_CRITICAL(&telemetryMux);
    state = medicationDosageControl;
    portEXIT_CRITICAL(&telemetryMux);

    const float targetDosage = clampMedicationDosage(state.requested);

    if (targetDosage <= MEDICATION_CRITICAL_DOSAGE_THRESHOLD) {
        state.criticalConfirmed = false;
    }

    if (state.criticalConfirmationPending && targetDosage <= MEDICATION_CRITICAL_DOSAGE_THRESHOLD) {
        state.criticalConfirmationPending = false;
        state.criticalConfirmed = false;
        state.lastRateLimitUpdateMs = now;
    }

    if (!state.criticalConfirmationPending && !state.criticalConfirmed && targetDosage > MEDICATION_CRITICAL_DOSAGE_THRESHOLD) {
        state.criticalConfirmationPending = true;
        state.applied = min(state.applied, MEDICATION_CRITICAL_DOSAGE_THRESHOLD);
        state.lastRateLimitUpdateMs = now;
    }

    const float goal = state.criticalConfirmationPending ? MEDICATION_CRITICAL_DOSAGE_THRESHOLD : targetDosage;
    if (goal > state.applied) {
        float elapsedSeconds = 1.0f;
        if (state.lastRateLimitUpdateMs != 0U && now > state.lastRateLimitUpdateMs) {
            elapsedSeconds = static_cast<float>(now - state.lastRateLimitUpdateMs) / 1000.0f;
        }
        if (elapsedSeconds <= 0.0f) {
            elapsedSeconds = 1.0f;
        }

        const float maxIncrease = MEDICATION_RATE_LIMIT_STEP_MG_PER_SEC * elapsedSeconds;
        const float allowedIncrease = min(maxIncrease, goal - state.applied);
        state.applied += allowedIncrease;
        state.lastRateLimitUpdateMs = now;
    } else {
        state.applied = goal;
        state.lastRateLimitUpdateMs = now;
    }

    state.applied = constrain(state.applied, MEDICATION_DOSAGE_MIN, MEDICATION_DOSAGE_MAX);

    portENTER_CRITICAL(&telemetryMux);
    medicationDosageControl.applied = state.applied;
    medicationDosageControl.criticalConfirmationPending = state.criticalConfirmationPending;
    medicationDosageControl.criticalConfirmed = state.criticalConfirmed;
    medicationDosageControl.lastRateLimitUpdateMs = state.lastRateLimitUpdateMs;
    portEXIT_CRITICAL(&telemetryMux);
}

/**
 * @brief Samples and debounces the physical confirmation pushbutton on GPIO 13.
 *
 * Debouncing Logic:
 * - Requires contact stability for at least 50 ms (MEDICATION_BUTTON_DEBOUNCE_MS).
 * - Detects falling edge (button press down on active-low input).
 * - If a critical dosage confirmation is pending, confirms the request, clears
 *   criticalConfirmationPending, sets criticalConfirmed = true, and unlocks upward rate limiting.
 */
static void handleMedicationConfirmationButton()
{
    const uint32_t now = millis();
    const bool rawPressed = (digitalRead(PUSHBUTTON_PIN) == LOW);

    portENTER_CRITICAL(&telemetryMux);
    const bool previousPressed = medicationDosageControl.buttonPressed;
    const uint32_t lastTransitionMs = medicationDosageControl.lastButtonTransitionMs;
    portEXIT_CRITICAL(&telemetryMux);

    if (rawPressed != previousPressed && (now - lastTransitionMs) >= MEDICATION_BUTTON_DEBOUNCE_MS) {
        portENTER_CRITICAL(&telemetryMux);
        medicationDosageControl.buttonPressed = rawPressed;
        medicationDosageControl.lastButtonTransitionMs = now;
        if (rawPressed && medicationDosageControl.criticalConfirmationPending) {
            medicationDosageControl.criticalConfirmationPending = false;
            medicationDosageControl.criticalConfirmed = true;
            medicationDosageControl.lastRateLimitUpdateMs = now;
        }
        portEXIT_CRITICAL(&telemetryMux);
    }
}

/**
 * @brief Drives physical LEDs and buzzer according to current medication dosage safety state.
 *
 * Visual & Acoustic Signaling Rules:
 * - Critical Pending (>80 mg/hr unconfirmed): Red LED and Buzzer flash synchronously at 1 Hz (500 ms period).
 * - Critical Confirmed (>80 mg/hr confirmed): Solid Red LED ON, Buzzer OFF.
 * - Warning (80.0 mg/hr): Solid Yellow LED ON, Buzzer OFF.
 * - Normal (0.0 - 79.0 mg/hr): Solid Green LED ON, Buzzer OFF.
 *
 * NOTE: These outputs are dedicated solely to medication infusion safety and never indicate patient vitals.
 */
static void updateMedicationSafetyOutputs()
{
    MedicationDosageControl state = getMedicationDosageSnapshot();
    const uint32_t now = millis();

    const bool criticalPending = state.criticalConfirmationPending && state.requested > MEDICATION_CRITICAL_DOSAGE_THRESHOLD;
    
    bool redLedOn = false;
    bool yellowLedOn = false;
    bool greenLedOn = false;
    bool desiredBuzzerOn = false;

    if (criticalPending) {
        const bool redBlinkOn = ((now / 500U) % 2U) == 0U;
        redLedOn = redBlinkOn;
        desiredBuzzerOn = redBlinkOn;
    } else if (state.criticalConfirmed || getMedicationSafetyLevel(state.applied) == MEDICATION_SAFETY_CRITICAL) {
        redLedOn = true;
    } else if (getMedicationSafetyLevel(state.applied) == MEDICATION_SAFETY_WARNING) {
        yellowLedOn = true;
    } else {
        greenLedOn = true;
    }

    setSafetyOutputs(redLedOn, yellowLedOn, greenLedOn, desiredBuzzerOn);
}

/**
 * @brief FreeRTOS task executing the 50 Hz medication safety supervisory loop.
 *
 * Sequence of Execution (every 20 ms):
 * 1. handleMedicationConfirmationButton(): Samples GPIO 13 with 50 ms debounce.
 * 2. updateMedicationDosageFromRequest(): Applies target clamping, interlock, and +5 mg/hr/s slew.
 * 3. updateMedicationSafetyOutputs(): Sets LEDs and buzzer states based on dosage risk tier.
 * 4. reportTaskHeartbeat(TASK_MEDICATION): Feeds software watchdog heartbeat table.
 * 5. vTaskDelay(20 ms): Yields execution to maintain 50 Hz sampling cadence.
 *
 * @param parameter Unused FreeRTOS task parameter pointer (NULL).
 */
void medicationTask(void *parameter)
{
    (void)parameter;
    esp_err_t wdtResult = esp_task_wdt_add(NULL);
    if (wdtResult != ESP_OK) {
        Serial.print("[WATCHDOG] MEDICATION registration failed: ");
        Serial.println(esp_err_to_name(wdtResult));
        return;
    }
    Serial.println("[WATCHDOG] MEDICATION task registered with TWDT");

    while (true) {
        handleMedicationConfirmationButton();
        updateMedicationDosageFromRequest();
        updateMedicationSafetyOutputs();
        reportTaskHeartbeat(TASK_MEDICATION);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
