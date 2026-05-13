#pragma once
#include <stdint.h>

// TCA9554 I2C expander wrapper for the 1.8" AMOLED board.
// The expander gates LCD_RESET (EXIO 0), TP_RESET (EXIO 1) and DSI power
// enable (EXIO 2). The display/touch chips will not respond on the QSPI/I2C
// bus until the expander pulses these high after init.
bool expander_init(void);
void expander_reset_sequence(void);
