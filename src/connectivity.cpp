#include "connectivity.h"
#include "adafruit_io_config.h"

const char *wifiSsid = "Wokwi-GUEST";
const char *mqttBroker = AIO_MQTT_SERVER;
const uint16_t mqttPort = AIO_MQTT_PORT;

WiFiClient wifiClient;

void setupConnectivity()
{
    // Optionally initiate WiFi here, or let mqttTask handle it
}
