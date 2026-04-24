#pragma once

#include "lvgl.h"

// Initialize display, touch, and LVGL
// Call once from app_main before starting LVGL task
void display_init(void);

// Acquire/release LVGL mutex (required before any lv_ calls)
bool display_lock(int timeout_ms);
void display_unlock(void);

// Screen resolution
#define ADHME_LCD_H_RES 368
#define ADHME_LCD_V_RES 448
