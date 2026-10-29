#pragma once
#include <Arduino.h>

struct WeatherData {
    float temp_c;
    float temp_max_c;
    int   uv_index;
    int   uv_index_max;
    int   precip_prob_pct;   // 0-100, max daily precipitation probability
    bool  rain_today;        // rain/drizzle expected (shows umbrella icon)
    bool  snow_today;        // snow expected (shows snowflake icon)
    bool  clear_today;       // clear/sunny sky (shows sun icon)
    // Rain alert from the 15-minute forecast (next RAIN_ALERT_MINUTES):
    bool   raining_now;      // precipitation right now
    time_t rain_start;       // dry now: when rain starts (0 = none expected)
    time_t rain_end;         // raining: when it stops (0 = not within the window)
    bool  valid;             // false until at least one successful fetch
};

// Fetch current weather, today's outlook and the short-term rain forecast for
// a location from Open-Meteo. Returns true on success; `out` is unchanged on failure.
bool weather_fetch(float lat, float lon, WeatherData& out);
