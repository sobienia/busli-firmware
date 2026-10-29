// ─────────────────────────────────────────────────────────────────────────────
// app.h — state and functions shared by the main-loop modules
// ─────────────────────────────────────────────────────────────────────────────
// Everything here runs on the Arduino loop task (core 1); the background
// fetch task (core 0) is only reached through fetch_task_*().
//
//   main.cpp        setup() + loop()
//   stops.cpp       transit stop table
//   pages.cpp       page list, switching, kiosk mode, remote control
//   page_views.cpp  drawing the current page
//   pager_ui.cpp    pager screens
//   input.cpp       buttons, touch gestures, color themes
//   screen.cpp      overlays, setup hotspot, night mode / screen power
//   power.cpp       USB/battery power profile, battery warnings
//   wifi_conn.cpp   Wi-Fi connect/reconnect, clock sync
//   ota_ui.cpp      update prompt
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wpa2.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <Preferences.h>
#include <ArduinoJson.h>
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
#include "../include/flight_tracker.h"
#include "../include/devlog.h"
#include "../include/light_sensor.h"

#include "display.h"
#include "api.h"
#include "weather.h"

// ── stops.cpp ────────────────────────────────────────────────────────────────
extern StopConfig g_stop_configs[8];
extern int        g_num_stops;
void setup_stops();

// ── pages.cpp ────────────────────────────────────────────────────────────────
struct PageInfo {
    // Values are stored in NVS (saved page) — append new types, don't reorder
    enum Type { TRANSIT, COMMUTE_HOME, COMMUTE_WORK, STOCKS, WEATHER, DEVICE, TODAY } type;
    int transit_idx; // only meaningful for TRANSIT pages
};
extern PageInfo g_pages[];
extern int      g_num_pages;
extern int      g_current_page;
extern int      g_current_stop;

// Transit page: departures or one of the tracked flights (swipe up/down)
enum View { VIEW_BOARD, VIEW_FLIGHT0, VIEW_FLIGHT1 };
extern View g_view;
extern int  g_flight_count;

// Commute — display-side copy of the background cache
extern CommuteData g_commute_home, g_commute_work;
extern String      g_home_station, g_work_station;
extern int         g_commute_conn_idx;
bool commute_configured();
bool is_commute_page();

// Portfolio
extern StocksConfig g_stocks_cfg;
extern StockQuote   g_stock_quotes[STOCKS_MAX];
extern int          g_stock_sub_page;
extern uint32_t     g_stock_rotate_ms;
extern int          g_stock_detail;
int  stock_sub_page_count();
void set_stock_sub_page(int idx);
void set_stock_detail(int idx);
void handle_stock_tap();

// Weather page location (slot)
extern int g_wx_slot, g_wx_count;
void step_weather_slot(int delta);

void   setup_pages();
void   set_initial_page();
int    page_key(const PageInfo& p);
int    find_page_by_key(int key);
String page_name(int i);
void   update_weather_place();
void   force_refresh_current_page();
// by_user = false for kiosk rotation (not saved, not counted as interaction)
void   switch_page(int new_page, bool by_user = true);
void   save_page_tick();
void   restore_saved_page();

extern KioskConfig g_kiosk;
extern uint32_t    g_last_input_ms;
void   kiosk_tick();
String remote_state();
bool   remote_command(const String& cmd, int arg);

// ── page_views.cpp ───────────────────────────────────────────────────────────
extern WeatherData g_weather;            // weather of the place on screen
extern String      g_weather_city;
extern uint32_t    g_setup_done_ms;
extern String      g_parcel_status;
extern int         g_parcel_pulses;
extern bool        g_large_font;
extern time_t      g_countdown_target;
extern int         g_countdown_icon;
extern uint32_t    g_last_redraw_ms;     // 0 = redraw on the next loop()
void views_pull_data();
void draw_current_page(uint32_t now_ms);

// ── pager_ui.cpp ─────────────────────────────────────────────────────────────
extern bool g_pager_dirty;               // pager screen needs repainting
void pager_ui_init();
bool pager_ui_active();
void pager_ui_toggle();
bool pager_ui_tick(bool swallow_input);

// ── input.cpp ────────────────────────────────────────────────────────────────
extern int  g_config_hint_stage;         // button-hold prompt on screen (0 = none)
extern bool g_display_flipped;
void load_theme();
void load_display_prefs();
void check_button();
void check_btn_bright();
void check_btn_zoom();
void check_btn_theme();
void check_touch(TouchGesture gesture);

// ── screen.cpp ───────────────────────────────────────────────────────────────
extern uint32_t    g_overlay_until_ms;
extern bool        g_input_swallowed;
extern NightConfig g_night;
extern bool        g_woken;
extern uint32_t    g_woke_at_ms;
extern bool        g_clock_showing;
bool any_input_down();
void hold_overlay(uint32_t duration_ms);
void show_overlay(const char* msg, uint32_t duration_ms);
void show_settings_qr();
void on_pairing_code(const char* code);
bool overlay_active();
void run_setup_portal();
bool power_tick();

// ── power.cpp ────────────────────────────────────────────────────────────────
extern int  g_battery_pct;
extern bool g_battery_charging;
void apply_wifi_power_save();
void set_power_profile(bool usb);
void update_battery();
void battery_tick();
void power_end_boot_boost();

// ── wifi_conn.cpp ────────────────────────────────────────────────────────────
bool connect_wifi();
void wifi_reconnect_tick();
void sync_clock();

// ── ota_ui.cpp ───────────────────────────────────────────────────────────────
extern String g_ota_failed_version;   // update rolled back at this start
void ota_report_rollback();
void ota_prompt_tick();                // also confirms a new firmware on trial
