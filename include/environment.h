#ifndef ENVIRONMENT_H
#define ENVIRONMENT_H

/**
 * @file environment.h
 * @brief Facility environmental simulation and serial testing command declarations (Task 2).
 *
 * This module declares facilities for Task 2 (Dual-Role IoT Monitoring System),
 * providing simulated hospital room telemetry (ambient temperature, room oxygen level,
 * air quality index) and serial test injection hooks.
 */

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
void handleSerialCommands();

/**
 * @brief Generates simulated facility environment readings with random perturbations.
 *
 * Perturbs room temperature (18-30 °C), oxygen level (18-24 %), and AQI (0-200),
 * writes the updated state under telemetryMux, evaluates isFacilityAbnormal(),
 * and signals alertSemaphore upon transition into an abnormal environmental state.
 */
void simulateFacilityData();

/**
 * @brief FreeRTOS task running dedicated facility environment simulation at 1 Hz.
 * @param parameter Unused FreeRTOS task parameter pointer (NULL).
 */
void environmentTask(void *parameter);

/**
 * @brief Resets rolling statistics accumulators for medical and facility summary payloads.
 *
 * Clears sample counts, running sums, and resets minimum SpO2 tracking to 100%.
 * Called every 60 seconds after hospital summary publication via MQTT.
 */
void resetSummaryStats();

#endif
