// ─────────────────────────────────────────────────────────────────────────────
// weather.cpp — Open-Meteo weather fetcher
// ─────────────────────────────────────────────────────────────────────────────
// API: http://api.open-meteo.com/v1/forecast  (free, no key)
// Fetches: current temperature, UV index, and today's precipitation probability
// to decide whether to show the umbrella icon.
//
// WMO weather codes >= 51 indicate precipitation (drizzle, rain, snow, storms).
// We also check daily precipitation_probability_max as a forward-looking signal.
// ─────────────────────────────────────────────────────────────────────────────

#include "weather.h"
#include "../include/config.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>

// Rain alert: 15-minute precipitation slots (each value = rain in the 15 min
// ending at its timestamp). A slot counts as wet at RAIN_ALERT_MM or more.
static const int   RAIN_SLOTS    = RAIN_ALERT_MINUTES / 15 + 1;
static const float RAIN_ALERT_MM = 0.1f;

bool weather_fetch(float lat, float lon, WeatherData& out) {
    char url[420];
    snprintf(url, sizeof(url),
        "http://api.open-meteo.com/v1/forecast"
        "?latitude=%.3f&longitude=%.3f"
        "&current=temperature_2m,weather_code,uv_index,precipitation"
        "&daily=weather_code,precipitation_probability_max,temperature_2m_max,uv_index_max"
        "&minutely_15=precipitation&forecast_minutely_15=%d"
        "&timezone=Europe%%2FZurich&timeformat=unixtime"
        "&forecast_days=1",
        (double)lat, (double)lon, RAIN_SLOTS);

    Serial.print("[Weather] Fetching: ");
    Serial.println(url);

    HTTPClient http;
    http.setTimeout(WEATHER_TIMEOUT_MS);
    http.setReuse(false);

    if (!http.begin(url)) {
        Serial.println("[Weather] http.begin() failed");
        return false;
    }

    int code = http.GET();
    if (code != 200) {
        Serial.printf("[Weather] HTTP error: %d\n", code);
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();
    Serial.printf("[Weather] Body: %d bytes\n", body.length());

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        Serial.printf("[Weather] JSON error: %s\n", err.c_str());
        return false;
    }

    out.temp_c       = doc["current"]["temperature_2m"]                  | 0.0f;
    out.temp_max_c   = doc["daily"]["temperature_2m_max"][0]             | out.temp_c;
    out.uv_index     = (int)roundf(doc["current"]["uv_index"]            | 0.0f);
    out.uv_index_max = (int)roundf(doc["daily"]["uv_index_max"][0]       | 0.0f);

    int daily_code      = doc["daily"]["weather_code"][0]                  | 0;
    out.precip_prob_pct = doc["daily"]["precipitation_probability_max"][0] | 0;

    // Classify condition — snow and rain are mutually exclusive for icon purposes
    bool is_snow = (daily_code >= 71 && daily_code <= 77) ||
                   (daily_code >= 85 && daily_code <= 86);
    bool is_rain = !is_snow && (daily_code >= 51);
    out.snow_today  = is_snow;
    out.clear_today = (daily_code == 0 || daily_code == 1);
    out.rain_today  = is_rain || (!is_snow && out.precip_prob_pct >= WEATHER_RAIN_PROB_PCT);

    // Short-term rain: is it raining now, and when does it start / stop?
    JsonArray times = doc["minutely_15"]["time"];
    JsonArray mm    = doc["minutely_15"]["precipitation"];
    time_t now      = time(nullptr);
    out.raining_now = (doc["current"]["precipitation"] | 0.0f) >= RAIN_ALERT_MM;
    out.rain_start  = 0;
    out.rain_end    = 0;
    for (size_t i = 0; i < times.size() && i < mm.size(); i++) {
        time_t slot_end = (time_t)(times[i] | 0L);
        if (slot_end <= now) continue;
        bool wet = (mm[i] | 0.0f) >= RAIN_ALERT_MM;
        if (!out.raining_now && wet)  { out.rain_start = slot_end - 15 * 60; break; }
        if (out.raining_now && !wet)  { out.rain_end   = slot_end - 15 * 60; break; }
    }
    if (out.rain_start > 0 && out.rain_start <= now) {   // already started
        out.raining_now = true;
        out.rain_start  = 0;
    }
    out.valid = true;

    Serial.printf("[Weather] cur=%.1f max=%.1f UV%d/%d rain=%s snow=%s clear=%s (code=%d prob=%d%%)"
                  " now=%s start=%ld end=%ld\n",
        out.temp_c, out.temp_max_c, out.uv_index, out.uv_index_max,
        out.rain_today ? "y" : "n", out.snow_today ? "y" : "n",
        out.clear_today ? "y" : "n", daily_code, out.precip_prob_pct,
        out.raining_now ? "y" : "n", (long)out.rain_start, (long)out.rain_end);

    return true;
}
