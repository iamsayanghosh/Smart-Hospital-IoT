#pragma once
#include <Arduino.h>
#include <PubSubClient.h>

/**
 * @file mqtt.h
 * @brief Adafruit IO MQTT communications, subscription routing, and telemetry publication declarations.
 *
 * Responsibilities:
 * - Transport Coordination: Manages Wi-Fi station connection and Adafruit IO MQTT client session.
 * - Inbound Subscription Routing:
 *     - "medication-dosage": Desired infusion rate in mg/hr (routes to medicationDosageControl).
 *     - "sampling-rate": Desired sensor acquisition rate [5 - 60 s] (overwrites samplingQueue).
 *     - "bed-angle": Desired bed elevation angle [0 - 90 deg] (overwrites bedQueue).
 * - Outbound Telemetry Publication:
 *     - Medical Feeds: medical-heart-rate, medical-spo2, medical-body-temperature.
 *     - Facility Feeds: facility-room-temperature, facility-oxygen, facility-aqi.
 *     - Shared Events: hospital-events (medical alerts, facility alerts, and 60 s combined summaries).
 * - Throttling & Rate-Limiting:
 *     - Live telemetry and offline replay share a 3000 ms round-robin dispatch slot (max 20 msgs/min).
 *     - Periodic summaries and event alerts have independent 60 s pacing; nominal aggregate rate is ~23 msgs/min
 *       under stated scheduling assumptions (targeting Adafruit IO's 30 data points/min free quota).
 *     - Note: Multiple publications (up to 4) can occur consecutively in one task iteration if timers coincide;
 *       the firmware enforces local cadence, not account-wide quota governance.
 * - Offline Queue Integration:
 *     - Diverts failed publishes to the 64-slot FreeRTOS FIFO queue (drops newest incoming entries when full to preserve initial failure state) and drains buffered entries upon reconnect.
 */

extern PubSubClient mqttClient; ///< Global PubSubClient MQTT client instance

/**
 * @brief Thread-safe query of MQTT client connection status.
 *
 * Updated solely within mqttTask to avoid cross-task socket access on PubSubClient.
 *
 * @return true if MQTT client is currently connected to broker.
 */
bool isMqttClientConnected();

/**
 * @brief Sets simulated Wi-Fi disconnect flag for Task 6 demonstration in Wokwi.
 *
 * @param enable True to force Wi-Fi drop; false to restore online replay.
 */
void setSimulateOfflineWifi(bool enable);

/**
 * @brief Sets simulated MQTT broker disconnect flag for Task 6 demonstration in Wokwi.
 *
 * @param enable True to force MQTT broker drop; false to restore online replay.
 */
void setSimulateOfflineMqtt(bool enable);

/**
 * @brief Queries whether simulated Wi-Fi offline mode is active.
 *
 * @return true if simulated Wi-Fi drop is enabled; false otherwise.
 */
bool isSimulateOfflineWifi();

/**
 * @brief Queries whether simulated MQTT broker offline mode is active.
 *
 * @return true if simulated MQTT drop is enabled; false otherwise.
 */
bool isSimulateOfflineMqtt();

/**
 * @brief FreeRTOS task managing MQTT lifecycle, message routing, and publication scheduling.
 *
 * Running at Priority 2 with an 8192-byte stack:
 * 1. Executes exponential backoff for Wi-Fi and MQTT connection attempts.
 * 2. Services MQTT packet loop (mqttClient.loop()) every 50 ms.
 * 3. Dispatches live sensor telemetry or replayed buffer slot every 3000 ms, and evaluates alerts.
 * 4. Interleaves offline buffer replay into the 3000 ms slot during connection recovery.
 * 5. Compiles and publishes aggregated medical and facility summaries every 60000 ms.
 * 6. Feeds its watchdog heartbeat.
 *
 * @param parameter Unused FreeRTOS task parameter pointer (NULL).
 */
void mqttTask(void *parameter);
