// ─────────────────────────────────────────────────────────────────────────────
// User_Setup.h — Pin definitions for the T-Display S3 Pro
// ─────────────────────────────────────────────────────────────────────────────
// These are documentation only. We use Arduino_GFX which takes pin numbers
// directly in code (see src/display.cpp), not via #defines like TFT_eSPI did.
//
// PIN ASSIGNMENTS for T-Display S3 Pro (from LILYGO's official board file):
//
//   Display (ST7796 SPI):
//     SCLK  = GPIO 18
//     MOSI  = GPIO 17
//     MISO  = GPIO  8  (rarely used)
//     CS    = GPIO 39
//     DC    = GPIO  9
//     RST   = GPIO 47
//     BL    = GPIO 48  (backlight, PWM)
//
//   Touchscreen (CST226SE, I2C):
//     SDA   = GPIO  5
//     SCL   = GPIO  6
//     INT   = GPIO  7
//     RST   = GPIO 13
//
//   Buttons:
//     BOOT  = GPIO  0
//     IO12  = GPIO 12  (user button on side)
//     IO16  = GPIO 16  (user button on side)
//
//   Battery / Power:
//     SY6970 PMIC via I2C (same bus as touch + light sensor)
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

// (intentionally empty — Arduino_GFX takes pins as parameters in display.cpp)
