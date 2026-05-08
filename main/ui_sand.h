#pragma once
#include "lvgl.h"

// Creates the Sand screen — call once at startup inside display_lock().
lv_obj_t* ui_sand_create(void);

// Call before lv_scr_load() each time the user enters Sand.
// Starts the physics task on Core 1 and resets particles.
void ui_sand_start(void);

// Call when leaving Sand (state transition away).
// Stops the physics task cleanly before screen unload.
void ui_sand_stop(void);
