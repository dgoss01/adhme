#include "ui_home.h"
#include "app_state.h"
#include "esp_log.h"
#include <time.h>
#include <sys/time.h>

static const char *TAG = "ui_home";

// ─────────────────────────────────────────
// Forest palette
// ─────────────────────────────────────────
#define COL_BG          0x030a03  // true black-green
#define COL_TILE_BG     0x0c1f0c  // dark tile
#define COL_TILE_BORDER 0x1a3218  // tile border
#define COL_TEXT_HI     0xc8e0b4  // bright leaf
#define COL_TEXT_MID    0x4a7a42  // mid green
#define COL_TEXT_DIM    0x2e4e28  // dim green
#define COL_DIVIDER     0x1a3d16  // hairline

// App accent colors (left edge bars)
#define COL_LOCK_IN     0x3d8b46  // forest green  — focus, growth
#define COL_CHECK_IN    0xc4a840  // amber         — warmth, energy
#define COL_DRIFT       0x6a9ab8  // steel blue    — calm, grounding
#define COL_BREATHE     0x7ab8a0  // seafoam       — breath, body
#define COL_SAND        0xc4a840  // warm amber    — matches sand particles
#define COL_SPARK       0x8b9a3d  // olive moss    — alive, reactive
#define COL_CAPTURE     0x7ab87a  // bright leaf   — always-on action

// ─────────────────────────────────────────
// Clock label (updated by ui_home_tick)
// ─────────────────────────────────────────
static lv_obj_t *s_clock_label = NULL;
static lv_obj_t *s_date_label  = NULL;

// ─────────────────────────────────────────
// App tile definition
// ─────────────────────────────────────────
typedef struct {
    const char      *icon;
    const char      *name;
    const char      *sub;
    uint32_t         accent;
    adhme_state_t    state;
} app_tile_t;

static const app_tile_t tiles[] = {
    { "[ ]", "LOCK IN",   "focus · pomodoro",    COL_LOCK_IN,  STATE_LOCK_IN  },
    { "<3",  "CHECK IN",  "mood · energy",        COL_CHECK_IN, STATE_CHECK_IN },
    { "~",   "DRIFT",     "grounding resets",     COL_DRIFT,    STATE_DRIFT    },
    { "o-o", "BREATHE",   "breathing patterns",   COL_BREATHE,  STATE_BREATHE  },
    { ":::","SAND",       "gravity toy",          COL_SAND,     STATE_SAND     },
    { "*~*", "SPARK",     "touch · flow",         COL_SPARK,    STATE_SPARK    },
};

// ─────────────────────────────────────────
// Tile tap handler
// ─────────────────────────────────────────
static void tile_tap_cb(lv_event_t *e)
{
    adhme_state_t state = (adhme_state_t)(intptr_t)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "Tile tapped → %d", state);
    adhme_goto(state);
}

static void capture_tap_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "Quick Capture tapped");
    adhme_goto(STATE_QUICK_CAPTURE);
}

// ─────────────────────────────────────────
// Helper — styled label
// ─────────────────────────────────────────
static lv_obj_t* make_label(lv_obj_t *parent, const char *text,
    uint32_t color, const lv_font_t *font)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    if (font) lv_obj_set_style_text_font(l, font, 0);
    return l;
}

// ─────────────────────────────────────────
// Build a 2-column app tile
// ─────────────────────────────────────────
static lv_obj_t* make_tile(lv_obj_t *parent, const app_tile_t *t)
{
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_set_size(tile, 162, 80);
    lv_obj_set_style_bg_color(tile, lv_color_hex(COL_TILE_BG), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(tile, lv_color_hex(COL_TILE_BORDER), 0);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_set_style_border_side(tile, LV_BORDER_SIDE_FULL, 0);
    lv_obj_set_style_outline_width(tile, 0, 0);
    lv_obj_set_style_radius(tile, 14, 0);
    lv_obj_set_style_pad_all(tile, 10, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

    // Left accent bar
    lv_obj_t *bar = lv_obj_create(tile);
    lv_obj_set_size(bar, 2, 60);
    lv_obj_align(bar, LV_ALIGN_LEFT_MID, -10, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(t->accent), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 1, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);  // pass touches to tile

    // Content column
    lv_obj_t *col = lv_obj_create(tile);
    lv_obj_set_size(col, 130, 60);
    lv_obj_align(col, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);  // pass touches to tile
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, 3, 0);

    // Icon — accent colored
    lv_obj_t *icon = lv_label_create(col);
    lv_label_set_text(icon, t->icon);
    lv_obj_set_style_text_color(icon, lv_color_hex(t->accent), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_16, 0);

    // Name
    make_label(col, t->name, COL_TEXT_HI, &lv_font_montserrat_12);

    // Sub
    make_label(col, t->sub, COL_TEXT_MID, &lv_font_montserrat_12);

    // Tap event
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tile, tile_tap_cb, LV_EVENT_CLICKED,
        (void*)(intptr_t)t->state);

    return tile;
}

// ─────────────────────────────────────────
// Long press clock → time set
// ─────────────────────────────────────────
static void clock_long_press_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "Clock long-press → TIME SET");
    adhme_goto(STATE_TIMESET);
}

// ─────────────────────────────────────────
// Build home screen
// ─────────────────────────────────────────
lv_obj_t* ui_home_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ── Status bar ──
    lv_obj_t *status = lv_obj_create(scr);
    lv_obj_set_size(status, 368, 24);
    lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(status, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status, 0, 0);
    lv_obj_set_style_pad_all(status, 0, 0);
    lv_obj_clear_flag(status, LV_OBJ_FLAG_SCROLLABLE);

    s_date_label = make_label(status, "THU 24 APR",
        COL_TEXT_MID, &lv_font_montserrat_12);
    lv_obj_align(s_date_label, LV_ALIGN_LEFT_MID, 24, 0);

    // ── Clock ──
    s_clock_label = make_label(scr, "--:--",
        COL_TEXT_HI, &lv_font_montserrat_48);
    lv_obj_set_style_text_letter_space(s_clock_label, -3, 0);
    lv_obj_align(s_clock_label, LV_ALIGN_TOP_MID, 0, 28);

    // ADHMe wordmark
    lv_obj_t *wm = make_label(scr, "ADHMe",
        COL_TEXT_DIM, &lv_font_montserrat_12);
    lv_obj_set_style_text_letter_space(wm, 4, 0);
    lv_obj_align(wm, LV_ALIGN_TOP_MID, 0, 84);

    // Divider
    lv_obj_t *div = lv_obj_create(scr);
    lv_obj_set_size(div, 32, 1);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 104);
    lv_obj_set_style_bg_color(div, lv_color_hex(COL_DIVIDER), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(div, 0, 0);

    // ── App grid ──
    lv_obj_t *grid = lv_obj_create(scr);
    lv_obj_set_size(grid, 348, 260);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 112);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_style_pad_gap(grid, 7, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < 6; i++) {
        make_tile(grid, &tiles[i]);
    }

    // ── Quick Capture (full width) ──
    lv_obj_t *cap = lv_obj_create(scr);
    lv_obj_set_size(cap, 320, 52);
    lv_obj_align(cap, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(cap, lv_color_hex(0x112211), 0);
    lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(cap, lv_color_hex(0x243d24), 0);
    lv_obj_set_style_border_width(cap, 1, 0);
    lv_obj_set_style_border_side(cap, LV_BORDER_SIDE_FULL, 0);
    lv_obj_set_style_border_color(cap,
        lv_color_hex(COL_CAPTURE), LV_PART_MAIN);

    // top accent line
    lv_obj_t *top = lv_obj_create(cap);
    lv_obj_set_size(top, 320, 2);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, -1);
    lv_obj_set_style_bg_color(top, lv_color_hex(COL_CAPTURE), 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(top, 0, 0);

    lv_obj_set_style_radius(cap, 14, 0);
    lv_obj_set_style_pad_all(cap, 10, 0);
    lv_obj_clear_flag(cap, LV_OBJ_FLAG_SCROLLABLE);

    // Icon
    lv_obj_t *ci = make_label(cap, "(+)",
        COL_CAPTURE, &lv_font_montserrat_20);
    lv_obj_align(ci, LV_ALIGN_LEFT_MID, 4, 0);

    // Labels
    lv_obj_t *cn = make_label(cap, "QUICK CAPTURE",
        COL_TEXT_HI, &lv_font_montserrat_12);
    lv_obj_align(cn, LV_ALIGN_LEFT_MID, 34, -7);

    lv_obj_t *cs = make_label(cap, "double-press PWR from anywhere",
        COL_TEXT_MID, &lv_font_montserrat_12);
    lv_obj_align(cs, LV_ALIGN_LEFT_MID, 34, 9);

    lv_obj_add_flag(cap, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cap, capture_tap_cb, LV_EVENT_CLICKED, NULL);

    // Initial clock update
    ui_home_tick();

    // Long-press clock → timeset
    lv_obj_add_flag(s_clock_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_clock_label,
        clock_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    return scr;
}

// ─────────────────────────────────────────
// Clock update — call from a timer or task
// ─────────────────────────────────────────
void ui_home_tick(void)
{
    if (!s_clock_label) return;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm *tm_info = localtime(&tv.tv_sec);

    static char time_buf[8];
    static char date_buf[16];
    static const char *days[] = {
        "SUN","MON","TUE","WED","THU","FRI","SAT"};
    static const char *months[] = {
        "JAN","FEB","MAR","APR","MAY","JUN",
        "JUL","AUG","SEP","OCT","NOV","DEC"};

    snprintf(time_buf, sizeof(time_buf), "%02d:%02d",
        tm_info->tm_hour, tm_info->tm_min);
    snprintf(date_buf, sizeof(date_buf), "%s %d %s",
        days[tm_info->tm_wday],
        tm_info->tm_mday,
        months[tm_info->tm_mon]);

    lv_label_set_text(s_clock_label, time_buf);
    lv_label_set_text(s_date_label, date_buf);
}