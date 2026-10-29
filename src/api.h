// ─────────────────────────────────────────────────────────────────────────────
// api.h — Swiss transit API client
// ─────────────────────────────────────────────────────────────────────────────
// Fetches live departure data from transport.opendata.ch
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <Arduino.h>
#include <vector>
#include "display.h"   // for the Departure struct

// Configuration for one transit stop
struct StopConfig {
    const char*  label;            // shown in the header (e.g. "ETH Hönggerberg")
    const char*  station;          // exact API station name
    const char** direction_filter; // array of substrings to match destinations
    int          direction_count;  // length of direction_filter array
    const char** line_filter;      // array of allowed line numbers (NULL = all)
    int          line_count;       // length of line_filter array
};

// Fetch departures for a given stop.
// Returns true on success, false on error (in which case `out_departures`
// is left unchanged so the previous data stays on screen).
bool api_fetch_departures(
    const StopConfig& stop,
    std::vector<Departure>& out_departures,
    int max_results
);

// The parsing step on its own (also used by the tests in test/): departures in
// a stationboard response that match the stop's filters and leave after `now`.
bool api_parse_departures(
    const char* json, size_t json_len,
    const StopConfig& stop,
    std::vector<Departure>& out_departures,
    int max_results,
    time_t now
);
