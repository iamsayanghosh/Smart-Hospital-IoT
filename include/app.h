#pragma once

#include <Arduino.h>
#include "state.h"

void appInit();
void appStartTasks();
void failSetup(const char *message);

uint32_t getSamplePeriodMs(MonitoredTaskId taskId);
const char *taskNameFromId(MonitoredTaskId taskId);
void reportTaskHeartbeat(MonitoredTaskId taskId);
