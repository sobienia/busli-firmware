// ─────────────────────────────────────────────────────────────────────────────
// flight_tracker.cpp — OpenSky Network live flight lookup
// ─────────────────────────────────────────────────────────────────────────────
// Two-step lookup per slot:
//  1. states/all?callsign=SWR161   → live position + ICAO24 hardware address
//  2. flights/aircraft?icao24=...  → departure/arrival airports + timestamps
// Step 2 is only attempted once per slot (result cached in s_icao24[]).
// ─────────────────────────────────────────────────────────────────────────────

#include "../include/flight_tracker.h"
#include "../include/http_lock.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#define MAX_FLIGHTS   2
#define HTTP_TIMEOUT  12000   // ms per request

static FlightEntry  s_entries[MAX_FLIGHTS];
static FlightInfo   s_cache[MAX_FLIGHTS];
static String       s_icao24[MAX_FLIGHTS];  // cached per slot; avoids re-querying
static int          s_count = 0;
static String       s_opensky_user;
static String       s_opensky_pass;

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

// ── HTTP body reader — PSRAM buffer, proper chunked decode ────────────────────
// Allocates buffer in PSRAM (avoids String realloc corruption).
// Handles both Content-Length and chunked transfer encoding.
// Returns bytes written; *out_buf must be free()d by caller (nullptr on failure).

static size_t http_read_psram(HTTPClient& http, char** out_buf, size_t max_bytes) {
    *out_buf = nullptr;
    char* buf = (char*)heap_caps_malloc(max_bytes + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { Serial.println("[Flight] PSRAM alloc failed"); return 0; }

    WiFiClient*  s          = http.getStreamPtr();
    int          clen       = http.getSize();
    uint32_t     deadline   = millis() + HTTP_TIMEOUT;
    size_t       total      = 0;

    if (clen >= 0) {
        // Known content-length — read directly
        size_t remaining = min((size_t)clen, max_bytes);
        while (remaining > 0 && millis() < deadline) {
            int av = s->available();
            if (av > 0) {
                size_t got = s->readBytes(buf + total, min((size_t)av, remaining));
                total += got; remaining -= got;
            } else delay(1);
        }
    } else {
        // Chunked transfer encoding — decode chunk-size headers manually.
        // getStreamPtr() gives the raw TCP stream; chunk-size lines (e.g. "e68\r\n")
        // would corrupt JSON parsing if not stripped.
        while (millis() < deadline) {
            // Read chunk-size line (hex digits + CRLF)
            char sz[16] = {}; int sl = 0;
            uint32_t ldl = millis() + 3000;
            while (millis() < ldl) {
                if (!s->available()) { delay(1); continue; }
                char c = (char)s->read();
                if (c == '\n') break;
                if (c != '\r' && sl < 15) sz[sl++] = c;
            }
            if (sl == 0) continue;
            size_t chunk_sz = strtoul(sz, nullptr, 16);
            if (chunk_sz == 0) break;  // final empty chunk

            // Read chunk data (drain overflow silently so framing stays intact)
            size_t rem = chunk_sz;
            while (rem > 0 && millis() < deadline) {
                int av = s->available();
                if (av <= 0) { delay(1); continue; }
                size_t n = min((size_t)av, rem);
                if (total + n < max_bytes) {
                    size_t got = s->readBytes(buf + total, n);
                    total += got; rem -= got;
                } else {
                    uint8_t drain[128];
                    rem -= s->readBytes(drain, min(n, sizeof(drain)));
                }
            }
            // Skip trailing CRLF after chunk body
            uint32_t cdl = millis() + 1000;
            while (millis() < cdl) {
                if (!s->available()) { delay(1); continue; }
                if ((char)s->read() == '\n') break;
            }
        }
    }

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
    fi.on_ground  = st[8].as<bool>();
    fi.airborne   = !fi.on_ground;
    if (!st[7].isNull()) fi.alt_ft    = st[7].as<float>() * 3.28084f;
    if (!st[9].isNull()) fi.speed_kmh = st[9].as<float>() * 3.6f;
    if (!st[10].isNull()) fi.heading  = st[10].as<float>();
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
    Serial.printf("[Flight] states URL: %s\n", url.c_str());

    WiFiClientSecure wc; wc.setInsecure();
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT * (have_icao ? 1 : 3));  // extra time for large scan
    http_lock_take();
    if (!http.begin(wc, url)) { Serial.println("[Flight] http.begin failed"); http_lock_give(); return false; }
    if (s_opensky_user.length() > 0)
        http.setAuthorization(s_opensky_user.c_str(), s_opensky_pass.c_str());

    int code = http.GET();
    Serial.printf("[Flight] HTTP %d\n", code);
    if (code != 200) { http.end(); http_lock_give(); return false; }

    // ── Path A: icao24-filtered — small response, parse normally ─────────────
    if (have_icao) {
        String body = http.getString();
        http.end();
        http_lock_give();
        Serial.printf("[Flight] icao24 body %u bytes: %s\n",
                      (unsigned)body.length(), body.c_str());

        JsonDocument doc;
        if (deserializeJson(doc, body) != DeserializationError::Ok) {
            Serial.println("[Flight] JSON error"); return false;
        }
        JsonVariant sv = doc["states"];
        if (sv.isNull()) {
            Serial.println("[Flight] states=null (not airborne)"); return false;
        }
        JsonArray st = sv.as<JsonArray>()[0].as<JsonArray>();
        if (!parse_state_entry(st, fi, icao24_out)) return false;
        Serial.printf("[Flight] live: %s alt=%.0fft spd=%.0fkm/h gnd=%d\n",
                      callsign.c_str(), fi.alt_ft, fi.speed_kmh, fi.on_ground);
        return true;
    }

    // ── Path B: no icao24 — full global dump, PSRAM buffer, substring scan ───
    Serial.println("[Flight] no icao24 cached — scanning full global state dump");
    char* buf = nullptr;
    size_t total = http_read_psram(http, &buf, 3UL * 1024 * 1024);  // 3 MB cap
    http.end();
    http_lock_give();
    if (!buf) return false;

    Serial.printf("[Flight] global body %u bytes\n", (unsigned)total);

    // Search for the quoted padded callsign: ["icao24hex","THA970  ","country",...
    bool result = false;
    char needle[12];
    snprintf(needle, sizeof(needle), "\"%s\"", cs.c_str());  // e.g. "\"THA970  \""
    char* hit = strstr(buf, needle);
    if (!hit) {
        // Also try without trailing spaces (some entries may be stored trimmed)
        String cs_trim = cs; cs_trim.trim();
        snprintf(needle, sizeof(needle), "\"%s\"", cs_trim.c_str());
        hit = strstr(buf, needle);
    }

    if (!hit) {
        Serial.printf("[Flight] '%s' not found in %u byte response (not airborne?)\n",
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
            Serial.printf("[Flight] found '%s' → icao24=%s\n", cs.c_str(), found_icao.c_str());

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
                            Serial.printf("[Flight] live: %s alt=%.0fft spd=%.0fkm/h gnd=%d\n",
                                          callsign.c_str(), fi.alt_ft, fi.speed_kmh, fi.on_ground);
                        }
                    } else {
                        Serial.println("[Flight] mini-JSON parse error");
                    }
                    free(mini);
                }
            }
        }
    }

    free(buf);
    return result;
}

// ── OpenSky flights API ───────────────────────────────────────────────────────
// Fetches departure/arrival airports and timestamps for a given aircraft on a given date.

static bool fetch_route(const String& icao24, const String& dep_date, FlightInfo& fi) {
    time_t begin_t = date_utc_epoch(dep_date);
    if (begin_t == 0) {
        Serial.printf("[Flight] route: dep_date empty or invalid ('%s') — skipping\n", dep_date.c_str());
        return false;
    }
    time_t end_t = begin_t + 2 * 86400L;  // search +2 days to catch delayed/long flights

    String url = "https://opensky-network.org/api/flights/aircraft?icao24=";
    url += icao24;
    url += "&begin=";
    url += String((long)begin_t);
    url += "&end=";
    url += String((long)end_t);

    Serial.printf("[Flight] route URL: %s\n", url.c_str());

    WiFiClientSecure wc;
    wc.setInsecure();
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT);
    http_lock_take();
    if (!http.begin(wc, url)) { http_lock_give(); return false; }
    if (s_opensky_user.length() > 0)
        http.setAuthorization(s_opensky_user.c_str(), s_opensky_pass.c_str());

    int code = http.GET();
    String body = http.getString();
    http.end();
    http_lock_give();

    Serial.printf("[Flight] route HTTP %d  body: %s\n", code, body.c_str());

    if (code != 200) return false;
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
    fi.callsign = s_entries[slot].callsign;   // keep user-entered name for display
    fi.dep_date = s_entries[slot].dep_date;
    fi.valid    = true;

    // User-entered airports take precedence; fall back to previously cached values
    fi.dep_icao      = s_entries[slot].dep_icao.length() > 0
                       ? s_entries[slot].dep_icao : s_cache[slot].dep_icao;
    fi.arr_icao      = s_entries[slot].arr_icao.length() > 0
                       ? s_entries[slot].arr_icao : s_cache[slot].arr_icao;
    fi.dep_time      = s_cache[slot].dep_time;
    fi.arr_time      = s_cache[slot].arr_time;
    // Skip route API only once timestamps are known or a previous attempt was made.
    // Do NOT skip just because airports are user-provided — timestamps come from
    // fetch_route too, and without them progress/times display stays broken.
    fi.route_checked = (s_cache[slot].dep_time > 0) || s_cache[slot].route_checked;

    // Step 1: live state — use ICAO callsign for the API, keep display name in fi.callsign
    String api_cs = to_icao_callsign(s_entries[slot].callsign);
    String icao24 = s_icao24[slot];
    bool live = fetch_states(api_cs, fi, icao24);
    if (live && icao24.length() > 0)
        s_icao24[slot] = icao24;

    // Step 2: route info — fetch once if we have icao24 and haven't tried yet.
    // /flights/aircraft requires a researcher account (free accounts get 403);
    // route_checked prevents burning HTTP calls on every 5-min refresh.
    if (s_icao24[slot].length() > 0 && !fi.route_checked) {
        fetch_route(s_icao24[slot], s_entries[slot].dep_date, fi);
        fi.route_checked = true;
    }

    // Step 3: fall back to user-entered times when API returned nothing.
    auto make_local_epoch = [](const String& date, const String& hhmm) -> time_t {
        if (date.length() < 10 || hhmm.length() < 5 || hhmm[2] != ':') return 0;
        struct tm t = {};
        t.tm_year  = date.substring(0, 4).toInt() - 1900;
        t.tm_mon   = date.substring(5, 7).toInt() - 1;
        t.tm_mday  = date.substring(8, 10).toInt();
        t.tm_hour  = hhmm.substring(0, 2).toInt();
        t.tm_min   = hhmm.substring(3, 5).toInt();
        t.tm_isdst = -1;
        return mktime(&t);
    };
    if (fi.dep_time == 0 && s_entries[slot].dep_time_str.length() >= 5)
        fi.dep_time = make_local_epoch(s_entries[slot].dep_date, s_entries[slot].dep_time_str);
    if (fi.arr_time == 0 && s_entries[slot].arr_time_str.length() >= 5) {
        time_t at = make_local_epoch(s_entries[slot].dep_date, s_entries[slot].arr_time_str);
        if (at > 0 && fi.dep_time > 0 && at < fi.dep_time) at += 86400;  // overnight flight
        fi.arr_time = at;
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
        s_cache[i].dep_date = entries[i].dep_date;
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
