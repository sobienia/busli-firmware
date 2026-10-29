// ─────────────────────────────────────────────────────────────────────────────
// light_sensor.h — LTR-553ALS ambient light + proximity sensor (T-Display S3 Pro)
// ─────────────────────────────────────────────────────────────────────────────
// On the shared I2C bus (Wire, same as touch and the charger chip) at 0x23. Not
// every board has it fitted: light_init() returns false then, and everything
// that uses it (automatic brightness, wave-to-wake) stays off.
// All calls from the loop task (the bus isn't shared with core 0).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <Arduino.h>

// After Wire.begin(). True if the sensor answered and was started.
bool  light_init();
bool  light_available();

// Call every loop(): reads proximity every 100 ms, light every second
void  light_poll();

// Smoothed ambient light in lux (-1 = no reading yet)
float light_lux();

// Something (a hand) is close in front of the screen
bool  light_near();

// Raw proximity count 0–2047 (Device page / tuning), -1 = none
int   light_proximity_raw();
