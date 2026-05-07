#pragma once

#include "lvgl.h"
#include "esp_io_expander.h"

// Initialize display, touch, and LVGL
// Call once from app_main before starting LVGL task
void display_init(void);

// Acquire/release LVGL mutex (required before any lv_ calls)
bool display_lock(int timeout_ms);
void display_unlock(void);

// Returns the TCA9554 IO expander handle — available after display_init().
// Used by button driver to poll PWR button on EXIO4.
esp_io_expander_handle_t display_get_expander(void);

// Screen resolution
#define ADHME_LCD_H_RES 368
#define ADHME_LCD_V_RES 448