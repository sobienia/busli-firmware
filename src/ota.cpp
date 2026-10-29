// ─────────────────────────────────────────────────────────────────────────────
// ota.cpp — Pull-based OTA firmware update
// ─────────────────────────────────────────────────────────────────────────────
// The device fetches a small version.json at OTA_VERSION_URL hourly.
// If the remote version is newer than FIRMWARE_VERSION, it downloads the
// binary URL from that JSON and flashes it via the ESP32 OTA partition.
// All networking uses WiFiClientSecure + the global http_lock. Certificates ARE
// verified (against include/ca_certs.h): a hostile network could otherwise serve
// a fake version.json and firmware. If GitHub ever moves to a CA that isn't in
// the bundle, checks fail safely (no update) — regenerate ca_certs.h with
// scripts/gen_ca_certs.py, or flash manually via the portal's Firmware page.
// ─────────────────────────────────────────────────────────────────────────────

#include "../include/ota.h"
#include "../include/devlog.h"
#include "../include/config.h"
#include "../include/http_lock.h"
#include "../include/ca_certs.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <Preferences.h>
#include <esp_ota_ops.h>

volatile bool g_ota_pending = false;
String        g_ota_url;
String        g_ota_remote_version;

// Compare "major.minor.patch" — returns true if a is strictly newer than b.
static bool is_newer(const char* a, const char* b) {
    int a0 = 0, a1 = 0, a2 = 0, b0 = 0, b1 = 0, b2 = 0;
    sscanf(a, "%d.%d.%d", &a0, &a1, &a2);
    sscanf(b, "%d.%d.%d", &b0, &b1, &b2);
    if (a0 != b0) return a0 > b0;
    if (a1 != b1) return a1 > b1;
    return a2 > b2;
}

bool ota_check(String& out_url) {
    if (strlen(OTA_VERSION_URL) == 0) return false;

    Log.printf("[OTA] Checking for updates (current: %s)\n", FIRMWARE_VERSION);
    if (time(nullptr) < 1700000000) {   // certificate dates can't be checked yet
        Log.println("[OTA] Clock not synced — skipping check");
        return false;
    }

    WiFiClientSecure wc;
    wc.setCACert(OTA_ROOT_CAS);
    HTTPClient http;
    http.setTimeout(10000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    http_lock_take();
    if (!http.begin(wc, OTA_VERSION_URL)) {
        http_lock_give();
        Log.println("[OTA] version check: http.begin failed");
        return false;
    }
    int code = http.GET();
    String body = (code == 200) ? http.getString() : "";
    http.end();
    http_lock_give();

    if (code != 200) { Log.printf("[OTA] HTTP %d\n", code); return false; }

    JsonDocument doc;
    if (deserializeJson(doc, body)) { Log.println("[OTA] JSON parse error"); return false; }

    const char* remote_ver = doc["version"] | "";
    const char* url        = doc["url"]     | "";

    Log.printf("[OTA] Remote: %s  Current: %s\n", remote_ver, FIRMWARE_VERSION);

    if (strlen(url) == 0 || !is_newer(remote_ver, FIRMWARE_VERSION)) return false;
    if (ota_skipped_version() == remote_ver) {
        Log.printf("[OTA] v%s failed on this device before — not offering it again\n", remote_ver);
        return false;
    }

    out_url              = url;
    g_ota_remote_version = remote_ver;
    Log.printf("[OTA] Update available: %s -> %s\n", FIRMWARE_VERSION, remote_ver);
    return true;
}

bool ota_apply(const String& url, void (*progress_cb)(int percent)) {
    Log.printf("[OTA] Downloading firmware: %s\n", url.c_str());

    WiFiClientSecure wc;
    wc.setCACert(OTA_ROOT_CAS);
    HTTPClient http;
    // setTimeout() takes a uint16_t (max 65535 ms) — it's a per-read timeout, not a
    // total. The overall stall guard is OTA_STALL_MS in the download loop below.
    http.setTimeout(30000);
    http.setConnectTimeout(15000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    http_lock_take();
    if (!http.begin(wc, url)) {
        http_lock_give();
        Log.println("[OTA] download http.begin failed");
        return false;
    }
    int code = http.GET();
    if (code != 200) {
        http.end(); http_lock_give();
        Log.printf("[OTA] download HTTP %d\n", code);
        return false;
    }

    int total = http.getSize();
    Log.printf("[OTA] Firmware: %d bytes\n", total);
    if (total <= 0) {
        http.end(); http_lock_give();
        Log.println("[OTA] No content-length — cannot stream");
        return false;
    }

    if (!Update.begin(total)) {
        http.end(); http_lock_give();
        Log.printf("[OTA] Update.begin failed: %s\n", Update.errorString());
        return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    if (!stream) {
        Update.abort();
        http.end(); http_lock_give();
        Log.println("[OTA] No stream");
        return false;
    }

    // Abort if no data arrives for this long. Resets on every chunk, so a slow
    // but steady connection can take as long as it needs.
    const uint32_t OTA_STALL_MS = 30000;
    uint8_t  buf[1024];
    int      written = 0;
    int      last_reported_pct = -1;
    int      last_logged_kb    = 0;
    uint32_t last_data_ms = millis();
    bool     write_failed = false;

    if (progress_cb) progress_cb(0);

    while (written < total && millis() - last_data_ms < OTA_STALL_MS) {
        int av = stream->available();
        if (av <= 0) {
            if (!stream->connected()) break;
            delay(1);
            continue;
        }
        int n = stream->readBytes(buf, min(av, (int)sizeof(buf)));
        if (n <= 0) continue;
        if (Update.write(buf, n) != (size_t)n) {
            Log.printf("[OTA] Update.write failed: %s\n", Update.errorString());
            write_failed = true;
            break;
        }
        written     += n;
        last_data_ms = millis();
        int pct = (int)((int64_t)written * 100 / total);
        if (progress_cb && pct != last_reported_pct) {
            last_reported_pct = pct;
            progress_cb(pct);
        }
        if (written / 65536 != last_logged_kb) {
            last_logged_kb = written / 65536;
            Log.printf("[OTA] %d / %d bytes (%d%%)\n", written, total, pct);
        }
    }

    http.end();
    http_lock_give();

    // Never finalize a partial image — Update.end(true) would try to boot it.
    if (write_failed || written < total) {
        Log.printf("[OTA] Download incomplete (%d / %d bytes) — aborting\n", written, total);
        Update.abort();
        return false;
    }
    if (!Update.end()) {
        Log.printf("[OTA] Update.end failed: %s\n", Update.errorString());
        return false;
    }
    Log.printf("[OTA] Flash complete (%d bytes written)\n", written);
    return true;
}

// ── Safety net (see ota.h) ───────────────────────────────────────────────────
// The bootloader is built with rollback support: a freshly installed app starts
// as "pending verify", and if it restarts in that state for any reason the
// previous app is booted instead. NVS "ota": "trial" = version on trial,
// "skip" = version that failed.

static const uint32_t OTA_TRIAL_MIN_MS = 60000;    // runs this long with data → good
static const uint32_t OTA_TRIAL_MAX_MS = 300000;   // decide by now at the latest
static bool s_trial = false;

// Arduino core hook: don't confirm a new app automatically at startup
extern "C" bool verifyRollbackLater() { return true; }

static bool running_pending() {
    esp_ota_img_states_t st;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
           st == ESP_OTA_IMG_PENDING_VERIFY;
}

static String nvs_get(const char* key) {
    Preferences p;
    p.begin("ota", true);
    String v = p.isKey(key) ? p.getString(key, "") : "";
    p.end();
    return v;
}

static void nvs_put(const char* key, const String& v) {
    Preferences p;
    p.begin("ota", false);
    if (v.isEmpty()) p.remove(key); else p.putString(key, v);
    p.end();
}

static void confirm(const char* why) {
    esp_ota_mark_app_valid_cancel_rollback();
    s_trial = false;
    nvs_put("trial", "");
    Log.printf("[OTA] v%s confirmed (%s)\n", FIRMWARE_VERSION, why);
}

// A planned restart during the trial (settings saved, …) means it was working
static void on_shutdown() {
    if (s_trial && running_pending()) esp_ota_mark_app_valid_cancel_rollback();
}

String ota_boot_check() {
    String trial = nvs_get("trial");
    if (running_pending()) {
        s_trial = true;
        if (trial != FIRMWARE_VERSION) nvs_put("trial", FIRMWARE_VERSION);
        esp_register_shutdown_handler(on_shutdown);
        Log.printf("[OTA] v%s is new — on trial until it has worked for a minute\n", FIRMWARE_VERSION);
        return "";
    }
    if (trial.isEmpty() || trial == FIRMWARE_VERSION) {
        if (!trial.isEmpty()) nvs_put("trial", "");
        return "";
    }
    // Another version was on trial, but this one is running: it was rolled back
    nvs_put("trial", "");
    nvs_put("skip", trial);
    Log.printf("[OTA] Update to v%s failed — back on v%s\n", trial.c_str(), FIRMWARE_VERSION);
    return trial;
}

bool ota_on_trial() { return s_trial; }

void ota_trial_tick(bool data_ok, bool wifi_ok) {
    if (!s_trial) return;
    uint32_t up = millis();
    if (up >= OTA_TRIAL_MIN_MS && data_ok) { confirm("data loaded"); return; }
    if (up < OTA_TRIAL_MAX_MS) return;
    if (!wifi_ok) { confirm("ran without Wi-Fi, no crash"); return; }
    Log.printf("[OTA] v%s loaded no data in %lu s although online — rolling back\n",
               FIRMWARE_VERSION, (unsigned long)(OTA_TRIAL_MAX_MS / 1000));
    delay(200);   // let the log line out
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

String ota_skipped_version() { return nvs_get("skip"); }
