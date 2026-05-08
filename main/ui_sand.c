#include "ui_sand.h"
#include "imu_qmi8658.h"
#include "app_state.h"
#include "display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ui_sand";

// ─────────────────────────────────────────
// Geometry
// ─────────────────────────────────────────
#define SCREEN_W        ADHME_LCD_H_RES     // 368
#define SCREEN_H        ADHME_LCD_V_RES     // 448
#define BORDER_PX       4                   // mossy green border thickness

// Particle sandbox bounds (inside border)
// Particles are also implicitly constrained by in_screen_bounds via buf_set_px,
// but we keep them away from corners via wall bounce so they don't cluster there.
#define SAND_X_MIN      BORDER_PX
#define SAND_X_MAX      (SCREEN_W - BORDER_PX - 1)
#define SAND_Y_MIN      BORDER_PX
#define SAND_Y_MAX      (SCREEN_H - BORDER_PX - 1)

// ─────────────────────────────────────────
// Particle config
// ─────────────────────────────────────────
#define PARTICLE_COUNT  250
#define PARTICLE_RADIUS 2       // px — visual dot size

// Amber palette for LV_COLOR_FORMAT_RGB565_SWAPPED buffer.
// The buffer stores pixels with bytes swapped vs native RGB565 so that
// the DMA can send them big-endian directly to the SH8601.
// Formula: rgb565 = ((R&0xF8)<<8)|((G&0xFC)<<3)|(B>>3)
//          store  = (rgb565 >> 8) | (rgb565 << 8)   (byte-swap for buffer)
#define AMBER_COUNT     5
static const uint16_t s_amber_rgb565[AMBER_COUNT] = {
    0x48C5,   // #c4a840  warm amber (base)
    0x45AC,   // #a8882a  darker amber
    0x0CE6,   // #e0c060  lighter gold
    0xC6B4,   // #b49830  mid amber
    0x89D5,   // #d4b048  bright amber
};

// ─────────────────────────────────────────
// Physics constants
// ─────────────────────────────────────────
#define GRAVITY_SCALE   120.0f      // pixels per second² per 1g
#define DAMPING         0.75f       // velocity damping on wall bounce
#define FRICTION        0.98f       // per-frame velocity decay (air resistance)
#define PHYSICS_HZ      40          // physics steps per second
#define PHYSICS_DT      (1.0f / PHYSICS_HZ)

// Flat detection: if |gz| > FLAT_THRESHOLD, device is face-up/down
#define FLAT_THRESHOLD  0.7f

// ─────────────────────────────────────────
// Particle state
// ─────────────────────────────────────────
typedef struct {
    float x, y;         // position (float for sub-pixel physics)
    float vx, vy;       // velocity (px/s)
    uint8_t color_idx;  // index into s_amber[]
    uint8_t radius;     // 1 or 2px for variety
} particle_t;

static particle_t s_particles[PARTICLE_COUNT];

// ─────────────────────────────────────────
// Shared state between physics (Core 1) and render (Core 0)
// ─────────────────────────────────────────
// Double-buffer: physics writes to inactive buffer, render reads active buffer
typedef struct {
    int16_t x[PARTICLE_COUNT];
    int16_t y[PARTICLE_COUNT];
    uint8_t color_idx[PARTICLE_COUNT];
    uint8_t radius[PARTICLE_COUNT];
} particle_snapshot_t;

static particle_snapshot_t s_buf[2];
static volatile int         s_active_buf = 0;  // render reads this index
static SemaphoreHandle_t    s_buf_mutex  = NULL;

// ─────────────────────────────────────────
// Physics task control
// ─────────────────────────────────────────
static TaskHandle_t         s_physics_task = NULL;
static volatile bool        s_running      = false;

// ─────────────────────────────────────────
// LVGL objects
// ─────────────────────────────────────────
static lv_obj_t            *s_screen     = NULL;
static lv_obj_t            *s_canvas     = NULL;
static lv_draw_buf_t        s_draw_buf;
static uint8_t             *s_canvas_buf = NULL;

// Canvas pixel buffer size: RGB565 = 2 bytes per pixel
#define CANVAS_BUF_SIZE (SCREEN_W * SCREEN_H * 2)

// Border cache — allocated at runtime into PSRAM (heap_caps_malloc).
// Computed once on first render, stamped each frame.
// NULL until ui_sand_create() allocates it.
// Zero = transparent pixel, non-zero = border color.
static uint16_t *s_border_cache  = NULL;
static bool      s_border_cached = false;

// ─────────────────────────────────────────
// LVGL render timer (Core 0, called from LVGL task)
// ─────────────────────────────────────────
static lv_timer_t *s_render_timer = NULL;

// ─────────────────────────────────────────
// Rounded corner border radius — must be larger than the physical screen
// corner clip so the green border curves are visible. Tweak as needed.
// ─────────────────────────────────────────
#define CORNER_RADIUS   70

static inline void buf_set_px(uint16_t *buf, int x, int y, uint16_t color)
{
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return;
    buf[y * SCREEN_W + x] = color;
}

// Fill a filled square of side (2r+1) — fast, looks like a dot at small r
static void buf_fill_dot(uint16_t *buf, int cx, int cy, int r, uint16_t color)
{
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            buf_set_px(buf, cx + dx, cy + dy, color);
        }
    }
}

// Draw a rounded-rect border outline.
// Pixels along straight edges: painted if within BORDER_PX of that edge.
// Pixels in corner zones: painted if their distance from the corner arc centre
// falls between (CORNER_RADIUS - BORDER_PX) and CORNER_RADIUS.
static void buf_rounded_border(uint16_t *buf, uint16_t color)
{
    int r  = CORNER_RADIUS;
    int r_inner = r - BORDER_PX;
    if (r_inner < 0) r_inner = 0;

    // Corner arc centres (one per corner)
    int cx[4] = { r,            SCREEN_W - 1 - r, r,            SCREEN_W - 1 - r };
    int cy[4] = { r,            r,                SCREEN_H - 1 - r, SCREEN_H - 1 - r };

    for (int y = 0; y < SCREEN_H; y++) {
        for (int x = 0; x < SCREEN_W; x++) {
            bool near_left   = x < r;
            bool near_right  = x >= SCREEN_W - r;
            bool near_top    = y < r;
            bool near_bottom = y >= SCREEN_H - r;

            bool in_corner = (near_left || near_right) && (near_top || near_bottom);

            if (in_corner) {
                // Which corner?
                int c = (near_right ? 1 : 0) + (near_bottom ? 2 : 0);
                int dx = x - cx[c];
                int dy = y - cy[c];
                int d2 = dx * dx + dy * dy;
                if (d2 >= r_inner * r_inner && d2 <= r * r) {
                    buf[y * SCREEN_W + x] = color;
                }
            } else {
                // Straight edge — paint if within BORDER_PX of any edge
                bool on_edge = x < BORDER_PX || x >= SCREEN_W - BORDER_PX ||
                               y < BORDER_PX || y >= SCREEN_H - BORDER_PX;
                if (on_edge) {
                    buf[y * SCREEN_W + x] = color;
                }
            }
        }
    }
}

// ─────────────────────────────────────────
// Render timer callback — runs on Core 0 inside LVGL task
// ─────────────────────────────────────────
static void render_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_running) return;

    uint16_t *buf = (uint16_t *)s_canvas_buf;

    // 1. Clear to black (0x0000 = black in any RGB565 variant)
    memset(buf, 0x00, CANVAS_BUF_SIZE);

    // 2. Draw mossy green rounded border — computed once into a static cache,
    //    then stamped each frame. Cache lives as a global to avoid stack overflow.
    if (!s_border_cached) {
        memset(s_border_cache, 0, CANVAS_BUF_SIZE);
        buf_rounded_border(s_border_cache, 0xE219);
        s_border_cached = true;
    }
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        if (s_border_cache[i]) buf[i] = s_border_cache[i];
    }

    // 3. Draw particles from snapshot (no mutex — double-buffer atomic flip)
    int buf_idx = s_active_buf;
    const particle_snapshot_t *snap = &s_buf[buf_idx];

    for (int i = 0; i < PARTICLE_COUNT; i++) {
        int px = snap->x[i];
        int py = snap->y[i];
        int r  = snap->radius[i];
        uint16_t color = s_amber_rgb565[snap->color_idx[i]];
        buf_fill_dot(buf, px, py, r, color);
    }

    // 4. Mark canvas dirty so LVGL flushes it to the display
    lv_obj_invalidate(s_canvas);
}

// ─────────────────────────────────────────
// Particle reset — scatter them randomly inside the sandbox
// ─────────────────────────────────────────
static void particles_reset(void)
{
    for (int i = 0; i < PARTICLE_COUNT; i++) {
        s_particles[i].x  = SAND_X_MIN + (rand() % (SAND_X_MAX - SAND_X_MIN));
        s_particles[i].y  = SAND_Y_MIN + (rand() % (SAND_Y_MAX - SAND_Y_MIN));
        s_particles[i].vx = ((float)(rand() % 100) - 50.0f) * 0.5f;
        s_particles[i].vy = ((float)(rand() % 100) - 50.0f) * 0.5f;
        s_particles[i].color_idx = rand() % AMBER_COUNT;
        s_particles[i].radius    = (rand() % 2) + 1;  // 1 or 2px
    }
}

// ─────────────────────────────────────────
// Simple spatial hash to detect particle–particle collisions
// We use a coarse grid approach — fast enough at 40Hz for 250 particles
// ─────────────────────────────────────────
#define GRID_CELL   6   // px — slightly larger than max particle diameter (4px)
#define GRID_COLS   ((SCREEN_W / GRID_CELL) + 1)
#define GRID_ROWS   ((SCREEN_H / GRID_CELL) + 1)
#define GRID_SIZE   (GRID_COLS * GRID_ROWS)

// Each cell holds up to 4 particle indices (keeps stack usage low)
#define CELL_CAPACITY 4
static int16_t s_grid[GRID_SIZE][CELL_CAPACITY];
static uint8_t s_grid_count[GRID_SIZE];

static void grid_clear(void)
{
    memset(s_grid_count, 0, sizeof(s_grid_count));
}

static inline int grid_cell(float x, float y)
{
    int cx = (int)(x / GRID_CELL);
    int cy = (int)(y / GRID_CELL);
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx >= GRID_COLS) cx = GRID_COLS - 1;
    if (cy >= GRID_ROWS) cy = GRID_ROWS - 1;
    return cy * GRID_COLS + cx;
}

static void grid_insert(int idx)
{
    int cell = grid_cell(s_particles[idx].x, s_particles[idx].y);
    if (s_grid_count[cell] < CELL_CAPACITY) {
        s_grid[cell][s_grid_count[cell]++] = (int16_t)idx;
    }
}

// Push two overlapping particles apart (simple elastic nudge)
static void resolve_overlap(particle_t *a, particle_t *b)
{
    float dx = b->x - a->x;
    float dy = b->y - a->y;
    float dist2 = dx * dx + dy * dy;
    float min_dist = (float)(a->radius + b->radius);

    if (dist2 < min_dist * min_dist && dist2 > 0.001f) {
        float dist  = sqrtf(dist2);
        float push  = (min_dist - dist) * 0.5f;
        float nx    = dx / dist;
        float ny    = dy / dist;

        a->x -= nx * push;
        a->y -= ny * push;
        b->x += nx * push;
        b->y += ny * push;

        // Swap velocity components along the collision normal (elastic)
        float va_n = a->vx * nx + a->vy * ny;
        float vb_n = b->vx * nx + b->vy * ny;
        a->vx += (vb_n - va_n) * nx;
        a->vy += (vb_n - va_n) * ny;
        b->vx += (va_n - vb_n) * nx;
        b->vy += (va_n - vb_n) * ny;
    }
}

// ─────────────────────────────────────────
// Physics step
// ─────────────────────────────────────────
static void physics_step(float grav_x, float grav_y)
{
    // 1. Integrate velocity + position
    for (int i = 0; i < PARTICLE_COUNT; i++) {
        particle_t *p = &s_particles[i];

        p->vx += grav_x * PHYSICS_DT;
        p->vy += grav_y * PHYSICS_DT;

        p->vx *= FRICTION;
        p->vy *= FRICTION;

        p->x += p->vx * PHYSICS_DT;
        p->y += p->vy * PHYSICS_DT;

        // Wall bounce
        if (p->x - p->radius < SAND_X_MIN) {
            p->x  = SAND_X_MIN + p->radius;
            p->vx = fabsf(p->vx) * DAMPING;
        }
        if (p->x + p->radius > SAND_X_MAX) {
            p->x  = SAND_X_MAX - p->radius;
            p->vx = -fabsf(p->vx) * DAMPING;
        }
        if (p->y - p->radius < SAND_Y_MIN) {
            p->y  = SAND_Y_MIN + p->radius;
            p->vy = fabsf(p->vy) * DAMPING;
        }
        if (p->y + p->radius > SAND_Y_MAX) {
            p->y  = SAND_Y_MAX - p->radius;
            p->vy = -fabsf(p->vy) * DAMPING;
        }
    }

    // 2. Particle–particle collision (spatial hash)
    grid_clear();
    for (int i = 0; i < PARTICLE_COUNT; i++) {
        grid_insert(i);
    }

    for (int i = 0; i < PARTICLE_COUNT; i++) {
        particle_t *a = &s_particles[i];
        int cx = (int)(a->x / GRID_CELL);
        int cy = (int)(a->y / GRID_CELL);

        // Check 3×3 neighbourhood
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int nx = cx + dx;
                int ny = cy + dy;
                if (nx < 0 || nx >= GRID_COLS || ny < 0 || ny >= GRID_ROWS) continue;
                int cell = ny * GRID_COLS + nx;
                for (int k = 0; k < s_grid_count[cell]; k++) {
                    int j = s_grid[cell][k];
                    if (j <= i) continue;  // avoid double-processing
                    resolve_overlap(a, &s_particles[j]);
                }
            }
        }
    }
}

// ─────────────────────────────────────────
// Snapshot active state into inactive buffer and flip
// ─────────────────────────────────────────
static void physics_publish_snapshot(void)
{
    int write_buf = 1 - s_active_buf;
    particle_snapshot_t *snap = &s_buf[write_buf];

    for (int i = 0; i < PARTICLE_COUNT; i++) {
        snap->x[i]         = (int16_t)s_particles[i].x;
        snap->y[i]         = (int16_t)s_particles[i].y;
        snap->color_idx[i] = s_particles[i].color_idx;
        snap->radius[i]    = s_particles[i].radius;
    }

    // Atomic flip — render task picks up new snapshot on next timer tick
    s_active_buf = write_buf;
}

// ─────────────────────────────────────────
// Physics task — Core 1
// ─────────────────────────────────────────
static void physics_task(void *arg)
{
    ESP_LOGI(TAG, "Physics task started on core %d", xPortGetCoreID());

    float gx = 0.0f, gy = 0.0f, gz = 0.0f;
    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(1000 / PHYSICS_HZ);

    while (s_running) {
        // Read IMU — non-fatal if it occasionally fails; last values persist
        imu_qmi8658_read_accel(&gx, &gy, &gz);

        // Map accelerometer axes to screen gravity vector.
        // The QMI8658 on the Waveshare 1.8" board is mounted with:
        //   chip's +X axis pointing DOWN the screen (toward screen bottom)
        //   chip's +Y axis pointing LEFT across the screen
        // So:
        //   screen_grav_x = -gy
        //   screen_grav_y = +gx
        float screen_grav_x = -gy * GRAVITY_SCALE;
        float screen_grav_y =  gx * GRAVITY_SCALE;

        // Flat mode: if device is lying face-up/down, gravity is near zero.
        // Let existing velocity decay naturally — particles drift and scatter.
        if (fabsf(gz) > FLAT_THRESHOLD) {
            screen_grav_x *= (1.0f - fabsf(gz));
            screen_grav_y *= (1.0f - fabsf(gz));
        }

        physics_step(screen_grav_x, screen_grav_y);
        physics_publish_snapshot();

        vTaskDelayUntil(&last_wake, period);
    }

    ESP_LOGI(TAG, "Physics task exiting");
    vTaskDelete(NULL);
}

// ─────────────────────────────────────────
// STOP button handler
// ─────────────────────────────────────────
static void stop_btn_cb(lv_event_t *e)
{
    (void)e;
    adhme_goto(STATE_HOME);
}

// ─────────────────────────────────────────
// Public API
// ─────────────────────────────────────────
lv_obj_t* ui_sand_create(void)
{
    // Initialise IMU once at startup
    esp_err_t imu_ret = imu_qmi8658_init();
    if (imu_ret != ESP_OK) {
        ESP_LOGW(TAG, "IMU init failed (%s) — Sand will use zero gravity",
            esp_err_to_name(imu_ret));
    }

    // Shared mutex for future use (currently lock-free via double-buffer flip)
    s_buf_mutex = xSemaphoreCreateMutex();

    // Allocate canvas pixel buffer in PSRAM (368 × 448 × 2 = ~330 KB)
    s_canvas_buf = heap_caps_malloc(CANVAS_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_canvas_buf) {
        s_canvas_buf = heap_caps_malloc(CANVAS_BUF_SIZE, MALLOC_CAP_DEFAULT);
    }
    if (!s_canvas_buf) {
        ESP_LOGE(TAG, "Cannot allocate canvas buffer (%d bytes)", CANVAS_BUF_SIZE);
        return NULL;
    }

    // Allocate border cache in PSRAM — same size, computed once on first render
    s_border_cache = heap_caps_malloc(CANVAS_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_border_cache) {
        s_border_cache = heap_caps_malloc(CANVAS_BUF_SIZE, MALLOC_CAP_DEFAULT);
    }
    if (!s_border_cache) {
        ESP_LOGE(TAG, "Cannot allocate border cache (%d bytes)", CANVAS_BUF_SIZE);
        return NULL;
    }
    memset(s_border_cache, 0, CANVAS_BUF_SIZE);

    // Screen
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    // Full-screen canvas — declared SWAPPED to match display.c's
    // lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565_SWAPPED).
    // LVGL will byte-swap on flush; we write natural RGB565 values.
    lv_draw_buf_init(&s_draw_buf, SCREEN_W, SCREEN_H,
                     LV_COLOR_FORMAT_RGB565_SWAPPED, LV_STRIDE_AUTO,
                     s_canvas_buf, CANVAS_BUF_SIZE);

    s_canvas = lv_canvas_create(s_screen);
    lv_canvas_set_draw_buf(s_canvas, &s_draw_buf);
    lv_obj_set_size(s_canvas, SCREEN_W, SCREEN_H);
    lv_obj_align(s_canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    memset(s_canvas_buf, 0x00, CANVAS_BUF_SIZE);  // clear to black

    // STOP label — dim, bottom-center, tap to go home
    lv_obj_t *stop_btn = lv_obj_create(s_screen);
    lv_obj_set_size(stop_btn, 80, 28);
    lv_obj_align(stop_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(stop_btn, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(stop_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(stop_btn, lv_color_hex(0x1a3d16), 0);
    lv_obj_set_style_border_width(stop_btn, 1, 0);
    lv_obj_set_style_border_opa(stop_btn, LV_OPA_50, 0);
    lv_obj_set_style_radius(stop_btn, 6, 0);
    lv_obj_clear_flag(stop_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(stop_btn, stop_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *stop_lbl = lv_label_create(stop_btn);
    lv_label_set_text(stop_lbl, "STOP");
    lv_obj_set_style_text_color(stop_lbl, lv_color_hex(0x1a3d16), 0);
    lv_obj_set_style_text_font(stop_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_opa(stop_lbl, LV_OPA_50, 0);
    lv_obj_center(stop_lbl);

    // Render timer — 33ms ≈ 30fps, runs on Core 0 inside LVGL task
    s_render_timer = lv_timer_create(render_timer_cb, 33, NULL);
    lv_timer_pause(s_render_timer);

    ESP_LOGI(TAG, "Sand screen created");
    return s_screen;
}

void ui_sand_start(void)
{
    particles_reset();
    physics_publish_snapshot();  // seed buffer before first render

    lv_timer_resume(s_render_timer);

    s_running = true;
    xTaskCreatePinnedToCore(
        physics_task,
        "sand_physics",
        4096,
        NULL,
        4,           // priority — below LVGL (5) so render never starves
        &s_physics_task,
        1            // Core 1
    );

    ESP_LOGI(TAG, "Sand started");
}

void ui_sand_stop(void)
{
    s_running = false;

    // Give physics task time to exit cleanly before we unload the screen
    if (s_physics_task) {
        vTaskDelay(pdMS_TO_TICKS(60));
        s_physics_task = NULL;
    }

    lv_timer_pause(s_render_timer);
    ESP_LOGI(TAG, "Sand stopped");
}
