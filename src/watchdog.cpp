#include "watchdog.h"
#include "app.h" // For taskNameFromId
#include "esp_task_wdt.h"

static const uint32_t WATCHDOG_TIMEOUT_MS = 6000;
static const uint32_t WATCHDOG_CHECK_INTERVAL_MS = 1000;
static const uint32_t STARTUP_GRACE_PERIOD_MS = 2000;
static const uint32_t HEARTBEAT_MUTEX_WAIT_MS = 50;

TaskHeartbeat taskHeartbeats[TASK_MONITORED_COUNT];

void watchdogInit()
{
    for (int i = 0; i < TASK_MONITORED_COUNT; ++i) {
        taskHeartbeats[i].lastHeartbeatMs = millis();
        taskHeartbeats[i].active = false;
    }
}

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
