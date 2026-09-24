#ifndef SENSORS_H
#define SENSORS_H

#include <Arduino.h>

void sensorsInit();
void temperatureTask(void *parameter);
void motionTask(void *parameter);
void vitalTask(void *parameter);
void ecgTask(void *parameter);

#endif
