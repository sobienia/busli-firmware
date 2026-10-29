// ─────────────────────────────────────────────────────────────────────────────
// wifi_conn.cpp — Wi-Fi connect/reconnect and clock sync
// ─────────────────────────────────────────────────────────────────────────────

#include "app.h"

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  WIFI — try NVS credentials first, fall back to secrets.h                 ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

// Saved networks (NVS) followed by the secrets.h fallback. Returns the count.
static const int MAX_WIFI_CREDS = 4;
static int load_wifi_creds(String ssids[], String passes[], String users[]) {
    int n = config_load_wifi(ssids, passes, users);   // up to 3
    ssids[n] = WIFI_SSID; passes[n] = WIFI_PASSWORD; users[n] = "";
    return n + 1;
}

// Start connecting to one network without waiting. user != "" → WPA2-Enterprise.
static void start_connect(const String& ssid, const String& pass, const String& user) {
    if (user.length() > 0) {
        Log.printf("[WiFi] Trying enterprise '%s' user='%s'...\n", ssid.c_str(), user.c_str());
        esp_wifi_sta_wpa2_ent_set_identity((uint8_t*)user.c_str(), user.length());
        esp_wifi_sta_wpa2_ent_set_username((uint8_t*)user.c_str(), user.length());
        esp_wifi_sta_wpa2_ent_set_password((uint8_t*)pass.c_str(), pass.length());
        esp_wifi_sta_wpa2_ent_enable();
        WiFi.begin(ssid.c_str());
    } else {
        Log.printf("[WiFi] Trying '%s'...\n", ssid.c_str());
        WiFi.begin(ssid.c_str(), pass.c_str());
    }
}

// Blocking connect, used at boot only (the boot animation is running meanwhile)
static bool try_connect(const String& ssid, const String& pass,
                        const String& user = "", uint32_t timeout_ms = 8000) {
    if (ssid.isEmpty()) return false;
    start_connect(ssid, pass, user);
    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < timeout_ms) {
        delay(300);
        Log.print(".");
    }
    Log.println();
    if (user.length() > 0) esp_wifi_sta_wpa2_ent_disable();
    if (WiFi.status() == WL_CONNECTED) {
        Log.printf("[WiFi] Connected, IP: %s\n", WiFi.localIP().toString().c_str());
        return true;
    }
    WiFi.disconnect(true);
    return false;
}

bool connect_wifi() {
    WiFi.mode(WIFI_STA);
    String ssids[MAX_WIFI_CREDS], passes[MAX_WIFI_CREDS], users[MAX_WIFI_CREDS];
    int n = load_wifi_creds(ssids, passes, users);
    for (int i = 0; i < n; i++)
        if (try_connect(ssids[i], passes[i], users[i])) return true;
    Log.println("[WiFi] All credentials failed");
    return false;
}

// ── Reconnect without freezing the UI ────────────────────────────────────────
// When the link drops, saved networks are tried one at a time: start an attempt,
// return to loop(), check back on later iterations, move on after
// WIFI_ATTEMPT_MS. A full round is retried every WIFI_RETRY_MS.
static const uint32_t WIFI_RETRY_MS   = 30000;
static const uint32_t WIFI_ATTEMPT_MS = 10000;
static String   g_wifi_ssids[MAX_WIFI_CREDS], g_wifi_passes[MAX_WIFI_CREDS], g_wifi_users[MAX_WIFI_CREDS];
static int      g_wifi_n         = 0;
static int      g_wifi_try       = -1;   // index being tried; -1 = idle
static uint32_t g_wifi_try_ms    = 0;
static uint32_t g_wifi_round_ms  = 0;
static bool     g_web_started    = false;

static void wifi_end_attempt() {
    if (g_wifi_try >= 0 && g_wifi_users[g_wifi_try].length() > 0) esp_wifi_sta_wpa2_ent_disable();
}

void wifi_reconnect_tick() {
    uint32_t now = millis();
    if (WiFi.status() == WL_CONNECTED) {
        if (g_wifi_try >= 0) {
            wifi_end_attempt();
            g_wifi_try = -1;
            apply_wifi_power_save();
            configTzTime(POSIX_TZ, "pool.ntp.org", "time.nist.gov");
            Log.printf("[WiFi] Reconnected, IP: %s\n", WiFi.localIP().toString().c_str());
        }
        if (!g_web_started) { config_web_begin(); g_web_started = true; }
        return;
    }
    if (g_wifi_try < 0) {
        if (now - g_wifi_round_ms < WIFI_RETRY_MS) return;
        g_wifi_round_ms = now;
        g_wifi_n = load_wifi_creds(g_wifi_ssids, g_wifi_passes, g_wifi_users);
        Log.println("[WiFi] Disconnected — reconnecting in the background");
        WiFi.disconnect();
        g_wifi_try = 0;
    } else if (now - g_wifi_try_ms < WIFI_ATTEMPT_MS) {
        return;                               // current attempt still running
    } else {
        wifi_end_attempt();
        WiFi.disconnect();
        if (++g_wifi_try >= g_wifi_n) { g_wifi_try = -1; g_wifi_round_ms = now; return; }
    }
    while (g_wifi_try < g_wifi_n && g_wifi_ssids[g_wifi_try].isEmpty()) g_wifi_try++;
    if (g_wifi_try >= g_wifi_n) { g_wifi_try = -1; return; }
    g_wifi_try_ms = now;
    start_connect(g_wifi_ssids[g_wifi_try], g_wifi_passes[g_wifi_try], g_wifi_users[g_wifi_try]);
}

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  HELPERS                                                                  ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

void sync_clock() {
    Log.println("[NTP] Syncing time...");
    configTzTime(POSIX_TZ, "pool.ntp.org", "time.nist.gov");
    uint32_t start = millis();
    time_t now = 0;
    while (millis() - start < 10000) {
        now = time(nullptr);
        if (now > 1700000000) break;
        delay(200);
    }
    struct tm t;
    localtime_r(&now, &t);
    Log.printf("[NTP] Time: %04d-%02d-%02d %02d:%02d:%02d\n",
                  t.tm_year+1900, t.tm_mon+1, t.tm_mday,
                  t.tm_hour, t.tm_min, t.tm_sec);
}
