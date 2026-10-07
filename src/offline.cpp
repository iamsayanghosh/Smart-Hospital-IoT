#include "offline.h"
#include "outputs.h"
#include "connectivity.h"
#include "watchdog.h"
#include "mqtt.h"
#include "esp_task_wdt.h"

static SystemHealthState currentHealth = SYSTEM_ONLINE;
static OfflineBuzzerMode currentBuzzer = BUZZER_MODE_NORMAL;
static QueueHandle_t offlineBufferQueue = NULL;

SystemHealthState getSystemHealthState() {
    return currentHealth;
}

OfflineBuzzerMode getOfflineBuzzerMode() {
    return currentBuzzer;
}

void updateSystemHealth(bool wifiConnected, bool mqttConnected, bool systemHealthy) {
    if (!wifiConnected) {
        currentHealth = SYSTEM_OFFLINE;
        currentBuzzer = BUZZER_MODE_WIFI_FAIL;
    } else if (!mqttConnected) {
        currentHealth = SYSTEM_OFFLINE;
        currentBuzzer = BUZZER_MODE_MQTT_FAIL;
    } else if (!systemHealthy) {
        currentHealth = SYSTEM_DEGRADED;
        currentBuzzer = BUZZER_MODE_NORMAL;
    } else {
        currentHealth = SYSTEM_ONLINE;
        currentBuzzer = BUZZER_MODE_NORMAL;
    }
}

void initOfflineBuffer() {
    offlineBufferQueue = xQueueCreate(64, sizeof(BufferedTelemetryMessage));
}

bool enqueueBufferedTelemetry(const char* topic, const char* payload) {
    if (offlineBufferQueue == NULL) return false;
    BufferedTelemetryMessage msg;
    strncpy(msg.topic, topic, sizeof(msg.topic) - 1);
    msg.topic[sizeof(msg.topic) - 1] = '\0';
    strncpy(msg.payload, payload, sizeof(msg.payload) - 1);
    msg.payload[sizeof(msg.payload) - 1] = '\0';
    
    if (xQueueSend(offlineBufferQueue, &msg, 0) != pdTRUE) {
        Serial.println("[OFFLINE] Buffer full, dropping new telemetry");
        return false;
    }
    return true;
}

bool dequeueBufferedTelemetry(BufferedTelemetryMessage* msg) {
    if (offlineBufferQueue == NULL) return false;
    return (xQueueReceive(offlineBufferQueue, msg, 0) == pdTRUE);
}

bool peekBufferedTelemetry(BufferedTelemetryMessage* msg) {
    if (offlineBufferQueue == NULL) return false;
    return (xQueuePeek(offlineBufferQueue, msg, 0) == pdTRUE);
}

size_t getBufferedTelemetryCount() {
    if (offlineBufferQueue == NULL) return 0;
    return uxQueueMessagesWaiting(offlineBufferQueue);
}

void offlineTask(void *parameter) {
    (void)parameter;
    esp_task_wdt_add(NULL);
    Serial.println("[WATCHDOG] OFFLINE task registered with TWDT");

    while (true) {
        bool wifi = (WiFi.status() == WL_CONNECTED);
        bool mqtt = mqttClient.connected();
        bool healthy = isSystemHealthy();
        
        updateSystemHealth(wifi, mqtt, healthy);
        
        updateOfflineBuzzer(currentBuzzer);
        
        reportTaskHeartbeat(TASK_OFFLINE);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
