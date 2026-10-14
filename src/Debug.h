#pragma once
#include <Arduino.h>

// Verbose sensor/diagnostic logging, gated behind the DEBUG_LOG build flag
// (see platformio.ini). Disable the flag to silence this output in production.
#ifdef DEBUG_LOG
#define DBG_PRINT(...) Serial.print(__VA_ARGS__)
#define DBG_PRINTLN(...) Serial.println(__VA_ARGS__)
#define DBG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
#define DBG_PRINT(...)
#define DBG_PRINTLN(...)
#define DBG_PRINTF(...)
#endif
