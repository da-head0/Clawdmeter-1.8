#pragma once
#include <Arduino.h>

struct UsageData {
    float session_pct;       // 5-hour window utilization (0-100)
    int session_reset_mins;  // minutes until session resets
    float weekly_pct;        // 7-day window utilization (0-100)
    int weekly_reset_mins;   // minutes until weekly resets
    char status[16];         // "allowed" or "limited"
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
    char session_name[40];   // Claude Code session slug, e.g. "splash-repair-usage-daemon"
    char task1_line[80];     // splash row 1 — daemon-formatted with glyph prefix, e.g. "✔ Phase 2: …"
    char task2_line[80];     // splash row 2
    char active_form[64];    // usage screen — current in-progress task's activeForm, no glyph (firmware adds ✳)
    uint32_t activity_ms;    // millis() of last update; UI blanks after 30s staleness
    bool idle;               // daemon: no busy Claude Code session. Forces P_SLEEP regardless of session_pct.
};
