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
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "freertos/semphr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_task_wdt.h"
#include "adafruit_io_config.h"
#include "display.h"
#include "state.h"
#include "outputs.h"
#include "medication.h"
#include "sensors.h"

// ============================================================================
// HARDWARE PIN DEFINITIONS
// ============================================================================

#define OLED_SDA 21
#define OLED_SCL 22
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





#include "connectivity.h"
#include "watchdog.h"

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



/**
 * @brief Combines aggregated medical and facility snapshots into one payload.
 *
 * The resulting message is published periodically through hospital-events.
 */

// ============================================================================
// DISPLAY TASK
// ============================================================================

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

    outputsInit();
    medicationInit();
    sensorsInit();

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