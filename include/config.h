// ─────────────────────────────────────────────────────────────────────────────
// config.h — Project-wide constants and settings
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  THINGS YOU MIGHT WANT TO CHANGE                                          ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

// Board hardware version. LILYGO changed the backlight driver between V1.0
// and V1.1. Check the back of your device near the USB-C port for "V1.1".
// If it has "V1.1" → set to 11. Otherwise → leave at 10.
#define BOARD_VERSION 11

// How often to refresh transit data (in seconds)
#define REFRESH_INTERVAL_SEC 30

// Display brightness (0–100, percent)
#define DAY_BRIGHTNESS    80
#define NIGHT_BRIGHTNESS  15

// Night mode hours (24-hour format)
#define NIGHT_START_HOUR  23
#define NIGHT_END_HOUR    6

// Timezone — Switzerland CET (DST handled automatically in code)
#define TIMEZONE_OFFSET_SEC  3600
#define DST_OFFSET_SEC       3600

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  THEME COLORS (16-bit RGB565 format)                                      ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
// ZVV amber (#F7B500 = RGB 247,181,0) at four brightness levels
#define COLOR_ROW0    0xF5A0   // 100% — main text
#define COLOR_ROWS    0xC480   //  80% — boot animation trail
#define COLOR_DIM     0x7AC0   //  50% — delays / stale indicator
#define COLOR_META    0x49A0   //  30% — separators / inactive dots
#define COLOR_BLACK   0x0000

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  DISPLAY LAYOUT                                                           ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

#define SCREEN_W   480
#define SCREEN_H   222

#define HEADER_H   36       // top bar (stop name, clock, dots)
#define FOOTER_H   34       // bottom bar (weather, rain, UV, refresh age)
#define ROW_COUNT  4        // number of departure rows
#define PAD        8        // padding from screen edge

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  BACKGROUND FETCH TASK                                                    ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

#define FETCH_TASK_STACK      12288  // FreeRTOS task stack (bytes)
#define FETCH_TASK_PRIORITY   1      // Low priority — runs on WiFi core 0
#define FETCH_INTERVAL_MS     60000  // Full cycle time across all stops (ms)

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  TOUCH                                                                    ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

#define TOUCH_SDA_PIN        5
#define TOUCH_SCL_PIN        6
#define TOUCH_I2C_ADDR       0x5A  // CST226SE (Mutual) — confirmed by I2C scan
#define TOUCH_SWIPE_PX       40     // minimum displacement to register a swipe
#define TOUCH_LONG_PRESS_MS  2000
#define TOUCH_DOUBLE_TAP_MS  350

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  HARDWARE PINS                                                            ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

#define PIN_BOOT_BUTTON   0     // BOOT button (used for switching stops)

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  FOOTER LAYOUT                                                            ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

#define FOOTER_AGE_REGION_W  120 // right-anchored dynamic region (age/countdown + dot)

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  TRANSIT API                                                              ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

#define API_BASE_URL       "http://transport.opendata.ch/v1/stationboard"
#define API_TIMEOUT_MS     15000
#define API_FETCH_LIMIT    20    // entries requested per stop

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  WEATHER (Open-Meteo — free, no API key required)                         ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

#define WEATHER_LAT            47.376   // Zürich
#define WEATHER_LON             8.541
#define WEATHER_REFRESH_SEC     900     // 15 minutes
#define WEATHER_TIMEOUT_MS     5000
#define WEATHER_RAIN_PROB_PCT    30     // umbrella if daily precip probability >= this

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  CONFIG PORTAL                                                            ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

#define PORTAL_TIMEOUT_MS  300000  // 5 minutes; 0 = no timeout
