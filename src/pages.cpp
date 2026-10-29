// ─────────────────────────────────────────────────────────────────────────────
// pages.cpp — page list, page switching, kiosk mode and remote control
// ─────────────────────────────────────────────────────────────────────────────

#include "app.h"

static const int MAX_PAGES = 16;
PageInfo g_pages[MAX_PAGES];
int      g_num_pages    = 0;
int      g_current_page = 0;

int      g_current_stop  = 0;
static uint32_t g_page_save_at_ms = 0;       // millis() when the current page should be saved; 0 = nothing pending
static const uint32_t PAGE_SAVE_DELAY_MS = 10000;

// Flight tracking
View g_view         = VIEW_BOARD;
int  g_flight_count = 0;

// Commute — display-side cache; populated from background task via fetch_task_get_commute()
CommuteData g_commute_home  = {};
CommuteData g_commute_work  = {};
String      g_home_station;
String      g_work_station;
int         g_commute_conn_idx = 0;

// Stocks — config from NVS; quotes are copied from the background cache on each redraw
StocksConfig g_stocks_cfg;
StockQuote   g_stock_quotes[STOCKS_MAX];
int          g_stock_sub_page  = 0;   // which group of tickers is on screen
uint32_t     g_stock_rotate_ms = 0;   // millis() of the last sub-page change
int          g_stock_detail    = -1;  // ticker shown in the detail view; -1 = list

// Weather page — which of its locations is shown (swipe up/down to change)
int          g_wx_slot  = 0;
int          g_wx_count = 1;

void step_weather_slot(int delta) {
    if (g_wx_count < 2) return;
    g_wx_slot = ((g_wx_slot + delta) % g_wx_count + g_wx_count) % g_wx_count;
    g_last_redraw_ms = 0;
    display_invalidate();
    Log.printf("[Weather] Page location → %d\n", g_wx_slot);
}

// Build the page list from transit stops + commute pages.
// Call after setup_stops() and after commute is resolved.
bool commute_configured() {
    return g_home_station.length() > 0 && g_work_station.length() > 0;
}

static void add_page(PageInfo::Type type, int idx = 0) {
    if (g_num_pages < MAX_PAGES) g_pages[g_num_pages++] = { type, idx };
}

void setup_pages() {
    g_num_pages = 0;
    bool today_page = config_load_today_page();
    if (today_page) add_page(PageInfo::TODAY);           // the morning overview comes first
    for (int i = 0; i < g_num_stops; i++) add_page(PageInfo::TRANSIT, i);
    // Both stations required: home commute = work→home, work commute = home→work
    if (commute_configured()) {
        add_page(PageInfo::COMMUTE_HOME);
        add_page(PageInfo::COMMUTE_WORK);
    }
    if (g_stocks_cfg.count > 0) add_page(PageInfo::STOCKS);
    bool weather_page = config_load_weather_page();
    if (weather_page) add_page(PageInfo::WEATHER);
    bool info_page = config_load_info_page();
    if (info_page) add_page(PageInfo::DEVICE);
    Log.printf("[Pages] %d pages (today %s, %d transit, commute %s, stocks %s, weather %s, info %s)\n",
                  g_num_pages, today_page ? "on" : "off", g_num_stops,
                  commute_configured() ? "on" : "off",
                  g_stocks_cfg.count > 0 ? "on" : "off",
                  weather_page ? "on" : "off",
                  info_page ? "on" : "off");
}

// Saved page = what it is (type + stop index), not its position, so adding or
// removing pages in Settings doesn't make the device come back on another page
int page_key(const PageInfo& p) { return (int)p.type * 32 + p.transit_idx; }

int find_page_by_key(int key) {
    for (int i = 0; i < g_num_pages; i++) if (page_key(g_pages[i]) == key) return i;
    return -1;
}

// Short name of a page (remote control page list)
String page_name(int i) {
    const PageInfo& p = g_pages[i];
    switch (p.type) {
        case PageInfo::TRANSIT:      return g_stop_configs[p.transit_idx].label;
        case PageInfo::COMMUTE_HOME: return "Work -> Home";
        case PageInfo::COMMUTE_WORK: return "Home -> Work";
        case PageInfo::STOCKS:       return "Portfolio";
        case PageInfo::WEATHER:      return "Weather";
        case PageInfo::DEVICE:       return "Device";
        case PageInfo::TODAY:        return "Today";
    }
    return "?";
}

bool is_commute_page() {
    PageInfo::Type t = g_pages[g_current_page].type;
    return t == PageInfo::COMMUTE_HOME || t == PageInfo::COMMUTE_WORK;
}

// Number of ticker groups on the stocks page (each fits on one screen).
int stock_sub_page_count() {
    int per_page = stocks_per_page(g_stocks_cfg.layout, g_stocks_cfg.count);
    return std::max(1, (g_stocks_cfg.count + per_page - 1) / per_page);
}

void set_stock_sub_page(int idx) {
    int n = stock_sub_page_count();
    g_stock_sub_page  = ((idx % n) + n) % n;
    g_stock_rotate_ms = millis();
    g_last_redraw_ms    = 0;   // force immediate redraw
}

// Detail view: tap a ticker to open it, swipe up/down for the next/previous one,
// tap again (or swipe left/right to another page) to go back.
// idx -1 closes it; the list returns at the sub-page that holds the ticker.
void set_stock_detail(int idx) {
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
    g_last_redraw_ms = 0;
    display_invalidate();
    Log.printf("[Stocks] Detail view → %d\n", idx);
}

// Portfolio tap: open the tapped ticker's details, or close the details
void handle_stock_tap() {
    if (g_stock_detail >= 0) { set_stock_detail(-1); return; }
    int per_page = stocks_per_page(g_stocks_cfg.layout, g_stocks_cfg.count);
    int cell = display_stock_cell_at(touch_last_screen_x(), touch_last_screen_y(), per_page);
    int idx  = (cell >= 0) ? g_stock_sub_page * per_page + cell : -1;
    if (idx >= 0 && idx < g_stocks_cfg.count) set_stock_detail(idx);
}

// Set initial page based on time: morning → work commute, afternoon/evening → home commute.
void set_initial_page() {
    time_t now_t = time(nullptr);
    if (now_t < 1700000000) return; // clock not synced, keep page 0
    struct tm tm_now; localtime_r(&now_t, &tm_now);
    bool is_morning = (tm_now.tm_hour < 12);
    for (int i = 0; i < g_num_pages; i++) {
        if (is_morning && g_pages[i].type == PageInfo::COMMUTE_WORK)  { g_current_page = i; return; }
        if (!is_morning && g_pages[i].type == PageInfo::COMMUTE_HOME) { g_current_page = i; return; }
    }
}

// Tell the fetch task what the page on screen needs:
//  • which departures/commute are visible (refreshed faster than the rest);
//  • the footer weather's place: a transit stop's own location, the commute
//    station you're leaving from, or the device's own location.
void update_fetch_focus() {
    const PageInfo& p = g_pages[g_current_page];
    fetch_task_set_visible(p.type == PageInfo::TRANSIT,
                           p.type == PageInfo::COMMUTE_HOME || p.type == PageInfo::COMMUTE_WORK);
    switch (p.type) {
        case PageInfo::TRANSIT:      fetch_task_set_weather_place(p.transit_idx);        break;
        case PageInfo::COMMUTE_WORK: fetch_task_set_weather_place(WEATHER_PLACE_HOME);   break;  // from home
        case PageInfo::COMMUTE_HOME: fetch_task_set_weather_place(WEATHER_PLACE_WORK);   break;  // from work
        case PageInfo::STOCKS:       fetch_task_set_weather_place(WEATHER_PLACE_HERE);   break;
        case PageInfo::WEATHER:      fetch_task_set_weather_place(WEATHER_PLACE_HERE);   break;
        case PageInfo::DEVICE:       break;   // footer shows the settings address, not weather
        case PageInfo::TODAY:        fetch_task_set_weather_place(WEATHER_PLACE_HERE);   break;
    }
}

// Re-fetch whatever the current page shows (long press, remote "Refresh")
void force_refresh_current_page() {
    switch (g_pages[g_current_page].type) {
        case PageInfo::TRANSIT:
            if (g_view == VIEW_BOARD) fetch_task_force_refresh();
            else fetch_task_force_flight_refresh(g_view == VIEW_FLIGHT1 ? 1 : 0);  // core 0, ~5 s
            break;
        case PageInfo::STOCKS:       fetch_task_force_stocks_refresh();   break;
        case PageInfo::WEATHER:      fetch_task_force_weather_refresh();  break;
        case PageInfo::COMMUTE_HOME:
        case PageInfo::COMMUTE_WORK: fetch_task_force_commute_refresh();  break;
        case PageInfo::TODAY:        fetch_task_force_refresh();          break;  // its departures
        case PageInfo::DEVICE:       break;
    }
}

// by_user = false for kiosk rotation: those pages aren't saved (a write every
// few seconds would wear the flash) and don't count as interaction
void switch_page(int new_page, bool by_user) {
    g_current_page     = new_page;
    g_commute_conn_idx = 0;
    g_view             = VIEW_BOARD;
    g_stock_sub_page   = 0;
    g_stock_rotate_ms  = millis();
    if (g_stock_detail >= 0) { g_stock_detail = -1; fetch_task_request_stock_chart(-1); }
    g_last_redraw_ms     = 0;   // force immediate redraw
    PageInfo& p = g_pages[new_page];
    if (p.type == PageInfo::TRANSIT) {
        g_current_stop = p.transit_idx;
        fetch_task_set_active_stop(g_current_stop);
    }
    update_fetch_focus();
    // Remember the page once the user settles on it (not on every swipe —
    // each NVS write wears flash). Saved by save_page_tick().
    if (by_user) g_page_save_at_ms = millis() + PAGE_SAVE_DELAY_MS;
    display_invalidate();
}

// Come back on the page that was showing before the restart
void restore_saved_page() {
    Preferences prefs;
    prefs.begin("tramli", true);
    int key   = prefs.getInt("page_key", -1);
    int old_i = prefs.getInt("page", -1);   // firmware ≤ 1.7.2 saved the position
    prefs.end();
    int saved = (key >= 0) ? find_page_by_key(key)
              : (old_i >= 0) ? old_i + (g_pages[0].type == PageInfo::TODAY ? 1 : 0)
              : -1;
    if (saved >= 0 && saved < g_num_pages) {
        g_current_page = saved;
        PageInfo& p = g_pages[saved];
        if (p.type == PageInfo::TRANSIT) {
            g_current_stop = p.transit_idx;
            fetch_task_set_active_stop(g_current_stop);
        }
        Log.printf("[Page] Restored page %d from NVS\n", saved);
        update_fetch_focus();
    }
}

void save_page_tick() {
    if (g_page_save_at_ms == 0 || (int32_t)(millis() - g_page_save_at_ms) < 0) return;
    g_page_save_at_ms = 0;
    int key = page_key(g_pages[g_current_page]);
    Preferences prefs;
    prefs.begin("tramli", false);
    if (prefs.getInt("page_key", -1) != key) prefs.putInt("page_key", key);
    prefs.end();
}

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  KIOSK MODE + REMOTE CONTROL                                              ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

KioskConfig g_kiosk;
static uint32_t    g_kiosk_last_ms = 0;   // last automatic page turn
uint32_t    g_last_input_ms = 0;   // last touch/button (or remote action)

// Kiosk mode: turn to the next page every g_kiosk.interval_sec; any interaction
// pauses it for KIOSK_PAUSE_SEC. Not while something else owns the screen.
void kiosk_tick() {
    uint32_t now = millis();
    if (any_input_down()) g_last_input_ms = now;
    if (!g_kiosk.enabled || g_num_pages < 2) return;
    bool busy = pager_ui_active() || g_clock_showing || !display_screen_on() ||
                g_overlay_until_ms != 0 || g_config_hint_stage != 0;
    bool paused = g_last_input_ms != 0 && now - g_last_input_ms < KIOSK_PAUSE_SEC * 1000UL;
    if (busy || paused) { g_kiosk_last_ms = now; return; }   // full interval after a pause
    if (now - g_kiosk_last_ms >= (uint32_t)g_kiosk.interval_sec * 1000UL) {
        g_kiosk_last_ms = now;
        switch_page((g_current_page + 1) % g_num_pages, false);
    }
}

// ── Remote control (Settings → Remote) ───────────────────────────────────────
// The browser page calls these through config_portal (always in loop() context).

String remote_state() {
    JsonDocument doc;
    doc["page"] = g_current_page;
    JsonArray pages = doc["pages"].to<JsonArray>();
    for (int i = 0; i < g_num_pages; i++) pages.add(page_name(i));
    doc["kiosk"]    = g_kiosk.enabled;
    doc["screen"]   = !display_screen_on() ? "off" : g_clock_showing ? "clock" : "on";
    doc["night"]    = display_is_night();
    doc["firmware"] = FIRMWARE_VERSION;
    String out;
    serializeJson(doc, out);
    return out;
}

bool remote_command(const String& cmd, int arg) {
    // Any remote action shows the pages (wakes the screen / leaves the clock)
    g_woken         = true;
    g_woke_at_ms    = millis();
    g_last_input_ms = millis();   // and pauses kiosk rotation like a touch would
    if      (cmd == "page" && arg >= 0 && arg < g_num_pages) switch_page(arg);
    else if (cmd == "next")    switch_page((g_current_page + 1) % g_num_pages);
    else if (cmd == "prev")    switch_page((g_current_page - 1 + g_num_pages) % g_num_pages);
    else if (cmd == "refresh") force_refresh_current_page();
    else if (cmd == "wake")    { /* woken above */ }
    else if (cmd == "bright")  display_step_brightness();
    else if (cmd == "kiosk")   { g_kiosk.enabled = arg != 0; config_save_kiosk(g_kiosk); }
    else return false;
    g_last_redraw_ms = 0;
    Log.printf("[Remote] %s %d\n", cmd.c_str(), arg);
    return true;
}
