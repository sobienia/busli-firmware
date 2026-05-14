// ─────────────────────────────────────────────────────────────────────────────
// api.cpp — Swiss transit API client implementation
// ─────────────────────────────────────────────────────────────────────────────

#include "api.h"
#include "../include/config.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <time.h>

// Compute UTC epoch from calendar fields without any TZ manipulation.
// Thread-safe: no global state touched.
static time_t make_utc(int yr, int mo, int dy, int hr, int mn, int sc) {
    static const uint16_t MOFF[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
    bool leap = (yr % 4 == 0) && (yr % 100 != 0 || yr % 400 == 0);
    int yday  = MOFF[mo - 1] + dy - 1 + (leap && mo > 2 ? 1 : 0);
    int y1    = yr - 1;
    int leaps = (y1/4 - 492) - (y1/100 - 19) + (y1/400 - 4);
    long days = (long)(yr - 1970) * 365 + leaps + yday;
    return (time_t)(days * 86400L + (long)hr * 3600L + (long)mn * 60L + sc);
}

// Convert ISO 8601 timestamp to UTC epoch.
// Handles "+0200", "+02:00", "Z", and bare local time (treated as UTC).
// Returns 0 on parse error. Thread-safe — no setenv/tzset.
static time_t parse_iso8601(const String& iso) {
    if (iso.length() < 19) return 0;

    int yr = iso.substring(0,  4).toInt();
    int mo = iso.substring(5,  7).toInt();
    int dy = iso.substring(8,  10).toInt();
    int hr = iso.substring(11, 13).toInt();
    int mn = iso.substring(14, 16).toInt();
    int sc = iso.substring(17, 19).toInt();

    if (yr < 2000 || mo < 1 || mo > 12 || dy < 1) return 0;

    time_t epoch = make_utc(yr, mo, dy, hr, mn, sc);
    if (epoch <= 0) return 0;

    int offset_seconds = 0;
    if (iso.length() >= 20) {
        char sign = iso.charAt(19);
        if (sign == '+' || sign == '-') {
            int hh = 0, mm = 0;
            if (iso.length() >= 22) hh = iso.substring(20, 22).toInt();
            if (iso.length() >= 24) {
                int mm_start = (iso.charAt(22) == ':') ? 23 : 22;
                mm = iso.substring(mm_start, mm_start + 2).toInt();
            }
            offset_seconds = (hh * 3600 + mm * 60) * (sign == '-' ? -1 : 1);
        }
    }
    return epoch - offset_seconds;
}

// Strip redundant city prefixes from destination names.
static const char* CITY_PREFIXES[] = {
    "Zürich, ", "Zurich, ", "Schlieren, ", "Baden, ", "Dietikon, "
};
static String format_destination(const String& dest) {
    for (const char* prefix : CITY_PREFIXES) {
        if (dest.startsWith(prefix))
            return dest.substring(strlen(prefix));
    }
    return dest;
}

// Check if a destination matches any of the direction filter substrings.
// If no filter is given (count == 0), everything matches.
static bool matches_direction(const String& dest, const StopConfig& stop) {
    if (stop.direction_count == 0) return true;
    String dest_lower = dest;
    dest_lower.toLowerCase();
    for (int i = 0; i < stop.direction_count; i++) {
        String pattern = String(stop.direction_filter[i]);
        pattern.toLowerCase();
        if (dest_lower.indexOf(pattern) != -1) return true;
    }
    return false;
}

// Check if a line matches the filter.
// Accepts either the bare number ("12") or the category-prefixed form ("S12").
// This handles the transport.opendata.ch API returning category and number separately
// for train lines (category="S", number="12") vs the user filtering on "S12".
// If no filter is configured (count == 0), everything matches.
static bool matches_line(const String& number, const String& category, const StopConfig& stop) {
    if (stop.line_count == 0) return true;
    String catnum = category + number;  // e.g. "S" + "12" = "S12"
    for (int i = 0; i < stop.line_count; i++) {
        String f = String(stop.line_filter[i]);
        if (number == f || catnum == f) return true;
    }
    return false;
}

// Parse JSON body and extract matching departures.
// Takes const char* so ArduinoJson copies strings — caller may free buf immediately after.
static bool process_json_body(
    const char* json, size_t json_len,
    const StopConfig& stop,
    std::vector<Departure>& out_departures,
    int max_results
) {
    JsonDocument filter;
    filter["stationboard"][0]["number"]                         = true;
    filter["stationboard"][0]["category"]                       = true;  // needed for "S12" train matching
    filter["stationboard"][0]["to"]                             = true;
    filter["stationboard"][0]["stop"]["departure"]              = true;
    filter["stationboard"][0]["stop"]["prognosis"]["departure"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json, json_len,
                                               DeserializationOption::Filter(filter));
    if (err) {
        Serial.print("[API] JSON error: ");
        Serial.println(err.c_str());
        return false;
    }
    Serial.println("[API] JSON OK");

    std::vector<Departure> results;
    JsonArray board = doc["stationboard"];

    time_t now = time(nullptr);
    int dbg_noparse = 0, dbg_past = 0, dbg_filt = 0;

    bool logged_sample = false;
    for (JsonObject entry : board) {
        String number   = String((const char*)(entry["number"]   | ""));
        String category = String((const char*)(entry["category"] | ""));
        String dest     = format_destination(String((const char*)(entry["to"] | "")));

        // Log the first entry so the serial monitor shows what the API actually returns.
        // Useful when a line filter produces zero results.
        if (!logged_sample) {
            Serial.printf("[API] sample: category='%s' number='%s' to='%s'\n",
                          category.c_str(), number.c_str(), dest.c_str());
            logged_sample = true;
        }

        // Build the display line: for trains prepend category (e.g. "S"+"12" → "S12");
        // for buses/trams the number alone is already the line name ("80", "7").
        String line = (category.length() > 0 && category != "B" &&
                       category != "T" && category != "NFB" && category != "NFT" &&
                       !number.startsWith(category))
                      ? category + number
                      : number;

        if (!matches_line(number, category, stop)) { dbg_filt++; continue; }
        if (!matches_direction(dest, stop))        { dbg_filt++; continue; }

        const char* sched_iso = entry["stop"]["departure"]              | "";
        const char* prog_iso  = entry["stop"]["prognosis"]["departure"] | "";

        time_t scheduled = parse_iso8601(String(sched_iso));
        time_t actual    = (strlen(prog_iso) > 0)
                            ? parse_iso8601(String(prog_iso))
                            : scheduled;

        if (actual == 0) { dbg_noparse++; continue; }

        int delay_minutes = scheduled > 0
            ? (int)((actual - scheduled) / 60)
            : 0;
        int mins_until = (int)((actual - now) / 60);

        if (mins_until < 0) { dbg_past++; continue; }
        if (mins_until > 99) mins_until = 99;

        Departure d;
        d.line        = line;
        d.destination = dest;
        d.minutes     = mins_until;
        d.delay       = delay_minutes;
        results.push_back(d);

        if ((int)results.size() >= max_results) break;
    }

    out_departures = results;
    Serial.printf("[API] Got %d departures (filt=%d noparse=%d past=%d)\n",
                  (int)results.size(), dbg_filt, dbg_noparse, dbg_past);
    return true;
}

bool api_fetch_departures(
    const StopConfig& stop,
    std::vector<Departure>& out_departures,
    int max_results
) {
    // Build URL
    String url = API_BASE_URL;
    url += "?station=";
    String station = stop.station;
    String encoded = "";
    for (size_t i = 0; i < station.length(); i++) {
        char c = station[i];
        if (isalnum(c)) {
            encoded += c;
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", (uint8_t)c);
            encoded += hex;
        }
    }
    url += encoded;
    url += "&limit=" + String(API_FETCH_LIMIT);

    Serial.print("[API] Fetching: ");
    Serial.println(url);

    HTTPClient http;
    http.setTimeout(API_TIMEOUT_MS);
    http.setReuse(false);

    if (!http.begin(url)) {
        Serial.println("[API] http.begin() failed");
        return false;
    }

    int code = http.GET();
    if (code != 200) {
        Serial.printf("[API] HTTP error: %d", code);
        if (code < 0) Serial.printf(" (%s)", http.errorToString(code).c_str());
        Serial.println();
        http.end();
        return false;
    }

    int content_len = http.getSize();
    Serial.printf("[API] Body: ~%d bytes\n", content_len);

    if (content_len < 0) {
        // Chunked transfer encoding — decode directly into a PSRAM buffer.
        // Using heap_caps_malloc (explicit PSRAM) instead of Arduino String avoids
        // the DRAM→PSRAM realloc path in String::concat, which silently misaligns
        // writes when the buffer crosses from DRAM into PSRAM.
        const int MAX_BODY = 300000;
        char* buf = (char*)heap_caps_malloc(MAX_BODY + 1,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) {
            http.end();
            Serial.println("[API] PSRAM alloc failed");
            return false;
        }
        int buf_len = 0;

        WiFiClient& stream = http.getStream();
        uint8_t  rb[512]; int ri = 0, rn = 0;
        uint32_t deadline = millis() + API_TIMEOUT_MS;

        auto refill = [&]() -> bool {
            for (;;) {
                if (millis() >= deadline) return false;
                int got = stream.read(rb, sizeof(rb));
                if (got > 0) { ri = 0; rn = got; return true; }
                if (!stream.connected() && !stream.available()) return false;
                delay(1);
            }
        };
        auto rb_get = [&]() -> int {
            if (ri >= rn && !refill()) return -1;
            return (uint8_t)rb[ri++];
        };

        for (bool running = true; running; ) {
            // Read chunk-size line (hex digits terminated by \r\n)
            char hx[12]; int hl = 0;
            for (;;) {
                int c = rb_get();
                if (c < 0) { running = false; break; }
                if (c == '\n') break;
                if (c != '\r' && hl < 11) hx[hl++] = (char)c;
            }
            if (!running) break;
            hx[hl] = '\0';
            int sz = (int)strtol(hx, nullptr, 16);
            if (sz <= 0) break;

            for (int rem = sz; rem > 0; ) {
                if (ri >= rn && !refill()) { running = false; break; }
                int take = min(rem, rn - ri);
                if (buf_len + take <= MAX_BODY) {
                    memcpy(buf + buf_len, rb + ri, take);
                    buf_len += take;
                }
                ri += take; rem -= take;
            }
            yield();

            if (rb_get() < 0 || rb_get() < 0) break;
        }
        buf[buf_len] = '\0';

        http.end();
        Serial.printf("[API] chunked: %d bytes\n", buf_len);

        bool ok = (buf_len > 0) &&
                  process_json_body((const char*)buf, buf_len, stop, out_departures, max_results);
        heap_caps_free(buf);
        return ok;
    }

    // Content-Length path — bulk read through _rxBuffer (works for smaller responses).
    String body;
    body.reserve(content_len + 1);
    {
        WiFiClient& stream = http.getStream();
        uint32_t deadline  = millis() + API_TIMEOUT_MS;
        uint8_t  buf[512];
        for (int rem = content_len; rem > 0 && millis() < deadline; ) {
            int got = stream.read(buf, min(rem, (int)sizeof(buf)));
            if (got > 0) { body.concat((const char*)buf, got); rem -= got; }
            else          { delay(1); }
        }
    }

    http.end();

    if (body.isEmpty()) {
        Serial.println("[API] Empty response body");
        return false;
    }
    return process_json_body(body.c_str(), body.length(), stop, out_departures, max_results);
}
