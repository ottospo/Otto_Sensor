#ifndef __OTA_H
#define __OTA_H

#include <Arduino.h>

// Returns "" on success, otherwise an error description. md5 is optional (32 hex chars).
String firmware_ota(String url, String md5 = "", void (*progress)(int percent) = nullptr);
#endif
