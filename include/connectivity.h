#pragma once
#include <WiFi.h>
#include <PubSubClient.h>

extern const char *wifiSsid;
extern const char *mqttBroker;
extern const uint16_t mqttPort;

extern WiFiClient wifiClient;
extern PubSubClient mqttClient;

void setupConnectivity();
