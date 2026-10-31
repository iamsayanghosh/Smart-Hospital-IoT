#pragma once
#include <Arduino.h>

/**
 * @file offline.h
 * @brief Advanced Offline Detection, Telemetry Buffering, and Health Supervision declarations (Task 6).
 *
 * Responsibilities:
 * - Detects Wi-Fi network disconnections, MQTT broker outages, and task watchdog failures.
 * - Stores outgoing telemetry messages into a thread-safe FreeRTOS queue buffer (depth 64)
 *   while the node is disconnected from the Adafruit IO broker.
 * - Replays buffered messages in strict FIFO order upon network restoration.
 * - Signals offline acoustic cadences (double-beep for Wi-Fi loss, single slow beep for MQTT loss)
 *   via the shared buzzer driver.
 *
 * SYSTEM HEALTH VS. PATIENT HEALTH DISTINCTION:
 * - SystemHealthState reflects hardware, transport, and runtime task health.
 * - Patient medical state (Telemetry.alertActive) reflects clinical vitals.
 * - These two state machines are completely decoupled.
 */

/**
 * @brief Infrastructure operational health states.
 */
enum SystemHealthState {
    SYSTEM_ONLINE,   ///< Wi-Fi connected, MQTT connected, all task heartbeats healthy
    SYSTEM_DEGRADED, ///< Transport online, but one or more monitored tasks missed watchdog heartbeats
    SYSTEM_OFFLINE   ///< Wi-Fi or MQTT disconnected; telemetry buffering activated
};

/**
 * @brief Acoustic cadence patterns for network failure alerts.
 */
enum OfflineBuzzerMode {
    BUZZER_MODE_NORMAL,    ///< Silent network operation (allows medication alarms through)
    BUZZER_MODE_WIFI_FAIL,  ///< Rapid double-pip cadence (100 ms on, 100 ms off, 100 ms on, 700 ms off)
    BUZZER_MODE_MQTT_FAIL   ///< Slow single-beep cadence (500 ms on, 500 ms off)
};

/**
 * @brief Fixed-size container for buffering outbound telemetry messages during offline periods.
 *
 * Sized to 512 bytes to accommodate full 60-second hospital summary reports (~340-400 bytes)
 * without truncation during offline queue storage and replay.
 */
struct BufferedTelemetryMessage {
    char topic[64];     ///< Destination Adafruit IO feed topic string
    char payload[512];  ///< Formatted telemetry, alert, or full hospital summary payload
};

/**
 * @brief Returns the current system health classification.
 *
 * @return SystemHealthState ONLINE, DEGRADED, or OFFLINE.
 */
SystemHealthState getSystemHealthState();

/**
 * @brief Queries whether the system is confirmed offline after boot grace (10s) and debounce hold (5s).
 *
 * Single canonical gate shared by both the buzzer (offline.cpp) and OLED banner (display.cpp).
 *
 * @return true if offline status is confirmed and active.
 */
bool isOfflineAlertConfirmed();

/**
 * @brief Evaluates transport and task watchdog inputs to update system health and buzzer cadence.
 *
 * @param wifiConnected True if Wi-Fi link is active (WL_CONNECTED).
 * @param mqttConnected True if MQTT client is connected to broker.
 * @param systemHealthy True if all monitored tasks have reported recent heartbeats.
 */
void updateSystemHealth(bool wifiConnected, bool mqttConnected, bool systemHealthy);

/**
 * @brief Returns the active network alert buzzer cadence mode.
 *
 * @return OfflineBuzzerMode Active cadence pattern.
 */
OfflineBuzzerMode getOfflineBuzzerMode();

/**
 * @brief Enqueues an outgoing telemetry message into the offline ring buffer.
 *
 * If the buffer (queue depth 64) is full, the message is dropped and an error logged,
 * preserving existing stored records.
 *
 * @param topic Target MQTT topic.
 * @param payload Telemetry payload string.
 * @return true if enqueued; false if buffer was full or uninitialized.
 */
bool enqueueBufferedTelemetry(const char* topic, const char* payload);

/**
 * @brief Removes and returns the oldest buffered message from the offline queue.
 *
 * @param msg Output pointer to store the retrieved message.
 * @return true if a message was dequeued; false if the buffer was empty.
 */
bool dequeueBufferedTelemetry(BufferedTelemetryMessage* msg);

/**
 * @brief Inspects the oldest buffered message without removing it from the queue.
 *
 * Allows mqttTask to verify successful MQTT transmission before discarding the message.
 *
 * @param msg Output pointer to store the peeked message.
 * @return true if a message was read; false if the buffer was empty.
 */
bool peekBufferedTelemetry(BufferedTelemetryMessage* msg);

/**
 * @brief Returns the number of telemetry messages currently awaiting transmission.
 *
 * @return size_t Count of buffered items waiting in queue (0 to 64).
 */
size_t getBufferedTelemetryCount();

/**
 * @brief Allocates the 64-slot FreeRTOS queue used for offline telemetry buffering.
 */
void initOfflineBuffer();

/**
 * @brief FreeRTOS task supervising connectivity states and offline buzzer signaling.
 *
 * Runs every 50 ms (Priority 2) on Core 1:
 * 1. Checks Wi-Fi and MQTT connection status.
 * 2. Checks task watchdog health (isSystemHealthy()).
 * 3. Updates system health state and triggers offline buzzer cadences.
 * 4. Feeds its watchdog heartbeat.
 *
 * @param parameter Unused FreeRTOS task parameter pointer (NULL).
 */
void offlineTask(void *parameter);
