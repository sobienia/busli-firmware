// ─────────────────────────────────────────────────────────────────────────────
// today_data.h — Extra data for the Today page: water temperatures + daily quote
// ─────────────────────────────────────────────────────────────────────────────
// Water (free, no key):
//   Lake Zürich  — Stadt Zürich Wasserschutzpolizei station Tiefenbrunnen,
//                  via tecdottir.metaodi.ch (Open Data Zürich)
//   Limmat       — BAFU hydrology station 2243 Baden, via api.existenz.ch
//                  (the Zürich station 2099 measures level/flow, not temperature)
// Quote: zenquotes.io "quote of the day" (free; attribution in Settings).
// ─────────────────────────────────────────────────────────────────────────────

#pragma once
#include <Arduino.h>

struct WaterTemps {
    float  lake_c  = NAN;   // Lake Zürich (NAN = unknown)
    float  river_c = NAN;   // Limmat at Baden
    time_t fetched = 0;
};

struct DailyQuote {
    String text;
    String author;
    String date;            // "YYYY-MM-DD" as reported by the API
    bool   valid = false;
};

// Blocking HTTPS — call from the fetch task, holding http_lock.
// Returns true if at least one of the two temperatures was read.
bool water_fetch(WaterTemps& out);
bool quote_fetch(DailyQuote& out);
