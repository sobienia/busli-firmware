#pragma once
#include <Arduino.h>

// One stop's editable fields — strings only, no raw pointer arrays.
// Call config_build_stop_configs() after loading to produce StopConfig[].
struct StopEntry {
    String label;       // shown in header
    String station;     // exact API station name
    String lines_csv;   // comma-separated line filter, "" = all
    String dirs_csv;    // comma-separated direction filter, "" = all
};

// One flight tracking slot (ICAO callsign + departure date).
struct FlightEntry {
    String callsign;    // ICAO callsign, e.g. "SWR161"
    String dep_date;    // "YYYY-MM-DD"
};

// Load WiFi credentials from NVS into ssids[]/passes[] (max 3 slots).
// Returns number of networks stored (0 if nothing saved yet).
int  config_load_wifi(String ssids[], String passes[]);

// Load stop entries from NVS into entries[].
// Returns number of stops stored (0 if nothing saved yet).
int  config_load_stops(StopEntry entries[]);

// Load countdown from NVS. Returns true if a countdown is configured.
// target_str format: "YYYY-MM-DD HH:MM"
// icon: 0=none 1=palm 2=calendar 3=plane
bool config_load_countdown(String& label, String& target_str, int& icon);

// Load flight entries from NVS into entries[] (max 2 slots).
// Returns number of configured flights (0–2); slots with empty callsign are skipped.
int  config_load_flights(FlightEntry entries[]);

// Run the AP config portal. Blocks until the user saves (then reboots)
// or until timeoutMs elapses (then returns so normal boot can continue).
// Pass timeoutMs=0 for no timeout.
void config_portal_run(uint32_t timeoutMs = 0);
