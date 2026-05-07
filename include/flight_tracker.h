// ─────────────────────────────────────────────────────────────────────────────
// flight_tracker.h — OpenSky Network flight tracking
// ─────────────────────────────────────────────────────────────────────────────

#pragma once
#include <Arduino.h>
#include "config_portal.h"

struct FlightInfo {
    bool   valid       = false;
    String callsign;           // ICAO callsign as entered by user
    String dep_icao;           // departure airport ICAO (e.g. "LSZH"), "" = unknown
    String arr_icao;           // arrival airport ICAO (e.g. "VTBS"),   "" = unknown
    time_t dep_time    = 0;    // firstSeen unix epoch (departure), 0 = unknown
    time_t arr_time    = 0;    // lastSeen  unix epoch (arrival),   0 = still flying
    float  alt_ft      = 0;    // current barometric altitude in feet
    float  speed_kmh   = 0;    // current ground speed in km/h
    float  heading     = 0;    // current true track in degrees (0 = North)
    bool   on_ground   = true; // aircraft on ground right now
    bool   airborne    = false; // live state data available
    time_t fetched_at  = 0;   // unix timestamp of last successful fetch
};

// Call once in setup() — does a first blocking fetch, caches data.
void flight_tracker_init(const FlightEntry* entries, int count);

// Fetch fresh data for one slot (blocking — call from main loop, not ISR).
// Returns true if the cache was updated.
bool flight_tracker_refresh(int slot);

// Copy cached data for one slot. Thread-safe (call from any context).
void flight_tracker_get(int slot, FlightInfo& out);

// How many flight slots were configured.
int flight_tracker_count();
