#include "rtc.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <sys/time.h>

static const char *TAG = "rtc";

#define PCF85063_ADDR   0x51
#define I2C_PORT        I2C_NUM_0
#define I2C_TIMEOUT_MS  1000
#define NVS_NAMESPACE   "adhme"
#define NVS_DST_KEY     "dst_offset"

static int s_dst_offset = 0;  // seconds: 0=STD, 3600=DST

// ─────────────────────────────────────────
// BCD helpers
// ─────────────────────────────────────────
static uint8_t bcd_to_dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t dec_to_bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

// ─────────────────────────────────────────
// Raw register I/O — I2C already initialized
// ─────────────────────────────────────────
static esp_err_t reg_read(uint8_t reg, uint8_t *data, size_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (PCF85063_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (PCF85063_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, data, len, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, cmd,
        pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t reg_write(uint8_t reg, uint8_t *data, size_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (PCF85063_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write(cmd, data, len, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, cmd,
        pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return ret;
}

// ─────────────────────────────────────────
// Public API
// ─────────────────────────────────────────
esp_err_t rtc_get_time(struct tm *t)
{
    uint8_t data[7];
    esp_err_t ret = reg_read(0x04, data, 7);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Read failed: %s", esp_err_to_name(ret));
        return ret;
    }
    memset(t, 0, sizeof(struct tm));
    t->tm_sec  = bcd_to_dec(data[0] & 0x7F);
    t->tm_min  = bcd_to_dec(data[1] & 0x7F);
    t->tm_hour = bcd_to_dec(data[2] & 0x3F);
    t->tm_mday = bcd_to_dec(data[3] & 0x3F);
    t->tm_mon  = bcd_to_dec(data[5] & 0x1F) - 1; // tm_mon is 0-based
    t->tm_year = bcd_to_dec(data[6]) + 100;       // years since 1900
    mktime(t); // fills tm_wday, tm_yday
    return ESP_OK;
}

esp_err_t rtc_set_time(const struct tm *t)
{
    uint8_t data[7] = {
        dec_to_bcd(t->tm_sec),
        dec_to_bcd(t->tm_min),
        dec_to_bcd(t->tm_hour),
        dec_to_bcd(t->tm_mday),
        dec_to_bcd(t->tm_wday),
        dec_to_bcd(t->tm_mon + 1),          // RTC is 1-based
        dec_to_bcd(t->tm_year - 100),        // RTC stores 2-digit year
    };
    esp_err_t ret = reg_write(0x04, data, 7);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "RTC set: %02d:%02d %02d/%02d/%04d",
            t->tm_hour, t->tm_min,
            t->tm_mday, t->tm_mon + 1, t->tm_year + 1900);
    }
    return ret;
}

esp_err_t rtc_sync_system_time(void)
{
    struct tm t;
    esp_err_t ret = rtc_get_time(&t);
    if (ret != ESP_OK) return ret;

    // Load saved DST offset from NVS
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        int32_t offset = 0;
        if (nvs_get_i32(nvs, NVS_DST_KEY, &offset) == ESP_OK) {
            s_dst_offset = (int)offset;
        }
        nvs_close(nvs);
    }

    // Apply DST offset
    t.tm_sec += s_dst_offset;
    mktime(&t);

    // Set ESP32 system time
    struct timeval tv = { .tv_sec = mktime(&t), .tv_usec = 0 };
    settimeofday(&tv, NULL);

    ESP_LOGI(TAG, "System time synced from RTC (DST offset: %ds)",
        s_dst_offset);
    return ESP_OK;
}

void rtc_set_dst_offset(int seconds)
{
    s_dst_offset = seconds;

    // Persist to NVS
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_i32(nvs, NVS_DST_KEY, (int32_t)seconds);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "DST offset set: %ds", seconds);
}

int rtc_get_dst_offset(void)
{
    return s_dst_offset;
}
