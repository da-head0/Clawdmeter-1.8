#include "buddy.h"
#include "buddy_common.h"
#include <Arduino_GFX_Library.h>
#include <string.h>

// Mirrors PersonaState in splash.cpp
enum { B_SLEEP, B_IDLE, B_BUSY, B_ATTENTION, B_CELEBRATE, B_DIZZY, B_HEART };

// ──────────────── shared geometry (Clawdmeter 1.8") ────────────────
// Internal canvas is 184×224 (= LCD/2). Buddy renders into it at scale=2,
// and splash.cpp tells LVGL to display the canvas at 2× scale, so the
// final on-screen size is 4× the 6×8 ASCII grid — readable from across
// the desk on the 368×448 panel.
const int BUDDY_X_CENTER  = 92;
const int BUDDY_CANVAS_W  = 184;
const int BUDDY_Y_BASE    = 30;
const int BUDDY_Y_OVERLAY = 6;
const int BUDDY_CHAR_W    = 6;
const int BUDDY_CHAR_H    = 8;

// ──────────────── shared colors ────────────────
const uint16_t BUDDY_BG     = 0x0000;
const uint16_t BUDDY_HEART  = 0xF810;
const uint16_t BUDDY_DIM    = 0x8410;
const uint16_t BUDDY_YEL    = 0xFFE0;
const uint16_t BUDDY_WHITE  = 0xFFFF;
const uint16_t BUDDY_CYAN   = 0x07FF;
const uint16_t BUDDY_GREEN  = 0x07E0;
const uint16_t BUDDY_PURPLE = 0xA01F;
const uint16_t BUDDY_RED    = 0xF800;
const uint16_t BUDDY_BLUE   = 0x041F;

// Buddy draws into whatever Arduino_GFX target the splash module hands us
// (typically an Arduino_Canvas sharing its framebuffer with an LVGL canvas).
static Arduino_GFX* s_tgt = nullptr;
static uint8_t      _scale = 2;

void buddy_set_target(Arduino_GFX* tgt) { s_tgt = tgt; }
static inline Arduino_GFX* tgt() { return s_tgt; }

void buddyPrintLine(const char* line, int yPx, uint16_t color, int xOff) {
    if (!tgt()) return;
    int len = strlen(line);
    if (_scale > 1) {
        while (len && line[len-1] == ' ') len--;
        while (len && *line == ' ') { line++; len--; }
    }
    int w = len * BUDDY_CHAR_W * _scale;
    int x = BUDDY_X_CENTER - w / 2 + xOff * _scale;
    tgt()->setTextColor(color, BUDDY_BG);
    tgt()->setCursor(x, yPx);
    for (int i = 0; i < len; i++) tgt()->print(line[i]);
}

void buddyPrintSprite(const char* const* lines, uint8_t nLines, int yOffset, uint16_t color, int xOff) {
    if (!tgt()) return;
    tgt()->setTextSize(_scale);
    int yBase = BUDDY_Y_BASE * _scale - (_scale - 1) * 14;
    for (uint8_t i = 0; i < nLines; i++) {
        buddyPrintLine(lines[i], yBase + (yOffset + i * BUDDY_CHAR_H) * _scale, color, xOff);
    }
}

void buddySetCursor(int x, int y) {
    if (!tgt()) return;
    tgt()->setCursor(BUDDY_X_CENTER + (x - BUDDY_X_CENTER) * _scale, y * _scale);
}
void buddySetColor(uint16_t fg) {
    if (tgt()) tgt()->setTextColor(fg, BUDDY_BG);
}
void buddyPrint(const char* s) {
    if (!tgt()) return;
    tgt()->setTextSize(_scale);
    tgt()->print(s);
}

// ──────────────── single-species registry ────────────────
extern const Species RABBIT_SPECIES;
static const Species* CURRENT = &RABBIT_SPECIES;

// ──────────────── tick state ────────────────
static uint32_t tickCount  = 0;
static uint32_t nextTickAt = 0;
static const uint32_t TICK_MS = 200;

static uint8_t lastDrawnState = 0xFF;
void buddyInvalidate() { lastDrawnState = 0xFF; }

void buddyInit() {
    tickCount = 0;
    nextTickAt = 0;
    lastDrawnState = 0xFF;
}

void buddyTick(uint8_t personaState) {
    if (!tgt()) return;

    uint32_t now = millis();
    bool ticked = false;
    if ((int32_t)(now - nextTickAt) >= 0) {
        nextTickAt = now + TICK_MS;
        tickCount++;
        ticked = true;
    }

    if (personaState >= 7) personaState = B_IDLE;
    if (!ticked && personaState == lastDrawnState) return;
    lastDrawnState = personaState;

    // Clear the full canvas before redraw. Particles can span anywhere,
    // and the previous fillRect range left the bottom band of the framebuffer
    // permanently in its uninitialized PSRAM state, showing up as garbage
    // pixels on AMOLED.
    tgt()->fillScreen(BUDDY_BG);

    if (CURRENT->states[personaState]) CURRENT->states[personaState](tickCount);
}

// Stubs to satisfy buddy.h API; not used in single-species build.
void buddySetSpeciesIdx(uint8_t) {}
void buddySetSpecies(const char*) {}
const char* buddySpeciesName() { return CURRENT->name; }
uint8_t buddySpeciesCount() { return 1; }
uint8_t buddySpeciesIdx() { return 0; }
void buddyNextSpecies() {}
void buddyPrevSpecies() {}
void buddySetPeek(bool) {}
void buddyRenderTo(Arduino_GFX*, uint8_t) {}
