#pragma once
#include <Arduino.h>

enum SystemHealthState {
    SYSTEM_ONLINE,
    SYSTEM_DEGRADED,
    SYSTEM_OFFLINE
};

enum OfflineBuzzerMode {
    BUZZER_MODE_NORMAL,
    BUZZER_MODE_WIFI_FAIL,
    BUZZER_MODE_MQTT_FAIL
};

struct BufferedTelemetryMessage {
    char topic[64];
    char payload[256];
};

SystemHealthState getSystemHealthState();
void updateSystemHealth(bool wifiConnected, bool mqttConnected, bool systemHealthy);
OfflineBuzzerMode getOfflineBuzzerMode();

bool enqueueBufferedTelemetry(const char* topic, const char* payload);
bool dequeueBufferedTelemetry(BufferedTelemetryMessage* msg);
bool peekBufferedTelemetry(BufferedTelemetryMessage* msg);
size_t getBufferedTelemetryCount();
void initOfflineBuffer();

void offlineTask(void *parameter);
