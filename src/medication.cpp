#include "medication.h"
#include "outputs.h"
#include "watchdog.h"
#include "app.h"
#include <math.h>
#include "esp_task_wdt.h"

static constexpr float MEDICATION_DOSAGE_MIN = 0.0f;
static constexpr float MEDICATION_DOSAGE_MAX = 100.0f;
static constexpr float MEDICATION_CRITICAL_DOSAGE_THRESHOLD = 80.0f;
static constexpr float MEDICATION_RATE_LIMIT_STEP_MG_PER_SEC = 5.0f;
static constexpr uint32_t MEDICATION_BUTTON_DEBOUNCE_MS = 50;
#define PUSHBUTTON_PIN 13

void medicationInit()
{
    pinMode(PUSHBUTTON_PIN, INPUT_PULLUP);
}

float clampMedicationDosage(float value)
{
    if (!isfinite(value)) {
        return MEDICATION_DOSAGE_MIN;
    }
    return constrain(value, MEDICATION_DOSAGE_MIN, MEDICATION_DOSAGE_MAX);
}

MedicationSafetyLevel getMedicationSafetyLevel(float dosage)
{
    if (dosage > MEDICATION_CRITICAL_DOSAGE_THRESHOLD) {
        return MEDICATION_SAFETY_CRITICAL;
    }
    if (dosage == MEDICATION_CRITICAL_DOSAGE_THRESHOLD) {
        return MEDICATION_SAFETY_WARNING;
    }
    return MEDICATION_SAFETY_NORMAL;
}

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
