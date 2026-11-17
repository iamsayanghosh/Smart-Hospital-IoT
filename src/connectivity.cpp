/**
 * @file connectivity.cpp
 * @brief Network configuration and Wi-Fi transport definitions.
 *
 * Provides definition of network credentials and broker endpoints.
 * The active network connection lifecycle, state machine, and exponential
 * backoff are implemented in mqttTask (src/mqtt.cpp).
 */

#include "connectivity.h"
#include "adafruit_io_config.h"

const char *wifiSsid = "Wokwi-GUEST";
const char *mqttBroker = AIO_MQTT_SERVER;
const uint16_t mqttPort = AIO_MQTT_PORT;

WiFiClient wifiClient;
