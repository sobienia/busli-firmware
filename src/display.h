// ─────────────────────────────────────────────────────────────────────────────
// display.h — Display rendering interface
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <Arduino.h>
#include <vector>

// One transit departure (used by both display and api modules)
struct Departure {
    String line;        // e.g. "80", "2"
    String destination; // e.g. "Triemlispital"
    int    minutes;     // minutes until departure (0 = now)
    int    delay;       // delay in minutes (0 = on time)
};

// Initialize the display hardware (call once in setup())
void display_init();

// Set backlight brightness (0–100)
void display_set_brightness(uint8_t percent);

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
    bool from_cache,
    bool wifi_ok,
    int age_seconds,
    time_t fetch_time
);

// Force a full redraw on the next display_draw_board call (call after stop changes,
// or any other event that clears the screen externally).
void display_invalidate();

// Boot animation (Matrix-style cascading characters)
void display_boot_animation(uint16_t duration_ms);
