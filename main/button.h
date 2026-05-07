#pragma once

// Initialize button driver — call once from app_main after display_init().
// Starts an internal polling task that feeds g_button_queue.
void button_init(void);