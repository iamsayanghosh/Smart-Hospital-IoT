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
 * - SET_ROOM <temp_C>       : Force room temperature
 * - SET_OXYGEN <percent>    : Force ambient oxygen percentage
 * - SET_AQI <index>         : Force Air Quality Index
 * - RESET_FACILITY          : Reset facility parameters to baseline normal values
 * - SHOW_TASK2              : Print feed layout for Dual-Role monitoring system
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
 * @brief Resets rolling statistics accumulators for medical and facility summary payloads.
 *
 * Clears sample counts, running sums, and resets minimum SpO2 tracking to 100%.
 * Called every 60 seconds after hospital summary publication via MQTT.
 */
void resetSummaryStats();

#endif
