#pragma once
#include "esp_err.h"
#include "driver/i2s_std.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Initialize ES8311 codec + I2S RX.
// Call once from app_main after display_init() — reuses the existing I2C bus.
esp_err_t audio_capture_init(void);

// Open a timestamped WAV file on SD and start recording.
// Filename is generated internally from RTC. Non-blocking — capture on Core 1.
esp_err_t audio_capture_start(void);

// Signal stop. Non-blocking — capture task writes WAV header and closes file.
// Poll audio_capture_poll_done() to know when safe to proceed.
void audio_capture_stop(void);

// Abort immediately — closes and deletes the partial file.
void audio_capture_abort(void);

// Current mic peak level 0–100. Safe to call from any task.
uint8_t audio_capture_get_level(void);

// Returns true once WAV is fully written after audio_capture_stop().
// out_path: filled with "/sdcard/qc_YYYYMMDD_HHMMSS.wav" (may be NULL)
// out_duration_sec: filled with recording length in seconds (may be NULL)
bool audio_capture_poll_done(char *out_path, size_t len,
                              uint32_t *out_duration_sec);

// Returns true when no recording is in progress.
// Safe to call from any task. Used by tone_player before writing to TX.
bool audio_capture_is_idle(void);

// Returns the I2S TX channel handle — valid after audio_capture_init().
// tone_player uses this to write PCM without owning I2S init.
i2s_chan_handle_t audio_capture_get_tx_handle(void);
