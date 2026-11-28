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

/**
 * @brief WiFiClient wrapper bounding multi-retry TCP TX stalls below watchdog limits.
 *
 * Framework NetworkClient::write() retries select() up to 10 x 1 s (10 s worst case),
 * which can exceed WATCHDOG_TIMEOUT_MS (6 s) and TWDT_TIMEOUT_MS (8 s) on a stalled
 * half-open socket. This subclass bounds the total write wall-clock duration and
 * closes the socket on TX timeout so MQTT cleanly triggers reconnect + FIFO buffering.
 */
class BoundedWiFiClient : public WiFiClient {
public:
    explicit BoundedWiFiClient(uint32_t maxWriteMs = 1500U) : WiFiClient(), _maxWriteMs(maxWriteMs) {}

    using WiFiClient::write;
    void setWriteTimeoutMs(uint32_t maxWriteMs) { _maxWriteMs = maxWriteMs; }
    size_t write(uint8_t data) override;
    size_t write(const uint8_t *buf, size_t size) override;

private:
    uint32_t _maxWriteMs;
};

extern BoundedWiFiClient wifiClient;    ///< Bounded TCP socket client used by PubSubClient
