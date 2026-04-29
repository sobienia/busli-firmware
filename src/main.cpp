// ─────────────────────────────────────────────────────────────────────────────
// main.cpp — Tramli Phase 1 entry point
// ─────────────────────────────────────────────────────────────────────────────
// What this does:
//   1. Initialize the display
//   2. Show boot animation
//   3. Connect to your WiFi
//   4. Sync the clock from the internet
//   5. Loop forever:
//      - Every 30 seconds: fetch fresh departures from the SBB API
//      - Redraw the screen
//      - Watch for the BOOT button to switch between stops
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include "../include/config.h"
#include "../include/secrets.h"

#include "display.h"
#include "api.h"
#include "weather.h"

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  STOPS — edit these to change which stops the device shows                ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
// In Phase 2 we'll move this into a web-editable config. For now it's
// hardcoded here.

// ── Per-stop filter arrays ────────────────────────────────────────────────────
// Direction filter: show only departures whose destination CONTAINS one of
//   these substrings (case-insensitive). Leave as nullptr for all directions.
// Line filter: show only these exact line numbers. Leave as nullptr for all.

static const char* eth_directions[]  = { "Triemli", "Milchbuck", "Bucheggplatz" };
static const char* schlieren_lines[] = { "2", "20" };

static const StopConfig STOPS[] = {
    {
        // Buses 69 + 80 toward the city only (Triemli / Milchbuck direction).
        // Oerlikon-bound departures are filtered out.
        .label            = "ETH Hönggerberg",
        .station          = "ETH Hönggerberg",
        .direction_filter = eth_directions,
        .direction_count  = 3,
        .line_filter      = nullptr,
        .line_count       = 0,
    },
    {
        // Lines 2 and 20 only — the trams that serve this stop.
        .label            = "Gasometerbrücke",
        .station          = "Schlieren, Gasometerbrücke",
        .direction_filter = nullptr,
        .direction_count  = 0,
        .line_filter      = schlieren_lines,
        .line_count       = 2,
    },
};
static const int NUM_STOPS = sizeof(STOPS) / sizeof(STOPS[0]);

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  RUNTIME STATE                                                            ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

static int current_stop_idx = 0;
static std::vector<Departure> last_departures;
static uint32_t last_fetch_ms = 0;
static uint32_t last_redraw_ms = 0;
static time_t   last_successful_fetch_time = 0;
static bool     from_cache = false;

static WeatherData weather_data = {};
static uint32_t    last_weather_ms = 0;

// Button debouncing state
static bool     button_was_pressed = false;
static uint32_t button_press_start_ms = 0;
static const uint32_t LONG_PRESS_MS = 2000;

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  HELPERS                                                                  ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

static bool connect_wifi() {
    Serial.printf("[WiFi] Connecting to '%s'...\n", WIFI_SSID);
    display_show_status("Connecting...");

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Connection FAILED");
        display_show_status("WiFi failed");
        return false;
    }

    Serial.print("[WiFi] Connected, IP: ");
    Serial.println(WiFi.localIP());
    return true;
}

static void sync_clock() {
    Serial.println("[NTP] Syncing time...");
    display_show_status("Syncing time...");

    // Switzerland uses CET/CEST — configTime handles DST automatically
    // when given the timezone string.
    configTzTime("CET-1CEST,M3.5.0,M10.5.0/3",
                 "pool.ntp.org", "time.nist.gov");

    // Wait up to 10 seconds for NTP sync
    uint32_t start = millis();
    time_t now = 0;
    while (millis() - start < 10000) {
        now = time(nullptr);
        if (now > 1700000000) break;     // Jan 2024 sanity check
        delay(200);
    }

    struct tm t;
    localtime_r(&now, &t);
    Serial.printf("[NTP] Time: %04d-%02d-%02d %02d:%02d:%02d\n",
                  t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                  t.tm_hour, t.tm_min, t.tm_sec);
}

static void fetch_now() {
    const StopConfig& stop = STOPS[current_stop_idx];
    Serial.printf("[Fetch] Stop %d: %s\n", current_stop_idx, stop.label);

    std::vector<Departure> fresh;
    bool ok = api_fetch_departures(stop, fresh, 6);

    if (ok) {
        last_departures = fresh;
        last_successful_fetch_time = time(nullptr);
        from_cache = false;
    } else {
        // Keep showing what we had — just mark it stale
        from_cache = true;
        Serial.println("[Fetch] FAILED — keeping last data");
    }
    last_fetch_ms = millis();
}

static void check_button() {
    bool pressed = (digitalRead(PIN_BOOT_BUTTON) == LOW);

    if (pressed && !button_was_pressed) {
        // Just pressed
        button_press_start_ms = millis();
    } else if (!pressed && button_was_pressed) {
        // Just released — was it a short or long press?
        uint32_t held_ms = millis() - button_press_start_ms;
        if (held_ms >= LONG_PRESS_MS) {
            Serial.println("[Button] Long press → force refresh");
            fetch_now();
        } else if (held_ms >= 50) {       // basic debounce
            Serial.println("[Button] Short press → next stop");
            current_stop_idx = (current_stop_idx + 1) % NUM_STOPS;
            // Clear the screen and force an immediate fetch
            display_show_status("Loading...");
            fetch_now();
        }
    }
    button_was_pressed = pressed;
}

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  ARDUINO ENTRY POINTS                                                     ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

void setup() {
    Serial.begin(115200);
    delay(500);   // give the USB CDC time to come up
    Serial.println();
    Serial.println("============================================");
    Serial.println("  Tramli Phase 1 — T-Display S3 Pro");
    Serial.println("============================================");

    pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);

    display_init();
    display_boot_animation(2500);

    if (!connect_wifi()) {
        // Stay here showing the failure message and try again every 30s
        while (WiFi.status() != WL_CONNECTED) {
            delay(30000);
            connect_wifi();
        }
    }

    sync_clock();

    display_show_status("Loading weather...");
    weather_fetch(weather_data);
    last_weather_ms = millis();

    // First fetch
    display_show_status("Loading departures...");
    fetch_now();
}

void loop() {
    check_button();

    uint32_t now_ms = millis();

    // Fetch fresh data every REFRESH_INTERVAL_SEC
    if (now_ms - last_fetch_ms >= REFRESH_INTERVAL_SEC * 1000UL) {
        fetch_now();
    }

    // Refresh weather every WEATHER_REFRESH_SEC
    if (now_ms - last_weather_ms >= WEATHER_REFRESH_SEC * 1000UL) {
        weather_fetch(weather_data);
        last_weather_ms = now_ms;
    }

    // Redraw screen every second so the clock and refresh-age update
    if (now_ms - last_redraw_ms >= 1000) {
        last_redraw_ms = now_ms;

        const StopConfig& stop = STOPS[current_stop_idx];
        time_t now = time(nullptr);
        int age_s = last_successful_fetch_time > 0
            ? (int)(now - last_successful_fetch_time)
            : 0;

        char weather_str[20] = "";
        char uv_str[10]      = "";
        if (weather_data.valid) {
            snprintf(weather_str, sizeof(weather_str), "%dC/%dC",
                     (int)roundf(weather_data.temp_c),
                     (int)roundf(weather_data.temp_max_c));
            snprintf(uv_str, sizeof(uv_str), "UV%d/%d",
                     weather_data.uv_index,
                     weather_data.uv_index_max);
        }

        display_draw_board(
            stop.label,
            current_stop_idx,
            NUM_STOPS,
            last_departures,
            weather_str,
            uv_str,
            weather_data.valid && weather_data.rain_today,
            weather_data.valid ? weather_data.precip_prob_pct : 0,
            from_cache,
            WiFi.status() == WL_CONNECTED,
            age_s,
            last_successful_fetch_time
        );
    }

    // Watch WiFi state — try to reconnect if it dropped.
    // 30 s interval avoids AUTH_LEAVE floods from aggressive reconnect calls.
    static uint32_t last_wifi_check = 0;
    if (now_ms - last_wifi_check > 30000) {
        last_wifi_check = now_ms;
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[WiFi] Disconnected — reconnecting...");
            WiFi.disconnect();
            delay(100);
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        }
    }

    delay(20);   // small yield to keep the system happy
}
