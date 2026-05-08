#pragma once

#include "esp_err.h"

// ─────────────────────────────────────────
// QMI8658 — 6-axis IMU driver
// Accelerometer only (gravity vector for Sand)
// I2C_NUM_0, SDA=GPIO15, SCL=GPIO14 (shared bus)
// ─────────────────────────────────────────

// Initialize the QMI8658. Call once after display_init() (I2C bus already up).
// Configures accelerometer at ±2g, 58.83 Hz ODR. Gyro left in standby.
esp_err_t imu_qmi8658_init(void);

// Read calibrated accelerometer values in g (gravity units).
// gx = left/right tilt, gy = forward/back tilt, gz = face-up/down.
// Returns ESP_OK on success; on failure, last good values are preserved.
esp_err_t imu_qmi8658_read_accel(float *gx, float *gy, float *gz);
