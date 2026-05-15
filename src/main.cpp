// ─────────────────────────────────────────────────────────────────────────────
// main.cpp — Tramli entry point
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wpa2.h>
#include <Preferences.h>
#include <time.h>
#include <Wire.h>
#define XPOWERS_CHIP_SY6970
#include <XPowersLib.h>

#include "../include/config.h"
#include "../include/secrets.h"
#include "../include/http_lock.h"
#include "../include/fetch_task.h"
#include "../include/ota.h"
#include "../include/touch_handler.h"
#include "../include/config_portal.h"
#include "../include/commute.h"

#include "display.h"
#include "api.h"
#include "weather.h"
#include "../include/flight_tracker.h"
#include <esp_wifi.h>

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
    enum Type { TRANSIT, COMMUTE_HOME, COMMUTE_WORK } type;
    int transit_idx; // only meaningful for TRANSIT pages
};
static PageInfo g_pages[12];
static int      g_num_pages    = 0;
static int      g_current_page = 0;

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
static View g_view         = VIEW_BOARD;
static int  g_flight_count = 0;

// Commute — display-side cache; populated from background task via fetch_task_get_commute()
static CommuteData g_commute_home  = {};
static CommuteData g_commute_work  = {};
static String      g_home_station;
static String      g_work_station;
static int         g_commute_conn_idx = 0;

// Weather — display-side cache; populated from background task via fetch_task_get_weather()
static WeatherData weather_data    = {};
static uint32_t    g_setup_done_ms = 0;  // set at end of setup(); used to detect "still loading"

// Battery — read via SY6970 PMU on the I2C bus shared with touch (SDA=5 SCL=6)
static XPowersPPM pmu;
static bool       s_pmu_ok          = false;
static int        g_battery_pct     = -1;
static bool       g_battery_charging = false;
static uint32_t   last_battery_ms   = 0;

// Button debounce state — BOOT button
static bool     button_was_pressed    = false;
static uint32_t button_press_start_ms = 0;
static bool     config_hint_shown     = false;

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
    Serial.printf("[Pages] %d pages (%d transit, %d commute)\n",
                  g_num_pages, g_num_stops, g_num_pages - g_num_stops);
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

static bool try_connect(const String& ssid, const String& pass,
                        const String& user = "", uint32_t timeout_ms = 8000) {
    if (ssid.isEmpty()) return false;
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

    // Try NVS-stored networks first
    String ssids[3], passes[3], users[3];
    int n = config_load_wifi(ssids, passes, users);
    for (int i = 0; i < n; i++) {
        if (try_connect(ssids[i], passes[i], users[i])) return true;
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

static void update_battery() {
    if (!s_pmu_ok) { g_battery_pct = -1; g_battery_charging = false; return; }
    uint16_t mv = pmu.getBattVoltage();
    // getBattVoltage() returns 0 when the ADC hasn't produced a reading yet
    if (mv == 0) { g_battery_pct = -1; g_battery_charging = false; return; }
    g_battery_charging = pmu.isCharging();
    int pct = (int)((mv - 3200) * 100 / (4200 - 3200));
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    g_battery_pct = pct;
    Serial.printf("[Batt] %umV → %d%%%s\n", mv, pct, g_battery_charging ? " (charging)" : "");
}

static void switch_page(int new_page) {
    g_current_page     = new_page;
    g_commute_conn_idx = 0;
    g_view             = VIEW_BOARD;
    last_redraw_ms     = 0;   // force immediate redraw
    PageInfo& p = g_pages[new_page];
    if (p.type == PageInfo::TRANSIT) {
        current_stop_idx = p.transit_idx;
        fetch_task_set_active_stop(current_stop_idx);
    }
    {
        Preferences prefs;
        prefs.begin("tramli", false);
        prefs.putInt("page", new_page);
        prefs.end();
    }
    display_invalidate();
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
        if (millis() - s_btn_bright_press_start >= 50) {
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

static void check_touch() {
    switch (touch_poll()) {
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
                    flight_tracker_refresh(slot);  // user-triggered: brief core-1 block is OK
                    display_invalidate();
                }
            } else {
                // Commute: ask background task to re-fetch on its next iteration
                fetch_task_force_commute_refresh();
            }
            break;
        case TOUCH_DOUBLE_TAP:
            Serial.println("[Touch] Double tap → toggle font size");
            large_font_mode = !large_font_mode;
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
        default:
            break;
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
    display_init_brightness();
    load_theme();
    {
        Preferences prefs;
        prefs.begin("tramli", true);
        g_display_flipped = prefs.getBool("flipped", false);
        prefs.end();
        if (g_display_flipped) display_set_flipped(true);
    }
    Wire.begin(TOUCH_SDA_PIN, TOUCH_SCL_PIN);
    touch_init();
    {
        if (!pmu.begin(Wire, SY6970_SLAVE_ADDRESS, TOUCH_SDA_PIN, TOUCH_SCL_PIN)) {
            Serial.println("[PMU] SY6970 not found — no battery indicator");
        } else {
            s_pmu_ok = true;
            pmu.enableCharge();
            pmu.enableMeasure();  // start continuous ADC so getBattVoltage() returns real data
            Serial.println("[PMU] SY6970 OK");
        }
    }
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

    setup_pages();
    set_initial_page();

    http_lock_init();
    fetch_task_start(g_stop_configs, g_num_stops);
    fetch_task_set_ota_enabled(config_load_ota_enabled());

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
    check_button();
    check_btn_bright();
    check_btn_zoom();
    check_btn_theme();
    check_touch();

    uint32_t now_ms = millis();

    // Poll PMU once per minute (I2C read; no need for higher rate)
    if (last_battery_ms == 0 || now_ms - last_battery_ms >= 60000) {
        last_battery_ms = now_ms;
        update_battery();
    }

    // Apply pending OTA update (flag set by background task when a newer version is found)
    if (g_ota_pending) {
        g_ota_pending = false;
        display_show_status("Firmware update found\nInstalling...");
        if (ota_apply(g_ota_url)) {
            display_show_status("Update complete!\nRestarting...");
            delay(2000);
            ESP.restart();
        } else {
            display_show_status("Update failed");
            delay(3000);
            display_invalidate();
        }
    }

    // Pull latest weather + commute from background cache once per second
    fetch_task_get_weather(weather_data);
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
    if (now_ms - last_redraw_ms >= 1000) {
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

                char weather_str[20] = "";
                char uv_str[10]      = "";
                if (weather_data.valid) {
                    snprintf(weather_str, sizeof(weather_str), "%dC/%dC",
                             (int)roundf(weather_data.temp_c),
                             (int)roundf(weather_data.temp_max_c));
                    snprintf(uv_str, sizeof(uv_str), "UV%d/%d",
                             weather_data.uv_index, weather_data.uv_index_max);
                } else if (g_setup_done_ms > 0 && millis() - g_setup_done_ms > 90000) {
                    strncpy(weather_str, "No weather", sizeof(weather_str) - 1);
                }

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

        } else {
            // Commute page
            bool is_home = (page.type == PageInfo::COMMUTE_HOME);
            CommuteData& cd = is_home ? g_commute_home : g_commute_work;
            const char* label = is_home ? "Work -> Home" : "Home -> Work";
            char weather_str[20] = "";
            char uv_str[10]      = "";
            if (weather_data.valid) {
                snprintf(weather_str, sizeof(weather_str), "%dC/%dC",
                         (int)roundf(weather_data.temp_c),
                         (int)roundf(weather_data.temp_max_c));
                snprintf(uv_str, sizeof(uv_str), "UV%d/%d",
                         weather_data.uv_index, weather_data.uv_index_max);
            } else if (g_setup_done_ms > 0 && millis() - g_setup_done_ms > 90000) {
                strncpy(weather_str, "No weather", sizeof(weather_str) - 1);
            }
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

    // Reconnect WiFi if dropped — 30s interval avoids AUTH_LEAVE floods
    static uint32_t last_wifi_check = 0;
    if (now_ms - last_wifi_check > 30000) {
        last_wifi_check = now_ms;
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[WiFi] Disconnected — reconnecting...");
            WiFi.disconnect();
            delay(100);
            if (connect_wifi()) {
                esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
                configTzTime(POSIX_TZ, "pool.ntp.org", "time.nist.gov");
            }
        }
    }

    delay(20);
}
