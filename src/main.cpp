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
#include "../include/config_portal.h"

#include "display.h"
#include "api.h"
#include "weather.h"
#include "../include/flight_tracker.h"

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  HARDCODED STOP DEFAULTS — used when NVS has no saved stops               ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

static const struct { const char* l; const char* n; const char* li; const char* di; }
DEFAULT_STOPS[] = {
    { "ETH Hönggerberg",  "ETH Hönggerberg",             "",     "Triemli,Milchbuck,Bucheggplatz" },
    { "Gasometerbrücke",  "Schlieren, Gasometerbrücke",  "2,20", ""  },
    { "Letzipark",        "Zürich, Letzipark",            "",     ""  },
    { "Letzipark West",   "Zürich, Letzipark West",       "",     ""  },
};
static const int N_DEFAULT_STOPS = sizeof(DEFAULT_STOPS) / sizeof(DEFAULT_STOPS[0]);

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  DYNAMIC STOP TABLE — built at boot from NVS or defaults                  ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

static StopEntry        g_stop_entries[8];
static std::vector<String>       g_line_strs[8];
static std::vector<String>       g_dir_strs[8];
static std::vector<const char*>  g_line_ptrs[8];
static std::vector<const char*>  g_dir_ptrs[8];
static StopConfig       g_stop_configs[8];
static int              g_num_stops = 0;

static void split_csv(const String& csv, std::vector<String>& out) {
    out.clear();
    int start = 0;
    for (int i = 0; i <= (int)csv.length(); i++) {
        if (i == (int)csv.length() || csv[i] == ',') {
            String tok = csv.substring(start, i);
            tok.trim();
            if (tok.length() > 0) out.push_back(tok);
            start = i + 1;
        }
    }
}

// Call after g_stop_entries[] and g_num_stops are set.
static void build_stop_configs() {
    for (int i = 0; i < g_num_stops; i++) {
        StopEntry& e = g_stop_entries[i];

        split_csv(e.lines_csv, g_line_strs[i]);
        g_line_ptrs[i].clear();
        for (auto& s : g_line_strs[i]) g_line_ptrs[i].push_back(s.c_str());

        split_csv(e.dirs_csv, g_dir_strs[i]);
        g_dir_ptrs[i].clear();
        for (auto& s : g_dir_strs[i]) g_dir_ptrs[i].push_back(s.c_str());

        g_stop_configs[i].label            = e.label.c_str();
        g_stop_configs[i].station          = e.station.c_str();
        g_stop_configs[i].line_filter      = g_line_ptrs[i].empty() ? nullptr : g_line_ptrs[i].data();
        g_stop_configs[i].line_count       = (int)g_line_ptrs[i].size();
        g_stop_configs[i].direction_filter = g_dir_ptrs[i].empty() ? nullptr : g_dir_ptrs[i].data();
        g_stop_configs[i].direction_count  = (int)g_dir_ptrs[i].size();
    }
}

static void setup_stops() {
    int n = config_load_stops(g_stop_entries);
    if (n > 0) {
        g_num_stops = n;
        Serial.printf("[Stops] Loaded %d stops from NVS\n", n);
    } else {
        g_num_stops = N_DEFAULT_STOPS;
        for (int i = 0; i < g_num_stops; i++) {
            g_stop_entries[i].label     = DEFAULT_STOPS[i].l;
            g_stop_entries[i].station   = DEFAULT_STOPS[i].n;
            g_stop_entries[i].lines_csv = DEFAULT_STOPS[i].li;
            g_stop_entries[i].dirs_csv  = DEFAULT_STOPS[i].di;
        }
        Serial.printf("[Stops] Using %d hardcoded defaults\n", g_num_stops);
    }
    build_stop_configs();
}

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  RUNTIME STATE                                                            ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

static int      current_stop_idx  = 0;
static bool     large_font_mode   = false;
static uint32_t last_redraw_ms    = 0;
static time_t   g_countdown_target = 0;
static int      g_countdown_icon   = 0;

// Flight tracking
enum View { VIEW_BOARD, VIEW_FLIGHT0, VIEW_FLIGHT1 };
static View     g_view            = VIEW_BOARD;
static int      g_flight_count    = 0;
static uint32_t g_last_flight_ms[2] = {0, 0};
#define FLIGHT_REFRESH_MS  (5UL * 60 * 1000)

static WeatherData weather_data    = {};
static uint32_t    last_weather_ms = 0;

// Button debounce state
static bool     button_was_pressed    = false;
static uint32_t button_press_start_ms = 0;
static bool     config_hint_shown     = false;

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  WIFI — try NVS credentials first, fall back to secrets.h                 ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

static bool try_connect(const String& ssid, const String& pass, uint32_t timeout_ms = 12000) {
    if (ssid.isEmpty()) return false;
    Serial.printf("[WiFi] Trying '%s'...\n", ssid.c_str());
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < timeout_ms) {
        delay(300);
        Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFi] Connected, IP: %s\n", WiFi.localIP().toString().c_str());
        return true;
    }
    WiFi.disconnect(true);
    return false;
}

static bool connect_wifi() {
    WiFi.mode(WIFI_STA);

    // Try NVS-stored networks first
    String ssids[3], passes[3];
    int n = config_load_wifi(ssids, passes);
    for (int i = 0; i < n; i++) {
        if (try_connect(ssids[i], passes[i])) return true;
    }

    // Fall back to hardcoded secrets.h credential
    if (try_connect(WIFI_SSID, WIFI_PASSWORD)) return true;

    Serial.println("[WiFi] All credentials failed");
    return false;
}

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  HELPERS                                                                  ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

static void sync_clock() {
    Serial.println("[NTP] Syncing time...");
    configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.nist.gov");
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
                  t.tm_year+1900, t.tm_mon+1, t.tm_mday,
                  t.tm_hour, t.tm_min, t.tm_sec);
}

static void switch_stop(int new_idx) {
    g_view           = VIEW_BOARD;
    current_stop_idx = new_idx;
    fetch_task_set_active_stop(new_idx);
    display_invalidate();
}

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  INPUT HANDLERS                                                           ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

static void check_button() {
    bool pressed = (digitalRead(PIN_BOOT_BUTTON) == LOW);

    if (pressed && !button_was_pressed) {
        button_press_start_ms = millis();
        config_hint_shown     = false;
    } else if (pressed && button_was_pressed) {
        // Still held — show hint when approaching config threshold
        uint32_t held_ms = millis() - button_press_start_ms;
        if (held_ms >= 1000 && !config_hint_shown) {
            config_hint_shown = true;
            display_show_status("Hold to configure...");
        }
    } else if (!pressed && button_was_pressed) {
        uint32_t held_ms = millis() - button_press_start_ms;
        config_hint_shown = false;
        if (held_ms >= 3000) {
            Serial.println("[Button] 3s hold → config portal");
            config_portal_run(PORTAL_TIMEOUT_MS);  // reboots on save; returns on timeout
        } else if (held_ms >= TOUCH_LONG_PRESS_MS) {
            Serial.println("[Button] Long press → force refresh");
            fetch_task_force_refresh();
        } else if (held_ms >= 50) {
            Serial.println("[Button] Short press → next stop");
            switch_stop((current_stop_idx + 1) % g_num_stops);
        }
    }
    button_was_pressed = pressed;
}

static void check_touch() {
    switch (touch_poll()) {
        case TOUCH_SWIPE_LEFT:
            Serial.println("[Touch] Swipe left → next stop");
            switch_stop((current_stop_idx + 1) % g_num_stops);
            break;
        case TOUCH_SWIPE_RIGHT:
            Serial.println("[Touch] Swipe right → prev stop");
            switch_stop((current_stop_idx - 1 + g_num_stops) % g_num_stops);
            break;
        case TOUCH_LONG_PRESS:
            Serial.println("[Touch] Long press → force refresh");
            if (g_view == VIEW_BOARD) {
                fetch_task_force_refresh();
            } else {
                int slot = (g_view == VIEW_FLIGHT1) ? 1 : 0;
                flight_tracker_refresh(slot);
                g_last_flight_ms[slot] = millis();
                display_invalidate();
            }
            break;
        case TOUCH_DOUBLE_TAP:
            Serial.println("[Touch] Double tap → toggle font size");
            large_font_mode = !large_font_mode;
            display_invalidate();
            break;
        case TOUCH_SWIPE_DOWN:
            // Cycle forward: BOARD → FLIGHT0 → FLIGHT1 → BOARD
            if (g_flight_count > 0) {
                View nv;
                if      (g_view == VIEW_BOARD)   nv = VIEW_FLIGHT0;
                else if (g_view == VIEW_FLIGHT0)  nv = (g_flight_count > 1) ? VIEW_FLIGHT1 : VIEW_BOARD;
                else                              nv = VIEW_BOARD;
                Serial.printf("[Touch] Swipe down → view %d\n", (int)nv);
                if (nv != g_view) { g_view = nv; display_invalidate(); }
            }
            break;
        case TOUCH_SWIPE_UP:
            // Cycle backward: BOARD → FLIGHT1 → FLIGHT0 → BOARD
            if (g_flight_count > 0) {
                View nv;
                if      (g_view == VIEW_BOARD)  nv = (g_flight_count > 1) ? VIEW_FLIGHT1 : VIEW_FLIGHT0;
                else if (g_view == VIEW_FLIGHT1) nv = VIEW_FLIGHT0;
                else                             nv = VIEW_BOARD;
                Serial.printf("[Touch] Swipe up → view %d\n", (int)nv);
                if (nv != g_view) { g_view = nv; display_invalidate(); }
            }
            break;
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
    Serial.println("  Busli — T-Display S3 Pro");
    Serial.println("============================================");

    pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);

    display_init();
    touch_init();
    display_boot_animation_start();

    setup_stops();

    if (!connect_wifi()) {
        while (WiFi.status() != WL_CONNECTED) {
            delay(30000);
            connect_wifi();
        }
    }

    sync_clock();

    {
        String cd_label, cd_target_str;
        if (config_load_countdown(cd_label, cd_target_str, g_countdown_icon) && cd_target_str.length() >= 16) {
            // configTzTime was called in sync_clock() so mktime() now uses Swiss local time
            struct tm t = {};
            t.tm_year  = cd_target_str.substring(0,  4).toInt() - 1900;
            t.tm_mon   = cd_target_str.substring(5,  7).toInt() - 1;
            t.tm_mday  = cd_target_str.substring(8,  10).toInt();
            t.tm_hour  = cd_target_str.substring(11, 13).toInt();
            t.tm_min   = cd_target_str.substring(14, 16).toInt();
            t.tm_isdst = -1;
            g_countdown_target = mktime(&t);
            Serial.printf("[Countdown] '%s' → %s → epoch %ld\n",
                          cd_label.c_str(), cd_target_str.c_str(), (long)g_countdown_target);
        }
    }

    fetch_task_start(g_stop_configs, g_num_stops);

    {
        FlightEntry fl_entries[2];
        g_flight_count = config_load_flights(fl_entries);
        if (g_flight_count > 0) {
            Serial.printf("[Flights] %d flight(s) configured\n", g_flight_count);
            flight_tracker_init(fl_entries, g_flight_count);
            for (int i = 0; i < g_flight_count; i++)
                g_last_flight_ms[i] = millis();
        }
    }

    weather_fetch(weather_data);
    last_weather_ms = millis();

    display_boot_animation_stop();  // wipes screen, then returns
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

    // Background flight data refresh (every 5 min per slot, non-blocking check)
    if (g_flight_count > 0) {
        for (int i = 0; i < g_flight_count; i++) {
            if (now_ms - g_last_flight_ms[i] >= FLIGHT_REFRESH_MS) {
                flight_tracker_refresh(i);
                g_last_flight_ms[i] = millis();
                if (g_view == VIEW_FLIGHT0 && i == 0) display_invalidate();
                if (g_view == VIEW_FLIGHT1 && i == 1) display_invalidate();
                break;  // refresh one slot per loop to avoid long blocking back-to-back
            }
        }
    }

    // Redraw every second (clock + age counter)
    if (now_ms - last_redraw_ms >= 1000) {
        last_redraw_ms = now_ms;

        if (g_view == VIEW_BOARD) {
            std::vector<Departure> departures;
            time_t fetch_time;
            bool   from_cache;
            fetch_task_get(current_stop_idx, departures, fetch_time, from_cache);

            const StopConfig& stop = g_stop_configs[current_stop_idx];
            time_t now_t  = time(nullptr);
            int    age_s  = (fetch_time > 0) ? (int)(now_t - fetch_time) : 0;

            char weather_str[20] = "";
            char uv_str[10]      = "";
            if (weather_data.valid) {
                snprintf(weather_str, sizeof(weather_str), "%dC/%dC",
                         (int)roundf(weather_data.temp_c),
                         (int)roundf(weather_data.temp_max_c));
                snprintf(uv_str, sizeof(uv_str), "UV%d/%d",
                         weather_data.uv_index, weather_data.uv_index_max);
            }

            display_draw_board(
                stop.label,
                current_stop_idx,
                g_num_stops,
                departures,
                weather_str,
                uv_str,
                weather_data.valid && weather_data.rain_today,
                weather_data.valid ? weather_data.precip_prob_pct : 0,
                from_cache,
                WiFi.status() == WL_CONNECTED,
                age_s,
                fetch_time,
                large_font_mode,
                g_countdown_target,
                g_countdown_icon
            );
        } else {
            int slot = (g_view == VIEW_FLIGHT1) ? 1 : 0;
            FlightInfo fi;
            flight_tracker_get(slot, fi);
            display_draw_flight(slot, g_flight_count, fi, WiFi.status() == WL_CONNECTED);
        }
    }

    // Reconnect WiFi if dropped — 30s interval avoids AUTH_LEAVE floods
    static uint32_t last_wifi_check = 0;
    if (now_ms - last_wifi_check > 30000) {
        last_wifi_check = now_ms;
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[WiFi] Disconnected — reconnecting...");
            WiFi.disconnect();
            delay(100);
            if (connect_wifi()) {
                // Re-arm SNTP after reconnect so clock drift is corrected
                configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.nist.gov");
            }
        }
    }

    delay(20);
}
