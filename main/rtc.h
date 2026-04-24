#pragma once
#include <stdint.h>
#include "esp_err.h"
#include <time.h>

// Read RTC → populates tm struct
esp_err_t rtc_get_time(struct tm *t);

// Write tm struct → RTC
esp_err_t rtc_set_time(const struct tm *t);

// Read RTC → set ESP32 system time
// Call once at boot after display_init()
esp_err_t rtc_sync_system_time(void);

// DST offset in seconds (0 or 3600)
void rtc_set_dst_offset(int seconds);
int  rtc_get_dst_offset(void);
