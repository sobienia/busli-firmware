// ─────────────────────────────────────────────────────────────────────────────
// battery.h — Battery level, charging state and remaining-time estimate
// ─────────────────────────────────────────────────────────────────────────────
// Reads the SY6970 charger/PMU (I2C, shared with touch). The percentage comes
// from a Li-ion discharge curve rather than a straight voltage line, and the
// remaining time is estimated from how fast the level has been dropping.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once
#include <Arduino.h>

struct BatteryStatus {
    int   pct        = -1;     // 0–100; -1 = no battery / no reading yet
    bool  charging   = false;
    bool  usb_power  = false;  // USB connected (also when the battery is full and not charging)
    float hours_left = -1;     // estimated runtime; -1 = unknown (charging or not enough data)
};

// Call once in setup() after Wire.begin(). Returns false if the PMU isn't found.
bool battery_init();

// Take a reading. Call about once a minute (the estimate assumes regular samples).
void battery_update();

const BatteryStatus& battery_status();
