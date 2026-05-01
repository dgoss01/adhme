#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "app_state.h"
#include "display.h"
#include "rtc.h"
#include "nvs_flash.h"
#include "ui_home.h"
#include "ui_timeset.h"
#include "ui_quick_capture.h"
#include "audio_capture.h"
#include "sd_card.h"

static const char *TAG = "ADHMe";

// ─────────────────────────────────────────
// Globals
// ─────────────────────────────────────────
QueueHandle_t g_state_queue;
QueueHandle_t g_button_queue;
volatile adhme_state_t g_current_state = STATE_HOME;
static lv_obj_t *s_home_screen          = NULL;
static lv_obj_t *s_timeset_screen       = NULL;
static lv_obj_t *s_quick_capture_screen = NULL;
static adhme_state_t s_return_state     = STATE_HOME;

// ─────────────────────────────────────────
// State name lookup
// ─────────────────────────────────────────
const char* adhme_state_name(adhme_state_t state) {
    switch (state) {
        case STATE_HOME:          return "HOME";
        case STATE_LOCK_IN:       return "LOCK_IN";
        case STATE_CHECK_IN:      return "CHECK_IN";
        case STATE_CHECK_BACK:    return "CHECK_BACK";
        case STATE_DRIFT:         return "DRIFT";
        case STATE_ANCHOR:        return "ANCHOR";
        case STATE_SPARK:         return "SPARK";
        case STATE_WIND_DOWN:     return "WIND_DOWN";
        case STATE_QUICK_CAPTURE: return "QUICK_CAPTURE";
        case STATE_TIMESET:       return "TIMESET";
        default:                  return "UNKNOWN";
    }
}

void adhme_goto(adhme_state_t next_state) {
    adhme_state_msg_t msg = { .next_state = next_state };
    xQueueSend(g_state_queue, &msg, pdMS_TO_TICKS(10));
}

adhme_state_t adhme_get_return_state(void) {
    return s_return_state;
}

// ─────────────────────────────────────────
// Clock timer — fires every 30s
// ─────────────────────────────────────────
static void clock_timer_cb(void *arg)
{
    if (display_lock(10)) {
        ui_home_tick();
        display_unlock();
    }
}

// ─────────────────────────────────────────
// Button task (Core 1)
// ─────────────────────────────────────────
static void button_task(void *pvParameters) {
    adhme_button_event_t event;
    while (1) {
        if (xQueueReceive(g_button_queue, &event, portMAX_DELAY)) {
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
                    ESP_LOGI(TAG, "BOOT → context action");
                    break;
            }
        }
    }
}

// ─────────────────────────────────────────
// State machine task (Core 0)
// ─────────────────────────────────────────
static void state_task(void *pvParameters) {
    adhme_state_msg_t msg;
    while (1) {
        if (xQueueReceive(g_state_queue, &msg, portMAX_DELAY)) {
            if (msg.next_state != g_current_state) {
                ESP_LOGI(TAG, "State: %s → %s",
                    adhme_state_name(g_current_state),
                    adhme_state_name(msg.next_state));

                // Save return state before entering Quick Capture
                if (msg.next_state == STATE_QUICK_CAPTURE) {
                    s_return_state = g_current_state;
                }

                g_current_state = msg.next_state;

                if (display_lock(100)) {
                    switch (msg.next_state) {
                        case STATE_HOME:
                            lv_scr_load(s_home_screen);
                            break;
                        case STATE_TIMESET:
                            lv_scr_load(s_timeset_screen);
                            break;
                        case STATE_QUICK_CAPTURE:
                            ui_quick_capture_reset();
                            lv_scr_load(s_quick_capture_screen);
                            break;
                        default:
                            lv_scr_load(s_home_screen);
                            break;
                    }
                    display_unlock();
                }
            }
        }
    }
}

// ─────────────────────────────────────────
// SD mount task — dedicated stack, runs once at boot
// ─────────────────────────────────────────
static void sd_mount_task(void *arg)
{
    esp_err_t ret = sd_card_mount();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s — capture will not save",
            esp_err_to_name(ret));
    }
    vTaskDelete(NULL);
}

// ─────────────────────────────────────────
// Entry point
// ─────────────────────────────────────────
void app_main(void)
{
    ESP_LOGI(TAG, "ADHMe starting...");

    g_state_queue  = xQueueCreate(8, sizeof(adhme_state_msg_t));
    g_button_queue = xQueueCreate(8, sizeof(adhme_button_event_t));

    display_init();

    // Audio codec + I2S (I2C must be up first, display_init handles that)
    if (audio_capture_init() != ESP_OK) {
        ESP_LOGW(TAG, "Audio init failed — Quick Capture will be silent");
    }

    // Sync system time from RTC
    nvs_flash_init();
    rtc_sync_system_time();

    // Build screens
    if (display_lock(-1)) {
        s_home_screen          = ui_home_create();
        s_timeset_screen       = ui_timeset_create();
        s_quick_capture_screen = ui_quick_capture_create();
        lv_scr_load(s_home_screen);
        display_unlock();
    }

    // Mount SD card in background — pins are now correct so this
    // completes quickly without blocking LVGL or triggering WDT
    xTaskCreate(sd_mount_task, "sd_init", 8192, NULL, 3, NULL);

    // Clock timer — update every 30 seconds
    const esp_timer_create_args_t clock_args = {
        .callback = clock_timer_cb,
        .name = "clock"
    };
    esp_timer_handle_t clock_timer;
    ESP_ERROR_CHECK(esp_timer_create(&clock_args, &clock_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(clock_timer, 30 * 1000 * 1000));

    xTaskCreatePinnedToCore(button_task, "button",
        4096, NULL, 7, NULL, 1);
    xTaskCreatePinnedToCore(state_task, "state",
        16384, NULL, 5, NULL, 0);

    ESP_LOGI(TAG, "ADHMe running");
}