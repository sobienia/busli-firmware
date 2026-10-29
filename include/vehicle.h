// ─────────────────────────────────────────────────────────────────────────────
// vehicle.h — kind of vehicle, for the "leaving now" icon (bus / tram / train)
// ─────────────────────────────────────────────────────────────────────────────
// transport.opendata.ch reports a category per departure/journey: "B" bus,
// "T" tram, "S"/"IR"/"IC"/… trains, "BAT" boat, "FUN"/"PB" funicular/cable car.
// Anything not listed below shows the bus icon (as all departures did before).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <string.h>
#include <stdint.h>

enum Vehicle : uint8_t { VEHICLE_BUS = 0, VEHICLE_TRAM, VEHICLE_TRAIN };

inline Vehicle vehicle_from_category(const char* cat) {
    static const char* const TRAM[]  = { "T", "NFT", "TRAM" };
    static const char* const TRAIN[] = { "S", "SN", "R", "RE", "IR", "IRE", "IC", "ICN", "ICE",
                                         "EC", "EN", "NJ", "TGV", "RJ", "RJX", "PE", "EXT", "M" };
    if (!cat) return VEHICLE_BUS;
    for (const char* c : TRAM)  if (strcmp(cat, c) == 0) return VEHICLE_TRAM;
    for (const char* c : TRAIN) if (strcmp(cat, c) == 0) return VEHICLE_TRAIN;
    return VEHICLE_BUS;
}
