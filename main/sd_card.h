#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>

#define SD_MOUNT_POINT     "/sdcard"

// ─────────────────────────────────────────
// Per-applet directories — lazy-created on first write
// Use these constants everywhere instead of hard-coded paths.
// ─────────────────────────────────────────
#define SD_DIR_CAPTURES    SD_MOUNT_POINT "/captures"   // Quick Capture WAVs
#define SD_DIR_CHECKINS    SD_MOUNT_POINT "/checkins"   // Check In CSV log

// Mount SD card via SDMMC. Safe to call if already mounted (no-op).
esp_err_t sd_card_mount(void);

// Unmount SD card. Safe to call if not mounted.
void sd_card_unmount(void);

bool sd_card_mounted(void);

// Ensure a directory exists on the SD card. Creates it if missing.
// Returns ESP_OK if the directory exists (or was just created),
// ESP_ERR_INVALID_STATE if SD not mounted, ESP_FAIL on mkdir failure.
// Single-level only — pass an absolute path under SD_MOUNT_POINT.
esp_err_t sd_card_ensure_dir(const char *path);

// Fills buf with a timestamped WAV path under SD_DIR_CAPTURES:
//   /sdcard/captures/YYDDD-HHmmss.WAV
// Ensures the captures directory exists before returning.
void sd_card_make_filename(char *buf, size_t len);
