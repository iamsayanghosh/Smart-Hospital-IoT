#ifndef ALERTS_H
#define ALERTS_H

#include <Arduino.h>

/**
 * @file alerts.h
 * @brief Asynchronous medical and facility alert logging task declaration.
 *
 * This module declares alertTask, an asynchronous FreeRTOS consumer that
 * activates whenever a clinical or environmental threshold transition occurs.
 *
 * Scope and Separation:
 * - Subsystem: Patient clinical and facility anomaly event logging.
 * - Independence: This task handles Serial event logging. Physical warning
 *   hardware (LEDs and buzzer) is owned exclusively by outputControlTask
 *   (Task 3: Medication Dosage Safety) and is NOT driven by this task.
 */

/**
 * @brief FreeRTOS task that waits for alert signals and logs abnormal snapshots.
 *
 * Blocks on alertSemaphore with a 500 ms timeout so it can regularly report its
 * watchdog heartbeat. When unblocked by a newly detected clinical or facility
 * abnormal state, it retrieves synchronized snapshots of both medical and
 * facility telemetry and emits a diagnostic alert log over UART.
 *
 * @param parameter Unused FreeRTOS task parameter pointer (NULL).
 */
void alertTask(void *parameter);

#endif
