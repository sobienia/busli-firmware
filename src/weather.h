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
    int   code_now;          // WMO weather code right now (snow 71–77/85–86, storm 95–99)
    bool  is_day;            // the sun is up there (night clock: sun or moon)
    bool  valid;             // false until at least one successful fetch
};

// Kind of weather by WMO code
inline bool weather_code_snow(int c)  { return (c >= 71 && c <= 77) || c == 85 || c == 86; }
inline bool weather_code_storm(int c) { return c >= 95 && c <= 99; }

// Fetch current weather, today's outlook and the short-term rain forecast for
// a location from Open-Meteo. Returns true on success; `out` is unchanged on failure.
bool weather_fetch(float lat, float lon, WeatherData& out);

// ── Weather page ─────────────────────────────────────────────────────────────

#define WEATHER_HOURS 12

enum Pollen { POLLEN_ALDER, POLLEN_BIRCH, POLLEN_GRASS, POLLEN_MUGWORT,
              POLLEN_OLIVE, POLLEN_RAGWEED, POLLEN_COUNT };

struct WeatherDetail {
    float  temp_c       = 0;
    float  feels_like_c = 0;               // apparent temperature (wind, humidity)
    float  wind_kmh     = 0;
    float  gust_kmh     = 0;
    int    weather_code = 0;
    int    uv_index     = 0;
    float  temp_min_c   = 0, temp_max_c = 0;
    int    rain_prob_max = 0;              // today, %
    time_t sunrise = 0, sunset = 0;
    // Next WEATHER_HOURS hours, starting with the current hour
    int    hours = 0;
    time_t hour_time[WEATHER_HOURS];
    float  hour_temp[WEATHER_HOURS];
    int    hour_rain_prob[WEATHER_HOURS];  // %
    // Air quality (Open-Meteo air-quality API; pollen: Europe, in season)
    int    aqi = -1;                       // European AQI; -1 = unknown
    float  pollen[POLLEN_COUNT];           // grains/m³; < 0 = not reported
    time_t fetched = 0;
    bool   valid = false;
};

// Forecast + air quality for the weather page (two small requests, ~2 KB).
// Returns true if the forecast part succeeded; air quality is best effort.
bool weather_fetch_detail(float lat, float lon, WeatherDetail& out);

// The parsing steps on their own (also used by the tests in test/).
// now = the time the rain alert is computed for.
bool weather_parse(const String& body, time_t now, WeatherData& out);
bool weather_parse_detail(const String& body, WeatherDetail& out);
bool weather_parse_air(const String& body, WeatherDetail& d);   // fills aqi + pollen

// Words for the page
const char* weather_condition_text(int wmo_code);   // "Partly cloudy"
const char* weather_aqi_text(int aqi);              // "Good", "Fair", … "" if unknown
// 0 = none, 1 = low, 2 = moderate, 3 = high, 4 = very high (per-type thresholds)
int         weather_pollen_level(Pollen type, float grains);
const char* weather_pollen_name(Pollen type);       // "Birch"
const char* weather_level_text(int level);          // "low" … "very high"
