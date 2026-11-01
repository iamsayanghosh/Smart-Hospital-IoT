/**
 * @file connectivity.cpp
 * @brief Network configuration and Wi-Fi transport definitions.
 *
 * Provides definition of network credentials and broker endpoints.
 * The active network connection lifecycle, state machine, and exponential backoff
 * are implemented in mqttTask (src/mqtt.cpp).
 */

#include "connectivity.h"
#include "adafruit_io_config.h"

const char *wifiSsid = "Wokwi-GUEST";
const char *mqttBroker = AIO_MQTT_SERVER;
const uint16_t mqttPort = AIO_MQTT_PORT;

WiFiClient wifiClient;

/**
 * @brief Pre-initialization hook for network connectivity.
 *
 * The active Wi-Fi connection loop is deliberately deferred to mqttTask
 * to ensure system startup and local control tasks (medication, bed, watchdog)
 * begin executing immediately without network blocking delays.
 */
void setupConnectivity()
{
    // The active Wi-Fi connection loop is deliberately deferred to mqttTask
    // to ensure system startup and local control tasks (medication, bed, watchdog)
    // begin executing immediately without network blocking delays.
}
