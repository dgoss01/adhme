#include "ui_quick_capture.h"
#include "app_state.h"
#include "display.h"
#include "audio_capture.h"
#include "sd_card.h"
#include "esp_log.h"
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>

static const char *TAG = "ui_qc";

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
#define COL_REC_RED     0xc04040
#define COL_WAVE_LIVE   0x3d8b46
#define COL_WAVE_FROZEN 0x1a4a1a

// ─────────────────────────────────────────
// Waveform geometry
// ─────────────────────────────────────────
#define WAVE_BARS     28
#define WAVE_BAR_W    6
#define WAVE_BAR_GAP  4
#define WAVE_W        (WAVE_BARS * (WAVE_BAR_W + WAVE_BAR_GAP) - WAVE_BAR_GAP)
#define WAVE_H        60

// ─────────────────────────────────────────
// Page states
// ─────────────────────────────────────────
typedef enum { QC_IDLE = 0, QC_RECORDING, QC_CONFIRM } qc_page_t;

// ─────────────────────────────────────────
// Static UI state
// ─────────────────────────────────────────
static qc_page_t    s_page          = QC_IDLE;
static lv_obj_t    *s_screen        = NULL;
static lv_obj_t    *s_idle_page     = NULL;
static lv_obj_t    *s_rec_page      = NULL;
static lv_obj_t    *s_conf_page     = NULL;

static lv_obj_t    *s_rec_bars[WAVE_BARS];
static lv_obj_t    *s_conf_bars[WAVE_BARS];
static uint8_t      s_wave_snap[WAVE_BARS];

static lv_obj_t    *s_rec_clock;
static lv_obj_t    *s_rec_duration;
static lv_obj_t    *s_conf_time;
static lv_obj_t    *s_conf_duration;
static lv_obj_t    *s_recent_rows[3];

static lv_timer_t  *s_wave_timer = NULL;  // drives waveform + polls done
static lv_timer_t  *s_dur_timer  = NULL;  // 1-second duration counter
static int          s_rec_seconds = 0;

// ─────────────────────────────────────────
// Waveform helpers
// ─────────────────────────────────────────
static void wave_set_bar(lv_obj_t *bars[], int i, uint8_t level)
{
    int h = 4 + (level * (WAVE_H - 8) / 100);
    lv_obj_set_height(bars[i], h);
    lv_obj_set_y(bars[i], (WAVE_H - h) / 2);
}

static void wave_scroll_push(uint8_t lvl)
{
    for (int i = 0; i < WAVE_BARS - 1; i++) {
        s_wave_snap[i] = s_wave_snap[i + 1];
        wave_set_bar(s_rec_bars, i, s_wave_snap[i]);
    }
    s_wave_snap[WAVE_BARS - 1] = lvl;
    wave_set_bar(s_rec_bars, WAVE_BARS - 1, lvl);
}

static void wave_freeze(void)
{
    for (int i = 0; i < WAVE_BARS; i++)
        wave_set_bar(s_conf_bars, i, s_wave_snap[i]);
}

// ─────────────────────────────────────────
// Duration counter — fires every second
// ─────────────────────────────────────────
static void dur_tick_cb(lv_timer_t *t)
{
    s_rec_seconds++;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d:%02d",
        s_rec_seconds / 60, s_rec_seconds % 60);
    lv_label_set_text(s_rec_duration, buf);
}

// ─────────────────────────────────────────
// Wave + poll timer — fires every 80 ms
// ─────────────────────────────────────────
static void wave_timer_cb(lv_timer_t *t)
{
    if (s_page == QC_RECORDING) {
        // Real mic level from capture task
        wave_scroll_push(audio_capture_get_level());

        // Poll for WAV finalization after stop
        if (audio_capture_poll_done(NULL, 0, NULL)) {
            lv_timer_pause(s_wave_timer);
        }
    }
}

// ─────────────────────────────────────────
// Page switch
// ─────────────────────────────────────────
static void show_page(qc_page_t page)
{
    lv_obj_add_flag(s_idle_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_rec_page,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_conf_page, LV_OBJ_FLAG_HIDDEN);
    switch (page) {
        case QC_IDLE:      lv_obj_clear_flag(s_idle_page, LV_OBJ_FLAG_HIDDEN); break;
        case QC_RECORDING: lv_obj_clear_flag(s_rec_page,  LV_OBJ_FLAG_HIDDEN); break;
        case QC_CONFIRM:   lv_obj_clear_flag(s_conf_page, LV_OBJ_FLAG_HIDDEN); break;
    }
    s_page = page;
}

// ─────────────────────────────────────────
// Recording start / stop
// ─────────────────────────────────────────
static void start_recording(void)
{
    esp_err_t ret = audio_capture_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "audio_capture_start failed: %d", ret);
        // Stay on idle page so user can retry
        return;
    }

    s_rec_seconds = 0;
    lv_label_set_text(s_rec_duration, "0:00");

    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm *t = localtime(&tv.tv_sec);
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", t->tm_hour, t->tm_min);
    lv_label_set_text(s_rec_clock, buf);

    lv_timer_resume(s_wave_timer);
    lv_timer_resume(s_dur_timer);

    show_page(QC_RECORDING);
    ESP_LOGI(TAG, "Recording started");
}

// ─────────────────────────────────────────
// Recent recordings list
// ─────────────────────────────────────────

// Parse YYDDD_HHmmss.WAV → display string "MMM DD  HH:mm"
// Returns false if filename doesn't match expected format
static bool parse_wav_filename(const char *name, char *out, size_t out_len)
{
    int yy, ddd, hh, mm, ss;
    if (sscanf(name, "%02d%03d_%02d%02d%02d.WAV", &yy, &ddd, &hh, &mm, &ss) != 5)
        return false;

    // Convert year + day-of-year → struct tm so we get month + mday
    struct tm t = {0};
    t.tm_year = yy + 100;   // years since 1900 (yy=26 → 2026 → 126)
    t.tm_yday = ddd - 1;    // 0-based
    t.tm_mday = 1;          // mktime needs a valid mday to start from
    t.tm_mon  = 0;
    // Use timegm-style trick: set Jan 1 of that year, add yday as seconds
    t.tm_hour = hh;
    t.tm_min  = mm;
    t.tm_sec  = ss + (ddd - 1) * 86400;  // roll forward by day-of-year
    mktime(&t);             // normalizes: fills tm_mon, tm_mday, tm_wday

    static const char *months[] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };
    snprintf(out, out_len, "%s %2d  %02d:%02d",
        months[t.tm_mon], t.tm_mday, hh, mm);
    return true;
}

// Scan SD card, collect up to MAX_WAV .WAV filenames, sort descending,
// display the 3 most recent in s_recent_rows[]
#define MAX_WAV 32
static void update_recent_list(void)
{
    if (!sd_card_mounted()) {
        for (int i = 0; i < 3; i++)
            lv_label_set_text(s_recent_rows[i], "no SD card");
        return;
    }

    // Collect WAV filenames
    static char names[MAX_WAV][20];
    int count = 0;

    DIR *dir = opendir(SD_MOUNT_POINT);
    if (!dir) {
        ESP_LOGW(TAG, "opendir failed");
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < MAX_WAV) {
        const char *n = ent->d_name;
        size_t l = strlen(n);
        // Match YYDDD_HHmmss.WAV — 16 chars exactly
        if (l == 16 && strcmp(n + 12, ".WAV") == 0) {
            strncpy(names[count], n, sizeof(names[0]) - 1);
            names[count][sizeof(names[0]) - 1] = '\0';
            count++;
        }
    }
    closedir(dir);

    if (count == 0) {
        for (int i = 0; i < 3; i++)
            lv_label_set_text(s_recent_rows[i], "--");
        return;
    }

    // Sort descending — newest filename = largest string = most recent
    // Simple insertion sort, count is small
    for (int i = 1; i < count; i++) {
        char tmp[20];
        strncpy(tmp, names[i], sizeof(tmp));
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], tmp) < 0) {
            strncpy(names[j + 1], names[j], sizeof(names[0]));
            j--;
        }
        strncpy(names[j + 1], tmp, sizeof(names[0]));
    }

    // Fill the 3 rows
    for (int i = 0; i < 3; i++) {
        if (i < count) {
            char display[24];
            if (parse_wav_filename(names[i], display, sizeof(display))) {
                lv_label_set_text(s_recent_rows[i], display);
            } else {
                lv_label_set_text(s_recent_rows[i], names[i]);
            }
        } else {
            lv_label_set_text(s_recent_rows[i], "--");
        }
    }
}

static void stop_recording(void)
{
    lv_timer_pause(s_dur_timer);
    // wave_timer keeps running to poll audio_capture_poll_done()

    audio_capture_stop();  // non-blocking — capture task finalizes WAV

    // Build confirm labels using UI-side duration counter
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm *t = localtime(&tv.tv_sec);

    static const char *days[]   = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
    static const char *months[] = {"JAN","FEB","MAR","APR","MAY","JUN",
                                    "JUL","AUG","SEP","OCT","NOV","DEC"};
    char ts[28];
    snprintf(ts, sizeof(ts), "%s %d %s  %02d:%02d",
        days[t->tm_wday], t->tm_mday, months[t->tm_mon],
        t->tm_hour, t->tm_min);
    lv_label_set_text(s_conf_time, ts);

    char dur[8];
    snprintf(dur, sizeof(dur), "%d:%02d",
        s_rec_seconds / 60, s_rec_seconds % 60);
    lv_label_set_text(s_conf_duration, dur);

    wave_freeze();
    update_recent_list();
    show_page(QC_CONFIRM);

    ESP_LOGI(TAG, "Recording stopped");
}

// ─────────────────────────────────────────
// Event callbacks
// ─────────────────────────────────────────
static void mic_tap_cb(lv_event_t *e)
{
    start_recording();
}

static void rec_page_tap_cb(lv_event_t *e)
{
    if (s_page == QC_RECORDING) stop_recording();
}

static void done_cb(lv_event_t *e)
{
    // Wave timer is either paused (WAV done) or still polling — pause it now
    lv_timer_pause(s_wave_timer);
    adhme_goto(adhme_get_return_state());
}

// Swipe down or right from idle/confirm → return to previous state
static void screen_swipe_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_BOTTOM || dir == LV_DIR_RIGHT) {
        if (s_page == QC_IDLE) {
            adhme_goto(adhme_get_return_state());
        } else if (s_page == QC_CONFIRM) {
            lv_timer_pause(s_wave_timer);
            adhme_goto(adhme_get_return_state());
        }
        // No swipe during QC_RECORDING — tap anywhere stops it
    }
}

// ─────────────────────────────────────────
// Build helpers
// ─────────────────────────────────────────
static lv_obj_t* make_wave_container(lv_obj_t *parent,
    lv_obj_t *bars[], uint32_t color)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, WAVE_W, WAVE_H);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < WAVE_BARS; i++) {
        lv_obj_t *bar = lv_obj_create(c);
        lv_obj_set_size(bar, WAVE_BAR_W, 4);
        lv_obj_set_pos(bar, i * (WAVE_BAR_W + WAVE_BAR_GAP), (WAVE_H - 4) / 2);
        lv_obj_set_style_bg_color(bar, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        bars[i] = bar;
    }
    return c;
}

static lv_obj_t* make_page(lv_obj_t *scr)
{
    lv_obj_t *pg = lv_obj_create(scr);
    lv_obj_set_size(pg, 368, 448);
    lv_obj_set_pos(pg, 0, 0);
    lv_obj_set_style_bg_opa(pg, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pg, 0, 0);
    lv_obj_set_style_pad_all(pg, 0, 0);
    lv_obj_clear_flag(pg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(pg, LV_OBJ_FLAG_CLICKABLE);
    return pg;
}

// ─────────────────────────────────────────
// IDLE page
// ─────────────────────────────────────────
static lv_obj_t* build_idle_page(lv_obj_t *scr)
{
    lv_obj_t *pg = make_page(scr);

    // Back button — top left, always visible
    lv_obj_t *back = lv_label_create(pg);
    lv_label_set_text(back, "< HOME");
    lv_obj_set_style_text_color(back, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(back, &lv_font_montserrat_16, 0);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 16, 18);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, done_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *hdr = lv_label_create(pg);
    lv_label_set_text(hdr, "QUICK CAPTURE");
    lv_obj_set_style_text_color(hdr, lv_color_hex(COL_TEXT_MID), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(hdr, 3, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 20);

    // Big mic button
    lv_obj_t *mic = lv_obj_create(pg);
    lv_obj_set_size(mic, 120, 120);
    lv_obj_align(mic, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_bg_color(mic, lv_color_hex(0x0f2a10), 0);
    lv_obj_set_style_bg_opa(mic, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(mic, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_width(mic, 2, 0);
    lv_obj_set_style_radius(mic, 60, 0);
    lv_obj_set_style_pad_all(mic, 0, 0);
    lv_obj_clear_flag(mic, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mic, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(mic, mic_tap_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *mic_lbl = lv_label_create(mic);
    lv_label_set_text(mic_lbl, "(o)");
    lv_obj_set_style_text_color(mic_lbl, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_font(mic_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(mic_lbl);

    lv_obj_t *hint = lv_label_create(pg);
    lv_label_set_text(hint, "tap to record");
    lv_obj_set_style_text_color(hint, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 90);

    return pg;
}

// ─────────────────────────────────────────
// RECORDING page
// ─────────────────────────────────────────
static lv_obj_t* build_rec_page(lv_obj_t *scr)
{
    lv_obj_t *pg = make_page(scr);
    lv_obj_add_flag(pg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pg, rec_page_tap_cb, LV_EVENT_CLICKED, NULL);

    // REC dot
    lv_obj_t *dot = lv_obj_create(pg);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_align(dot, LV_ALIGN_TOP_LEFT, 20, 27);
    lv_obj_set_style_bg_color(dot, lv_color_hex(COL_REC_RED), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_radius(dot, 4, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *rec_lbl = lv_label_create(pg);
    lv_label_set_text(rec_lbl, "REC");
    lv_obj_set_style_text_color(rec_lbl, lv_color_hex(COL_REC_RED), 0);
    lv_obj_set_style_text_font(rec_lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(rec_lbl, LV_ALIGN_TOP_LEFT, 34, 22);

    s_rec_clock = lv_label_create(pg);
    lv_label_set_text(s_rec_clock, "--:--");
    lv_obj_set_style_text_color(s_rec_clock, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(s_rec_clock, &lv_font_montserrat_16, 0);
    lv_obj_align(s_rec_clock, LV_ALIGN_TOP_RIGHT, -20, 20);

    s_rec_duration = lv_label_create(pg);
    lv_label_set_text(s_rec_duration, "0:00");
    lv_obj_set_style_text_color(s_rec_duration, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(s_rec_duration, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_letter_space(s_rec_duration, -2, 0);
    lv_obj_align(s_rec_duration, LV_ALIGN_CENTER, 0, -60);

    lv_obj_t *wave = make_wave_container(pg, s_rec_bars, COL_WAVE_LIVE);
    lv_obj_align(wave, LV_ALIGN_CENTER, 0, 34);

    lv_obj_t *hint = lv_label_create(pg);
    lv_label_set_text(hint, "tap anywhere to stop");
    lv_obj_set_style_text_color(hint, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -24);

    return pg;
}

// ─────────────────────────────────────────
// CONFIRM page
// ─────────────────────────────────────────
static lv_obj_t* build_conf_page(lv_obj_t *scr)
{
    lv_obj_t *pg = make_page(scr);

    lv_obj_t *saved = lv_label_create(pg);
    lv_label_set_text(saved, "SAVED");
    lv_obj_set_style_text_color(saved, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_font(saved, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_letter_space(saved, 4, 0);
    lv_obj_align(saved, LV_ALIGN_TOP_MID, 0, 20);

    s_conf_time = lv_label_create(pg);
    lv_label_set_text(s_conf_time, "---");
    lv_obj_set_style_text_color(s_conf_time, lv_color_hex(COL_TEXT_MID), 0);
    lv_obj_set_style_text_font(s_conf_time, &lv_font_montserrat_12, 0);
    lv_obj_align(s_conf_time, LV_ALIGN_TOP_MID, 0, 46);

    lv_obj_t *wave = make_wave_container(pg, s_conf_bars, COL_WAVE_FROZEN);
    lv_obj_align(wave, LV_ALIGN_TOP_MID, 0, 74);

    s_conf_duration = lv_label_create(pg);
    lv_label_set_text(s_conf_duration, "0:00");
    lv_obj_set_style_text_color(s_conf_duration, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(s_conf_duration, &lv_font_montserrat_24, 0);
    lv_obj_align(s_conf_duration, LV_ALIGN_TOP_MID, 0, 150);

    lv_obj_t *sync = lv_label_create(pg);
    lv_label_set_text(sync, "syncs when wifi connects");
    lv_obj_set_style_text_color(sync, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(sync, &lv_font_montserrat_12, 0);
    lv_obj_align(sync, LV_ALIGN_TOP_MID, 0, 186);

    lv_obj_t *div = lv_obj_create(pg);
    lv_obj_set_size(div, 280, 1);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 218);
    lv_obj_set_style_bg_color(div, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(div, 0, 0);
    lv_obj_clear_flag(div, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *rec_hdr = lv_label_create(pg);
    lv_label_set_text(rec_hdr, "RECENT");
    lv_obj_set_style_text_color(rec_hdr, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(rec_hdr, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(rec_hdr, 2, 0);
    lv_obj_align(rec_hdr, LV_ALIGN_TOP_LEFT, 44, 228);

    for (int i = 0; i < 3; i++) {
        s_recent_rows[i] = lv_label_create(pg);
        lv_label_set_text(s_recent_rows[i], "--:--   --");
        lv_obj_set_style_text_color(s_recent_rows[i],
            lv_color_hex(COL_TEXT_MID), 0);
        lv_obj_set_style_text_font(s_recent_rows[i],
            &lv_font_montserrat_16, 0);
        lv_obj_align(s_recent_rows[i], LV_ALIGN_TOP_LEFT,
            44, 252 + i * 28);
    }

    // DONE button
    lv_obj_t *done = lv_obj_create(pg);
    lv_obj_set_size(done, 320, 52);
    lv_obj_align(done, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(done, lv_color_hex(0x0f2a10), 0);
    lv_obj_set_style_bg_opa(done, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(done, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_width(done, 1, 0);
    lv_obj_set_style_radius(done, 14, 0);
    lv_obj_set_style_pad_all(done, 0, 0);
    lv_obj_clear_flag(done, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(done, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(done, done_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *done_lbl = lv_label_create(done);
    lv_label_set_text(done_lbl, "DONE");
    lv_obj_set_style_text_color(done_lbl, lv_color_hex(COL_TEXT_HI), 0);
    lv_obj_set_style_text_font(done_lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(done_lbl);

    return pg;
}

// ─────────────────────────────────────────
// Public API
// ─────────────────────────────────────────
lv_obj_t* ui_quick_capture_create(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    s_idle_page = build_idle_page(s_screen);
    s_rec_page  = build_rec_page(s_screen);
    s_conf_page = build_conf_page(s_screen);

    // Wave + poll timer — created paused, resumed on record start
    s_wave_timer = lv_timer_create(wave_timer_cb, 80, NULL);
    lv_timer_pause(s_wave_timer);

    // Duration counter — created paused
    s_dur_timer = lv_timer_create(dur_tick_cb, 1000, NULL);
    lv_timer_pause(s_dur_timer);

    show_page(QC_IDLE);

    // Swipe to dismiss
    lv_obj_add_event_cb(s_screen, screen_swipe_cb, LV_EVENT_GESTURE, NULL);

    ESP_LOGI(TAG, "Quick Capture ready");
    return s_screen;
}

void ui_quick_capture_reset(void)
{
    if (!s_screen) return;

    if (s_wave_timer) lv_timer_pause(s_wave_timer);
    if (s_dur_timer)  lv_timer_pause(s_dur_timer);

    s_rec_seconds = 0;
    if (s_rec_duration) lv_label_set_text(s_rec_duration, "0:00");

    audio_capture_abort();
    show_page(QC_IDLE);
}

void ui_quick_capture_set_level(uint8_t level)
{
    // Kept for external callers — wave_timer_cb polls directly now
    (void)level;
}