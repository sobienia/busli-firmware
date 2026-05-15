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
#include "api.h"
#include "weather.h"
#include "commute.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

static SemaphoreHandle_t s_mutex;
static StopCache         s_cache[8];
static const StopConfig* s_stops      = nullptr;
static int               s_num_stops  = 0;
static volatile int      s_active_stop    = 0;
static volatile bool     s_force_refresh  = false;
static volatile bool     s_force_commute  = false;

// Weather cache
static WeatherData s_weather   = {};
static bool        s_weather_ok = false;

// Commute cache
static String      s_home_station;
static String      s_work_station;
static CommuteData s_commute_home = {};
static CommuteData s_commute_work = {};
static bool        s_commute_ok   = false;

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

static void do_fetch_weather() {
    WeatherData wd;
    http_lock_take();
    bool ok = weather_fetch(wd);
    http_lock_give();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (ok) { s_weather = wd; s_weather_ok = true; }
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

static void fetch_task_loop(void* /*param*/) {
    // Fetch weather and commute immediately so data is available within seconds of boot.
    do_fetch_weather();
    do_fetch_commute();

    time_t last_weather_fetch = time(nullptr);
    time_t last_commute_fetch = time(nullptr);
    time_t last_ota_check     = time(nullptr);

    int round_robin_idx = (s_num_stops > 1) ? 1 : 0; // stop 0 already fetched synchronously

    for (;;) {
        // Transit stop fetch (force-refresh or round-robin)
        int fetch_idx;
        if (s_force_refresh) {
            fetch_idx       = s_active_stop;
            s_force_refresh = false;
        } else {
            fetch_idx       = round_robin_idx;
            round_robin_idx = (round_robin_idx + 1) % s_num_stops;
        }
        do_fetch(fetch_idx);

        time_t now = time(nullptr);

        // Weather: refresh every WEATHER_REFRESH_SEC
        if (now - last_weather_fetch >= WEATHER_REFRESH_SEC) {
            do_fetch_weather();
            last_weather_fetch = time(nullptr);
        }

        // Commute: refresh every COMMUTE_REFRESH_SEC, or on demand
        bool commute_due = s_force_commute ||
                           (now - last_commute_fetch >= COMMUTE_REFRESH_SEC);
        if (commute_due) {
            s_force_commute = false;
            do_fetch_commute();
            last_commute_fetch = time(nullptr);
        }

        // OTA: check for firmware updates once per OTA_CHECK_INTERVAL_SEC
        if (now - last_ota_check >= OTA_CHECK_INTERVAL_SEC && !g_ota_pending) {
            String url;
            if (ota_check(url)) {
                g_ota_url     = url;
                g_ota_pending = true;
            }
            last_ota_check = time(nullptr);
        }

        uint32_t delay_ms = (s_num_stops > 0)
                            ? (FETCH_INTERVAL_MS / s_num_stops)
                            : FETCH_INTERVAL_MS;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

void fetch_task_start(const StopConfig* stops, int num_stops) {
    s_stops     = stops;
    s_num_stops = num_stops;
    s_mutex     = xSemaphoreCreateMutex();

    // Fetch stop 0 synchronously so there is data to display immediately
    do_fetch(0);

    xTaskCreatePinnedToCore(
        fetch_task_loop, "fetch",
        FETCH_TASK_STACK, nullptr, FETCH_TASK_PRIORITY,
        nullptr, 0   // pin to core 0 (WiFi core)
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
    if (needs_fetch) s_force_refresh = true;
}

void fetch_task_force_refresh() {
    s_force_refresh = true;
}

void fetch_task_set_commute(const String& home, const String& work) {
    s_home_station = home;
    s_work_station = work;
}

bool fetch_task_get_weather(WeatherData& out) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = s_weather_ok;
    if (ok) out = s_weather;
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
}
