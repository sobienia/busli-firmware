// ─────────────────────────────────────────────────────────────────────────────
// pager.cpp — ntfy.sh publish + poll for peer-to-peer messaging
// ─────────────────────────────────────────────────────────────────────────────
// Message wire format (plain text body, newline-separated):
//   Line 1:  human-readable text shown on screen, e.g. "AJ: Beer in 30min?"
//   Line 2+: "reply=<topic>" — lets recipient know where to send a reply
// ─────────────────────────────────────────────────────────────────────────────

#include "../include/pager.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <esp_random.h>

static const char* NTFY_BASE  = "https://ntfy.sh";
static const int   TIMEOUT_MS = 6000;

String pager_generate_topic() {
    char buf[24];
    snprintf(buf, sizeof(buf), "busli-%08lx%08lx",
             (unsigned long)esp_random(), (unsigned long)esp_random());
    return String(buf);
}

bool pager_publish(const String& to_topic, const String& sender_name,
                   const String& own_topic, const String& text) {
    if (to_topic.isEmpty()) return false;

    String body = text;
    if (own_topic.length() > 0) { body += "\nreply="; body += own_topic; }

    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) delay(1500);
        WiFiClientSecure wc;
        wc.setInsecure();
        HTTPClient http;
        http.setTimeout(TIMEOUT_MS);
        http.setReuse(false);
        if (!http.begin(wc, String(NTFY_BASE) + "/" + to_topic)) continue;
        http.addHeader("Title",        sender_name);
        http.addHeader("Content-Type", "text/plain;charset=UTF-8");
        int code = http.POST(body);
        http.end();
        Serial.printf("[Pager] Publish to %s: HTTP %d (attempt %d)\n",
                      to_topic.c_str(), code, attempt + 1);
        if (code == 200) return true;
    }
    return false;
}

bool pager_poll(const String& own_topic, time_t& last_poll_time,
                std::vector<PagerIncoming>& out_new) {
    if (own_topic.isEmpty()) return false;
    time_t now = time(nullptr);
    if (now < 1000000000L) return false;  // NTP not synced yet

    WiFiClientSecure wc;
    wc.setInsecure();
    HTTPClient http;
    http.setTimeout(TIMEOUT_MS);
    http.setReuse(false);

    String url = String(NTFY_BASE) + "/" + own_topic + "/json?poll=1&since=";
    url += String((long)(last_poll_time > 0 ? last_poll_time : now));

    if (!http.begin(wc, url)) return false;
    int code = http.GET();
    if (code != 200) { http.end(); return false; }

    String body = http.getString();
    http.end();
    last_poll_time = now;
    if (body.isEmpty()) return true;

    // Parse NDJSON — one JSON object per line
    int start = 0;
    while (start < (int)body.length()) {
        int end = body.indexOf('\n', start);
        if (end < 0) end = body.length();
        String line = body.substring(start, end);
        line.trim();
        start = end + 1;
        if (line.isEmpty()) continue;

        JsonDocument doc;
        if (deserializeJson(doc, line)) continue;
        if (strcmp(doc["event"] | "", "message") != 0) continue;
        const char* msg = doc["message"] | (const char*)nullptr;
        if (!msg || !*msg) continue;

        PagerIncoming inc;
        inc.received_at  = (time_t)(doc["time"] | (long)now);
        const char* title = doc["title"] | (const char*)nullptr;
        if (title && *title) inc.sender_name = String(title);
        String full(msg);
        int nl = full.indexOf('\n');
        if (nl >= 0) {
            inc.display_text = full.substring(0, nl);
            String rest      = full.substring(nl + 1);
            // Parse key=value metadata lines
            int ls = 0;
            while (ls < (int)rest.length()) {
                int le = rest.indexOf('\n', ls);
                if (le < 0) le = rest.length();
                String line = rest.substring(ls, le);
                line.trim();
                if (line.startsWith("reply=")) {
                    inc.reply_topic = line.substring(6);
                    inc.reply_topic.trim();
                } else if (line.startsWith("re=")) {
                    inc.re_text = line.substring(3);
                    inc.re_text.trim();
                }
                ls = le + 1;
            }
        } else {
            inc.display_text = full;
        }
        if (!inc.display_text.isEmpty()) out_new.push_back(inc);
    }
    return true;
}
