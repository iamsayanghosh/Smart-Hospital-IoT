#pragma once
#include <Arduino.h>
#include <PubSubClient.h>

extern PubSubClient mqttClient;

void mqttTask(void *parameter);
