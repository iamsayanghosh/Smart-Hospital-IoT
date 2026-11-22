/**
 * @file sensors.cpp
 * @brief Multi-sensor acquisition and physiological data simulation tasks (Task 1).
 *
 * Implements physical DS18B20 1-Wire temperature (GPIO 4), HC-SR501 PIR motion (GPIO 27),
 * optical PPG heart rate and SpO2 simulation, and AD8232 ECG waveform synthesis.
 */

#include "sensors.h"
#include "app.h"
#include "environment.h"
#include "esp_task_wdt.h"
#include "state.h"
#include "watchdog.h"
#include <Arduino.h>
#include <DallasTemperature.h>
#include <OneWire.h>
#include <atomic>

static std::atomic<int> simOverrideHeartRate{-1};
static std::atomic<int> simOverrideSpo2{-1};
static std::atomic<float> simOverrideTemperature{-999.0f};
static std::atomic<bool> simImmediateVitalSample{false};
static std::atomic<bool> simImmediateTempSample{false};

/**
 * @brief Overrides simulated heart rate for interactive testing/demonstration
 * in Wokwi/CLI.
 *
 * @param hr Forced heart rate in BPM, or -1 to restore random simulation.
 */
void setSimulatedHeartRate(int hr) {
  simOverrideHeartRate = hr;
  simImmediateVitalSample = true;
  Serial.print("[SIM CLI] Forced Heart Rate: ");
  Serial.print(hr);
  Serial.println(" BPM");
}

/**
 * @brief Overrides simulated blood oxygen saturation for interactive testing in
 * Wokwi/CLI.
 *
 * @param spo2 Forced SpO2 percentage [0-100], or -1 to restore random
 * simulation.
 */
void setSimulatedSpo2(int spo2) {
  simOverrideSpo2 = spo2;
  simImmediateVitalSample = true;
  Serial.print("[SIM CLI] Forced SpO2: ");
  Serial.print(spo2);
  Serial.println(" %");
}

/**
 * @brief Overrides simulated patient body temperature for interactive testing
 * in Wokwi/CLI.
 *
 * @param temp Forced temperature in deg C, or -999.0f to restore sensor
 * simulation.
 */
void setSimulatedTemperature(float temp) {
  simOverrideTemperature = temp;
  simImmediateTempSample = true;
  Serial.print("[SIM CLI] Forced Temperature: ");
  Serial.print(temp, 1);
  Serial.println(" C");
}

/**
 * @brief Restores nominal physiological vitals simulation, clearing all active
 * overrides.
 */
void resetSimulatedVitals() {
  simOverrideHeartRate = -1;
  simOverrideSpo2 = -1;
  simOverrideTemperature = -999.0f;
  simImmediateVitalSample = true;
  simImmediateTempSample = true;
  Serial.println("[SIM CLI] Restored nominal random vitals simulation");
}

#define DS18B20_PIN 4 ///< GPIO pin assigned to 1-Wire data bus for DS18B20
#define PIR_PIN 27 ///< GPIO pin assigned to digital output of PIR motion sensor

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
 * @brief Acquires physical body temperature from DS18B20 and posts readings to
 * sensorQueue.
 *
 * Concurrency & Adaptive Timing:
 * - Sampling Cadence: Determined dynamically by getSamplePeriodMs(TASK_TEMP) [5
 * s to 60 s].
 * - Non-blocking conversion: Two-step state machine (TEMP_IDLE ->
 * TEMP_CONVERTING) ensures 750 ms 12-bit DS18B20 thermal conversion does not
 * block FreeRTOS scheduler or starve TWDT.
 * - Startup Guard: Rejects power-on reset scratchpad reading (+85.0 C) and
 * DEVICE_DISCONNECTED_C.
 * - Watchdog Polling: Runs an internal 100 ms evaluation tick to regularly
 * report task heartbeats to TWDT even when the temperature acquisition interval
 * is stretched to 60 seconds.
 * - IPC: Dispatches TEMP_DATA messages via sendSensorMessage() into
 * sensorQueue.
 *
 * @param parameter Unused FreeRTOS task parameter pointer (NULL).
 */
void temperatureTask(void *parameter) {
  (void)parameter;
  esp_err_t wdtResult = esp_task_wdt_add(NULL);
  if (wdtResult != ESP_OK) {
    Serial.print("[WATCHDOG] TEMP registration failed: ");
    Serial.println(esp_err_to_name(wdtResult));
    vTaskDelete(NULL);
    return;
  }
  Serial.println("[WATCHDOG] TEMP task registered with TWDT");

  enum TempReadState { TEMP_IDLE, TEMP_CONVERTING };
  TempReadState tempState = TEMP_IDLE;
  uint32_t lastSampleMs = 0;
  uint32_t requestStartMs = 0;
  bool hasReadValidTemperature = false;
  bool pendingTempDisconnectMessage = false;
  uint8_t consecutiveScratchpadCount = 0;
  bool temperatureFaultReported = false; // latch: suppress duplicate fault notifications per outage

  while (true) {
    uint32_t now = millis();
    uint32_t period = getSamplePeriodMs(TASK_TEMP);

    if (pendingTempDisconnectMessage) {
      SensorMessage retryMsg = {};
      retryMsg.type = TEMP_DATA;
      retryMsg.temperature = DEVICE_DISCONNECTED_C;
      bool sent = sendSensorMessage(retryMsg, "TEMP TASK");
      if (!retryTemperatureDisconnect(sent, pendingTempDisconnectMessage, temperatureFaultReported)) {
        // Queue still saturated; yield and retry next tick, preserving pending fault
        reportTaskHeartbeat(TASK_TEMP);
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }
    }

    // Consume immediate sample request unconditionally to prevent duplicate samples
    bool forced = simImmediateTempSample.exchange(false);
    if (forced && tempState == TEMP_CONVERTING) {
      // Abort in-flight physical conversion to immediately dispatch requested override
      tempState = TEMP_IDLE;
    }

    if (tempState == TEMP_IDLE &&
        (forced || now - lastSampleMs >= period || lastSampleMs == 0)) {
      lastSampleMs = now;
      float overrideTemp = simOverrideTemperature.load();
      if (overrideTemp > -100.0f) {
        // Simulation override: does not alter physical sensor hardware history
        consecutiveScratchpadCount = 0;

        SensorMessage message = {};
        message.type = TEMP_DATA;
        message.temperature = overrideTemp;
        bool sent = sendSensorMessage(message, "TEMP TASK");

        updateTemperatureFaultReportState(false, sent, pendingTempDisconnectMessage, temperatureFaultReported);

        Serial.print("[TEMP TASK] Temperature (override): ");
        Serial.print(message.temperature, 1);
        Serial.println(" C");
      } else {
        temperatureSensor
            .requestTemperatures(); // Non-blocking: returns in <1 ms
        requestStartMs = now;
        tempState = TEMP_CONVERTING;
      }
    } else if (tempState == TEMP_CONVERTING && (now - requestStartMs >= 800)) {
      tempState = TEMP_IDLE;
      float overrideTemp = simOverrideTemperature.load();
      float rawTemp = (overrideTemp > -100.0f)
                          ? overrideTemp
                          : temperatureSensor.getTempCByIndex(0);

      // Distinguish initial cold-boot scratchpad (+85.0 C) from runtime sensor reset
      if (abs(rawTemp - 85.0f) <= 0.05f) {
        Ds18b20ScratchpadStatus status = classifyDs18b20Scratchpad(
            hasReadValidTemperature, pendingTempDisconnectMessage, consecutiveScratchpadCount);

        if (status == DS18B20_SCRATCHPAD_IGNORE_INITIAL) {
          Serial.print("[TEMP TASK] Ignored initial DS18B20 power-on reset scratchpad reading (85.0 C), attempt ");
          Serial.print(consecutiveScratchpadCount);
          Serial.println("/3");
        } else if (status == DS18B20_SCRATCHPAD_PERSISTENT_FAULT) {
          hasReadValidTemperature = false;
          if (!temperatureFaultReported && !pendingTempDisconnectMessage) {
            Serial.println("[TEMP TASK] Warning: Persistent 85.0 C scratchpad detected; marking sensor unavailable");
            SensorMessage message = {};
            message.type = TEMP_DATA;
            message.temperature = DEVICE_DISCONNECTED_C;
            bool sent = sendSensorMessage(message, "TEMP TASK");
            updateTemperatureFaultReportState(true, sent, pendingTempDisconnectMessage, temperatureFaultReported);
          } else {
            Serial.println("[TEMP TASK] Persistent 85.0 C scratchpad; fault already reported, retrying");
          }
        } else { // DS18B20_SCRATCHPAD_RUNTIME_RESET
          hasReadValidTemperature = false;
          if (!temperatureFaultReported && !pendingTempDisconnectMessage) {
            Serial.println("[TEMP TASK] Warning: DS18B20 reset during operation (85.0 C scratchpad); marking unavailable");
            SensorMessage message = {};
            message.type = TEMP_DATA;
            message.temperature = DEVICE_DISCONNECTED_C;
            bool sent = sendSensorMessage(message, "TEMP TASK");
            updateTemperatureFaultReportState(true, sent, pendingTempDisconnectMessage, temperatureFaultReported);
          } else {
            Serial.println("[TEMP TASK] DS18B20 85.0 C runtime reset; fault already reported, retrying");
          }
        }
      } else if (rawTemp == DEVICE_DISCONNECTED_C || rawTemp <= -55.0f ||
                 rawTemp > 125.0f) {
        consecutiveScratchpadCount = 0;
        hasReadValidTemperature = false;
        if (!temperatureFaultReported && !pendingTempDisconnectMessage) {
          Serial.println("[TEMP TASK] Warning: DS18B20 disconnected");
          SensorMessage message = {};
          message.type = TEMP_DATA;
          message.temperature = DEVICE_DISCONNECTED_C;
          bool sent = sendSensorMessage(message, "TEMP TASK");
          updateTemperatureFaultReportState(true, sent, pendingTempDisconnectMessage, temperatureFaultReported);
        } else {
          Serial.println("[TEMP TASK] DS18B20 disconnected; fault already reported, retrying");
        }
      } else {
        // Physical DS18B20 conversion succeeded: record hardware acquisition history
        recordPhysicalTemperatureAcquisition(hasReadValidTemperature, consecutiveScratchpadCount);

        SensorMessage message = {};
        message.type = TEMP_DATA;
        message.temperature = rawTemp;
        bool sent = sendSensorMessage(message, "TEMP TASK");

        // Downstream queue delivery & fault latch clearance
        updateTemperatureFaultReportState(false, sent, pendingTempDisconnectMessage, temperatureFaultReported);

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
 * Enqueues digital motion state (true=motion detected, false=quiescent) to
 * sensorQueue. Runs on a dedicated 1000 ms period, registering and feeding its
 * TWDT heartbeat.
 *
 * @param parameter Unused FreeRTOS task parameter pointer (NULL).
 */
void motionTask(void *parameter) {
  (void)parameter;
  esp_err_t wdtResult = esp_task_wdt_add(NULL);
  if (wdtResult != ESP_OK) {
    Serial.print("[WATCHDOG] MOTION registration failed: ");
    Serial.println(esp_err_to_name(wdtResult));
    vTaskDelete(NULL);
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
 * @brief Generates simulated heart rate (BPM) and blood oxygen saturation (SpO2
 * %).
 *
 * Concurrency & Adaptive Timing:
 * - Dynamic Sampling: Evaluates acquisition schedule dynamically via
 * getSamplePeriodMs(TASK_VITAL) (5 s to 60 s), adjusting sampling frequency
 * based on patient clinical stability.
 * - Watchdog Polling: Runs an inner 500 ms evaluation loop to feed its TWDT
 * heartbeat frequently, preventing watchdog timeouts during 60-second stable
 * periods.
 *
 * @param parameter Unused FreeRTOS task parameter pointer (NULL).
 */
void vitalTask(void *parameter) {
  (void)parameter;
  esp_err_t wdtResult = esp_task_wdt_add(NULL);
  if (wdtResult != ESP_OK) {
    Serial.print("[WATCHDOG] VITAL registration failed: ");
    Serial.println(esp_err_to_name(wdtResult));
    vTaskDelete(NULL);
    return;
  }
  Serial.println("[WATCHDOG] VITAL task registered with TWDT");

  uint32_t lastSampleMs = 0;
  while (true) {
    uint32_t now = millis();
    uint32_t period = getSamplePeriodMs(TASK_VITAL);

    bool forced = simImmediateVitalSample.exchange(false);

    if (forced || now - lastSampleMs >= period || lastSampleMs == 0) {
      lastSampleMs = now;
      SensorMessage message = {};
      message.type = VITAL_DATA;
      int overrideHr = simOverrideHeartRate.load();
      int overrideSpo2 = simOverrideSpo2.load();
      message.heartRate =
          (overrideHr >= 0) ? overrideHr : static_cast<int>(random(68, 81));
      message.spo2 =
          (overrideSpo2 >= 0) ? overrideSpo2 : static_cast<int>(random(95, 99));
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
 * @brief Generates the simulated P/Q/R/S/T ECG waveform using Gaussian curve
 * superposition.
 *
 * @param phase Normalized cardiac cycle phase [0.0 - 1.0].
 * @return float Synthesized millivolt-scale electrocardiogram amplitude.
 */
static float generateEcgWaveform(float phase) {
  const float pWave = 0.15f * expf(-28.0f * (phase - 0.18f) * (phase - 0.18f));
  const float qWave = -0.12f * expf(-96.0f * (phase - 0.26f) * (phase - 0.26f));
  const float rWave = 1.30f * expf(-180.0f * (phase - 0.33f) * (phase - 0.33f));
  const float sWave =
      -0.38f * expf(-180.0f * (phase - 0.40f) * (phase - 0.40f));
  const float tWave = 0.18f * expf(-20.0f * (phase - 0.72f) * (phase - 0.72f));
  return pWave + qWave + rWave + sWave + tWave;
}

/**
 * @brief Generates and queues simulated ECG waveform samples at fixed cadence
 * (5 Hz / 200 ms).
 *
 * Runs on a dedicated high-priority schedule (Priority 3) querying
 * samplingPeriods.ecgMs (200 ms). Operates independently of adaptive sampling
 * rules to maintain uninterrupted cardiac rhythms.
 *
 * @param parameter Unused FreeRTOS task parameter pointer (NULL).
 */
void ecgTask(void *parameter) {
  (void)parameter;
  esp_err_t wdtResult = esp_task_wdt_add(NULL);
  if (wdtResult != ESP_OK) {
    Serial.print("[WATCHDOG] ECG registration failed: ");
    Serial.println(esp_err_to_name(wdtResult));
    vTaskDelete(NULL);
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
