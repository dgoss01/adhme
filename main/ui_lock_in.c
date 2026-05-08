#include "ui_lock_in.h"
#include "app_state.h"
#include "display.h"
#include "tone_player.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_lock_in";

// ─────────────────────────────────────────
// Forest palette
// ─────────────────────────────────────────
#define COL_BG          0x030a03
#define COL_TILE        0x0c1f0c
#define COL_BORDER      0x1a3218
#define COL_TEXT_HI     0xc8e0b4
#define COL_TEXT_MID    0x4a7a42
#define COL_TEXT_DIM    0x2e4e28
#define COL_ACCENT      0x3d8b46
#define COL_ARC_TRACK   0x0c1f0c
#define COL_ARC_FILL    0x3d8b46
#define COL_CTRL_DIM    0x1a3218

// ─────────────────────────────────────────
// Config — work duration presets (minutes)
// ─────────────────────────────────────────
static const uint8_t WORK_DURATIONS[]  = {10, 15, 25, 45, 60};
static const uint8_t WORK_DUR_COUNT    = 5;
static const uint8_t BREAK_DURATIONS[] = {0, 5, 10};   // 0 = none
static const uint8_t BREAK_DUR_COUNT   = 3;

// ─────────────────────────────────────────
// Page enum
// ─────────────────────────────────────────
typedef enum { LI_SETUP = 0, LI_ACTIVE, LI_NUDGE, LI_DONE } li_page_t;

// ─────────────────────────────────────────
// Session state
// ─────────────────────────────────────────
typedef struct {
    uint8_t  work_idx;          // index into WORK_DURATIONS
    uint8_t  break_idx;         // index into BREAK_DURATIONS
    uint32_t remaining_sec;     // seconds left in current work arc
    uint32_t total_sec;         // total seconds for current arc (= WORK_DURATIONS[work_idx]*60)
    uint32_t rounds_done;       // completed arcs this session
    uint32_t session_elapsed;   // total seconds elapsed across all arcs
    bool     paused;            // true if paused — resumable
    bool     in_session;        // true if a session is active or paused
} li_session_t;

static li_session_t   s_sess    = {0};
static li_page_t      s_page    = LI_SETUP;
static lv_obj_t      *s_screen  = NULL;
static lv_obj_t      *s_setup_page = NULL;
static lv_obj_t      *s_active_page = NULL;
static lv_obj_t      *s_nudge_overlay = NULL;
static lv_obj_t      *s_done_page = NULL;

// Setup page widgets
static lv_obj_t      *s_work_chips[5];
static lv_obj_t      *s_break_chips[3];
static lv_obj_t      *s_lock_btn;
static lv_obj_t      *s_lock_lbl;
static lv_obj_t      *s_frozen_label;   // shows "24:12 remaining" when paused

// Active page widgets
static lv_obj_t      *s_arc;
static lv_obj_t      *s_timer_label;
static lv_obj_t      *s_round_label;
static lv_obj_t      *s_pause_btn;
static lv_obj_t      *s_stop_btn;

// Nudge overlay widgets
static lv_obj_t      *s_nudge_round_lbl;
static lv_obj_t      *s_nudge_break_lbl;

// Done page widgets
static lv_obj_t      *s_done_rounds;
static lv_obj_t      *s_done_time;

// LVGL timer — fires every second, drives countdown
static lv_timer_t    *s_tick_timer = NULL;

// ─────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────
static void fmt_time(char *buf, size_t len, uint32_t sec)
{
    uint32_t m = sec / 60;
    uint32_t s = sec % 60;
    snprintf(buf, len, "%lu:%02lu", (unsigned long)m, (unsigned long)s);
}

static void fmt_elapsed(char *buf, size_t len, uint32_t sec)
{
    uint32_t h = sec / 3600;
    uint32_t m = (sec % 3600) / 60;
    if (h > 0)
        snprintf(buf, len, "%luh %lum total", (unsigned long)h, (unsigned long)m);
    else
        snprintf(buf, len, "%lu min total", (unsigned long)m);
}

// Arc angle: LVGL arc goes 0–360. We map remaining time to angle.
// Full arc (arc at start) = 360 deg filled. Empty (done) = 0 deg filled.
static int16_t time_to_arc_angle(uint32_t remaining, uint32_t total)
{
    if (total == 0) return 0;
    return (int16_t)((uint32_t)360 * remaining / total);
}

static void update_arc(void)
{
    int16_t angle = time_to_arc_angle(s_sess.remaining_sec, s_sess.total_sec);
    // LVGL arc: end_angle relative to start (270 = top). We sweep clockwise.
    // Arc drawn from 270 → 270+angle.
    lv_arc_set_value(s_arc, angle);
}

static void update_timer_label(void)
{
    char buf[8];
    fmt_time(buf, sizeof(buf), s_sess.remaining_sec);
    lv_label_set_text(s_timer_label, buf);
}

static void update_round_label(void)
{
    char buf[20];
    snprintf(buf, sizeof(buf), "round %lu",
        (unsigned long)(s_sess.rounds_done + 1));
    lv_label_set_text(s_round_label, buf);
}

// ─────────────────────────────────────────
// Page switching
// ─────────────────────────────────────────
static void show_page(li_page_t page)
{
    lv_obj_add_flag(s_setup_page,     LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_active_page,    LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_nudge_overlay,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_done_page,      LV_OBJ_FLAG_HIDDEN);

    switch (page) {
        case LI_SETUP:
            lv_obj_clear_flag(s_setup_page, LV_OBJ_FLAG_HIDDEN);
            break;
        case LI_ACTIVE:
            lv_obj_clear_flag(s_active_page, LV_OBJ_FLAG_HIDDEN);
            // Nudge may show on top of active — do NOT show it here
            break;
        case LI_NUDGE:
            lv_obj_clear_flag(s_active_page, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_nudge_overlay, LV_OBJ_FLAG_HIDDEN);
            break;
        case LI_DONE:
            lv_obj_clear_flag(s_done_page, LV_OBJ_FLAG_HIDDEN);
            break;
    }
    s_page = page;
}

// ─────────────────────────────────────────
// Chip selection helpers
// ─────────────────────────────────────────
static void select_work_chip(uint8_t idx)
{
    for (uint8_t i = 0; i < WORK_DUR_COUNT; i++) {
        lv_obj_set_style_border_color(s_work_chips[i],
            lv_color_hex(i == idx ? COL_ACCENT : COL_BORDER), 0);
        lv_obj_set_style_bg_color(s_work_chips[i],
            lv_color_hex(i == idx ? 0x0f2a10 : COL_TILE), 0);
        lv_obj_set_style_text_color(
            lv_obj_get_child(s_work_chips[i], 0),
            lv_color_hex(i == idx ? COL_TEXT_HI : COL_TEXT_MID), 0);
    }
    s_sess.work_idx = idx;
}

static void select_break_chip(uint8_t idx)
{
    for (uint8_t i = 0; i < BREAK_DUR_COUNT; i++) {
        lv_obj_set_style_border_color(s_break_chips[i],
            lv_color_hex(i == idx ? COL_ACCENT : COL_BORDER), 0);
        lv_obj_set_style_bg_color(s_break_chips[i],
            lv_color_hex(i == idx ? 0x0f2a10 : COL_TILE), 0);
        lv_obj_set_style_text_color(
            lv_obj_get_child(s_break_chips[i], 0),
            lv_color_hex(i == idx ? COL_TEXT_HI : COL_TEXT_MID), 0);
    }
    s_sess.break_idx = idx;
}

// ─────────────────────────────────────────
// Session control
// ─────────────────────────────────────────
static void session_start_arc(void)
{
    s_sess.total_sec     = WORK_DURATIONS[s_sess.work_idx] * 60u;
    s_sess.remaining_sec = s_sess.total_sec;
    s_sess.in_session    = true;
    s_sess.paused        = false;
    update_arc();
    update_timer_label();
    update_round_label();
    lv_timer_resume(s_tick_timer);
    show_page(LI_ACTIVE);
}

static void session_resume(void)
{
    s_sess.paused = false;
    update_arc();
    update_timer_label();
    update_round_label();
    lv_timer_resume(s_tick_timer);
    show_page(LI_ACTIVE);
    ESP_LOGI(TAG, "Resumed — %lu sec remaining", (unsigned long)s_sess.remaining_sec);
}

static void session_pause(void)
{
    lv_timer_pause(s_tick_timer);
    s_sess.paused = true;

    // Update setup page to show frozen time + RESUME
    char buf[16];
    fmt_time(buf, sizeof(buf), s_sess.remaining_sec);
    char frozen[32];
    snprintf(frozen, sizeof(frozen), "%s remaining", buf);
    lv_label_set_text(s_frozen_label, frozen);
    lv_obj_clear_flag(s_frozen_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_lock_lbl, "RESUME");

    show_page(LI_SETUP);
    ESP_LOGI(TAG, "Paused — %lu sec remaining", (unsigned long)s_sess.remaining_sec);
}

static void session_stop(void)
{
    lv_timer_pause(s_tick_timer);

    // Build done page
    char rbuf[8], tbuf[24];
    snprintf(rbuf, sizeof(rbuf), "%lu", (unsigned long)s_sess.rounds_done);
    fmt_elapsed(tbuf, sizeof(tbuf), s_sess.session_elapsed);
    lv_label_set_text(s_done_rounds, rbuf);
    lv_label_set_text(s_done_time, tbuf);

    s_sess.in_session = false;
    s_sess.paused     = false;
    show_page(LI_DONE);
    ESP_LOGI(TAG, "Session done — %lu rounds", (unsigned long)s_sess.rounds_done);
}

// Called when arc completes — show break nudge
static void arc_complete(void)
{
    lv_timer_pause(s_tick_timer);
    s_sess.rounds_done++;
    s_sess.session_elapsed += s_sess.total_sec;

    // Update nudge labels
    char rbuf[32];
    snprintf(rbuf, sizeof(rbuf), "round %lu complete",
        (unsigned long)s_sess.rounds_done);
    lv_label_set_text(s_nudge_round_lbl, rbuf);

    uint8_t break_min = BREAK_DURATIONS[s_sess.break_idx];
    if (break_min > 0) {
        char bbuf[24];
        snprintf(bbuf, sizeof(bbuf), "%d min break suggested", break_min);
        lv_label_set_text(s_nudge_break_lbl, bbuf);
        lv_obj_clear_flag(s_nudge_break_lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_nudge_break_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    // Two soft low-frequency clicks — advisory nudge, not a harsh alarm
    tone_play_double_click();

    show_page(LI_NUDGE);
    ESP_LOGI(TAG, "Arc complete — round %lu done", (unsigned long)s_sess.rounds_done);
}

// ─────────────────────────────────────────
// Tick timer — fires every second
// ─────────────────────────────────────────
static void tick_cb(lv_timer_t *t)
{
    if (s_sess.remaining_sec == 0) {
        arc_complete();
        return;
    }
    s_sess.remaining_sec--;
    update_timer_label();
    update_arc();
}

// ─────────────────────────────────────────
// Event callbacks
// ─────────────────────────────────────────
static void work_chip_cb(lv_event_t *e)
{
    if (s_sess.in_session && !s_sess.paused) return;   // locked during active run
    uint8_t idx = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    select_work_chip(idx);
}

static void break_chip_cb(lv_event_t *e)
{
    if (s_sess.in_session && !s_sess.paused) return;
    uint8_t idx = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    select_break_chip(idx);
}

static void lock_btn_cb(lv_event_t *e)
{
    if (s_sess.paused) {
        session_resume();
    } else {
        // Fresh start
        s_sess.rounds_done      = 0;
        s_sess.session_elapsed  = 0;
        session_start_arc();
    }
}

static void pause_btn_cb(lv_event_t *e)
{
    session_pause();
}

static void stop_btn_cb(lv_event_t *e)
{
    session_stop();
}

static void nudge_break_cb(lv_event_t *e)
{
    // User chose to take a break — just hide the nudge.
    // We don't enforce the break timer; ADHMe is advisory only.
    // Start a fresh arc when they tap "done" / return to this screen.
    // For now: start fresh arc immediately (they'll look up when ready).
    session_start_arc();
}

static void nudge_keep_cb(lv_event_t *e)
{
    // Skip break — start fresh arc immediately
    session_start_arc();
}

static void done_home_cb(lv_event_t *e)
{
    adhme_goto(STATE_HOME);
}

// ─────────────────────────────────────────
// Build helpers
// ─────────────────────────────────────────
static lv_obj_t* make_page(lv_obj_t *parent)
{
    lv_obj_t *pg = lv_obj_create(parent);
    lv_obj_set_size(pg, 368, 448);
    lv_obj_set_pos(pg, 0, 0);
    lv_obj_set_style_bg_opa(pg, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pg, 0, 0);
    lv_obj_set_style_pad_all(pg, 0, 0);
    lv_obj_clear_flag(pg, LV_OBJ_FLAG_SCROLLABLE);
    return pg;
}

static lv_obj_t* make_chip(lv_obj_t *parent, const char *text,
    lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *chip = lv_obj_create(parent);
    lv_obj_set_style_bg_color(chip, lv_color_hex(COL_TILE), 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(chip, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_border_width(chip, 1, 0);
    lv_obj_set_style_radius(chip, 10, 0);
    lv_obj_set_style_pad_hor(chip, 14, 0);
    lv_obj_set_style_pad_ver(chip, 10, 0);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(chip, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *lbl = lv_label_create(chip);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT_MID), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl);

    return chip;
}

// ─────────────────────────────────────────
// Build: Setup page
// ─────────────────────────────────────────
static lv_obj_t* build_setup_page(lv_obj_t *parent)
{
    lv_obj_t *pg = make_page(parent);

    // Header
    lv_obj_t *hdr = lv_label_create(pg);
    lv_label_set_text(hdr, "LOCK IN");
    lv_obj_set_style_text_color(hdr, lv_color_hex(COL_TEXT_MID), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(hdr, 4, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 20);

    // Frozen time label (hidden unless paused)
    s_frozen_label = lv_label_create(pg);
    lv_label_set_text(s_frozen_label, "");
    lv_obj_set_style_text_color(s_frozen_label, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_font(s_frozen_label, &lv_font_montserrat_16, 0);
    lv_obj_align(s_frozen_label, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_add_flag(s_frozen_label, LV_OBJ_FLAG_HIDDEN);

    // Section: Work length
    lv_obj_t *wl = lv_label_create(pg);
    lv_label_set_text(wl, "WORK LENGTH");
    lv_obj_set_style_text_color(wl, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(wl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(wl, 2, 0);
    lv_obj_align(wl, LV_ALIGN_TOP_MID, 0, 78);

    // Work chips row
    lv_obj_t *work_row = lv_obj_create(pg);
    lv_obj_set_size(work_row, 340, LV_SIZE_CONTENT);
    lv_obj_align(work_row, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_set_style_bg_opa(work_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(work_row, 0, 0);
    lv_obj_set_style_pad_all(work_row, 0, 0);
    lv_obj_clear_flag(work_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(work_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(work_row,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(work_row, 8, 0);

    static const char *work_labels[] = {"10", "15", "25", "45", "60"};
    for (uint8_t i = 0; i < WORK_DUR_COUNT; i++) {
        s_work_chips[i] = make_chip(work_row, work_labels[i],
            work_chip_cb, (void*)(intptr_t)i);
    }

    // Section: Break
    lv_obj_t *bl = lv_label_create(pg);
    lv_label_set_text(bl, "BREAK");
    lv_obj_set_style_text_color(bl, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(bl, 2, 0);
    lv_obj_align(bl, LV_ALIGN_TOP_MID, 0, 190);

    // Break chips row
    lv_obj_t *break_row = lv_obj_create(pg);
    lv_obj_set_size(break_row, 340, LV_SIZE_CONTENT);
    lv_obj_align(break_row, LV_ALIGN_TOP_MID, 0, 212);
    lv_obj_set_style_bg_opa(break_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(break_row, 0, 0);
    lv_obj_set_style_pad_all(break_row, 0, 0);
    lv_obj_clear_flag(break_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(break_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(break_row,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(break_row, 10, 0);

    static const char *break_labels[] = {"none", "5 min", "10 min"};
    for (uint8_t i = 0; i < BREAK_DUR_COUNT; i++) {
        s_break_chips[i] = make_chip(break_row, break_labels[i],
            break_chip_cb, (void*)(intptr_t)i);
    }

    // Lock In / Resume button
    s_lock_btn = lv_obj_create(pg);
    lv_obj_set_size(s_lock_btn, 320, 56);
    lv_obj_align(s_lock_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(s_lock_btn, lv_color_hex(0x0f2a10), 0);
    lv_obj_set_style_bg_opa(s_lock_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_lock_btn, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_width(s_lock_btn, 2, 0);
    lv_obj_set_style_radius(s_lock_btn, 14, 0);
    lv_obj_set_style_pad_all(s_lock_btn, 0, 0);
    lv_obj_clear_flag(s_lock_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_lock_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_lock_btn, lock_btn_cb, LV_EVENT_CLICKED, NULL);

    s_lock_lbl = lv_label_create(s_lock_btn);
    lv_label_set_text(s_lock_lbl, "LOCK IN");
    lv_obj_set_style_text_color(s_lock_lbl, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(s_lock_lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(s_lock_lbl);

    return pg;
}

// ─────────────────────────────────────────
// Build: Active page
// ─────────────────────────────────────────
static lv_obj_t* build_active_page(lv_obj_t *parent)
{
    lv_obj_t *pg = make_page(parent);

    // Arc — fills the screen, burns clockwise from top
    // LVGL arc: use as a progress indicator (range 0–360)
    s_arc = lv_arc_create(pg);
    lv_obj_set_size(s_arc, 340, 340);
    lv_obj_center(s_arc);
    lv_arc_set_rotation(s_arc, 270);             // start at 12 o'clock
    lv_arc_set_bg_angles(s_arc, 0, 360);         // full circle track
    lv_arc_set_range(s_arc, 0, 360);
    lv_arc_set_value(s_arc, 360);                // full at start
    lv_arc_set_mode(s_arc, LV_ARC_MODE_NORMAL);
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);

    // Track (background arc)
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(COL_ARC_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 6, LV_PART_MAIN);

    // Fill (indicator arc)
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(COL_ARC_FILL), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_arc, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_arc, true, LV_PART_INDICATOR);

    // Hide the knob
    lv_obj_set_style_bg_opa(s_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_arc, 0, LV_PART_KNOB);
    lv_obj_set_style_size(s_arc, 0, 0, LV_PART_KNOB);

    // Timer label — large, centered
    s_timer_label = lv_label_create(pg);
    lv_label_set_text(s_timer_label, "--:--");
    lv_obj_set_style_text_color(s_timer_label, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(s_timer_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_letter_space(s_timer_label, -2, 0);
    lv_obj_align(s_timer_label, LV_ALIGN_CENTER, 0, -14);

    // Round label
    s_round_label = lv_label_create(pg);
    lv_label_set_text(s_round_label, "round 1");
    lv_obj_set_style_text_color(s_round_label, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(s_round_label, &lv_font_montserrat_16, 0);
    lv_obj_align(s_round_label, LV_ALIGN_CENTER, 0, 36);

    // Controls row — dim until needed
    lv_obj_t *ctrl_row = lv_obj_create(pg);
    lv_obj_set_size(ctrl_row, 200, LV_SIZE_CONTENT);
    lv_obj_align(ctrl_row, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_bg_opa(ctrl_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctrl_row, 0, 0);
    lv_obj_set_style_pad_all(ctrl_row, 0, 0);
    lv_obj_clear_flag(ctrl_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(ctrl_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl_row,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ctrl_row, 16, 0);

    // Pause button
    s_pause_btn = lv_obj_create(ctrl_row);
    lv_obj_set_size(s_pause_btn, 88, 40);
    lv_obj_set_style_bg_opa(s_pause_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_pause_btn, lv_color_hex(COL_CTRL_DIM), 0);
    lv_obj_set_style_border_width(s_pause_btn, 1, 0);
    lv_obj_set_style_radius(s_pause_btn, 10, 0);
    lv_obj_set_style_pad_all(s_pause_btn, 0, 0);
    lv_obj_clear_flag(s_pause_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_pause_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_pause_btn, pause_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pause_lbl = lv_label_create(s_pause_btn);
    lv_label_set_text(pause_lbl, "pause");
    lv_obj_set_style_text_color(pause_lbl, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(pause_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(pause_lbl);

    // Stop button
    s_stop_btn = lv_obj_create(ctrl_row);
    lv_obj_set_size(s_stop_btn, 88, 40);
    lv_obj_set_style_bg_opa(s_stop_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_stop_btn, lv_color_hex(COL_CTRL_DIM), 0);
    lv_obj_set_style_border_width(s_stop_btn, 1, 0);
    lv_obj_set_style_radius(s_stop_btn, 10, 0);
    lv_obj_set_style_pad_all(s_stop_btn, 0, 0);
    lv_obj_clear_flag(s_stop_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_stop_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_stop_btn, stop_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *stop_lbl = lv_label_create(s_stop_btn);
    lv_label_set_text(stop_lbl, "stop");
    lv_obj_set_style_text_color(stop_lbl, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(stop_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(stop_lbl);

    return pg;
}

// ─────────────────────────────────────────
// Build: Break nudge overlay
// ─────────────────────────────────────────
static lv_obj_t* build_nudge_overlay(lv_obj_t *parent)
{
    // Sits on top of active page — partial height overlay from bottom
    lv_obj_t *ov = lv_obj_create(parent);
    lv_obj_set_size(ov, 368, 200);
    lv_obj_align(ov, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(ov, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ov, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_border_width(ov, 1, 0);
    lv_obj_set_style_border_side(ov, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_pad_all(ov, 0, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);

    // Round complete label
    s_nudge_round_lbl = lv_label_create(ov);
    lv_label_set_text(s_nudge_round_lbl, "round 1 complete");
    lv_obj_set_style_text_color(s_nudge_round_lbl, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_font(s_nudge_round_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_letter_space(s_nudge_round_lbl, 1, 0);
    lv_obj_align(s_nudge_round_lbl, LV_ALIGN_TOP_MID, 0, 18);

    // Break suggestion label
    s_nudge_break_lbl = lv_label_create(ov);
    lv_label_set_text(s_nudge_break_lbl, "5 min break suggested");
    lv_obj_set_style_text_color(s_nudge_break_lbl, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(s_nudge_break_lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(s_nudge_break_lbl, LV_ALIGN_TOP_MID, 0, 44);

    // "Take a break" button
    lv_obj_t *brk_btn = lv_obj_create(ov);
    lv_obj_set_size(brk_btn, 300, 48);
    lv_obj_align(brk_btn, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_bg_color(brk_btn, lv_color_hex(0x0f2a10), 0);
    lv_obj_set_style_bg_opa(brk_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(brk_btn, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_width(brk_btn, 1, 0);
    lv_obj_set_style_radius(brk_btn, 12, 0);
    lv_obj_set_style_pad_all(brk_btn, 0, 0);
    lv_obj_clear_flag(brk_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(brk_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(brk_btn, nudge_break_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *brk_lbl = lv_label_create(brk_btn);
    lv_label_set_text(brk_lbl, "take a break");
    lv_obj_set_style_text_color(brk_lbl, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(brk_lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(brk_lbl);

    // "Keep going" button
    lv_obj_t *keep_btn = lv_obj_create(ov);
    lv_obj_set_size(keep_btn, 300, 44);
    lv_obj_align(keep_btn, LV_ALIGN_TOP_MID, 0, 130);
    lv_obj_set_style_bg_opa(keep_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(keep_btn, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_border_width(keep_btn, 1, 0);
    lv_obj_set_style_radius(keep_btn, 12, 0);
    lv_obj_set_style_pad_all(keep_btn, 0, 0);
    lv_obj_clear_flag(keep_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(keep_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(keep_btn, nudge_keep_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *keep_lbl = lv_label_create(keep_btn);
    lv_label_set_text(keep_lbl, "keep going");
    lv_obj_set_style_text_color(keep_lbl, lv_color_hex(COL_TEXT_MID), 0);
    lv_obj_set_style_text_font(keep_lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(keep_lbl);

    return ov;
}

// ─────────────────────────────────────────
// Build: Done page
// ─────────────────────────────────────────
static lv_obj_t* build_done_page(lv_obj_t *parent)
{
    lv_obj_t *pg = make_page(parent);

    // Check mark
    lv_obj_t *check = lv_label_create(pg);
    lv_label_set_text(check, "✓");
    lv_obj_set_style_text_color(check, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_font(check, &lv_font_montserrat_48, 0);
    lv_obj_align(check, LV_ALIGN_CENTER, 0, -80);

    // Rounds number — the star of the show
    s_done_rounds = lv_label_create(pg);
    lv_label_set_text(s_done_rounds, "0");
    lv_obj_set_style_text_color(s_done_rounds, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(s_done_rounds, &lv_font_montserrat_48, 0);
    lv_obj_align(s_done_rounds, LV_ALIGN_CENTER, 0, -14);

    lv_obj_t *rounds_lbl = lv_label_create(pg);
    lv_label_set_text(rounds_lbl, "ROUNDS");
    lv_obj_set_style_text_color(rounds_lbl, lv_color_hex(COL_TEXT_MID), 0);
    lv_obj_set_style_text_font(rounds_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(rounds_lbl, 3, 0);
    lv_obj_align(rounds_lbl, LV_ALIGN_CENTER, 0, 38);

    // Total time — secondary
    s_done_time = lv_label_create(pg);
    lv_label_set_text(s_done_time, "");
    lv_obj_set_style_text_color(s_done_time, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(s_done_time, &lv_font_montserrat_16, 0);
    lv_obj_align(s_done_time, LV_ALIGN_CENTER, 0, 68);

    // Home button
    lv_obj_t *home_btn = lv_obj_create(pg);
    lv_obj_set_size(home_btn, 320, 56);
    lv_obj_align(home_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(home_btn, lv_color_hex(0x0f2a10), 0);
    lv_obj_set_style_bg_opa(home_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(home_btn, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_width(home_btn, 1, 0);
    lv_obj_set_style_radius(home_btn, 14, 0);
    lv_obj_set_style_pad_all(home_btn, 0, 0);
    lv_obj_clear_flag(home_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(home_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(home_btn, done_home_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *home_lbl = lv_label_create(home_btn);
    lv_label_set_text(home_lbl, "HOME");
    lv_obj_set_style_text_color(home_lbl, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(home_lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(home_lbl);

    return pg;
}

// ─────────────────────────────────────────
// Public API
// ─────────────────────────────────────────
lv_obj_t* ui_lock_in_create(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    // Build all sub-pages
    s_setup_page    = build_setup_page(s_screen);
    s_active_page   = build_active_page(s_screen);
    s_nudge_overlay = build_nudge_overlay(s_screen);
    s_done_page     = build_done_page(s_screen);

    // 1-second tick timer — created paused, resumed when session starts
    s_tick_timer = lv_timer_create(tick_cb, 1000, NULL);
    lv_timer_pause(s_tick_timer);

    // Initial selections
    s_sess.work_idx  = 2;   // 25 min default
    s_sess.break_idx = 0;   // none default
    select_work_chip(s_sess.work_idx);
    select_break_chip(s_sess.break_idx);

    show_page(LI_SETUP);

    ESP_LOGI(TAG, "Lock In ready");
    return s_screen;
}

void ui_lock_in_reset(void)
{
    if (!s_screen) return;

    // If a session is paused — leave state intact so user can resume.
    // If no session is in progress — reset to fresh setup.
    if (!s_sess.in_session) {
        // Clean slate
        s_sess.rounds_done     = 0;
        s_sess.session_elapsed = 0;
        s_sess.paused          = false;
        lv_label_set_text(s_lock_lbl, "LOCK IN");
        lv_obj_add_flag(s_frozen_label, LV_OBJ_FLAG_HIDDEN);
    }
    // Paused session: button already says RESUME, frozen label already set —
    // nothing to do; ui_lock_in_reset just navigates to the right page.

    lv_timer_pause(s_tick_timer);
    show_page(LI_SETUP);
}

void ui_lock_in_pause(void)
{
    if (!s_screen) return;
    if (!s_sess.in_session || s_sess.paused) return;   // nothing to pause

    // Same logic as pause_btn_cb but callable externally (e.g. from state_task)
    // Caller must already hold display_lock().
    lv_timer_pause(s_tick_timer);
    s_sess.paused = true;

    char buf[16];
    fmt_time(buf, sizeof(buf), s_sess.remaining_sec);
    char frozen[32];
    snprintf(frozen, sizeof(frozen), "%s remaining", buf);
    lv_label_set_text(s_frozen_label, frozen);
    lv_obj_clear_flag(s_frozen_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_lock_lbl, "RESUME");

    // Don't call show_page here — state_task will load Quick Capture next.
    // When the user returns to Lock In, ui_lock_in_reset() will show LI_SETUP
    // with RESUME already set.
    ESP_LOGI(TAG, "Lock In paused externally — %lu sec remaining",
        (unsigned long)s_sess.remaining_sec);
}
