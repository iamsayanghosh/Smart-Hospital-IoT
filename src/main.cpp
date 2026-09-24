/**
 * @file main.cpp
 * @brief ESP32 Smart Hospital IoT monitoring prototype.
 *
 * Implements a FreeRTOS-based concurrent monitoring system with physical
 * DS18B20 temperature and PIR motion inputs; simulated heart rate, SpO2, ECG,
 * room temperature, oxygen, and AQI data; local OLED display; Adafruit IO MQTT
 * communication; alert handling; telemetry aggregation; and task monitoring.
 */

// ============================================================================
// LIBRARIES
// ============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <math.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#include "freertos/semphr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_task_wdt.h"
#include "adafruit_io_config.h"
#include "display.h"
#include "state.h"

// ============================================================================
// HARDWARE PIN DEFINITIONS
// ============================================================================

#define DS18B20_PIN 4
#define PIR_PIN 27
#define OLED_SDA 21
#define OLED_SCL 22
#define RED_LED_PIN 25
#define YELLOW_LED_PIN 26
#define GREEN_LED_PIN 33
#define BUZZER_PIN 14
#define PUSHBUTTON_PIN 13
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

// DS18B20 temperature input: GPIO 4. PIR motion input: GPIO 27.
// OLED uses I2C on SDA GPIO 21 and SCL GPIO 22 at 128x64 resolution.
// Red alert LED: GPIO 25. Yellow LED: GPIO 26. Green LED: GPIO 33.
// Buzzer: GPIO 14. Pushbutton: GPIO 13 for medication confirmation.

// ============================================================================
// MQTT / ADAFRUIT IO CONFIGURATION
// ============================================================================

#define MEDICAL_HEART_RATE_FEED "medical-heart-rate"
#define MEDICAL_SPO2_FEED "medical-spo2"
#define MEDICAL_BODY_TEMPERATURE_FEED "medical-body-temperature"
#define FACILITY_ROOM_TEMPERATURE_FEED "facility-room-temperature"
#define FACILITY_OXYGEN_FEED "facility-oxygen"
#define FACILITY_AQI_FEED "facility-aqi"
#define HOSPITAL_EVENTS_FEED "hospital-events"
#define MEDICATION_DOSAGE_FEED "medication-dosage"





const uint32_t MEDICATION_BUTTON_DEBOUNCE_MS = 50;

#include "connectivity.h"
#include "watchdog.h"

// ============================================================================
// HARDWARE INTERFACES
// ============================================================================

OneWire oneWire(DS18B20_PIN);
DallasTemperature temperatureSensor(&oneWire);

// ============================================================================
// FREERTOS SYNCHRONIZATION OBJECTS
// ============================================================================

#include "state.h"
#include "app.h"

// ============================================================================
// WATCHDOG CONFIGURATION
// ============================================================================

// Worker tasks subscribe to the ESP Task Watchdog and feed it through
// reportTaskHeartbeat(). The heartbeat timeout is diagnostic logging; the TWDT
// timeout is the enforcement mechanism for a subscribed task that stops feeding.
const uint32_t TWDT_TIMEOUT_MS = 8000;
const uint32_t WATCHDOG_TIMEOUT_MS = 6000; // Diagnostic heartbeat age limit, below the 8-second TWDT limit.
const uint32_t WATCHDOG_CHECK_INTERVAL_MS = 1000; // Heartbeat scan interval.
const uint32_t HEARTBEAT_MUTEX_WAIT_MS = 50; // Maximum wait to access heartbeat state.
const uint32_t MQTT_TASK_INTERVAL_MS = 3000;
const uint32_t MQTT_LOOP_INTERVAL_MS = 50;
const uint32_t MQTT_INITIAL_RETRY_DELAY_MS = 1000;
const uint32_t MQTT_MAX_RETRY_DELAY_MS = 30000;
const uint32_t STARTUP_GRACE_PERIOD_MS = 2000; // Allows workers to report their first heartbeat.
const uint32_t SUMMARY_INTERVAL_MS = 60000;
const uint32_t ALERT_PUBLISH_INTERVAL_MS = 60000;
const uint16_t MQTT_SUMMARY_BUFFER_MARGIN = 64;




// Tracks whether each live feed has been published after connection or
// reconnection, allowing the first value through even when it has not changed.
enum LiveFeedIndex {
    LIVE_MEDICAL_HEART_RATE,
    LIVE_MEDICAL_SPO2,
    LIVE_MEDICAL_BODY_TEMPERATURE,
    LIVE_FACILITY_ROOM_TEMPERATURE,
    LIVE_FACILITY_OXYGEN,
    LIVE_FACILITY_AQI,
    LIVE_FEED_COUNT
};


// ============================================================================
// HELPER FUNCTIONS
// ============================================================================



void handleMedicationConfirmationButton()
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

void updateMedicationSafetyOutputs()
{
    static bool buzzerOn = false;
    MedicationDosageControl state = getMedicationDosageSnapshot();
    const uint32_t now = millis();

    const bool criticalPending = state.criticalConfirmationPending && state.requested > MEDICATION_CRITICAL_DOSAGE_THRESHOLD;
    const bool criticalActive = criticalPending || getMedicationSafetyLevel(state.applied) == MEDICATION_SAFETY_CRITICAL;
    bool redLedOn = false;
    bool yellowLedOn = false;
    bool greenLedOn = false;
    bool desiredBuzzerOn = false;

    if (criticalActive) {
        const bool redBlinkOn = ((now / 150U) % 2U) == 0U;
        redLedOn = redBlinkOn;
        desiredBuzzerOn = redBlinkOn;
    } else if (getMedicationSafetyLevel(state.applied) == MEDICATION_SAFETY_WARNING) {
        yellowLedOn = true;
    } else {
        greenLedOn = true;
    }

    digitalWrite(RED_LED_PIN, redLedOn ? HIGH : LOW);
    digitalWrite(YELLOW_LED_PIN, yellowLedOn ? HIGH : LOW);
    digitalWrite(GREEN_LED_PIN, greenLedOn ? HIGH : LOW);

    if (desiredBuzzerOn != buzzerOn) {
        buzzerOn = desiredBuzzerOn;
        if (buzzerOn) {
            tone(BUZZER_PIN, 1200);
        } else {
            noTone(BUZZER_PIN);
        }
    }
}

// ============================================================================
// MEDICATION TASK
// ============================================================================

/**
 * @brief Runs medication confirmation, dosage control, and safety outputs.
 *
 * The task remains responsive to the button and independent of MQTT timing.
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

/**
 * @brief Combines aggregated medical and facility snapshots into one payload.
 *
 * The resulting message is published periodically through hospital-events.
 */

// ============================================================================
// TEMPERATURE TASK
// ============================================================================

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

// ============================================================================
// MOTION TASK
// ============================================================================

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

// ============================================================================
// VITAL SIGNS TASK
// ============================================================================

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

// ============================================================================
// ECG TASK
// ============================================================================

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

// ============================================================================
// DISPLAY TASK
// ============================================================================

// ============================================================================
// ALERT TASK
// ============================================================================

/**
 * @brief Responds to newly detected medical or facility abnormal states.
 *
 * Reads protected telemetry snapshots after alertSemaphore is signaled and
 * logs the event. It feeds its watchdog heartbeat after TWDT registration
 * while waiting for alerts, using a 500 ms semaphore wait and 50 ms retry
 * delay.
 */
void alertTask(void *parameter)
{
    (void)parameter;
    esp_err_t wdtResult = esp_task_wdt_add(NULL);
    if (wdtResult != ESP_OK) {
        Serial.print("[WATCHDOG] ALERT registration failed: ");
        Serial.println(esp_err_to_name(wdtResult));
        return;
    }
    Serial.println("[WATCHDOG] ALERT task registered with TWDT");

    while (true) {
        reportTaskHeartbeat(TASK_ALERT);
        if (xSemaphoreTake(alertSemaphore, pdMS_TO_TICKS(500)) == pdTRUE) {
            Telemetry snapshot = getTelemetrySnapshot();
            FacilityTelemetry facilitySnapshot = getFacilitySnapshot();
            Serial.print("[ALERT TASK] Medical abnormal: HR=");
            Serial.print(snapshot.heartRate);
            Serial.print(" SpO2=");
            Serial.print(snapshot.spo2);
            Serial.print(" Temp=");
            Serial.print(snapshot.temperature, 1);
            Serial.print(" | Facility abnormal: room=");
            Serial.print(facilitySnapshot.roomTemperature, 1);
            Serial.print("C oxygen=");
            Serial.print(facilitySnapshot.oxygenLevel, 1);
            Serial.print("% AQI=");
            Serial.println(facilitySnapshot.aqi);

        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

// ============================================================================
// MQTT TASK SUPPORT
// ============================================================================


/**
 * @brief Logs a fatal initialization error and holds the system in a safe loop.
 */


/**
 * @brief Processes serial commands for facility telemetry simulation/testing.
 *
 * Supported commands are SET_ROOM <C>, SET_OXYGEN <percent>, SET_AQI <value>,
 * RESET_FACILITY, and SHOW_TASK2; these are simulation controls, not
 * production facility-management commands.
 */
void handleSerialCommands()
{
    static char lineBuffer[96];
    static size_t lineLength = 0;

    while (Serial.available() > 0) {
        const char character = static_cast<char>(Serial.read());
        if (character == '\n' || character == '\r') {
            if (lineLength == 0) {
                continue;
            }
            lineBuffer[lineLength] = '\0';
            String command(lineBuffer);
            lineLength = 0;

            if (command.startsWith("SET_ROOM ")) {
                String value = command.substring(9);
                portENTER_CRITICAL(&telemetryMux);
                facilityTelemetry.roomTemperature = value.toFloat();
                const float roomTemperature = facilityTelemetry.roomTemperature;
                portEXIT_CRITICAL(&telemetryMux);
                Serial.print("[SIM] Room temperature set to ");
                Serial.println(roomTemperature, 1);
            } else if (command.startsWith("SET_OXYGEN ")) {
                String value = command.substring(11);
                const float oxygenLevel = constrain(value.toFloat(), 16.0f, 25.0f);
                portENTER_CRITICAL(&telemetryMux);
                facilityTelemetry.oxygenLevel = oxygenLevel;
                portEXIT_CRITICAL(&telemetryMux);
                Serial.print("[SIM] Oxygen set to ");
                Serial.println(oxygenLevel, 1);
            } else if (command.startsWith("SET_AQI ")) {
                String value = command.substring(8);
                const int aqi = constrain(value.toInt(), 0, 250);
                portENTER_CRITICAL(&telemetryMux);
                facilityTelemetry.aqi = aqi;
                portEXIT_CRITICAL(&telemetryMux);
                Serial.print("[SIM] AQI set to ");
                Serial.println(aqi);
            } else if (command == "RESET_FACILITY") {
                portENTER_CRITICAL(&telemetryMux);
                facilityTelemetry.roomTemperature = 22.0f;
                facilityTelemetry.oxygenLevel = 20.8f;
                facilityTelemetry.aqi = 35;
                portEXIT_CRITICAL(&telemetryMux);
                Serial.println("[SIM] Facility simulation reset to normal range");
            } else if (command == "SHOW_TASK2") {
                Serial.println("[TASK2] Medical feeds: medical-heart-rate | medical-spo2 | medical-body-temperature");
                Serial.println("[TASK2] Facility feeds: facility-room-temperature | facility-oxygen | facility-aqi");
                Serial.println("[TASK2] Event feed: hospital-events (alerts + summaries + status)");
            } else {
                Serial.println("[SIM] Commands: SET_ROOM <C>, SET_OXYGEN <percent>, SET_AQI <value>, RESET_FACILITY, SHOW_TASK2");
            }
            continue;
        }

        if (lineLength < sizeof(lineBuffer) - 1) {
            lineBuffer[lineLength++] = character;
        }
    }
}

// ============================================================================
// SETUP
// ============================================================================

/**
 * @brief Initializes hardware, synchronization objects, watchdog, and tasks.
 *
 * The sequence is serial, GPIO and sensor setup, I2C/OLED setup, RTOS object
 * creation, telemetry initialization, watchdog configuration, and task start.
 */
void setup()
{
    // Initialize serial communication for startup, sensor, MQTT, and watchdog logs.
    Serial.begin(115200);
    vTaskDelay(pdMS_TO_TICKS(500));
    Serial.println("\n================================");
    Serial.println(" SMART HOSPITAL IoT - TASK 3");
    Serial.println(" REMOTE MEDICATION DOSAGE CONTROL");
    Serial.println("================================");
    Serial.println("[TASK3] Adafruit IO credentials loaded from include/adafruit_io_config.h or include/adafruit_io_config.local.h");

    // Initialize the PIR GPIO and physical DS18B20 temperature interface.
    pinMode(PIR_PIN, INPUT);
    pinMode(RED_LED_PIN, OUTPUT);
    pinMode(YELLOW_LED_PIN, OUTPUT);
    pinMode(GREEN_LED_PIN, OUTPUT);
    pinMode(BUZZER_PIN, OUTPUT);
    pinMode(PUSHBUTTON_PIN, INPUT_PULLUP);
    digitalWrite(RED_LED_PIN, LOW);
    digitalWrite(YELLOW_LED_PIN, LOW);
    digitalWrite(GREEN_LED_PIN, LOW);
    noTone(BUZZER_PIN);
    temperatureSensor.begin();

    // Initialize I2C and the 128x64 OLED at the configured 0x3C address.
    displayInit(OLED_SDA, OLED_SCL);

    appInit();
    resetSummaryStats();
    appStartTasks();
}

// ============================================================================
// LOOP
// ============================================================================

/**
 * @brief Processes serial simulation commands while FreeRTOS tasks run.
 *
 * The Arduino loop does not handle sensors or MQTT directly; it remains
 * lightweight and yields for 1000 ms after command processing.
 */
void loop()
{
    handleSerialCommands();
    vTaskDelay(pdMS_TO_TICKS(1000));
}