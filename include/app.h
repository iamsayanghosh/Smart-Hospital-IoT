#pragma once

#include <Arduino.h>
#include "state.h"

/**
 * @file app.h
 * @brief Core application initialization, FreeRTOS task spawning, and system supervisory declarations.
 *
 * This module coordinates the centralized creation of FreeRTOS IPC objects
 * (queues, mutexes, semaphores), telemetry state baseline initialization,
 * hardware watchdog timer (TWDT) configuration, and task creation pinned to Core 1.
 */

/**
 * @brief Allocates and initializes all FreeRTOS synchronization primitives and telemetry states.
 *
 * Allocates:
 * - sensorQueue (depth 16, SensorMessage)
 * - samplingQueue (depth 1, uint32_t, overwrite semantics)
 * - bedQueue (depth 1, float, overwrite semantics)
 * - displayMutex (standard mutex)
 * - alertSemaphore (binary semaphore)
 * - heartbeatMutex (standard mutex)
 *
 * Also initializes the offline circular ring buffer and resets baseline telemetry structs.
 * Halts the system via failSetup() if any IPC allocation fails.
 */
void appInit();

/**
 * @brief Configures ESP32 Task Watchdog Timer (TWDT) and spawns all FreeRTOS tasks on Core 1.
 *
 * Sets up the 8-second TWDT configuration and creates:
 * - watchdogTask (priority 4)
 * - vitalTask, ecgTask, medicationTask (priority 3)
 * - temperatureTask, motionTask, mqttTask, alertTask, samplingTask, bedTask, offlineTask (priority 2)
 * - displayTask (priority 1)
 */
void appStartTasks();

/**
 * @brief Halts execution safely upon an unrecoverable initialization error.
 *
 * Logs the error message over Serial and enters an infinite non-terminating loop
 * with periodic vTaskDelay() yields, preventing corrupted operation.
 *
 * @param message Description of the initialization failure.
 */
void failSetup(const char *message);

/**
 * @brief Retrieves the active sensor acquisition interval for a monitored task.
 *
 * Queries samplingPeriods struct protected by telemetryMux critical section.
 *
 * @param taskId Monitored task identifier (e.g. TASK_TEMP, TASK_VITAL, TASK_ECG).
 * @return uint32_t Active period in milliseconds (0 if unsupported).
 */
uint32_t getSamplePeriodMs(MonitoredTaskId taskId);

/**
 * @brief Maps a MonitoredTaskId enum to a human-readable text string.
 *
 * @param taskId Monitored task identifier enum.
 * @return const char* String representation (e.g. "TEMP", "VITAL", "MQTT").
 */
const char *taskNameFromId(MonitoredTaskId taskId);

/**
 * @brief Records an alive timestamp for the calling task in the watchdog heartbeat table.
 *
 * Safe to call from any monitored FreeRTOS task; synchronizes access using heartbeatMutex.
 *
 * @param taskId Enum representing the reporting task.
 */
void reportTaskHeartbeat(MonitoredTaskId taskId);
