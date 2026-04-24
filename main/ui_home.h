#pragma once
#include "lvgl.h"

// Build and return the home screen object
lv_obj_t* ui_home_create(void);

// Call periodically to update the clock
void ui_home_tick(void);
