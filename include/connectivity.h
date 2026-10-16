#pragma once
#include <WiFi.h>

/**
 * @file connectivity.h
 * @brief Network configuration and Wi-Fi client transport declarations.
 *
 * Defines the shared Wi-Fi SSID, Adafruit IO MQTT endpoint host and port,
 * and the global WiFiClient socket instance used by PubSubClient in mqtt.cpp.
 *
 * Connection Management:
 * - Active connection, exponential backoff, and reconnection are managed
 *   asynchronously inside mqttTask (src/mqtt.cpp).
 */

extern const char *wifiSsid;     ///< Target Wi-Fi network SSID (configured for Wokwi simulation)
extern const char *mqttBroker;   ///< Adafruit IO MQTT broker hostname (io.adafruit.com)
extern const uint16_t mqttPort;  ///< Adafruit IO MQTT TCP port (default 1883)

extern WiFiClient wifiClient;    ///< Underlying TCP socket client used by PubSubClient

/**
 * @brief Optional pre-initialization hook for network connectivity.
 *
 * The actual Wi-Fi connection loop is handled asynchronously within mqttTask
 * to avoid blocking system startup before FreeRTOS tasks are launched.
 */
void setupConnectivity();
