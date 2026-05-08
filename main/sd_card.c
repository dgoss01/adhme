#include "sd_card.h"
#include "rtc.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <errno.h>

static const char *TAG = "sd_card";

// ─────────────────────────────────────────
// SDMMC pins — from Waveshare board sdkconfig.defaults
// (NOT the generic ESP32-S3 defaults — those are wrong for this board)
// ─────────────────────────────────────────
#define SD_CLK_IO   GPIO_NUM_2
#define SD_CMD_IO   GPIO_NUM_1
#define SD_D0_IO    GPIO_NUM_3

static sdmmc_card_t *s_card    = NULL;
static bool          s_mounted = false;

esp_err_t sd_card_mount(void)
{
    if (s_mounted) return ESP_OK;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 4,
        .allocation_unit_size   = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;  // 20 MHz

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width  = 1;           // 1-bit mode per board sdkconfig
    slot.clk    = SD_CLK_IO;
    slot.cmd    = SD_CMD_IO;
    slot.d0     = SD_D0_IO;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(
        SD_MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);

    if (ret == ESP_OK) {
        s_mounted = true;
        ESP_LOGI(TAG, "Mounted on %s — %s %.1f MB",
            SD_MOUNT_POINT,
            s_card->cid.name,
            (float)((uint64_t)s_card->csd.capacity *
                    s_card->csd.sector_size) / (1024.0f * 1024.0f));
    } else {
        ESP_LOGE(TAG, "Mount failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

void sd_card_unmount(void)
{
    if (!s_mounted) return;
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    s_card    = NULL;
    s_mounted = false;
    ESP_LOGI(TAG, "Unmounted");
}

bool sd_card_mounted(void)
{
    return s_mounted;
}

// ─────────────────────────────────────────
// sd_card_ensure_dir — single-level mkdir if missing
// ─────────────────────────────────────────
esp_err_t sd_card_ensure_dir(const char *path)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return ESP_OK;          // already exists — common case, silent
        }
        ESP_LOGE(TAG, "Path exists but is not a directory: %s", path);
        return ESP_FAIL;
    }

    // Doesn't exist — create it. FAT-fs ignores mode bits, but POSIX wants them.
    if (mkdir(path, 0755) != 0) {
        ESP_LOGE(TAG, "mkdir(%s) failed: %s", path, strerror(errno));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Created directory: %s", path);
    return ESP_OK;
}

void sd_card_make_filename(char *buf, size_t len)
{
    // Make sure the captures directory exists so the writer doesn't trip.
    // If SD is not mounted, ensure_dir will return early and the caller's
    // open() will fail naturally — same behavior as before, just cleaner.
    sd_card_ensure_dir(SD_DIR_CAPTURES);

    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm *t = localtime(&tv.tv_sec);
    // Format: YYDDD-HHmmss.WAV
    //   YY  = 2-digit year (26 = 2026)
    //   DDD = day-of-year (001–366)
    //   HH  = hour, mm = minute, ss = second
    // Sorts chronologically, collision-proof, LFN-friendly
    snprintf(buf, len, SD_DIR_CAPTURES "/%02d%03d-%02d%02d%02d.WAV",
        t->tm_year - 100,   // years since 2000
        t->tm_yday + 1,     // 1-based day of year
        t->tm_hour,
        t->tm_min,
        t->tm_sec);
}
