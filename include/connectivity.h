#pragma once
#include <WiFi.h>

extern const char *wifiSsid;
extern const char *mqttBroker;
extern const uint16_t mqttPort;

extern WiFiClient wifiClient;

void setupConnectivity();
