#include "ui_timeset.h"
#include "ui_home.h"
#include "rtc.h"
#include "display.h"
#include "app_state.h"
#include "esp_log.h"
#include <time.h>
#include <string.h>

static const char *TAG = "ui_timeset";

// ─────────────────────────────────────────
// Palette
// ─────────────────────────────────────────
#define COL_BG        0x030a03
#define COL_TILE      0x0c1f0c
#define COL_BORDER    0x1a3218
#define COL_TEXT_HI   0xc8e0b4
#define COL_TEXT_MID  0x4a7a42
#define COL_TEXT_DIM  0x2e4e28
#define COL_ACCENT    0x3d8b46
#define COL_DST_ON    0x3d8b46
#define COL_DST_OFF   0x1a3218

// ─────────────────────────────────────────
// State
// ─────────────────────────────────────────
typedef enum {
    STEP_HOUR = 0,
    STEP_MIN,
    STEP_DAY,
    STEP_MON,
    STEP_YEAR,
    STEP_DST,
    STEP_COUNT
} set_step_t;

static set_step_t  s_step = STEP_HOUR;
static lv_obj_t   *s_rollers[5];   // H M D Mo Y
static lv_obj_t   *s_dst_btn;
static lv_obj_t   *s_dst_label;
static lv_obj_t   *s_next_btn;
static lv_obj_t   *s_next_label;
static lv_obj_t   *s_screen;
static int         s_dst_on = 0;

// ─────────────────────────────────────────
// Roller option strings
// ─────────────────────────────────────────
static const char *HOURS =
    "00\n01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n"
    "12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23";

static const char *MINUTES =
    "00\n01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n"
    "12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23\n"
    "24\n25\n26\n27\n28\n29\n30\n31\n32\n33\n34\n35\n"
    "36\n37\n38\n39\n40\n41\n42\n43\n44\n45\n46\n47\n"
    "48\n49\n50\n51\n52\n53\n54\n55\n56\n57\n58\n59";

static const char *DAYS =
    "01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12\n"
    "13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23\n24\n"
    "25\n26\n27\n28\n29\n30\n31";

static const char *MONTHS =
    "Jan\nFeb\nMar\nApr\nMay\nJun\n"
    "Jul\nAug\nSep\nOct\nNov\nDec";

static const char *YEARS =
    "2024\n2025\n2026\n2027\n2028\n2029\n2030";


// ─────────────────────────────────────────
// Highlight active roller
// ─────────────────────────────────────────
static void highlight_step(set_step_t step)
{
    for (int i = 0; i < 5; i++) {
        bool active = ((int)step == i);
        lv_obj_set_style_text_color(s_rollers[i],
            lv_color_hex(active ? COL_TEXT_HI : COL_TEXT_DIM), 
            LV_PART_SELECTED);
        lv_obj_set_style_border_color(s_rollers[i],
            lv_color_hex(active ? COL_ACCENT : COL_BORDER), 0);
        lv_obj_set_style_border_width(s_rollers[i],
            active ? 2 : 1, 0);
    }

    // DST step
    bool dst_active = (step == STEP_DST);
    lv_obj_set_style_border_color(s_dst_btn,
        lv_color_hex(dst_active ? COL_ACCENT : COL_BORDER), 0);
    lv_obj_set_style_border_width(s_dst_btn, dst_active ? 2 : 1, 0);

    // NEXT button label
    lv_label_set_text(s_next_label,
        (step == STEP_DST) ? "SAVE  \xE2\x9C\x93" : "NEXT  \xE2\x86\x92");
}

// ─────────────────────────────────────────
// Advance to next step on double-tap
// ─────────────────────────────────────────
static void advance_step(void)
{
    s_step = (set_step_t)(s_step + 1);

    if (s_step >= STEP_COUNT) {
        // All done — build tm and save
        struct tm t = {0};
        t.tm_hour = lv_roller_get_selected(s_rollers[STEP_HOUR]);
        t.tm_min  = lv_roller_get_selected(s_rollers[STEP_MIN]);
        t.tm_mday = lv_roller_get_selected(s_rollers[STEP_DAY]) + 1;
        t.tm_mon  = lv_roller_get_selected(s_rollers[STEP_MON]);
        t.tm_year = lv_roller_get_selected(s_rollers[STEP_YEAR]) + 124; // 2024=124
        t.tm_sec  = 0;
        mktime(&t);

        rtc_set_time(&t);
        rtc_set_dst_offset(s_dst_on ? 3600 : 0);
        rtc_sync_system_time();

        ESP_LOGI(TAG, "Time saved. Returning home.");
        adhme_goto(STATE_HOME);
        return;
    }

    highlight_step(s_step);
}

// ─────────────────────────────────────────
// NEXT button handler
// ─────────────────────────────────────────
static void next_btn_cb(lv_event_t *e)
{
    advance_step();
}

// ─────────────────────────────────────────
// DST toggle
// ─────────────────────────────────────────
static void dst_tap_cb(lv_event_t *e)
{
    if (s_step != STEP_DST) return;
    s_dst_on = !s_dst_on;
    lv_label_set_text(s_dst_label, s_dst_on ? "DST" : "STD");
    lv_obj_set_style_bg_color(s_dst_btn,
        lv_color_hex(s_dst_on ? 0x0f2a10 : COL_TILE), 0);
    lv_obj_set_style_text_color(s_dst_label,
        lv_color_hex(s_dst_on ? COL_ACCENT : COL_TEXT_DIM), 0);
}

// ─────────────────────────────────────────
// Build a single roller
// ─────────────────────────────────────────
static lv_obj_t* make_roller(lv_obj_t *parent,
    const char *opts, int initial, int width)
{
    lv_obj_t *r = lv_roller_create(parent);
    lv_roller_set_options(r, opts, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_selected(r, initial, LV_ANIM_OFF);
    lv_roller_set_visible_row_count(r, 3);
    lv_obj_set_width(r, width);

    // Style
    lv_obj_set_style_bg_color(r, lv_color_hex(COL_TILE), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_radius(r, 10, 0);
    lv_obj_set_style_text_color(r,
        lv_color_hex(COL_TEXT_DIM), LV_PART_MAIN);
    lv_obj_set_style_text_font(r,
        &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(r,
        lv_color_hex(COL_TEXT_DIM), LV_PART_SELECTED);
    lv_obj_set_style_bg_color(r,
        lv_color_hex(COL_ACCENT), LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(r,
        LV_OPA_20, LV_PART_SELECTED);

    return r;
}

// ─────────────────────────────────────────
// Build screen
// ─────────────────────────────────────────
lv_obj_t* ui_timeset_create(void)
{
    // Get current time to pre-populate rollers
    struct tm now = {0};
    rtc_get_time(&now);
    s_step = STEP_HOUR;
    s_dst_on = (rtc_get_dst_offset() == 3600);

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    // ── Header ──
    lv_obj_t *hdr = lv_label_create(s_screen);
    lv_label_set_text(hdr, "SET TIME & DATE");
    lv_obj_set_style_text_color(hdr, lv_color_hex(COL_TEXT_MID), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(hdr, 3, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 16);

    // ── Time row: HH : MM ──
    lv_obj_t *time_row = lv_obj_create(s_screen);
    lv_obj_set_size(time_row, 320, 110);
    lv_obj_align(time_row, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_opa(time_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(time_row, 0, 0);
    lv_obj_set_style_pad_all(time_row, 0, 0);
    lv_obj_clear_flag(time_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_row,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(time_row, 8, 0);

    s_rollers[STEP_HOUR] = make_roller(time_row, HOURS, now.tm_hour, 70);

    lv_obj_t *colon = lv_label_create(time_row);
    lv_label_set_text(colon, ":");
    lv_obj_set_style_text_color(colon, lv_color_hex(COL_TEXT_MID), 0);
    lv_obj_set_style_text_font(colon, &lv_font_montserrat_24, 0);

    s_rollers[STEP_MIN] = make_roller(time_row, MINUTES, now.tm_min, 70);

    // ── Date row: DD / Mon / YYYY ──
    lv_obj_t *date_row = lv_obj_create(s_screen);
    lv_obj_set_size(date_row, 320, 110);
    lv_obj_align(date_row, LV_ALIGN_TOP_MID, 0, 158);
    lv_obj_set_style_bg_opa(date_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(date_row, 0, 0);
    lv_obj_set_style_pad_all(date_row, 0, 0);
    lv_obj_clear_flag(date_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(date_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(date_row,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(date_row, 6, 0);

    s_rollers[STEP_DAY] = make_roller(date_row, DAYS,
        now.tm_mday - 1, 58);

    lv_obj_t *slash1 = lv_label_create(date_row);
    lv_label_set_text(slash1, "/");
    lv_obj_set_style_text_color(slash1, lv_color_hex(COL_TEXT_MID), 0);
    lv_obj_set_style_text_font(slash1, &lv_font_montserrat_16, 0);

    s_rollers[STEP_MON] = make_roller(date_row, MONTHS,
        now.tm_mon, 72);

    lv_obj_t *slash2 = lv_label_create(date_row);
    lv_label_set_text(slash2, "/");
    lv_obj_set_style_text_color(slash2, lv_color_hex(COL_TEXT_MID), 0);
    lv_obj_set_style_text_font(slash2, &lv_font_montserrat_16, 0);

    int year_idx = (now.tm_year + 1900) - 2024;
    if (year_idx < 0) year_idx = 0;
    if (year_idx > 6) year_idx = 6;
    s_rollers[STEP_YEAR] = make_roller(date_row, YEARS, year_idx, 74);

    // ── DST toggle ──
    s_dst_btn = lv_obj_create(s_screen);
    lv_obj_set_size(s_dst_btn, 120, 44);
    lv_obj_align(s_dst_btn, LV_ALIGN_TOP_MID, 0, 278);
    lv_obj_set_style_bg_color(s_dst_btn,
        lv_color_hex(s_dst_on ? 0x0f2a10 : COL_TILE), 0);
    lv_obj_set_style_bg_opa(s_dst_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_dst_btn,
        lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_border_width(s_dst_btn, 1, 0);
    lv_obj_set_style_radius(s_dst_btn, 12, 0);
    lv_obj_clear_flag(s_dst_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_dst_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_dst_btn, dst_tap_cb, LV_EVENT_CLICKED, NULL);

    s_dst_label = lv_label_create(s_dst_btn);
    lv_label_set_text(s_dst_label, s_dst_on ? "DST" : "STD");
    lv_obj_set_style_text_color(s_dst_label,
        lv_color_hex(s_dst_on ? COL_ACCENT : COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(s_dst_label,
        &lv_font_montserrat_20, 0);
    lv_obj_center(s_dst_label);

    // ── NEXT / SAVE button ──
    s_next_btn = lv_obj_create(s_screen);
    lv_obj_set_size(s_next_btn, 320, 52);
    lv_obj_align(s_next_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(s_next_btn, lv_color_hex(0x0f2a10), 0);
    lv_obj_set_style_bg_opa(s_next_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_next_btn, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_width(s_next_btn, 1, 0);
    lv_obj_set_style_radius(s_next_btn, 14, 0);
    lv_obj_set_style_pad_all(s_next_btn, 0, 0);
    lv_obj_clear_flag(s_next_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_next_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_next_btn, next_btn_cb, LV_EVENT_CLICKED, NULL);

    s_next_label = lv_label_create(s_next_btn);
    lv_label_set_text(s_next_label, "NEXT  \xE2\x86\x92");
    lv_obj_set_style_text_color(s_next_label, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(s_next_label, &lv_font_montserrat_20, 0);
    lv_obj_center(s_next_label);

    // Initial highlight
    highlight_step(STEP_HOUR);

    return s_screen;
}