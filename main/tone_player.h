#pragma once
#include <stdint.h>

// Initialize tone player — call once from app_main after audio_capture_init().
// Registers the fire-and-forget task infrastructure. No-op if already called.
void tone_player_init(void);

// Play a tone burst — non-blocking, fire and forget.
// freq_hz   : frequency in Hz (80–400 Hz recommended for low clicks)
// duration_ms: length of each click in ms
// count     : number of clicks to play
// gap_ms    : silence between clicks in ms
//
// Silently skipped if audio capture is currently active.
// Safe to call from any task (LVGL timer, button event, etc.)
void tone_play(uint16_t freq_hz, uint16_t duration_ms,
               uint8_t count, uint16_t gap_ms);

// Convenience: two soft 180 Hz clicks — the Lock In break nudge sound.
void tone_play_double_click(void);
