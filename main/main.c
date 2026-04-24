#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "lvgl.h"
#include "app_state.h"
#include "display.h"

static const char *TAG = "ADHMe";

// ─────────────────────────────────────────
// Global handles
// ─────────────────────────────────────────
QueueHandle_t g_state_queue;
QueueHandle_t g_button_queue;
volatile adhme_state_t g_current_state = STATE_HOME;

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
        default:                  return "UNKNOWN";
    }
}

// ─────────────────────────────────────────
// Transition
// ─────────────────────────────────────────
void adhme_goto(adhme_state_t next_state) {
    adhme_state_msg_t msg = { .next_state = next_state };
    xQueueSend(g_state_queue, &msg, pdMS_TO_TICKS(10));
}

// ─────────────────────────────────────────
// First screen — forest home
// ─────────────────────────────────────────
static void draw_home_screen(void)
{
    lv_obj_t *scr = lv_scr_act();

    // True black background
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x030a03), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    // ADHMe label — forest green, centered
    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "ADHMe");
    lv_obj_set_style_text_color(label,
        lv_color_hex(0x7ab87a), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(label,
        &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
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
                g_current_state = msg.next_state;
                // TODO: switch LVGL screens per state
            }
        }
    }
}

// ─────────────────────────────────────────
// Entry point
// ─────────────────────────────────────────
void app_main(void)
{
    ESP_LOGI(TAG, "ADHMe starting...");

    // Queues
    g_state_queue  = xQueueCreate(8, sizeof(adhme_state_msg_t));
    g_button_queue = xQueueCreate(8, sizeof(adhme_button_event_t));

    // Display — must come before any lv_ calls
    display_init();

    // First screen
    if (display_lock(-1)) {
        draw_home_screen();
        display_unlock();
    }

    // Tasks
    xTaskCreatePinnedToCore(button_task, "button",
        4096, NULL, 7, NULL, 1);
    xTaskCreatePinnedToCore(state_task, "state",
        4096, NULL, 5, NULL, 0);

    ESP_LOGI(TAG, "ADHMe running — state: %s",
        adhme_state_name(g_current_state));
}
