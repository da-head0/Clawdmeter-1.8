#include "expander.h"
#include "display_cfg.h"
#include <Adafruit_XCA9554.h>
#include <Arduino.h>

static Adafruit_XCA9554 g_expander;

bool expander_init(void) {
    if (!g_expander.begin(TCA9554_ADDR)) {
        Serial.println("TCA9554 init failed");
        return false;
    }
    g_expander.pinMode(EXIO_LCD_RESET,  OUTPUT);
    g_expander.pinMode(EXIO_TP_RESET,   OUTPUT);
    g_expander.pinMode(EXIO_DSI_PWR_EN, OUTPUT);
    Serial.println("TCA9554 init OK");
    return true;
}

void expander_reset_sequence(void) {
    g_expander.digitalWrite(EXIO_LCD_RESET,  LOW);
    g_expander.digitalWrite(EXIO_TP_RESET,   LOW);
    g_expander.digitalWrite(EXIO_DSI_PWR_EN, LOW);
    delay(20);
    g_expander.digitalWrite(EXIO_LCD_RESET,  HIGH);
    g_expander.digitalWrite(EXIO_TP_RESET,   HIGH);
    g_expander.digitalWrite(EXIO_DSI_PWR_EN, HIGH);
    delay(20);
}
