// ─────────────────────────────────────────────────────────────────────────────
// today_data.cpp — Water temperatures (Lake Zürich, Limmat) + quote of the day
// ─────────────────────────────────────────────────────────────────────────────

#include "../include/today_data.h"
#include "../include/devlog.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

static const int TIMEOUT_MS = 8000;

// GET a small document over HTTPS (public data — certificate not checked)
static bool get_body(const char* url, String& body) {
    WiFiClientSecure wc;
    wc.setInsecure();
    HTTPClient http;
    http.setTimeout(TIMEOUT_MS);
    http.setReuse(false);
    if (!http.begin(wc, url)) return false;
    http.addHeader("User-Agent", "Busli/1.7");
    int code = http.GET();
    body = (code == 200) ? http.getString() : "";
    http.end();
    if (code != 200) { Log.printf("[Today] %s → HTTP %d\n", url, code); return false; }
    return true;
}

// Lake Zürich — latest reading of the Tiefenbrunnen station (tecdottir)
float water_parse_lake(const String& body) {
    JsonDocument filter;
    filter["result"][0]["values"]["water_temperature"]["value"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return NAN;
    JsonVariant v = doc["result"][0]["values"]["water_temperature"]["value"];
    return v.isNull() ? NAN : v.as<float>();
}

// Limmat — BAFU hydro data via existenz.ch
float water_parse_river(const String& body) {
    JsonDocument doc;
    if (deserializeJson(doc, body)) return NAN;
    JsonVariant v = doc["payload"][0]["val"];
    return v.isNull() ? NAN : v.as<float>();
}

bool water_fetch(WaterTemps& out) {
    WaterTemps w;
    String body;
    if (get_body("https://tecdottir.metaodi.ch/measurements/tiefenbrunnen"
                 "?sort=timestamp_cet%20desc&limit=1", body))
        w.lake_c = water_parse_lake(body);
    // BAFU station 2243 (Baden, Limmatpromenade)
    if (get_body("https://api.existenz.ch/apiv1/hydro/latest"
                 "?locations=2243&parameters=temperature&app=busli", body))
        w.river_c = water_parse_river(body);

    bool ok = !isnan(w.lake_c) || !isnan(w.river_c);
    if (ok) {
        w.fetched = time(nullptr);
        out = w;
    }
    Log.printf("[Today] water: lake %.1f, Limmat %.1f\n", w.lake_c, w.river_c);
    return ok;
}

bool quote_parse(const String& body, DailyQuote& out) {
    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    JsonObject q = doc[0];
    if (q.isNull() || q["q"].isNull()) return false;
    out.text   = String(q["q"] | "");
    out.author = String(q["a"] | "");
    out.date   = String(q["date"] | "");
    out.valid  = out.text.length() > 0;
    return out.valid;
}

bool quote_fetch(DailyQuote& out) {
    String body;
    if (!get_body("https://zenquotes.io/api/today", body) || !quote_parse(body, out)) return false;
    Log.printf("[Today] quote (%d chars) by %s\n", (int)out.text.length(), out.author.c_str());
    return true;
}
