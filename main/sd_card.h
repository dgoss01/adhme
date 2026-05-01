#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>

#define SD_MOUNT_POINT "/sdcard"

// Mount SD card via SDMMC. Safe to call if already mounted (no-op).
esp_err_t sd_card_mount(void);

// Unmount SD card. Safe to call if not mounted.
void sd_card_unmount(void);

bool sd_card_mounted(void);

// Fills buf with a timestamped WAV path:
//   /sdcard/qc_YYYYMMDD_HHMMSS.wav
void sd_card_make_filename(char *buf, size_t len);