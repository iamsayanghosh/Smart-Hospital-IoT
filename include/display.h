#pragma once
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

void displayInit(uint8_t sda, uint8_t scl);
void displayTask(void *parameter);
