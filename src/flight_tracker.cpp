// ─────────────────────────────────────────────────────────────────────────────
// flight_tracker.cpp — OpenSky Network live flight lookup
// ─────────────────────────────────────────────────────────────────────────────
// Two-step lookup per slot:
//  1. states/all?callsign=SWR161   → live position + ICAO24 hardware address
//  2. flights/aircraft?icao24=...  → departure/arrival airports + timestamps
// Step 2 is only attempted once per slot (result cached in s_icao24[]).
// ─────────────────────────────────────────────────────────────────────────────

#include "../include/flight_tracker.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#define MAX_FLIGHTS   2
#define HTTP_TIMEOUT  12000   // ms per request

static FlightEntry  s_entries[MAX_FLIGHTS];
static FlightInfo   s_cache[MAX_FLIGHTS];
static String       s_icao24[MAX_FLIGHTS];  // cached per slot; avoids re-querying
static int          s_count = 0;

// ── Helpers ───────────────────────────────────────────────────────────────────

static String url_encode(const String& s) {
    String out;
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.') {
            out += c;
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", (uint8_t)c);
            out += hex;
        }
    }
    return out;
}

// Compute UTC midnight epoch for "YYYY-MM-DD" without touching TZ globals.
static time_t date_utc_epoch(const String& d) {
    if (d.length() < 10) return 0;
    int y   = d.substring(0, 4).toInt();
    int m   = d.substring(5, 7).toInt();
    int day = d.substring(8, 10).toInt();
    if (y < 2020 || m < 1 || m > 12 || day < 1 || day > 31) return 0;

    static const int MDAYS[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    long days = 0;
    for (int yr = 1970; yr < y; yr++) {
        days += (yr % 4 == 0 && (yr % 100 != 0 || yr % 400 == 0)) ? 366 : 365;
    }
    bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    for (int mo = 1; mo < m; mo++) {
        days += MDAYS[mo - 1];
        if (mo == 2 && leap) days++;
    }
    days += day - 1;
    return (time_t)(days * 86400L);
}

// ── OpenSky states API ─────────────────────────────────────────────────────────
// Returns true if the aircraft was found (airborne or on ground with a state vector).
// Populates fi (position/speed/heading) and icao24_out.

static bool fetch_states(const String& callsign, FlightInfo& fi, String& icao24_out) {
    // Pad callsign to 8 chars (OpenSky transmits padded ICAO callsigns)
    String cs = callsign;
    cs.toUpperCase();
    while ((int)cs.length() < 8) cs += ' ';

    String url = "https://opensky-network.org/api/states/all?callsign=";
    url += url_encode(cs);

    WiFiClientSecure wc;
    wc.setInsecure();
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT);
    if (!http.begin(wc, url)) return false;

    int code = http.GET();
    if (code != 200) {
        Serial.printf("[Flight] states HTTP %d\n", code);
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) return false;

    JsonArray states = doc["states"].as<JsonArray>();
    if (!states || states.size() == 0) {
        Serial.printf("[Flight] callsign '%s' not found in live states\n", callsign.c_str());
        return false;
    }

    JsonArray st = states[0].as<JsonArray>();
    if (!st || st.size() < 11) return false;

    const char* raw_icao = st[0] | "";
    icao24_out = raw_icao;
    icao24_out.trim();

    fi.on_ground  = st[8].as<bool>();
    fi.airborne   = !fi.on_ground;
    if (!st[7].isNull()) fi.alt_ft    = st[7].as<float>() * 3.28084f;
    if (!st[9].isNull()) fi.speed_kmh = st[9].as<float>() * 3.6f;
    if (!st[10].isNull()) fi.heading  = st[10].as<float>();
    fi.fetched_at = time(nullptr);

    Serial.printf("[Flight] live: %s icao24=%s alt=%.0fft spd=%.0fkm/h gnd=%d\n",
                  callsign.c_str(), icao24_out.c_str(), fi.alt_ft, fi.speed_kmh, fi.on_ground);
    return true;
}

// ── OpenSky flights API ───────────────────────────────────────────────────────
// Fetches departure/arrival airports and timestamps for a given aircraft on a given date.

static bool fetch_route(const String& icao24, const String& dep_date, FlightInfo& fi) {
    time_t begin_t = date_utc_epoch(dep_date);
    if (begin_t == 0) return false;
    time_t end_t = begin_t + 2 * 86400L;  // search +2 days to catch delayed/long flights

    String url = "https://opensky-network.org/api/flights/aircraft?icao24=";
    url += icao24;
    url += "&begin=";
    url += String((long)begin_t);
    url += "&end=";
    url += String((long)end_t);

    WiFiClientSecure wc;
    wc.setInsecure();
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT);
    if (!http.begin(wc, url)) return false;

    int code = http.GET();
    if (code == 404) { http.end(); return false; }
    if (code != 200) {
        Serial.printf("[Flight] route HTTP %d\n", code);
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();

    if (body.isEmpty() || body == "null" || body == "[]") return false;

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) return false;
    if (!doc.is<JsonArray>()) return false;

    JsonArray arr = doc.as<JsonArray>();
    if (arr.size() == 0) return false;

    // Find best matching entry by callsign; fall back to first entry
    JsonObject best = arr[0].as<JsonObject>();
    String target = fi.callsign;
    target.toUpperCase();
    for (JsonObject fl : arr) {
        String cs = fl["callsign"] | "";
        cs.trim();
        cs.toUpperCase();
        if (cs == target) { best = fl; break; }
    }

    const char* dep = best["estDepartureAirport"] | "";
    const char* arr2 = best["estArrivalAirport"]  | "";
    if (dep  && strlen(dep)  > 0) fi.dep_icao = dep;
    if (arr2 && strlen(arr2) > 0) fi.arr_icao = arr2;
    fi.dep_time = best["firstSeen"] | 0L;
    fi.arr_time = best["lastSeen"]  | 0L;

    Serial.printf("[Flight] route: %s → %s dep=%ld arr=%ld\n",
                  fi.dep_icao.c_str(), fi.arr_icao.c_str(),
                  (long)fi.dep_time, (long)fi.arr_time);
    return (fi.dep_icao.length() > 0 || fi.arr_icao.length() > 0);
}

// ── Internal refresh ──────────────────────────────────────────────────────────

static void do_refresh(int slot) {
    if (slot < 0 || slot >= s_count) return;

    FlightInfo fi;
    fi.callsign = s_entries[slot].callsign;
    fi.valid    = true;

    // Preserve known route info from previous cache
    fi.dep_icao  = s_cache[slot].dep_icao;
    fi.arr_icao  = s_cache[slot].arr_icao;
    fi.dep_time  = s_cache[slot].dep_time;
    fi.arr_time  = s_cache[slot].arr_time;

    // Step 1: live state (gives position + icao24)
    String icao24 = s_icao24[slot];
    bool live = fetch_states(fi.callsign, fi, icao24);
    if (live && icao24.length() > 0)
        s_icao24[slot] = icao24;

    // Step 2: route info — fetch if we have icao24 but no route yet
    if (s_icao24[slot].length() > 0 &&
        fi.dep_icao.isEmpty() && fi.arr_icao.isEmpty())
    {
        fetch_route(s_icao24[slot], s_entries[slot].dep_date, fi);
    }

    fi.fetched_at = time(nullptr);
    s_cache[slot] = fi;
}

// ── Public API ────────────────────────────────────────────────────────────────

void flight_tracker_init(const FlightEntry* entries, int count) {
    s_count = (count < MAX_FLIGHTS) ? count : MAX_FLIGHTS;
    for (int i = 0; i < s_count; i++) {
        s_entries[i] = entries[i];
        s_cache[i]   = FlightInfo{};
        s_cache[i].callsign = entries[i].callsign;
        s_icao24[i]  = "";
    }
    // Initial blocking fetch so the first flight view has data
    for (int i = 0; i < s_count; i++) do_refresh(i);
}

bool flight_tracker_refresh(int slot) {
    if (slot < 0 || slot >= s_count) return false;
    do_refresh(slot);
    return s_cache[slot].valid;
}

void flight_tracker_get(int slot, FlightInfo& out) {
    if (slot < 0 || slot >= s_count) { out = FlightInfo{}; return; }
    out = s_cache[slot];
}

int flight_tracker_count() { return s_count; }
