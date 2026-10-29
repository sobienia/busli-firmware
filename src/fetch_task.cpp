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
#include "../include/devlog.h"
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
#include "../include/today_data.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
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
static volatile bool     s_departures_visible = true;    // a stop's board is on screen
static volatile bool     s_commute_visible    = false;   // a commute page is on screen
static volatile bool     s_wake_pending       = false;   // screen back after a quiet night

// Daytime = outside the night mode hours (the main loop sets the power mode)
static bool daytime() { return s_power == FETCH_POWER_NORMAL; }

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
static bool           s_here_stale    = false;   // remembered location > 1 day old

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

// Stocks — s_stock_quotes[] is protected by s_mutex; scheduling state is core-0
// only except the volatile flags, which the main loop sets.
//   Prices: one batch request for all tickers (stocks_fetch_batch), every
//           STOCKS_REFRESH_SEC while markets are open + one pass after close.
//   Details (name, ranges, currency/pence scale): per ticker, once a day,
//           staggered one per loop iteration (stocks_fetch_quote).
static StockQuote    s_stock_quotes[STOCKS_MAX];
static volatile int  s_stock_count = 0;
static time_t        s_stock_last_attempt[STOCKS_MAX] = {};   // details fetch attempts
static time_t        s_stock_batch_last       = 0;            // last batch attempt
static bool          s_stock_batch_need_close = false;        // last batch was during market hours
static volatile bool s_stocks_force           = false;        // long press / remote refresh
static const long    STOCK_DETAILS_SEC        = 86400;        // details are refreshed daily
static volatile bool s_portfolio_visible      = false;        // Portfolio page on screen
static bool          s_has_crypto             = false;        // a coin among the tickers

// Showing tickers in a chosen currency (Settings → Portfolio). The rates
// ("USDPLN=X", …) are fetched in the same request as the prices.
static String        s_show_cur[STOCKS_MAX];                  // "" = the ticker's own
struct FxRate { String pair; float rate; };
static FxRate        s_fx[STOCKS_MAX];                        // guarded by s_mutex
static int           s_fx_n = 0;

// The rate symbol ticker i needs ("" = none: own currency, or an exchange rate itself)
static String fx_pair_for(int i) {
    const StockQuote& q = s_stock_quotes[i];
    const String& to = s_show_cur[i];
    if (to.isEmpty() || q.currency.isEmpty() || q.currency == to || q.symbol.endsWith("=X")) return "";
    return stocks_fx_symbol(q.currency, to);
}

static float fx_rate(const String& pair) {
    for (int k = 0; k < s_fx_n; k++) if (s_fx[k].pair == pair) return s_fx[k].rate;
    return 0;
}

static void fx_store(const String& pair, float rate) {
    for (int k = 0; k < s_fx_n; k++) if (s_fx[k].pair == pair) { s_fx[k].rate = rate; return; }
    if (s_fx_n < STOCKS_MAX) s_fx[s_fx_n++] = { pair, rate };
}

// Ticker i as shown: in its chosen currency once the rate is known
static void to_show_currency(int i, StockQuote& q) {
    if (q.status != STOCK_OK) return;
    String pair = fx_pair_for(i);
    float rate = pair.isEmpty() ? 0 : fx_rate(pair);
    if (rate > 0) stocks_convert(q, rate, s_show_cur[i].c_str());
}

// Detail-view chart — fetched on demand for the ticker the user opened
static volatile int  s_chart_request = -1;   // ticker index wanted; -1 = detail view closed
static StockChart    s_chart;                // guarded by s_mutex
static int           s_chart_idx     = -1;   // ticker s_chart belongs to
static time_t        s_chart_time    = 0;

// Today page extras (only fetched when the page is enabled; guarded by s_mutex)
static volatile bool s_today_enabled = false;
static WaterTemps    s_water;
static time_t        s_water_attempt = 0;
static DailyQuote    s_quote;
static time_t        s_quote_attempt = 0;

static String local_date(time_t t) {
    struct tm tm_l; localtime_r(&t, &tm_l);
    char buf[11];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_l);
    return buf;
}

static void do_fetch_today(time_t now) {
    // Water temperatures: once at startup (any time, so the page has a value),
    // then every 3 h between WATER_FROM_HOUR and WATER_UNTIL_HOUR; a failed
    // attempt is retried after 15 min
    bool water_ok = !isnan(s_water.lake_c) || !isnan(s_water.river_c);
    struct tm lt; localtime_r(&now, &lt);
    bool daytime = lt.tm_hour >= WATER_FROM_HOUR && lt.tm_hour < WATER_UNTIL_HOUR;
    long water_wait = water_ok ? TODAY_WATER_REFRESH_SEC : 900;
    bool water_due = (s_water_attempt == 0) ||
                     (daytime && now - s_water_attempt >= water_wait);
    if (water_due) {
        s_water_attempt = now;
        WaterTemps w;
        http_lock_take();
        bool ok = water_fetch(w);
        http_lock_give();
        if (ok) { xSemaphoreTake(s_mutex, portMAX_DELAY); s_water = w; xSemaphoreGive(s_mutex); }
        return;   // one request batch per loop iteration
    }
    // Quote: once per day (after midnight local time), retry every 30 min
    if (now > 1700000000 && (!s_quote.valid || s_quote.date != local_date(now)) &&
        (s_quote_attempt == 0 || now - s_quote_attempt >= 1800)) {
        s_quote_attempt = now;
        DailyQuote q;
        http_lock_take();
        bool ok = quote_fetch(q);
        http_lock_give();
        if (ok) { xSemaphoreTake(s_mutex, portMAX_DELAY); s_quote = q; xSemaphoreGive(s_mutex); }
    }
}

// Per-source health for the device info page (guarded by s_mutex)
static SourceStatus  s_status[SRC_COUNT];

static void note_result(DataSource src, bool ok) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status[src].used = true;
    (ok ? s_status[src].last_ok : s_status[src].last_fail) = time(nullptr);
    xSemaphoreGive(s_mutex);
}

// Startup handshake: the task waits for setup() to finish configuring it, then
// runs one batch of every fetch; the boot animation waits for that batch.
static volatile bool s_config_done  = false;
static volatile bool s_startup_done = false;

// Lets the UI cut the loop's idle wait short for user-triggered work
static TaskHandle_t  s_task = nullptr;
static void wake_task() { if (s_task) xTaskNotifyGive(s_task); }

static void do_fetch(int idx) {
    std::vector<Departure> fresh;
    http_lock_take();
    bool ok = api_fetch_departures(s_stops[idx], fresh, 6);
    http_lock_give();
    note_result(SRC_DEPARTURES, ok);

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

// Distance in km (flat approximation — fine for a few km)
static float km_between(float lat1, float lon1, float lat2, float lon2) {
    float dx = (lon2 - lon1) * cosf((lat1 + lat2) * 0.5f * DEG_TO_RAD) * 111.32f;
    float dy = (lat2 - lat1) * 110.57f;
    return sqrtf(dx * dx + dy * dy);
}

static bool do_fetch_weather(int place) {
    if (place < 0 || place >= MAX_PLACES) return false;
    PlaceWeather& pw = s_places[place];
    float lat, lon;
    place_coords(place, lat, lon);

    WeatherData wd;
    http_lock_take();
    bool ok = weather_fetch(lat, lon, wd);
    http_lock_give();
    note_result(SRC_WEATHER, ok);
    time_t now = time(nullptr);
    int shared = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    pw.last_attempt = now;
    if (ok) {
        pw.wx = wd; pw.ok = true;
        // Places within WEATHER_SHARE_KM get the same forecast (each keeps its own
        // town name) — Letzipark and Letzipark West don't need two requests
        for (int p = 0; p < place_count() && p < MAX_PLACES; p++) {
            PlaceWeather& o = s_places[p];
            if (p == place || !o.geo.valid || !place_configured(p)) continue;
            if (km_between(lat, lon, o.geo.lat, o.geo.lon) > WEATHER_SHARE_KM) continue;
            o.wx = wd; o.ok = true; o.last_attempt = now;
            shared++;
        }
    }
    xSemaphoreGive(s_mutex);
    if (shared) Log.printf("[Weather] shared with %d nearby place(s)\n", shared);
    return ok;
}

static void place_coords(int place, float& lat, float& lon) {
    PlaceWeather& pw = s_places[place];
    time_t now = time(nullptr);

    // Find where the place is. Stations: once, NVS-cached forever. "Here": the
    // location remembered for this Wi-Fi network if there is one (no request —
    // saves ~3 s at boot; a stale one is refreshed later), else an IP lookup.
    if (!pw.geo.valid && place == place_here()) {
        GeoPlace g;
        bool stale = false;
        if (geo_here_cached(g, stale)) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            pw.geo = g;
            xSemaphoreGive(s_mutex);
            s_here_stale = stale;
        }
    }
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
    note_result(SRC_COMMUTE, home.valid || work.valid);
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
    note_result(SRC_PAGER, ok);
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
    Log.printf("[Pager] %d new message(s)\n", (int)msgs.size());
}

static void do_fetch_parcel() {
    if (s_parcel_tracking.isEmpty()) return;
    String new_status;
    http_lock_take();
    bool ok = parcel_fetch(s_parcel_tracking.c_str(), new_status);
    http_lock_give();
    note_result(SRC_PARCEL, ok);
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
        Log.printf("[Parcel] Status changed → '%s'\n", new_status.c_str());
}

// The price batch is due when its interval has passed and either the market is
// open, a ticker has no price yet, or the closing price is still missing (the
// last batch ran during market hours). A forced refresh skips all of that.
static bool stock_batch_due(time_t now, bool market_open) {
    if (s_stocks_force) return true;
    if (s_power == FETCH_POWER_SCREEN_OFF) return false;   // nobody's looking; caught up on wake
    if (s_stock_batch_last != 0 && now - s_stock_batch_last < STOCKS_REFRESH_SEC) return false;
    if (market_open || s_stock_batch_need_close) return true;
    // Coins trade around the clock: outside market hours they're refreshed
    // only while the Portfolio page is on screen
    if (s_has_crypto && s_portfolio_visible) return true;
    bool pending = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_stock_count; i++) pending |= (s_stock_quotes[i].status == STOCK_PENDING);
    xSemaphoreGive(s_mutex);
    return pending;
}

// Ticker whose details are due (never fetched → soon; otherwise daily), or -1
static int stock_details_due(time_t now) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int pick = -1;
    for (int i = 0; i < s_stock_count && pick < 0; i++) {
        const StockQuote& q = s_stock_quotes[i];
        time_t dt = q.detail_time, la = s_stock_last_attempt[i];
        // Never fetched, or fetched by firmware that didn't keep the session hours
        bool unknown = dt == 0 || (q.status == STOCK_OK && q.session_open < 0 && !q.always_open);
        bool never = unknown && (la == 0 || now - la >= 120);
        bool old   = !unknown && now - dt >= STOCK_DETAILS_SEC && now - la >= 600;
        if (never || old) pick = i;
    }
    xSemaphoreGive(s_mutex);
    return pick;
}

static void do_fetch_stock_batch() {
    // The tickers, then the exchange rates their chosen currencies need (same request)
    static StockQuote q[2 * STOCKS_MAX];   // static: too big for the task stack
    bool found[2 * STOCKS_MAX];
    int n = s_stock_count, m = n;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < n; i++) q[i] = s_stock_quotes[i];
    for (int i = 0; i < n; i++) {
        String pair = fx_pair_for(i);
        bool dup = pair.isEmpty();
        for (int k = n; k < m && !dup; k++) dup = (q[k].symbol == pair);
        if (dup) continue;
        q[m] = StockQuote();
        q[m].symbol      = pair;
        q[m].price_scale = 1.0f;
        m++;
    }
    xSemaphoreGive(s_mutex);

    http_lock_take();
    StockFetchResult r = stocks_fetch_batch(q, m, found);
    http_lock_give();
    note_result(SRC_STOCKS, r == STOCK_FETCH_OK);

    time_t now = time(nullptr);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int k = n; k < m; k++)
        if (found[k] && q[k].price > 0) fx_store(q[k].symbol, q[k].price);
    for (int i = 0; i < n; i++) {
        if (!found[i]) continue;
        StockQuote& c = s_stock_quotes[i];
        c.status     = STOCK_OK;
        c.price      = q[i].price;
        c.change_pct = q[i].change_pct;
        c.prev_close = q[i].prev_close;
        c.spark_n    = q[i].spark_n;
        memcpy(c.spark, q[i].spark, sizeof(float) * q[i].spark_n);
        c.fetch_time = now;
    }
    xSemaphoreGive(s_mutex);
    s_stock_batch_last       = now;
    s_stock_batch_need_close = stocks_market_open(now);
}

// Full quote + details for one ticker (also learns the pence/pound scale)
static void do_fetch_stock(int i) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    String symbol = s_stock_quotes[i].symbol;
    xSemaphoreGive(s_mutex);

    StockQuote q;
    http_lock_take();
    StockFetchResult r = stocks_fetch_quote(symbol, q);
    http_lock_give();
    note_result(SRC_STOCKS, r != STOCK_FETCH_ERROR);   // unknown symbol isn't an outage

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
        c.price_scale = q.price_scale;
        c.detail_time = q.detail_time;
        c.session_open  = q.session_open;
        c.session_close = q.session_close;
        c.gmt_offset    = q.gmt_offset;
        c.always_open   = q.always_open;
        // Now that its currency is known: fetch the rate for its chosen currency soon
        String pair = fx_pair_for(i);
        if (!pair.isEmpty() && fx_rate(pair) == 0) s_stocks_force = true;
    } else if (r == STOCK_FETCH_NOT_FOUND) {
        c.status      = STOCK_NOT_FOUND;
        c.detail_time = now;   // don't keep asking; re-checked with the daily details
    }
    // STOCK_FETCH_ERROR: keep what we have; details are retried after 2 minutes
    xSemaphoreGive(s_mutex);
    s_stock_last_attempt[i] = now;
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

// ── Snapshot: last known data across restarts ────────────────────────────────
// Saved to LittleFS (the 3.4 MB data partition, wear-levelled) every
// SNAPSHOT_SAVE_SEC and after the startup batch; loaded at the next start so
// the first page can show remembered data at once while fresh data loads.
// Entries are matched by station / symbol name, so changed settings simply
// don't find their old entries.
static const char* SNAPSHOT_FILE = "/snapshot.json";
static bool   s_fs_ok           = false;
static time_t s_snapshot_age    = -1;    // seconds old when loaded; -1 = none
static time_t s_snapshot_saved  = 0;

static String place_key(int place) {
    return (place == place_here()) ? String("@here") : place_station(place);
}

static void snapshot_save() {
    if (!s_fs_ok) return;
    JsonDocument doc;
    time_t now = time(nullptr);
    doc["t"] = (long)now;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    JsonArray stops = doc["stops"].to<JsonArray>();
    for (int i = 0; i < s_num_stops; i++) {
        if (!s_cache[i].ever_fetched) continue;
        JsonObject o = stops.add<JsonObject>();
        o["s"] = s_stops[i].station;
        o["f"] = (long)s_cache[i].fetch_time;
        JsonArray deps = o["d"].to<JsonArray>();
        for (const Departure& d : s_cache[i].departures) {
            JsonArray a = deps.add<JsonArray>();
            a.add(d.line); a.add(d.destination); a.add((long)d.dep_time);
            a.add(d.delay); a.add(d.platform); a.add(d.platform_changed); a.add((int)d.vehicle);
        }
    }
    JsonArray wx = doc["wx"].to<JsonArray>();
    for (int p = 0; p < place_count() && p < MAX_PLACES; p++) {
        const PlaceWeather& pw = s_places[p];
        if (!pw.ok) continue;
        JsonObject o = wx.add<JsonObject>();
        o["k"] = place_key(p);
        o["c"] = pw.geo.city;
        o["la"] = pw.geo.lat; o["lo"] = pw.geo.lon; o["gv"] = pw.geo.valid;
        o["at"] = (long)pw.last_attempt;
        const WeatherData& w = pw.wx;
        o["tc"] = w.temp_c; o["tm"] = w.temp_max_c; o["uv"] = w.uv_index; o["um"] = w.uv_index_max;
        o["pp"] = w.precip_prob_pct; o["rt"] = w.rain_today; o["st"] = w.snow_today;
        o["ct"] = w.clear_today; o["rn"] = w.raining_now;
        o["rs"] = (long)w.rain_start; o["re"] = (long)w.rain_end; o["cn"] = w.code_now;
        o["dy"] = w.is_day;
    }
    if (!isnan(s_water.lake_c) || !isnan(s_water.river_c)) {
        JsonObject o = doc["water"].to<JsonObject>();
        if (!isnan(s_water.lake_c))  o["l"] = s_water.lake_c;
        if (!isnan(s_water.river_c)) o["r"] = s_water.river_c;
        o["f"] = (long)s_water.fetched;
    }
    if (s_quote.valid) {
        JsonObject o = doc["quote"].to<JsonObject>();
        o["q"] = s_quote.text; o["a"] = s_quote.author; o["d"] = s_quote.date;
    }
    JsonArray st = doc["stocks"].to<JsonArray>();
    for (int i = 0; i < s_stock_count; i++) {
        const StockQuote& q = s_stock_quotes[i];
        if (q.status == STOCK_PENDING) continue;
        JsonObject o = st.add<JsonObject>();
        o["s"] = q.symbol; o["st"] = (int)q.status;
        o["p"] = q.price; o["c"] = q.change_pct; o["pc"] = q.prev_close; o["cu"] = q.currency;
        o["ft"] = (long)q.fetch_time; o["sc"] = q.price_scale; o["dt"] = (long)q.detail_time;
        o["n"] = q.name; o["o"] = q.open;
        o["dl"] = q.day_low; o["dh"] = q.day_high; o["yl"] = q.year_low; o["yh"] = q.year_high;
        o["so"] = q.session_open; o["se"] = q.session_close; o["go"] = q.gmt_offset;
        o["ao"] = q.always_open;
        JsonArray sp = o["sp"].to<JsonArray>();
        for (int k = 0; k < q.spark_n; k++) sp.add(q.spark[k]);
    }
    JsonObject fx = doc["fx"].to<JsonObject>();   // exchange rates: {"USDPLN=X": 3.62, …}
    for (int k = 0; k < s_fx_n; k++) fx[s_fx[k].pair] = s_fx[k].rate;
    xSemaphoreGive(s_mutex);

    File f = LittleFS.open(SNAPSHOT_FILE, "w");
    if (!f) { Log.println("[Snapshot] can't write"); return; }
    size_t bytes = serializeJson(doc, f);
    f.close();
    s_snapshot_saved = now;
    Log.printf("[Snapshot] saved %u bytes\n", (unsigned)bytes);
}

static void snapshot_load() {
    if (!s_fs_ok || !LittleFS.exists(SNAPSHOT_FILE)) return;
    File f = LittleFS.open(SNAPSHOT_FILE, "r");
    if (!f) return;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) { Log.printf("[Snapshot] unreadable (%s)\n", err.c_str()); return; }

    time_t now = time(nullptr), saved = doc["t"] | 0L;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (JsonObject o : doc["stops"].as<JsonArray>()) {
        String station = o["s"] | "";
        for (int i = 0; i < s_num_stops; i++) {
            if (station != s_stops[i].station) continue;
            s_cache[i].departures.clear();
            for (JsonArray a : o["d"].as<JsonArray>()) {
                Departure d;
                d.line = String(a[0] | ""); d.destination = String(a[1] | "");
                d.dep_time = (time_t)(a[2] | 0L); d.delay = a[3] | 0;
                d.platform = String(a[4] | ""); d.platform_changed = a[5] | false;
                d.vehicle  = (Vehicle)constrain(a[6] | 0, 0, (int)VEHICLE_TRAIN);   // older snapshots: bus
                s_cache[i].departures.push_back(d);
            }
            s_cache[i].fetch_time   = (time_t)(o["f"] | 0L);
            s_cache[i].ever_fetched = true;
            s_cache[i].from_cache   = true;    // footer says "cached" until refreshed
        }
    }
    for (JsonObject o : doc["wx"].as<JsonArray>()) {
        String key = o["k"] | "";
        for (int p = 0; p < place_count() && p < MAX_PLACES; p++) {
            if (key != place_key(p) || key.isEmpty()) continue;
            PlaceWeather& pw = s_places[p];
            pw.geo.city = String(o["c"] | ""); pw.geo.lat = o["la"] | 0.0f; pw.geo.lon = o["lo"] | 0.0f;
            pw.geo.valid = o["gv"] | false;
            WeatherData& w = pw.wx;
            w.temp_c = o["tc"] | 0.0f; w.temp_max_c = o["tm"] | 0.0f;
            w.uv_index = o["uv"] | 0; w.uv_index_max = o["um"] | 0; w.precip_prob_pct = o["pp"] | 0;
            w.rain_today = o["rt"] | false; w.snow_today = o["st"] | false; w.clear_today = o["ct"] | false;
            w.raining_now = o["rn"] | false;
            w.rain_start = (time_t)(o["rs"] | 0L); w.rain_end = (time_t)(o["re"] | 0L);
            w.code_now = o["cn"] | 0;
            w.is_day   = o["dy"] | true;
            w.valid = true;
            pw.ok = true;
            pw.last_attempt = (time_t)(o["at"] | 0L);   // refreshed on its normal schedule
        }
    }
    if (!doc["water"].isNull()) {
        JsonObject o = doc["water"];
        s_water.lake_c  = o["l"].isNull() ? NAN : o["l"].as<float>();
        s_water.river_c = o["r"].isNull() ? NAN : o["r"].as<float>();
        s_water.fetched = (time_t)(o["f"] | 0L);
    }
    if (!doc["quote"].isNull()) {
        s_quote.text   = String(doc["quote"]["q"] | "");
        s_quote.author = String(doc["quote"]["a"] | "");
        s_quote.date   = String(doc["quote"]["d"] | "");
        s_quote.valid  = s_quote.text.length() > 0;
    }
    for (JsonObject o : doc["stocks"].as<JsonArray>()) {
        String sym = o["s"] | "";
        for (int i = 0; i < s_stock_count; i++) {
            StockQuote& q = s_stock_quotes[i];
            if (sym != q.symbol) continue;
            q.status = (StockStatus)(o["st"] | 0);
            q.price = o["p"] | 0.0f; q.change_pct = o["c"] | 0.0f; q.prev_close = o["pc"] | 0.0f;
            q.currency = String(o["cu"] | ""); q.fetch_time = (time_t)(o["ft"] | 0L);
            q.price_scale = o["sc"] | 0.0f; q.detail_time = (time_t)(o["dt"] | 0L);
            q.name = String(o["n"] | ""); q.open = o["o"] | 0.0f;
            q.day_low = o["dl"] | 0.0f; q.day_high = o["dh"] | 0.0f;
            q.year_low = o["yl"] | 0.0f; q.year_high = o["yh"] | 0.0f;
            q.session_open  = o["so"] | -1; q.session_close = o["se"] | -1;
            q.gmt_offset    = o["go"] | 0;  q.always_open   = o["ao"] | false;
            JsonArray sp = o["sp"];
            q.spark_n = 0;
            for (JsonVariant v : sp) if (q.spark_n < STOCK_SPARK_POINTS) q.spark[q.spark_n++] = v.as<float>();
        }
    }
    for (JsonPair p : doc["fx"].as<JsonObject>()) fx_store(p.key().c_str(), p.value() | 0.0f);
    xSemaphoreGive(s_mutex);
    s_snapshot_age = (now > 1700000000 && saved > 0) ? now - saved : -1;
    Log.printf("[Snapshot] loaded (%ld s old)\n", (long)s_snapshot_age);
}

// ── Slow lane ────────────────────────────────────────────────────────────────
// Flights (up to a 1 MB download), the parcel check (4 requests) and update
// checks run in their own task with their own HTTP lane (see http_lock.h), so
// they never delay departures, weather or stocks in fetch_task_loop.
static TaskHandle_t s_slow_task = nullptr;
static void wake_slow_task() { if (s_slow_task) xTaskNotifyGive(s_slow_task); }

static void slow_task_loop(void* /*param*/) {
    while (!s_config_done) vTaskDelay(pdMS_TO_TICKS(20));

    // Parcel status right away (the Today page and footer show it)
    time_t last_parcel_fetch = 0;
    if (!s_parcel_tracking.isEmpty()) {
        do_fetch_parcel();
        last_parcel_fetch = time(nullptr);
    }
    // First update check at boot + 2 min, then every OTA_CHECK_INTERVAL_SEC
    time_t last_ota_check = time(nullptr) - OTA_CHECK_INTERVAL_SEC + 120;

    for (;;) {
        time_t now = time(nullptr);

        // Flights: one slot per round to avoid back-to-back long downloads.
        // Every FLIGHT_REFRESH_SEC; closer around takeoff and in the air.
        for (int i = 0; i < s_flight_count; i++) {
            long every = flight_tracker_near_flight(i, now) ? FLIGHT_ACTIVE_REFRESH_SEC
                                                             : FLIGHT_REFRESH_SEC;
            if (s_flight_forced[i] || now - s_flight_last_fetch[i] >= every) {
                s_flight_forced[i] = false;
                flight_tracker_refresh(i);
                s_flight_last_fetch[i] = time(nullptr);
                break;
            }
        }

        // Firmware update check
        if (s_ota_enabled && now - last_ota_check >= OTA_CHECK_INTERVAL_SEC && !g_ota_pending) {
            String url;
            if (ota_check(url)) {
                g_ota_url     = url;
                g_ota_pending = true;
            }
            last_ota_check = time(nullptr);
        }

        // Parcel tracking: daytime only, and not once it's delivered (a new
        // tracking number in Settings restarts it)
        bool delivered = s_parcel_delivered_at != 0;
        if (!s_parcel_tracking.isEmpty() && !delivered && daytime() &&
            now - last_parcel_fetch >= PARCEL_REFRESH_SEC) {
            do_fetch_parcel();
            last_parcel_fetch = time(nullptr);
        }

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(FETCH_LOOP_MS));
    }
}

static void fetch_task_loop(void* /*param*/) {
    // Wait until setup() has handed over everything (stocks, Today page, …)
    while (!s_config_done) vTaskDelay(pdMS_TO_TICKS(20));

    // ── Startup batch ────────────────────────────────────────────────────────
    // Everything the pages show, once, before the normal schedule — the boot
    // animation stays up until this is done (fetch_task_startup_done()), so the
    // first page appears complete. Departures first (visible stop first), then
    // weather, commute, Today extras, parcel and one quote per ticker. Flights
    // (up to a 1 MB download) are left to the normal schedule.
    do_fetch(s_active_stop);
    s_stop_last_attempt[s_active_stop] = time(nullptr);
    for (int i = 0; i < s_num_stops; i++) {
        if (i == s_active_stop) continue;
        do_fetch(i);
        s_stop_last_attempt[i] = time(nullptr);
    }
    do_fetch_weather(s_weather_place);
    do_fetch_commute();
    if (s_today_enabled) {
        do_fetch_today(time(nullptr));   // water temperatures
        do_fetch_today(time(nullptr));   // quote of the day
    }
    // (the parcel status loads in parallel on the slow lane)
    // Stocks: one batch request for all prices; tickers it couldn't price (no
    // pence/pound scale yet, unknown symbol) get an individual request now
    if (s_stock_count > 0) {
        do_fetch_stock_batch();
        for (int i = 0; i < s_stock_count; i++) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            bool pending = s_stock_quotes[i].status == STOCK_PENDING;
            xSemaphoreGive(s_mutex);
            if (pending) do_fetch_stock(i);
        }
    }
    s_force_refresh = false;   // the page restored at boot asked for its stop: just fetched
    s_startup_done  = true;
    Log.println("[Fetch] Startup batch done");
    snapshot_save();

    time_t last_commute_fetch = time(nullptr);
    time_t last_ntp_sync      = time(nullptr);
    // First pager poll fires at boot+15s.
    time_t last_pager_poll    = time(nullptr) - PAGER_POLL_SEC + 15;

    for (;;) {
        time_t now = time(nullptr);

        // ── Screen back after a quiet night: everything on the pages is due ───
        if (s_wake_pending) {
            s_wake_pending = false;
            for (int i = 0; i < s_num_stops; i++) s_stop_last_attempt[i] = 0;   // stop on screen first
            s_places[s_weather_place].last_attempt = 0;
            if (s_stock_count > 0) s_stocks_force = true;
            Log.println("[Fetch] Screen on — refreshing the pages");
        }

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
                time_t details_age = s_chart_time - s_stock_quotes[want].detail_time;
                xSemaphoreGive(s_mutex);
                // The detail view shows day range/open/52 w — refresh them if they're
                // more than half an hour old (same kept-alive Yahoo connection)
                if (details_age > 1800) do_fetch_stock(want);
            }
        }

        // ── Weather page (user is looking at it) ──────────────────────────────
        if (s_wx_page_active && s_wx_count > 0) {
            int slot = s_wx_active;
            if (s_wx[slot].attempt == 0 || now - s_wx[slot].attempt >= WEATHER_DETAIL_REFRESH_SEC)
                do_fetch_weather_detail(slot);
        }

        // ── Transit: adaptive scheduling ──────────────────────────────────────
        // Stop on screen: ACTIVE_REFRESH_SEC. Other stops (and the last one,
        // while another page shows): INACTIVE_REFRESH_SEC. Slower at night; none
        // while the screen is off / the clock shows (fetch_task_wake() catches
        // up). Pick the most overdue stop on each iteration.
        int  fetch_idx   = -1;
        long best_overdue = -1;
        int  stops_due   = 0;

        if (s_force_refresh) {
            fetch_idx       = s_active_stop;
            s_force_refresh = false;
        } else if (s_power != FETCH_POWER_SCREEN_OFF) {
            bool day         = daytime();
            int inactive_sec = day ? INACTIVE_REFRESH_SEC : NIGHT_INACTIVE_REFRESH_SEC;
            int active_sec   = !s_departures_visible ? inactive_sec
                             : day ? ACTIVE_REFRESH_SEC : NIGHT_ACTIVE_REFRESH_SEC;
            for (int i = 0; i < s_num_stops; i++) {
                int  threshold = (i == s_active_stop) ? active_sec : inactive_sec;
                long overdue   = (long)(now - s_stop_last_attempt[i]) - threshold;
                if (overdue >= 0) stops_due++;
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

        // ── Refresh a day-old remembered "here" location (in the background) ──
        if (s_here_stale) {
            s_here_stale = false;
            GeoPlace g;
            if (geo_resolve_here(g)) {
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_places[place_here()].geo = g;
                xSemaphoreGive(s_mutex);
            }
        }

        // ── Today page extras (water temperatures, quote of the day) ─────────
        if (s_today_enabled) do_fetch_today(now);

        // ── Commute (daytime only; slower while its page isn't showing) ───────
        long commute_sec = s_commute_visible ? COMMUTE_REFRESH_SEC : COMMUTE_BG_REFRESH_SEC;
        if (s_force_commute || (daytime() && now - last_commute_fetch >= commute_sec)) {
            s_force_commute = false;
            do_fetch_commute();
            last_commute_fetch = time(nullptr);
        }

        // (Flights, parcel and update checks run in slow_task_loop)

        // ── Snapshot of the data, for the next start ──────────────────────────
        if (now - s_snapshot_saved >= SNAPSHOT_SAVE_SEC) snapshot_save();

        // ── Pager polling (daytime only) ──────────────────────────────────────
        // PAGER_ACTIVE_POLL_SEC for a while after the pager was used, else PAGER_POLL_SEC
        if (s_pager_ready && daytime()) {
            int poll_sec = (now - s_pager_last_activity < PAGER_ACTIVE_SEC) ? PAGER_ACTIVE_POLL_SEC
                                                                            : PAGER_POLL_SEC;
            if (now - last_pager_poll >= poll_sec) {
                do_poll_pager();
                last_pager_poll = time(nullptr);
            }
        }

        // ── Stocks ────────────────────────────────────────────────────────────
        // Prices: one batch request for all tickers. Details: at most one
        // ticker per iteration so transit refreshes stay responsive.
        if (s_stock_count > 0) {
            if (stock_batch_due(now, stocks_market_open(now))) {
                s_stocks_force = false;
                do_fetch_stock_batch();
            } else if (s_power != FETCH_POWER_SCREEN_OFF) {
                int i = stock_details_due(now);
                if (i >= 0) do_fetch_stock(i);
            }
        }

        // ── NTP daily resync ──────────────────────────────────────────────────
        if (now - last_ntp_sync >= NTP_RESYNC_SEC) {
            configTzTime(POSIX_TZ, "pool.ntp.org", "time.nist.gov");
            last_ntp_sync = time(nullptr);
            Log.println("[NTP] Daily resync triggered");
        }

        // Idle until the next round — or earlier when the UI asks for something.
        // More stops waiting (after waking up): carry on right away.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(stops_due > 1 ? 50 : FETCH_LOOP_MS));
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
    xTaskCreatePinnedToCore(
        slow_task_loop, "fetch_slow",
        FETCH_TASK_STACK, nullptr, FETCH_TASK_PRIORITY,
        &s_slow_task, 0
    );
    http_lock_set_slow_task(s_slow_task);   // its HTTP calls use the slow lane
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
    // Swiped to it: fetch right away, unless its data is still fresh (quick
    // swipes back and forth, kiosk mode)
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const StopCache& c = s_cache[new_idx];
    bool needs_fetch = !c.ever_fetched || time(nullptr) - c.fetch_time >= SWITCH_FRESH_SEC;
    xSemaphoreGive(s_mutex);
    if (needs_fetch) { s_force_refresh = true; wake_task(); }
}

void fetch_task_set_visible(bool departures, bool commute, bool portfolio) {
    s_departures_visible = departures;
    s_commute_visible    = commute;
    s_portfolio_visible  = portfolio;
    if (portfolio) wake_task();   // coins outside market hours: refresh if due
}

void fetch_task_wake() {
    s_wake_pending = true;
    wake_task();
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

void fetch_task_config_done() {
    // Remembered data first, so the pages can show it before anything is fetched.
    // The first mount formats the partition (a few seconds, once ever).
    s_fs_ok = LittleFS.begin(true);
    if (!s_fs_ok) Log.println("[Snapshot] LittleFS not available");
    snapshot_load();
    s_config_done = true;
}

bool fetch_task_startup_done() { return s_startup_done; }

bool fetch_task_snapshot_fresh() {
    return s_snapshot_age >= 0 && s_snapshot_age < SNAPSHOT_FRESH_SEC;
}

void fetch_task_set_today_enabled(bool enabled) {
    s_today_enabled = enabled;
}

void fetch_task_get_today(WaterTemps& water, DailyQuote& quote) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    water = s_water;
    quote = s_quote;
    xSemaphoreGive(s_mutex);
}

void fetch_task_get_source_status(SourceStatus out[SRC_COUNT]) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < SRC_COUNT; i++) out[i] = s_status[i];
    xSemaphoreGive(s_mutex);
}

void fetch_task_set_power_mode(FetchPower mode) {
    if (mode != s_power) {
        s_power = mode;
        Log.printf("[Fetch] Power mode %d\n", (int)mode);
    }
}

void fetch_task_force_flight_refresh(int slot) {
    if (slot >= 0 && slot < 2) { s_flight_forced[slot] = true; wake_slow_task(); }
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
    if (ok) {
        out = s_chart;
        // Same currency as the quote it belongs to (see fetch_task_get_stocks)
        String pair = fx_pair_for(idx);
        float rate  = pair.isEmpty() ? 0 : fx_rate(pair);
        if (rate > 0) stocks_convert_chart(out, rate);
    }
    xSemaphoreGive(s_mutex);
    return ok;
}

void fetch_task_notify_pager_activity() {
    s_pager_last_activity = time(nullptr);
}

void fetch_task_set_stocks(const String symbols[], const String currencies[], int count) {
    if (count > STOCKS_MAX) count = STOCKS_MAX;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < count; i++) {
        s_stock_quotes[i]        = StockQuote();
        s_stock_quotes[i].symbol = symbols[i];
        s_show_cur[i]            = currencies[i];
        s_stock_last_attempt[i]  = 0;
    }
    s_stock_batch_last       = 0;       // prices: fetched once at startup
    s_stock_batch_need_close = false;
    s_stock_count = count;
    s_has_crypto  = false;
    for (int i = 0; i < count; i++) s_has_crypto |= stocks_is_crypto(symbols[i]);
    xSemaphoreGive(s_mutex);
}

int fetch_task_get_stocks(StockQuote out[]) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = s_stock_count;
    for (int i = 0; i < n; i++) {
        out[i] = s_stock_quotes[i];
        to_show_currency(i, out[i]);
    }
    xSemaphoreGive(s_mutex);
    return n;
}

void fetch_task_force_stocks_refresh() {
    s_stocks_force = true;
    s_chart_time   = 0;   // detail chart too, if one is open
    wake_task();
}
