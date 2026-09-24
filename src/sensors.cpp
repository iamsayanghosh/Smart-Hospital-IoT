#include "sensors.h"
#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "state.h"
#include "watchdog.h"
#include "esp_task_wdt.h"
#include "app.h"
#include "environment.h"

#define DS18B20_PIN 4
#define PIR_PIN 27

OneWire oneWire(DS18B20_PIN);
DallasTemperature temperatureSensor(&oneWire);

void sensorsInit() {
    pinMode(PIR_PIN, INPUT);
    temperatureSensor.begin();
}

/**
 * @brief Reads the physical DS18B20 temperature sensor.
 *
 * Enqueues each reading for the display task to apply to shared telemetry.
 * It subscribes to the TWDT, reports its heartbeat, and samples every 2 seconds.
 */
void temperatureTask(void *parameter)
{
    (void)parameter;
    esp_err_t wdtResult = esp_task_wdt_add(NULL);
    if (wdtResult != ESP_OK) {
        Serial.print("[WATCHDOG] TEMP registration failed: ");
        Serial.println(esp_err_to_name(wdtResult));
        return;
    }
    Serial.println("[WATCHDOG] TEMP task registered with TWDT");

    while (true) {
        temperatureSensor.requestTemperatures();
        SensorMessage message = {};
        message.type = TEMP_DATA;
        message.temperature = temperatureSensor.getTempCByIndex(0);
        sendSensorMessage(message, "TEMP TASK");

        Serial.print("[TEMP TASK] Temperature: ");
        Serial.print(message.temperature, 1);
        Serial.println(" C");
        reportTaskHeartbeat(TASK_TEMP);
        vTaskDelay(pdMS_TO_TICKS(getSamplePeriodMs(TASK_TEMP)));
    }
}

/**
 * @brief Reads the physical PIR motion input.
 *
 * Enqueues the digital motion state for the display task. The task feeds its
 * watchdog heartbeat after TWDT registration and samples every 1 second.
 */
void motionTask(void *parameter)
{
    (void)parameter;
    esp_err_t wdtResult = esp_task_wdt_add(NULL);
    if (wdtResult != ESP_OK) {
        Serial.print("[WATCHDOG] MOTION registration failed: ");
        Serial.println(esp_err_to_name(wdtResult));
        return;
    }
    Serial.println("[WATCHDOG] MOTION task registered with TWDT");

    while (true) {
        SensorMessage message = {};
        message.type = MOTION_DATA;
        message.motion = digitalRead(PIR_PIN);
        sendSensorMessage(message, "MOTION TASK");

        Serial.print("[MOTION TASK] Motion: ");
        Serial.println(message.motion ? "DETECTED" : "NO MOTION");
        reportTaskHeartbeat(TASK_MOTION);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/**
 * @brief Generates simulated heart-rate and SpO2 values.
 *
 * Vital samples are sent to the display task, while simulated facility values
 * are updated under telemetryMux. The task feeds its watchdog heartbeat and
 * runs every 2 seconds after registering with the TWDT.
 */
void vitalTask(void *parameter)
{
    (void)parameter;
    esp_err_t wdtResult = esp_task_wdt_add(NULL);
    if (wdtResult != ESP_OK) {
        Serial.print("[WATCHDOG] VITAL registration failed: ");
        Serial.println(esp_err_to_name(wdtResult));
        return;
    }
    Serial.println("[WATCHDOG] VITAL task registered with TWDT");

    while (true) {
        SensorMessage message = {};
        message.type = VITAL_DATA;
        message.heartRate = random(68, 81);
        message.spo2 = random(95, 99);
        sendSensorMessage(message, "VITAL TASK");

        Serial.print("[VITAL TASK] HR: ");
        Serial.print(message.heartRate);
        Serial.print(" BPM | SpO2: ");
        Serial.print(message.spo2);
        Serial.println(" %");

        simulateFacilityData();
        reportTaskHeartbeat(TASK_VITAL);
        vTaskDelay(pdMS_TO_TICKS(getSamplePeriodMs(TASK_VITAL)));
    }
}

/**
 * @brief Generates the simulated P/Q/R/S/T ECG waveform used by the prototype.
 */
static float generateEcgWaveform(float phase)
{
    const float pWave = 0.15f * expf(-28.0f * (phase - 0.18f) * (phase - 0.18f));
    const float qWave = -0.12f * expf(-96.0f * (phase - 0.26f) * (phase - 0.26f));
    const float rWave = 1.30f * expf(-180.0f * (phase - 0.33f) * (phase - 0.33f));
    const float sWave = -0.38f * expf(-180.0f * (phase - 0.40f) * (phase - 0.40f));
    const float tWave = 0.18f * expf(-20.0f * (phase - 0.72f) * (phase - 0.72f));
    return pWave + qWave + rWave + sWave + tWave;
}

/**
 * @brief Generates and queues a simulated ECG waveform sample.
 *
 * The waveform and heart rate are passed to displayTask through sensorQueue.
 * The task registers with the TWDT, reports its heartbeat, and samples every
 * 200 ms; it does not acquire physical ECG hardware.
 */
void ecgTask(void *parameter)
{
    (void)parameter;
    esp_err_t wdtResult = esp_task_wdt_add(NULL);
    if (wdtResult != ESP_OK) {
        Serial.print("[WATCHDOG] ECG registration failed: ");
        Serial.println(esp_err_to_name(wdtResult));
        return;
    }
    Serial.println("[WATCHDOG] ECG task registered with TWDT");

    float phase = 0.0f;
    int ecgRate = 72;

    while (true) {
        const float cycleDuration = 60.0f / static_cast<float>(ecgRate);
        const float normalizedPhase = fmodf(phase, cycleDuration) / cycleDuration;
        SensorMessage message = {};
        message.type = ECG_DATA;
        message.ecgRate = ecgRate;
        message.ecgSample = generateEcgWaveform(normalizedPhase);
        sendSensorMessage(message, "ECG TASK");

        if ((millis() / 1000) % 5 == 0 && message.ecgSample > 0.8f) {
            Serial.print("[ECG TASK] Simulated rhythm: ");
            Serial.print(message.ecgRate);
            Serial.print(" BPM | sample=");
            Serial.println(message.ecgSample, 2);
        }

        const uint32_t sampleMs = getSamplePeriodMs(TASK_ECG);
        phase += static_cast<float>(sampleMs) / 1000.0f;
        if (phase >= cycleDuration) {
            phase -= cycleDuration;
            ecgRate = random(70, 79);
        }

        reportTaskHeartbeat(TASK_ECG);
        vTaskDelay(pdMS_TO_TICKS(sampleMs));
    }
}
