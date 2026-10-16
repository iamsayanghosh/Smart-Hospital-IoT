#pragma once
#include <Arduino.h>

/**
 * @file sampling.h
 * @brief Smart Dynamic Sampling Rate (Adaptive Monitoring) task declarations (Task 5).
 *
 * This module declares samplingTask, which dynamically arbitrates sensor acquisition
 * intervals between energy-efficient monitoring (60 s) and acute response monitoring (5 s).
 *
 * Architecture & Constraints:
 * - Dynamic Tasks: Controls acquisition intervals for temperatureTask and vitalTask
 *   via the shared samplingPeriods structure.
 * - Fixed Tasks: ecgTask (fixed 200 ms cadence for clinical waveform continuity) and
 *   motionTask (fixed 1000 ms cadence for occupancy detection) are excluded from adaptive scaling.
 * - Automatic-First Priority: Automatic clinical assessment overrides manual doctor settings
 *   upon any transition in the patient's condition.
 */

/**
 * @brief FreeRTOS supervisory task arbitrating dynamic sensor sampling rates.
 *
 * Operational Mechanics:
 * - Automatic Targets: 60 s for clinically stable patients; 5 s for abnormal patients.
 * - Manual Override: Ingests doctor-commanded intervals [5 - 60 s] via samplingQueue from MQTT.
 * - Priority Rule: Manual overrides persist ONLY while the patient's clinical state remains constant.
 *   Any transition between STABLE and ABNORMAL restores the automatic clinical target.
 * - Startup Grace Period: Applies conservative 5 s acquisition while initial vitals and ECG are settling.
 * - Latency Limitation: In 60 s stable mode, detection of newly emerging anomalies occurs on the next
 *   scheduled sensor acquisition cycle.
 *
 * @param parameter Unused FreeRTOS task parameter pointer (NULL).
 */
void samplingTask(void *parameter);
