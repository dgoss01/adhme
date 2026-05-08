#include "tone_player.h"
#include "audio_capture.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s_std.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "tone";

// ─────────────────────────────────────────
// Config
// ─────────────────────────────────────────
#define SAMPLE_RATE      16000
#define TONE_AMPLITUDE   4000    // out of 32767 — soft, not startling
#define CHUNK_FRAMES     256     // frames per i2s_channel_write call

// ─────────────────────────────────────────
// Message queue
// ─────────────────────────────────────────
typedef struct {
    uint16_t freq_hz;
    uint16_t duration_ms;
    uint8_t  count;
    uint16_t gap_ms;
} tone_msg_t;

static QueueHandle_t s_tone_queue = NULL;
static bool          s_initialized = false;

// ─────────────────────────────────────────
// Sine table — 256 entries, one full cycle
// Generated once at init to avoid math in the hot path
// ─────────────────────────────────────────
static int16_t s_sine_table[256];

static void build_sine_table(void)
{
    for (int i = 0; i < 256; i++) {
        s_sine_table[i] = (int16_t)(TONE_AMPLITUDE *
            sinf(2.0f * (float)M_PI * i / 256.0f));
    }
}

// ─────────────────────────────────────────
// Play one click — blocking, called from tone_task
// Writes stereo interleaved PCM (L+R same sample) to TX handle
// ─────────────────────────────────────────
static void play_click(i2s_chan_handle_t tx,
    uint16_t freq_hz, uint16_t duration_ms)
{
    uint32_t total_frames = (uint32_t)SAMPLE_RATE * duration_ms / 1000;

    // Phase accumulator — avoids per-sample float division
    // phase_inc = 256 * freq_hz / SAMPLE_RATE (in 16.16 fixed point)
    uint32_t phase_inc = ((uint32_t)freq_hz << 8) / SAMPLE_RATE;
    uint32_t phase     = 0;

    // Apply a simple linear fade-in and fade-out envelope (10% each end)
    // to eliminate clicks at the start/stop of the tone itself
    uint32_t fade_frames = total_frames / 10;
    if (fade_frames < 1) fade_frames = 1;

    // Stereo buffer: 2 channels × 2 bytes × CHUNK_FRAMES
    int16_t buf[CHUNK_FRAMES * 2];
    uint32_t frames_done = 0;

    while (frames_done < total_frames) {
        uint32_t chunk = total_frames - frames_done;
        if (chunk > CHUNK_FRAMES) chunk = CHUNK_FRAMES;

        for (uint32_t i = 0; i < chunk; i++) {
            uint32_t f = frames_done + i;
            int16_t sample = s_sine_table[(phase >> 8) & 0xFF];

            // Envelope
            if (f < fade_frames) {
                sample = (int16_t)((int32_t)sample * f / fade_frames);
            } else if (f > total_frames - fade_frames) {
                uint32_t tail = total_frames - f;
                sample = (int16_t)((int32_t)sample * tail / fade_frames);
            }

            buf[i * 2]     = sample;   // L
            buf[i * 2 + 1] = sample;   // R

            phase += phase_inc;
        }

        size_t written = 0;
        i2s_channel_write(tx, buf,
            chunk * 2 * sizeof(int16_t),
            &written, pdMS_TO_TICKS(200));

        frames_done += chunk;
    }
}

// ─────────────────────────────────────────
// Tone task — Core 1, waits on queue
// ─────────────────────────────────────────
static void tone_task(void *arg)
{
    ESP_LOGI(TAG, "tone_task started");
    tone_msg_t msg;

    while (1) {
        if (xQueueReceive(s_tone_queue, &msg, portMAX_DELAY) != pdTRUE)
            continue;

        // Skip if capture is running — TX and RX share clock, writing
        // during capture would corrupt the recording.
        if (!audio_capture_is_idle()) {
            ESP_LOGD(TAG, "Skipping tone — capture active");
            continue;
        }

        i2s_chan_handle_t tx = audio_capture_get_tx_handle();
        if (!tx) {
            ESP_LOGW(TAG, "TX handle not available");
            continue;
        }

        for (uint8_t c = 0; c < msg.count; c++) {
            // Re-check idle before each click in a multi-click sequence
            if (!audio_capture_is_idle()) break;

            play_click(tx, msg.freq_hz, msg.duration_ms);

            if (c < msg.count - 1 && msg.gap_ms > 0) {
                vTaskDelay(pdMS_TO_TICKS(msg.gap_ms));
            }
        }

        // Write a short silence buffer to flush DMA and prevent
        // the last sample from buzzing on the speaker
        int16_t silence[CHUNK_FRAMES * 2] = {0};
        size_t written = 0;
        i2s_channel_write(tx, silence,
            sizeof(silence), &written, pdMS_TO_TICKS(50));
    }
}

// ─────────────────────────────────────────
// Public API
// ─────────────────────────────────────────
void tone_player_init(void)
{
    if (s_initialized) return;
    s_initialized = true;

    build_sine_table();

    s_tone_queue = xQueueCreate(4, sizeof(tone_msg_t));
    assert(s_tone_queue);

    // Core 1, lower priority than audio_cap (6) and btn_poll (3)
    // Wakes only when a tone is queued — no polling overhead
    xTaskCreatePinnedToCore(tone_task, "tone",
        4096, NULL, 2, NULL, 1);

    ESP_LOGI(TAG, "Tone player ready — %d Hz sample rate", SAMPLE_RATE);
}

void tone_play(uint16_t freq_hz, uint16_t duration_ms,
               uint8_t count, uint16_t gap_ms)
{
    if (!s_tone_queue) {
        ESP_LOGW(TAG, "tone_play called before tone_player_init");
        return;
    }
    tone_msg_t msg = {
        .freq_hz     = freq_hz,
        .duration_ms = duration_ms,
        .count       = count,
        .gap_ms      = gap_ms,
    };
    // Non-blocking send — if queue is full (another tone playing), drop it
    xQueueSend(s_tone_queue, &msg, 0);
}

void tone_play_double_click(void)
{
    // 180 Hz, 80 ms each, two clicks, 140 ms gap
    // Soft, low, clearly audible even with high-frequency hearing loss
    tone_play(180, 80, 2, 140);
}
