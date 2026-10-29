// ─────────────────────────────────────────────────────────────────────────────
// flight_tracker.cpp — OpenSky Network live flight lookup
// ─────────────────────────────────────────────────────────────────────────────
// Per refresh (only inside the search window around the flight):
//  1. states/all          → live position/altitude/speed + ICAO24 hardware address
//                           (full ~1 MB dump until the ICAO24 is known, then filtered)
//  2. tracks/all?icao24=  → actual takeoff time, once per flight
//  3. ETA                 → computed locally: distance to arrival airport / ground speed
// Times the user entered in the portal are the fallback when nothing better is known.
// ─────────────────────────────────────────────────────────────────────────────

#include "../include/flight_tracker.h"
#include "../include/devlog.h"
#include "../include/http_lock.h"
#include "../include/airports.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define MAX_FLIGHTS   2
#define HTTP_TIMEOUT  12000   // ms per request

// Live lookups (up to a 1 MB download while the aircraft ID is unknown) only run
// inside this window around the flight; outside it the screen uses cached/entered times.
#define SEARCH_BEFORE_DEP_SEC  (3 * 3600)   // start looking 3 h before departure
#define SEARCH_AFTER_ARR_SEC   (1 * 3600)   // stop 1 h after arrival
#define MAX_FLIGHT_SEC         (20 * 3600)  // assumed arrival when only departure is known

static FlightEntry          s_entries[MAX_FLIGHTS];
static FlightInfo           s_cache[MAX_FLIGHTS];
static String               s_icao24[MAX_FLIGHTS];
static int                  s_count = 0;
static String               s_opensky_user;
static String               s_opensky_pass;
static SemaphoreHandle_t    s_mutex = nullptr;

void flight_tracker_set_opensky_auth(const String& user, const String& pass) {
    s_opensky_user = user;
    s_opensky_pass = pass;
}

// ── IATA airline code → ICAO callsign prefix conversion ──────────────────────
// Allows users to enter standard IATA flight numbers (e.g. TG971, LX161).
// 3-letter prefixes are treated as ICAO already and passed through unchanged.

static const struct { const char* iata; const char* icao; } AIRLINE_TABLE[] = {
    // Swiss-relevant
    {"LX", "SWR"}, {"WK", "EDW"}, {"2L", "OAW"},
    // European
    {"LH", "DLH"}, {"OS", "AUA"}, {"AF", "AFR"}, {"BA", "BAW"},
    {"KL", "KLM"}, {"SK", "SAS"}, {"AY", "FIN"}, {"IB", "IBE"},
    {"TP", "TAP"}, {"TK", "THY"}, {"LO", "LOT"}, {"AZ", "ITY"},
    {"BT", "BTI"}, {"PS", "AUI"}, {"FR", "RYR"}, {"U2", "EZY"},
    {"W6", "WZZ"}, {"4U", "GWI"},
    // Middle East
    {"EK", "UAE"}, {"QR", "QTR"}, {"EY", "ETD"}, {"FZ", "FDB"},
    {"GF", "GFA"}, {"WY", "OMA"},
    // Africa / Asia
    {"ET", "ETH"}, {"MS", "MSR"}, {"TG", "THA"}, {"SQ", "SIA"},
    {"CX", "CPA"}, {"MH", "MAS"}, {"GA", "GIA"}, {"AI", "AIC"},
    {"NH", "ANA"}, {"JL", "JAL"}, {"KE", "KAL"}, {"OZ", "AAR"},
    {"CI", "CAL"}, {"BR", "EVA"}, {"TZ", "ATC"}, {"UL", "ALK"},
    // Americas
    {"UA", "UAL"}, {"AA", "AAL"}, {"DL", "DAL"}, {"AC", "ACA"},
    {"WN", "SWA"}, {"B6", "JBU"}, {"AS", "ASA"}, {"LA", "LAN"},
    {"CM", "CMP"}, {"AM", "AMX"}, {"AR", "ARG"}, {"G3", "GLO"},
    // Oceania
    {"QF", "QFA"}, {"NZ", "ANZ"}, {"VA", "VOZ"},
};
static const int AIRLINE_TABLE_SIZE = sizeof(AIRLINE_TABLE) / sizeof(AIRLINE_TABLE[0]);

// Convert a user-entered flight number to the ICAO callsign used by the aircraft.
// "TG971" → "THA971", "LX161" → "SWR161", "THA971" → "THA971" (pass-through).
static String to_icao_callsign(const String& flight_num) {
    String fn = flight_num;
    fn.toUpperCase();
    fn.trim();
    if (fn.length() < 3) return fn;

    // Count leading alpha characters
    int alpha_len = 0;
    while (alpha_len < (int)fn.length() && isalpha((unsigned char)fn[alpha_len]))
        alpha_len++;

    if (alpha_len == 3) return fn;  // already 3-letter ICAO prefix

    if (alpha_len == 2) {
        String prefix = fn.substring(0, 2);
        String number = fn.substring(2);
        for (int i = 0; i < AIRLINE_TABLE_SIZE; i++) {
            if (prefix == AIRLINE_TABLE[i].iata)
                return String(AIRLINE_TABLE[i].icao) + number;
        }
    }
    return fn;  // no match — pass through as-is
}

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

// ── HTTP body reader — PSRAM buffer, proper chunked decode ────────────────────
// Allocates buffer in PSRAM (avoids String realloc corruption).
// Handles both Content-Length and chunked transfer encoding.
// stop_after: optional text — reading stops once it (and the ']' closing its
// entry) has arrived. Returns bytes written; *out_buf must be free()d by caller
// (nullptr on failure).

static size_t http_read_psram(HTTPClient& http, char** out_buf, size_t max_bytes,
                              const char* stop_after = nullptr) {
    *out_buf = nullptr;
    char* buf = (char*)heap_caps_malloc(max_bytes + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { Log.println("[Flight] PSRAM alloc failed"); return 0; }

    WiFiClient*  s          = http.getStreamPtr();
    int          clen       = http.getSize();
    size_t       total      = 0;

    // The global state dump is 1–2 MB, which takes longer than a fixed per-request
    // timeout at 80 MHz. Keep reading while data flows; give up after READ_STALL_MS
    // without data, or READ_CAP_MS overall (http_lock is held, so transit waits).
    const uint32_t READ_STALL_MS = 8000;
    const uint32_t READ_CAP_MS   = 45000;
    uint32_t start_ms = millis(), last_data_ms = millis();
    auto timed_out = [&]() {
        uint32_t now = millis();
        return now - last_data_ms > READ_STALL_MS || now - start_ms > READ_CAP_MS;
    };
    bool complete = false;
    bool stopped  = false;   // stop_after found — the rest isn't needed

    // Early stop: once stop_after has arrived together with the ']' that closes
    // its state entry, stop reading (on average halves a 1 MB+ download).
    // Only the newly appended bytes (plus an overlap) are searched each time.
    auto found_stop = [&](size_t prev_total) -> bool {
        if (!stop_after || total == prev_total) return false;
        size_t overlap = strlen(stop_after) + 256;
        size_t from = (prev_total > overlap) ? prev_total - overlap : 0;
        buf[total] = '\0';
        const char* hit = strstr(buf + from, stop_after);
        return hit && strchr(hit, ']');
    };

    if (clen >= 0) {
        // Known content-length — read directly
        size_t remaining = min((size_t)clen, max_bytes);
        while (remaining > 0 && !timed_out()) {
            int av = s->available();
            if (av > 0) {
                size_t before = total;
                size_t got = s->readBytes(buf + total, min((size_t)av, remaining));
                total += got; remaining -= got;
                if (got > 0) last_data_ms = millis();
                if (found_stop(before)) { stopped = true; break; }
            } else if (!s->connected()) {
                break;
            } else delay(1);
        }
        complete = (total == (size_t)clen);
    } else {
        // Chunked transfer encoding — decode chunk-size headers manually.
        // getStreamPtr() gives the raw TCP stream; chunk-size lines (e.g. "e68\r\n")
        // would corrupt JSON parsing if not stripped.
        while (!timed_out() && !stopped) {
            // Read chunk-size line (hex digits + CRLF)
            char sz[16] = {}; int sl = 0;
            while (!timed_out()) {
                if (!s->available()) {
                    if (!s->connected()) break;
                    delay(1); continue;
                }
                char c = (char)s->read();
                last_data_ms = millis();
                if (c == '\n') break;
                if (c != '\r' && sl < 15) sz[sl++] = c;
            }
            if (sl == 0) {
                if (!s->connected() && !s->available()) break;
                continue;
            }
            size_t chunk_sz = strtoul(sz, nullptr, 16);
            if (chunk_sz == 0) { complete = true; break; }  // final empty chunk

            // Read chunk data (drain overflow silently so framing stays intact)
            size_t rem = chunk_sz;
            while (rem > 0 && !timed_out()) {
                int av = s->available();
                if (av <= 0) {
                    if (!s->connected()) break;
                    delay(1); continue;
                }
                size_t n = min((size_t)av, rem);
                if (total + n < max_bytes) {
                    size_t before = total;
                    size_t got = s->readBytes(buf + total, n);
                    total += got; rem -= got;
                    if (found_stop(before)) { stopped = true; break; }
                } else {
                    uint8_t drain[128];
                    rem -= s->readBytes(drain, min(n, sizeof(drain)));
                }
                last_data_ms = millis();
            }
            if (stopped) break;
            // Skip trailing CRLF after chunk body
            while (!timed_out()) {
                if (!s->available()) {
                    if (!s->connected()) break;
                    delay(1); continue;
                }
                if ((char)s->read() == '\n') break;
            }
        }
    }

    Log.printf("[Flight] read %u bytes in %lu ms (%s, content-length %d)\n",
                  (unsigned)total, (unsigned long)(millis() - start_ms),
                  stopped ? "found early" : complete ? "complete" : "TRUNCATED", clen);
    buf[total] = '\0';
    *out_buf   = buf;
    return total;
}

// ── Parse a single state array entry from the PSRAM buffer ───────────────────
// Extracts fields into fi; icao24_out is set from st[0].

static bool parse_state_entry(JsonArray st, FlightInfo& fi, String& icao24_out) {
    if (!st || st.size() < 11) return false;
    const char* raw = st[0] | "";
    icao24_out = raw; icao24_out.trim();
    // State vector: [5]=lon [6]=lat [7]=baro alt m [8]=on_ground [9]=speed m/s
    //               [10]=true track deg [11]=vertical rate m/s
    fi.on_ground  = st[8].as<bool>();
    fi.airborne   = !fi.on_ground;
    if (!st[7].isNull()) fi.alt_ft    = st[7].as<float>() * 3.28084f;
    if (!st[9].isNull()) fi.speed_kmh = st[9].as<float>() * 3.6f;
    if (!st[10].isNull()) fi.heading  = st[10].as<float>();
    if (st.size() > 11 && !st[11].isNull()) fi.vert_rate = st[11].as<float>();
    if (!st[5].isNull() && !st[6].isNull()) {
        fi.lon = st[5].as<float>();
        fi.lat = st[6].as<float>();
        fi.has_position = true;
    }
    fi.fetched_at = time(nullptr);
    return true;
}

// ── OpenSky states API ─────────────────────────────────────────────────────────
// Two paths:
//  • icao24_out non-empty → ?icao24=<hex>: OpenSky honours this filter, tiny response.
//  • icao24_out empty     → full global dump (~1-5 MB); scan PSRAM buffer for callsign,
//                           extract ICAO24 and state data, cache for future calls.

static bool fetch_states(const String& callsign, FlightInfo& fi, String& icao24_out) {
    String cs = callsign;
    cs.toUpperCase(); cs.trim();
    while ((int)cs.length() < 8) cs += ' ';  // pad to 8 chars for substring match

    bool have_icao = icao24_out.length() > 0;
    String url = "https://opensky-network.org/api/states/all?";
    if (have_icao) {
        url += "icao24=" + icao24_out;
    } else {
        // callsign filter is ignored server-side; include it for logging clarity only
        url += "callsign=" + url_encode(cs);
    }
    Log.printf("[Flight] states URL: %s\n", url.c_str());

    WiFiClientSecure wc; wc.setInsecure();
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT * (have_icao ? 1 : 3));  // extra time for large scan
    http_lock_take();
    if (!http.begin(wc, url)) { Log.println("[Flight] http.begin failed"); http_lock_give(); return false; }
    if (s_opensky_user.length() > 0)
        http.setAuthorization(s_opensky_user.c_str(), s_opensky_pass.c_str());

    int code = http.GET();
    Log.printf("[Flight] HTTP %d\n", code);
    if (code != 200) { http.end(); http_lock_give(); return false; }

    // ── Path A: icao24-filtered — small response, parse normally ─────────────
    if (have_icao) {
        String body = http.getString();
        http.end();
        http_lock_give();
        Log.printf("[Flight] icao24 body %u bytes: %s\n",
                      (unsigned)body.length(), body.c_str());

        JsonDocument doc;
        if (deserializeJson(doc, body) != DeserializationError::Ok) {
            Log.println("[Flight] JSON error"); return false;
        }
        JsonVariant sv = doc["states"];
        if (sv.isNull()) {
            Log.println("[Flight] states=null (not airborne)"); return false;
        }
        JsonArray st = sv.as<JsonArray>()[0].as<JsonArray>();
        if (!parse_state_entry(st, fi, icao24_out)) return false;
        Log.printf("[Flight] live: %s alt=%.0fft spd=%.0fkm/h gnd=%d\n",
                      callsign.c_str(), fi.alt_ft, fi.speed_kmh, fi.on_ground);
        return true;
    }

    // ── Path B: no icao24 — full global dump, PSRAM buffer, substring scan ───
    Log.println("[Flight] no icao24 cached — scanning full global state dump");
    // Search for the quoted padded callsign: ["icao24hex","THA970  ","country",...
    // The download stops as soon as that entry has arrived.
    char needle[12];
    snprintf(needle, sizeof(needle), "\"%s\"", cs.c_str());  // e.g. "\"THA970  \""
    char* buf = nullptr;
    size_t total = http_read_psram(http, &buf, 3UL * 1024 * 1024, needle);  // 3 MB cap
    http.end();
    http_lock_give();
    if (!buf) return false;

    Log.printf("[Flight] global body %u bytes\n", (unsigned)total);

    bool result = false;
    char* hit = strstr(buf, needle);
    if (!hit) {
        // Also try without trailing spaces (some entries may be stored trimmed)
        String cs_trim = cs; cs_trim.trim();
        snprintf(needle, sizeof(needle), "\"%s\"", cs_trim.c_str());
        hit = strstr(buf, needle);
    }

    if (!hit) {
        Log.printf("[Flight] '%s' not found in %u byte response (not airborne?)\n",
                      cs.c_str(), (unsigned)total);
    } else {
        // Walk back to the [ that opens this state array entry
        char* entry_start = hit - 1;
        while (entry_start > buf && *entry_start != '[') entry_start--;

        // The ICAO24 is between [" and ","callsign"
        char* icao_start = entry_start + 2;  // skip ["
        char* icao_end   = strchr(icao_start, '"');

        if (icao_end && (icao_end - icao_start) <= 8) {
            String found_icao(icao_start, icao_end - icao_start);
            found_icao.trim();
            Log.printf("[Flight] found '%s' → icao24=%s\n", cs.c_str(), found_icao.c_str());

            // Find end of this state entry and parse just that slice
            char* entry_end = strchr(hit, ']');
            if (entry_end) {
                size_t entry_len = (entry_end + 1) - entry_start;
                // Build: {"states":[...entry...]}
                size_t mini_cap = entry_len + 16;
                char* mini = (char*)heap_caps_malloc(mini_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (mini) {
                    int mlen = snprintf(mini, mini_cap, "{\"states\":[%.*s]}", (int)entry_len, entry_start);
                    JsonDocument doc;
                    if (deserializeJson(doc, mini, mlen) == DeserializationError::Ok) {
                        JsonArray st = doc["states"].as<JsonArray>()[0].as<JsonArray>();
                        if (parse_state_entry(st, fi, icao24_out)) {
                            icao24_out = found_icao;  // override with directly extracted value
                            result = true;
                            Log.printf("[Flight] live: %s alt=%.0fft spd=%.0fkm/h gnd=%d\n",
                                          callsign.c_str(), fi.alt_ft, fi.speed_kmh, fi.on_ground);
                        }
                    } else {
                        Log.println("[Flight] mini-JSON parse error");
                    }
                    free(mini);
                }
            }
        }
    }

    free(buf);
    return result;
}

// ── OpenSky tracks API — actual takeoff time ─────────────────────────────────
// /tracks/all?icao24=…&time=0 returns the aircraft's current track (free accounts
// allowed; /flights/aircraft is research-only and always answers 403):
//   {"startTime":…, "path":[[time, lat, lon, baro_alt, track, on_ground], …]}
// The takeoff time is the first waypoint with on_ground=false (falls back to
// startTime). Verified 2026-10-29 on GFA002: 09:36:59 UTC vs Flightradar24 09:37.
// The path can be long, so it's scanned as text instead of parsed into a JsonDocument.

static bool fetch_takeoff_time(const String& icao24, time_t& out) {
    String url = "https://opensky-network.org/api/tracks/all?icao24=" + icao24 + "&time=0";
    Log.printf("[Flight] track URL: %s\n", url.c_str());

    WiFiClientSecure wc; wc.setInsecure();
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT);
    http_lock_take();
    if (!http.begin(wc, url)) { http_lock_give(); return false; }
    if (s_opensky_user.length() > 0)
        http.setAuthorization(s_opensky_user.c_str(), s_opensky_pass.c_str());
    int code = http.GET();
    if (code != 200) {
        http.end(); http_lock_give();
        Log.printf("[Flight] track HTTP %d\n", code);
        return false;
    }
    char* buf = nullptr;
    http_read_psram(http, &buf, 512UL * 1024);
    http.end();
    http_lock_give();
    if (!buf) return false;

    time_t takeoff = 0;
    const char* p = strstr(buf, "\"path\":[");
    if (p) {
        p += 8;
        // Each waypoint: [time,lat,lon,alt,track,true|false]
        while ((p = strchr(p, '[')) != nullptr) {
            const char* end = strchr(p, ']');
            if (!end) break;
            long t = strtol(p + 1, nullptr, 10);
            // on_ground is the last field — "false]" means airborne
            if (end - p > 6 && strncmp(end - 5, "false", 5) == 0 && t > 0) {
                takeoff = (time_t)t;
                break;
            }
            p = end + 1;
        }
    }
    if (takeoff == 0) {
        const char* s = strstr(buf, "\"startTime\":");
        if (s) takeoff = (time_t)strtod(s + 12, nullptr);
    }
    free(buf);

    if (takeoff <= 0) { Log.println("[Flight] track: no takeoff time"); return false; }
    out = takeoff;
    struct tm t; localtime_r(&out, &t);
    Log.printf("[Flight] takeoff %02d:%02d local (epoch %ld)\n", t.tm_hour, t.tm_min, (long)out);
    return true;
}

// ── Estimated arrival ────────────────────────────────────────────────────────
// ETA = great-circle distance to the arrival airport / current ground speed,
// plus an allowance for descent and approach (slower than cruise). Only
// meaningful in the air; the arrival airport comes from the user's entry.

#define ETA_APPROACH_SEC  (10 * 60)   // added when far out; shrinks to 0 near the airport
#define ETA_APPROACH_KM   150.0f      // distance over which the allowance shrinks

static const AirportCoord* find_airport(const String& code) {
    String c = code; c.trim(); c.toUpperCase();
    if (c.length() == 3) {
        int lo = 0, hi = AIRPORTS_COUNT - 1;   // table is sorted by IATA
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            int cmp = strcmp(c.c_str(), AIRPORTS[mid].iata);
            if (cmp == 0) return &AIRPORTS[mid];
            if (cmp < 0) hi = mid - 1; else lo = mid + 1;
        }
    } else if (c.length() == 4) {
        for (int i = 0; i < AIRPORTS_COUNT; i++)
            if (strcmp(c.c_str(), AIRPORTS[i].icao) == 0) return &AIRPORTS[i];
    }
    return nullptr;
}

static float distance_km(float lat1, float lon1, float lat2, float lon2) {
    const float R = 6371.0f, D2R = 0.0174532925f;
    float dlat = (lat2 - lat1) * D2R, dlon = (lon2 - lon1) * D2R;
    float a = sinf(dlat / 2) * sinf(dlat / 2) +
              cosf(lat1 * D2R) * cosf(lat2 * D2R) * sinf(dlon / 2) * sinf(dlon / 2);
    return 2.0f * R * asinf(sqrtf(a));
}

static void update_eta(FlightInfo& fi) {
    if (!fi.airborne || !fi.has_position || fi.speed_kmh < 150.0f) return;
    const AirportCoord* dest = find_airport(fi.arr_icao);
    if (!dest) {
        Log.printf("[Flight] ETA: arrival airport '%s' unknown\n", fi.arr_icao.c_str());
        return;
    }
    float km  = distance_km(fi.lat, fi.lon, dest->lat, dest->lon);
    float app = (km >= ETA_APPROACH_KM) ? ETA_APPROACH_SEC : ETA_APPROACH_SEC * km / ETA_APPROACH_KM;
    fi.arr_time      = time(nullptr) + (time_t)(km / fi.speed_kmh * 3600.0f + app);
    fi.arr_estimated = true;
    struct tm t; localtime_r(&fi.arr_time, &t);
    Log.printf("[Flight] ETA %s: %.0f km at %.0f km/h → %02d:%02d local\n",
                  dest->iata, km, fi.speed_kmh, t.tm_hour, t.tm_min);
}

// ── Internal refresh ──────────────────────────────────────────────────────────

// ── Aircraft ID cache ────────────────────────────────────────────────────────
// Finding the ICAO24 needs the 1 MB+ worldwide dump; once known it's kept in NVS
// per flight + date, so a reboot (or OTA) during the flight doesn't repeat it.
static String icao_cache_key(const FlightEntry& e) {
    uint32_t h = 2166136261u;
    for (char c : e.callsign + "|" + e.dep_date) h = (h ^ (uint8_t)c) * 16777619u;
    char key[12];
    snprintf(key, sizeof(key), "i%08lx", (unsigned long)h);
    return key;
}

static String icao_cache_get(const FlightEntry& e) {
    Preferences prefs;
    prefs.begin("flight", true);
    String key = icao_cache_key(e);
    String v   = prefs.isKey(key.c_str()) ? prefs.getString(key.c_str(), "") : "";
    prefs.end();
    return v;
}

static void icao_cache_put(const FlightEntry& e, const String& icao24) {
    Preferences prefs;
    prefs.begin("flight", false);
    prefs.putString(icao_cache_key(e).c_str(), icao24);
    prefs.end();
}

// "YYYY-MM-DD" + "HH:MM" in Swiss local time → epoch (0 if either is malformed).
static time_t make_local_epoch(const String& date, const String& hhmm) {
    if (date.length() < 10 || hhmm.length() < 5 || hhmm[2] != ':') return 0;
    struct tm t = {};
    t.tm_year  = date.substring(0, 4).toInt() - 1900;
    t.tm_mon   = date.substring(5, 7).toInt() - 1;
    t.tm_mday  = date.substring(8, 10).toInt();
    t.tm_hour  = hhmm.substring(0, 2).toInt();
    t.tm_min   = hhmm.substring(3, 5).toInt();
    t.tm_isdst = -1;
    return mktime(&t);
}

// True if it's worth asking OpenSky about this flight right now: from
// SEARCH_BEFORE_DEP_SEC before departure until SEARCH_AFTER_ARR_SEC after arrival.
// Best known times win: API timestamps, then entered times, then the whole
// departure day (+ next day for overnight flights). No date → always search.
static bool in_search_window(int slot, time_t now) {
    if (now < 1700000000) return true;  // clock not synced — can't judge
    const FlightEntry& e = s_entries[slot];
    time_t day_start = make_local_epoch(e.dep_date, "00:00");
    if (day_start == 0) return true;

    time_t dep = s_cache[slot].dep_time;
    if (dep == 0) dep = make_local_epoch(e.dep_date, e.dep_time_str);
    time_t arr = s_cache[slot].arr_time;
    if (arr == 0) {
        arr = make_local_epoch(e.dep_date, e.arr_time_str);
        if (arr > 0 && dep > 0 && arr < dep) arr += 86400;  // overnight flight
    }

    time_t start = (dep > 0 ? dep : day_start) - SEARCH_BEFORE_DEP_SEC;
    time_t end   = (arr > 0) ? arr + SEARCH_AFTER_ARR_SEC
                 : (dep > 0) ? dep + MAX_FLIGHT_SEC
                 :             day_start + 2 * 86400;
    return now >= start && now <= end;
}

static void do_refresh(int slot) {
    if (slot < 0 || slot >= s_count) return;

    FlightInfo fi;
    fi.callsign = s_entries[slot].callsign;   // keep user-entered name for display
    fi.dep_date = s_entries[slot].dep_date;
    fi.valid    = true;

    // User-entered airports take precedence; fall back to previously cached values
    fi.dep_icao      = s_entries[slot].dep_icao.length() > 0
                       ? s_entries[slot].dep_icao : s_cache[slot].dep_icao;
    fi.arr_icao      = s_entries[slot].arr_icao.length() > 0
                       ? s_entries[slot].arr_icao : s_cache[slot].arr_icao;
    // Times learned on earlier refreshes (takeoff, latest ETA) carry over
    fi.dep_time      = s_cache[slot].dep_time;
    fi.dep_actual    = s_cache[slot].dep_actual;
    fi.arr_time      = s_cache[slot].arr_time;
    fi.arr_estimated = s_cache[slot].arr_estimated;

    if (in_search_window(slot, time(nullptr))) {
        // Step 1: live state — use ICAO callsign for the API, keep display name in fi.callsign
        String api_cs = to_icao_callsign(s_entries[slot].callsign);
        if (s_icao24[slot].isEmpty()) s_icao24[slot] = icao_cache_get(s_entries[slot]);
        String icao24 = s_icao24[slot];
        bool live = fetch_states(api_cs, fi, icao24);
        if (live && icao24.length() > 0 && icao24 != s_icao24[slot]) {
            s_icao24[slot] = icao24;
            icao_cache_put(s_entries[slot], icao24);
        }

        // Step 2: actual takeoff time — once per flight, as soon as it's airborne
        if (live && fi.airborne && !fi.dep_actual && s_icao24[slot].length() > 0) {
            time_t takeoff;
            if (fetch_takeoff_time(s_icao24[slot], takeoff)) {
                fi.dep_time   = takeoff;
                fi.dep_actual = true;
            }
        }

        // Step 3: estimated arrival from live position (every refresh while flying)
        if (live) update_eta(fi);

        // Back on the ground after a tracked takeoff → landed now if earlier than the ETA
        time_t now = time(nullptr);
        if (live && fi.on_ground && fi.dep_actual && fi.arr_estimated && fi.arr_time > now)
            fi.arr_time = now;
    } else {
        Log.printf("[Flight] %s: outside search window — skipping live lookup\n",
                      s_entries[slot].callsign.c_str());
    }

    // Step 4: fall back to user-entered times when nothing better is known.
    if (fi.dep_time == 0 && s_entries[slot].dep_time_str.length() >= 5)
        fi.dep_time = make_local_epoch(s_entries[slot].dep_date, s_entries[slot].dep_time_str);
    if (fi.arr_time == 0 && s_entries[slot].arr_time_str.length() >= 5) {
        time_t at = make_local_epoch(s_entries[slot].dep_date, s_entries[slot].arr_time_str);
        if (at > 0 && fi.dep_time > 0 && at < fi.dep_time) at += 86400;  // overnight flight
        fi.arr_time = at;
    }

    fi.fetched_at = time(nullptr);
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_cache[slot] = fi;
    if (s_mutex) xSemaphoreGive(s_mutex);
}

// ── Public API ────────────────────────────────────────────────────────────────

void flight_tracker_init(const FlightEntry* entries, int count) {
    s_mutex = xSemaphoreCreateMutex();
    s_count = (count < MAX_FLIGHTS) ? count : MAX_FLIGHTS;
    for (int i = 0; i < s_count; i++) {
        s_entries[i] = entries[i];
        s_cache[i]   = FlightInfo{};
        s_cache[i].callsign = entries[i].callsign;
        s_cache[i].dep_date = entries[i].dep_date;
        s_icao24[i]  = "";
    }
    // No blocking HTTP fetch here — background task calls flight_tracker_refresh() on core 0
}

bool flight_tracker_refresh(int slot) {
    if (slot < 0 || slot >= s_count) return false;
    do_refresh(slot);
    return s_cache[slot].valid;
}

void flight_tracker_get(int slot, FlightInfo& out) {
    if (slot < 0 || slot >= s_count) { out = FlightInfo{}; return; }
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    out = s_cache[slot];
    if (s_mutex) xSemaphoreGive(s_mutex);
}

int flight_tracker_count() { return s_count; }
