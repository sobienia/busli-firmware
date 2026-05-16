// ─────────────────────────────────────────────────────────────────────────────
// parcel_tracker.cpp — Swiss Post delivery status (undocumented ekp-web API)
// ─────────────────────────────────────────────────────────────────────────────
// 4-step flow:
//   1. GET  /api/user                         → userId + session cookie
//   2. POST /api/history?userId=…             → hash
//   3. GET  /api/history/not-included/{hash}  → identity
//   4. GET  /api/shipment/id/{identity}/events/ → events → status
//
// Best-effort: if the API layout changes or a step fails, returns false.
// The caller (fetch_task.cpp) holds the http_lock for the full call.
// ─────────────────────────────────────────────────────────────────────────────

#include "../include/parcel_tracker.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

static const char* BASE_URL  = "https://service.post.ch/ekp-web/api";
static const int   TIMEOUT_MS = 12000;

// Strip cookie attributes (everything from the first ';' onward).
static String strip_cookie_attrs(const String& set_cookie) {
    int semi = set_cookie.indexOf(';');
    return (semi >= 0) ? set_cookie.substring(0, semi) : set_cookie;
}

// Map the latest event's description text to a short display status.
static String classify_status(const String& desc) {
    String d = desc;
    d.toLowerCase();
    if (d.indexOf("deliver") >= 0 || d.indexOf("zugestellt") >= 0 ||
        d.indexOf("delivered")  >= 0)
        return "Delivered";
    if (d.indexOf("out for delivery") >= 0 || d.indexOf("zustellung") >= 0 ||
        d.indexOf("courier") >= 0 || d.indexOf("zustell") >= 0)
        return "In delivery";
    if (d.indexOf("custom") >= 0 || d.indexOf("zoll") >= 0 ||
        d.indexOf("clearance") >= 0)
        return "Customs";
    return "Shipped";
}

// Build an HTTPClient pointing at url, with shared headers.
static bool begin_https(HTTPClient& http, WiFiClientSecure& wc,
                        const String& url, const String& cookie) {
    wc.setInsecure();
    http.setTimeout(TIMEOUT_MS);
    http.setReuse(false);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(wc, url)) return false;
    http.addHeader("Accept", "application/json");
    http.addHeader("X-Requested-With", "XMLHttpRequest");
    if (cookie.length() > 0) http.addHeader("Cookie", cookie);
    return true;
}

bool parcel_fetch(const char* tracking_number, String& out_status) {
    String base = BASE_URL;
    String cookie;

    // ── Step 1: GET /user → userId + session cookie ────────────────────────
    {
        WiFiClientSecure wc;
        HTTPClient http;
        const char* collect[] = {"Set-Cookie", "x-csrf-token"};
        http.collectHeaders(collect, 2);

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
        String body = http.getString();
        http.end();

        Serial.printf("[Parcel] Step1 OK cookie='%s'\n", cookie.c_str());

        JsonDocument doc;
        if (deserializeJson(doc, body)) {
            Serial.println("[Parcel] Step1 JSON error"); return false;
        }
        String user_id = doc["userId"] | "";
        if (user_id.isEmpty()) {
            Serial.println("[Parcel] Step1 no userId"); return false;
        }

        // ── Step 2: POST /history?userId=… → hash ─────────────────────────
        {
            WiFiClientSecure wc2;
            HTTPClient http2;
            const char* collect2[] = {"Set-Cookie"};
            http2.collectHeaders(collect2, 1);

            String url2 = base + "/history?userId=" + user_id;
            if (!begin_https(http2, wc2, url2, cookie)) {
                Serial.println("[Parcel] Step2 begin failed"); return false;
            }
            http2.addHeader("Content-Type", "application/json");

            String req_body = "{\"searchQuery\":\"";
            req_body += tracking_number;
            req_body += "\"}";

            int code2 = http2.POST(req_body);
            if (code2 != 200 && code2 != 201) {
                Serial.printf("[Parcel] Step2 HTTP %d\n", code2);
                http2.end(); return false;
            }

            String new_ck = strip_cookie_attrs(http2.header("Set-Cookie"));
            if (new_ck.length() > 0) cookie = new_ck;

            String body2 = http2.getString();
            http2.end();
            Serial.printf("[Parcel] Step2 body: %.120s\n", body2.c_str());

            JsonDocument doc2;
            if (deserializeJson(doc2, body2)) {
                Serial.println("[Parcel] Step2 JSON error"); return false;
            }

            // hash may live at root or inside the first array element
            String hash = doc2["hash"] | "";
            if (hash.isEmpty() && doc2.is<JsonArray>() && doc2.size() > 0)
                hash = doc2[0]["hash"] | "";
            if (hash.isEmpty()) {
                Serial.println("[Parcel] Step2 no hash"); return false;
            }

            // ── Step 3: GET /history/not-included/{hash} → identity ────────
            {
                WiFiClientSecure wc3;
                HTTPClient http3;

                String url3 = base + "/history/not-included/" + hash
                              + "?userId=" + user_id;
                if (!begin_https(http3, wc3, url3, cookie)) {
                    Serial.println("[Parcel] Step3 begin failed"); return false;
                }

                int code3 = http3.GET();
                if (code3 != 200) {
                    Serial.printf("[Parcel] Step3 HTTP %d\n", code3);
                    http3.end(); return false;
                }

                String body3 = http3.getString();
                http3.end();
                Serial.printf("[Parcel] Step3 body: %.120s\n", body3.c_str());

                JsonDocument doc3;
                if (deserializeJson(doc3, body3)) {
                    Serial.println("[Parcel] Step3 JSON error"); return false;
                }

                String identity = doc3["identity"] | "";
                if (identity.isEmpty() && doc3.is<JsonArray>() && doc3.size() > 0)
                    identity = doc3[0]["identity"] | "";
                if (identity.isEmpty()) {
                    Serial.println("[Parcel] Step3 no identity"); return false;
                }

                // ── Step 4: GET /shipment/id/{identity}/events/ ────────────
                {
                    WiFiClientSecure wc4;
                    HTTPClient http4;

                    String url4 = base + "/shipment/id/" + identity + "/events/";
                    if (!begin_https(http4, wc4, url4, cookie)) {
                        Serial.println("[Parcel] Step4 begin failed"); return false;
                    }

                    int code4 = http4.GET();
                    if (code4 != 200) {
                        Serial.printf("[Parcel] Step4 HTTP %d\n", code4);
                        http4.end(); return false;
                    }

                    String body4 = http4.getString();
                    http4.end();
                    Serial.printf("[Parcel] Step4 body: %.200s\n", body4.c_str());

                    JsonDocument doc4;
                    if (deserializeJson(doc4, body4)) {
                        Serial.println("[Parcel] Step4 JSON error"); return false;
                    }

                    // Walk the events array (may be at root or under "events" key)
                    String latest_desc;
                    auto walk_events = [&](JsonArray arr) {
                        for (JsonObject ev : arr) {
                            // Try common field name variants
                            const char* d =
                                ev["description"]      | ev["eventDescription"] |
                                ev["text"]             | ev["status"]           |
                                ev["descriptionLocal"] | (const char*)nullptr;
                            if (d && *d) latest_desc = d;
                        }
                    };

                    if (doc4.is<JsonArray>())
                        walk_events(doc4.as<JsonArray>());
                    else if (doc4["events"].is<JsonArray>())
                        walk_events(doc4["events"].as<JsonArray>());

                    if (latest_desc.isEmpty()) {
                        // Got events but couldn't read description — assume in transit
                        Serial.println("[Parcel] No event description, defaulting Shipped");
                        out_status = "Shipped";
                        return true;
                    }

                    out_status = classify_status(latest_desc);
                    Serial.printf("[Parcel] Status='%s' (desc='%s')\n",
                                  out_status.c_str(), latest_desc.c_str());
                    return true;
                }
            }
        }
    }
}
