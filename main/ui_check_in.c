#include "ui_check_in.h"
#include "app_state.h"
#include "display.h"
#include "sd_card.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "ui_check_in";

// ─────────────────────────────────────────
// Forest palette — base UI colors
// ─────────────────────────────────────────
#define COL_BG          0x030a03
#define COL_TILE        0x0c1f0c
#define COL_BORDER      0x1a3218
#define COL_TEXT_HI     0xc8e0b4
#define COL_TEXT_MID    0x4a7a42
#define COL_TEXT_DIM    0x2e4e28
#define COL_ACCENT      0x3d8b46
#define COL_TRACK       0x1a3218

// ─────────────────────────────────────────
// CSV path
// ─────────────────────────────────────────
#define CHECKIN_CSV_PATH    SD_DIR_CHECKINS "/checkin.csv"
#define CHECKIN_CSV_HEADER  "timestamp,energy,mood,focus,body\n"

// ─────────────────────────────────────────
// Auto-return delay (ms) on confirmation
// ─────────────────────────────────────────
#define CONFIRM_RETURN_MS  2000

// ─────────────────────────────────────────
// Page enum
// ─────────────────────────────────────────
typedef enum { CI_SLIDERS = 0, CI_CONFIRM } ci_page_t;

// ─────────────────────────────────────────
// Slider definitions — each row carries its own color.
// Order matches CSV column order.
// ─────────────────────────────────────────
typedef struct {
    const char *label;
    const char *lo_word;
    const char *hi_word;
    uint32_t    color;     // indicator + knob fill
} slider_def_t;

static const slider_def_t SLIDERS[] = {
    { "ENERGY", "low",       "high",   0xd4a73d },   // warm amber
    { "MOOD",   "dark",      "bright", 0x6aa8c4 },   // soft sky blue
    { "FOCUS",  "scattered", "locked", 0x7ac442 },   // spring green
    { "BODY",   "tense",     "easy",   0xc47aa8 },   // warm rose
};
#define SLIDER_COUNT  4

// ─────────────────────────────────────────
// Layout — sliders page
// ─────────────────────────────────────────
#define PAGE_W           368
#define PAGE_H           448
#define ROW_TOP_FIRST     62
#define ROW_HEIGHT        66
#define LABEL_X           24
#define LO_WORD_X         24
#define LO_WORD_W         60
#define HI_WORD_X        284
#define HI_WORD_W         60
#define SLIDER_X          90
#define SLIDER_W         188
#define SLIDER_H          16
#define SLIDER_Y_OFFSET   24

#define BTN_W            320
#define BTN_H             48
#define BTN_Y            382

// ─────────────────────────────────────────
// Layout — recap mini-bars on confirm page
// ─────────────────────────────────────────
#define RECAP_TOP        260       // y where the first mini-bar label sits
#define RECAP_ROW_H       22       // vertical spacing between mini-bars
#define RECAP_LABEL_X     90
#define RECAP_LABEL_W     56
#define RECAP_BAR_X      150
#define RECAP_BAR_W      130
#define RECAP_BAR_H        3
#define RECAP_BAR_Y_OFF    7       // y offset of bar within row (centered on label)
#define RECAP_KNOB_DIAM    7

// ─────────────────────────────────────────
// Static UI state
// ─────────────────────────────────────────
static ci_page_t   s_page         = CI_SLIDERS;
static lv_obj_t   *s_screen       = NULL;
static lv_obj_t   *s_sliders_page = NULL;
static lv_obj_t   *s_confirm_page = NULL;
static lv_obj_t   *s_sliders[SLIDER_COUNT];
static lv_obj_t   *s_confirm_time = NULL;
static lv_timer_t *s_return_timer = NULL;

// Confirm-page recap mini-bars — knob position updated on each LOG IT
static lv_obj_t   *s_recap_indicator[SLIDER_COUNT];
static lv_obj_t   *s_recap_knob[SLIDER_COUNT];

// ─────────────────────────────────────────
// Per-slider styles — one set per row, same shape, different color.
// Built lazily on first ui_check_in_create() call.
// ─────────────────────────────────────────
static lv_style_t s_slider_main;                       // shared across all sliders (track)
static lv_style_t s_slider_indicator[SLIDER_COUNT];    // one per row (colored fill)
static lv_style_t s_slider_knob[SLIDER_COUNT];         // one per row (colored knob)
static bool       s_styles_inited = false;

static void init_slider_styles(void)
{
    if (s_styles_inited) return;

    // Shared track style
    lv_style_init(&s_slider_main);
    lv_style_set_bg_opa(&s_slider_main, LV_OPA_COVER);
    lv_style_set_bg_color(&s_slider_main, lv_color_hex(COL_TRACK));
    lv_style_set_radius(&s_slider_main, 2);
    // Negative vertical pad lets the knob overflow the thin track
    lv_style_set_pad_ver(&s_slider_main, -2);

    // One indicator + knob style per slider, colored per definition
    for (int i = 0; i < SLIDER_COUNT; i++) {
        lv_style_init(&s_slider_indicator[i]);
        lv_style_set_bg_opa(&s_slider_indicator[i], LV_OPA_COVER);
        lv_style_set_bg_color(&s_slider_indicator[i], lv_color_hex(SLIDERS[i].color));
        lv_style_set_radius(&s_slider_indicator[i], 2);

        lv_style_init(&s_slider_knob[i]);
        lv_style_set_bg_opa(&s_slider_knob[i], LV_OPA_COVER);
        lv_style_set_bg_color(&s_slider_knob[i], lv_color_hex(SLIDERS[i].color));
        lv_style_set_border_color(&s_slider_knob[i], lv_color_hex(COL_TEXT_HI));
        lv_style_set_border_width(&s_slider_knob[i], 2);
        lv_style_set_radius(&s_slider_knob[i], LV_RADIUS_CIRCLE);
        lv_style_set_pad_all(&s_slider_knob[i], 6);
    }

    s_styles_inited = true;
}

// ─────────────────────────────────────────
// Helper — make a fixed-size page that fills the screen
// ─────────────────────────────────────────
static lv_obj_t* make_page(lv_obj_t *parent)
{
    lv_obj_t *pg = lv_obj_create(parent);
    lv_obj_set_size(pg, PAGE_W, PAGE_H);
    lv_obj_set_pos(pg, 0, 0);
    lv_obj_set_style_bg_opa(pg, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pg, 0, 0);
    lv_obj_set_style_pad_all(pg, 0, 0);
    lv_obj_clear_flag(pg, LV_OBJ_FLAG_SCROLLABLE);
    return pg;
}

// ─────────────────────────────────────────
// Page switch
// ─────────────────────────────────────────
static void show_page(ci_page_t page)
{
    lv_obj_add_flag(s_sliders_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_confirm_page, LV_OBJ_FLAG_HIDDEN);
    switch (page) {
        case CI_SLIDERS: lv_obj_clear_flag(s_sliders_page, LV_OBJ_FLAG_HIDDEN); break;
        case CI_CONFIRM: lv_obj_clear_flag(s_confirm_page, LV_OBJ_FLAG_HIDDEN); break;
    }
    s_page = page;
}

// ─────────────────────────────────────────
// Append a row to the CSV
// ─────────────────────────────────────────
static esp_err_t append_checkin_row(int e, int m, int f, int b)
{
    if (!sd_card_mounted()) {
        ESP_LOGW(TAG, "SD not mounted — skipping CSV write");
        return ESP_ERR_INVALID_STATE;
    }

    if (sd_card_ensure_dir(SD_DIR_CHECKINS) != ESP_OK) {
        ESP_LOGE(TAG, "Could not ensure %s exists", SD_DIR_CHECKINS);
        return ESP_FAIL;
    }

    bool need_header = false;
    FILE *probe = fopen(CHECKIN_CSV_PATH, "r");
    if (probe == NULL) {
        need_header = true;
    } else {
        fclose(probe);
    }

    FILE *f_out = fopen(CHECKIN_CSV_PATH, "a");
    if (f_out == NULL) {
        ESP_LOGE(TAG, "fopen(%s) failed", CHECKIN_CSV_PATH);
        return ESP_FAIL;
    }

    if (need_header) {
        fputs(CHECKIN_CSV_HEADER, f_out);
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm *t = localtime(&tv.tv_sec);
    char ts[24];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", t);

    fprintf(f_out, "%s,%d,%d,%d,%d\n", ts, e, m, f, b);
    fclose(f_out);

    ESP_LOGI(TAG, "Logged: %s,%d,%d,%d,%d", ts, e, m, f, b);
    return ESP_OK;
}

// ─────────────────────────────────────────
// Update a recap mini-bar to reflect a logged value (0–100)
// ─────────────────────────────────────────
static void update_recap_bar(int idx, int val)
{
    if (val < 0)   val = 0;
    if (val > 100) val = 100;

    // Indicator fill width = proportion of bar
    int fill_w = (val * RECAP_BAR_W) / 100;
    if (fill_w < 1) fill_w = 1;
    lv_obj_set_width(s_recap_indicator[idx], fill_w);

    // Knob position — left edge centered on value point
    int row_y   = RECAP_TOP + idx * RECAP_ROW_H;
    int knob_x  = RECAP_BAR_X + (val * RECAP_BAR_W / 100) - (RECAP_KNOB_DIAM / 2);
    int knob_y  = row_y + RECAP_BAR_Y_OFF + (RECAP_BAR_H / 2) - (RECAP_KNOB_DIAM / 2);
    lv_obj_set_pos(s_recap_knob[idx], knob_x, knob_y);
}

// ─────────────────────────────────────────
// Auto-return timer
// ─────────────────────────────────────────
static void return_timer_cb(lv_timer_t *t)
{
    lv_timer_pause(s_return_timer);
    adhme_goto(STATE_HOME);
}

// ─────────────────────────────────────────
// LOG IT button handler
// ─────────────────────────────────────────
static void log_it_cb(lv_event_t *e)
{
    int vals[SLIDER_COUNT];
    for (int i = 0; i < SLIDER_COUNT; i++) {
        vals[i] = lv_slider_get_value(s_sliders[i]);
        update_recap_bar(i, vals[i]);
    }

    append_checkin_row(vals[0], vals[1], vals[2], vals[3]);

    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm *t = localtime(&tv.tv_sec);
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", t->tm_hour, t->tm_min);
    lv_label_set_text(s_confirm_time, buf);

    show_page(CI_CONFIRM);

    lv_timer_set_period(s_return_timer, CONFIRM_RETURN_MS);
    lv_timer_reset(s_return_timer);
    lv_timer_resume(s_return_timer);
}

// ─────────────────────────────────────────
// Build a single slider row directly on the page
// ─────────────────────────────────────────
static lv_obj_t* build_slider_row(lv_obj_t *page, int idx)
{
    const slider_def_t *def = &SLIDERS[idx];
    int y = ROW_TOP_FIRST + idx * ROW_HEIGHT;

    // Row title — colored to match its slider, for instant pairing
    lv_obj_t *lbl = lv_label_create(page);
    lv_label_set_text(lbl, def->label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(def->color), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(lbl, 2, 0);
    lv_obj_set_pos(lbl, LABEL_X, y);

    // Lo endpoint word
    lv_obj_t *lo = lv_label_create(page);
    lv_label_set_text(lo, def->lo_word);
    lv_obj_set_style_text_color(lo, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lo, &lv_font_montserrat_12, 0);
    lv_obj_set_width(lo, LO_WORD_W);
    lv_obj_set_style_text_align(lo, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(lo, LO_WORD_X, y + SLIDER_Y_OFFSET);

    // Hi endpoint word
    lv_obj_t *hi = lv_label_create(page);
    lv_label_set_text(hi, def->hi_word);
    lv_obj_set_style_text_color(hi, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(hi, &lv_font_montserrat_12, 0);
    lv_obj_set_width(hi, HI_WORD_W);
    lv_obj_set_style_text_align(hi, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(hi, HI_WORD_X, y + SLIDER_Y_OFFSET);

    // Slider — slider_2 pattern, with this row's color styles
    lv_obj_t *sl = lv_slider_create(page);
    lv_obj_remove_style_all(sl);
    lv_obj_add_style(sl, &s_slider_main,           LV_PART_MAIN);
    lv_obj_add_style(sl, &s_slider_indicator[idx], LV_PART_INDICATOR);
    lv_obj_add_style(sl, &s_slider_knob[idx],      LV_PART_KNOB);

    lv_obj_set_size(sl, SLIDER_W, SLIDER_H);
    lv_obj_set_pos(sl, SLIDER_X, y + SLIDER_Y_OFFSET - 6);
    lv_slider_set_range(sl, 0, 100);
    lv_slider_set_value(sl, 50, LV_ANIM_OFF);

    return sl;
}

// ─────────────────────────────────────────
// Build a single recap row on the confirm page
// ─────────────────────────────────────────
static void build_recap_row(lv_obj_t *page, int idx)
{
    const slider_def_t *def = &SLIDERS[idx];
    int y = RECAP_TOP + idx * RECAP_ROW_H;

    // Tiny label, colored
    lv_obj_t *lbl = lv_label_create(page);
    lv_label_set_text(lbl, def->label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(def->color), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(lbl, 2, 0);
    lv_obj_set_width(lbl, RECAP_LABEL_W);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(lbl, RECAP_LABEL_X, y);

    // Track — full width, dim
    lv_obj_t *track = lv_obj_create(page);
    lv_obj_set_size(track, RECAP_BAR_W, RECAP_BAR_H);
    lv_obj_set_pos(track, RECAP_BAR_X, y + RECAP_BAR_Y_OFF);
    lv_obj_set_style_bg_color(track, lv_color_hex(COL_TRACK), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(track, 0, 0);
    lv_obj_set_style_radius(track, 1, 0);
    lv_obj_set_style_pad_all(track, 0, 0);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_CLICKABLE);

    // Indicator — colored, width set on each LOG IT
    lv_obj_t *ind = lv_obj_create(page);
    lv_obj_set_size(ind, 1, RECAP_BAR_H);     // initial width replaced in update_recap_bar
    lv_obj_set_pos(ind, RECAP_BAR_X, y + RECAP_BAR_Y_OFF);
    lv_obj_set_style_bg_color(ind, lv_color_hex(def->color), 0);
    lv_obj_set_style_bg_opa(ind, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ind, 0, 0);
    lv_obj_set_style_radius(ind, 1, 0);
    lv_obj_set_style_pad_all(ind, 0, 0);
    lv_obj_clear_flag(ind, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ind, LV_OBJ_FLAG_CLICKABLE);

    // Tiny dot at the value position
    lv_obj_t *knob = lv_obj_create(page);
    lv_obj_set_size(knob, RECAP_KNOB_DIAM, RECAP_KNOB_DIAM);
    lv_obj_set_style_bg_color(knob, lv_color_hex(def->color), 0);
    lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(knob, 0, 0);
    lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(knob, 0, 0);
    lv_obj_clear_flag(knob, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(knob, LV_OBJ_FLAG_CLICKABLE);

    s_recap_indicator[idx] = ind;
    s_recap_knob[idx]      = knob;
}

// ─────────────────────────────────────────
// Build: Sliders page
// ─────────────────────────────────────────
static lv_obj_t* build_sliders_page(lv_obj_t *parent)
{
    lv_obj_t *pg = make_page(parent);

    lv_obj_t *hdr = lv_label_create(pg);
    lv_label_set_text(hdr, "CHECK IN");
    lv_obj_set_style_text_color(hdr, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(hdr, 4, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 18);

    lv_obj_t *sub = lv_label_create(pg);
    lv_label_set_text(sub, "right now, in this moment");
    lv_obj_set_style_text_color(sub, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 36);

    for (int i = 0; i < SLIDER_COUNT; i++) {
        s_sliders[i] = build_slider_row(pg, i);
    }

    // LOG IT button — forest green, action surface
    lv_obj_t *btn = lv_obj_create(pg);
    lv_obj_set_size(btn, BTN_W, BTN_H);
    lv_obj_set_pos(btn, (PAGE_W - BTN_W) / 2, BTN_Y);
    lv_obj_set_style_border_color(btn, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, log_it_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "LOG IT");
    lv_obj_set_style_text_color(btn_lbl, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_letter_space(btn_lbl, 4, 0);
    lv_obj_center(btn_lbl);

    return pg;
}

// ─────────────────────────────────────────
// Build: Confirmation page
// ─────────────────────────────────────────
static lv_obj_t* build_confirm_page(lv_obj_t *parent)
{
    lv_obj_t *pg = make_page(parent);

    // Check ring — forest green, the celebratory note
    lv_obj_t *ring = lv_obj_create(pg);
    lv_obj_set_size(ring, 80, 80);
    lv_obj_align(ring, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(ring, 0, 0);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *check = lv_label_create(ring);
    lv_label_set_text(check, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(check, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_font(check, &lv_font_montserrat_24, 0);
    lv_obj_center(check);

    // "LOGGED"
    lv_obj_t *logged = lv_label_create(pg);
    lv_label_set_text(logged, "LOGGED");
    lv_obj_set_style_text_color(logged, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(logged, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_letter_space(logged, 4, 0);
    lv_obj_align(logged, LV_ALIGN_TOP_MID, 0, 156);

    // Time stamp
    s_confirm_time = lv_label_create(pg);
    lv_label_set_text(s_confirm_time, "--:--");
    lv_obj_set_style_text_color(s_confirm_time, lv_color_hex(COL_TEXT_MID), 0);
    lv_obj_set_style_text_font(s_confirm_time, &lv_font_montserrat_16, 0);
    lv_obj_align(s_confirm_time, LV_ALIGN_TOP_MID, 0, 188);

    // Recap mini-bars
    for (int i = 0; i < SLIDER_COUNT; i++) {
        build_recap_row(pg, i);
    }

    // "returning home..."
    lv_obj_t *footer = lv_label_create(pg);
    lv_label_set_text(footer, "returning home...");
    lv_obj_set_style_text_color(footer, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(footer, &lv_font_montserrat_12, 0);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -32);

    return pg;
}

// ─────────────────────────────────────────
// Public API
// ─────────────────────────────────────────
lv_obj_t* ui_check_in_create(void)
{
    init_slider_styles();   // must come before any slider is created

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    s_sliders_page = build_sliders_page(s_screen);
    s_confirm_page = build_confirm_page(s_screen);

    s_return_timer = lv_timer_create(return_timer_cb, CONFIRM_RETURN_MS, NULL);
    lv_timer_pause(s_return_timer);

    show_page(CI_SLIDERS);

    ESP_LOGI(TAG, "Check In ready");
    return s_screen;
}

void ui_check_in_reset(void)
{
    if (s_return_timer) lv_timer_pause(s_return_timer);

    for (int i = 0; i < SLIDER_COUNT; i++) {
        lv_slider_set_value(s_sliders[i], 50, LV_ANIM_OFF);
    }

    show_page(CI_SLIDERS);
}
