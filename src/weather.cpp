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

// ── Weather page ─────────────────────────────────────────────────────────────

const char* weather_condition_text(int c) {
    if (c == 0)             return "Clear";
    if (c == 1)             return "Mainly clear";
    if (c == 2)             return "Partly cloudy";
    if (c == 3)             return "Overcast";
    if (c == 45 || c == 48) return "Fog";
    if (c >= 51 && c <= 57) return "Drizzle";
    if (c >= 61 && c <= 67) return "Rain";
    if (c >= 71 && c <= 77) return "Snow";
    if (c >= 80 && c <= 82) return "Showers";
    if (c >= 85 && c <= 86) return "Snow showers";
    if (c >= 95)            return "Thunderstorm";
    return "";
}

// European AQI bands (EEA)
const char* weather_aqi_text(int aqi) {
    if (aqi < 0)    return "";
    if (aqi <= 20)  return "Good";
    if (aqi <= 40)  return "Fair";
    if (aqi <= 60)  return "Moderate";
    if (aqi <= 80)  return "Poor";
    if (aqi <= 100) return "Very poor";
    return "Extremely poor";
}

// Thresholds (grains/m³) for low / moderate / high / very high, after the
// MeteoSwiss/DWD style classes: trees are counted in larger numbers than grass,
// and weeds (mugwort, ragweed) cause symptoms at very low counts.
static const struct { const char* name; float t[4]; } POLLEN_INFO[POLLEN_COUNT] = {
    { "Alder",   { 1, 11, 71, 301 } },
    { "Birch",   { 1, 11, 71, 301 } },
    { "Grass",   { 1, 20, 50, 150 } },
    { "Mugwort", { 1,  6, 16,  51 } },
    { "Olive",   { 1, 11, 71, 301 } },
    { "Ragweed", { 1,  6, 16,  51 } },
};

int weather_pollen_level(Pollen type, float grains) {
    if (type < 0 || type >= POLLEN_COUNT || grains < 0) return 0;
    int level = 0;
    for (int i = 0; i < 4; i++) if (grains >= POLLEN_INFO[type].t[i]) level = i + 1;
    return level;
}

const char* weather_pollen_name(Pollen type) {
    return (type >= 0 && type < POLLEN_COUNT) ? POLLEN_INFO[type].name : "";
}

const char* weather_level_text(int level) {
    static const char* T[] = { "none", "low", "moderate", "high", "very high" };
    return T[constrain(level, 0, 4)];
}

bool weather_fetch_detail(float lat, float lon, WeatherDetail& out) {
    char url[480];
    snprintf(url, sizeof(url),
        "http://api.open-meteo.com/v1/forecast"
        "?latitude=%.3f&longitude=%.3f"
        "&current=temperature_2m,weather_code,uv_index,apparent_temperature,"
                 "wind_speed_10m,wind_gusts_10m"
        "&hourly=temperature_2m,precipitation_probability&forecast_hours=%d"
        "&daily=temperature_2m_min,temperature_2m_max,sunrise,sunset,precipitation_probability_max"
        "&timezone=Europe%%2FZurich&timeformat=unixtime&forecast_days=1",
        (double)lat, (double)lon, WEATHER_HOURS);

    HTTPClient http;
    http.setTimeout(WEATHER_TIMEOUT_MS);
    http.setReuse(false);
    if (!http.begin(url)) return false;
    int code = http.GET();
    String body = (code == 200) ? http.getString() : "";
    http.end();
    if (code != 200) { Serial.printf("[Weather] detail HTTP %d\n", code); return false; }

    JsonDocument doc;
    if (deserializeJson(doc, body)) { Serial.println("[Weather] detail JSON error"); return false; }

    WeatherDetail d;
    d.temp_c        = doc["current"]["temperature_2m"] | 0.0f;
    d.feels_like_c  = doc["current"]["apparent_temperature"] | d.temp_c;
    d.wind_kmh      = doc["current"]["wind_speed_10m"] | 0.0f;   // km/h is the API default
    d.gust_kmh      = doc["current"]["wind_gusts_10m"] | 0.0f;
    d.weather_code  = doc["current"]["weather_code"]   | 0;
    d.uv_index      = (int)roundf(doc["current"]["uv_index"] | 0.0f);
    d.temp_min_c    = doc["daily"]["temperature_2m_min"][0] | d.temp_c;
    d.temp_max_c    = doc["daily"]["temperature_2m_max"][0] | d.temp_c;
    d.rain_prob_max = doc["daily"]["precipitation_probability_max"][0] | 0;
    d.sunrise       = (time_t)(doc["daily"]["sunrise"][0] | 0L);
    d.sunset        = (time_t)(doc["daily"]["sunset"][0]  | 0L);
    JsonArray ht = doc["hourly"]["time"];
    JsonArray hT = doc["hourly"]["temperature_2m"];
    JsonArray hP = doc["hourly"]["precipitation_probability"];
    d.hours = std::min((int)ht.size(), WEATHER_HOURS);
    for (int i = 0; i < d.hours; i++) {
        d.hour_time[i]      = (time_t)(ht[i] | 0L);
        d.hour_temp[i]      = hT[i] | 0.0f;
        d.hour_rain_prob[i] = hP[i] | 0;
    }
    for (float& p : d.pollen) p = -1;
    d.fetched = time(nullptr);
    d.valid   = true;

    // Air quality + pollen — separate API; the page works without it
    char aq_url[320];
    snprintf(aq_url, sizeof(aq_url),
        "http://air-quality-api.open-meteo.com/v1/air-quality?latitude=%.3f&longitude=%.3f"
        "&current=european_aqi,alder_pollen,birch_pollen,grass_pollen,"
        "mugwort_pollen,olive_pollen,ragweed_pollen",
        (double)lat, (double)lon);
    HTTPClient aq;
    aq.setTimeout(WEATHER_TIMEOUT_MS);
    aq.setReuse(false);
    if (aq.begin(aq_url) && aq.GET() == 200) {
        JsonDocument adoc;
        if (!deserializeJson(adoc, aq.getString())) {
            JsonObject c = adoc["current"];
            d.aqi = c["european_aqi"].isNull() ? -1 : (int)c["european_aqi"];
            const char* keys[POLLEN_COUNT] = { "alder_pollen", "birch_pollen", "grass_pollen",
                                               "mugwort_pollen", "olive_pollen", "ragweed_pollen" };
            for (int i = 0; i < POLLEN_COUNT; i++)
                d.pollen[i] = c[keys[i]].isNull() ? -1.0f : c[keys[i]].as<float>();
        }
    }
    aq.end();

    Serial.printf("[Weather] detail: %.1fC code=%d %d hours, AQI %d\n",
                  d.temp_c, d.weather_code, d.hours, d.aqi);
    out = d;
    return true;
}

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
