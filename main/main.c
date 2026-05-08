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
#include "ui_lock_in.h"
#include "audio_capture.h"
#include "tone_player.h"
#include "sd_card.h"
#include "button.h"

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
static lv_obj_t *s_lock_in_screen       = NULL;

// Stub screens — replaced when each app is implemented
static lv_obj_t *s_check_in_screen     = NULL;
static lv_obj_t *s_drift_screen        = NULL;
static lv_obj_t *s_breathe_screen      = NULL;
static lv_obj_t *s_sand_screen         = NULL;
static lv_obj_t *s_spark_screen        = NULL;

static adhme_state_t s_return_state    = STATE_HOME;

// ─────────────────────────────────────────
// Stub screen builder
// ─────────────────────────────────────────
static lv_obj_t* build_stub_screen(const char *name, uint32_t accent)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x030a03), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, name);
    lv_obj_set_style_text_color(lbl, lv_color_hex(accent), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "coming soon");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x2e4e28), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 28);

    return scr;
}

// ─────────────────────────────────────────
// State name lookup
// ─────────────────────────────────────────
const char* adhme_state_name(adhme_state_t state) {
    switch (state) {
        case STATE_HOME:          return "HOME";
        case STATE_LOCK_IN:       return "LOCK_IN";
        case STATE_CHECK_IN:      return "CHECK_IN";
        case STATE_DRIFT:         return "DRIFT";
        case STATE_BREATHE:       return "BREATHE";
        case STATE_SAND:          return "SAND";
        case STATE_SPARK:         return "SPARK";
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

                if (display_lock(100)) {

                    // ── Pre-transition cleanup ──────────────────────────
                    // If we're heading to Quick Capture and Lock In is active,
                    // pause the timer so the session is resumable on return.
                    if (msg.next_state == STATE_QUICK_CAPTURE &&
                        g_current_state == STATE_LOCK_IN) {
                        ui_lock_in_pause();
                    }

                    // Save return state before entering Quick Capture
                    if (msg.next_state == STATE_QUICK_CAPTURE) {
                        s_return_state = g_current_state;
                    }

                    g_current_state = msg.next_state;

                    // ── Screen load ─────────────────────────────────────
                    switch (msg.next_state) {
                        case STATE_HOME:
                            lv_scr_load(s_home_screen);
                            break;
                        case STATE_LOCK_IN:
                            ui_lock_in_reset();
                            lv_scr_load(s_lock_in_screen);
                            break;
                        case STATE_CHECK_IN:
                            lv_scr_load(s_check_in_screen);
                            break;
                        case STATE_DRIFT:
                            lv_scr_load(s_drift_screen);
                            break;
                        case STATE_BREATHE:
                            lv_scr_load(s_breathe_screen);
                            break;
                        case STATE_SAND:
                            lv_scr_load(s_sand_screen);
                            break;
                        case STATE_SPARK:
                            lv_scr_load(s_spark_screen);
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

    // Audio codec + I2S — must come before tone_player_init()
    if (audio_capture_init() != ESP_OK) {
        ESP_LOGW(TAG, "Audio init failed — Quick Capture and tones disabled");
    }

    // Tone player — uses TX handle from audio_capture, init after
    tone_player_init();

    // Sync system time from RTC
    nvs_flash_init();
    rtc_sync_system_time();

    // Build screens
    if (display_lock(-1)) {
        s_home_screen          = ui_home_create();
        s_timeset_screen       = ui_timeset_create();
        s_quick_capture_screen = ui_quick_capture_create();
        s_lock_in_screen       = ui_lock_in_create();

        // Stub screens — replaced as each app is implemented
        s_check_in_screen = build_stub_screen("CHECK IN", 0xc4a840);
        s_drift_screen    = build_stub_screen("DRIFT",    0x6a9ab8);
        s_breathe_screen  = build_stub_screen("BREATHE",  0x7ab8a0);
        s_sand_screen     = build_stub_screen("SAND",     0xc4a840);
        s_spark_screen    = build_stub_screen("SPARK",    0x8b9a3d);

        lv_scr_load(s_home_screen);
        display_unlock();
    }

    // Mount SD card in background
    xTaskCreate(sd_mount_task, "sd_init", 8192, NULL, 3, NULL);

    // Clock timer — update every 30 seconds
    const esp_timer_create_args_t clock_args = {
        .callback = clock_timer_cb,
        .name = "clock"
    };
    esp_timer_handle_t clock_timer;
    ESP_ERROR_CHECK(esp_timer_create(&clock_args, &clock_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(clock_timer, 30 * 1000 * 1000));

    button_init();
    xTaskCreatePinnedToCore(state_task, "state",
        16384, NULL, 5, NULL, 0);

    ESP_LOGI(TAG, "ADHMe running");
}
