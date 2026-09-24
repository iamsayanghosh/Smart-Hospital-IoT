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

void setSafetyOutputs(bool red, bool yellow, bool green, bool buzzer)
{
    static bool buzzerOn = false;

    digitalWrite(RED_LED_PIN, red ? HIGH : LOW);
    digitalWrite(YELLOW_LED_PIN, yellow ? HIGH : LOW);
    digitalWrite(GREEN_LED_PIN, green ? HIGH : LOW);

    if (buzzer != buzzerOn) {
        buzzerOn = buzzer;
        if (buzzerOn) {
            tone(BUZZER_PIN, 1200);
        } else {
            noTone(BUZZER_PIN);
        }
    }
}
