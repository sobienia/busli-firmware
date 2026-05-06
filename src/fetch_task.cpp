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
#include "api.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

static SemaphoreHandle_t s_mutex;
static StopCache         s_cache[8];        // max 8 stops; indexed by stop idx
static const StopConfig* s_stops      = nullptr;
static int               s_num_stops  = 0;
static volatile int      s_active_stop    = 0;
static volatile bool     s_force_refresh  = false;

static void do_fetch(int idx) {
    std::vector<Departure> fresh;
    bool ok = api_fetch_departures(s_stops[idx], fresh, 6);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (ok) {
        s_cache[idx].departures   = fresh;
        s_cache[idx].fetch_time   = time(nullptr);
        s_cache[idx].from_cache   = false;
        s_cache[idx].ever_fetched = true;
    } else {
        s_cache[idx].from_cache = true;
    }
    xSemaphoreGive(s_mutex);
}

static void fetch_task_loop(void* /*param*/) {
    int round_robin_idx = (s_num_stops > 1) ? 1 : 0; // stop 0 already fetched at start

    for (;;) {
        int fetch_idx;
        if (s_force_refresh) {
            fetch_idx      = s_active_stop;
            s_force_refresh = false;
        } else {
            fetch_idx       = round_robin_idx;
            round_robin_idx = (round_robin_idx + 1) % s_num_stops;
        }

        do_fetch(fetch_idx);

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
}

void fetch_task_force_refresh() {
    s_force_refresh = true;
}
