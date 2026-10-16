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
 *     are breached (Room Temp <19 or >27 °C, O2 <19.5 or >23.5 %, AQI >100).
 * - Diagnostic Command Interface:
 *     Processes interactive UART commands for testing boundary conditions in Wokwi simulator.
 */

#include "environment.h"
#include <Arduino.h>
#include "state.h"

/**
 * @brief Processes serial commands for facility telemetry simulation/testing.
 *
 * Supported commands:
 * - SET_ROOM <C>        : Forces simulated room temperature in degrees Celsius
 * - SET_OXYGEN <percent>: Forces simulated room oxygen percentage [16.0 - 25.0 %]
 * - SET_AQI <value>     : Forces simulated air quality index [0 - 250]
 * - RESET_FACILITY      : Restores default nominal facility parameters
 * - SHOW_TASK2          : Prints topic mapping for Task 2 dual-role monitoring
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

    nextState.roomTemperature = constrain(22.0f + roomDelta, 18.0f, 30.0f);
    nextState.oxygenLevel = constrain(20.8f + oxygenDelta, 18.0f, 24.0f);
    nextState.aqi = constrain(35 + aqiDelta, 0, 200);

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
