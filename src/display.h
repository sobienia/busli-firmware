// ─────────────────────────────────────────────────────────────────────────────
// display.h — Display rendering interface
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <Arduino.h>
#include <vector>
#include "../include/flight_tracker.h"
#include "../include/commute.h"
#include "../include/stocks.h"
#include "../include/vehicle.h"
#include "weather.h"

// One transit departure (used by both display and api modules)
struct Departure {
    String line;        // e.g. "80", "2"
    String destination; // e.g. "Triemlispital"
    time_t dep_time;    // actual (prognosis) departure, UTC epoch — minutes are
                        // computed at draw time so the board counts down live
    int    delay;       // delay in minutes (0 = on time)
    String platform;    // e.g. "3", "41/42", "C"; "" = not reported
    bool   platform_changed = false;  // differs from the timetable — highlighted
    Vehicle vehicle = VEHICLE_BUS;    // icon when it's leaving now
};

// Initialize the display hardware (call once in setup())
void display_init();

// Load the previously saved day-brightness level from NVS and apply it.
// Call once in setup() after display_init().
void display_init_brightness();

// Set backlight brightness (0–100)
void display_set_brightness(uint8_t percent);

// Switch color theme at runtime. Call before the first draw.
// row0=100%, rows=80%, dim=50%, meta=30% — all RGB565.
void display_set_theme(uint16_t row0, uint16_t rows, uint16_t dim, uint16_t meta);

// Flip display orientation 180°. Invalidates all partial-redraw state so the
// next draw call does a full repaint in the new orientation.
void display_set_flipped(bool flipped);

// Step through brightness levels for the current mode (wraps), saved per mode:
//   day   (auto) → 100 → 80 → 60 → 40 → 20
//   night (auto) →  30 → 20 → 15 → 10 → 5
// "auto" only with a light sensor. Returns the new level (0 = auto).
uint8_t display_step_brightness();

// Light sensor present: offer "auto", and use it unless a level was chosen
void display_enable_auto_brightness();
// Current mode (day/night) follows the room light
bool display_auto_brightness();
// Room light in lux, about once a second — adjusts the backlight in auto mode
void display_set_ambient_light(float lux);
// An alert flash drives the backlight meanwhile (no automatic changes)
void display_pause_auto_brightness(bool paused);

// Night hours as minutes after midnight (start == end disables night mode).
// Defaults to NIGHT_START_HOUR..NIGHT_END_HOUR until set.
void display_set_night_schedule(int start_min, int end_min);
bool display_is_night();

// Re-apply the day/night brightness level (e.g. after an alert pulse).
void display_restore_brightness();

// Screen power for the screen-off night mode: off = backlight off + panel sleep.
// Turning it on forces a full repaint on the next draw.
void display_set_screen_on(bool on);
bool display_screen_on();

// Estimated battery runtime in hours (-1 = unknown). When the battery is low the
// header shows it next to the percentage, e.g. "18% ~2h".
void display_set_battery_estimate(float hours_left);

// Show a centered message (used during boot, errors)
void display_show_status(const char* message);

// Gesture hint: replaces the footer of the page on screen for duration_ms.
// display_hint_tick() (every loop) restores the footer afterwards.
void display_show_hint(const char* text, uint32_t duration_ms);
void display_hint_tick();

// Weather animation over the whole screen for a few seconds, with a message
// ("Rain in 10 min"). While active, call display_weather_anim_tick() instead of
// drawing the page; it returns false once finished (the page then repaints).
enum WeatherAnim { WX_ANIM_RAIN, WX_ANIM_SNOW, WX_ANIM_STORM };
void display_weather_anim_start(WeatherAnim kind, const char* text);
bool display_weather_anim_tick();
bool display_weather_anim_active();

// Show an OTA update prompt with No (left) and Yes (right) tap zones.
void display_show_ota_prompt(const char* cur_ver, const char* new_ver);

// Draw the main departure board.
// fetch_time: timestamp of the last successful fetch (used to skip row redraws
//             when departure data hasn't changed since the last screen update).
void display_draw_board(
    const char* stop_name,
    int stop_index,
    int stop_count,
    const std::vector<Departure>& departures,
    const char* weather_str,
    const char* uv_str,
    bool rain_today,
    int  rain_pct,
    bool from_cache,
    bool wifi_ok,
    int age_seconds,
    time_t fetch_time,
    bool large_font_mode,
    time_t countdown_target, // 0 = disabled; shows remaining time in footer when > now
    int    countdown_icon,   // 0=none 1=palm 2=calendar 3=plane
    bool   snow_today,       // show snowflake icon in footer
    bool   clear_today,      // show sun icon in footer
    int    battery_pct,      // 0–100; -1 = don't show battery icon
    bool   battery_charging  // true = show "+" after percentage
);

// Force a full redraw on the next display_draw_board call (call after stop changes,
// or any other event that clears the screen externally).
void display_invalidate();

// Draw the flight tracking screen for one slot.
// slot/flight_count drive the dot indicator in the header (like stop dots).
// If fi.callsign is empty, shows a "No flight configured" placeholder.
void display_draw_flight(int slot, int flight_count, const FlightInfo& fi, bool wifi_ok);

// Draw the commute board (home or work direction).
// direction_label: shown in header, e.g. "> Home" or "> Work"
// connection_idx:  which connection to show at the top (swipe-down advances it)
void display_draw_commute(
    const char* direction_label,
    int page_idx,
    int page_count,
    const CommuteData& data,
    int connection_idx,
    bool wifi_ok,
    int battery_pct,
    bool battery_charging,
    const char* weather_str,
    const char* uv_str,
    bool rain_today,
    int  rain_pct,
    bool snow_today,
    bool clear_today
);

// Draw the stock ticker page (one sub-page of up to per_page tickers).
// label:       header text, e.g. "Portfolio 1/2"
// quotes:      all configured tickers; first = index of the first one on this sub-page
// per_page:    5 → large font, one column; 10 → small font, two columns of 5
// market_open: false → footer shows "closed" instead of the data age
// Only cells whose quote changed are repainted, so updates don't flicker.
void display_draw_stocks(
    const char* label,
    int page_idx,
    int page_count,
    const StockQuote quotes[],
    int count,
    int first,
    int per_page,
    bool market_open,
    int age_seconds,
    bool wifi_ok,
    int battery_pct,
    bool battery_charging,
    const char* weather_str,
    const char* uv_str,
    bool rain_today,
    int  rain_pct,
    bool snow_today,
    bool clear_today
);

// Which list cell (0 … per_page-1) is at screen position x,y on the stocks
// page, or -1 if none. Lets the caller turn a tap into a ticker index.
int display_stock_cell_at(int x, int y, int per_page);

// Everything the standard footer shows
struct FooterInfo {
    const char* weather_str = "";
    const char* uv_str      = "";
    bool rain_today  = false;
    int  rain_pct    = 0;
    bool snow_today  = false;
    bool clear_today = false;
    bool wifi_ok     = true;
    int  battery_pct = -1;
    bool battery_charging = false;
};

// Detail view for one ticker: price, change, previous close, open, day range,
// 52-week range and a large intraday chart.
// label: header text, e.g. "VWCE  3/11"; chart: nullptr while it's loading.
void display_draw_stock_detail(const char* label, int page_idx, int page_count,
                               const StockQuote& q, const StockChart* chart,
                               bool market_open, int age_seconds,
                               const FooterInfo& footer);

// Weather page: current conditions + today (left), next 12 hours as a
// temperature line over rain-chance bars (right), air quality and pollen in
// the footer. d == nullptr while the first fetch is running (or, with
// not_found, when a town from Settings couldn't be located).
// label: header text, e.g. "Weather  Zürich  1/3".
void display_draw_weather(const char* label, int page_idx, int page_count,
                          const WeatherDetail* d, bool not_found, const FooterInfo& footer);

// One "label  value" line on an info page. state: 0 = normal, 1 = good, 2 = problem.
// color: explicit value colour (RGB565) instead of the state's; 0 = use state.
struct InfoRow {
    String   label;
    String   value;
    uint8_t  state = 0;
    uint16_t color = 0;
    InfoRow() {}
    InfoRow(const String& l, const String& v, uint8_t s = 0, uint16_t c = 0)
        : label(l), value(v), state(s), color(c) {}
};

// Two-column list page (device info). Repaints only when the text changes.
void display_draw_info(const char* label, int page_idx, int page_count,
                       const InfoRow* left, int n_left,
                       const InfoRow* right, int n_right,
                       const FooterInfo& footer);

// Setup hotspot screen: two QR codes — "1. Join Wi-Fi" (joins the hotspot) and
// "2. Open settings" (opens the page) — plus the same in words.
void display_show_setup_qr(const char* ssid, const char* url);

// Settings shortcut: one QR code that opens (and pairs) the settings page on
// the normal Wi-Fi, with the address in words beside it.
void display_show_settings_qr(const char* url, const char* hostname, const char* ip);

// Today page: one column of "label  value" rows, then the quote of the day
// (word-wrapped, with its author) at the bottom. quote may be "" (not loaded).
void display_draw_today(const char* label, int page_idx, int page_count,
                        const InfoRow* rows, int n_rows,
                        const String& quote, const String& author,
                        const char* bottom_hint, const FooterInfo& footer);

// Clock: large HH:MM with a small line underneath (e.g. date + temp), on a
// scene for the weather outside (-1 = none). animate: the scene moves (call
// every loop, ~15 frames/s); otherwise one still frame a minute. bright: day
// clock (Today page) instead of the dim night clock.
enum ClockScene {
    SCENE_RAIN = WX_ANIM_RAIN, SCENE_SNOW = WX_ANIM_SNOW, SCENE_STORM = WX_ANIM_STORM,
    SCENE_SUN, SCENE_CLOUDS, SCENE_PARTLY_SUN, SCENE_MOON, SCENE_PARTLY_MOON,
    SCENE_RAIN_NIGHT, SCENE_SNOW_NIGHT, SCENE_STORM_NIGHT,   // moon behind a cloud
    SCENE_FOG, SCENE_FOG_NIGHT,                              // drifting mist (+ moon glow)
    SCENE_SHOWERS,                                           // sun behind a rain cloud
    SCENE_COUNT
};
void display_draw_night_clock(const char* subtitle, int scene, bool animate, bool bright);

// Clock style (stored as an int in Settings — keep values): seven-segment digits,
// a split-flap board whose cards flip when the time changes, or the Matrix rain
// with the time made of its symbols (no weather)
enum ClockStyle { CLOCK_DIGITAL = 0, CLOCK_FLAP = 1, CLOCK_MATRIX = 2, CLOCK_STYLE_COUNT };
void display_set_clock_style(int style);
// A line at the bottom of the clock for duration_ms ("Swipe: clock style · Tap: back")
void display_clock_hint(const char* text, uint32_t duration_ms);

// Boot animation (Matrix-style cascading characters).
// Start it before the blocking setup steps; update the status text as each
// phase begins; stop it when the board is ready to appear.
void display_boot_animation_start();
// Name for the ending ("Wake up, AJ..."); "" = "Wake up..."
void display_boot_animation_set_name(const String& name);
// Blocks until the animation is gone. ending = play "Wake up, …" first (~2.5 s;
// skip_requested() returning true cuts it short — e.g. a touch).
void display_boot_animation_stop(bool ending = false, bool (*skip_requested)() = nullptr);
bool display_boot_animation_skipped();

// ── Pager UI screens ──────────────────────────────────────────────────────────
// All pager screens fill the full 480×222 display (no external footer/header).
// highlighted: index of the currently selected item, or -1 for none.

// Activity selection: 2 rows × 5 columns of labeled tiles.
// acts[10] — activity labels (up to 8 chars each).
void display_draw_pager_activities(const char* const acts[], int highlighted);

// Time selection: 2 rows × 3 columns.
// activity_label: shown in header for context (e.g. "Beer").
// times[6] — time labels.
void display_draw_pager_times(const char* activity_label,
                               const char* const times[], int highlighted);

// Friend selection: vertical list, tap to send.
// header: shows the composed message (e.g. "Beer in 30 min?").
// names[n] — friend display names; n — count.
void display_draw_pager_friends(const char* header,
                                 const char* const names[], int n, int highlighted);

// Incoming message overlay: header + icon + text + timestamp + reply/close buttons.
// header:       shown top-left of the header bar (e.g. "From: AJ").
// icon_idx:     activity index 0–9; -1 = no icon (used for invite messages).
// sent_at:      epoch of original send (right-aligned in header); 0 = hidden.
// show_replies: true → [Yes][No][Later][Close]; false → single full-width [Close].
// highlighted:  0–3 for 4-btn mode; 0 for single-btn mode; -1 = none.
// re_text:      original question text to show as a quote block (nullptr = none).
// re_icon_idx:  activity index for the quoted question's icon; -1 = no icon.
void display_draw_pager_incoming(const char* header,
                                  const char* text,
                                  int icon_idx,
                                  time_t sent_at,
                                  bool show_replies,
                                  int highlighted,
                                  const char* re_text   = nullptr,
                                  int         re_icon_idx = -1);
