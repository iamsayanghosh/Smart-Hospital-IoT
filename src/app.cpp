#include "app.h"
#include "watchdog.h"
#include "display.h"
#include "esp_task_wdt.h"

#include "sensors.h"
#include "alerts.h"
#include "sampling.h"

// Forward declare tasks from main.cpp that will be moved in later phases
extern void mqttTask(void *parameter);

#include "medication.h"
#include "bed.h"
#include "offline.h"

static const uint32_t TWDT_TIMEOUT_MS = 8000;
static const uint32_t HEARTBEAT_MUTEX_WAIT_MS = 50;

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
        default:
            return "UNKNOWN";
    }
}



void failSetup(const char *message)
{
    Serial.println(message);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

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
    // medication 3, temperature/motion/MQTT/alert 2, and display 1.
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
        createTask(displayTask, "Display Task", 1, &monitoredHandles[TASK_DISPLAY]);
    if (!tasksStarted) {
        failSetup("[SETUP] One or more tasks failed to start");
    }

    Serial.println("[SETUP] All FreeRTOS tasks started on Core 1");
}
