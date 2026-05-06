// ─────────────────────────────────────────────────────────────────────────────
// main.cpp — Tramli entry point
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include "../include/config.h"
#include "../include/secrets.h"
#include "../include/fetch_task.h"
#include "../include/touch_handler.h"

#include "display.h"
#include "api.h"
#include "weather.h"

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  STOPS — edit these to change which stops the device shows                ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

static const char* eth_directions[]  = { "Triemli", "Milchbuck", "Bucheggplatz" };
static const char* schlieren_lines[] = { "2", "20" };

static const StopConfig STOPS[] = {
    {
        .label            = "ETH Hönggerberg",
        .station          = "ETH Hönggerberg",
        .direction_filter = eth_directions,
        .direction_count  = 3,
        .line_filter      = nullptr,
        .line_count       = 0,
    },
    {
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

static int      current_stop_idx = 0;
static bool     large_font_mode  = false;
static uint32_t last_redraw_ms   = 0;

static WeatherData weather_data    = {};
static uint32_t    last_weather_ms = 0;

// Button debouncing state
static bool     button_was_pressed   = false;
static uint32_t button_press_start_ms = 0;

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

    configTzTime("CET-1CEST,M3.5.0,M10.5.0/3",
                 "pool.ntp.org", "time.nist.gov");

    uint32_t start = millis();
    time_t now = 0;
    while (millis() - start < 10000) {
        now = time(nullptr);
        if (now > 1700000000) break;
        delay(200);
    }

    struct tm t;
    localtime_r(&now, &t);
    Serial.printf("[NTP] Time: %04d-%02d-%02d %02d:%02d:%02d\n",
                  t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                  t.tm_hour, t.tm_min, t.tm_sec);
}

static void switch_stop(int new_idx) {
    current_stop_idx = new_idx;
    fetch_task_set_active_stop(new_idx);
    display_invalidate();
}

static void check_button() {
    bool pressed = (digitalRead(PIN_BOOT_BUTTON) == LOW);

    if (pressed && !button_was_pressed) {
        button_press_start_ms = millis();
    } else if (!pressed && button_was_pressed) {
        uint32_t held_ms = millis() - button_press_start_ms;
        if (held_ms >= TOUCH_LONG_PRESS_MS) {
            Serial.println("[Button] Long press → force refresh");
            fetch_task_force_refresh();
        } else if (held_ms >= 50) {
            Serial.println("[Button] Short press → next stop");
            switch_stop((current_stop_idx + 1) % NUM_STOPS);
        }
    }
    button_was_pressed = pressed;
}

static void check_touch() {
    switch (touch_poll()) {
        case TOUCH_SWIPE_LEFT:
        case TOUCH_SWIPE_RIGHT:
            Serial.println("[Touch] Swipe → next stop");
            switch_stop((current_stop_idx + 1) % NUM_STOPS);
            break;
        case TOUCH_LONG_PRESS:
            Serial.println("[Touch] Long press → force refresh");
            fetch_task_force_refresh();
            break;
        case TOUCH_DOUBLE_TAP:
            Serial.println("[Touch] Double tap → toggle font size");
            large_font_mode = !large_font_mode;
            display_invalidate();
            break;
        case TOUCH_SWIPE_UP:
        case TOUCH_SWIPE_DOWN:
            break;   // reserved for future use
        default:
            break;
    }
}

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  ARDUINO ENTRY POINTS                                                     ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println("============================================");
    Serial.println("  Tramli — T-Display S3 Pro");
    Serial.println("============================================");

    pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);

    display_init();
    touch_init();
    display_boot_animation(2500);

    if (!connect_wifi()) {
        while (WiFi.status() != WL_CONNECTED) {
            delay(30000);
            connect_wifi();
        }
    }

    sync_clock();

    display_show_status("Loading weather...");
    weather_fetch(weather_data);
    last_weather_ms = millis();

    display_show_status("Loading departures...");
    fetch_task_start(STOPS, NUM_STOPS);
}

void loop() {
    check_button();
    check_touch();

    uint32_t now_ms = millis();

    // Refresh weather every WEATHER_REFRESH_SEC
    if (now_ms - last_weather_ms >= WEATHER_REFRESH_SEC * 1000UL) {
        weather_fetch(weather_data);
        last_weather_ms = now_ms;
    }

    // Redraw screen every second so the clock and age counter update
    if (now_ms - last_redraw_ms >= 1000) {
        last_redraw_ms = now_ms;

        std::vector<Departure> departures;
        time_t fetch_time;
        bool   from_cache;
        fetch_task_get(current_stop_idx, departures, fetch_time, from_cache);

        const StopConfig& stop = STOPS[current_stop_idx];
        time_t now = time(nullptr);
        int age_s = (fetch_time > 0) ? (int)(now - fetch_time) : 0;

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
            departures,
            weather_str,
            uv_str,
            weather_data.valid && weather_data.rain_today,
            weather_data.valid ? weather_data.precip_prob_pct : 0,
            from_cache,
            WiFi.status() == WL_CONNECTED,
            age_s,
            fetch_time,
            large_font_mode
        );
    }

    // Reconnect WiFi if dropped — 30s interval avoids AUTH_LEAVE floods
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

    delay(20);
}
