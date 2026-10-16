#ifndef SENSORS_H
#define SENSORS_H

#include <Arduino.h>

/**
 * @file sensors.h
 * @brief Multi-sensor acquisition and physiological data simulation task declarations (Task 1).
 *
 * This module declares initialization and FreeRTOS task entry functions for all
 * medical and telemetry sensing sources:
 * - Physical DS18B20 1-Wire Digital Temperature Sensor (GPIO 4)
 * - Physical PIR Motion Detector (GPIO 27)
 * - Simulated Photoplethysmogram (MAX30102 Heart Rate & SpO2)
 * - Simulated Electrocardiogram (AD8232 PQRST waveform generator)
 *
 * Dynamic vs. Fixed Cadences:
 * - Temperature and Vital tasks adaptively alter their acquisition schedules (5 s to 60 s)
 *   via samplingPeriods (Task 5 Adaptive Monitoring).
 * - ECG (200 ms) and Motion (1000 ms) tasks operate on dedicated fixed schedules.
 */

/**
 * @brief Configures physical GPIO pins and initializes sensor driver libraries.
 *
 * Configures PIR pin (GPIO 27) as INPUT and initializes the DallasTemperature
 * 1-Wire bus instance on GPIO 4.
 */
void sensorsInit();

/**
 * @brief FreeRTOS task acquiring physical patient temperature via DS18B20.
 *
 * Operates on adaptive schedule (5 s - 60 s) while executing a 500 ms watchdog
 * heartbeat loop to prevent TWDT starvation during extended 60 s idle periods.
 *
 * @param parameter Unused FreeRTOS task parameter pointer (NULL).
 */
void temperatureTask(void *parameter);

/**
 * @brief FreeRTOS task acquiring binary patient motion from physical PIR sensor.
 *
 * Samples GPIO 27 every 1000 ms (Priority 2) and dispatches MOTION_DATA to sensorQueue.
 *
 * @param parameter Unused FreeRTOS task parameter pointer (NULL).
 */
void motionTask(void *parameter);

/**
 * @brief FreeRTOS task generating simulated SpO2 and Heart Rate physiological vitals.
 *
 * Operates on adaptive schedule (5 s - 60 s) with a 500 ms heartbeat polling cadence.
 * Also invokes simulateFacilityData() to advance ambient room conditions in lockstep.
 *
 * @param parameter Unused FreeRTOS task parameter pointer (NULL).
 */
void vitalTask(void *parameter);

/**
 * @brief FreeRTOS task synthesizing continuous diagnostic PQRST ECG waveform data.
 *
 * Runs at fixed cadence (5 Hz / 200 ms period, Priority 3) calculating mathematical
 * Gaussian curve models of cardiac electrophysiology.
 *
 * @param parameter Unused FreeRTOS task parameter pointer (NULL).
 */
void ecgTask(void *parameter);

#endif
