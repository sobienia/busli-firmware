// ─────────────────────────────────────────────────────────────────────────────
// geo.cpp — Station coordinates + town name, cached in NVS
// ─────────────────────────────────────────────────────────────────────────────

#include "../include/geo.h"
#include "../include/http_lock.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>

static const char* NVS_NS     = "geo";
static const char* USER_AGENT = "Busli/1.6 (github.com/sobienia/busli-firmware)";  // Nominatim policy
static const int   TIMEOUT_MS = 8000;

static String url_encode(const String& s) {
    String out;
    for (int i = 0; i < (int)s.length(); i++) {
        char c = s[i];
        if (isalnum((uint8_t)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", (uint8_t)c);
            out += hex;
        }
    }
    return out;
}

// NVS keys are limited to 15 chars → key by a hash of the name.
// prefix: 's' = transit station, 'c' = town typed by the user
static String cache_key(const String& name, char prefix = 's') {
    uint32_t h = 2166136261u;
    for (char c : name) h = (h ^ (uint8_t)c) * 16777619u;
    char key[12];
    snprintf(key, sizeof(key), "%c%08lx", prefix, (unsigned long)h);
    return key;
}

static bool cache_get(const String& station, GeoPlace& out, char prefix = 's') {
    Preferences prefs;
    prefs.begin(NVS_NS, true);
    String json = prefs.getString(cache_key(station, prefix).c_str(), "");
    prefs.end();
    if (json.isEmpty()) return false;
    JsonDocument doc;
    if (deserializeJson(doc, json)) return false;
    out.lat   = doc["a"] | 0.0f;
    out.lon   = doc["o"] | 0.0f;
    out.city  = String(doc["c"] | "");
    out.valid = true;
    return true;
}

static void cache_put(const String& station, const GeoPlace& p, char prefix = 's') {
    JsonDocument doc;
    doc["a"] = p.lat;
    doc["o"] = p.lon;
    doc["c"] = p.city;
    String json;
    serializeJson(doc, json);
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.putString(cache_key(station, prefix).c_str(), json);
    prefs.end();
}

static bool fetch_coordinates(const String& station, GeoPlace& out) {
    String url = "http://transport.opendata.ch/v1/locations?type=station&query=" + url_encode(station)
               + "&fields[]=stations/coordinate";
    HTTPClient http;
    http.setTimeout(TIMEOUT_MS);
    http_lock_take();
    bool ok = http.begin(url);
    int code = ok ? http.GET() : -1;
    String body = (code == 200) ? http.getString() : "";
    http.end();
    http_lock_give();
    if (code != 200) { Serial.printf("[Geo] locations HTTP %d\n", code); return false; }

    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    JsonObject c = doc["stations"][0]["coordinate"];
    if (c.isNull() || c["x"].isNull()) return false;
    out.lat = c["x"];   // this API uses x = latitude, y = longitude
    out.lon = c["y"];
    return out.lat != 0 || out.lon != 0;
}

static bool fetch_town(float lat, float lon, String& out_city) {
    char url[200];
    snprintf(url, sizeof(url),
             "https://nominatim.openstreetmap.org/reverse?format=jsonv2&zoom=10"
             "&addressdetails=1&lat=%.5f&lon=%.5f", lat, lon);
    WiFiClientSecure wc;
    wc.setInsecure();
    HTTPClient http;
    http.setTimeout(TIMEOUT_MS);
    http_lock_take();
    bool ok = http.begin(wc, url);
    if (ok) http.addHeader("User-Agent", USER_AGENT);
    int code = ok ? http.GET() : -1;
    String body = (code == 200) ? http.getString() : "";
    http.end();
    http_lock_give();
    if (code != 200) { Serial.printf("[Geo] nominatim HTTP %d\n", code); return false; }

    JsonDocument filter;
    filter["address"]["city"] = true;
    filter["address"]["town"] = true;
    filter["address"]["village"] = true;
    filter["address"]["municipality"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return false;
    JsonObject a = doc["address"];
    for (const char* k : { "city", "town", "village", "municipality" }) {
        const char* v = a[k] | (const char*)nullptr;
        if (v && *v) { out_city = v; return true; }
    }
    return false;
}

bool geo_resolve_city(const String& name, GeoPlace& out) {
    String q = name; q.trim();
    if (q.isEmpty()) return false;
    if (cache_get(q, out, 'c')) return true;

    String url = "http://geocoding-api.open-meteo.com/v1/search?count=1&language=de&name=" + url_encode(q);
    HTTPClient http;
    http.setTimeout(TIMEOUT_MS);
    http_lock_take();
    bool ok = http.begin(url);
    int code = ok ? http.GET() : -1;
    String body = (code == 200) ? http.getString() : "";
    http.end();
    http_lock_give();
    if (code != 200) { Serial.printf("[Geo] geocoding HTTP %d\n", code); return false; }

    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    JsonObject r = doc["results"][0];
    if (r.isNull()) { Serial.printf("[Geo] town '%s' not found\n", q.c_str()); return false; }
    GeoPlace p;
    p.lat   = r["latitude"]  | 0.0f;
    p.lon   = r["longitude"] | 0.0f;
    p.city  = q;               // show it the way the user wrote it
    p.valid = true;
    cache_put(q, p, 'c');
    Serial.printf("[Geo] town %s → %.4f,%.4f\n", q.c_str(), p.lat, p.lon);
    out = p;
    return true;
}

bool geo_resolve_here(GeoPlace& out) {
    HTTPClient http;
    http.setTimeout(TIMEOUT_MS);
    http_lock_take();
    bool ok = http.begin("http://ip-api.com/json/?fields=status,city,lat,lon");
    int code = ok ? http.GET() : -1;
    String body = (code == 200) ? http.getString() : "";
    http.end();
    http_lock_give();
    if (code != 200) { Serial.printf("[Geo] ip-api HTTP %d\n", code); return false; }

    JsonDocument doc;
    if (deserializeJson(doc, body) || String(doc["status"] | "") != "success") return false;
    out.lat   = doc["lat"] | 0.0f;
    out.lon   = doc["lon"] | 0.0f;
    out.valid = true;
    // Name the town the same way as the stops (OpenStreetMap, local spelling:
    // "Zürich", not ip-api's English "Zurich"); ip-api's name is the fallback
    if (!fetch_town(out.lat, out.lon, out.city)) out.city = String(doc["city"] | "");
    Serial.printf("[Geo] here → %.4f,%.4f '%s'\n", out.lat, out.lon, out.city.c_str());
    return true;
}

bool geo_resolve_station(const String& station, GeoPlace& out) {
    if (station.isEmpty()) return false;
    if (cache_get(station, out)) return true;

    GeoPlace p;
    if (!fetch_coordinates(station, p)) return false;
    p.valid = true;

    bool town_ok = fetch_town(p.lat, p.lon, p.city);
    if (!town_ok) {
        int comma = station.indexOf(", ");
        p.city = (comma > 0) ? station.substring(0, comma) : "";
    }
    if (town_ok) cache_put(station, p);   // fallback name isn't cached → retried next boot

    Serial.printf("[Geo] %s → %.4f,%.4f '%s'%s\n", station.c_str(), p.lat, p.lon,
                  p.city.c_str(), town_ok ? "" : " (fallback name)");
    out = p;
    return true;
}
