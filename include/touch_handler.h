#pragma once
#include <Arduino.h>

enum TouchGesture {
    TOUCH_NONE = 0,
    TOUCH_SWIPE_LEFT,
    TOUCH_SWIPE_RIGHT,
    TOUCH_SWIPE_UP,
    TOUCH_SWIPE_DOWN,
    TOUCH_LONG_PRESS,
    TOUCH_DOUBLE_TAP,
};

// Call once in setup() after display_init() — starts I2C and initialises the IC.
void touch_init();

// Call every loop iteration. Returns the detected gesture (or TOUCH_NONE).
// Each gesture is returned exactly once per event.
TouchGesture touch_poll();

// Raw state — call touch_poll() first to update.
// Use these inside blocking prompt loops to detect finger-up and tap position.
bool touch_is_pressed();       // true while screen is being touched
int  touch_last_screen_x();    // screen X of last touch (0=left .. SCREEN_W-1=right)
