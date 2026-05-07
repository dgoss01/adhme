#include "button.h"
#include "app_state.h"
#include "display.h"
#include "driver/gpio.h"
#include "esp_io_expander.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "button";

#define BOOT_GPIO       GPIO_NUM_0

#define DEBOUNCE_MS          30
#define DOUBLE_TAP_WINDOW_MS 350
#define POLL_MS              50

typedef struct {
    bool     last_raw;
    bool     stable;
    uint32_t debounce_start;
    bool     debouncing;
    bool     pressed_pending;
    uint32_t press_time;
} btn_state_t;

static btn_state_t s_boot = {0};
static btn_state_t s_pwr  = {0};

static inline uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void enqueue(adhme_button_event_t ev)
{
    xQueueSend(g_button_queue, &ev, 0);
}

static void process_button(btn_state_t *b, bool raw_pressed,
    adhme_button_event_t ev_single, int ev_double)
{
    uint32_t t = now_ms();

    if (raw_pressed != b->last_raw) {
        b->last_raw       = raw_pressed;
        b->debounce_start = t;
        b->debouncing     = true;
    }

    if (b->debouncing && (t - b->debounce_start) >= DEBOUNCE_MS) {
        b->debouncing = false;
        bool confirmed = b->last_raw;
        if (confirmed && !b->stable) {
            if (ev_double >= 0 && b->pressed_pending &&
                (t - b->press_time) <= DOUBLE_TAP_WINDOW_MS)
            {
                b->pressed_pending = false;
                enqueue((adhme_button_event_t)ev_double);
            } else {
                b->pressed_pending = true;
                b->press_time      = t;
            }
        }
        b->stable = confirmed;
    }

    if (b->pressed_pending && (t - b->press_time) > DOUBLE_TAP_WINDOW_MS) {
        b->pressed_pending = false;
        enqueue(ev_single);
    }
}

// ─────────────────────────────────────────
// Poll task — detects button presses, enqueues events
// ─────────────────────────────────────────
static void button_poll_task(void *arg)
{
    ESP_LOGI(TAG, "button_poll_task started on core %d", xPortGetCoreID());

    // Configure BOOT button GPIO
    gpio_config_t boot_cfg = {
        .pin_bit_mask = 1ULL << BOOT_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&boot_cfg);

    // Fetch expander handle once — stable after display_init().
    // IO4 direction is already set to input by display.c; no re-init needed.
    esp_io_expander_handle_t expander = display_get_expander();
    if (!expander) {
        ESP_LOGE(TAG, "IO expander handle not available — btn_poll exiting");
        vTaskDelete(NULL);  // clean FreeRTOS task exit
        return;
    }

    ESP_LOGI(TAG, "Button poll using IO expander handle OK");

    while (1) {
        bool boot_pressed = (gpio_get_level(BOOT_GPIO) == 0);

        // Read PWR button level via expander API (correct signature)
        uint32_t pwr_level = 0;
        bool pwr_pressed  = false;
        if (esp_io_expander_get_level(expander,
                IO_EXPANDER_PIN_NUM_4, &pwr_level) == ESP_OK) {
            pwr_pressed = (pwr_level != 0);
        }

        process_button(&s_boot, boot_pressed, BTN_BOOT_SHORT, -1);
        process_button(&s_pwr,  pwr_pressed,  BTN_PWR_SHORT, (int)BTN_PWR_DOUBLE);

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

// ─────────────────────────────────────────
// Consumer task — reads g_button_queue, drives state transitions
// ─────────────────────────────────────────
static void button_event_task(void *arg)
{
    ESP_LOGI(TAG, "button_event_task started on core %d", xPortGetCoreID());
    adhme_button_event_t event;
    while (1) {
        if (xQueueReceive(g_button_queue, &event, portMAX_DELAY)) {
            ESP_LOGI(TAG, "event %d received", (int)event);
            switch (event) {
                case BTN_PWR_SHORT:
                    ESP_LOGI(TAG, "PWR short → HOME");
                    adhme_goto(STATE_HOME);
                    break;
                case BTN_PWR_DOUBLE:
                    ESP_LOGI(TAG, "PWR double → QUICK_CAPTURE");
                    adhme_goto(STATE_QUICK_CAPTURE);
                    break;
                case BTN_BOOT_SHORT:
                    ESP_LOGI(TAG, "BOOT → HOME");
                    adhme_goto(STATE_HOME);
                    break;
            }
        }
    }
}

// ─────────────────────────────────────────
// Public API
// ─────────────────────────────────────────
void button_init(void)
{
    // Poll task — Core 1, priority 3. Sleeps 50 ms between polls so it
    // doesn't need to be high-priority; keeps audio_cap (pri 6) dominant.
    xTaskCreatePinnedToCore(button_poll_task, "btn_poll",
        4096, NULL, 3, NULL, 1);

    // Event task — Core 1, priority 3. Moved off Core 0 so it can no
    // longer preempt the LVGL task (pri 2) or state_task (pri 5).
    // Only wakes on a queued button event, so co-existing with audio_cap
    // on Core 1 is fine — recording sessions are rare and brief.
    xTaskCreatePinnedToCore(button_event_task, "btn_event",
        4096, NULL, 3, NULL, 1);

    ESP_LOGI(TAG, "Button init OK");
}