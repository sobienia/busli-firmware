// ─────────────────────────────────────────────────────────────────────────────
// commute.h — Home/Work commute connection types and fetch interface
// ─────────────────────────────────────────────────────────────────────────────

#pragma once
#include <Arduino.h>
#include "vehicle.h"

struct CommuteLeg {
    String  line;     // e.g. "IC5", "S12", "31"
    String  from;     // departure station name
    String  to;       // arrival station name
    time_t  dep_time; // UTC epoch
    time_t  arr_time; // UTC epoch
    Vehicle vehicle;  // icon when it's leaving now
};

#define COMMUTE_MAX_LEGS         4
#define COMMUTE_MAX_CONNECTIONS  5

struct CommuteConnection {
    CommuteLeg legs[COMMUTE_MAX_LEGS];
    int        leg_count;
    time_t     dep_time;   // first leg departure
};

struct CommuteData {
    CommuteConnection connections[COMMUTE_MAX_CONNECTIONS];
    int    connection_count;
    time_t fetch_time;
    bool   valid;
};

// Fetch up to COMMUTE_MAX_CONNECTIONS connections from→to via transport.opendata.ch.
// Blocks during HTTP call. Returns true if at least one connection was parsed.
bool commute_fetch(const String& from_station,
                   const String& to_station,
                   CommuteData& out);

// The parsing step on its own (also used by the tests in test/): connections in
// a /v1/connections response that leave after now - 1 min.
bool commute_parse(const char* json, size_t json_len, time_t now, CommuteData& out);
