// ─────────────────────────────────────────────────────────────────────────────
// geo.h — Station name → coordinates + town name
// ─────────────────────────────────────────────────────────────────────────────
// Used to show the weather (and the town name in the footer) for the stop that's
// on screen. Results are cached in NVS forever, so each station costs two small
// requests once, ever:
//   1. transport.opendata.ch/v1/locations  → coordinates
//   2. nominatim.openstreetmap.org/reverse → town ("Zürich", "Bern", …)
// If the town lookup fails, the part before ", " in the station name is used
// ("Schlieren, Gasometerbrücke" → "Schlieren") and the lookup is retried next boot.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once
#include <Arduino.h>

struct GeoPlace {
    float  lat   = 0;
    float  lon   = 0;
    String city;            // may be "" if unknown
    bool   valid = false;   // coordinates known
};

// Blocking HTTP on a cache miss — call from the fetch task. Takes http_lock itself.
bool geo_resolve_station(const String& station, GeoPlace& out);

// Approximate location of the device itself, from its internet connection's IP
// (ip-api.com, free, no key), named via OpenStreetMap like the stops. Usually
// right to the city; a VPN or company proxy can report somewhere else.
// Blocking HTTP — call from the fetch task. The result is remembered in NVS
// together with the Wi-Fi network it was found on.
bool geo_resolve_here(GeoPlace& out);

// The remembered location, if it was found on the Wi-Fi network we're on now
// (no network request). stale = older than a day — refresh it when convenient.
bool geo_here_cached(GeoPlace& out, bool& stale);

// Coordinates of a town typed by the user ("Bern", "Genève"), via Open-Meteo's
// geocoding (free, no key); NVS-cached. out.city is the name as typed.
// Blocking HTTP on a cache miss — call from the fetch task or the portal.
bool geo_resolve_city(const String& name, GeoPlace& out);
