#include "audio_capture.h"
#include "sd_card.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "es8311.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

static const char *TAG = "audio_cap";

// ─────────────────────────────────────────
// Hardware config  (from example_config.h, ESP32-S3)
// ─────────────────────────────────────────
#define I2S_PORT        I2S_NUM_0
#define I2C_PORT        I2C_NUM_0   // already initialized by display.c
#define SAMPLE_RATE     16000
#define MCLK_MULTIPLE   384
#define GPIO_PA_ENABLE  GPIO_NUM_46

#define PIN_I2S_MCK     GPIO_NUM_16
#define PIN_I2S_BCK     GPIO_NUM_9
#define PIN_I2S_WS      GPIO_NUM_45
#define PIN_I2S_DOUT    GPIO_NUM_8
#define PIN_I2S_DIN     GPIO_NUM_10

// ─────────────────────────────────────────
// Capture config
// ─────────────────────────────────────────
// 1024 stereo frames × 2 bytes × 2 channels = 4096 bytes = 64 ms at 16 kHz
#define DMA_BUF_FRAMES   1024
#define STEREO_BUF_BYTES (DMA_BUF_FRAMES * 2 * sizeof(int16_t))
#define MONO_BUF_BYTES   (DMA_BUF_FRAMES * sizeof(int16_t))
#define CAPTURE_GAIN      6   // software gain, applied before clipping and peak detection

// ─────────────────────────────────────────
// State
// ─────────────────────────────────────────
typedef enum { ACAP_IDLE = 0, ACAP_RUNNING, ACAP_STOP_REQ } acap_state_t;

static volatile acap_state_t  s_state         = ACAP_IDLE;
static volatile uint8_t       s_level         = 0;
static volatile bool          s_done          = false;

static i2s_chan_handle_t       s_tx_handle    = NULL;  // needed for proper clock init
static i2s_chan_handle_t       s_rx_handle    = NULL;
static es8311_handle_t         s_es_handle    = NULL;
static FILE                   *s_file         = NULL;
static uint32_t                s_bytes_written = 0;
static uint32_t                s_duration_sec  = 0;
static char                    s_filepath[80];

// ─────────────────────────────────────────
// WAV header — 44 bytes, call with f at position 0
// ─────────────────────────────────────────
static void write_wav_header(FILE *f, uint32_t data_bytes)
{
    const uint16_t channels    = 1;
    const uint16_t bits        = 16;
    const uint32_t sample_rate = SAMPLE_RATE;
    const uint16_t block_align = channels * (bits / 8);
    const uint32_t byte_rate   = sample_rate * block_align;
    const uint16_t audio_fmt   = 1; // PCM
    const uint32_t sub1_size   = 16;
    const uint32_t chunk_size  = 36 + data_bytes;

    fwrite("RIFF",       1, 4, f);
    fwrite(&chunk_size,  4, 1, f);
    fwrite("WAVE",       1, 4, f);
    fwrite("fmt ",       1, 4, f);
    fwrite(&sub1_size,   4, 1, f);
    fwrite(&audio_fmt,   2, 1, f);
    fwrite(&channels,    2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate,   4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits,        2, 1, f);
    fwrite("data",       1, 4, f);
    fwrite(&data_bytes,  4, 1, f);
    // 44 bytes total
}

// ─────────────────────────────────────────
// Capture task — Core 1, permanent
// ─────────────────────────────────────────
static void capture_task(void *arg)
{
    int16_t *stereo_buf = malloc(STEREO_BUF_BYTES);
    int16_t *mono_buf   = malloc(MONO_BUF_BYTES);
    assert(stereo_buf && mono_buf);

    while (1) {
        if (s_state == ACAP_IDLE) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(s_rx_handle,
            stereo_buf, STEREO_BUF_BYTES,
            &bytes_read, pdMS_TO_TICKS(200));

        if (ret != ESP_OK || bytes_read == 0) continue;

        // Stereo → mono: use RIGHT channel + apply gain + compute peak
        int frames = (int)(bytes_read / (2 * sizeof(int16_t)));
        int16_t peak = 0;

        for (int i = 0; i < frames; i++) {
            // Grab right channel
            int32_t sample = stereo_buf[i * 2 + 1];

            // Apply software gain
            sample *= CAPTURE_GAIN;

            // Clamp to int16 range
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;

            // Store
            mono_buf[i] = (int16_t)sample;

            // Peak detection (after gain)
            int16_t a = mono_buf[i] < 0 ? -mono_buf[i] : mono_buf[i];
            if (a > peak) peak = a;
        }

        s_level = (uint8_t)((uint32_t)peak * 100u / 32767u);

        if (s_file) {
            fwrite(mono_buf, sizeof(int16_t), frames, s_file);
            s_bytes_written += (uint32_t)(frames * sizeof(int16_t));
        }

        if (s_state == ACAP_STOP_REQ) {
            if (s_file) {
                fseek(s_file, 0, SEEK_SET);
                write_wav_header(s_file, s_bytes_written);
                fclose(s_file);
                s_file = NULL;
            }
            s_duration_sec = s_bytes_written / (SAMPLE_RATE * sizeof(int16_t));
            ESP_LOGI(TAG, "WAV saved: %lu bytes, %lu sec",
                (unsigned long)s_bytes_written,
                (unsigned long)s_duration_sec);
            s_state = ACAP_IDLE;
            s_level = 0;
            s_done  = true;
        }
    }

    free(stereo_buf);
    free(mono_buf);
    vTaskDelete(NULL);
}

// ─────────────────────────────────────────
// Public API
// ─────────────────────────────────────────
esp_err_t audio_capture_init(void)
{
    static bool s_initialized = false;
    if (s_initialized) {
        ESP_LOGW(TAG, "audio_capture_init called twice — ignoring");
        return ESP_OK;
    }
    s_initialized = true;

    // ── I2S FIRST — ES8311 needs MCLK running when its registers are written ──
    // Matches the Waveshare example init order exactly.
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(
        I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    // Create BOTH TX and RX — same clock tree, same as working example
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_handle, &s_rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCK,
            .bclk = PIN_I2S_BCK,
            .ws   = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din  = PIN_I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = MCLK_MULTIPLE;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_handle));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_handle));

    // ── PA enable ──
    gpio_config_t pa_cfg = {
        .pin_bit_mask = 1ULL << GPIO_PA_ENABLE,
        .mode         = GPIO_MODE_OUTPUT,
        .intr_type    = GPIO_INTR_DISABLE,
        .pull_down_en = 0,
        .pull_up_en   = 0,
    };
    gpio_config(&pa_cfg);
    gpio_set_level(GPIO_PA_ENABLE, 1);

    // ── ES8311 AFTER I2S — MCLK now running, PLL can lock ──
    s_es_handle = es8311_create(I2C_PORT, ES8311_ADDRRES_0);
    if (!s_es_handle) {
        ESP_LOGE(TAG, "ES8311 create failed");
        return ESP_FAIL;
    }

    const es8311_clock_config_t clk = {
        .mclk_inverted      = false,
        .sclk_inverted      = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency     = (uint32_t)SAMPLE_RATE * MCLK_MULTIPLE,
        .sample_frequency   = SAMPLE_RATE,
    };
    ESP_ERROR_CHECK(es8311_init(s_es_handle, &clk,
        ES8311_RESOLUTION_16, ES8311_RESOLUTION_16));
    ESP_ERROR_CHECK(es8311_sample_frequency_config(s_es_handle,
        (uint32_t)SAMPLE_RATE * MCLK_MULTIPLE, SAMPLE_RATE));
    ESP_ERROR_CHECK(es8311_voice_volume_set(s_es_handle, 70, NULL));
    ESP_ERROR_CHECK(es8311_microphone_config(s_es_handle, false));
    ESP_ERROR_CHECK(es8311_microphone_gain_set(s_es_handle, ES8311_MIC_GAIN_42DB));

    xTaskCreatePinnedToCore(capture_task, "audio_cap",
        8192, NULL, 6, NULL, 1);

    ESP_LOGI(TAG, "Audio ready — %d Hz mono WAV to SD", SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t audio_capture_start(void)
{
    if (s_state != ACAP_IDLE) {
        ESP_LOGW(TAG, "Already recording");
        return ESP_ERR_INVALID_STATE;
    }

    if (!sd_card_mounted()) {
        ESP_LOGE(TAG, "SD card not mounted — cannot record");
        return ESP_FAIL;
    }

    sd_card_make_filename(s_filepath, sizeof(s_filepath));

    s_file = fopen(s_filepath, "wb");
    if (!s_file) {
        ESP_LOGE(TAG, "Cannot open %s (errno=%d: %s)",
            s_filepath, errno, strerror(errno));
        return ESP_FAIL;
    }

    // Placeholder WAV header — rewritten with real sizes on stop
    uint8_t placeholder[44] = {0};
    fwrite(placeholder, 1, 44, s_file);

    s_bytes_written = 0;
    s_duration_sec  = 0;
    s_done          = false;
    s_level         = 0;
    s_state         = ACAP_RUNNING;

    ESP_LOGI(TAG, "Recording → %s", s_filepath);
    return ESP_OK;
}

void audio_capture_stop(void)
{
    if (s_state == ACAP_RUNNING) s_state = ACAP_STOP_REQ;
}

void audio_capture_abort(void)
{
    acap_state_t prev = s_state;
    s_state = ACAP_IDLE;
    if (prev != ACAP_IDLE && s_file) {
        fclose(s_file);
        s_file = NULL;
        remove(s_filepath);
        ESP_LOGI(TAG, "Capture aborted");
    }
}

uint8_t audio_capture_get_level(void)
{
    return s_level;
}

bool audio_capture_poll_done(char *out_path, size_t len,
                              uint32_t *out_duration_sec)
{
    if (!s_done) return false;
    if (out_path)         strncpy(out_path, s_filepath, len);
    if (out_duration_sec) *out_duration_sec = s_duration_sec;
    return true;
}

bool audio_capture_is_idle(void)
{
    return (s_state == ACAP_IDLE);
}

i2s_chan_handle_t audio_capture_get_tx_handle(void)
{
    return s_tx_handle;
}