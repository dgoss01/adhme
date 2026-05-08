#pragma once
#include "lvgl.h"

// Creates the Check In screen — call once at startup inside display_lock().
lv_obj_t* ui_check_in_create(void);

// Call before lv_scr_load() each time the user enters Check In.
// Resets all four sliders to center (50) and shows the sliders page.
void ui_check_in_reset(void);
