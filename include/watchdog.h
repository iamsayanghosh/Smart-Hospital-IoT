#pragma once

#include <Arduino.h>
#include "state.h"

void watchdogInit();
void watchdogTask(void *parameter);
void reportTaskHeartbeat(MonitoredTaskId taskId);
