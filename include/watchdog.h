#pragma once

#include <Arduino.h>
#include "state.h"

/**
 * @file watchdog.h
 * @brief Dual-tier task supervisory watchdog and health monitoring declarations.
 *
 * Architecture:
 * - Tier 1: ESP32 Hardware Task Watchdog Timer (TWDT) configured for 8000 ms timeout.
 *   Hard lockups trigger hardware panic and automatic microcontroller reset.
 * - Tier 2: Software Heartbeat Monitor running inside watchdogTask at Priority 4.
 *   Tasks report alive timestamps into a thread-safe table. If any task misses its
 *   heartbeat for > 6000 ms, globalSystemHealthy drops false and the system transitions
 *   into SYSTEM_DEGRADED.
 */

/**
 * @brief Initializes the software heartbeat timestamp table for all monitored tasks.
 */
void watchdogInit();

/**
 * @brief FreeRTOS task running at Priority 4 auditing task health and stack utilization.
 *
 * Cycles every 1000 ms:
 * 1. Checks heartbeats of all registered application tasks against 6000 ms threshold.
 * 2. Emits warnings and flags system status if any task becomes unresponsive.
 * 3. Logs heap statistics and task stack high-water marks every 30 seconds.
 * 4. Feeds its own TWDT registration.
 *
 * @param parameter Unused FreeRTOS task parameter pointer (NULL).
 */
void watchdogTask(void *parameter);

/**
 * @brief Reports an alive timestamp for the calling task and resets its hardware TWDT.
 *
 * Safe to invoke from any monitored FreeRTOS task; synchronizes access using heartbeatMutex.
 *
 * @param taskId MonitoredTaskId enum representing the calling task.
 */
void reportTaskHeartbeat(MonitoredTaskId taskId);

/**
 * @brief Returns whether all monitored tasks are currently meeting their heartbeat deadlines.
 *
 * @return true if all tasks healthy; false if one or more tasks missed heartbeats.
 */
bool isSystemHealthy();
