/**
 * @file environment.cpp
 * @brief Implementation of facility environment simulation and test command processing (Task 2).
 *
 * Responsibilities:
 * - Implements simulated environmental sensing for Task 2 (Dual-Role IoT Monitoring):
 *     1. Room Temperature (18.0 - 30.0 °C)
 *     2. Ambient Oxygen Percentage (18.0 - 24.0 %)
 *     3. Air Quality Index (0 - 200 AQI)
 * - Concurrency & Synchronization:
 *     Updates facilityTelemetry protected by telemetryMux spinlock critical sections.
 * - Alerting:
 *     Checks isFacilityAbnormal() and signals alertSemaphore when environmental thresholds
 *     are breached (Room Temp <18 or >28 °C, O2 <19.5 or >24.0 %, AQI >100).
 * - Diagnostic Command Interface:
 *     Processes interactive UART commands for testing boundary conditions in Wokwi simulator.
 */

#include "environment.h"
#include <Arduino.h>
#include "state.h"
#include "sensors.h"
#include "watchdog.h"
#include "esp_task_wdt.h"
#include "parser_utils.h"
#include "mqtt.h"

static volatile float simOverrideRoomTemp = -999.0f;
static volatile float simOverrideOxygen = -999.0f;
static volatile int simOverrideAqi = -1;

/**
 * @brief FreeRTOS task running dedicated facility environment simulation at 1 Hz.
 *
 * @param parameter Unused FreeRTOS task parameter pointer (NULL).
 */
void environmentTask(void *parameter)
{
    (void)parameter;
    esp_task_wdt_add(NULL);
    Serial.println("[WATCHDOG] ENVIRONMENT task registered with TWDT");

    while (true) {
        simulateFacilityData();
        reportTaskHeartbeat(TASK_ENVIRONMENT);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/**
 * @brief Parses and executes UART commands for testing facility telemetry in simulation.
 *
 * Supported commands:
 * - Facility Simulation:
 *     - SET_ROOM <15-40>        : Force ambient room temperature in °C
 *     - SET_OXYGEN <18-25>      : Force ambient oxygen percentage
 *     - SET_AQI <0-300>         : Force Air Quality Index
 *     - RESET_FACILITY          : Restore nominal room parameters (22°C, 20.8% O2, 35 AQI)
 * - Patient Vitals Simulation:
 *     - SET_HR <40-140>         : Force heart rate in BPM
 *     - SET_SPO2 <80-100>       : Force blood oxygen saturation %
 *     - SET_TEMP <34-42>        : Force body temperature in °C
 *     - RESET_VITALS            : Restore nominal random physiological vitals
 * - Offline & Replay Simulation (Task 6):
 *     - SIM_OFFLINE [WIFI|MQTT] : Force Wi-Fi or MQTT drop, activate buzzer cadence & RAM buffering
 *     - SIM_ONLINE              : Restore network link and replay buffered FIFO records
 * - Diagnostics:
 *     - SHOW_TASK2              : Print Dual-Role MQTT feed mappings
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
            command.trim();
            command.toUpperCase();

            if (command.startsWith("SET_ROOM ")) {
                String arg = command.substring(9);
                float val = 0.0f;
                if (!parseFloatStrict(arg.c_str(), 15.0f, 40.0f, val)) {
                    Serial.println("[SIM CLI] Error: Room temperature must be a valid float between 15.0 and 40.0 C");
                } else {
                    simOverrideRoomTemp = val;
                    Serial.print("[SIM CLI] Room temperature overridden to ");
                    Serial.print(val, 1);
                    Serial.println(" C (persists until RESET_FACILITY)");
                }
            } else if (command.startsWith("SET_OXYGEN ")) {
                String arg = command.substring(11);
                float val = 0.0f;
                if (!parseFloatStrict(arg.c_str(), 18.0f, 25.0f, val)) {
                    Serial.println("[SIM CLI] Error: Facility oxygen must be a valid float between 18.0 and 25.0 %");
                } else {
                    simOverrideOxygen = val;
                    Serial.print("[SIM CLI] Facility oxygen overridden to ");
                    Serial.print(val, 1);
                    Serial.println(" % (persists until RESET_FACILITY)");
                }
            } else if (command.startsWith("SET_AQI ")) {
                String arg = command.substring(8);
                long val = 0;
                if (!parseLongStrict(arg.c_str(), 0, 300, val)) {
                    Serial.println("[SIM CLI] Error: AQI must be an integer between 0 and 300");
                } else {
                    simOverrideAqi = static_cast<int>(val);
                    Serial.print("[SIM CLI] AQI overridden to ");
                    Serial.print(val);
                    Serial.println(" (persists until RESET_FACILITY)");
                }
            } else if (command == "RESET_FACILITY") {
                simOverrideRoomTemp = -999.0f;
                simOverrideOxygen = -999.0f;
                simOverrideAqi = -1;
                Serial.println("[SIM CLI] Facility overrides cleared; restored nominal facility simulation");
            } else if (command.startsWith("SET_HR ")) {
                String arg = command.substring(7);
                long val = 0;
                if (!parseLongStrict(arg.c_str(), 40, 140, val)) {
                    Serial.println("[SIM CLI] Error: Heart rate must be an integer between 40 and 140 BPM");
                } else {
                    setSimulatedHeartRate(static_cast<int>(val));
                }
            } else if (command.startsWith("SET_SPO2 ")) {
                String arg = command.substring(9);
                long val = 0;
                if (!parseLongStrict(arg.c_str(), 80, 100, val)) {
                    Serial.println("[SIM CLI] Error: SpO2 must be an integer between 80 and 100 %");
                } else {
                    setSimulatedSpo2(static_cast<int>(val));
                }
            } else if (command.startsWith("SET_TEMP ")) {
                String arg = command.substring(9);
                float val = 0.0f;
                if (!parseFloatStrict(arg.c_str(), 34.0f, 42.0f, val)) {
                    Serial.println("[SIM CLI] Error: Body temperature must be a valid float between 34.0 and 42.0 C");
                } else {
                    setSimulatedTemperature(val);
                }
            } else if (command == "RESET_VITALS") {
                resetSimulatedVitals();
            } else if (command == "SIM_OFFLINE" || command == "SIM_OFFLINE WIFI") {
                setSimulateOfflineWifi(true);
                Serial.println("[SIM CLI] SIM_OFFLINE WIFI: Wi-Fi link dropped; rapid double-pip cadence & buffering active");
            } else if (command == "SIM_OFFLINE MQTT") {
                setSimulateOfflineMqtt(true);
                Serial.println("[SIM CLI] SIM_OFFLINE MQTT: MQTT link dropped; slow single-beep cadence & buffering active");
            } else if (command == "SIM_ONLINE") {
                setSimulateOfflineWifi(false);
                setSimulateOfflineMqtt(false);
                Serial.println("[SIM CLI] SIM_ONLINE: Network restored; replaying buffered telemetry in FIFO order");
            } else if (command == "SHOW_TASK2") {
                Serial.println("[TASK2] Medical feeds: medical-heart-rate | medical-spo2 | medical-body-temperature");
                Serial.println("[TASK2] Facility feeds: facility-room-temperature | facility-oxygen | facility-aqi");
                Serial.println("[TASK2] Event feed: hospital-events (alerts + summaries + status)");
            } else {
                Serial.println("[SIM CLI] Commands: SET_ROOM <15-40>, SET_OXYGEN <18-25>, SET_AQI <0-300>, RESET_FACILITY, SET_HR <40-140>, SET_SPO2 <80-100>, SET_TEMP <34-42>, RESET_VITALS, SIM_OFFLINE [WIFI|MQTT], SIM_ONLINE, SHOW_TASK2");
            }
            continue;
        }

        if (lineLength < sizeof(lineBuffer) - 1) {
            lineBuffer[lineLength++] = character;
        }
    }
}

/**
 * @brief Generates simulated room temperature, oxygen, and AQI values.
 *
 * Values are stored under telemetryMux. A transition into an abnormal facility
 * state signals alertSemaphore for alertTask; these are not physical sensors.
 */
void simulateFacilityData()
{
    FacilityTelemetry nextState = getFacilitySnapshot();

    const float roomDelta = random(-4, 5) * 0.4f;
    const float oxygenDelta = random(-3, 3) * 0.2f;
    const int aqiDelta = random(-12, 18);

    if (simOverrideRoomTemp > -100.0f) {
        nextState.roomTemperature = simOverrideRoomTemp;
    } else {
        nextState.roomTemperature = constrain(22.0f + roomDelta, 18.0f, 30.0f);
    }

    if (simOverrideOxygen > 0.0f) {
        nextState.oxygenLevel = simOverrideOxygen;
    } else {
        nextState.oxygenLevel = constrain(20.8f + oxygenDelta, 18.0f, 24.0f);
    }

    if (simOverrideAqi >= 0) {
        nextState.aqi = simOverrideAqi;
    } else {
        nextState.aqi = constrain(35 + aqiDelta, 0, 200);
    }

    portENTER_CRITICAL(&telemetryMux);
    facilityTelemetry = nextState;
    const bool wasAbnormal = facilityTelemetry.alertActive;
    facilityTelemetry.alertActive = isFacilityAbnormal(facilityTelemetry);
    portEXIT_CRITICAL(&telemetryMux);

    if (facilityTelemetry.alertActive && !wasAbnormal) {
        xSemaphoreGive(alertSemaphore);
    }
}

/**
 * @brief Clears medical and facility accumulators for the next summary window.
 */
void resetSummaryStats()
{
    medicalSummaryStats = MedicalSummaryStats();
    facilitySummaryStats = FacilitySummaryStats();
    medicalSummaryStats.minSpo2 = 100;
}
