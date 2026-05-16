// ─────────────────────────────────────────────────────────────────────────────
// touch_handler.cpp — CST226SE touch gesture detection
// ─────────────────────────────────────────────────────────────────────────────
// The T-Display S3 Pro uses a CST226SE capacitive touch IC (CST Mutual family)
// at I2C address 0x5A on SDA=GPIO5 / SCL=GPIO6.
// No interrupt pin is wired to the MCU, so we poll touch.read() every loop.
//
// Coordinate note: the CST226SE reports in portrait orientation (x=0..222,
// y=0..480). With display rotation=3 (landscape, USB-C left), raw x maps to
// the screen's vertical axis and raw y maps to the horizontal axis. For swipe
// classification we use raw_y delta for left/right and raw_x delta for up/down.
// ─────────────────────────────────────────────────────────────────────────────

#include "../include/touch_handler.h"
#include "../include/config.h"
#include <Wire.h>
#include <TouchLib.h>

static TouchLib s_touch(Wire, TOUCH_SDA_PIN, TOUCH_SCL_PIN, TOUCH_I2C_ADDR);

// Gesture state
static bool     s_touching          = false;
static int16_t  s_start_raw_x      = 0;
static int16_t  s_start_raw_y      = 0;
static int16_t  s_last_raw_x       = 0;
static int16_t  s_last_raw_y       = 0;
static uint32_t s_touch_start_ms   = 0;
static bool     s_long_press_fired = false;

// Double-tap state
static uint32_t s_last_tap_ms        = 0;
static bool     s_waiting_double_tap = false;

bool touch_is_pressed()    { return s_touching; }
int  touch_last_screen_x() {
    // rotation=3: raw_y maps to the horizontal screen axis (inverted)
    return SCREEN_W - (int)s_last_raw_y * SCREEN_W / 480;
}

int  touch_last_screen_y() {
    // rotation=3: raw_x maps to the vertical screen axis (0=top, SCREEN_H-1=bottom)
    return (int)s_last_raw_x * SCREEN_H / 222;
}

void touch_init() {
    Wire.begin(TOUCH_SDA_PIN, TOUCH_SCL_PIN);
    bool ok = s_touch.init();
    Serial.printf("[Touch] init() = %s (SDA=%d SCL=%d addr=0x%02X)\n",
                  ok ? "OK" : "FAIL",
                  TOUCH_SDA_PIN, TOUCH_SCL_PIN, TOUCH_I2C_ADDR);
}

TouchGesture touch_poll() {
    bool touched = s_touch.read();

    if (touched) {
        TP_Point p = s_touch.getPoint(0);
        s_last_raw_x = p.x;
        s_last_raw_y = p.y;

        if (!s_touching) {
            // Finger just down
            s_touching          = true;
            s_start_raw_x       = p.x;
            s_start_raw_y       = p.y;
            s_touch_start_ms    = millis();
            s_long_press_fired  = false;
        } else {
            // Finger held — check for long press
            if (!s_long_press_fired &&
                millis() - s_touch_start_ms >= TOUCH_LONG_PRESS_MS) {
                s_long_press_fired = true;
                return TOUCH_LONG_PRESS;
            }
        }
    } else if (s_touching) {
        // Finger just lifted
        s_touching = false;

        if (s_long_press_fired) {
            return TOUCH_NONE;   // already fired on hold
        }

        // raw_y maps to the screen's horizontal axis (rotation=3)
        // raw_x maps to the screen's vertical axis
        int dx = s_last_raw_y - s_start_raw_y;  // horizontal on screen
        int dy = s_last_raw_x - s_start_raw_x;  // vertical on screen

        // For rotation=3: screen_x = max_raw_y - raw_y (inverted), screen_y = raw_x
        // So rightward swipe → raw_y decreases → dx < 0
        if (abs(dx) >= TOUCH_SWIPE_PX && abs(dx) > abs(dy)) {
            return (dx < 0) ? TOUCH_SWIPE_RIGHT : TOUCH_SWIPE_LEFT;
        }
        if (abs(dy) >= TOUCH_SWIPE_PX && abs(dy) > abs(dx)) {
            return (dy > 0) ? TOUCH_SWIPE_DOWN : TOUCH_SWIPE_UP;
        }

        // Tap — check for double-tap
        uint32_t now = millis();
        if (s_waiting_double_tap && (now - s_last_tap_ms) < TOUCH_DOUBLE_TAP_MS) {
            s_waiting_double_tap = false;
            return TOUCH_DOUBLE_TAP;
        }
        s_last_tap_ms        = now;
        s_waiting_double_tap = true;
    }

    return TOUCH_NONE;
}
