/**
 * @file watchdog.cpp
 * @brief Implementation of dual-tier task watchdog and diagnostic supervision.
 *
 * Responsibilities:
 * 1. Hardware Watchdog Interfacing:
 *    - Each monitored FreeRTOS task subscribes to ESP32 Task Watchdog Timer (TWDT).
 *    - Invocations of reportTaskHeartbeat() perform the hardware esp_task_wdt_reset() feed.
 * 2. Software Task Liveness Supervision:
 *    - Maintains a table of alive timestamps (taskHeartbeats[]) synchronized with heartbeatMutex.
 *    - watchdogTask (Priority 4) audits all task timestamps every 1000 ms against a 6000 ms limit.
 * 3. System Health Flagging:
 *    - Sets globalSystemHealthy = false if any task is unresponsive, triggering SYSTEM_DEGRADED.
 * 4. Resource Diagnostics:
 *    - Emits free heap and task stack high-water mark logs every 30 seconds to catch memory leaks
 *      or impending stack overflows early.
 */

#include "watchdog.h"
#include "app.h" // For taskNameFromId
#include "esp_task_wdt.h"

static const uint32_t WATCHDOG_TIMEOUT_MS = 6000;         ///< Maximum allowable silent period before task flagged unresponsive (6 s)
static const uint32_t WATCHDOG_CHECK_INTERVAL_MS = 1000;  ///< Supervisory evaluation period (1 s)
static const uint32_t STARTUP_GRACE_PERIOD_MS = 2000;     ///< Initial boot grace period allowing slow tasks to register (2 s)
static const uint32_t HEARTBEAT_MUTEX_WAIT_MS = 50;       ///< Maximum mutex wait when logging task heartbeat (50 ms)

TaskHeartbeat taskHeartbeats[TASK_MONITORED_COUNT];
static bool globalSystemHealthy = true;

/**
 * @brief Returns whether all monitored tasks are healthy and responsive.
 *
 * @return true if no tasks have missed heartbeats; false otherwise.
 */
bool isSystemHealthy() {
    return globalSystemHealthy;
}

/**
 * @brief Initializes the task heartbeat tracking table.
 */
void watchdogInit()
{
    for (int i = 0; i < TASK_MONITORED_COUNT; ++i) {
        taskHeartbeats[i].lastHeartbeatMs = millis();
        taskHeartbeats[i].active = false;
    }
}

/**
 * @brief Records task heartbeat timestamp and feeds the calling task's hardware TWDT registration.
 *
 * Synchronization & Safety:
 * - Obtains heartbeatMutex with 50 ms timeout to prevent deadlock.
 * - Updates taskHeartbeats[taskId].lastHeartbeatMs with current millis().
 * - Calls esp_task_wdt_reset() for hardware watchdog timer enforcement.
 *
 * @param taskId Enum representing the calling task.
 */
void reportTaskHeartbeat(MonitoredTaskId taskId)
{
    if (taskId < TASK_TEMP || taskId >= TASK_MONITORED_COUNT) {
        return;
    }

    uint32_t now = millis();
    if (heartbeatMutex != NULL && xSemaphoreTake(heartbeatMutex, pdMS_TO_TICKS(HEARTBEAT_MUTEX_WAIT_MS)) == pdTRUE) {
        taskHeartbeats[taskId].lastHeartbeatMs = now;
        if (!taskHeartbeats[taskId].active) {
            taskHeartbeats[taskId].active = true;
            Serial.print("[WATCHDOG] First heartbeat: ");
            Serial.println(taskNameFromId(taskId));
        }
        xSemaphoreGive(heartbeatMutex);
    }

    // TWDT reset is the enforcement point. The heartbeat monitor is only diagnostic logging.
    esp_err_t resetResult = esp_task_wdt_reset();
    if (resetResult != ESP_OK && resetResult != ESP_ERR_INVALID_STATE) {
        Serial.print("[WATCHDOG] TWDT feed failed for ");
        Serial.print(taskNameFromId(taskId));
        Serial.print(": ");
        Serial.println(esp_err_to_name(resetResult));
    }
}

/**
 * @brief FreeRTOS supervisory task running at Priority 4 monitoring task health and memory.
 *
 * @param parameter Unused FreeRTOS task parameter pointer (NULL).
 */
void watchdogTask(void *parameter)
{
    (void)parameter;
    esp_err_t addResult = esp_task_wdt_add(NULL);
    if (addResult != ESP_OK && addResult != ESP_ERR_INVALID_STATE) {
        Serial.print("[WATCHDOG] Failed to subscribe watchdog to self: ");
        Serial.println(esp_err_to_name(addResult));
    }

    Serial.println("[WATCHDOG] Task health monitor active");
    const uint32_t monitoringStartMs = millis();
    uint32_t lastHealthyLogMs = 0;
    uint32_t lastStackLogMs = 0;

    while (true) {
        uint32_t now = millis();
        bool unhealthyFound = false;

        const bool startupGraceActive =
            (now - monitoringStartMs) < STARTUP_GRACE_PERIOD_MS;

        for (int taskIndex = 0; taskIndex < TASK_MONITORED_COUNT; ++taskIndex) {
            TaskHeartbeat heartbeat;
            if (xSemaphoreTake(heartbeatMutex, pdMS_TO_TICKS(HEARTBEAT_MUTEX_WAIT_MS)) == pdTRUE) {
                heartbeat = taskHeartbeats[taskIndex];
                xSemaphoreGive(heartbeatMutex);
            } else {
                Serial.println("[WATCHDOG] Could not read task heartbeat state");
                unhealthyFound = true;
                continue;
            }

            if (!heartbeat.active) {
                if (startupGraceActive) {
                    continue;
                }
                unhealthyFound = true;
                Serial.print("[WATCHDOG] Heartbeat timeout: ");
                Serial.print(taskNameFromId(static_cast<MonitoredTaskId>(taskIndex)));
                Serial.println(" has not reported a heartbeat yet");
                continue;
            }

            if ((now - heartbeat.lastHeartbeatMs) > WATCHDOG_TIMEOUT_MS) {
                unhealthyFound = true;
                Serial.print("[WATCHDOG] Heartbeat timeout: ");
                Serial.print(taskNameFromId(static_cast<MonitoredTaskId>(taskIndex)));
                Serial.print(" missed last heartbeat by ");
                Serial.print(now - heartbeat.lastHeartbeatMs);
                Serial.println(" ms");
            }
        }

        globalSystemHealthy = !unhealthyFound;

        if (!unhealthyFound) {
            if (millis() - lastHealthyLogMs > 10000) {
                Serial.println("[WATCHDOG] All monitored tasks healthy");
                lastHealthyLogMs = millis();
            }
        }

        if (now - lastStackLogMs >= 30000U) {
            lastStackLogMs = now;
            Serial.print("[WATCHDOG] Free heap: ");
            Serial.println(ESP.getFreeHeap());
            Serial.print("[WATCHDOG] Minimum free heap: ");
            Serial.println(ESP.getMinFreeHeap());
            for (int taskIndex = 0; taskIndex < TASK_MONITORED_COUNT; ++taskIndex) {
                if (monitoredHandles[taskIndex] != nullptr) {
                    Serial.print("[WATCHDOG] ");
                    Serial.print(taskNameFromId(static_cast<MonitoredTaskId>(taskIndex)));
                    Serial.print(" stack high-water mark: ");
                    Serial.println(uxTaskGetStackHighWaterMark(monitoredHandles[taskIndex]));
                }
            }
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(WATCHDOG_CHECK_INTERVAL_MS));
    }
}
