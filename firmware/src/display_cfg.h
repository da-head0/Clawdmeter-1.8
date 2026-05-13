#pragma once

#include <Arduino_GFX_Library.h>
#include <XPowersLib.h>
#include <SensorQMI8658.hpp>
#include <Wire.h>

// ---- Display resolution (Waveshare ESP32-S3-Touch-AMOLED-1.8) ----
#define LCD_WIDTH   368
#define LCD_HEIGHT  448

// ---- QSPI display pins (SH8601) ----
#define LCD_CS      12
#define LCD_SCLK    11
#define LCD_SDIO0   4
#define LCD_SDIO1   5
#define LCD_SDIO2   6
#define LCD_SDIO3   7
// LCD reset is routed through the TCA9554 expander (EXIO 0), not a direct GPIO.

// ---- Shared I2C bus (touch, PMU, IMU, RTC, codec, expander) ----
#define IIC_SDA     15
#define IIC_SCL     14

// ---- Touch (FT3168) ----
#define TP_INT      21
// Touch reset is via TCA9554 (EXIO 1).

// ---- TCA9554 I/O expander ----
#define TCA9554_ADDR    0x20
#define EXIO_LCD_RESET  0
#define EXIO_TP_RESET   1
#define EXIO_DSI_PWR_EN 2
#define EXIO_AXP_IRQ    5

// ---- Single physical button (1.8" board only exposes KEY1) ----
#define BTN_KEY1    0  // GPIO0; active-low, has external pullup

// ---- PMU (AXP2101) ----
#define AXP2101_ADDR 0x34

// ---- Global hardware objects (defined in main.cpp) ----
extern Arduino_DataBus *bus;
extern Arduino_SH8601  *gfx;
extern XPowersPMU pmu;
extern SensorQMI8658 imu;
