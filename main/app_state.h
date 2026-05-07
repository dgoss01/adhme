#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

// ─────────────────────────────────────────
// App States
// ─────────────────────────────────────────
typedef enum {
    STATE_HOME = 0,
    STATE_LOCK_IN,       // Focus timer with pomodoro rhythm
    STATE_CHECK_IN,      // Mood · energy · focus · body sliders
    STATE_DRIFT,         // Grounding micro-task card deck
    STATE_BREATHE,       // Breathing pacer with pattern selection
    STATE_SAND,          // IMU-driven gravity sandbox
    STATE_SPARK,         // Touch-reactive flow field visual
    STATE_QUICK_CAPTURE, // Voice note recorder (PWR double from anywhere)
    STATE_TIMESET,       // RTC time/date setter
    STATE_COUNT          // Always last — lets us bounds-check
} adhme_state_t;

// ─────────────────────────────────────────
// Button Events
// ─────────────────────────────────────────
typedef enum {
    BTN_PWR_SHORT = 0,   // → HOME from anywhere
    BTN_PWR_DOUBLE,      // → QUICK_CAPTURE from anywhere
    BTN_BOOT_SHORT,      // → Back / exit current app
} adhme_button_event_t;

// ─────────────────────────────────────────
// State Change Message (sent via queue)
// ─────────────────────────────────────────
typedef struct {
    adhme_state_t next_state;
} adhme_state_msg_t;

// ─────────────────────────────────────────
// Global Handles (defined in main.c)
// ─────────────────────────────────────────
extern QueueHandle_t g_state_queue;
extern QueueHandle_t g_button_queue;
extern volatile adhme_state_t g_current_state;

// ─────────────────────────────────────────
// API
// ─────────────────────────────────────────

// Request a state transition from any task
void adhme_goto(adhme_state_t next_state);

// Returns the state to restore after Quick Capture exits
adhme_state_t adhme_get_return_state(void);

// Returns human-readable state name (for logging)
const char* adhme_state_name(adhme_state_t state);