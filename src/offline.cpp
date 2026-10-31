/**
 * @file offline.cpp
 * @brief Implementation of Advanced Offline Detection and Telemetry Buffering (Task 6).
 *
 * Responsibilities:
 * 1. Health Monitoring: Polls Wi-Fi link status, Adafruit IO MQTT connection state, and
 *    all task watchdog heartbeats to maintain a unified SystemHealthState.
 * 2. Telemetry Persistence: Buffers outgoing MQTT messages in a dedicated 64-item FreeRTOS queue
 *    when network connectivity is interrupted.
 * 3. Controlled Drain: When the broker connection is restored, mqttTask reads buffered messages
 *    using peek/dequeue operations, sharing the 3000 ms telemetry slot with live feeds (max 20 msgs/min combined)
 *    to maintain nominal pacing toward Adafruit IO quotas (subject to the 64-item buffer limit).
 * 4. Acoustic Indication: Triggers distinct buzzer cadence patterns when network links fail,
 *    subordinate to medication critical alarms.
 */

#include "offline.h"
#include "outputs.h"
#include "connectivity.h"
#include "watchdog.h"
#include "mqtt.h"
#include "esp_task_wdt.h"
#include "offline_banner_gate.h"

static SystemHealthState currentHealth = SYSTEM_ONLINE;       ///< Current overall system infrastructure status
static OfflineBuzzerMode currentBuzzer = BUZZER_MODE_NORMAL; ///< Current acoustic cadence demanded by network state
static volatile bool offlineAlertConfirmed = false;

bool isOfflineAlertConfirmed() {
    return offlineAlertConfirmed;
}
static QueueHandle_t offlineBufferQueue = NULL;              ///< Thread-safe FreeRTOS queue storing buffered outbound telemetry

/**
 * @brief Retrieves the active infrastructure health state.
 *
 * @return SystemHealthState Current state: SYSTEM_ONLINE, SYSTEM_DEGRADED, or SYSTEM_OFFLINE.
 */
SystemHealthState getSystemHealthState() {
    return currentHealth;
}

/**
 * @brief Retrieves the active network buzzer cadence pattern.
 *
 * @return OfflineBuzzerMode Active cadence: BUZZER_MODE_NORMAL, BUZZER_MODE_WIFI_FAIL, or BUZZER_MODE_MQTT_FAIL.
 */
OfflineBuzzerMode getOfflineBuzzerMode() {
    return currentBuzzer;
}

/**
 * @brief Evaluates connectivity and watchdog health inputs to update system status and buzzer demand.
 *
 * Decision Logic:
 * - If Wi-Fi is disconnected: System is OFFLINE; cadence is BUZZER_MODE_WIFI_FAIL (double beep).
 * - Else if MQTT is disconnected: System is OFFLINE; cadence is BUZZER_MODE_MQTT_FAIL (single slow beep).
 * - Else if any monitored task is unresponsive: System is DEGRADED; buzzer is silent.
 * - Else: System is ONLINE; buzzer is silent.
 *
 * @param wifiConnected True if Wi-Fi STA is connected.
 * @param mqttConnected True if MQTT client is connected to broker.
 * @param systemHealthy True if all monitored tasks reported heartbeats within the watchdog window.
 */
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

/**
 * @brief Allocates the 64-item FreeRTOS queue for offline telemetry storage.
 */
void initOfflineBuffer() {
    offlineBufferQueue = xQueueCreate(64, sizeof(BufferedTelemetryMessage));
}

/**
 * @brief Enqueues an outgoing telemetry message during network outages.
 *
 * Buffer Policy:
 * - If the queue has space, copies topic and payload into a BufferedTelemetryMessage and enqueues.
 * - If the queue is full (64 items), drops the new message with a diagnostic log to protect memory.
 *
 * @param topic Target MQTT feed topic.
 * @param payload Telemetry payload string.
 * @return true if successfully queued; false if dropped or queue uninitialized.
 */
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

/**
 * @brief Dequeues the oldest buffered message for transmission to MQTT.
 *
 * @param msg Output pointer where the popped message will be copied.
 * @return true if an item was successfully removed; false if queue was empty or uninitialized.
 */
bool dequeueBufferedTelemetry(BufferedTelemetryMessage* msg) {
    if (offlineBufferQueue == NULL) return false;
    return (xQueueReceive(offlineBufferQueue, msg, 0) == pdTRUE);
}

/**
 * @brief Reads the oldest buffered message without removing it from the queue.
 *
 * Used by mqttTask to attempt transmission first; only upon confirmed publish
 * is dequeueBufferedTelemetry called to permanently consume the record.
 *
 * @param msg Output pointer where the message is previewed.
 * @return true if an item was available to peek; false if queue was empty.
 */
bool peekBufferedTelemetry(BufferedTelemetryMessage* msg) {
    if (offlineBufferQueue == NULL) return false;
    return (xQueuePeek(offlineBufferQueue, msg, 0) == pdTRUE);
}

/**
 * @brief Returns the count of messages currently queued in the offline buffer.
 *
 * @return size_t Number of pending messages (0 to 64).
 */
size_t getBufferedTelemetryCount() {
    if (offlineBufferQueue == NULL) return 0;
    return uxQueueMessagesWaiting(offlineBufferQueue);
}

/**
 * @brief FreeRTOS task supervising connectivity states and driving offline buzzer cadences.
 *
 * Cycle Execution (every 50 ms):
 * 1. Polls Wi-Fi link state (WiFi.status() == WL_CONNECTED).
 * 2. Polls Adafruit IO MQTT connection state (mqttClient.connected()).
 * 3. Polls task watchdog health table (isSystemHealthy()).
 * 4. Updates system health state and selects buzzer cadence.
 * 5. Calls updateOfflineBuzzer() to generate auditory alert patterns if needed.
 * 6. Reports task heartbeat to watchdog and yields with vTaskDelay(50 ms).
 *
 * @param parameter Unused FreeRTOS task parameter pointer (NULL).
 */
void offlineTask(void *parameter) {
    (void)parameter;
    esp_task_wdt_add(NULL);
    Serial.println("[WATCHDOG] OFFLINE task registered with TWDT");

    OfflineBannerGate buzzerGate(OFFLINE_HOLD_DEBOUNCE_MS, OFFLINE_BOOT_GRACE_MS);

    while (true) {
        bool wifi = (WiFi.status() == WL_CONNECTED);
        bool mqtt = isMqttClientConnected();
        bool healthy = isSystemHealthy();
        
        updateSystemHealth(wifi, mqtt, healthy);
        
        // Suppress buzzer and banner during initial boot grace (10s) and transient hiccups (<5s)
        offlineAlertConfirmed = buzzerGate.update(!wifi || !mqtt, millis());
        OfflineBuzzerMode effectiveBuzzer = offlineAlertConfirmed ? currentBuzzer : BUZZER_MODE_NORMAL;
        
        updateOfflineBuzzer(effectiveBuzzer);
        
        reportTaskHeartbeat(TASK_OFFLINE);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
