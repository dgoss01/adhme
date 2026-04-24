#pragma once
#include "lvgl.h"

// Creates the time/date set screen
// On completion saves to RTC and returns to home
lv_obj_t* ui_timeset_create(void);
