// ─────────────────────────────────────────────────────────────────────────────
// parcel_tracker.cpp — Swiss Post delivery status (undocumented ekp-web API)
// ─────────────────────────────────────────────────────────────────────────────
// 4-step flow:
//   1. GET  /api/user                         → userIdentifier + session cookie + CSRF token
//   2. POST /api/history?userId=…             → hash
//   3. GET  /api/history/not-included/{hash}  → identity
//   4. GET  /api/shipment/id/{identity}/events/ → events → status
// ─────────────────────────────────────────────────────────────────────────────

#include "../include/parcel_tracker.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

static const char* BASE_URL   = "https://service.post.ch/ekp-web/api";
static const char* ORIGIN     = "https://service.post.ch";
static const char* REFERER    = "https://service.post.ch/ekp-web/ui/";
static const char* USER_AGENT = "Mozilla/5.0 (Linux; Android 10) AppleWebKit/537.36 "
                                "(KHTML, like Gecko) Chrome/120.0.0.0 Mobile Safari/537.36";
static const int   TIMEOUT_MS = 12000;

static String strip_cookie_attrs(const String& set_cookie) {
    int semi = set_cookie.indexOf(';');
    return (semi >= 0) ? set_cookie.substring(0, semi) : set_cookie;
}

static String url_encode(const String& s) {
    String out;
    out.reserve(s.length() + 16);
    for (int i = 0; i < (int)s.length(); i++) {
        char c = s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", (uint8_t)c);
            out += buf;
        }
    }
    return out;
}

// Map a Swiss Post eventCode (PARCEL.*.X.NNNN) to a short status string.
// Falls back to keyword matching on the city name for customs detection.
static String classify_event_code(const String& code, const String& city) {
    // Extract the trailing numeric segment
    int last_dot = code.lastIndexOf('.');
    int n = (last_dot >= 0) ? code.substring(last_dot + 1).toInt() : 0;

    // Known event code ranges (from Swiss Post public API docs + community research):
    if (n >= 6500 && n < 7000) return "Delivered";   // 6500-6699 = delivered
    if (n >= 5600 && n < 5700) return "In delivery";  // 5600-5699 = out for delivery
    if (n >= 5500 && n < 5600) return "In delivery";  // 5500-5599 = delivery attempt

    // Sorting center
    String ci = city; ci.toLowerCase();
    if (ci.indexOf("paketzentrum") >= 0) return "Sorted";

    // Customs: detect via code keyword or city name
    String c = code; c.toLowerCase();
    if (c.indexOf("custom") >= 0 || c.indexOf("zoll") >= 0 ||
        ci.indexOf("zoll") >= 0  || ci.indexOf("customs") >= 0)
        return "Customs";

    return "Shipped";
}

// Fallback: keyword matching on human-readable description text.
static String classify_desc(const String& desc) {
    String d = desc; d.toLowerCase();
    if (d.indexOf("zugestellt") >= 0 || d.indexOf("delivered") >= 0)
        return "Delivered";
    if (d.indexOf("out for delivery") >= 0 || d.indexOf("zustell") >= 0)
        return "In delivery";
    if (d.indexOf("custom") >= 0 || d.indexOf("zoll") >= 0)
        return "Customs";
    return "Shipped";
}

// Configure an HTTPClient for the Swiss Post API.
// Always looks like a browser request (User-Agent, Origin, Referer).
static bool begin_https(HTTPClient& http, WiFiClientSecure& wc,
                        const String& url, const String& cookie,
                        const String& csrf = "") {
    wc.setInsecure();
    http.setTimeout(TIMEOUT_MS);
    http.setReuse(false);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(wc, url)) return false;
    http.addHeader("User-Agent",       USER_AGENT);
    http.addHeader("Accept",           "application/json, text/plain, */*");
    http.addHeader("Accept-Language",  "en-US,en;q=0.9");
    http.addHeader("Origin",           ORIGIN);
    http.addHeader("Referer",          REFERER);
    if (cookie.length() > 0) http.addHeader("Cookie", cookie);
    if (csrf.length()   > 0) http.addHeader("X-CSRF-TOKEN", csrf);
    return true;
}

bool parcel_fetch(const char* tracking_number, String& out_status) {
    String base = BASE_URL;
    String cookie, csrf, user_id;

    // ── Step 1: GET /user → userIdentifier + session cookie + CSRF token ──
    {
        WiFiClientSecure wc;
        HTTPClient http;
        const char* collect[] = {"Set-Cookie", "X-CSRF-TOKEN", "x-csrf-token",
                                 "XSRF-TOKEN",  "x-xsrf-token"};
        http.collectHeaders(collect, 5);

        if (!begin_https(http, wc, base + "/user", "")) {
            Serial.println("[Parcel] Step1 begin failed");
            return false;
        }

        int code = http.GET();
        if (code != 200) {
            Serial.printf("[Parcel] Step1 HTTP %d\n", code);
            http.end(); return false;
        }

        cookie = strip_cookie_attrs(http.header("Set-Cookie"));
        // Try every CSRF header name variant
        for (const char* h : {"X-CSRF-TOKEN", "x-csrf-token", "XSRF-TOKEN", "x-xsrf-token"}) {
            csrf = http.header(h);
            if (csrf.length() > 0) break;
        }

        String body = http.getString();
        http.end();

        Serial.printf("[Parcel] Step1 cookie='%s' csrf='%s'\n",
                      cookie.c_str(), csrf.c_str());
        Serial.printf("[Parcel] Step1 body: %.280s\n", body.c_str());

        if (!body.isEmpty()) {
            JsonDocument doc;
            if (!deserializeJson(doc, body)) {
                for (const char* key : {"userIdentifier", "userId", "id", "customerId"}) {
                    const char* v = doc[key] | (const char*)nullptr;
                    if (v && *v) { user_id = v; break; }
                }
            }
        }
        if (user_id.isEmpty())
            Serial.println("[Parcel] Step1 no userId — cookie-only");
        else
            Serial.printf("[Parcel] Step1 userId='%s'\n", user_id.c_str());
    }

    // ── Step 2: POST /history?userId=… → hash ─────────────────────────────
    String hash;
    {
        WiFiClientSecure wc;
        HTTPClient http;
        const char* collect[] = {"Set-Cookie"};
        http.collectHeaders(collect, 1);

        String url = base + "/history";
        if (!user_id.isEmpty()) url += "?userId=" + url_encode(user_id);
        if (!begin_https(http, wc, url, cookie, csrf)) {
            Serial.println("[Parcel] Step2 begin failed"); return false;
        }
        http.addHeader("Content-Type", "application/json;charset=UTF-8");

        String req = "{\"searchQuery\":\"";
        req += tracking_number;
        req += "\"}";

        int code = http.POST(req);
        String body = http.getString();
        http.end();

        Serial.printf("[Parcel] Step2 HTTP %d body: %.200s\n", code, body.c_str());
        if (code != 200 && code != 201) return false;

        JsonDocument doc;
        if (deserializeJson(doc, body)) { Serial.println("[Parcel] Step2 JSON error"); return false; }
        const char* h = doc["hash"] | (const char*)nullptr;
        if (!h && doc.is<JsonArray>() && doc.size() > 0)
            h = doc[0]["hash"] | (const char*)nullptr;
        if (!h || !*h) { Serial.println("[Parcel] Step2 no hash"); return false; }
        hash = h;
    }

    // ── Step 3: GET /history/not-included/{hash} → identity ───────────────
    String identity;
    {
        WiFiClientSecure wc;
        HTTPClient http;

        String url = base + "/history/not-included/" + hash;
        if (!user_id.isEmpty()) url += "?userId=" + url_encode(user_id);
        if (!begin_https(http, wc, url, cookie, csrf)) {
            Serial.println("[Parcel] Step3 begin failed"); return false;
        }

        int code = http.GET();
        String body = http.getString();
        http.end();

        Serial.printf("[Parcel] Step3 HTTP %d body: %.200s\n", code, body.c_str());
        if (code != 200) return false;

        JsonDocument doc;
        if (deserializeJson(doc, body)) { Serial.println("[Parcel] Step3 JSON error"); return false; }
        const char* id = doc["identity"] | (const char*)nullptr;
        if (!id && doc.is<JsonArray>() && doc.size() > 0)
            id = doc[0]["identity"] | (const char*)nullptr;
        if (!id || !*id) { Serial.println("[Parcel] Step3 no identity"); return false; }
        identity = id;
    }

    // ── Step 4: GET events for this shipment ──────────────────────────────
    // Try URL patterns in order — the correct one varies across API versions.
    {
        // Candidates to try in sequence; %s = identity (already ASCII-safe)
        static const char* PATTERNS[] = {
            "/shipment/id/%s/events/",
            "/shipment/id/%s/events",
            "/shipment/%s/events/",
            "/shipment/%s/events",
        };

        int code = 0;
        String body;
        for (const char* pat : PATTERNS) {
            char path[160];
            snprintf(path, sizeof(path), pat, identity.c_str());
            String url = base + path;
            if (!user_id.isEmpty()) { url += "?userId="; url += url_encode(user_id); }

            WiFiClientSecure wc;
            HTTPClient http;
            if (!begin_https(http, wc, url, cookie, csrf)) continue;
            code = http.GET();
            body = http.getString();
            http.end();
            Serial.printf("[Parcel] Step4 %s → HTTP %d\n", path, code);
            if (code == 200) break;
        }
        Serial.printf("[Parcel] Step4 body: %.280s\n", body.c_str());
        if (code != 200) return false;

        JsonDocument doc;
        if (deserializeJson(doc, body)) { Serial.println("[Parcel] Step4 JSON error"); return false; }

        // Events are newest-first — take the first one that parses successfully.
        String latest_status;
        auto walk_events = [&](JsonArray arr) {
            for (JsonObject ev : arr) {
                const char* code_str = ev["eventCode"] | (const char*)nullptr;
                const char* city_str = ev["city"]      | (const char*)nullptr;
                if (code_str && *code_str) {
                    latest_status = classify_event_code(String(code_str),
                                                        city_str ? String(city_str) : String());
                    Serial.printf("[Parcel] event code=%s city=%s → %s\n",
                                  code_str, city_str ? city_str : "", latest_status.c_str());
                    return;  // newest event wins; stop here
                }
                // Fallback: description fields (older API variants)
                const char* d = ev["description"]      | ev["eventDescription"] |
                                ev["text"]             | ev["status"]           |
                                ev["descriptionLocal"] | (const char*)nullptr;
                if (d && *d) {
                    latest_status = classify_desc(String(d));
                    return;
                }
            }
        };
        if (doc.is<JsonArray>())                walk_events(doc.as<JsonArray>());
        else if (doc["events"].is<JsonArray>()) walk_events(doc["events"].as<JsonArray>());

        if (latest_status.isEmpty()) {
            Serial.println("[Parcel] No events parsed — defaulting Shipped");
            out_status = "Shipped";
            return true;
        }
        out_status = latest_status;
        Serial.printf("[Parcel] Final status='%s'\n", out_status.c_str());
        return true;
    }
}
