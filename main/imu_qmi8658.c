#include "imu_qmi8658.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "qmi8658";

// ─────────────────────────────────────────
// I2C config — shared bus, already initialised by display_init()
// ─────────────────────────────────────────
#define QMI8658_ADDR        0x6B        // SA0 pin pulled high on Waveshare 1.8
#define I2C_PORT            I2C_NUM_0
#define I2C_TIMEOUT_MS      50          // short — reads are 6 bytes, bus is idle

// ─────────────────────────────────────────
// Register map (QMI8658A datasheet §6)
// ─────────────────────────────────────────
#define REG_WHO_AM_I        0x00        // should read 0x05
#define REG_REVISION        0x01
#define REG_CTRL1           0x02        // SPI/I2C config
#define REG_CTRL2           0x03        // Accel ODR + full-scale
#define REG_CTRL3           0x04        // Gyro ODR + full-scale
#define REG_CTRL5           0x06        // Low-pass filter config
#define REG_CTRL7           0x08        // Enable sensors
#define REG_AX_L            0x35        // Accel X low byte (6 bytes: AX_L/H, AY_L/H, AZ_L/H)

// CTRL2 — Accel: ±2g (aScale=00), 58.83Hz ODR (aODR=0110)
#define CTRL2_ACC_2G_58HZ   0x06        // [7:4]=0000 (±2g) | [3:0]=0110 (58.83Hz)

// CTRL7 — enable accelerometer only (bit 0), gyro off (bit 1)
#define CTRL7_ACC_ENABLE    0x01

// Sensitivity: ±2g → 16384 LSB/g
#define ACC_SENSITIVITY     16384.0f

// ─────────────────────────────────────────
// Raw I2C helpers (same idiom as rtc.c)
// ─────────────────────────────────────────
static esp_err_t reg_read(uint8_t reg, uint8_t *data, size_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (QMI8658_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (QMI8658_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, data, len, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t reg_write_byte(uint8_t reg, uint8_t val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (QMI8658_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return ret;
}

// ─────────────────────────────────────────
// Public API
// ─────────────────────────────────────────
esp_err_t imu_qmi8658_init(void)
{
    // Verify chip identity
    uint8_t who = 0;
    esp_err_t ret = reg_read(REG_WHO_AM_I, &who, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WHO_AM_I read failed: %s", esp_err_to_name(ret));
        return ret;
    }
    if (who != 0x05) {
        ESP_LOGE(TAG, "Unexpected WHO_AM_I: 0x%02X (expected 0x05)", who);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "QMI8658 found (WHO_AM_I=0x%02X)", who);

    // CTRL1: I2C enabled, auto-increment addresses on burst read
    ret = reg_write_byte(REG_CTRL1, 0x40);
    if (ret != ESP_OK) return ret;

    // CTRL2: Accel ±2g, 58.83 Hz
    ret = reg_write_byte(REG_CTRL2, CTRL2_ACC_2G_58HZ);
    if (ret != ESP_OK) return ret;

    // CTRL5: Accel low-pass filter enabled (bit 0), gyro LPF off
    ret = reg_write_byte(REG_CTRL5, 0x01);
    if (ret != ESP_OK) return ret;

    // CTRL7: Enable accelerometer only
    ret = reg_write_byte(REG_CTRL7, CTRL7_ACC_ENABLE);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "QMI8658 configured: accel ±2g @ 58.83Hz");
    return ESP_OK;
}

esp_err_t imu_qmi8658_read_accel(float *gx, float *gy, float *gz)
{
    uint8_t raw[6];
    esp_err_t ret = reg_read(REG_AX_L, raw, 6);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Accel read failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Reassemble 16-bit signed little-endian values
    int16_t ax = (int16_t)((raw[1] << 8) | raw[0]);
    int16_t ay = (int16_t)((raw[3] << 8) | raw[2]);
    int16_t az = (int16_t)((raw[5] << 8) | raw[4]);

    *gx = (float)ax / ACC_SENSITIVITY;
    *gy = (float)ay / ACC_SENSITIVITY;
    *gz = (float)az / ACC_SENSITIVITY;

    return ESP_OK;
}
