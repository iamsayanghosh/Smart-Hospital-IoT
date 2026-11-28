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
#include <errno.h>
#include <lwip/sockets.h>

const char *wifiSsid = "Wokwi-GUEST";
const char *mqttBroker = AIO_MQTT_SERVER;
const uint16_t mqttPort = AIO_MQTT_PORT;

BoundedWiFiClient wifiClient(1500U);

size_t BoundedWiFiClient::write(uint8_t data)
{
    return write(&data, 1U);
}

size_t BoundedWiFiClient::write(const uint8_t *buf, size_t size)
{
    if (buf == nullptr || size == 0U) {
        return 0U;
    }

    const int socketFileDescriptor = fd();
    if (!_connected || socketFileDescriptor < 0) {
        return 0U;
    }

    if (_lastWriteTimeout != _timeout) {
        struct timeval timeoutTv;
        timeoutTv.tv_sec = _timeout / 1000;
        timeoutTv.tv_usec = (_timeout % 1000) * 1000;
        if (setSocketOption(SOL_SOCKET, SO_SNDTIMEO, &timeoutTv, sizeof(timeoutTv)) >= 0) {
            _lastWriteTimeout = _timeout;
        }
    }

    const uint32_t startMs = millis();
    size_t totalBytesSent = 0U;

    while (totalBytesSent < size) {
        const uint32_t elapsedMs = static_cast<uint32_t>(millis() - startMs);
        if (elapsedMs >= _maxWriteMs) {
            log_e("Bounded TX timeout (%lu ms) on fd %d", static_cast<unsigned long>(_maxWriteMs), socketFileDescriptor);
            stop();
            return 0U;
        }

        const uint32_t remainingMs = _maxWriteMs - elapsedMs;
        const uint32_t stepMs = (remainingMs < 250U) ? remainingMs : 250U;

        fd_set writeSet;
        struct timeval tv;
        FD_ZERO(&writeSet);
        FD_SET(socketFileDescriptor, &writeSet);
        tv.tv_sec = static_cast<long>(stepMs / 1000U);
        tv.tv_usec = static_cast<long>((stepMs % 1000U) * 1000U);

        const int ready = select(socketFileDescriptor + 1, nullptr, &writeSet, nullptr, &tv);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            stop();
            return 0U;
        }
        if (ready == 0 || !FD_ISSET(socketFileDescriptor, &writeSet)) {
            continue;
        }

        const int sent = send(socketFileDescriptor, buf + totalBytesSent, size - totalBytesSent, MSG_DONTWAIT);
        if (sent > 0) {
            totalBytesSent += static_cast<size_t>(sent);
        } else if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            log_e("Bounded TX fail on fd %d, errno: %d", socketFileDescriptor, errno);
            stop();
            return 0U;
        }
    }

    return totalBytesSent;
}
