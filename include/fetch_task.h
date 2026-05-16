#pragma once
#include <Arduino.h>
#include <vector>
#include "display.h"          // Departure struct
#include "api.h"              // StopConfig struct
#include "weather.h"          // WeatherData
#include "commute.h"          // CommuteData
#include "flight_tracker.h"   // FlightEntry

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

// Configure commute stations for background fetching.
// Call before fetch_task_start(). Empty strings disable commute fetching.
void fetch_task_set_commute(const String& home, const String& work);

// Read the latest weather / commute data from the background cache.
// Never blocks; returns false if no data has been fetched yet.
bool fetch_task_get_weather(WeatherData& out);
bool fetch_task_get_commute(CommuteData& out_home, CommuteData& out_work);

// Ask the background task to re-fetch commute data on its next iteration.
void fetch_task_force_commute_refresh();

// Register flights with the background task. Call from setup() after fetch_task_start().
// The background task performs all fetches on core 0 — setup() never blocks on HTTP.
void fetch_task_init_flights(const FlightEntry* entries, int count);

// Enable or disable automatic OTA checks. Call from setup() after loading config.
// Default is true; set to false if the user has opted out in the portal.
void fetch_task_set_ota_enabled(bool enabled);

// Configure Swiss Post parcel tracking. Call from setup() after loading config.
// Pass an empty tracking string to disable. pulses = how many times to flash on change.
void fetch_task_set_parcel(const String& tracking, int pulses);

// Read the latest parcel status from the background cache.
// out_changed is set true (once) when the status has changed since last read.
// Returns false if no tracking number is configured or no fetch has completed yet.
bool fetch_task_get_parcel(String& out_status, bool& out_changed);
