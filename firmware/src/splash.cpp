#include "splash.h"
#include "data.h"
#include "ble.h"
#include "buddy/buddy.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>

// PersonaState mirrors the buddy enum order: sleep, idle, busy, attention,
// celebrate, dizzy, heart.
enum PersonaState { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };

// Buddy canvas is 184×224 (= 368×448 / 2). LVGL displays it scaled 2×.
static constexpr int CANVAS_W = 184;
static constexpr int CANVAS_H = 224;

static lv_obj_t* splash_container = nullptr;
static lv_obj_t* lvgl_canvas    = nullptr;
static lv_obj_t* lbl_session    = nullptr;
static lv_obj_t* lbl_task1      = nullptr;
static lv_obj_t* lbl_task2      = nullptr;
static uint16_t* canvas_buf     = nullptr;

static Arduino_Canvas*  buddy_canvas = nullptr;

static bool active = false;

// Cache last-rendered strings to avoid LVGL redraws every tick.
static char rendered_session[40] = {0};
static char rendered_task1[80]   = {0};
static char rendered_task2[80]   = {0};

// Forward decl from theme.h — keep splash.cpp standalone of ui.cpp colors.
#include "theme.h"
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);

static const UsageData* last_data_ptr = nullptr;

// State machine
static PersonaState state         = P_SLEEP;
static uint32_t     state_until   = 0;   // millis() time at which a one-shot state expires (0 = persistent)
static uint32_t     last_redraw_ms = 0;
static uint32_t     next_random_celebrate_ms = 0;
static float        last_session_pct = 0.0f;
static float        last_weekly_pct  = 0.0f;
static bool         have_usage = false;
static bool         last_idle  = true;   // start treating it as idle until daemon says otherwise
static char         last_status[16] = {0};

// One pick from {busy, heart, celebrate} that the rabbit holds while in the
// 5%+ band. Re-rolled on band entry and on splash re-entry. Default busy so
// first frame after boot is sane before any roll happens.
static PersonaState rolled_busy_band = P_BUSY;

static bool in_busy_band(float session_pct, const char* status, bool idle) {
    if (idle) return false;
    if (status && strcmp(status, "limited") == 0) return false;
    return session_pct > 5.0f && session_pct <= 90.0f;
}

static PersonaState pick_busy_band_state(void) {
    static const PersonaState pool[] = { P_BUSY, P_HEART, P_CELEBRATE };
    return pool[esp_random() % 3];
}

static PersonaState compute_state_from_usage(float session_pct,
                                             const char* status,
                                             bool idle,
                                             ble_state_t ble_state) {
    if (ble_state != BLE_STATE_CONNECTED) return P_SLEEP;
    if (idle) return P_SLEEP;
    if (status && strcmp(status, "limited") == 0) return P_DIZZY;
    if (session_pct > 90.0f) return P_DIZZY;
    if (session_pct > 5.0f)  return rolled_busy_band;
    if (session_pct >= 2.0f) return P_BUSY;
    if (session_pct > 0.0f)  return P_IDLE;
    return P_IDLE;  // 0% with busy session = session just started, treat as idle.
}

static uint32_t random_celebrate_interval_ms() {
    // 25–30 minutes during quiet periods.
    return 25UL * 60 * 1000 + (uint32_t)random(0, 5UL * 60 * 1000);
}

void splash_init(lv_obj_t *parent) {
    splash_container = lv_obj_create(parent);
    lv_obj_set_size(splash_container, 368, 448);
    lv_obj_set_pos(splash_container, 0, 0);
    lv_obj_set_style_bg_color(splash_container, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(splash_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(splash_container, 0, 0);
    lv_obj_set_style_pad_all(splash_container, 0, 0);
    lv_obj_clear_flag(splash_container, LV_OBJ_FLAG_SCROLLABLE);

    // Allocate the RGB565 framebuffer in SRAM (~82KB). Arduino_Canvas
    // expects to own its framebuffer, so let it allocate via begin() —
    // GFX_SKIP_OUTPUT_BEGIN avoids re-initializing the already-begun
    // SH8601 display.
    buddy_canvas = new Arduino_Canvas(CANVAS_W, CANVAS_H, nullptr);
    if (!buddy_canvas->begin(GFX_SKIP_OUTPUT_BEGIN)) {
        Serial.println("splash: Arduino_Canvas begin failed");
        return;
    }
    canvas_buf = buddy_canvas->getFramebuffer();
    if (!canvas_buf) {
        Serial.println("splash: canvas framebuffer is null");
        return;
    }
    // Zero the framebuffer once so any pixels never written by the buddy
    // engine (corners, gaps) stay black instead of showing PSRAM garbage.
    memset(canvas_buf, 0, CANVAS_W * CANVAS_H * 2);

    // LVGL canvas widget wrapping the same buffer. Size/align must be set
    // BEFORE set_buffer — set_buffer otherwise clobbers the widget's
    // bounding box back to the buffer dimensions, defeating STRETCH.
    lvgl_canvas = lv_canvas_create(splash_container);
    lv_obj_align(lvgl_canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_size(lvgl_canvas, 368, 448);
    lv_image_set_inner_align(lvgl_canvas, LV_IMAGE_ALIGN_STRETCH);
    lv_image_set_antialias(lvgl_canvas, false);
    lv_canvas_set_buffer(lvgl_canvas, canvas_buf, CANVAS_W, CANVAS_H, LV_COLOR_FORMAT_RGB565);

    // Wire buddy renderer to the canvas.
    buddy_set_target(buddy_canvas);
    buddyInit();

    // Bottom info panel: session slug (top) + two recent tasks (below).
    // Each label has an explicit height so LV_LABEL_LONG_DOT actually trims
    // a single line — without a fixed height the label expands vertically
    // and stacks wrap onto neighbours.
    const int W      = 336;
    const int H_TASK = 20;
    const int H_SESS = 24;
    lbl_session = lv_label_create(splash_container);
    lv_label_set_text(lbl_session, "");
    lv_obj_set_style_text_font(lbl_session, &font_styrene_20, 0);
    lv_obj_set_style_text_color(lbl_session, THEME_TEXT, 0);
    lv_obj_set_style_text_align(lbl_session, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(lbl_session, W, H_SESS);
    lv_label_set_long_mode(lbl_session, LV_LABEL_LONG_DOT);
    lv_obj_align(lbl_session, LV_ALIGN_BOTTOM_MID, 0, -72);

    lbl_task1 = lv_label_create(splash_container);
    lv_label_set_text(lbl_task1, "");
    lv_obj_set_style_text_font(lbl_task1, &font_styrene_16, 0);
    lv_obj_set_style_text_color(lbl_task1, THEME_DIM, 0);
    lv_obj_set_style_text_align(lbl_task1, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_size(lbl_task1, W, H_TASK);
    lv_label_set_long_mode(lbl_task1, LV_LABEL_LONG_DOT);
    lv_obj_align(lbl_task1, LV_ALIGN_BOTTOM_MID, 0, -38);

    lbl_task2 = lv_label_create(splash_container);
    lv_label_set_text(lbl_task2, "");
    lv_obj_set_style_text_font(lbl_task2, &font_styrene_16, 0);
    lv_obj_set_style_text_color(lbl_task2, THEME_ACCENT, 0);
    lv_obj_set_style_text_align(lbl_task2, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_size(lbl_task2, W, H_TASK);
    lv_label_set_long_mode(lbl_task2, LV_LABEL_LONG_DOT);
    lv_obj_align(lbl_task2, LV_ALIGN_BOTTOM_MID, 0, -12);

    next_random_celebrate_ms = millis() + random_celebrate_interval_ms();

    lv_obj_add_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
}

static void splash_render_activity(void) {
    if (!lbl_session || !lbl_task1 || !lbl_task2) return;

    const char* sn = "";
    const char* t1 = "";
    const char* t2 = "";
    // Show whenever we have valid data AND BLE is still connected. If BLE
    // drops, the labels blank — the daemon stops sending in that case too,
    // and we don't want stale info pinned to the splash forever.
    if (last_data_ptr && last_data_ptr->valid &&
        ble_get_state() == BLE_STATE_CONNECTED) {
        sn = last_data_ptr->session_name;
        t1 = last_data_ptr->task1_line;
        t2 = last_data_ptr->task2_line;
    }
    if (strcmp(sn, rendered_session) != 0) {
        strlcpy(rendered_session, sn, sizeof(rendered_session));
        lv_label_set_text(lbl_session, sn);
    }
    if (strcmp(t1, rendered_task1) != 0) {
        strlcpy(rendered_task1, t1, sizeof(rendered_task1));
        lv_label_set_text(lbl_task1, t1);
    }
    if (strcmp(t2, rendered_task2) != 0) {
        strlcpy(rendered_task2, t2, sizeof(rendered_task2));
        lv_label_set_text(lbl_task2, t2);
    }
}

void splash_tick(void) {
    if (!active || !buddy_canvas) return;

    uint32_t now = millis();

    // Expire one-shot states (celebrate, etc.)
    if (state_until && (int32_t)(now - state_until) >= 0) {
        state_until = 0;
        // Recompute steady-state from latest usage.
        state = have_usage
            ? compute_state_from_usage(last_session_pct, last_status, last_idle, ble_get_state())
            : (ble_get_state() == BLE_STATE_CONNECTED ? P_SLEEP : P_SLEEP);
        buddyInvalidate();
    }

    // Random celebrate flash only during P_IDLE — sleeping rabbit suddenly
    // throwing confetti reads as broken.
    if (!state_until && state == P_IDLE &&
        (int32_t)(now - next_random_celebrate_ms) >= 0) {
        state = P_CELEBRATE;
        state_until = now + 5000;
        next_random_celebrate_ms = now + random_celebrate_interval_ms();
        buddyInvalidate();
    }

    // Drive the buddy animation engine at its own 200ms tick cadence.
    // buddyTick() internally gates redraws to once per tick + on state change.
    buddyTick((uint8_t)state);

    // Tell LVGL the canvas pixels changed so it composites them.
    if (now - last_redraw_ms >= 60) {
        last_redraw_ms = now;
        if (lvgl_canvas) lv_obj_invalidate(lvgl_canvas);
    }

    splash_render_activity();
}

void splash_next(void) {
    // Debug: walk through states for QA. On 1.8" no UI path triggers this.
    state = (PersonaState)(((int)state + 1) % 7);
    state_until = 0;
    buddyInvalidate();
}

void splash_on_usage_update(const UsageData* data) {
    if (!data || !data->valid) return;
    last_data_ptr = data;

    // Detect window resets — utilization drop ≥50pp signals a 5h or 7d
    // boundary roll. Fire celebrate as a one-shot, then resume.
    bool reset_detected = false;
    if (have_usage) {
        if (last_session_pct - data->session_pct >= 50.0f) reset_detected = true;
        if (last_weekly_pct  - data->weekly_pct  >= 50.0f) reset_detected = true;
    }

    // Detect band transitions BEFORE updating cached state so we know
    // whether this packet pushed us from non-band → band.
    bool was_in_band = in_busy_band(last_session_pct, last_status, last_idle);
    bool now_in_band = in_busy_band(data->session_pct, data->status, data->idle);
    if (now_in_band && !was_in_band) {
        rolled_busy_band = pick_busy_band_state();
    }

    last_session_pct = data->session_pct;
    last_weekly_pct  = data->weekly_pct;
    last_idle        = data->idle;
    strlcpy(last_status, data->status, sizeof(last_status));
    have_usage = true;

    if (reset_detected) {
        state = P_CELEBRATE;
        state_until = millis() + 5000;
        buddyInvalidate();
        return;
    }

    if (!state_until) {
        PersonaState ns = compute_state_from_usage(data->session_pct, data->status, data->idle, ble_get_state());
        if (ns != state) {
            state = ns;
            buddyInvalidate();
        }
    }
}

bool splash_is_active(void) { return active; }

void splash_show(void) {
    // Re-roll the 5%+ band pick on every splash entry so the user gets visual
    // variety between revisits. State recompute below picks it up.
    if (have_usage && in_busy_band(last_session_pct, last_status, last_idle)) {
        rolled_busy_band = pick_busy_band_state();
        if (!state_until) {
            state = compute_state_from_usage(last_session_pct, last_status, last_idle, ble_get_state());
        }
    }
    if (splash_container) lv_obj_clear_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
    active = true;
    // Force a redraw at the entry state.
    buddyInvalidate();
}

void splash_hide(void) {
    if (splash_container) lv_obj_add_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
    active = false;
}

lv_obj_t* splash_get_root(void) {
    return splash_container;
}
