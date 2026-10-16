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
 *     - Limits publication to one changed feed per 3000 ms cycle to comply with Adafruit IO rate limits.
 * - Offline Queue Integration:
 *     - Diverts failed publishes to the 64-slot ring buffer and drains buffered entries upon reconnect.
 */

extern PubSubClient mqttClient; ///< Global PubSubClient MQTT client instance

/**
 * @brief FreeRTOS task managing MQTT lifecycle, message routing, and publication scheduling.
 *
 * Running at Priority 2 with an 8192-byte stack:
 * 1. Executes exponential backoff for Wi-Fi and MQTT connection attempts.
 * 2. Services MQTT packet loop (mqttClient.loop()) every 50 ms.
 * 3. Dispatches live sensor telemetry and alerts every 3000 ms.
 * 4. Interleaves offline buffer replay during connection recovery.
 * 5. Compiles and publishes aggregated medical and facility summaries every 60000 ms.
 * 6. Feeds its watchdog heartbeat.
 *
 * @param parameter Unused FreeRTOS task parameter pointer (NULL).
 */
void mqttTask(void *parameter);
