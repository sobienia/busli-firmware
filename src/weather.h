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
    bool  valid;             // false until at least one successful fetch
};

// Fetch current weather + today's rain outlook from Open-Meteo.
// Returns true on success; `out` is unchanged on failure.
bool weather_fetch(WeatherData& out);
