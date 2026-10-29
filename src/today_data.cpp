// ─────────────────────────────────────────────────────────────────────────────
// today_data.cpp — Water temperatures (Lake Zürich, Limmat) + quote of the day
// ─────────────────────────────────────────────────────────────────────────────

#include "../include/today_data.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

static const int TIMEOUT_MS = 8000;

// GET a small JSON document over HTTPS (public data — certificate not checked)
static bool get_json(const char* url, JsonDocument& doc, const JsonDocument* filter = nullptr) {
    WiFiClientSecure wc;
    wc.setInsecure();
    HTTPClient http;
    http.setTimeout(TIMEOUT_MS);
    http.setReuse(false);
    if (!http.begin(wc, url)) return false;
    http.addHeader("User-Agent", "Busli/1.7");
    int code = http.GET();
    String body = (code == 200) ? http.getString() : "";
    http.end();
    if (code != 200) { Serial.printf("[Today] %s → HTTP %d\n", url, code); return false; }
    DeserializationError err = filter
        ? deserializeJson(doc, body, DeserializationOption::Filter(*filter))
        : deserializeJson(doc, body);
    return !err;
}

bool water_fetch(WaterTemps& out) {
    WaterTemps w;

    // Lake Zürich — latest reading of the Tiefenbrunnen station
    {
        JsonDocument filter;
        filter["result"][0]["values"]["water_temperature"]["value"] = true;
        JsonDocument doc;
        if (get_json("https://tecdottir.metaodi.ch/measurements/tiefenbrunnen"
                     "?sort=timestamp_cet%20desc&limit=1", doc, &filter)) {
            JsonVariant v = doc["result"][0]["values"]["water_temperature"]["value"];
            if (!v.isNull()) w.lake_c = v.as<float>();
        }
    }
    // Limmat — BAFU station 2243 (Baden, Limmatpromenade)
    {
        JsonDocument doc;
        if (get_json("https://api.existenz.ch/apiv1/hydro/latest"
                     "?locations=2243&parameters=temperature&app=busli", doc)) {
            JsonVariant v = doc["payload"][0]["val"];
            if (!v.isNull()) w.river_c = v.as<float>();
        }
    }

    bool ok = !isnan(w.lake_c) || !isnan(w.river_c);
    if (ok) {
        w.fetched = time(nullptr);
        out = w;
    }
    Serial.printf("[Today] water: lake %.1f, Limmat %.1f\n", w.lake_c, w.river_c);
    return ok;
}

bool quote_fetch(DailyQuote& out) {
    JsonDocument doc;
    if (!get_json("https://zenquotes.io/api/today", doc)) return false;
    JsonObject q = doc[0];
    if (q.isNull() || q["q"].isNull()) return false;
    out.text   = String(q["q"] | "");
    out.author = String(q["a"] | "");
    out.date   = String(q["date"] | "");
    out.valid  = out.text.length() > 0;
    Serial.printf("[Today] quote (%d chars) by %s\n", (int)out.text.length(), out.author.c_str());
    return out.valid;
}
