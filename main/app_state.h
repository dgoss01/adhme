#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

// ─────────────────────────────────────────
// App States
// ─────────────────────────────────────────
typedef enum {
    STATE_HOME = 0,
    STATE_LOCK_IN,
    STATE_CHECK_IN,
    STATE_CHECK_BACK,
    STATE_DRIFT,
    STATE_ANCHOR,
    STATE_SPARK,
    STATE_WIND_DOWN,
    STATE_QUICK_CAPTURE,
    STATE_TIMESET,
    STATE_COUNT  // always last — lets us bounds-check
} adhme_state_t;

// ─────────────────────────────────────────
// Button Events
// ─────────────────────────────────────────
typedef enum {
    BTN_PWR_SHORT = 0,
    BTN_PWR_DOUBLE,
    BTN_BOOT_SHORT,
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

// Returns human-readable state name (for logging)
const char* adhme_state_name(adhme_state_t state);
