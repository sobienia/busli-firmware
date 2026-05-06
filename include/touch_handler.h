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
