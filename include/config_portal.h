#pragma once
#include <Arduino.h>
#include "pager.h"
#include "stocks.h"
#include "config.h"

// One stop's editable fields — strings only, no raw pointer arrays.
// Call config_build_stop_configs() after loading to produce StopConfig[].
struct StopEntry {
    String label;       // shown in header
    String station;     // exact API station name
    String lines_csv;   // comma-separated line filter, "" = all
    String dirs_csv;    // comma-separated direction filter, "" = all
};

// One flight tracking slot (ICAO callsign + departure date + optional airports/times).
struct FlightEntry {
    String callsign;    // ICAO callsign, e.g. "SWR161"
    String dep_date;    // "YYYY-MM-DD"
    String dep_icao;    // departure airport ICAO, e.g. "LSZH" (user-entered, optional)
    String arr_icao;    // arrival airport ICAO,   e.g. "VTBS" (user-entered, optional)
    String dep_time_str; // "HH:MM" local departure time (optional; used when API has no data)
    String arr_time_str; // "HH:MM" local arrival time   (optional; enables progress bar)
};

// Load WiFi credentials from NVS into ssids[]/passes[]/users[] (max 3 slots).
// users[i] is non-empty only for WPA2-Enterprise networks (login username).
// Returns number of networks stored (0 if nothing saved yet).
int  config_load_wifi(String ssids[], String passes[], String users[]);

// Load stop entries from NVS into entries[].
// Returns number of stops stored (0 if nothing saved yet).
int  config_load_stops(StopEntry entries[]);

// Load countdown from NVS. Returns true if a countdown is configured.
// target_str format: "YYYY-MM-DD HH:MM"
// icon: 0=none 1=palm 2=calendar 3=plane
bool config_load_countdown(String& label, String& target_str, int& icon);

// Load flight entries from NVS into entries[] (max 2 slots).
// Returns number of configured flights (0–2); slots with empty callsign are skipped.
int  config_load_flights(FlightEntry entries[]);

// Load OpenSky Network credentials. Returns true if a username is configured.
// Without credentials the callsign filter is ignored server-side (returns all aircraft).
bool config_load_opensky(String& user, String& pass);

struct CommuteConfig {
    String home_station;  // API station name for home
    String work_station;  // API station name for work
};

// Load commute config from NVS. Returns true if at least one station is configured.
bool config_load_commute(CommuteConfig& out);

// Load OTA-enabled flag from NVS. Returns true (default) if the user has not disabled it.
bool config_load_ota_enabled();

// Load parcel tracking config. Returns true if a tracking number is set.
// out_pulses: how many times to pulse brightness on status change (default 3).
bool config_load_parcel(String& out_tracking, int& out_pulses);

// Load pager config (device name, own topic, friends list).
// Auto-generates and persists a topic on first call if none exists.
// Returns true if at least a topic is present.
bool config_load_pager(PagerConfig& out);

// Persist pager config to NVS.
void config_save_pager(const PagerConfig& cfg);

struct StocksConfig {
    String symbols[STOCKS_MAX];  // Yahoo format, e.g. "NESN.SW"; only [0, count) are used
    int    count      = 0;
    int    layout     = STOCKS_LAYOUT_AUTO;
    int    rotate_sec = STOCKS_ROTATE_SEC_DEFAULT;  // sub-page rotation interval
};

// Night mode: what the screen does during these hours.
enum NightMode {
    NIGHT_DIM   = 0,   // normal pages at night brightness
    NIGHT_OFF   = 1,   // screen off; touch/button wakes it for WAKE_ON_TOUCH_SEC
    NIGHT_CLOCK = 2,   // large dim clock; touch/button shows the pages for a while
};
struct NightConfig {
    int start_min = NIGHT_START_HOUR * 60;  // minutes after midnight
    int end_min   = NIGHT_END_HOUR * 60;    // start == end → night mode off
    int mode      = NIGHT_DIM;
};

// Load night-mode settings (defaults when nothing is saved).
void config_load_night(NightConfig& out);

// Show the weather page in the page cycle? (default: yes)
bool config_load_weather_page();

// Show the device info page in the page cycle? (default: yes)
bool config_load_info_page();

// Show the Today page (first in the cycle)? (default: yes)
bool config_load_today_page();

// Kiosk mode: pages turn by themselves every interval_sec
struct KioskConfig {
    bool enabled      = false;
    int  interval_sec = 15;
};
void config_load_kiosk(KioskConfig& out);
void config_save_kiosk(const KioskConfig& cfg);

// Remote control (Settings → Remote): the browser page asks the main program
// for its state and sends it commands. Both run in loop() context.
struct RemoteHooks {
    String (*state)()                                = nullptr;  // JSON for the page
    bool   (*command)(const String& cmd, int arg)    = nullptr;  // false = unknown
};
void config_web_set_remote(const RemoteHooks& hooks);

// Weather page locations, in swipe order: the device's own location (unless
// turned off) followed by up to WEATHER_TOWNS_MAX towns typed in Settings.
#define WEATHER_TOWNS_MAX 3
struct WeatherLocations {
    bool   device  = true;
    String towns[WEATHER_TOWNS_MAX];
    int    n_towns = 0;
};
void config_load_weather_locations(WeatherLocations& out);

// Load stock tickers + display settings. Returns true if at least one ticker is set.
bool config_load_stocks(StocksConfig& out);

// Persist stock config to NVS (removes the key when no tickers are set).
void config_save_stocks(const StocksConfig& cfg);

// Run the AP config portal. Blocks until the user saves (then reboots), until
// timeoutMs elapses (0 = no timeout), or until should_cancel() returns true
// (e.g. the screen was touched) — then returns so normal operation continues.
void config_portal_run(uint32_t timeoutMs = 0, bool (*should_cancel)() = nullptr);

// ── Settings page on the normal Wi-Fi (http://busli.local or the device IP) ──
// Start serving the settings page on the home network. Call once WiFi is up.
void config_web_begin();

// Serve pending requests; reboots after a save. Call every loop() iteration.
void config_web_poll();

// Called with the 6-digit pairing code when a browser asks to pair (show it on
// screen), and with nullptr when the code is used up or expires (hide it) — or
// when a phone has just paired through the QR link.
void config_web_set_pairing_callback(void (*cb)(const char* code));

// Link for the settings QR code: opens the page and pairs in one step (the
// one-time code is part of the link). "" when not connected to Wi-Fi.
String config_web_pair_url();
