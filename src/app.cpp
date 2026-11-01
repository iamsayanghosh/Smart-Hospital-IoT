/**
 * @file app.cpp
 * @brief Core application initialization, FreeRTOS task spawning, and system supervisory implementation.
 *
 * Architecture Notes:
 * - IPC Allocation: Allocates all FreeRTOS queues, mutexes, and semaphores before tasks run.
 * - Core Pinning: All 13 application tasks are explicitly pinned to Core 1 (xTaskCreatePinnedToCore
 *   with coreID=1). In the installed PlatformIO toolchain build configuration (sdkconfig.h), the Wi-Fi
 *   driver task (CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0=1) and lwIP TCP/IP stack (CONFIG_LWIP_TCPIP_TASK_AFFINITY=0x0)
 *   are configured for Core 0, the FreeRTOS timer service task (CONFIG_FREERTOS_TIMER_SERVICE_TASK_CORE_AFFINITY=0x7FFFFFFF)
 *   is configured with tskNO_AFFINITY (allowing either core), and Arduino loopTask is configured for Core 1
 *   (CONFIG_ARDUINO_RUNNING_CORE=1). Dynamic runtime scheduling on physical hardware was not instrumented.
 *   Pinning application tasks to Core 1 organizes application workload, but does not imply exclusive
 *   reservation of Core 0, complete network isolation, or immunity from shared bus/cache contention
 *   and interrupt preemption.
 * - Priority Hierarchy:
 *     Priority 4: watchdogTask (highest application priority to guarantee timely health checks)
 *     Priority 3: vitalTask, ecgTask, medicationTask (high-rate physiological acquisition & safety-critical dosage control)
 *     Priority 2: temperatureTask, motionTask, mqttTask, alertTask, samplingTask, bedTask, offlineTask, environmentTask (standard processing)
 *     Priority 1: displayTask (lowest priority background UI rendering)
 */

#include "app.h"
#include "watchdog.h"
#include "display.h"
#include "esp_task_wdt.h"

#include "sensors.h"
#include "environment.h"
#include "alerts.h"
#include "sampling.h"

#include "mqtt.h"

#include "medication.h"
#include "bed.h"
#include "offline.h"

static const uint32_t TWDT_TIMEOUT_MS = 8000;
static const uint32_t HEARTBEAT_MUTEX_WAIT_MS = 50;

/**
 * @brief Retrieves the active sensor acquisition interval for a monitored task.
 *
 * @param taskId Monitored task identifier (TASK_TEMP, TASK_VITAL, or TASK_ECG).
 * @return uint32_t Active period in milliseconds, or 0 if taskId has no dynamic period.
 */
uint32_t getSamplePeriodMs(MonitoredTaskId taskId)
{
    uint32_t periodMs = 0;
    portENTER_CRITICAL(&telemetryMux);
    switch (taskId) {
        case TASK_TEMP:
            periodMs = samplingPeriods.temperatureMs;
            break;
        case TASK_VITAL:
            periodMs = samplingPeriods.vitalMs;
            break;
        case TASK_ECG:
            periodMs = samplingPeriods.ecgMs;
            break;
        default:
            break;
    }
    portEXIT_CRITICAL(&telemetryMux);
    return periodMs;
}

/**
 * @brief Maps a monitored task identifier to its human-readable debug tag.
 *
 * @param taskId Monitored task enum identifier.
 * @return const char* Corresponding name string.
 */
const char *taskNameFromId(MonitoredTaskId taskId)
{
    switch (taskId) {
        case TASK_TEMP:
            return "TEMP";
        case TASK_MOTION:
            return "MOTION";
        case TASK_VITAL:
            return "VITAL";
        case TASK_ECG:
            return "ECG";
        case TASK_MQTT:
            return "MQTT";
        case TASK_DISPLAY:
            return "DISPLAY";
        case TASK_ALERT:
            return "ALERT";
        case TASK_MEDICATION:
            return "MEDICATION";
        case TASK_SAMPLING:
            return "SAMPLING";
        case TASK_BED:
            return "BED";
        case TASK_OFFLINE:
            return "OFFLINE";
        case TASK_ENVIRONMENT:
            return "ENVIRONMENT";
        default:
            return "UNKNOWN";
    }
}

/**
 * @brief Halts the system in a safe infinite loop upon critical initialization failure.
 *
 * Emits the failure message to Serial and yields with vTaskDelay() to prevent
 * watchdog starvation or hard faults while holding execution.
 *
 * @param message Diagnostic description of the fatal initialization failure.
 */
void failSetup(const char *message)
{
    Serial.println(message);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/**
 * @brief Helper function to pin a task to Core 1 and check creation status.
 *
 * @param function Entry function for the FreeRTOS task.
 * @param name Human-readable diagnostic name for the task.
 * @param priority FreeRTOS task priority (1 = lowest, 4 = highest).
 * @param taskHandle Output pointer to store the generated FreeRTOS TaskHandle_t.
 * @param stackSize Stack depth in bytes (defaults to 4096; MQTT uses 8192).
 * @return true if task creation succeeded; false otherwise.
 */
static bool createTask(TaskFunction_t function, const char *name, UBaseType_t priority, TaskHandle_t *taskHandle, uint32_t stackSize = 4096)
{
    BaseType_t result = xTaskCreatePinnedToCore(
        function, name, stackSize, NULL, priority, taskHandle, 1);
    if (result != pdPASS) {
        Serial.print("[SETUP] Task creation failed: ");
        Serial.println(name);
        return false;
    }
    Serial.print("[SETUP] Started ");
    Serial.println(name);
    return true;
}

/**
 * @brief Creates all system queues, mutexes, semaphores, and resets baseline state.
 *
 * IPC objects created:
 * - sensorQueue: Queue for decoupling sensor acquisition tasks from displayTask.
 * - samplingQueue: Single-element queue with overwrite semantics for MQTT rate updates.
 * - bedQueue: Single-element queue with overwrite semantics for MQTT bed angle commands.
 * - displayMutex: Protects SSD1306 I2C bus transactions.
 * - alertSemaphore: Binary semaphore for notifying alertTask of clinical/facility transitions.
 * - heartbeatMutex: Mutex protecting the monitored task heartbeat timestamp table.
 */
void appInit()
{
    // Create the queue, display mutex, alert semaphore, and heartbeat mutex.
    sensorQueue = xQueueCreate(16, sizeof(SensorMessage));
    if (sensorQueue == NULL) {
        failSetup("[SETUP] Queue creation failed");
    }
    samplingQueue = xQueueCreate(1, sizeof(uint32_t));
    if (samplingQueue == NULL) {
        failSetup("[SETUP] Sampling queue creation failed");
    }
    bedQueue = xQueueCreate(1, sizeof(float));
    if (bedQueue == NULL) {
        failSetup("[SETUP] Bed queue creation failed");
    }
    displayMutex = xSemaphoreCreateMutex();
    if (displayMutex == NULL) {
        failSetup("[SETUP] Display mutex creation failed");
    }
    alertSemaphore = xSemaphoreCreateBinary();
    if (alertSemaphore == NULL) {
        failSetup("[SETUP] Alert semaphore creation failed");
    }
    heartbeatMutex = xSemaphoreCreateMutex();
    if (heartbeatMutex == NULL) {
        failSetup("[SETUP] Heartbeat mutex creation failed");
    }
    watchdogInit();
    initOfflineBuffer();
    Serial.println("[SETUP] Queues, mutexes, and offline buffer created");

    // Initialize thresholds, simulation state, telemetry, and summary windows.
    medicalThresholds = MedicalThresholdConfig();
    facilityThresholds = FacilityThresholdConfig();
    facilityTelemetry = FacilityTelemetry();
    telemetry = Telemetry();
    medicationDosageControl = MedicationDosageControl();
}

/**
 * @brief Configures ESP32 Task Watchdog Timer (TWDT) and launches all FreeRTOS tasks.
 *
 * Configures TWDT with an 8000 ms timeout and enables Core 1 idle task monitoring
 * only (.idle_core_mask = (1 << 1)), with panic triggered on expiration.
 * All 13 application tasks are explicitly pinned to Core 1 with calibrated priorities and stacks.
 */
void appStartTasks()
{
    // Configure the ESP Task Watchdog before starting monitored tasks.
    esp_task_wdt_config_t wdtConfig = {
        .timeout_ms = TWDT_TIMEOUT_MS,
        .idle_core_mask = (1 << 1),
        .trigger_panic = true
    };
    esp_err_t wdtResult = esp_task_wdt_init(&wdtConfig);
    bool watchdogReconfigured = false;
    if (wdtResult == ESP_ERR_INVALID_STATE) {
        Serial.println("[SETUP] TWDT already initialized; reconfiguring to 8 s");
        watchdogReconfigured = true;
        wdtResult = esp_task_wdt_reconfigure(&wdtConfig);
    }
    if (wdtResult != ESP_OK) {
        Serial.print("[SETUP] Watchdog initialization failed: ");
        Serial.println(esp_err_to_name(wdtResult));
        failSetup("[SETUP] System halted for safe failure");
    }
    if (watchdogReconfigured) {
        Serial.println("[SETUP] ESP Task Watchdog configured with 8 s timeout");
    } else {
        Serial.println("[SETUP] ESP Task Watchdog initialized with 8 s timeout");
    }

    // Pin every task to Core 1. Actual priorities are watchdog 4, vital/ECG/
    // medication 3, temperature/motion/MQTT/alert/sampling/bed/offline/environment 2, and display 1.
    bool tasksStarted =
        createTask(watchdogTask, "Watchdog Task", 4, &watchdogHandle) &&
        createTask(vitalTask, "Vital Task", 3, &monitoredHandles[TASK_VITAL]) &&
        createTask(ecgTask, "ECG Task", 3, &monitoredHandles[TASK_ECG]) &&
        createTask(temperatureTask, "Temperature Task", 2, &monitoredHandles[TASK_TEMP]) &&
        createTask(motionTask, "Motion Task", 2, &monitoredHandles[TASK_MOTION]) &&
        createTask(mqttTask, "MQTT Task", 2, &monitoredHandles[TASK_MQTT], 8192) &&
        createTask(alertTask, "Alert Task", 2, &monitoredHandles[TASK_ALERT]) &&
        createTask(medicationTask, "Medication Task", 3, &monitoredHandles[TASK_MEDICATION]) &&
        createTask(samplingTask, "Sampling Task", 2, &monitoredHandles[TASK_SAMPLING]) &&
        createTask(bedTask, "Bed Task", 2, &monitoredHandles[TASK_BED]) &&
        createTask(offlineTask, "Offline Task", 2, &monitoredHandles[TASK_OFFLINE]) &&
        createTask(environmentTask, "Environment Task", 2, &monitoredHandles[TASK_ENVIRONMENT]) &&
        createTask(displayTask, "Display Task", 1, &monitoredHandles[TASK_DISPLAY]);
    if (!tasksStarted) {
        failSetup("[SETUP] One or more tasks failed to start");
    }

    Serial.println("[SETUP] All FreeRTOS tasks started on Core 1");
}
