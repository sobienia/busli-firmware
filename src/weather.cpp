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

bool weather_fetch(WeatherData& out) {
    char url[320];
    snprintf(url, sizeof(url),
        "http://api.open-meteo.com/v1/forecast"
        "?latitude=%.3f&longitude=%.3f"
        "&current=temperature_2m,weather_code,uv_index"
        "&daily=weather_code,precipitation_probability_max,temperature_2m_max,uv_index_max"
        "&timezone=Europe%%2FZurich"
        "&forecast_days=1",
        (double)WEATHER_LAT, (double)WEATHER_LON);

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
    out.rain_today      = (daily_code >= 51) || (out.precip_prob_pct >= WEATHER_RAIN_PROB_PCT);
    out.valid           = true;

    Serial.printf("[Weather] cur=%.1f max=%.1f UV%d/%d rain=%s (code=%d prob=%d%%)\n",
        out.temp_c, out.temp_max_c, out.uv_index, out.uv_index_max,
        out.rain_today ? "yes" : "no",
        daily_code, out.precip_prob_pct);
    return true;
}
