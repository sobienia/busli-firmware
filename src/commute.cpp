// ─────────────────────────────────────────────────────────────────────────────
// commute.cpp — Fetch and parse connections from transport.opendata.ch/v1/connections
// ─────────────────────────────────────────────────────────────────────────────

#include "../include/commute.h"
#include "../include/config.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <time.h>

static String url_encode(const String& s) {
    String out;
    out.reserve(s.length() * 3);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", (uint8_t)c);
            out += hex;
        }
    }
    return out;
}

static time_t make_utc_cm(int yr, int mo, int dy, int hr, int mn, int sc) {
    static const uint16_t MOFF[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
    bool leap = (yr%4==0) && (yr%100!=0 || yr%400==0);
    int yday  = MOFF[mo-1] + dy - 1 + (leap && mo>2 ? 1 : 0);
    int y1    = yr - 1;
    int leaps = (y1/4-492) - (y1/100-19) + (y1/400-4);
    long days = (long)(yr-1970)*365 + leaps + yday;
    return (time_t)(days*86400L + (long)hr*3600L + (long)mn*60L + sc);
}

static time_t parse_iso8601_cm(const String& iso) {
    if (iso.length() < 19) return 0;
    int yr = iso.substring(0,  4).toInt();
    int mo = iso.substring(5,  7).toInt();
    int dy = iso.substring(8,  10).toInt();
    int hr = iso.substring(11, 13).toInt();
    int mn = iso.substring(14, 16).toInt();
    int sc = iso.substring(17, 19).toInt();
    if (yr < 2000 || mo < 1 || mo > 12 || dy < 1) return 0;
    time_t epoch = make_utc_cm(yr, mo, dy, hr, mn, sc);
    if (epoch <= 0) return 0;
    int off = 0;
    if (iso.length() >= 20) {
        char sign = iso.charAt(19);
        if (sign == '+' || sign == '-') {
            int hh = 0, mm = 0;
            if (iso.length() >= 22) hh = iso.substring(20, 22).toInt();
            if (iso.length() >= 24) {
                int ms = (iso.charAt(22) == ':') ? 23 : 22;
                mm = iso.substring(ms, ms+2).toInt();
            }
            off = (hh*3600 + mm*60) * (sign=='-' ? -1 : 1);
        }
    }
    return epoch - off;
}

static bool parse_connections(const char* json, size_t json_len, CommuteData& out) {
    JsonDocument filter;
    filter["connections"][0]["sections"][0]["journey"]["name"]              = true;
    filter["connections"][0]["sections"][0]["journey"]["category"]          = true;
    filter["connections"][0]["sections"][0]["journey"]["number"]            = true;
    filter["connections"][0]["sections"][0]["departure"]["station"]["name"] = true;
    filter["connections"][0]["sections"][0]["departure"]["departure"]       = true;
    filter["connections"][0]["sections"][0]["arrival"]["station"]["name"]   = true;
    filter["connections"][0]["sections"][0]["arrival"]["arrival"]           = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json, json_len,
                                               DeserializationOption::Filter(filter));
    if (err) {
        Serial.printf("[Commute] JSON error: %s\n", err.c_str());
        return false;
    }

    time_t now = time(nullptr);
    int n_conns = 0;

    for (JsonObject conn : doc["connections"].as<JsonArray>()) {
        if (n_conns >= COMMUTE_MAX_CONNECTIONS) break;

        CommuteConnection& cc = out.connections[n_conns];
        cc.leg_count = 0;
        cc.dep_time  = 0;

        for (JsonObject sec : conn["sections"].as<JsonArray>()) {
            if (cc.leg_count >= COMMUTE_MAX_LEGS) break;

            // Skip walking/transfer sections (no journey)
            const char* jname = sec["journey"]["name"]     | "";
            const char* jcat  = sec["journey"]["category"] | "";
            const char* jnum  = sec["journey"]["number"]   | "";
            if (strlen(jname) == 0 && strlen(jnum) == 0) continue;

            CommuteLeg& leg = cc.legs[cc.leg_count];

            // journey.name is sometimes a raw trip number (all digits, e.g. "41994")
            // rather than the marketed line. Use journey.number ("80") in that case.
            bool name_is_digits = (strlen(jname) > 0);
            for (const char* p = jname; *p && name_is_digits; p++)
                if (!isdigit((uint8_t)*p)) name_is_digits = false;

            if (!name_is_digits && strlen(jname) > 0) {
                leg.line = String(jname);
                leg.line.replace(" ", "");          // "IC 5" → "IC5"
            } else if (strlen(jnum) > 0) {
                leg.line = String(jnum);            // use line number e.g. "80"
            } else {
                leg.line = String(jcat);            // last resort: "B", "T", …
            }

            leg.from     = String(sec["departure"]["station"]["name"] | "");
            leg.to       = String(sec["arrival"]["station"]["name"]   | "");
            leg.dep_time = parse_iso8601_cm(String(sec["departure"]["departure"] | ""));
            leg.arr_time = parse_iso8601_cm(String(sec["arrival"]["arrival"]     | ""));

            if (cc.leg_count == 0) cc.dep_time = leg.dep_time;
            cc.leg_count++;
        }

        // Only keep connections that haven't departed more than 1 minute ago
        if (cc.leg_count > 0 && cc.dep_time > now - 60) {
            n_conns++;
        }
    }

    out.connection_count = n_conns;
    out.fetch_time       = time(nullptr);
    out.valid            = (n_conns > 0);
    Serial.printf("[Commute] Parsed %d connections\n", n_conns);
    return out.valid;
}

bool commute_fetch(const String& from_station, const String& to_station, CommuteData& out) {
    out = {};
    if (from_station.isEmpty() || to_station.isEmpty()) return false;

    String url = "http://transport.opendata.ch/v1/connections?from=";
    url += url_encode(from_station);
    url += "&to=";
    url += url_encode(to_station);
    url += "&limit=";
    url += String(COMMUTE_MAX_CONNECTIONS);

    Serial.print("[Commute] Fetching: ");
    Serial.println(url);

    HTTPClient http;
    http.setTimeout(API_TIMEOUT_MS);
    http.setReuse(false);
    if (!http.begin(url)) {
        Serial.println("[Commute] http.begin() failed");
        return false;
    }

    int code = http.GET();
    if (code != 200) {
        Serial.printf("[Commute] HTTP error: %d\n", code);
        http.end();
        return false;
    }

    int content_len = http.getSize();
    Serial.printf("[Commute] Body: ~%d bytes\n", content_len);

    const int MAX_BODY = 150000;
    char* buf = (char*)heap_caps_malloc(MAX_BODY + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        http.end();
        Serial.println("[Commute] PSRAM alloc failed");
        return false;
    }
    int buf_len = 0;

    WiFiClient& stream = http.getStream();
    uint32_t deadline  = millis() + API_TIMEOUT_MS;

    if (content_len < 0) {
        // Chunked transfer
        uint8_t rb[512]; int ri = 0, rn = 0;
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
    } else {
        // Content-Length path
        uint8_t rbuf[512];
        for (int rem = content_len; rem > 0 && millis() < deadline; ) {
            int got = stream.read(rbuf, min(rem, (int)sizeof(rbuf)));
            if (got > 0) {
                if (buf_len + got <= MAX_BODY) {
                    memcpy(buf + buf_len, rbuf, got);
                    buf_len += got;
                }
                rem -= got;
            } else { delay(1); }
        }
    }
    buf[buf_len] = '\0';
    http.end();

    Serial.printf("[Commute] Read %d bytes\n", buf_len);
    bool ok = (buf_len > 0) && parse_connections(buf, buf_len, out);
    heap_caps_free(buf);
    return ok;
}
