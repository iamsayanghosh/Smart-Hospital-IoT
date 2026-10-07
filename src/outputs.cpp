#include "outputs.h"

#define RED_LED_PIN 25
#define YELLOW_LED_PIN 26
#define GREEN_LED_PIN 33
#define BUZZER_PIN 14

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

static bool medicationBuzzerActive = false;
static bool actualBuzzerState = false;

void setSafetyOutputs(bool red, bool yellow, bool green, bool buzzer)
{
    digitalWrite(RED_LED_PIN, red ? HIGH : LOW);
    digitalWrite(YELLOW_LED_PIN, yellow ? HIGH : LOW);
    digitalWrite(GREEN_LED_PIN, green ? HIGH : LOW);

    medicationBuzzerActive = buzzer;
}

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
