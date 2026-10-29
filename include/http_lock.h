#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// HTTP locks -- serialize HTTP operations so TLS sessions (~45 KB RAM each)
// don't pile up. There are two lanes:
//   main lane — everything (departures, weather, stocks, pager, portal, …)
//   slow lane — the task registered with http_lock_set_slow_task(): flights
//               (up to a 1 MB download), parcel (4 requests) and update checks,
//               so they never hold up the main lane
// The lane is picked by the calling task, so callers don't need to know.
// Call http_lock_init() once from setup() before fetch_task_start().
// Wrap every HTTP session: http_lock_take() -> http.begin()...http.end() -> http_lock_give().
void http_lock_init();
void http_lock_set_slow_task(TaskHandle_t task);
void http_lock_take();
void http_lock_give();
