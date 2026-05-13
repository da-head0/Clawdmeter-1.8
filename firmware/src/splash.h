#pragma once
#include <stdint.h>
#include <lvgl.h>

struct UsageData;

// Initialize splash — creates an LVGL canvas widget inside `parent`,
// allocates the buddy's 184×224 RGB565 framebuffer, and sets up the
// rabbit animation engine (200ms tick).
void splash_init(lv_obj_t *parent);

// Advance the buddy's animation. Call from main loop.
void splash_tick(void);

// QA-only: cycle through persona states (sleep → idle → busy → …).
// Wired to the legacy "next animation" button on 2.16"; on 1.8" the
// only button is reserved for Space / cycle-screen, so this is unused
// in production but exposed for debug.
void splash_next(void);

void splash_show(void);
void splash_hide(void);

// Notify splash that fresh usage data arrived (5h_pct + 7d_pct + status).
// Splash uses this to update the persona state and detect window resets.
void splash_on_usage_update(const UsageData* data);

bool splash_is_active(void);
lv_obj_t* splash_get_root(void);
