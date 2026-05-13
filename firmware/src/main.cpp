#include <Arduino.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include <Arduino_DriveBus_Library.h>
#include <memory>
#include "display_cfg.h"
#include "expander.h"
#include "data.h"
#include "ui.h"
#include "ble.h"
#include "power.h"
#include "imu.h"
#include "splash.h"
#include "usage_rate.h"

// The 1.8" board only exposes one user-facing button (KEY1 / GPIO0).
//   Short tap  → Space (Claude Code voice-mode trigger).
//   Long press → cycle screens (Usage ↔ Bluetooth ↔ Splash).
// Shift+Tab (mode toggle) has no button on this board.
#define KEY1_LONGPRESS_MS 700

// ---- Hardware objects ----
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
// LCD_RESET handled by TCA9554 expander, not a direct GPIO.
Arduino_SH8601 *gfx = new Arduino_SH8601(
    bus, GFX_NOT_DEFINED, 0 /* rotation */,
    LCD_WIDTH, LCD_HEIGHT, 0, 0, 0, 0);
XPowersPMU pmu;
SensorQMI8658 imu;

static std::shared_ptr<Arduino_IIC_DriveBus> ft_iic;
static std::unique_ptr<Arduino_IIC>          ft3168;

static UsageData usage = {};

// ---- Touch shared state (read once per loop) ----
static volatile bool     touch_irq_flag = false;
static bool              touch_pressed  = false;
static uint16_t          touch_x = 0;
static uint16_t          touch_y = 0;

static void IRAM_ATTR touch_isr(void) {
    touch_irq_flag = true;
}

static void touch_read() {
    // FT3168 only IRQs on edges, so during a drag we must keep polling
    // while a finger is reported down. Poll on IRQ or while last state was down.
    bool was_down = touch_pressed;
    if (!touch_irq_flag && !was_down) return;
    touch_irq_flag = false;
    if (!ft3168) { touch_pressed = false; return; }

    uint8_t fingers = (uint8_t)ft3168->IIC_Read_Device_Value(
        Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
    if (fingers > 0) {
        touch_x = (uint16_t)ft3168->IIC_Read_Device_Value(
            Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
        touch_y = (uint16_t)ft3168->IIC_Read_Device_Value(
            Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
        touch_pressed = true;
    } else {
        touch_pressed = false;
    }
}

// ---- LVGL draw buffers (PSRAM-backed, partial render) ----
#define BUF_LINES 40
static uint16_t *buf1 = nullptr;
static uint16_t *buf2 = nullptr;

static uint32_t my_tick(void) {
    return millis();
}

// SH8601 supports native rotation via MADCTL — no CPU pixel remap needed.
// IMU auto-rotation is wired through gfx->setRotation() instead.
static uint8_t current_panel_rotation = 0;
static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t*)px_map, w, h);
    lv_display_flush_ready(disp);
}

// QSPI AMOLEDs prefer even-aligned flush regions (same constraint as CO5300).
static void rounder_cb(lv_event_t* e) {
    lv_area_t *area = (lv_area_t*)lv_event_get_param(e);
    area->x1 = area->x1 & ~1;
    area->y1 = area->y1 & ~1;
    area->x2 = area->x2 | 1;
    area->y2 = area->y2 | 1;
}

static void my_touch_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    if (touch_pressed) {
        data->point.x = touch_x;
        data->point.y = touch_y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static bool parse_json(const char* json, UsageData* out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return false;
    }
    out->session_pct = doc["s"] | 0.0f;
    out->session_reset_mins = doc["sr"] | -1;
    out->weekly_pct = doc["w"] | 0.0f;
    out->weekly_reset_mins = doc["wr"] | -1;
    strlcpy(out->status, doc["st"] | "unknown", sizeof(out->status));
    out->ok = doc["ok"] | false;
    out->valid = true;
    // Daemon includes the latest session + task snapshot on every packet, so
    // we always overwrite. Empty string = "nothing to show".
    strlcpy(out->session_name, doc["sn"] | "", sizeof(out->session_name));
    strlcpy(out->task1_line,   doc["t1"] | "", sizeof(out->task1_line));
    strlcpy(out->task2_line,   doc["t2"] | "", sizeof(out->task2_line));
    strlcpy(out->active_form,  doc["ta"] | "", sizeof(out->active_form));
    out->idle = doc["idle"] | false;
    out->activity_ms = millis();
    return true;
}

#define CMD_BUF_SIZE 64
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_pos = 0;

static void send_screenshot() {
    const uint32_t w = LCD_WIDTH, h = LCD_HEIGHT;
    const uint32_t row_bytes = w * 2;
    const uint32_t buf_size = row_bytes * h;
    uint8_t* sbuf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!sbuf) {
        Serial.println("SCREENSHOT_ERR");
        return;
    }
    // PSRAM malloc returns uninitialized bytes — zero so any area lv_snapshot
    // doesn't draw into (e.g. fully-transparent widget gaps) reads back as
    // black instead of stale memory.
    memset(sbuf, 0, buf_size);
    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, w, h, LV_COLOR_FORMAT_RGB565, row_bytes, sbuf, buf_size);
    lv_result_t res = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &draw_buf);
    if (res != LV_RESULT_OK) {
        heap_caps_free(sbuf);
        Serial.println("SCREENSHOT_ERR");
        return;
    }
    Serial.printf("SCREENSHOT_START %lu %lu %lu\n", (unsigned long)w, (unsigned long)h, (unsigned long)buf_size);
    Serial.flush();
    Serial.write(sbuf, buf_size);
    Serial.flush();
    Serial.println();
    Serial.println("SCREENSHOT_END");
    heap_caps_free(sbuf);
}

static void check_serial_cmd() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            cmd_buf[cmd_pos] = '\0';
            if (strcmp(cmd_buf, "screenshot") == 0) {
                send_screenshot();
            } else if (strcmp(cmd_buf, "screen splash") == 0) {
                ui_show_screen(SCREEN_SPLASH);
                Serial.println("OK splash");
            } else if (strcmp(cmd_buf, "screen usage") == 0) {
                ui_show_screen(SCREEN_USAGE);
                Serial.println("OK usage");
            } else if (strcmp(cmd_buf, "screen bluetooth") == 0) {
                ui_show_screen(SCREEN_BLUETOOTH);
                Serial.println("OK bluetooth");
            }
            cmd_pos = 0;
        } else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = c;
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("{\"ready\":true,\"board\":\"waveshare_amoled_18\"}");

    // I2C first — expander, AXP, IMU, FT3168 all share this bus.
    Wire.begin(IIC_SDA, IIC_SCL);
    Wire.setClock(400000);

    // TCA9554 expander gates LCD_RESET + TP_RESET; must come up before
    // display/touch can be talked to.
    if (!expander_init()) {
        Serial.println("Expander init failed — display/touch will not respond");
    }
    expander_reset_sequence();

    // Init display
    gfx->begin();
    gfx->fillScreen(0x0000);
    gfx->setBrightness(200);

    // PMU + IMU
    power_init();
    imu_init();

    // Touch (FT3168 via DriveBus library)
    pinMode(TP_INT, INPUT_PULLUP);
    ft_iic = std::make_shared<Arduino_HWIIC>(IIC_SDA, IIC_SCL, &Wire);
    ft3168.reset(new Arduino_FT3x68(ft_iic, FT3168_DEVICE_ADDRESS,
                                    DRIVEBUS_DEFAULT_VALUE, TP_INT, touch_isr));
    bool touch_ok = false;
    for (int i = 0; i < 5; i++) {
        if (ft3168->begin()) { touch_ok = true; break; }
        delay(100);
    }
    Serial.println(touch_ok ? "FT3168 init OK" : "FT3168 init failed");

    // LVGL
    lv_init();
    lv_tick_set_cb(my_tick);

    buf1 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);
    buf2 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, MALLOC_CAP_SPIRAM);

    lv_display_t* disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, LCD_WIDTH * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_cb);

    ble_init();

    pinMode(BTN_KEY1, INPUT_PULLUP);

    ui_init();
    ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());
    ui_update_battery(power_battery_pct(), power_is_charging());
    ui_show_screen(SCREEN_SPLASH);

    Serial.println("Dashboard ready, waiting for data on BLE...");
}

static ble_state_t last_ble_state = BLE_STATE_INIT;

// Native panel rotation via SH8601 MADCTL. Cheaper than CPU pixel remap.
// IMU returns 0..3; map to gfx setRotation directly. On change, blank
// brightness briefly then ramp back up so the transition reads intentional.
static void handle_rotation_change(void) {
    static uint8_t  ramp_step = 0;
    static uint32_t ramp_last = 0;
    uint8_t rot = imu_get_rotation();
    if (rot != current_panel_rotation) {
        gfx->setBrightness(0);
        gfx->setRotation(rot);
        current_panel_rotation = rot;
        lv_obj_invalidate(lv_screen_active());
        ramp_step = 1;
        return;
    }
    if (ramp_step == 0) return;
    uint32_t now = millis();
    if (now - ramp_last < 25) return;
    ramp_last = now;
    static const uint8_t levels[] = {60, 120, 170, 200};
    gfx->setBrightness(levels[ramp_step - 1]);
    if (ramp_step >= 4) ramp_step = 0;
    else                ramp_step++;
}

// KEY1 (GPIO0) state machine: tap = Space, long press (>700ms) = cycle screen.
// Decision is deferred until release-or-hold-threshold so the same press can
// resolve as either action without ever firing both.
static void handle_key1(void) {
    static bool     was_pressed = false;
    static uint32_t press_start = 0;
    static bool     long_consumed = false;

    bool now_pressed = (digitalRead(BTN_KEY1) == LOW);
    uint32_t now = millis();

    if (now_pressed && !was_pressed) {
        press_start = now;
        long_consumed = false;
    } else if (now_pressed && !long_consumed && (now - press_start) >= KEY1_LONGPRESS_MS) {
        // Long-press fires once while held.
        long_consumed = true;
        if (ui_get_current_screen() == SCREEN_SPLASH) {
            ui_show_screen(SCREEN_USAGE);
        } else {
            ui_cycle_screen();
        }
    } else if (!now_pressed && was_pressed) {
        // Released — short tap emits a single Space if the long-press
        // handler hasn't already claimed this press.
        if (!long_consumed) {
            ble_keyboard_press(0x2C, 0);  // HID Space, no modifiers
            delay(20);
            ble_keyboard_release();
        }
    }
    was_pressed = now_pressed;
}

void loop() {
    touch_read();
    lv_timer_handler();
    ui_tick_anim();
    ble_tick();
    power_tick();
    imu_tick();
    splash_tick();

    handle_key1();
    handle_rotation_change();

    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state) {
        last_ble_state = bs;
        ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
    }

    static int last_pct = -2;
    static bool last_charging = false;
    int pct = power_battery_pct();
    bool charging = power_is_charging();
    if (pct != last_pct || charging != last_charging) {
        last_pct = pct;
        last_charging = charging;
        ui_update_battery(pct, charging);
    }

    check_serial_cmd();

    if (ble_has_data()) {
        if (parse_json(ble_get_data(), &usage)) {
            usage_rate_sample(usage.session_pct);
            splash_on_usage_update(&usage);
            ui_update(&usage);
            ble_send_ack();
        } else {
            ble_send_nack();
        }
    }

    delay(5);
}
