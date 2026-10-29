// ─────────────────────────────────────────────────────────────────────────────
// main.cpp — Tramli entry point
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wpa2.h>
#include <Preferences.h>
#include <time.h>
#include <Wire.h>

#include "../include/config.h"
#include "../include/secrets.h"
#include "../include/http_lock.h"
#include "../include/fetch_task.h"
#include "../include/ota.h"
#include "../include/touch_handler.h"
#include "../include/config_portal.h"
#include "../include/commute.h"
#include "../include/pager.h"
#include "../include/stocks.h"
#include "../include/battery.h"

#include "display.h"
#include "api.h"
#include "weather.h"
#include "../include/flight_tracker.h"
#include <esp_wifi.h>

// loop() makes HTTPS calls (pager send, config-portal Check/Test buttons). A TLS
// handshake needs more stack than the default 8 KB loopTask — overflowing it
// crashes with "Double exception". Measured: a portal stock Check uses ~9 KB.
// 16 KB matches FETCH_TASK_STACK.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

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
// ║  PAGE SYSTEM                                                              ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

struct PageInfo {
    enum Type { TRANSIT, COMMUTE_HOME, COMMUTE_WORK, STOCKS } type;
    int transit_idx; // only meaningful for TRANSIT pages
};
static PageInfo g_pages[12];
static int      g_num_pages    = 0;
static int      g_current_page = 0;

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  RUNTIME STATE                                                            ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

static int      current_stop_idx  = 0;
static bool     large_font_mode   = false;   // persisted in NVS ("large_font")
static uint32_t g_page_save_at_ms = 0;       // millis() when the current page should be saved; 0 = nothing pending
static const uint32_t PAGE_SAVE_DELAY_MS = 10000;
static uint32_t last_redraw_ms    = 0;
static time_t   g_countdown_target = 0;
static int      g_countdown_icon   = 0;

// Flight tracking
enum View { VIEW_BOARD, VIEW_FLIGHT0, VIEW_FLIGHT1 };
static View g_view         = VIEW_BOARD;
static int  g_flight_count = 0;

// Commute — display-side cache; populated from background task via fetch_task_get_commute()
static CommuteData g_commute_home  = {};
static CommuteData g_commute_work  = {};
static String      g_home_station;
static String      g_work_station;
static int         g_commute_conn_idx = 0;

// Stocks — config from NVS; quotes are copied from the background cache on each redraw
static StocksConfig g_stocks_cfg;
static StockQuote   g_stock_quotes[STOCKS_MAX];
static int          g_stock_sub_page  = 0;   // which group of tickers is on screen
static uint32_t     g_stock_rotate_ms = 0;   // millis() of the last sub-page change
static int          g_stock_detail    = -1;  // ticker shown in the detail view; -1 = list

// Weather — display-side cache; populated from background task via fetch_task_get_weather()
static WeatherData weather_data    = {};
static String      g_weather_city;        // town of the place the weather is for
static uint32_t    g_setup_done_ms = 0;  // set at end of setup(); used to detect "still loading"

// Pager — display-side state
enum PagerState { PAGER_OFF=0, PAGER_ACTIVITIES, PAGER_TIME_SEL, PAGER_FRIENDS, PAGER_INCOMING };
static PagerState    g_pager_state     = PAGER_OFF;
static int           g_pager_act       = -1;   // selected activity (0-9)
static int           g_pager_time      = -1;   // selected time slot (0-5)
static int           g_pager_hilite    = -1;   // highlighted tile/row for visual feedback
static bool          g_pager_touch_was = false;
static bool          g_pager_dirty     = false; // set on state change; cleared after draw
static int           g_pager_last_min  = -1;   // elapsed minutes shown on incoming screen
static PagerConfig   g_pager_cfg;
static PagerIncoming g_pager_incoming;

static const char* PAGER_ACTS[]  = { "Beer",   "Coffee", "Movie", "Gym",  "Run",
                                      "Bike",   "Phone",  "Dinner","Lunch","Love" };
static const char* PAGER_TIMES[] = { "5 min",  "10 min", "15 min",
                                      "30 min", "Tonight","Now"    };

// Parcel tracking — display-side state
static String   g_parcel_status;         // "" = not configured or not yet fetched
static int      g_parcel_pulses    = 3;  // brightness flashes on status change
static bool     g_parcel_pulsing   = false;
static int      g_parcel_pulse_rem = 0;  // remaining flash cycles
static uint32_t g_parcel_pulse_ms  = 0;  // millis() when current half-cycle started
static bool     g_parcel_pulse_hi  = false; // true = currently at 100% brightness

// Battery — cached from battery_status() once a minute (see update_battery())
static int        g_battery_pct     = -1;
static bool       g_battery_charging = false;
static uint32_t   last_battery_ms   = 0;
static int        g_battery_warned_at = 101;  // lowest warning level already shown

// Full-screen message overlay (low battery warning, …): page redraws pause until
// this millis() deadline or until the screen is touched. 0 = no overlay.
static uint32_t   g_overlay_until_ms = 0;

// Button debounce state — BOOT button
static bool     button_was_pressed    = false;
static uint32_t button_press_start_ms = 0;
// Config-hold prompt: 0 = none, 1 = "Hold to configure...", 2 = "Release to configure".
// While non-zero, loop() skips page redraws so the prompt stays on screen.
static int      config_hint_stage     = 0;
static const uint32_t CONFIG_HINT_MS = 1000;  // show "Hold to configure..."
static const uint32_t CONFIG_HOLD_MS = 3000;  // releasing after this opens the portal

// Button debounce state — bottom-right button (brightness cycle)
// ≥50ms hold required to filter GPIO glitches and touch-IRQ pulses.
static bool     s_btn_bright_was_pressed  = false;
static uint32_t s_btn_bright_press_start  = 0;

// Button debounce state — circular button (GPIO 38): long press = flip orientation
static bool     s_btn_zoom_was_pressed    = false;
static uint32_t s_btn_zoom_press_start    = 0;

// Display orientation state (persisted to NVS)
static bool g_display_flipped = false;

// Build the page list from transit stops + commute pages.
// Call after setup_stops() and after commute is resolved.
static bool commute_configured() {
    return g_home_station.length() > 0 && g_work_station.length() > 0;
}

static void setup_pages() {
    g_num_pages = 0;
    for (int i = 0; i < g_num_stops && g_num_pages < 12; i++) {
        g_pages[g_num_pages++] = { PageInfo::TRANSIT, i };
    }
    // Both stations required: home commute = work→home, work commute = home→work
    if (commute_configured()) {
        if (g_num_pages < 12) g_pages[g_num_pages++] = { PageInfo::COMMUTE_HOME, 0 };
        if (g_num_pages < 12) g_pages[g_num_pages++] = { PageInfo::COMMUTE_WORK, 0 };
    }
    if (g_stocks_cfg.count > 0 && g_num_pages < 12) {
        g_pages[g_num_pages++] = { PageInfo::STOCKS, 0 };
    }
    Serial.printf("[Pages] %d pages (%d transit, commute %s, stocks %s)\n",
                  g_num_pages, g_num_stops,
                  commute_configured() ? "on" : "off",
                  g_stocks_cfg.count > 0 ? "on" : "off");
}

// Number of ticker groups on the stocks page (each fits on one screen).
static int stock_sub_page_count() {
    int per_page = stocks_per_page(g_stocks_cfg.layout, g_stocks_cfg.count);
    return std::max(1, (g_stocks_cfg.count + per_page - 1) / per_page);
}

static void set_stock_sub_page(int idx) {
    int n = stock_sub_page_count();
    g_stock_sub_page  = ((idx % n) + n) % n;
    g_stock_rotate_ms = millis();
    last_redraw_ms    = 0;   // force immediate redraw
}

// Detail view: tap a ticker to open it, swipe up/down for the next/previous one,
// tap again (or swipe left/right to another page) to go back.
// idx -1 closes it; the list returns at the sub-page that holds the ticker.
static void set_stock_detail(int idx) {
    int count = g_stocks_cfg.count;
    if (idx >= 0 && count > 0) idx = ((idx % count) + count) % count;
    else idx = -1;
    if (idx < 0 && g_stock_detail >= 0) {
        int per_page = stocks_per_page(g_stocks_cfg.layout, count);
        g_stock_sub_page  = g_stock_detail / per_page;
        g_stock_rotate_ms = millis();
    }
    g_stock_detail = idx;
    fetch_task_request_stock_chart(idx);
    last_redraw_ms = 0;
    display_invalidate();
    Serial.printf("[Stocks] Detail view → %d\n", idx);
}

// Portfolio tap: open the tapped ticker's details, or close the details
static void handle_stock_tap() {
    if (g_stock_detail >= 0) { set_stock_detail(-1); return; }
    int per_page = stocks_per_page(g_stocks_cfg.layout, g_stocks_cfg.count);
    int cell = display_stock_cell_at(touch_last_screen_x(), touch_last_screen_y(), per_page);
    int idx  = (cell >= 0) ? g_stock_sub_page * per_page + cell : -1;
    if (idx >= 0 && idx < g_stocks_cfg.count) set_stock_detail(idx);
}

// Set initial page based on time: morning → work commute, afternoon/evening → home commute.
static void set_initial_page() {
    time_t now_t = time(nullptr);
    if (now_t < 1700000000) return; // clock not synced, keep page 0
    struct tm tm_now; localtime_r(&now_t, &tm_now);
    bool is_morning = (tm_now.tm_hour < 12);
    for (int i = 0; i < g_num_pages; i++) {
        if (is_morning && g_pages[i].type == PageInfo::COMMUTE_WORK)  { g_current_page = i; return; }
        if (!is_morning && g_pages[i].type == PageInfo::COMMUTE_HOME) { g_current_page = i; return; }
    }
}

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
        Serial.printf("[WiFi] Trying enterprise '%s' user='%s'...\n", ssid.c_str(), user.c_str());
        esp_wifi_sta_wpa2_ent_set_identity((uint8_t*)user.c_str(), user.length());
        esp_wifi_sta_wpa2_ent_set_username((uint8_t*)user.c_str(), user.length());
        esp_wifi_sta_wpa2_ent_set_password((uint8_t*)pass.c_str(), pass.length());
        esp_wifi_sta_wpa2_ent_enable();
        WiFi.begin(ssid.c_str());
    } else {
        Serial.printf("[WiFi] Trying '%s'...\n", ssid.c_str());
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
        Serial.print(".");
    }
    Serial.println();
    if (user.length() > 0) esp_wifi_sta_wpa2_ent_disable();
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFi] Connected, IP: %s\n", WiFi.localIP().toString().c_str());
        return true;
    }
    WiFi.disconnect(true);
    return false;
}

static bool connect_wifi() {
    WiFi.mode(WIFI_STA);
    String ssids[MAX_WIFI_CREDS], passes[MAX_WIFI_CREDS], users[MAX_WIFI_CREDS];
    int n = load_wifi_creds(ssids, passes, users);
    for (int i = 0; i < n; i++)
        if (try_connect(ssids[i], passes[i], users[i])) return true;
    Serial.println("[WiFi] All credentials failed");
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

static void wifi_reconnect_tick() {
    uint32_t now = millis();
    if (WiFi.status() == WL_CONNECTED) {
        if (g_wifi_try >= 0) {
            wifi_end_attempt();
            g_wifi_try = -1;
            esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
            configTzTime(POSIX_TZ, "pool.ntp.org", "time.nist.gov");
            Serial.printf("[WiFi] Reconnected, IP: %s\n", WiFi.localIP().toString().c_str());
        }
        if (!g_web_started) { config_web_begin(); g_web_started = true; }
        return;
    }
    if (g_wifi_try < 0) {
        if (now - g_wifi_round_ms < WIFI_RETRY_MS) return;
        g_wifi_round_ms = now;
        g_wifi_n = load_wifi_creds(g_wifi_ssids, g_wifi_passes, g_wifi_users);
        Serial.println("[WiFi] Disconnected — reconnecting in the background");
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

static void sync_clock() {
    Serial.println("[NTP] Syncing time...");
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
    Serial.printf("[NTP] Time: %04d-%02d-%02d %02d:%02d:%02d\n",
                  t.tm_year+1900, t.tm_mon+1, t.tm_mday,
                  t.tm_hour, t.tm_min, t.tm_sec);
}

// Show a message over the current page for duration_ms (touch dismisses it).
static void show_overlay(const char* msg, uint32_t duration_ms) {
    display_set_screen_on(true);   // also during screen-off night mode
    display_show_status(msg);
    g_overlay_until_ms = millis() + duration_ms;
    if (g_overlay_until_ms == 0) g_overlay_until_ms = 1;
}

// Settings on the normal Wi-Fi: show the pairing code while a browser waits for it
static void on_pairing_code(const char* code) {
    if (code) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Settings code\n%s\nEnter it in your browser", code);
        show_overlay(msg, 120000);
    } else if (g_overlay_until_ms != 0) {
        g_overlay_until_ms = 0;
        last_redraw_ms     = 0;
        g_pager_dirty      = true;
    }
}

static bool overlay_active() {
    if (g_overlay_until_ms == 0) return false;
    if ((int32_t)(millis() - g_overlay_until_ms) >= 0 || touch_is_pressed()) {
        g_overlay_until_ms = 0;
        last_redraw_ms     = 0;   // repaint the page right away
        g_pager_dirty      = true;
        return false;
    }
    return true;
}

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  SCREEN POWER — night mode (dim or off) and wake-on-touch                 ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
// In "screen off" night mode any touch or button wakes the screen for
// WAKE_ON_TOUCH_SEC (each further interaction extends it). The press that wakes
// the screen is swallowed, so it doesn't also switch pages or open menus.

static NightConfig g_night;
static bool        g_woken           = false;
static uint32_t    g_woke_at_ms      = 0;
static bool        g_input_swallowed = false;

static bool any_input_down() {
    return touch_is_pressed() ||
           digitalRead(PIN_BOOT_BUTTON) == LOW ||
           digitalRead(PIN_BTN_BRIGHT)  == LOW ||
           digitalRead(PIN_BTN_THEME)   == LOW;
}

// Updates screen power and the fetch rate. Returns true while input must be
// ignored (from the waking press until it is released, inclusive).
static bool power_tick() {
    bool night    = display_is_night();
    bool off_mode = g_night.screen_off && night;
    bool input    = any_input_down();

    if (input && off_mode) {
        if (!display_screen_on()) g_input_swallowed = true;
        g_woken      = true;
        g_woke_at_ms = millis();
    }
    if (g_woken && millis() - g_woke_at_ms >= WAKE_ON_TOUCH_SEC * 1000UL) g_woken = false;

    // Overlays (pairing code, warnings) keep the screen on while they show
    bool want_on = !off_mode || g_woken || g_overlay_until_ms != 0;
    if (want_on != display_screen_on()) {
        display_set_screen_on(want_on);
        if (want_on) {
            last_redraw_ms = 0;
            g_pager_dirty  = true;
            fetch_task_force_refresh();   // data was refreshed rarely while off
        }
    }
    fetch_task_set_power_mode(!want_on ? FETCH_POWER_SCREEN_OFF
                              : night  ? FETCH_POWER_NIGHT
                                       : FETCH_POWER_NORMAL);

    bool swallow = g_input_swallowed;
    if (!input) g_input_swallowed = false;
    return swallow;
}

static void update_battery() {
    battery_update();
    const BatteryStatus& b = battery_status();
    g_battery_pct      = b.pct;
    g_battery_charging = b.charging;
    display_set_battery_estimate(b.hours_left);

    // Low-battery warning, once per level while unplugged. Not while the screen
    // is off at night — it shows at the next wake-up instead of lighting the room.
    if (b.pct < 0 || b.charging) { g_battery_warned_at = 101; return; }
    if (!display_screen_on()) return;
    for (int level : BATTERY_WARN_LEVELS) {
        if (b.pct <= level && g_battery_warned_at > level) {
            g_battery_warned_at = level;
            char msg[64];
            if (b.hours_left >= 0)
                snprintf(msg, sizeof(msg), "Battery low: %d%%\nAbout %.0f h left\nPlease charge",
                         b.pct, b.hours_left < 1 ? 1.0f : b.hours_left);
            else
                snprintf(msg, sizeof(msg), "Battery low: %d%%\nPlease charge", b.pct);
            show_overlay(msg, BATTERY_WARN_SHOW_MS);
            Serial.printf("[Batt] Low-battery warning at %d%%\n", b.pct);
            break;
        }
    }
}

// The footer weather follows the page: a transit stop's own location, the
// commute station you're leaving from, or (portfolio) the device's own location.
static void update_weather_place() {
    const PageInfo& p = g_pages[g_current_page];
    switch (p.type) {
        case PageInfo::TRANSIT:      fetch_task_set_weather_place(p.transit_idx);        break;
        case PageInfo::COMMUTE_WORK: fetch_task_set_weather_place(WEATHER_PLACE_HOME);   break;  // from home
        case PageInfo::COMMUTE_HOME: fetch_task_set_weather_place(WEATHER_PLACE_WORK);   break;  // from work
        case PageInfo::STOCKS:       fetch_task_set_weather_place(WEATHER_PLACE_HERE);   break;
    }
}

static void switch_page(int new_page) {
    g_current_page     = new_page;
    g_commute_conn_idx = 0;
    g_view             = VIEW_BOARD;
    g_stock_sub_page   = 0;
    g_stock_rotate_ms  = millis();
    if (g_stock_detail >= 0) { g_stock_detail = -1; fetch_task_request_stock_chart(-1); }
    last_redraw_ms     = 0;   // force immediate redraw
    PageInfo& p = g_pages[new_page];
    if (p.type == PageInfo::TRANSIT) {
        current_stop_idx = p.transit_idx;
        fetch_task_set_active_stop(current_stop_idx);
    }
    update_weather_place();
    // Remember the page once the user settles on it (not on every swipe —
    // each NVS write wears flash). Saved by save_page_tick().
    g_page_save_at_ms = millis() + PAGE_SAVE_DELAY_MS;
    display_invalidate();
}

static void save_page_tick() {
    if (g_page_save_at_ms == 0 || (int32_t)(millis() - g_page_save_at_ms) < 0) return;
    g_page_save_at_ms = 0;
    Preferences prefs;
    prefs.begin("tramli", false);
    if (prefs.getInt("page", -1) != g_current_page) prefs.putInt("page", g_current_page);
    prefs.end();
}

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  INPUT HANDLERS                                                           ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

// When flipped 180°, the top-left (GPIO 0) and bottom-right (GPIO 12) buttons
// physically swap roles so the function stays at the same visual corner.
// PIN_BOOT_BUTTON (GPIO 0) = nav/config in normal; brightness in flipped.
// PIN_BTN_BRIGHT  (GPIO 12) = brightness in normal; nav/config in flipped.
static void check_button() {
    int pin = g_display_flipped ? PIN_BTN_BRIGHT : PIN_BOOT_BUTTON;
    bool pressed = (digitalRead(pin) == LOW);

    if (pressed && !button_was_pressed) {
        button_press_start_ms = millis();
        config_hint_stage     = 0;
    } else if (pressed && button_was_pressed) {
        // Still held — prompt the user, then tell them when it's safe to let go
        uint32_t held_ms = millis() - button_press_start_ms;
        if (held_ms >= CONFIG_HOLD_MS && config_hint_stage < 2) {
            config_hint_stage = 2;
            display_show_status("Release to configure");
        } else if (held_ms >= CONFIG_HINT_MS && config_hint_stage < 1) {
            config_hint_stage = 1;
            if (WiFi.status() == WL_CONNECTED) {
                // Also point to the settings page on the normal Wi-Fi
                char msg[128];
                snprintf(msg, sizeof(msg),
                         "Hold to configure...\n\nOr on this Wi-Fi open\n%s.local\n%s",
                         WEB_HOSTNAME, WiFi.localIP().toString().c_str());
                display_show_status(msg);
            } else {
                display_show_status("Hold to configure...");
            }
        }
    } else if (!pressed && button_was_pressed) {
        uint32_t held_ms = millis() - button_press_start_ms;
        if (config_hint_stage > 0) {
            // Prompt covered the screen — repaint whatever was showing
            last_redraw_ms = 0;
            g_pager_dirty  = true;
        }
        config_hint_stage = 0;
        if (held_ms >= CONFIG_HOLD_MS) {
            Serial.println("[Button] 3s hold → config portal");
            config_portal_run(PORTAL_TIMEOUT_MS);  // reboots on save; returns on timeout
        } else if (held_ms >= TOUCH_LONG_PRESS_MS) {
            Serial.println("[Button] Long press → force refresh");
            if (g_pages[g_current_page].type == PageInfo::STOCKS) fetch_task_force_stocks_refresh();
            else                                                   fetch_task_force_refresh();
        } else if (held_ms >= 50) {
            Serial.println("[Button] Short press → prev page");
            switch_page((g_current_page - 1 + g_num_pages) % g_num_pages);
        }
    }
    button_was_pressed = pressed;
}

static void check_btn_bright() {
    int pin = g_display_flipped ? PIN_BOOT_BUTTON : PIN_BTN_BRIGHT;
    bool pressed = (digitalRead(pin) == LOW);
    if (pressed && !s_btn_bright_was_pressed) {
        s_btn_bright_press_start = millis();
    } else if (!pressed && s_btn_bright_was_pressed) {
        uint32_t held = millis() - s_btn_bright_press_start;
        if (held >= 2000) {
            // Long press — toggle pager
            if (g_pager_state == PAGER_OFF) {
                g_pager_state  = PAGER_ACTIVITIES;
                g_pager_act    = -1;
                g_pager_time   = -1;
                g_pager_hilite = -1;
                g_pager_dirty  = true;
                fetch_task_notify_pager_activity();
                Serial.println("[Pager] Opened");
            } else {
                g_pager_state = PAGER_OFF;
                display_invalidate();  // reset board draw state
                Serial.println("[Pager] Cancelled");
            }
        } else if (held >= 50 && g_pager_state == PAGER_OFF) {
            Serial.println("[BtnBright] cycle brightness");
            display_step_brightness();
        }
    }
    s_btn_bright_was_pressed = pressed;
}

// GPIO 38 button — currently diagnostic only (short press prints to Serial).
// Orientation flip has been moved to GPIO 16 long press (theme button).
static void check_btn_zoom() {
    bool pressed = (digitalRead(PIN_BTN_ZOOM) == LOW);
    if (pressed && !s_btn_zoom_was_pressed) {
        s_btn_zoom_press_start = millis();
        Serial.println("[GPIO38] DOWN — button detected");
    } else if (!pressed && s_btn_zoom_was_pressed) {
        uint32_t held_ms = millis() - s_btn_zoom_press_start;
        Serial.printf("[GPIO38] UP — held %lu ms\n", held_ms);
    }
    s_btn_zoom_was_pressed = pressed;
}

// ── Color themes ──────────────────────────────────────────────────────────────
// RGB565 values: R=5bit G=6bit B=5bit.
// Each theme has 4 brightness levels: 100%/80%/50%/30%.
struct Theme {
    const char* name;
    uint16_t row0, rows, dim, meta;
};

// Palette derivation (all verified bit-by-bit):
//  Zürich Amber  base #F09000 (R30 G36 B0):  G/R≈0.6  → warm amber
//  Matrix Green  base #00FC00 (R0  G63 B0):  pure phosphor green
//  Tron Blue     base #00A0F8 (R0  G40 B31): electric cyan-blue
//  Stranger Red  base #F81800 (R31 G6  B0):  deep neon red
static const Theme THEMES[] = {
    { "Zurich Amber",   0xF480, 0xC3A0, 0x7A40, 0x4960 },
    { "Matrix Green",   0x07E0, 0x0640, 0x0400, 0x0260 },
    { "Tron Blue",      0x051F, 0x0419, 0x0290, 0x0189 },
    { "Stranger Red",   0xF8C0, 0xC8A0, 0x7860, 0x4840 },
};
static const int N_THEMES = sizeof(THEMES) / sizeof(THEMES[0]);
static int g_theme_idx = 0;

static void apply_theme(int idx) {
    g_theme_idx = idx;
    const Theme& t = THEMES[idx];
    display_set_theme(t.row0, t.rows, t.dim, t.meta);
    display_invalidate();
    Serial.printf("[Theme] %s\n", t.name);
}

static void load_theme() {
    Preferences prefs;
    prefs.begin("tramli", true);
    int idx = prefs.getInt("theme", 0);
    prefs.end();
    if (idx < 0 || idx >= N_THEMES) idx = 0;
    g_theme_idx = idx;
    const Theme& t = THEMES[idx];
    display_set_theme(t.row0, t.rows, t.dim, t.meta);
    Serial.printf("[Theme] Loaded: %s\n", t.name);
}

static void save_theme(int idx) {
    Preferences prefs;
    prefs.begin("tramli", false);
    prefs.putInt("theme", idx);
    prefs.end();
}

// ── Theme button (GPIO 16) ────────────────────────────────────────────────────
static bool     s_btn_theme_was_pressed = false;
static uint32_t s_btn_theme_press_start = 0;

static void check_btn_theme() {
    bool pressed = (digitalRead(PIN_BTN_THEME) == LOW);
    if (pressed && !s_btn_theme_was_pressed) {
        s_btn_theme_press_start = millis();
    } else if (!pressed && s_btn_theme_was_pressed) {
        uint32_t held = millis() - s_btn_theme_press_start;
        if (held >= TOUCH_LONG_PRESS_MS) {
            // Long press → flip orientation 180° and persist
            g_display_flipped = !g_display_flipped;
            display_set_flipped(g_display_flipped);
            Preferences prefs;
            prefs.begin("tramli", false);
            prefs.putBool("flipped", g_display_flipped);
            prefs.end();
            Serial.printf("[Theme] Long press → orientation flipped=%d\n", g_display_flipped);
        } else if (held >= 50) {
            // Short press → cycle color theme
            int new_idx = (g_theme_idx + 1) % N_THEMES;
            apply_theme(new_idx);
            save_theme(new_idx);
        }
    }
    s_btn_theme_was_pressed = pressed;
}

// gesture comes from touch_poll() in loop(). While the pager is open its own
// tap handling owns the screen, so gestures are ignored here.
static void check_touch(TouchGesture gesture) {
    if (g_pager_state != PAGER_OFF) return;
    switch (gesture) {
        case TOUCH_SWIPE_LEFT:
            Serial.println("[Touch] Swipe left → next page");
            switch_page((g_current_page + 1) % g_num_pages);
            break;
        case TOUCH_SWIPE_RIGHT:
            Serial.println("[Touch] Swipe right → prev page");
            switch_page((g_current_page - 1 + g_num_pages) % g_num_pages);
            break;
        case TOUCH_LONG_PRESS:
            Serial.println("[Touch] Long press → force refresh");
            if (g_pages[g_current_page].type == PageInfo::TRANSIT) {
                if (g_view == VIEW_BOARD) fetch_task_force_refresh();
                else {
                    int slot = (g_view == VIEW_FLIGHT1) ? 1 : 0;
                    fetch_task_force_flight_refresh(slot);  // runs on core 0 within ~5 s
                }
            } else if (g_pages[g_current_page].type == PageInfo::STOCKS) {
                fetch_task_force_stocks_refresh();
            } else {
                // Commute: ask background task to re-fetch on its next iteration
                fetch_task_force_commute_refresh();
            }
            break;
        case TOUCH_DOUBLE_TAP:
            // Font size only applies to the departure board; on the portfolio a
            // quick double tap is just an impatient tap
            if (g_pages[g_current_page].type == PageInfo::STOCKS) { handle_stock_tap(); break; }
            Serial.println("[Touch] Double tap → toggle font size");
            large_font_mode = !large_font_mode;
            {
                Preferences prefs;
                prefs.begin("tramli", false);
                prefs.putBool("large_font", large_font_mode);
                prefs.end();
            }
            display_invalidate();
            break;
        case TOUCH_SWIPE_DOWN:
            if (g_pages[g_current_page].type == PageInfo::TRANSIT) {
                // Cycle forward: BOARD → FLIGHT0 → FLIGHT1 → BOARD
                if (g_flight_count > 0) {
                    View nv;
                    if      (g_view == VIEW_BOARD)  nv = VIEW_FLIGHT0;
                    else if (g_view == VIEW_FLIGHT0) nv = (g_flight_count > 1) ? VIEW_FLIGHT1 : VIEW_BOARD;
                    else                             nv = VIEW_BOARD;
                    Serial.printf("[Touch] Swipe down → flight view %d\n", (int)nv);
                    if (nv != g_view) { g_view = nv; display_invalidate(); }
                }
            } else if (g_pages[g_current_page].type == PageInfo::STOCKS) {
                if (g_stock_detail >= 0) {
                    set_stock_detail(g_stock_detail + 1);       // next ticker's details
                } else {
                    set_stock_sub_page(g_stock_sub_page + 1);
                    Serial.printf("[Touch] Stocks → sub-page %d\n", g_stock_sub_page);
                }
            } else {
                // Commute: advance to next connection
                CommuteData& cd = (g_pages[g_current_page].type == PageInfo::COMMUTE_HOME)
                                  ? g_commute_home : g_commute_work;
                if (cd.connection_count > 1) {
                    g_commute_conn_idx = (g_commute_conn_idx + 1) % cd.connection_count;
                    Serial.printf("[Touch] Commute → connection %d\n", g_commute_conn_idx);
                    display_invalidate();
                }
            }
            break;
        case TOUCH_SWIPE_UP:
            if (g_pages[g_current_page].type == PageInfo::TRANSIT) {
                // Cycle backward: BOARD → FLIGHT1 → FLIGHT0 → BOARD
                if (g_flight_count > 0) {
                    View nv;
                    if      (g_view == VIEW_BOARD)   nv = (g_flight_count > 1) ? VIEW_FLIGHT1 : VIEW_FLIGHT0;
                    else if (g_view == VIEW_FLIGHT1)  nv = VIEW_FLIGHT0;
                    else                              nv = VIEW_BOARD;
                    Serial.printf("[Touch] Swipe up → flight view %d\n", (int)nv);
                    if (nv != g_view) { g_view = nv; display_invalidate(); }
                }
            } else if (g_pages[g_current_page].type == PageInfo::STOCKS) {
                if (g_stock_detail >= 0) {
                    set_stock_detail(g_stock_detail - 1);       // previous ticker's details
                } else {
                    set_stock_sub_page(g_stock_sub_page - 1);
                    Serial.printf("[Touch] Stocks → sub-page %d\n", g_stock_sub_page);
                }
            } else {
                // Commute: go back to previous connection
                CommuteData& cd = (g_pages[g_current_page].type == PageInfo::COMMUTE_HOME)
                                  ? g_commute_home : g_commute_work;
                if (cd.connection_count > 1) {
                    g_commute_conn_idx = (g_commute_conn_idx - 1 + cd.connection_count) % cd.connection_count;
                    Serial.printf("[Touch] Commute → connection %d\n", g_commute_conn_idx);
                    display_invalidate();
                }
            }
            break;
        case TOUCH_TAP:
            if (g_pages[g_current_page].type == PageInfo::STOCKS) handle_stock_tap();
            break;
        default:
            break;
    }
}

// ── Pager touch + send ────────────────────────────────────────────────────────

static void pager_reply(int btn_idx) {
    // btn_idx: 0=Yes 1=No 2=Later; sends reply back, no reply_topic so sender gets Close-only
    if (g_pager_incoming.reply_topic.isEmpty()) return;
    if (btn_idx < 0 || btn_idx > 2) return;
    const char* labels[] = { "Yes", "No", "Later" };
    String text = String(labels[btn_idx]);
    if (!g_pager_cfg.name.isEmpty()) { text += " (from "; text += g_pager_cfg.name; text += ")"; }
    // Embed the original question so the sender can see what they asked
    if (!g_pager_incoming.display_text.isEmpty()) {
        text += "\nre=";
        text += g_pager_incoming.display_text;
    }
    http_lock_take();
    pager_publish(g_pager_incoming.reply_topic,
                  g_pager_cfg.name.isEmpty() ? "Busli" : g_pager_cfg.name,
                  "",      // empty own_topic → no reply= line → sender sees Close-only
                  text);
    http_lock_give();
}

static void pager_send(int friend_idx) {
    if (friend_idx < 0 || friend_idx >= g_pager_cfg.n_friends) return;
    if (g_pager_act < 0) return;
    if (g_pager_act != 9 && g_pager_time < 0) return;  // time required except for Love
    const String& to_topic = g_pager_cfg.friends[friend_idx].topic;
    String text = (g_pager_act == 9)
        ? "Love"
        : (String(PAGER_ACTS[g_pager_act]) + " " + String(PAGER_TIMES[g_pager_time]) + "?");
    Serial.printf("[Pager] Send to %s: %s\n",
                  g_pager_cfg.friends[friend_idx].name.c_str(), text.c_str());
    fetch_task_notify_pager_activity();
    http_lock_take();
    pager_publish(to_topic, g_pager_cfg.name.isEmpty() ? "Busli" : g_pager_cfg.name,
                  g_pager_cfg.topic, text);
    http_lock_give();
}


static void handle_pager_touch() {
    bool now_pressed  = touch_is_pressed();
    bool just_pressed = (now_pressed && !g_pager_touch_was);
    bool just_lifted  = (!now_pressed && g_pager_touch_was);
    g_pager_touch_was = now_pressed;

    int tx = touch_last_screen_x();
    int ty = touch_last_screen_y();
    int content_h = SCREEN_H - HEADER_H;

    // On press-down: highlight the touched tile immediately for instant feedback
    if (just_pressed) {
        int hilite = -1;
        switch (g_pager_state) {
            case PAGER_ACTIVITIES: {
                int col = tx * 5 / SCREEN_W;
                int row = (ty - HEADER_H) * 2 / content_h;
                if (ty >= HEADER_H && row >= 0 && row < 2 && col >= 0 && col < 5)
                    hilite = row * 5 + col;
                break;
            }
            case PAGER_TIME_SEL: {
                int col = tx * 3 / SCREEN_W;
                int row = (ty - HEADER_H) * 2 / content_h;
                if (ty >= HEADER_H && row >= 0 && row < 2 && col >= 0 && col < 3)
                    hilite = row * 3 + col;
                break;
            }
            case PAGER_FRIENDS: {
                int n    = g_pager_cfg.n_friends;
                int rows = (n <= 3) ? 1 : 2;
                int col  = tx * 3 / SCREEN_W;
                int row  = (ty - HEADER_H) * rows / content_h;
                if (ty >= HEADER_H && row >= 0 && row < rows && col >= 0 && col < 3) {
                    int idx = row * 3 + col;
                    if (idx < n) hilite = idx;
                }
                break;
            }
            case PAGER_INCOMING:
                if (ty >= SCREEN_H - 70) {
                    bool has_replies = !g_pager_incoming.reply_topic.isEmpty();
                    hilite = has_replies ? constrain(tx * 4 / SCREEN_W, 0, 3) : 0;
                }
                break;
            default: break;
        }
        if (hilite != g_pager_hilite) {
            g_pager_hilite = hilite;
            g_pager_dirty  = true;
        }
        return;
    }

    if (!just_lifted) return;

    switch (g_pager_state) {
        case PAGER_ACTIVITIES: {
            int col = tx * 5 / SCREEN_W;
            int row = (ty - HEADER_H) * 2 / content_h;
            if (ty >= HEADER_H && row >= 0 && row < 2 && col >= 0 && col < 5) {
                g_pager_act    = row * 5 + col;
                g_pager_time   = -1;
                // Love (9) skips time selection — goes straight to friend picker
                g_pager_state  = (g_pager_act == 9) ? PAGER_FRIENDS : PAGER_TIME_SEL;
                g_pager_hilite = -1;
                g_pager_dirty  = true;
            }
            break;
        }
        case PAGER_TIME_SEL: {
            int col = tx * 3 / SCREEN_W;
            int row = (ty - HEADER_H) * 2 / content_h;
            if (ty >= HEADER_H && row >= 0 && row < 2 && col >= 0 && col < 3) {
                g_pager_time   = row * 3 + col;
                g_pager_state  = PAGER_FRIENDS;
                g_pager_hilite = -1;
                g_pager_dirty  = true;
            }
            break;
        }
        case PAGER_FRIENDS: {
            int n    = g_pager_cfg.n_friends;
            int rows = (n <= 3) ? 1 : 2;
            int col  = tx * 3 / SCREEN_W;
            int row  = (ty - HEADER_H) * rows / content_h;
            if (n > 0 && ty >= HEADER_H && row >= 0 && row < rows && col >= 0 && col < 3) {
                int idx = row * 3 + col;
                if (idx < n) {
                    pager_send(idx);
                    g_pager_state = PAGER_OFF;
                    display_invalidate();
                }
            }
            break;
        }
        case PAGER_INCOMING: {
            if (ty >= SCREEN_H - 70) {
                if (!g_pager_incoming.reply_topic.isEmpty()) {
                    // This is an invite — 4 buttons: Yes/No/Later send reply; Close does nothing
                    int btn = constrain(tx * 4 / SCREEN_W, 0, 3);
                    if (btn < 3) pager_reply(btn);
                }
                // else: it's a reply message — just close, no further action
                g_pager_state = PAGER_OFF;
                display_invalidate();
            }
            break;
        }
        default: break;
    }
}

// ── Parcel brightness pulse ───────────────────────────────────────────────────
// Called every loop() tick when g_parcel_pulsing is true.
// Each "pulse" = 250 ms at 100%, then 250 ms at dimmed level.
// After g_parcel_pulse_rem cycles, restores brightness via display_invalidate().
static void parcel_pulse_tick() {
    if (!g_parcel_pulsing) return;
    uint32_t now_ms = millis();
    if (now_ms - g_parcel_pulse_ms < 250) return;   // wait out the current half-cycle
    g_parcel_pulse_ms = now_ms;
    g_parcel_pulse_hi = !g_parcel_pulse_hi;
    display_set_brightness(g_parcel_pulse_hi ? 100 : 15);
    if (!g_parcel_pulse_hi) {
        // finished one full pulse (hi→lo)
        if (--g_parcel_pulse_rem <= 0) {
            g_parcel_pulsing = false;
            display_restore_brightness();  // back to the day/night level (was stuck at 15%)
        }
    }
}

// Footer text shared by all pages:
//   weather_str  "Zürich 10C/11C"  (town of the stop on screen + now/max temperature)
//   uv_str       a rain alert when rain is coming or falling, otherwise "UV3/4";
//                plus " | <parcel_prefix><status>" while a parcel is tracked
// Shows "No weather" once the first fetch is clearly overdue.
static void format_footer_weather(char* weather_str, size_t weather_len,
                                  char* uv_str, size_t uv_len,
                                  const char* parcel_prefix) {
    weather_str[0] = '\0';
    uv_str[0]      = '\0';
    if (weather_data.valid) {
        snprintf(weather_str, weather_len, "%s%s%dC/%dC",
                 g_weather_city.c_str(), g_weather_city.isEmpty() ? "" : " ",
                 (int)roundf(weather_data.temp_c),
                 (int)roundf(weather_data.temp_max_c));

        // Rain alert replaces UV (which doesn't matter while it rains)
        char info[24];
        struct tm t;
        if (weather_data.raining_now && weather_data.rain_end > 0) {
            localtime_r(&weather_data.rain_end, &t);
            snprintf(info, sizeof(info), "Rain until %02d:%02d", t.tm_hour, t.tm_min);
        } else if (weather_data.raining_now) {
            snprintf(info, sizeof(info), "Raining");
        } else if (weather_data.rain_start > 0) {
            localtime_r(&weather_data.rain_start, &t);
            snprintf(info, sizeof(info), "Rain from %02d:%02d", t.tm_hour, t.tm_min);
        } else {
            snprintf(info, sizeof(info), "UV%d/%d",
                     weather_data.uv_index, weather_data.uv_index_max);
        }
        if (g_parcel_status.length() > 0)
            snprintf(uv_str, uv_len, "%s | %s%s", info, parcel_prefix, g_parcel_status.c_str());
        else
            snprintf(uv_str, uv_len, "%s", info);
    } else if (g_setup_done_ms > 0 && millis() - g_setup_done_ms > 90000) {
        strncpy(weather_str, "No weather", weather_len - 1);
        weather_str[weather_len - 1] = '\0';
    }
}

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  ARDUINO ENTRY POINTS                                                     ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

void setup() {
    setCpuFrequencyMhz(80);   // 80 MHz is the minimum with WiFi; saves ~100 mA vs 240 MHz
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println("============================================");
    Serial.println("  Busli — T-Display S3 Pro");
    Serial.println("============================================");

    pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
    pinMode(PIN_BTN_BRIGHT,  INPUT_PULLUP);
    pinMode(PIN_BTN_ZOOM,    INPUT_PULLUP);
    pinMode(PIN_BTN_THEME,   INPUT_PULLUP);

    display_init();
    config_load_night(g_night);
    display_set_night_schedule(g_night.start_min, g_night.end_min);
    display_init_brightness();
    config_web_set_pairing_callback(on_pairing_code);
    load_theme();
    {
        Preferences prefs;
        prefs.begin("tramli", true);
        g_display_flipped = prefs.getBool("flipped", false);
        large_font_mode   = prefs.getBool("large_font", false);
        prefs.end();
        if (g_display_flipped) display_set_flipped(true);
    }
    Wire.begin(TOUCH_SDA_PIN, TOUCH_SCL_PIN);
    touch_init();
    battery_init();
    display_boot_animation_start();

    setup_stops();

    if (!connect_wifi()) {
        // All credentials failed — stop the animation and show a message.
        // The reconnect loop in loop() will keep retrying every 30 s.
        // The user can hold the boot button at any time to enter config portal.
        display_boot_animation_stop();
        display_show_status("No WiFi\nHold boot button to configure");
        delay(4000);
        display_invalidate();
    }
    // Enable modem sleep: WiFi radio powers down between beacon intervals.
    // Cuts idle WiFi draw ~30-40%. Note: may occasionally increase latency on
    // the first HTTP request after a long idle; disable if connection drops occur.
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

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

    // Load commute station names from NVS and hand them to the background task
    {
        CommuteConfig cc;
        if (config_load_commute(cc)) {
            g_home_station = cc.home_station;
            g_work_station = cc.work_station;
            Serial.printf("[Commute] Home: '%s'  Work: '%s'\n",
                          g_home_station.c_str(), g_work_station.c_str());
        }
        fetch_task_set_commute(g_home_station, g_work_station);
    }

    if (config_load_stocks(g_stocks_cfg))
        Serial.printf("[Stocks] %d ticker(s), layout %d, rotate %ds\n",
                      g_stocks_cfg.count, g_stocks_cfg.layout, g_stocks_cfg.rotate_sec);

    setup_pages();
    set_initial_page();
    update_weather_place();   // before the fetch task's first weather request

    http_lock_init();
    fetch_task_start(g_stop_configs, g_num_stops);
    fetch_task_set_ota_enabled(config_load_ota_enabled());
    fetch_task_set_stocks(g_stocks_cfg.symbols, g_stocks_cfg.count);
    {
        String pt_tracking;
        if (config_load_parcel(pt_tracking, g_parcel_pulses)) {
            fetch_task_set_parcel(pt_tracking, g_parcel_pulses);
            Serial.printf("[Parcel] Tracking: %s (%d pulses)\n",
                          pt_tracking.c_str(), g_parcel_pulses);
        }
    }
    {
        config_load_pager(g_pager_cfg);
        // Inject self as loopback "Me" friend for testing (not persisted)
        if (g_pager_cfg.n_friends < PAGER_MAX_FRIENDS && !g_pager_cfg.topic.isEmpty()) {
            g_pager_cfg.friends[g_pager_cfg.n_friends].name  = "Me";
            g_pager_cfg.friends[g_pager_cfg.n_friends].topic = g_pager_cfg.topic;
            g_pager_cfg.n_friends++;
        }
        fetch_task_set_pager(g_pager_cfg);
        Serial.printf("[Pager] Topic: %s  Name: %s  Friends: %d\n",
                      g_pager_cfg.topic.c_str(), g_pager_cfg.name.c_str(), g_pager_cfg.n_friends);
    }

    // Restore last-viewed page from NVS (overrides time-based initial page)
    {
        Preferences prefs;
        prefs.begin("tramli", true);
        int saved = prefs.getInt("page", -1);
        prefs.end();
        if (saved >= 0 && saved < g_num_pages) {
            g_current_page = saved;
            PageInfo& p = g_pages[saved];
            if (p.type == PageInfo::TRANSIT) {
                current_stop_idx = p.transit_idx;
                fetch_task_set_active_stop(current_stop_idx);
            }
            Serial.printf("[Page] Restored page %d from NVS\n", saved);
            update_weather_place();
        }
    }

    {
        FlightEntry fl_entries[2];
        g_flight_count = config_load_flights(fl_entries);
        if (g_flight_count > 0) {
            Serial.printf("[Flights] %d flight(s) configured\n", g_flight_count);
            String osky_u, osky_p;
            if (config_load_opensky(osky_u, osky_p))
                flight_tracker_set_opensky_auth(osky_u, osky_p);
            // Registers entries and schedules initial fetch on core 0 — setup() doesn't block
            fetch_task_init_flights(fl_entries, g_flight_count);
        }
    }

    // Weather and commute are now fetched by the background task immediately after
    // it starts, so setup() can complete without blocking on those HTTP calls.
    g_setup_done_ms = millis();
    display_boot_animation_stop();
}

void loop() {
    // Touch is polled once per loop; power_tick() may swallow the press that
    // wakes the screen so it doesn't also act as a tap/swipe/button press.
    TouchGesture gesture = touch_poll();
    bool swallow_input   = power_tick();
    if (!swallow_input) {
        check_button();
        check_btn_bright();
        check_btn_zoom();
        check_btn_theme();
        check_touch(gesture);
    }
    save_page_tick();
    wifi_reconnect_tick();
    config_web_poll();

    uint32_t now_ms = millis();

    // Poll PMU once per minute (I2C read; no need for higher rate)
    if (last_battery_ms == 0 || now_ms - last_battery_ms >= 60000) {
        last_battery_ms = now_ms;
        update_battery();
    }

    // OTA update prompt — shown when background task finds a newer firmware version.
    if (g_ota_pending) {
        g_ota_pending = false;
        display_show_ota_prompt(FIRMWARE_VERSION, g_ota_remote_version.c_str());

        // Wait up to 60 s for the user to tap Yes (right) or No (left).
        // Physical buttons: right (PIN_BTN_BRIGHT) = Yes, left (PIN_BTN_ZOOM) = No.
        bool install = false;
        bool decided = false;
        bool was_pressed = false;
        uint32_t deadline = millis() + 60000;
        while (!decided && millis() < deadline) {
            if (digitalRead(PIN_BTN_BRIGHT) == LOW) { install = true;  decided = true; }
            if (digitalRead(PIN_BTN_ZOOM)   == LOW) { install = false; decided = true; }
            touch_poll();
            bool pressed = touch_is_pressed();
            if (was_pressed && !pressed) {
                install = (touch_last_screen_x() > SCREEN_W / 2);
                decided = true;
            }
            was_pressed = pressed;
            delay(20);
        }

        if (install && g_battery_pct >= 0 && g_battery_pct < 50 && !g_battery_charging) {
            display_show_status("Battery too low\nCharge to 50% first");
            delay(4000);
            display_invalidate();
            install = false;
        }

        if (install) {
            display_show_status("Installing firmware...\n0%");
            // HTTPS download needs more stack than loopTask's 8 KB — use a dedicated task.
            // Pass our own handle so the task can resume loop() if the download fails.
            TaskHandle_t loop_task = xTaskGetCurrentTaskHandle();
            xTaskCreate([](void* arg) {
                TaskHandle_t caller = static_cast<TaskHandle_t>(arg);
                auto progress = [](int pct) {
                    char msg[40];
                    snprintf(msg, sizeof(msg), "Installing firmware...\n%d%%", pct);
                    display_show_status(msg);
                };
                if (ota_apply(g_ota_url, progress)) {
                    display_show_status("Update complete!\nRestarting...");
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    ESP.restart();
                } else {
                    display_show_status("Update failed");
                    vTaskDelay(pdMS_TO_TICKS(3000));
                    display_invalidate();
                    vTaskResume(caller); // unblock loop() so buttons work again
                }
                vTaskDelete(nullptr);
            }, "ota_apply", 32768, (void*)loop_task, 5, nullptr);
            vTaskSuspend(nullptr); // pause loop() while OTA task runs
        } else {
            display_invalidate(); // user declined — resume normal display
        }
    }

    // ── Incoming pager message notification ──────────────────────────────────
    // Only dequeue when idle — if a message is already showing, let it finish.
    // The queue holds up to 4; the next pops automatically after Close/reply.
    if (g_pager_state == PAGER_OFF) {
        PagerIncoming inc;
        if (fetch_task_get_pager_incoming(inc)) {
            g_pager_incoming  = inc;
            g_pager_state     = PAGER_INCOMING;
            g_pager_hilite    = -1;
            g_pager_last_min  = -1;
            g_pager_dirty     = true;
            fetch_task_notify_pager_activity();
            Serial.printf("[Pager] Incoming from %s: %s\n",
                          inc.sender_name.c_str(), inc.display_text.c_str());
        }
    }

    // ── Pager touch + render (intercepts everything when pager is open) ──────
    if (g_pager_state != PAGER_OFF) {
        // Keep the incoming timestamp live — trigger redraw each minute
        if (g_pager_state == PAGER_INCOMING && g_pager_incoming.received_at > 0) {
            int elapsed = (int)((time(nullptr) - g_pager_incoming.received_at) / 60);
            if (elapsed != g_pager_last_min) {
                g_pager_last_min = elapsed;
                g_pager_dirty    = true;
            }
        }

        if (!swallow_input) handle_pager_touch();
        if (g_pager_dirty && display_screen_on() && !overlay_active()) {
            g_pager_dirty = false;
            switch (g_pager_state) {
                case PAGER_ACTIVITIES: {
                    // Pass empty label for Love tile so only the heart icon shows
                    const char* acts[10];
                    for (int i = 0; i < 10; i++) acts[i] = (i == 9) ? "" : PAGER_ACTS[i];
                    display_draw_pager_activities(acts, g_pager_hilite);
                    break;
                }
                case PAGER_TIME_SEL:
                    display_draw_pager_times(
                        (g_pager_act >= 0) ? PAGER_ACTS[g_pager_act] : "",
                        PAGER_TIMES, g_pager_hilite);
                    break;
                case PAGER_FRIENDS: {
                    char hdr[40];
                    if (g_pager_act == 9) {
                        snprintf(hdr, sizeof(hdr), "Send love to?");
                    } else {
                        snprintf(hdr, sizeof(hdr), "%s %s?",
                                 (g_pager_act  >= 0) ? PAGER_ACTS[g_pager_act]   : "",
                                 (g_pager_time >= 0) ? PAGER_TIMES[g_pager_time] : "");
                    }
                    const char* names[PAGER_MAX_FRIENDS];
                    for (int i = 0; i < g_pager_cfg.n_friends; i++)
                        names[i] = g_pager_cfg.friends[i].name.c_str();
                    display_draw_pager_friends(hdr, names, g_pager_cfg.n_friends, g_pager_hilite);
                    break;
                }
                case PAGER_INCOMING: {
                    // icon for the main message (invite: match activity name)
                    int icon_idx = -1;
                    for (int i = 0; i < 10; i++) {
                        if (g_pager_incoming.display_text.startsWith(PAGER_ACTS[i])) {
                            icon_idx = i; break;
                        }
                    }
                    // icon for the quoted original question (reply context)
                    int re_icon_idx = -1;
                    if (!g_pager_incoming.re_text.isEmpty()) {
                        for (int i = 0; i < 10; i++) {
                            if (g_pager_incoming.re_text.startsWith(PAGER_ACTS[i])) {
                                re_icon_idx = i; break;
                            }
                        }
                    }
                    char hdr[48];
                    if (!g_pager_incoming.sender_name.isEmpty())
                        snprintf(hdr, sizeof(hdr), "From: %s",
                                 g_pager_incoming.sender_name.c_str());
                    else
                        snprintf(hdr, sizeof(hdr), "Message");
                    bool show_replies = !g_pager_incoming.reply_topic.isEmpty();
                    display_draw_pager_incoming(hdr,
                                                g_pager_incoming.display_text.c_str(),
                                                icon_idx,
                                                g_pager_incoming.received_at,
                                                show_replies,
                                                g_pager_hilite,
                                                g_pager_incoming.re_text.c_str(),
                                                re_icon_idx);
                    break;
                }
                default: break;
            }
        }
        return;
    }

    // Parcel pulse animation tick
    parcel_pulse_tick();

    // Pull latest parcel status — start pulse animation on status change
    {
        String new_parcel;
        bool   parcel_changed = false;
        if (fetch_task_get_parcel(new_parcel, parcel_changed)) {
            g_parcel_status = new_parcel;
            // No flashing while the screen is off at night — the new status is
            // visible in the footer at the next wake-up
            if (parcel_changed && g_parcel_pulses > 0 && !g_parcel_pulsing && display_screen_on()) {
                g_parcel_pulsing   = true;
                g_parcel_pulse_rem = g_parcel_pulses;
                g_parcel_pulse_hi  = true;
                g_parcel_pulse_ms  = millis();
                display_set_brightness(100);
            }
        }
    }

    // Pull latest weather + commute from background cache once per second
    fetch_task_get_weather(weather_data, g_weather_city);
    if (commute_configured()) {
        CommuteData new_home, new_work;
        if (fetch_task_get_commute(new_home, new_work)) {
            // Clamp connection index if the number of connections shrank
            if (new_home.connection_count != g_commute_home.connection_count ||
                new_work.connection_count  != g_commute_work.connection_count) {
                if (g_commute_conn_idx >= new_home.connection_count) g_commute_conn_idx = 0;
                if (g_commute_conn_idx >= new_work.connection_count)  g_commute_conn_idx = 0;
            }
            g_commute_home = new_home;
            g_commute_work = new_work;
        }
    }

    // Redraw every second (clock + age counter; commute marquee advances from millis())
    PageInfo& page = g_pages[g_current_page];
    if (config_hint_stage == 0 && !overlay_active() && display_screen_on() &&
        now_ms - last_redraw_ms >= 1000) {
        last_redraw_ms = now_ms;

        if (page.type == PageInfo::TRANSIT) {
            current_stop_idx = page.transit_idx;

            if (g_view == VIEW_BOARD) {
                std::vector<Departure> departures;
                time_t fetch_time;
                bool   from_cache;
                fetch_task_get(current_stop_idx, departures, fetch_time, from_cache);

                const StopConfig& stop = g_stop_configs[current_stop_idx];
                time_t now_t  = time(nullptr);
                int    age_s  = (fetch_time > 0) ? (int)(now_t - fetch_time) : 0;

                char weather_str[40];
                char uv_str[56];
                format_footer_weather(weather_str, sizeof(weather_str),
                                      uv_str, sizeof(uv_str), "Package: ");

                display_draw_board(
                    stop.label,
                    g_current_page,
                    g_num_pages,
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
                    g_countdown_icon,
                    weather_data.valid && weather_data.snow_today,
                    weather_data.valid && weather_data.clear_today,
                    g_battery_pct,
                    g_battery_charging
                );
            } else {
                int slot = (g_view == VIEW_FLIGHT1) ? 1 : 0;
                FlightInfo fi;
                flight_tracker_get(slot, fi);
                display_draw_flight(slot, g_flight_count, fi, WiFi.status() == WL_CONNECTED);
            }

        } else if (page.type == PageInfo::STOCKS) {
            int count    = fetch_task_get_stocks(g_stock_quotes);
            int per_page = stocks_per_page(g_stocks_cfg.layout, count);
            int n_sub    = stock_sub_page_count();
            bool market_open = stocks_market_open(time(nullptr));

            char weather_str[40];
            char uv_str[56];
            format_footer_weather(weather_str, sizeof(weather_str), uv_str, sizeof(uv_str), "");

            if (g_stock_detail >= count) set_stock_detail(-1);   // ticker list shrank
            if (g_stock_detail >= 0) {
                // Detail view of one ticker (no auto-rotation)
                const StockQuote& q = g_stock_quotes[g_stock_detail];
                char label[40];
                snprintf(label, sizeof(label), "%s  %d/%d",
                         stocks_display_symbol(q.symbol).c_str(), g_stock_detail + 1, count);
                StockChart chart;
                bool have_chart = fetch_task_get_stock_chart(g_stock_detail, chart);
                int age_s = (q.fetch_time > 0) ? (int)(time(nullptr) - q.fetch_time) : 0;

                FooterInfo f;
                f.weather_str = weather_str;
                f.uv_str      = uv_str;
                f.rain_today  = weather_data.valid && weather_data.rain_today;
                f.rain_pct    = weather_data.valid ? weather_data.precip_prob_pct : 0;
                f.snow_today  = weather_data.valid && weather_data.snow_today;
                f.clear_today = weather_data.valid && weather_data.clear_today;
                f.wifi_ok     = WiFi.status() == WL_CONNECTED;
                f.battery_pct = g_battery_pct;
                f.battery_charging = g_battery_charging;
                display_draw_stock_detail(label, g_current_page, g_num_pages, q,
                                          have_chart ? &chart : nullptr, market_open, age_s, f);
            } else {
                // Auto-rotate through ticker groups (swipe up/down resets the timer)
                if (n_sub > 1 && now_ms - g_stock_rotate_ms >= (uint32_t)g_stocks_cfg.rotate_sec * 1000)
                    set_stock_sub_page(g_stock_sub_page + 1);
                if (g_stock_sub_page >= n_sub) g_stock_sub_page = 0;

                char label[32] = "Portfolio";
                if (n_sub > 1)
                    snprintf(label, sizeof(label), "Portfolio %d/%d", g_stock_sub_page + 1, n_sub);

                // Footer age = time since the newest quote arrived
                time_t newest = 0;
                for (int i = 0; i < count; i++) newest = std::max(newest, g_stock_quotes[i].fetch_time);
                int age_s = (newest > 0) ? (int)(time(nullptr) - newest) : 0;

                display_draw_stocks(label, g_current_page, g_num_pages,
                                    g_stock_quotes, count,
                                    g_stock_sub_page * per_page, per_page, market_open, age_s,
                                    WiFi.status() == WL_CONNECTED, g_battery_pct, g_battery_charging,
                                    weather_str, uv_str,
                                    weather_data.valid && weather_data.rain_today,
                                    weather_data.valid ? weather_data.precip_prob_pct : 0,
                                    weather_data.valid && weather_data.snow_today,
                                    weather_data.valid && weather_data.clear_today);
            }
        } else {
            // Commute page
            bool is_home = (page.type == PageInfo::COMMUTE_HOME);
            CommuteData& cd = is_home ? g_commute_home : g_commute_work;
            const char* label = is_home ? "Work -> Home" : "Home -> Work";
            char weather_str[40];
            char uv_str[56];
            format_footer_weather(weather_str, sizeof(weather_str), uv_str, sizeof(uv_str), "");
            display_draw_commute(label, g_current_page, g_num_pages,
                                 cd, g_commute_conn_idx,
                                 WiFi.status() == WL_CONNECTED, g_battery_pct, g_battery_charging,
                                 weather_str, uv_str,
                                 weather_data.valid && weather_data.rain_today,
                                 weather_data.valid ? weather_data.precip_prob_pct : 0,
                                 weather_data.valid && weather_data.snow_today,
                                 weather_data.valid && weather_data.clear_today);
        }
    }

    delay(20);
}
