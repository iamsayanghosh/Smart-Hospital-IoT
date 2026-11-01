/**
 * @file sensors.cpp
 * @brief Multi-sensor acquisition and physiological data simulation task implementations (Task 1).
 *
 * Responsibilities:
 * 1. Physical Sensing:
 *    - temperatureTask: Acquires Dallas DS18B20 1-Wire temperature from GPIO 4.
 *    - motionTask: Acquires binary motion detection from HC-SR501 PIR sensor on GPIO 27.
 * 2. Physiological Simulation:
 *    - vitalTask: Simulates optical PPG telemetry (Heart Rate in BPM and SpO2 in %).
 *    - ecgTask: Synthesizes continuous AD8232 PQRST electrocardiogram waveforms using Gaussian modeling.
 * 3. Concurrency & Adaptive Sampling:
 *    - Both temperatureTask and vitalTask query their active period dynamically via
 *      getSamplePeriodMs() [5000 ms to 60000 ms], while executing an inner 500 ms delay
 *      to ensure prompt watchdog heartbeat reporting without TWDT starvation.
 *    - All sensor readings are packaged into SensorMessage structs and dispatched to sensorQueue.
 */

#include "sensors.h"
#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "state.h"
#include "watchdog.h"
#include "esp_task_wdt.h"
#include "app.h"
#include "environment.h"

static volatile int simOverrideHeartRate = -1;
static volatile int simOverrideSpo2 = -1;
static volatile float simOverrideTemperature = -999.0f;

/**
 * @brief Overrides simulated heart rate for interactive testing/demonstration in Wokwi/CLI.
 *
 * @param hr Forced heart rate in BPM, or -1 to restore random simulation.
 */
void setSimulatedHeartRate(int hr) {
    simOverrideHeartRate = hr;
    Serial.print("[SIM CLI] Forced Heart Rate: ");
    Serial.print(hr);
    Serial.println(" BPM");
}

/**
 * @brief Overrides simulated blood oxygen saturation for interactive testing in Wokwi/CLI.
 *
 * @param spo2 Forced SpO2 percentage [0-100], or -1 to restore random simulation.
 */
void setSimulatedSpo2(int spo2) {
    simOverrideSpo2 = spo2;
    Serial.print("[SIM CLI] Forced SpO2: ");
    Serial.print(spo2);
    Serial.println(" %");
}

/**
 * @brief Overrides simulated patient body temperature for interactive testing in Wokwi/CLI.
 *
 * @param temp Forced temperature in deg C, or -999.0f to restore sensor simulation.
 */
void setSimulatedTemperature(float temp) {
    simOverrideTemperature = temp;
    Serial.print("[SIM CLI] Forced Temperature: ");
    Serial.print(temp, 1);
    Serial.println(" C");
}

/**
 * @brief Restores nominal physiological vitals simulation, clearing all active overrides.
 */
void resetSimulatedVitals() {
    simOverrideHeartRate = -1;
    simOverrideSpo2 = -1;
    simOverrideTemperature = -999.0f;
    Serial.println("[SIM CLI] Restored nominal random vitals simulation");
}

#define DS18B20_PIN 4  ///< GPIO pin assigned to 1-Wire data bus for DS18B20
#define PIR_PIN 27     ///< GPIO pin assigned to digital output of PIR motion sensor

OneWire oneWire(DS18B20_PIN);
DallasTemperature temperatureSensor(&oneWire);

/**
 * @brief Configures sensor hardware interfaces.
 */
void sensorsInit() {
    pinMode(PIR_PIN, INPUT);
    temperatureSensor.begin();
    temperatureSensor.setWaitForConversion(false);
}

/**
 * @brief Acquires physical body temperature from DS18B20 and posts readings to sensorQueue.
 *
 * Concurrency & Adaptive Timing:
 * - Sampling Cadence: Determined dynamically by getSamplePeriodMs(TASK_TEMP) [5 s to 60 s].
 * - Non-blocking conversion: Two-step state machine (TEMP_IDLE -> TEMP_CONVERTING) ensures
 *   750 ms 12-bit DS18B20 thermal conversion does not block FreeRTOS scheduler or starve TWDT.
 * - Startup Guard: Rejects power-on reset scratchpad reading (+85.0 C) and DEVICE_DISCONNECTED_C.
 * - Watchdog Polling: Runs an internal 100 ms evaluation tick to regularly report task heartbeats
 *   to TWDT even when the temperature acquisition interval is stretched to 60 seconds.
 * - IPC: Dispatches TEMP_DATA messages via sendSensorMessage() into sensorQueue.
 *
 * @param parameter Unused FreeRTOS task parameter pointer (NULL).
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

    enum TempReadState { TEMP_IDLE, TEMP_CONVERTING };
    TempReadState tempState = TEMP_IDLE;
    uint32_t lastSampleMs = 0;
    uint32_t requestStartMs = 0;

    while (true) {
        uint32_t now = millis();
        uint32_t period = getSamplePeriodMs(TASK_TEMP);
        
        if (tempState == TEMP_IDLE && (now - lastSampleMs >= period || lastSampleMs == 0)) {
            lastSampleMs = now;
            if (simOverrideTemperature > -100.0f) {
                SensorMessage message = {};
                message.type = TEMP_DATA;
                message.temperature = simOverrideTemperature;
                sendSensorMessage(message, "TEMP TASK");

                Serial.print("[TEMP TASK] Temperature (override): ");
                Serial.print(message.temperature, 1);
                Serial.println(" C");
            } else {
                temperatureSensor.requestTemperatures(); // Non-blocking: returns in <1 ms
                requestStartMs = now;
                tempState = TEMP_CONVERTING;
            }
        } else if (tempState == TEMP_CONVERTING && (now - requestStartMs >= 800)) {
            tempState = TEMP_IDLE;
            float rawTemp = simOverrideTemperature > -100.0f ? simOverrideTemperature : temperatureSensor.getTempCByIndex(0);

            // Filter out default uninitialized power-on scratchpad (+85.0 C)
            if (abs(rawTemp - 85.0f) <= 0.05f) {
                Serial.println("[TEMP TASK] Ignored DS18B20 power-on reset scratchpad reading (85.0 C)");
            } else if (rawTemp == DEVICE_DISCONNECTED_C || rawTemp <= -55.0f || rawTemp > 125.0f) {
                Serial.println("[TEMP TASK] Warning: DS18B20 disconnected");
                SensorMessage message = {};
                message.type = TEMP_DATA;
                message.temperature = DEVICE_DISCONNECTED_C;
                sendSensorMessage(message, "TEMP TASK");
            } else {
                SensorMessage message = {};
                message.type = TEMP_DATA;
                message.temperature = rawTemp;
                sendSensorMessage(message, "TEMP TASK");

                Serial.print("[TEMP TASK] Temperature: ");
                Serial.print(message.temperature, 1);
                Serial.println(" C");
            }
        }

        reportTaskHeartbeat(TASK_TEMP);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/**
 * @brief Reads the physical PIR motion input on GPIO 27.
 *
 * Enqueues digital motion state (true=motion detected, false=quiescent) to sensorQueue.
 * Runs on a dedicated 1000 ms period, registering and feeding its TWDT heartbeat.
 *
 * @param parameter Unused FreeRTOS task parameter pointer (NULL).
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
 * @brief Generates simulated heart rate (BPM) and blood oxygen saturation (SpO2 %).
 *
 * Concurrency & Adaptive Timing:
 * - Dynamic Sampling: Evaluates acquisition schedule dynamically via getSamplePeriodMs(TASK_VITAL)
 *   (5 s to 60 s), adjusting sampling frequency based on patient clinical stability.
 * - Watchdog Polling: Runs an inner 500 ms evaluation loop to feed its TWDT heartbeat
 *   frequently, preventing watchdog timeouts during 60-second stable periods.
 * - Facility Sync: Calls simulateFacilityData() upon each vital sample generation to advance
 *   ambient room parameters in lockstep.
 *
 * @param parameter Unused FreeRTOS task parameter pointer (NULL).
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

    uint32_t lastSampleMs = 0;
    while (true) {
        uint32_t now = millis();
        uint32_t period = getSamplePeriodMs(TASK_VITAL);
        
        if (now - lastSampleMs >= period || lastSampleMs == 0) {
            lastSampleMs = now;
            SensorMessage message = {};
            message.type = VITAL_DATA;
            message.heartRate = (simOverrideHeartRate >= 0) ? simOverrideHeartRate : random(68, 81);
            message.spo2 = (simOverrideSpo2 >= 0) ? simOverrideSpo2 : random(95, 99);
            sendSensorMessage(message, "VITAL TASK");

            Serial.print("[VITAL TASK] HR: ");
            Serial.print(message.heartRate);
            Serial.print(" BPM | SpO2: ");
            Serial.print(message.spo2);
            Serial.println(" %");
        }
        reportTaskHeartbeat(TASK_VITAL);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/**
 * @brief Generates the simulated P/Q/R/S/T ECG waveform using Gaussian curve superposition.
 *
 * @param phase Normalized cardiac cycle phase [0.0 - 1.0].
 * @return float Synthesized millivolt-scale electrocardiogram amplitude.
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
 * @brief Generates and queues simulated ECG waveform samples at fixed cadence (5 Hz / 200 ms).
 *
 * Runs on a dedicated high-priority schedule (Priority 3) querying samplingPeriods.ecgMs (200 ms).
 * Operates independently of adaptive sampling rules to maintain uninterrupted cardiac rhythms.
 *
 * @param parameter Unused FreeRTOS task parameter pointer (NULL).
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
