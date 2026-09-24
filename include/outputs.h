#ifndef OUTPUTS_H
#define OUTPUTS_H

#include <Arduino.h>

void outputsInit();
void setSafetyOutputs(bool red, bool yellow, bool green, bool buzzer);

#endif
