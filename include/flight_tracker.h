// ─────────────────────────────────────────────────────────────────────────────
// flight_tracker.h — OpenSky Network flight tracking
// ─────────────────────────────────────────────────────────────────────────────

#pragma once
#include <Arduino.h>
#include "config_portal.h"

struct FlightInfo {
    bool   valid       = false;
    String callsign;           // display name (what user entered, e.g. "TG971")
    String dep_icao;           // departure airport ICAO (e.g. "LSZH"), "" = unknown
    String arr_icao;           // arrival airport ICAO (e.g. "VTBS"),   "" = unknown
    String dep_date;           // departure date from config "YYYY-MM-DD", for display fallback
    time_t dep_time    = 0;    // departure epoch, 0 = unknown (actual or entered, see dep_actual)
    time_t arr_time    = 0;    // arrival epoch,   0 = unknown (estimate or entered, see arr_estimated)
    bool   dep_actual    = false; // dep_time is the real takeoff time (OpenSky track)
    bool   arr_estimated = false; // arr_time is an ETA from live position + speed
    float  alt_ft      = 0;    // current barometric altitude in feet
    float  speed_kmh   = 0;    // current ground speed in km/h
    float  heading     = 0;    // current true track in degrees (0 = North)
    float  vert_rate   = 0;    // vertical rate in m/s (+ climbing, − descending)
    float  lat         = 0;    // current position (valid when has_position)
    float  lon         = 0;
    bool   has_position = false;
    bool   on_ground    = true;  // aircraft on ground right now
    bool   airborne     = false; // live state data available
    time_t fetched_at   = 0;    // unix timestamp of last successful fetch
};

// Set OpenSky Basic Auth credentials. Call before init() or any refresh.
// Without credentials the server ignores the callsign filter (returns ~5 MB — unusable).
void flight_tracker_set_opensky_auth(const String& user, const String& pass);

// Call once in setup() — does a first blocking fetch, caches data.
void flight_tracker_init(const FlightEntry* entries, int count);

// Fetch fresh data for one slot (blocking HTTP — call only from the fetch task;
// the UI uses fetch_task_force_flight_refresh()). Returns true if the cache was updated.
bool flight_tracker_refresh(int slot);

// From 30 min before departure until landing (best known times): refresh more
// often then. False outside, or when the times aren't known.
bool flight_tracker_near_flight(int slot, time_t now);

// Copy cached data for one slot. Thread-safe (call from any context).
void flight_tracker_get(int slot, FlightInfo& out);

// How many flight slots were configured.
int flight_tracker_count();
