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
#include <math.h>
#include "esp_task_wdt.h"

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

    bool cancelledCriticalHold = false;
    if (state.criticalConfirmationPending && targetDosage <= MEDICATION_CRITICAL_DOSAGE_THRESHOLD) {
        state.criticalConfirmationPending = false;
        state.criticalConfirmed = false;
        state.lastRateLimitUpdateMs = now;
        cancelledCriticalHold = true;
    }

    bool enteredCriticalHold = false;
    if (!state.criticalConfirmationPending && !state.criticalConfirmed && targetDosage > MEDICATION_CRITICAL_DOSAGE_THRESHOLD) {
        state.criticalConfirmationPending = true;
        state.applied = min(state.applied, MEDICATION_CRITICAL_DOSAGE_THRESHOLD);
        state.lastRateLimitUpdateMs = now;
        enteredCriticalHold = true;
    }

    const float goal = state.criticalConfirmationPending ? MEDICATION_CRITICAL_DOSAGE_THRESHOLD : targetDosage;
    state.applied = calculateSlewStep(state.applied, goal, state.lastRateLimitUpdateMs, now);

    state.applied = constrain(state.applied, MEDICATION_DOSAGE_MIN, MEDICATION_DOSAGE_MAX);

    portENTER_CRITICAL(&telemetryMux);
    medicationDosageControl.applied = state.applied;
    medicationDosageControl.criticalConfirmationPending = state.criticalConfirmationPending;
    medicationDosageControl.criticalConfirmed = state.criticalConfirmed;
    medicationDosageControl.lastRateLimitUpdateMs = state.lastRateLimitUpdateMs;
    portEXIT_CRITICAL(&telemetryMux);

    if (enteredCriticalHold) {
        Serial.print("[MEDICATION] CRITICAL hold: requested=");
        Serial.print(targetDosage, 1);
        Serial.print(" mg/hr (> ");
        Serial.print(MEDICATION_CRITICAL_DOSAGE_THRESHOLD, 1);
        Serial.print("); holding applied=");
        Serial.print(state.applied, 1);
        Serial.println(" mg/hr awaiting GPIO 13 button confirmation");
    } else if (cancelledCriticalHold) {
        Serial.print("[MEDICATION] Critical hold cancelled: requested=");
        Serial.print(targetDosage, 1);
        Serial.println(" mg/hr <= 80.0 mg/hr");
    }
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
        bool confirmedCritical = false;
        float requestedTarget = 0.0f;

        portENTER_CRITICAL(&telemetryMux);
        medicationDosageControl.buttonPressed = rawPressed;
        medicationDosageControl.lastButtonTransitionMs = now;
        if (rawPressed && medicationDosageControl.criticalConfirmationPending) {
            medicationDosageControl.criticalConfirmationPending = false;
            medicationDosageControl.criticalConfirmed = true;
            medicationDosageControl.lastRateLimitUpdateMs = now;
            confirmedCritical = true;
            requestedTarget = medicationDosageControl.requested;
        }
        portEXIT_CRITICAL(&telemetryMux);

        if (confirmedCritical) {
            Serial.print("[MEDICATION] GPIO 13 button pressed: CRITICAL dose confirmed -> ramping to ");
            Serial.print(clampMedicationDosage(requestedTarget), 1);
            Serial.println(" mg/hr");
        } else if (rawPressed) {
            Serial.println("[MEDICATION] GPIO 13 button pressed (no critical hold pending)");
        }
    }
}

enum MedicationOutputMode {
    MED_OUT_NORMAL,
    MED_OUT_WARNING,
    MED_OUT_CRITICAL_CONFIRMED,
    MED_OUT_CRITICAL_PENDING
};

struct MedicationReportTracker {
    bool initialized = false;
    float lastReportedApplied = 0.0f;
    MedicationOutputMode lastOutputMode = MED_OUT_NORMAL;
};

/**
 * @brief Maps physical medication output tier to a concise Serial Monitor label.
 */
static const char *medicationOutputModeLabel(MedicationOutputMode mode)
{
    switch (mode) {
        case MED_OUT_CRITICAL_PENDING:
            return "CRITICAL_HOLD (LED=RED_BLINK, BUZZER=ACTIVE)";
        case MED_OUT_CRITICAL_CONFIRMED:
            return "CRITICAL_CONFIRMED (LED=RED, BUZZER=OFF)";
        case MED_OUT_WARNING:
            return "WARNING (LED=YELLOW, BUZZER=OFF)";
        default:
            return "NORMAL (LED=GREEN, BUZZER=OFF)";
    }
}

/**
 * @brief Logs medication infusion rate changes (>=1.0 mg/hr or target arrival) and safety tier changes.
 */
static void reportMedicationChanges(const MedicationDosageControl &state, MedicationOutputMode outputMode, MedicationReportTracker &tracker)
{
    const float targetDosage = clampMedicationDosage(state.requested);
    const float effectiveGoal = state.criticalConfirmationPending
                                    ? min(targetDosage, MEDICATION_CRITICAL_DOSAGE_THRESHOLD)
                                    : targetDosage;
    const float deltaFromReported = fabsf(state.applied - tracker.lastReportedApplied);
    const bool reachedGoal = (fabsf(state.applied - effectiveGoal) < 0.001f);
    const bool modeChanged = (outputMode != tracker.lastOutputMode);

    if (!tracker.initialized || modeChanged || deltaFromReported >= 0.999f || (reachedGoal && deltaFromReported > 0.001f)) {
        tracker.initialized = true;
        tracker.lastReportedApplied = state.applied;
        tracker.lastOutputMode = outputMode;
        Serial.print("[MEDICATION] Dosage: applied=");
        Serial.print(state.applied, 1);
        Serial.print(" mg/hr (target=");
        Serial.print(targetDosage, 1);
        Serial.print(" mg/hr, ");
        Serial.print(medicationOutputModeLabel(outputMode));
        Serial.println(")");
    }
}

/**
 * @brief Drives physical LEDs and buzzer according to current medication dosage safety state.
 *
 * Visual & Acoustic Signaling Rules:
 * - Critical Pending (>80 mg/hr unconfirmed): Red LED and Buzzer flash synchronously at rapid ~3.3 Hz (150 ms period).
 * - Critical Confirmed (>80 mg/hr confirmed): Solid Red LED ON, Buzzer OFF.
 * - Warning (80.0 mg/hr): Solid Yellow LED ON, Buzzer OFF.
 * - Normal (0.0 - 79.9 mg/hr): Solid Green LED ON, Buzzer OFF.
 *
 * NOTE: These outputs are dedicated solely to medication infusion safety and never indicate patient vitals.
 */
static void updateMedicationSafetyOutputs(MedicationReportTracker &tracker)
{
    MedicationDosageControl state = getMedicationDosageSnapshot();
    const uint32_t now = millis();

    const bool criticalPending = state.criticalConfirmationPending && state.requested > MEDICATION_CRITICAL_DOSAGE_THRESHOLD;
    
    bool redLedOn = false;
    bool yellowLedOn = false;
    bool greenLedOn = false;
    bool desiredBuzzerOn = false;
    MedicationOutputMode outputMode = MED_OUT_NORMAL;

    if (criticalPending) {
        const bool redBlinkOn = ((now / 150U) % 2U) == 0U; // Rapid ~3.3 Hz blink per Task 3 spec
        redLedOn = redBlinkOn;
        desiredBuzzerOn = redBlinkOn;
        outputMode = MED_OUT_CRITICAL_PENDING;
    } else if (state.criticalConfirmed || getMedicationSafetyLevel(state.applied) == MEDICATION_SAFETY_CRITICAL) {
        redLedOn = true;
        outputMode = MED_OUT_CRITICAL_CONFIRMED;
    } else if (getMedicationSafetyLevel(state.applied) == MEDICATION_SAFETY_WARNING) {
        yellowLedOn = true;
        outputMode = MED_OUT_WARNING;
    } else {
        greenLedOn = true;
        outputMode = MED_OUT_NORMAL;
    }

    setSafetyOutputs(redLedOn, yellowLedOn, greenLedOn, desiredBuzzerOn);
    reportMedicationChanges(state, outputMode, tracker);
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
    if (!registerTaskWithWatchdog(TASK_MEDICATION, "MEDICATION")) {
        return;
    }

    MedicationReportTracker reportTracker;
    while (true) {
        handleMedicationConfirmationButton();
        updateMedicationDosageFromRequest();
        updateMedicationSafetyOutputs(reportTracker);
        reportTaskHeartbeat(TASK_MEDICATION);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
