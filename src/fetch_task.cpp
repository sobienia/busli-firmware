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
#include "api.h"
#include "weather.h"
#include "commute.h"
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

// Per-stop last-fetch timestamps for adaptive scheduling
static time_t s_stop_last_attempt[8] = {};

// Flight state (registered via fetch_task_init_flights, managed on core 0)
static int    s_flight_count = 0;
static time_t s_flight_last_fetch[2] = {0, 0};

// Weather cache
static WeatherData s_weather   = {};
static bool        s_weather_ok = false;

// Commute cache
static String      s_home_station;
static String      s_work_station;
static CommuteData s_commute_home = {};
static CommuteData s_commute_work = {};
static bool        s_commute_ok   = false;

// Parcel tracking
static String s_parcel_tracking;
static int    s_parcel_pulses      = 3;
static String s_parcel_status;
static bool   s_parcel_changed     = false;
static bool   s_parcel_ok          = false;  // at least one fetch succeeded
static time_t s_parcel_delivered_at = 0;     // epoch when "Delivered" was first recorded

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

static bool do_fetch_weather() {
    WeatherData wd;
    http_lock_take();
    bool ok = weather_fetch(wd);
    http_lock_give();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (ok) { s_weather = wd; s_weather_ok = true; }
    xSemaphoreGive(s_mutex);
    return ok;
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

static void fetch_task_loop(void* /*param*/) {
    // Fetch weather and commute immediately so data is available within seconds of boot.
    do_fetch_weather();
    do_fetch_commute();

    // Initial flight fetches (may block up to ~24s per slot — intentionally on core 0).
    for (int i = 0; i < s_flight_count; i++) {
        flight_tracker_refresh(i);
        s_flight_last_fetch[i] = time(nullptr);
    }

    time_t last_weather_fetch = time(nullptr);
    time_t last_commute_fetch = time(nullptr);
    // First OTA check fires at boot+3min, then every OTA_CHECK_INTERVAL_SEC after that.
    time_t last_ota_check     = time(nullptr) - OTA_CHECK_INTERVAL_SEC + 180;
    time_t last_ntp_sync      = time(nullptr);
    // First parcel check fires at boot+30s to avoid crowding the initial fetches.
    time_t last_parcel_fetch  = time(nullptr) - PARCEL_REFRESH_SEC + 30;

    for (;;) {
        time_t now = time(nullptr);

        // ── Transit: adaptive scheduling ──────────────────────────────────────
        // Active stop: ACTIVE_REFRESH_SEC. Inactive stops: INACTIVE_REFRESH_SEC.
        // Pick the most overdue stop on each iteration.
        int  fetch_idx   = -1;
        long best_overdue = -1;

        if (s_force_refresh) {
            fetch_idx       = s_active_stop;
            s_force_refresh = false;
        } else {
            for (int i = 0; i < s_num_stops; i++) {
                int  threshold = (i == s_active_stop) ? ACTIVE_REFRESH_SEC
                                                      : INACTIVE_REFRESH_SEC;
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
        if (now - last_weather_fetch >= WEATHER_REFRESH_SEC) {
            bool ok = do_fetch_weather();
            // On failure retry in 60 s; on success wait the full refresh interval.
            last_weather_fetch = ok ? time(nullptr)
                                    : time(nullptr) - WEATHER_REFRESH_SEC + 60;
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
            if (now - s_flight_last_fetch[i] >= FLIGHT_REFRESH_SEC) {
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

        // ── NTP daily resync ──────────────────────────────────────────────────
        if (now - last_ntp_sync >= NTP_RESYNC_SEC) {
            configTzTime(POSIX_TZ, "pool.ntp.org", "time.nist.gov");
            last_ntp_sync = time(nullptr);
            Serial.println("[NTP] Daily resync triggered");
        }

        vTaskDelay(pdMS_TO_TICKS(FETCH_LOOP_MS));
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

void fetch_task_init_flights(const FlightEntry* entries, int count) {
    // Initialise data structures (no HTTP) so the display shows callsign names immediately.
    flight_tracker_init(entries, count);
    s_flight_count = count;
    for (int i = 0; i < count; i++) s_flight_last_fetch[i] = 0; // trigger immediate fetch
}
