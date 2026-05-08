#pragma once
#include "lvgl.h"

// Creates the Lock In screen — call once at startup inside display_lock()
lv_obj_t* ui_lock_in_create(void);

// Call before lv_scr_load() each time the user enters Lock In.
// If a session was paused, restores the frozen state (RESUME button shown).
// If no session is in progress, resets to fresh setup.
void ui_lock_in_reset(void);

// Pause the active timer from outside the screen — e.g. when Quick Capture
// is triggered while a session is running. Safe to call from state_task()
// inside display_lock(). No-op if not currently in an active session.
void ui_lock_in_pause(void);
