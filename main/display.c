#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_io_expander_tca9554.h"
#include "lvgl.h"
#include "esp_lcd_sh8601.h"
#include "esp_lcd_touch_ft5x06.h"
#include "display.h"

static const char *TAG = "display";

// ─────────────────────────────────────────
// Pin definitions
// ─────────────────────────────────────────
#define LCD_HOST            SPI2_HOST
#define TOUCH_HOST          I2C_NUM_0

#define PIN_LCD_CS          GPIO_NUM_12
#define PIN_LCD_PCLK        GPIO_NUM_11
#define PIN_LCD_DATA0       GPIO_NUM_4
#define PIN_LCD_DATA1       GPIO_NUM_5
#define PIN_LCD_DATA2       GPIO_NUM_6
#define PIN_LCD_DATA3       GPIO_NUM_7
#define PIN_LCD_RST         (-1)

#define PIN_TOUCH_SCL       GPIO_NUM_14
#define PIN_TOUCH_SDA       GPIO_NUM_15
#define PIN_TOUCH_RST       (-1)
#define PIN_TOUCH_INT       GPIO_NUM_21

// ─────────────────────────────────────────
// LVGL config
// ─────────────────────────────────────────
#if CONFIG_LV_COLOR_DEPTH == 32
#define LCD_BIT_PER_PIXEL   (24)
#else
#define LCD_BIT_PER_PIXEL   (16)
#endif

#define LVGL_BUF_HEIGHT     (ADHME_LCD_V_RES / 4)
#define LVGL_TICK_PERIOD_MS 2
#define LVGL_TASK_MAX_DELAY 500
#define LVGL_TASK_MIN_DELAY 1
#define LVGL_TASK_STACK     (6 * 1024)
#define LVGL_TASK_PRIORITY  2

// ─────────────────────────────────────────
// SH8601 init commands
// ─────────────────────────────────────────
static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x44, (uint8_t[]){0x01, 0xD1}, 2, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 10},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0x6F}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xBF}, 4, 0},
    {0x51, (uint8_t[]){0x00}, 1, 10},
    {0x29, (uint8_t[]){0x00}, 0, 10},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
};

// ─────────────────────────────────────────
// Statics
// ─────────────────────────────────────────
static SemaphoreHandle_t lvgl_mux = NULL;
static esp_lcd_touch_handle_t tp  = NULL;

// ─────────────────────────────────────────
// LVGL callbacks
// ─────────────────────────────────────────
static bool notify_flush_ready(esp_lcd_panel_io_handle_t io,
    esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_disp_drv_t *drv = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(drv);
    return false;
}

static void lvgl_flush_cb(lv_disp_drv_t *drv,
    const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
    esp_lcd_panel_draw_bitmap(panel,
        area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);
}

static void lvgl_rounder_cb(lv_disp_drv_t *drv, lv_area_t *area)
{
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

static void lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    esp_lcd_touch_handle_t touch = (esp_lcd_touch_handle_t)drv->user_data;
    uint16_t x, y;
    uint8_t cnt = 0;
    esp_lcd_touch_read_data(touch);
    bool pressed = esp_lcd_touch_get_coordinates(touch, &x, &y, NULL, &cnt, 1);
    if (pressed && cnt > 0) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

// ─────────────────────────────────────────
// LVGL task
// ─────────────────────────────────────────
static void lvgl_task(void *arg)
{
    ESP_LOGI(TAG, "LVGL task started");
    uint32_t delay_ms = LVGL_TASK_MAX_DELAY;
    while (1) {
        if (display_lock(-1)) {
            delay_ms = lv_timer_handler();
            display_unlock();
        }
        delay_ms = (delay_ms > LVGL_TASK_MAX_DELAY) ? LVGL_TASK_MAX_DELAY :
                   (delay_ms < LVGL_TASK_MIN_DELAY) ? LVGL_TASK_MIN_DELAY : delay_ms;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

// ─────────────────────────────────────────
// Public API
// ─────────────────────────────────────────
bool display_lock(int timeout_ms)
{
    assert(lvgl_mux);
    TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(lvgl_mux, ticks) == pdTRUE;
}

void display_unlock(void)
{
    assert(lvgl_mux);
    xSemaphoreGive(lvgl_mux);
}

void display_init(void)
{
    esp_log_level_set("lcd_panel.io.i2c", ESP_LOG_NONE);
    esp_log_level_set("FT5x06", ESP_LOG_NONE);

    static lv_disp_draw_buf_t disp_buf;
    static lv_disp_drv_t disp_drv;

    // ── I2C (touch + IO expander) ──
    ESP_LOGI(TAG, "Init I2C");
    const i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_TOUCH_SDA,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = PIN_TOUCH_SCL,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 200 * 1000,
    };
    ESP_ERROR_CHECK(i2c_param_config(TOUCH_HOST, &i2c_conf));
    ESP_ERROR_CHECK(i2c_driver_install(TOUCH_HOST, i2c_conf.mode, 0, 0, 0));

    // ── IO expander (powers display + touch) ──
    ESP_LOGI(TAG, "Init IO expander");
    esp_io_expander_handle_t expander = NULL;
    ESP_ERROR_CHECK(esp_io_expander_new_i2c_tca9554(
        TOUCH_HOST, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &expander));
    esp_io_expander_set_dir(expander,
        IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_2,
        IO_EXPANDER_OUTPUT);
    // Reset pulse
    esp_io_expander_set_level(expander, IO_EXPANDER_PIN_NUM_0, 0);
    esp_io_expander_set_level(expander, IO_EXPANDER_PIN_NUM_1, 0);
    esp_io_expander_set_level(expander, IO_EXPANDER_PIN_NUM_2, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_io_expander_set_level(expander, IO_EXPANDER_PIN_NUM_0, 1);
    esp_io_expander_set_level(expander, IO_EXPANDER_PIN_NUM_1, 1);
    esp_io_expander_set_level(expander, IO_EXPANDER_PIN_NUM_2, 1);

    // ── SPI bus ──
    ESP_LOGI(TAG, "Init SPI");
    const spi_bus_config_t buscfg = SH8601_PANEL_BUS_QSPI_CONFIG(
        PIN_LCD_PCLK, PIN_LCD_DATA0, PIN_LCD_DATA1,
        PIN_LCD_DATA2, PIN_LCD_DATA3,
        ADHME_LCD_H_RES * ADHME_LCD_V_RES * LCD_BIT_PER_PIXEL / 8);
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // ── Panel IO ──
    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    const esp_lcd_panel_io_spi_config_t io_config =
        SH8601_PANEL_IO_QSPI_CONFIG(PIN_LCD_CS, notify_flush_ready, &disp_drv);
    sh8601_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags = { .use_qspi_interface = 1 },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    // ── SH8601 panel driver ──
    ESP_LOGI(TAG, "Install SH8601 driver");
    esp_lcd_panel_handle_t panel = NULL;
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BIT_PER_PIXEL,
        .vendor_config = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(io_handle, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    // ── Touch ──
    ESP_LOGI(TAG, "Init touch");
    esp_lcd_panel_io_handle_t tp_io = NULL;
    const esp_lcd_panel_io_i2c_config_t tp_io_cfg =
        ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(
        (esp_lcd_i2c_bus_handle_t)TOUCH_HOST, &tp_io_cfg, &tp_io));
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = ADHME_LCD_H_RES,
        .y_max = ADHME_LCD_V_RES,
        .rst_gpio_num = PIN_TOUCH_RST,
        .int_gpio_num = PIN_TOUCH_INT,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_ft5x06(tp_io, &tp_cfg, &tp));

    // ── LVGL ──
    ESP_LOGI(TAG, "Init LVGL");
    lv_init();
    lv_color_t *buf1 = heap_caps_malloc(
        ADHME_LCD_H_RES * LVGL_BUF_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_DMA);
    lv_color_t *buf2 = heap_caps_malloc(
        ADHME_LCD_H_RES * LVGL_BUF_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf1 && buf2);
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2,
        ADHME_LCD_H_RES * LVGL_BUF_HEIGHT);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = ADHME_LCD_H_RES;
    disp_drv.ver_res = ADHME_LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.rounder_cb = lvgl_rounder_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel;
    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);

    // ── Touch input device ──
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.disp = disp;
    indev_drv.read_cb = lvgl_touch_cb;
    indev_drv.user_data = tp;
    lv_indev_drv_register(&indev_drv);

    // ── LVGL tick timer ──
    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer,
        LVGL_TICK_PERIOD_MS * 1000));

    // ── LVGL mutex + task ──
    lvgl_mux = xSemaphoreCreateMutex();
    assert(lvgl_mux);
    xTaskCreatePinnedToCore(lvgl_task, "lvgl",
        LVGL_TASK_STACK, NULL, LVGL_TASK_PRIORITY, NULL, 0);

    ESP_LOGI(TAG, "Display ready");
}
