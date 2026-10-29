#pragma once
#include <Arduino.h>
#include <vector>
#include "display.h"          // Departure struct
#include "pager.h"            // PagerConfig, PagerIncoming
#include "api.h"              // StopConfig struct
#include "weather.h"          // WeatherData
#include "commute.h"          // CommuteData
#include "flight_tracker.h"   // FlightEntry
#include "stocks.h"           // StockQuote

struct StopCache {
    std::vector<Departure> departures;
    time_t fetch_time   = 0;     // epoch of last successful fetch (0 = never)
    bool   from_cache   = false; // true if last fetch failed (showing stale data)
    bool   ever_fetched = false;
};

// Health of each data source, for the device info page
enum DataSource {
    SRC_DEPARTURES, SRC_WEATHER, SRC_COMMUTE, SRC_STOCKS, SRC_PARCEL, SRC_PAGER,
    SRC_COUNT
};
struct SourceStatus {
    bool   used      = false;  // attempted at least once (= configured)
    time_t last_ok   = 0;
    time_t last_fail = 0;
};
void fetch_task_get_source_status(SourceStatus out[SRC_COUNT]);

// How eagerly to refresh transit data (see TRANSIT_*_SEC in config.h)
enum FetchPower {
    FETCH_POWER_NORMAL,      // day: visible stop every 30 s
    FETCH_POWER_NIGHT,       // night hours, screen on (dimmed)
    FETCH_POWER_SCREEN_OFF,  // screen off: rarely — the board isn't visible
};

// Set from the main loop whenever the screen/night state changes.
void fetch_task_set_power_mode(FetchPower mode);

// Call once from setup() after WiFi is connected. Launches the background
// FreeRTOS task (core 0), which loads all stops' departures first, then the rest.
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

// Which place's weather the footer shows: a transit stop index (0…), or one of:
enum WeatherPlace {
    WEATHER_PLACE_HOME = -1,   // commute home station
    WEATHER_PLACE_WORK = -2,   // commute work station
    WEATHER_PLACE_HERE = -3,   // the device itself (IP location) — portfolio page
};
void fetch_task_set_weather_place(int place);

// Read the latest weather for the current place, plus its town name ("" if
// unknown). Never blocks; returns false if that place has no data yet.
bool fetch_task_get_weather(WeatherData& out, String& out_city);

// Read the latest commute data from the background cache.
// Never blocks; returns false if no data has been fetched yet.
bool fetch_task_get_commute(CommuteData& out_home, CommuteData& out_work);

// Ask the background task to re-fetch commute data on its next iteration.
void fetch_task_force_commute_refresh();

// Register flights with the background task. Call from setup() after fetch_task_start().
// The background task performs all fetches on core 0 — setup() never blocks on HTTP.
void fetch_task_init_flights(const FlightEntry* entries, int count);

// Ask the background task to refresh one flight slot on its next iteration (≤5 s).
// Use this from the UI instead of calling flight_tracker_refresh() directly, so the
// UI never blocks on HTTP and the slot is never refreshed from two cores at once.
void fetch_task_force_flight_refresh(int slot);

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

// Configure pager (ntfy.sh polling). Call from setup() after loading config.
// Polling starts immediately; pass an empty topic to disable.
void fetch_task_set_pager(const PagerConfig& cfg);

// Pop the oldest unread incoming pager message. Returns false if queue is empty.
bool fetch_task_get_pager_incoming(PagerIncoming& out);

// Signal that the pager was just used (send/receive/open). Switches poll interval
// to 15 s for the next 5 minutes, then falls back to 60 s when idle.
void fetch_task_notify_pager_activity();

// Configure stock tickers. Call from setup() after fetch_task_start(); count 0 disables.
// One ticker is fetched per loop iteration (staggered), each every STOCKS_REFRESH_SEC
// while markets are open. Outside market hours only missing quotes and one final
// closing-price pass are fetched.
void fetch_task_set_stocks(const String symbols[], int count);

// Copy the quote cache into out[] (must hold STOCKS_MAX entries). Returns the ticker count.
int  fetch_task_get_stocks(StockQuote out[]);

// Re-fetch all tickers (staggered), even outside market hours.
void fetch_task_force_stocks_refresh();

// Weather page locations (call once from setup(), after fetch_task_start()).
// Slots in order: the device's location (if enabled), then the towns.
void fetch_task_set_weather_locations(const WeatherLocations& wl);

// Is the weather page on screen, and which location (slot)? While active, that
// slot's data (hourly forecast, air quality, pollen) is fetched right away and
// refreshed every WEATHER_DETAIL_REFRESH_SEC.
void fetch_task_set_weather_page(bool active, int slot);

// Long press on the weather page: re-fetch the current location now.
void fetch_task_force_weather_refresh();

// Weather page data for a slot, plus its display name. Returns false until the
// first fetch has completed; out_not_found = the town couldn't be located.
bool fetch_task_get_weather_detail(int slot, WeatherDetail& out, String& out_name,
                                   bool& out_not_found);

// Detail view: ask for ticker idx's fine-grained chart (-1 = detail view closed).
// Fetched right away, then refreshed with the quotes while the market is open.
void fetch_task_request_stock_chart(int idx);

// The chart for ticker idx, once it has arrived (false = still loading).
bool fetch_task_get_stock_chart(int idx, StockChart& out);
