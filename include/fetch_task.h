#pragma once
#include <Arduino.h>
#include <vector>
#include "display.h"   // Departure struct
#include "api.h"       // StopConfig struct

struct StopCache {
    std::vector<Departure> departures;
    time_t fetch_time   = 0;     // epoch of last successful fetch (0 = never)
    bool   from_cache   = false; // true if last fetch failed (showing stale data)
    bool   ever_fetched = false;
};

// Call once from setup() after WiFi is connected.
// Fetches stop 0 synchronously so the display has data immediately,
// then launches a background FreeRTOS task (core 0) for all subsequent fetches.
void fetch_task_start(const StopConfig* stops, int num_stops);

// Read the cached data for a stop. Takes the mutex briefly; never blocks.
void fetch_task_get(int stop_idx,
                    std::vector<Departure>& out_departures,
                    time_t& out_fetch_time,
                    bool&   out_from_cache);

// Tell the fetch task which stop is currently displayed.
// It will prioritize fetching that stop after a stop switch.
void fetch_task_set_active_stop(int new_idx);

// Request an immediate re-fetch of the active stop (long-press action).
void fetch_task_force_refresh();
