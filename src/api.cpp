// ─────────────────────────────────────────────────────────────────────────────
// api.cpp — Swiss transit API client implementation
// ─────────────────────────────────────────────────────────────────────────────

#include "api.h"
#include "../include/config.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

// Convert ISO 8601 timestamp to a time_t (UTC epoch).
// Handles formats with explicit timezone offsets:
//   "2026-04-28T18:30:00+0200"  → 16:30 UTC
//   "2026-04-28T18:30:00+02:00" → 16:30 UTC
//   "2026-04-28T18:30:00Z"      → 18:30 UTC
//   "2026-04-28T18:30:00"       → assumed UTC (fallback)
// Returns 0 on parse error.
static time_t parse_iso8601(const String& iso) {
    if (iso.length() < 19) return 0;

    struct tm t = {};
    t.tm_year = iso.substring(0, 4).toInt() - 1900;
    t.tm_mon  = iso.substring(5, 7).toInt() - 1;
    t.tm_mday = iso.substring(8, 10).toInt();
    t.tm_hour = iso.substring(11, 13).toInt();
    t.tm_min  = iso.substring(14, 16).toInt();
    t.tm_sec  = iso.substring(17, 19).toInt();

    // Treat the H:M:S as UTC first using timegm-style conversion.
    // Arduino doesn't have timegm(), so we compute it manually:
    // mktime() respects the current TZ env, so we temporarily switch TZ
    // to UTC, call mktime, then restore.
    char old_tz[64] = "";
    const char* cur_tz = getenv("TZ");
    if (cur_tz) strncpy(old_tz, cur_tz, sizeof(old_tz) - 1);
    setenv("TZ", "UTC0", 1);
    tzset();
    time_t epoch = mktime(&t);
    if (old_tz[0]) setenv("TZ", old_tz, 1);
    else           unsetenv("TZ");
    tzset();

    if (epoch <= 0) return 0;

    // Now adjust for the timezone offset given in the string
    int offset_seconds = 0;
    if (iso.length() >= 20) {
        char sign = iso.charAt(19);
        if (sign == 'Z') {
            offset_seconds = 0;
        } else if (sign == '+' || sign == '-') {
            // Find offset hours/minutes — handle both "+0200" and "+02:00"
            int hh = 0, mm = 0;
            if (iso.length() >= 22) hh = iso.substring(20, 22).toInt();
            if (iso.length() >= 24) {
                int mm_start = (iso.charAt(22) == ':') ? 23 : 22;
                mm = iso.substring(mm_start, mm_start + 2).toInt();
            }
            offset_seconds = (hh * 3600 + mm * 60) * (sign == '-' ? -1 : 1);
        }
    }

    // Subtract the offset to get UTC epoch
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

// Check if a line number matches the line filter.
// If no filter is given (count == 0), everything matches.
static bool matches_line(const String& line, const StopConfig& stop) {
    if (stop.line_count == 0) return true;
    for (int i = 0; i < stop.line_count; i++) {
        if (line == String(stop.line_filter[i])) return true;
    }
    return false;
}

bool api_fetch_departures(
    const StopConfig& stop,
    std::vector<Departure>& out_departures,
    int max_results
) {
    // Build URL
    String url = API_BASE_URL;
    url += "?station=";
    // URL-encode the station name
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
    url += "&limit=6";

    Serial.print("[API] Fetching: ");
    Serial.println(url);

    HTTPClient http;
    http.setTimeout(API_TIMEOUT_MS);
    http.setReuse(false);  // fresh connection each time — more reliable on flaky WiFi

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

    // getString() handles chunked transfer encoding correctly.
    // The field filter below keeps the parsed document tiny regardless of body size.
    String body = http.getString();
    http.end();

    Serial.printf("[API] Body: %d bytes\n", body.length());

    if (body.length() < 10) {
        Serial.println("[API] Body too short");
        return false;
    }

    // Only parse the four fields we need — parsed doc stays small even for 80 KB+ bodies.
    JsonDocument filter;
    filter["stationboard"][0]["number"]                         = true;
    filter["stationboard"][0]["to"]                             = true;
    filter["stationboard"][0]["stop"]["departure"]              = true;
    filter["stationboard"][0]["stop"]["prognosis"]["departure"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body,
                                               DeserializationOption::Filter(filter));
    if (err) {
        Serial.print("[API] JSON error: ");
        Serial.println(err.c_str());
        return false;
    }
    Serial.println("[API] JSON OK");

    // Extract departures, applying direction + line filters
    std::vector<Departure> results;
    JsonArray board = doc["stationboard"];

    time_t now = time(nullptr);

    for (JsonObject entry : board) {
        String line = String((const char*)(entry["number"] | ""));
        String dest = format_destination(
            String((const char*)(entry["to"] | ""))
        );

        if (!matches_line(line, stop))      continue;
        if (!matches_direction(dest, stop)) continue;

        // Compute minutes until departure
        const char* sched_iso = entry["stop"]["departure"]              | "";
        const char* prog_iso  = entry["stop"]["prognosis"]["departure"] | "";

        time_t scheduled = parse_iso8601(String(sched_iso));
        time_t actual    = (strlen(prog_iso) > 0)
                            ? parse_iso8601(String(prog_iso))
                            : scheduled;

        if (actual == 0) continue;

        int delay_minutes = scheduled > 0
            ? (int)((actual - scheduled) / 60)
            : 0;
        int mins_until = (int)((actual - now) / 60);

        if (mins_until < 0) continue;        // past departures
        if (mins_until > 99) mins_until = 99; // clamp display

        Departure d;
        d.line        = line;
        d.destination = dest;
        d.minutes     = mins_until;
        d.delay       = delay_minutes;
        results.push_back(d);

        if ((int)results.size() >= max_results) break;
    }

    out_departures = results;
    Serial.printf("[API] Got %d departures\n", (int)results.size());
    return true;
}
