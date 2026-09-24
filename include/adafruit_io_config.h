#pragma once

// Copy this file's values into adafruit_io_config.local.h for real credentials.
// The placeholder values keep the project buildable without exposing a secret.

#if __has_include("adafruit_io_config.local.h")
#include "adafruit_io_config.local.h"
#else
#define AIO_USERNAME "YOUR_ADAFRUIT_IO_USERNAME"
#define AIO_KEY "YOUR_ADAFRUIT_IO_KEY"
#define AIO_MQTT_SERVER "io.adafruit.com"
#define AIO_MQTT_PORT 1883
#endif
