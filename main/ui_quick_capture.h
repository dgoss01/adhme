#pragma once
#include "lvgl.h"
#include <stdint.h>

// Creates the Quick Capture screen — call once at startup
lv_obj_t* ui_quick_capture_create(void);

// Resets to idle page — call before lv_scr_load each time we enter
void ui_quick_capture_reset(void);

// Feed real-time audio level (0–100) from the audio capture task.
// Scrolls waveform left and appends new bar on the right.
// Safe to call from Core 1 — only touches LVGL inside display_lock.
void ui_quick_capture_set_level(uint8_t level);