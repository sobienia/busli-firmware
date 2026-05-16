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

// Transit refresh intervals
#define ACTIVE_REFRESH_SEC    30   // currently-displayed stop
#define INACTIVE_REFRESH_SEC  90   // background stops (3× less frequent)

// Display brightness (0–100, percent)
#define DAY_BRIGHTNESS    80
#define NIGHT_BRIGHTNESS  15

// Night mode hours (24-hour format)
#define NIGHT_START_HOUR  23
#define NIGHT_END_HOUR    6

// Timezone — Switzerland CET (DST handled automatically in code)
#define TIMEZONE_OFFSET_SEC  3600
#define DST_OFFSET_SEC       3600
#define POSIX_TZ  "CET-1CEST,M3.5.0,M10.5.0/3"

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  THEME COLORS (16-bit RGB565 format)                                      ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
// Transit amber (~RGB 240,144,0 = #F09000) at four brightness levels.
// G/R ratio ≈ 0.6 gives warm amber; the old palette (G/R ≈ 0.75) read as yellow.
#define COLOR_ROW0    0xF480   // 100% — main text
#define COLOR_ROWS    0xC3A0   //  80% — boot animation trail
#define COLOR_DIM     0x7A40   //  50% — delays / stale indicator
#define COLOR_META    0x4960   //  30% — separators / inactive dots
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

#define FETCH_TASK_STACK      16384  // FreeRTOS task stack (bytes)
#define FETCH_TASK_PRIORITY   1      // Low priority — runs on WiFi core 0
#define FETCH_LOOP_MS         5000   // How often the background task checks for due work
#define FLIGHT_REFRESH_SEC    300    // Flight data refresh interval (5 min)
#define NTP_RESYNC_SEC        86400  // Re-sync NTP once per day

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  TOUCH                                                                    ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

#define TOUCH_SDA_PIN        5
#define TOUCH_SCL_PIN        6
#define TOUCH_I2C_ADDR       0x5A  // CST226SE (Mutual) — confirmed by I2C scan
#define TOUCH_SWIPE_PX       25     // minimum displacement to register a swipe (lower = more sensitive)
#define TOUCH_LONG_PRESS_MS  2000
#define TOUCH_DOUBLE_TAP_MS  350

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  HARDWARE PINS                                                            ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

#define PIN_BOOT_BUTTON   0     // BOOT button: short press = prev stop, long = refresh, 3s = config
#define PIN_BTN_ZOOM     38     // bottom-left button:  short press toggles large-font mode
#define PIN_BTN_BRIGHT   12     // bottom-right button: short press cycles brightness
#define PIN_BTN_THEME    16     // side button (confirmed GPIO 16): short press cycles color theme
#define PIN_BATTERY_ADC   4     // battery ADC: reads half the battery voltage through a 1:2 divider

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

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  COMMUTE REFRESH                                                          ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

#define COMMUTE_REFRESH_SEC  60    // how often the background task re-fetches commute data

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  PARCEL TRACKING (Swiss Post)                                             ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

#define PARCEL_REFRESH_SEC  3600   // re-check tracking status once per hour

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  FIRMWARE / OTA                                                           ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
// Bump FIRMWARE_VERSION before each release. The device checks OTA_VERSION_URL
// hourly and self-flashes if the remote version is newer.
//
// Hosting on GitHub:
//  1. Commit a file (e.g. ota/version.json) with content:
//       {"version":"X.Y.Z","url":"https://github.com/<user>/<repo>/releases/download/vX.Y.Z/firmware.bin"}
//  2. Set OTA_VERSION_URL to the raw URL of that file.
//  3. On each release: bump FIRMWARE_VERSION here, build, upload firmware.bin
//     to GitHub Releases, update version.json to point to the new binary.
//     Every device — including a friend's — picks it up on the next hourly check.
//
// Leave OTA_VERSION_URL as "" to disable OTA checking entirely.
#define FIRMWARE_VERSION        "1.5.4"
#define OTA_VERSION_URL         "https://raw.githubusercontent.com/sobienia/busli-firmware/master/ota/version.json"
#define OTA_CHECK_INTERVAL_SEC  86400

// Public URL of the GitHub Releases page where users can download firmware.bin manually.
// Shown as a link in the config portal /update page. Leave "" to hide.
#define FIRMWARE_RELEASE_URL    "https://github.com/sobienia/busli-firmware/releases"

