// ─────────────────────────────────────────────────────────────────────────────
// display.h — Display rendering interface
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <Arduino.h>
#include <vector>
#include "../include/flight_tracker.h"
#include "../include/commute.h"

// One transit departure (used by both display and api modules)
struct Departure {
    String line;        // e.g. "80", "2"
    String destination; // e.g. "Triemlispital"
    int    minutes;     // minutes until departure (0 = now)
    int    delay;       // delay in minutes (0 = on time)
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

// Step through day-brightness levels: 100→90→80→70→60→50→100 (wraps).
// Night mode is unaffected — it still overrides to NIGHT_BRIGHTNESS automatically.
void display_step_brightness();

// Show a centered message (used during boot, errors)
void display_show_status(const char* message);

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

// Boot animation (Matrix-style cascading characters).
// Start it before the blocking setup steps; update the status text as each
// phase begins; stop it when the board is ready to appear.
void display_boot_animation_start();
void display_boot_animation_stop();
