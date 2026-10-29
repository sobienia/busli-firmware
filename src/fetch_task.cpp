// ─────────────────────────────────────────────────────────────────────────────
// fetch_task.cpp — Background FreeRTOS task for departure fetching
// ─────────────────────────────────────────────────────────────────────────────
// Runs on core 0 (the WiFi core) to avoid blocking the UI render loop on
// core 1. Each stop's departures are cached so stop-switching is instant.
//
// Fetch cycle: all stops are fetched in round-robin order at a rate that
// completes one full cycle every FETCH_INTERVAL_MS. The currently-displayed
// stop is prioritised when fetch_task_force_refresh() is called.
//
// Thread safety: s_cache[] is protected by s_mutex. The volatile fields
// (s_active_stop, s_force_refresh) are written by core 1 and read by core 0;
// single-word volatile writes are atomic on ESP32-S3 without a mutex.
// ─────────────────────────────────────────────────────────────────────────────

#include "../include/fetch_task.h"
#include "../include/config.h"
#include "../include/http_lock.h"
#include "../include/ota.h"
#include "../include/flight_tracker.h"
#include "../include/parcel_tracker.h"
#include "../include/pager.h"
#include "api.h"
#include "weather.h"
#include "commute.h"
#include "../include/geo.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

static SemaphoreHandle_t s_mutex;
static StopCache         s_cache[8];
static const StopConfig* s_stops          = nullptr;
static int               s_num_stops      = 0;
static volatile int      s_active_stop    = 0;
static volatile bool     s_force_refresh  = false;
static volatile bool     s_force_commute  = false;
static volatile bool     s_ota_enabled    = true;
static volatile FetchPower s_power        = FETCH_POWER_NORMAL;

// Per-stop last-fetch timestamps for adaptive scheduling
static time_t s_stop_last_attempt[8] = {};

// Flight state (registered via fetch_task_init_flights, managed on core 0)
static int    s_flight_count = 0;
static time_t s_flight_last_fetch[2] = {0, 0};
static volatile bool s_flight_forced[2] = {false, false};  // set by main loop (long press)

// Weather per place — the footer shows the weather of the stop on screen.
// Places: transit stops [0, s_num_stops), then commute home, commute work, and
// "here" (the device's own location, for the portfolio page). All configured
// places are kept fresh in the background so switching pages shows the right
// town immediately; the place on screen is refreshed first.
static const int MAX_PLACES = 8 + 3;
struct PlaceWeather {
    GeoPlace    geo;               // coordinates + town (geo module, NVS-cached)
    time_t      geo_tried    = 0;  // last lookup attempt (retried hourly on failure)
    WeatherData wx           = {};
    bool        ok           = false;
    time_t      last_attempt = 0;
};
static PlaceWeather   s_places[MAX_PLACES];
static volatile int   s_weather_place = 0;

// Commute cache
static String      s_home_station;
static String      s_work_station;
static CommuteData s_commute_home = {};
static CommuteData s_commute_work = {};
static bool        s_commute_ok   = false;

// Parcel tracking
static String s_parcel_tracking;
static int    s_parcel_pulses      = 3;

// Pager (ntfy.sh)
static PagerConfig         s_pager_cfg;
static bool                s_pager_ready        = false;
static time_t              s_pager_last_poll    = 0;
static PagerIncoming       s_pager_queue[4];
static int                 s_pager_queue_head   = 0;
static int                 s_pager_queue_tail   = 0;
static volatile time_t     s_pager_last_activity = 0;  // set by main loop on send/receive/open
static String s_parcel_status;
static bool   s_parcel_changed     = false;
static bool   s_parcel_ok          = false;  // at least one fetch succeeded
static time_t s_parcel_delivered_at = 0;     // epoch when "Delivered" was first recorded

// Stocks — s_stock_quotes[] is protected by s_mutex; scheduling state is core-0 only
// except s_stock_forced[], which the main loop sets via fetch_task_force_stocks_refresh().
static StockQuote    s_stock_quotes[STOCKS_MAX];
static volatile int  s_stock_count = 0;
static time_t        s_stock_last_attempt[STOCKS_MAX] = {};
static bool          s_stock_need_close[STOCKS_MAX]   = {};  // last fetch was during market hours
static volatile bool s_stock_forced[STOCKS_MAX]       = {};

// Detail-view chart — fetched on demand for the ticker the user opened
static volatile int  s_chart_request = -1;   // ticker index wanted; -1 = detail view closed
static StockChart    s_chart;                // guarded by s_mutex
static int           s_chart_idx     = -1;   // ticker s_chart belongs to
static time_t        s_chart_time    = 0;

// Lets the UI cut the loop's idle wait short for user-triggered work
static TaskHandle_t  s_task = nullptr;
static void wake_task() { if (s_task) xTaskNotifyGive(s_task); }

static void do_fetch(int idx) {
    std::vector<Departure> fresh;
    http_lock_take();
    bool ok = api_fetch_departures(s_stops[idx], fresh, 6);
    http_lock_give();

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (ok) {
        bool had_data    = !s_cache[idx].departures.empty();
        bool cache_stale = !s_cache[idx].ever_fetched ||
                           (time(nullptr) - s_cache[idx].fetch_time) > 600;

        if (!fresh.empty() || !had_data || cache_stale) {
            // Accept new result: either it has data, we had nothing, or cache is old enough
            // that 0 results likely means genuine end of service.
            s_cache[idx].departures = fresh;
            s_cache[idx].fetch_time = time(nullptr);
            s_cache[idx].from_cache = false;
        }
        // else: transient empty result — keep previous departures, don't update fetch_time
        s_cache[idx].ever_fetched = true;
    } else {
        s_cache[idx].from_cache = true;
    }
    xSemaphoreGive(s_mutex);
}

static int place_here()  { return s_num_stops + 2; }
static int place_count() { return s_num_stops + 3; }

static String place_station(int place) {
    if (place < s_num_stops)      return s_stops[place].station;
    if (place == s_num_stops)     return s_home_station;
    if (place == s_num_stops + 1) return s_work_station;
    return "";
}

// A place is worth fetching weather for if we know (or can find out) where it is
static bool place_configured(int place) {
    return place == place_here() || !place_station(place).isEmpty();
}

// Coordinates for a place, looking them up first if needed (see below)
static void place_coords(int place, float& lat, float& lon);

static bool do_fetch_weather(int place) {
    if (place < 0 || place >= MAX_PLACES) return false;
    PlaceWeather& pw = s_places[place];
    float lat, lon;
    place_coords(place, lat, lon);

    WeatherData wd;
    http_lock_take();
    bool ok = weather_fetch(lat, lon, wd);
    http_lock_give();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    pw.last_attempt = time(nullptr);
    if (ok) { pw.wx = wd; pw.ok = true; }
    xSemaphoreGive(s_mutex);
    return ok;
}

static void place_coords(int place, float& lat, float& lon) {
    PlaceWeather& pw = s_places[place];
    time_t now = time(nullptr);

    // Find where the place is (stations: once, NVS-cached; "here": once per boot)
    if (!pw.geo.valid && (pw.geo_tried == 0 || now - pw.geo_tried >= 3600)) {
        pw.geo_tried = now;
        GeoPlace g;
        bool found = (place == place_here()) ? geo_resolve_here(g)
                                             : geo_resolve_station(place_station(place), g);
        if (found) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            pw.geo = g;
            xSemaphoreGive(s_mutex);
        }
    }
    // Unknown location: "here" falls back to the first stop, everything else to Zürich
    const GeoPlace& g = pw.geo.valid ? pw.geo
                      : (place == place_here() && s_places[0].geo.valid) ? s_places[0].geo
                      : pw.geo;
    lat = g.valid ? g.lat : (float)WEATHER_LAT;
    lon = g.valid ? g.lon : (float)WEATHER_LON;
}

// Weather page (hourly + air quality) for each of its locations: the device's
// own location and/or towns from Settings. Only the location on screen is
// fetched — right when it's shown, then every WEATHER_DETAIL_REFRESH_SEC.
static const int WX_SLOTS = 1 + WEATHER_TOWNS_MAX;
struct WxSlot {
    bool          device    = false;   // true = IP location, else `town`
    String        town;
    GeoPlace      geo;                 // towns: resolved via geocoding
    time_t        geo_tried = 0;
    bool          not_found = false;   // town unknown to the geocoder
    WeatherDetail d;                   // guarded by s_mutex
    time_t        attempt   = 0;       // last fetch attempt (0 = never)
};
static WxSlot         s_wx[WX_SLOTS];
static int            s_wx_count       = 0;
static volatile int   s_wx_active      = 0;
static volatile bool  s_wx_page_active = false;

static void do_fetch_weather_detail(int slot) {
    WxSlot& w = s_wx[slot];
    float lat, lon;
    if (w.device) {
        place_coords(place_here(), lat, lon);
    } else {
        time_t now = time(nullptr);
        if (!w.geo.valid && (w.geo_tried == 0 || now - w.geo_tried >= 3600)) {
            w.geo_tried = now;
            GeoPlace g;
            bool found = geo_resolve_city(w.town, g);
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            if (found) w.geo = g;
            w.not_found = !found;
            xSemaphoreGive(s_mutex);
        }
        if (!w.geo.valid) { w.attempt = time(nullptr); return; }
        lat = w.geo.lat; lon = w.geo.lon;
    }
    WeatherDetail d;
    http_lock_take();
    bool ok = weather_fetch_detail(lat, lon, d);
    http_lock_give();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (ok) w.d = d;
    // Success: full interval; failure: retry in about a minute
    w.attempt = time(nullptr) - (ok ? 0 : WEATHER_DETAIL_REFRESH_SEC - 60);
    xSemaphoreGive(s_mutex);
}

static void do_fetch_commute() {
    if (s_home_station.isEmpty() || s_work_station.isEmpty()) return;
    CommuteData home = {}, work = {};
    // home commute = work→home; work commute = home→work
    http_lock_take(); commute_fetch(s_work_station, s_home_station, home); http_lock_give();
    http_lock_take(); commute_fetch(s_home_station, s_work_station, work); http_lock_give();
    time_t now = time(nullptr);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (home.valid) {
        s_commute_home = home;
    } else {
        // Keep existing valid data; only update fetch_time so the display moves
        // past "Loading..." to "No connections" and doesn't appear frozen.
        s_commute_home.fetch_time = now;
    }
    if (work.valid) {
        s_commute_work = work;
    } else {
        s_commute_work.fetch_time = now;
    }
    s_commute_ok = true;
    xSemaphoreGive(s_mutex);
}

static void do_poll_pager() {
    if (!s_pager_ready || s_pager_cfg.topic.isEmpty()) return;
    std::vector<PagerIncoming> msgs;
    http_lock_take();
    bool ok = pager_poll(s_pager_cfg.topic, s_pager_last_poll, msgs);
    http_lock_give();
    if (!ok || msgs.empty()) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (auto& m : msgs) {
        // Circular buffer — drop oldest if full
        int next = (s_pager_queue_tail + 1) % 4;
        if (next == s_pager_queue_head) s_pager_queue_head = (s_pager_queue_head + 1) % 4;
        s_pager_queue[s_pager_queue_tail] = m;
        s_pager_queue_tail = next;
    }
    xSemaphoreGive(s_mutex);
    Serial.printf("[Pager] %d new message(s)\n", (int)msgs.size());
}

static void do_fetch_parcel() {
    if (s_parcel_tracking.isEmpty()) return;
    String new_status;
    http_lock_take();
    bool ok = parcel_fetch(s_parcel_tracking.c_str(), new_status);
    http_lock_give();
    if (!ok) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool changed = (s_parcel_ok && new_status != s_parcel_status);
    s_parcel_status = new_status;
    s_parcel_ok     = true;
    if (changed) s_parcel_changed = true;
    // Track when "Delivered" was first confirmed so we can clear it after 24 h.
    if (new_status == "Delivered" && s_parcel_delivered_at == 0)
        s_parcel_delivered_at = time(nullptr);
    else if (new_status != "Delivered")
        s_parcel_delivered_at = 0;
    xSemaphoreGive(s_mutex);

    if (changed)
        Serial.printf("[Parcel] Status changed → '%s'\n", new_status.c_str());
}

// A ticker is due when its refresh interval has passed and either the market is
// open, it has no result yet, or it still needs the closing price (its last fetch
// happened during market hours). A forced refresh skips all of that.
static bool stock_due(int i, time_t now, bool market_open) {
    if (s_stock_forced[i]) return true;
    if (s_stock_last_attempt[i] != 0 && now - s_stock_last_attempt[i] < STOCKS_REFRESH_SEC)
        return false;
    if (market_open) return true;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool pending = (s_stock_quotes[i].status == STOCK_PENDING);
    xSemaphoreGive(s_mutex);
    return pending || s_stock_need_close[i];
}

static void do_fetch_stock(int i) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    String symbol = s_stock_quotes[i].symbol;
    xSemaphoreGive(s_mutex);

    StockQuote q;
    http_lock_take();
    StockFetchResult r = stocks_fetch_quote(symbol, q);
    http_lock_give();

    time_t now = time(nullptr);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    StockQuote& c = s_stock_quotes[i];
    if (r == STOCK_FETCH_OK) {
        c.status     = STOCK_OK;
        c.price      = q.price;
        c.change_pct = q.change_pct;
        c.currency   = q.currency;
        c.fetch_time = now;
        c.prev_close = q.prev_close;
        c.spark_n    = q.spark_n;
        memcpy(c.spark, q.spark, sizeof(float) * q.spark_n);
        c.name       = q.name;
        c.open       = q.open;
        c.day_low    = q.day_low;   c.day_high  = q.day_high;
        c.year_low   = q.year_low;  c.year_high = q.year_high;
        s_stock_need_close[i] = stocks_market_open(now);
    } else if (r == STOCK_FETCH_NOT_FOUND) {
        c.status = STOCK_NOT_FOUND;
        s_stock_need_close[i] = false;
    }
    // STOCK_FETCH_ERROR: keep the last good quote; retried after STOCKS_REFRESH_SEC
    xSemaphoreGive(s_mutex);
}

// Weather interval for a place: retry failures after 60 s; slower with the screen off
static long weather_interval(const PlaceWeather& pw) {
    if (!pw.ok) return 60;
    return (s_power == FETCH_POWER_SCREEN_OFF) ? WEATHER_OFF_REFRESH_SEC : WEATHER_REFRESH_SEC;
}

static bool weather_due(int place, time_t now) {
    const PlaceWeather& pw = s_places[place];
    return pw.last_attempt == 0 || now - pw.last_attempt >= weather_interval(pw);
}

static void fetch_task_loop(void* /*param*/) {
    // Startup: departures first — they're what the screen shows. The visible stop
    // goes first, then the others (~3 KB each, so this takes a few seconds).
    // Weather and commute follow; flights (up to a 1 MB download) are left to the
    // normal schedule below so they never delay the first board.
    do_fetch(s_active_stop);
    s_stop_last_attempt[s_active_stop] = time(nullptr);
    for (int i = 0; i < s_num_stops; i++) {
        if (i == s_active_stop) continue;
        do_fetch(i);
        s_stop_last_attempt[i] = time(nullptr);
    }
    do_fetch_weather(s_weather_place);
    do_fetch_commute();

    time_t last_commute_fetch = time(nullptr);
    // First OTA check fires at boot+2min, then every OTA_CHECK_INTERVAL_SEC after that.
    time_t last_ota_check     = time(nullptr) - OTA_CHECK_INTERVAL_SEC + 120;
    time_t last_ntp_sync      = time(nullptr);
    // First parcel check fires at boot+30s to avoid crowding the initial fetches.
    time_t last_parcel_fetch  = time(nullptr) - PARCEL_REFRESH_SEC + 30;
    // First pager poll fires at boot+15s.
    time_t last_pager_poll    = time(nullptr) - PAGER_POLL_SEC + 15;

    for (;;) {
        time_t now = time(nullptr);

        // ── Stock detail chart (user is looking at it — goes first) ───────────
        // Fetched when a ticker is opened; refreshed with the quotes while the
        // market is open.
        {
            int want = s_chart_request;
            if (want >= 0 && want < s_stock_count &&
                (want != s_chart_idx ||
                 (stocks_market_open(now) && now - s_chart_time >= STOCKS_REFRESH_SEC))) {
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                String symbol = s_stock_quotes[want].symbol;
                xSemaphoreGive(s_mutex);
                StockChart chart;
                http_lock_take();
                StockFetchResult r = stocks_fetch_chart(symbol, chart);
                http_lock_give();
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                if (r == STOCK_FETCH_OK) s_chart = chart;
                else                     s_chart.n = 0;   // show "no chart" rather than another ticker's
                s_chart_idx  = want;
                s_chart_time = time(nullptr);
                xSemaphoreGive(s_mutex);
            }
        }

        // ── Weather page (user is looking at it) ──────────────────────────────
        if (s_wx_page_active && s_wx_count > 0) {
            int slot = s_wx_active;
            if (s_wx[slot].attempt == 0 || now - s_wx[slot].attempt >= WEATHER_DETAIL_REFRESH_SEC)
                do_fetch_weather_detail(slot);
        }

        // ── Transit: adaptive scheduling ──────────────────────────────────────
        // Active stop: ACTIVE_REFRESH_SEC. Inactive stops: INACTIVE_REFRESH_SEC.
        // Pick the most overdue stop on each iteration.
        int  fetch_idx   = -1;
        long best_overdue = -1;

        if (s_force_refresh) {
            fetch_idx       = s_active_stop;
            s_force_refresh = false;
        } else {
            FetchPower power = s_power;
            int active_sec   = (power == FETCH_POWER_NORMAL) ? ACTIVE_REFRESH_SEC
                             : (power == FETCH_POWER_NIGHT)  ? NIGHT_ACTIVE_REFRESH_SEC
                                                             : SCREEN_OFF_REFRESH_SEC;
            int inactive_sec = (power == FETCH_POWER_NORMAL) ? INACTIVE_REFRESH_SEC
                             : (power == FETCH_POWER_NIGHT)  ? NIGHT_INACTIVE_REFRESH_SEC
                                                             : SCREEN_OFF_REFRESH_SEC;
            for (int i = 0; i < s_num_stops; i++) {
                int  threshold = (i == s_active_stop) ? active_sec : inactive_sec;
                long overdue   = (long)(now - s_stop_last_attempt[i]) - threshold;
                if (overdue > best_overdue) {
                    best_overdue = overdue;
                    fetch_idx    = i;
                }
            }
        }

        if (fetch_idx >= 0) {
            do_fetch(fetch_idx);
            s_stop_last_attempt[fetch_idx] = time(nullptr);
        }

        // ── Weather ───────────────────────────────────────────────────────────
        // The place on screen first; otherwise one other place that's due, so
        // every stop's town and weather are ready before you swipe to it.
        {
            int wp = s_weather_place;
            if (weather_due(wp, now)) {
                do_fetch_weather(wp);
            } else {
                for (int p = 0; p < place_count() && p < MAX_PLACES; p++) {
                    if (p != wp && place_configured(p) && weather_due(p, now)) {
                        do_fetch_weather(p);
                        break;
                    }
                }
            }
        }

        // ── Commute ───────────────────────────────────────────────────────────
        if (s_force_commute || (now - last_commute_fetch >= COMMUTE_REFRESH_SEC)) {
            s_force_commute = false;
            do_fetch_commute();
            last_commute_fetch = time(nullptr);
        }

        // ── Flights ───────────────────────────────────────────────────────────
        // Refresh one slot per loop iteration to avoid back-to-back blocking calls.
        for (int i = 0; i < s_flight_count; i++) {
            if (s_flight_forced[i] || now - s_flight_last_fetch[i] >= FLIGHT_REFRESH_SEC) {
                s_flight_forced[i] = false;
                flight_tracker_refresh(i);
                s_flight_last_fetch[i] = time(nullptr);
                break;
            }
        }

        // ── OTA ───────────────────────────────────────────────────────────────
        if (s_ota_enabled && now - last_ota_check >= OTA_CHECK_INTERVAL_SEC && !g_ota_pending) {
            String url;
            if (ota_check(url)) {
                g_ota_url     = url;
                g_ota_pending = true;
            }
            last_ota_check = time(nullptr);
        }

        // ── Parcel tracking ───────────────────────────────────────────────────
        if (!s_parcel_tracking.isEmpty() && now - last_parcel_fetch >= PARCEL_REFRESH_SEC) {
            do_fetch_parcel();
            last_parcel_fetch = time(nullptr);
        }

        // ── Pager polling ─────────────────────────────────────────────────────
        // 15 s when pager was used in the last 5 minutes; 60 s when idle.
        if (s_pager_ready) {
            int poll_sec = (now - s_pager_last_activity < 300) ? 15 : 60;
            if (now - last_pager_poll >= poll_sec) {
                do_poll_pager();
                last_pager_poll = time(nullptr);
            }
        }

        // ── Stocks ────────────────────────────────────────────────────────────
        // At most one ticker per iteration so transit refreshes stay responsive.
        // Picks the ticker that has waited longest.
        if (s_stock_count > 0) {
            bool open = stocks_market_open(now);
            int  pick = -1;
            for (int i = 0; i < s_stock_count; i++) {
                if (!stock_due(i, now, open)) continue;
                if (pick < 0 || s_stock_last_attempt[i] < s_stock_last_attempt[pick]) pick = i;
            }
            if (pick >= 0) {
                s_stock_forced[pick] = false;
                do_fetch_stock(pick);
                s_stock_last_attempt[pick] = time(nullptr);
            }
        }

        // ── NTP daily resync ──────────────────────────────────────────────────
        if (now - last_ntp_sync >= NTP_RESYNC_SEC) {
            configTzTime(POSIX_TZ, "pool.ntp.org", "time.nist.gov");
            last_ntp_sync = time(nullptr);
            Serial.println("[NTP] Daily resync triggered");
        }

        // Idle until the next round — or earlier when the UI asks for something
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(FETCH_LOOP_MS));
    }
}

void fetch_task_start(const StopConfig* stops, int num_stops) {
    s_stops     = stops;
    s_num_stops = num_stops;
    s_mutex     = xSemaphoreCreateMutex();
    // Background task fetches stop 0 first on its initial iteration (adaptive scheduler
    // treats all stops as overdue at t=0 and picks active stop first due to lower threshold).
    xTaskCreatePinnedToCore(
        fetch_task_loop, "fetch",
        FETCH_TASK_STACK, nullptr, FETCH_TASK_PRIORITY,
        &s_task, 0   // pin to core 0 (WiFi core)
    );
}

void fetch_task_get(int stop_idx,
                    std::vector<Departure>& out_departures,
                    time_t& out_fetch_time,
                    bool&   out_from_cache) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    out_departures = s_cache[stop_idx].departures;
    out_fetch_time = s_cache[stop_idx].fetch_time;
    out_from_cache = s_cache[stop_idx].from_cache;
    xSemaphoreGive(s_mutex);
}

void fetch_task_set_active_stop(int new_idx) {
    s_active_stop = new_idx;
    // If this stop has never been fetched, prioritise it immediately
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool needs_fetch = !s_cache[new_idx].ever_fetched;
    xSemaphoreGive(s_mutex);
    if (needs_fetch) { s_force_refresh = true; wake_task(); }
}

void fetch_task_force_refresh() {
    s_force_refresh = true;
    wake_task();
}

void fetch_task_set_commute(const String& home, const String& work) {
    s_home_station = home;
    s_work_station = work;
}

void fetch_task_set_weather_place(int place) {
    if      (place == WEATHER_PLACE_HOME) place = s_num_stops;
    else if (place == WEATHER_PLACE_WORK) place = s_num_stops + 1;
    else if (place == WEATHER_PLACE_HERE) place = place_here();
    if (place >= 0 && place < MAX_PLACES) s_weather_place = place;
}

bool fetch_task_get_weather(WeatherData& out, String& out_city) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const PlaceWeather& pw = s_places[s_weather_place];
    bool ok = pw.ok;
    if (ok) { out = pw.wx; out_city = pw.geo.city; }
    xSemaphoreGive(s_mutex);
    return ok;
}

bool fetch_task_get_commute(CommuteData& out_home, CommuteData& out_work) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = s_commute_ok;
    if (ok) { out_home = s_commute_home; out_work = s_commute_work; }
    xSemaphoreGive(s_mutex);
    return ok;
}

void fetch_task_force_commute_refresh() {
    s_force_commute = true;
    wake_task();
}

void fetch_task_set_ota_enabled(bool enabled) {
    s_ota_enabled = enabled;
}

void fetch_task_set_parcel(const String& tracking, int pulses) {
    s_parcel_tracking = tracking;
    s_parcel_pulses   = pulses;
}

bool fetch_task_get_parcel(String& out_status, bool& out_changed) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = s_parcel_ok;
    if (ok) {
        // Suppress "Delivered" after 24 h — go back to showing UV index.
        bool delivered_expired = (s_parcel_status == "Delivered" &&
                                  s_parcel_delivered_at > 0 &&
                                  time(nullptr) - s_parcel_delivered_at >= 86400);
        if (delivered_expired) ok = false;
        else {
            out_status       = s_parcel_status;
            out_changed      = s_parcel_changed;
            s_parcel_changed = false;
        }
    }
    xSemaphoreGive(s_mutex);
    return ok;
}

void fetch_task_set_pager(const PagerConfig& cfg) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_pager_cfg   = cfg;
    s_pager_ready = cfg.topic.length() > 0;
    xSemaphoreGive(s_mutex);
}

bool fetch_task_get_pager_incoming(PagerIncoming& out) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool has = (s_pager_queue_head != s_pager_queue_tail);
    if (has) {
        out = s_pager_queue[s_pager_queue_head];
        s_pager_queue_head = (s_pager_queue_head + 1) % 4;
    }
    xSemaphoreGive(s_mutex);
    return has;
}

void fetch_task_init_flights(const FlightEntry* entries, int count) {
    // Initialise data structures (no HTTP) so the display shows callsign names immediately.
    flight_tracker_init(entries, count);
    s_flight_count = count;
    for (int i = 0; i < count; i++) s_flight_last_fetch[i] = 0; // trigger immediate fetch
}

void fetch_task_set_power_mode(FetchPower mode) {
    if (mode != s_power) {
        s_power = mode;
        Serial.printf("[Fetch] Power mode %d\n", (int)mode);
    }
}

void fetch_task_force_flight_refresh(int slot) {
    if (slot >= 0 && slot < 2) { s_flight_forced[slot] = true; wake_task(); }
}

void fetch_task_set_weather_locations(const WeatherLocations& wl) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = 0;
    if (wl.device) { s_wx[n] = WxSlot(); s_wx[n].device = true; n++; }
    for (int i = 0; i < wl.n_towns && n < WX_SLOTS; i++) {
        s_wx[n] = WxSlot();
        s_wx[n].town = wl.towns[i];
        n++;
    }
    s_wx_count  = n;
    s_wx_active = 0;
    xSemaphoreGive(s_mutex);
}

void fetch_task_force_weather_refresh() {
    s_wx[s_wx_active].attempt = 0;
    wake_task();
}

void fetch_task_set_weather_page(bool active, int slot) {
    if (slot < 0 || slot >= s_wx_count) slot = 0;
    bool changed = (active != s_wx_page_active) || (slot != s_wx_active);
    s_wx_active      = slot;
    s_wx_page_active = active;
    if (changed && active) wake_task();
}

bool fetch_task_get_weather_detail(int slot, WeatherDetail& out, String& out_name, bool& out_not_found) {
    if (slot < 0 || slot >= s_wx_count) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const WxSlot& w = s_wx[slot];
    out_name      = w.device ? s_places[place_here()].geo.city : w.town;
    out_not_found = w.not_found;
    bool ok = w.d.valid;
    if (ok) out = w.d;
    xSemaphoreGive(s_mutex);
    return ok;
}

void fetch_task_request_stock_chart(int idx) {
    if (idx == s_chart_request) return;
    s_chart_request = idx;
    if (idx >= 0) wake_task();
}

bool fetch_task_get_stock_chart(int idx, StockChart& out) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = (idx >= 0 && idx == s_chart_idx);
    if (ok) out = s_chart;
    xSemaphoreGive(s_mutex);
    return ok;
}

void fetch_task_notify_pager_activity() {
    s_pager_last_activity = time(nullptr);
}

void fetch_task_set_stocks(const String symbols[], int count) {
    if (count > STOCKS_MAX) count = STOCKS_MAX;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < count; i++) {
        s_stock_quotes[i]        = StockQuote();
        s_stock_quotes[i].symbol = symbols[i];
        s_stock_last_attempt[i]  = 0;   // fetch everything once at boot
        s_stock_need_close[i]    = false;
    }
    s_stock_count = count;
    xSemaphoreGive(s_mutex);
}

int fetch_task_get_stocks(StockQuote out[]) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = s_stock_count;
    for (int i = 0; i < n; i++) out[i] = s_stock_quotes[i];
    xSemaphoreGive(s_mutex);
    return n;
}

void fetch_task_force_stocks_refresh() {
    for (int i = 0; i < STOCKS_MAX; i++) s_stock_forced[i] = true;
    s_chart_time = 0;   // detail chart too, if one is open
    wake_task();
}
