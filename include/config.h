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
// Default: amber (looks like an old train station display)

#define COLOR_ROW0    0xFE60   // bright amber for first row
#define COLOR_ROWS    0xCCC0   // medium amber for other rows
#define COLOR_DIM     0x8B20   // dim amber for delays
#define COLOR_META    0x4A00   // very dim amber for separators
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
// ║  HARDWARE PINS                                                            ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

#define PIN_BOOT_BUTTON   0     // BOOT button (used for switching stops)

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  TRANSIT API                                                              ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

#define API_BASE_URL  "http://transport.opendata.ch/v1/stationboard"
#define API_TIMEOUT_MS 10000

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  WEATHER (Open-Meteo — free, no API key required)                         ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

#define WEATHER_LAT            47.376   // Zürich
#define WEATHER_LON             8.541
#define WEATHER_REFRESH_SEC     900     // 15 minutes
#define WEATHER_TIMEOUT_MS    10000
#define WEATHER_RAIN_PROB_PCT    30     // umbrella if daily precip probability >= this
