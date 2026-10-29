// ─────────────────────────────────────────────────────────────────────────────
// light_sensor.cpp — LTR-553ALS driver (see include/light_sensor.h)
// ─────────────────────────────────────────────────────────────────────────────

#include "../include/light_sensor.h"
#include "../include/devlog.h"
#include <Wire.h>

static const uint8_t ADDR = 0x23;
enum : uint8_t {
    REG_ALS_CONTR  = 0x80,   // bit0 active, bits4:2 gain (000 = 1x, 011 = 8x, 111 = 96x)
    REG_PS_CONTR   = 0x81,   // bits1:0 = 11 → active
    REG_PS_LED     = 0x82,
    REG_PS_PULSES  = 0x83,
    REG_PS_RATE    = 0x84,
    REG_ALS_RATE   = 0x85,   // bits5:3 integration, bits2:0 repeat rate
    REG_PART_ID    = 0x86,   // 0x92
    REG_MANUFAC_ID = 0x87,   // 0x05
    REG_ALS_DATA   = 0x88,   // CH1 lo, CH1 hi, CH0 lo, CH0 hi (read in this order)
    REG_PS_DATA    = 0x8D,   // lo, hi (bits 2:0; bit 7 = saturated)
};

static bool     s_ok         = false;
static float    s_lux        = -1;
static int      s_ps         = -1;
static float    s_ps_base    = -1;    // proximity with nothing in front (slowly tracked)
static int      s_near_count = 0;
static uint32_t s_ps_ms      = 0;
static uint32_t s_als_ms     = 0;

// A hand counts as near when the reading is this far above the empty baseline,
// for two readings in a row (~200 ms) — a single noisy sample isn't enough.
static const int PS_NEAR_DELTA = 120;

// The sensor sits behind the display glass and gets only a small part of the
// light (a phone flashlight read ~70 lux at gain 8x), so it runs at its most
// sensitive: gain 96x, 400 ms integration. Values are "lux at the sensor",
// far below the room's real lux — the brightness curve is tuned to that.
static const float ALS_GAIN  = 96.0f;
static const float ALS_INT_X = 4.0f;    // 400 ms / 100 ms

static bool write_reg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool read_regs(uint8_t reg, uint8_t* buf, size_t n) {
    Wire.beginTransmission(ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(ADDR, (uint8_t)n) != n) return false;
    for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
    return true;
}

bool light_init() {
    uint8_t id[2];
    if (!read_regs(REG_PART_ID, id, 2) || (id[0] >> 4) != 0x9 || id[1] != 0x05) {
        Log.println("[Light] No light sensor found — automatic brightness off");
        return false;
    }
    s_ok = write_reg(REG_ALS_RATE,  0x1B) &&   // 400 ms integration, every 500 ms
           write_reg(REG_ALS_CONTR, 0x1D) &&   // active, gain 96x (see ALS_GAIN)
           write_reg(REG_PS_LED,    0x7F) &&   // 60 kHz, 100 % duty, 100 mA (default)
           write_reg(REG_PS_PULSES, 0x04) &&   // 4 pulses per reading: steadier
           write_reg(REG_PS_RATE,   0x02) &&   // every 100 ms
           write_reg(REG_PS_CONTR,  0x03);     // active
    Log.printf("[Light] LTR-553 (id 0x%02X) %s\n", id[0], s_ok ? "started" : "setup failed");
    return s_ok;
}

bool light_available() { return s_ok; }

// Lux formula from the LTR-553 appendix (for gain 1x, 100 ms integration)
static float compute_lux(uint16_t ch0, uint16_t ch1) {
    if (ch0 + ch1 == 0) return 0;
    float ratio = (float)ch1 / (ch0 + ch1);
    float lux;
    if      (ratio < 0.45f) lux = 1.7743f * ch0 + 1.1059f * ch1;
    else if (ratio < 0.64f) lux = 4.2785f * ch0 - 1.9548f * ch1;
    else if (ratio < 0.85f) lux = 0.5926f * ch0 + 0.1185f * ch1;
    else                    lux = 0;
    return lux < 0 ? 0 : lux;
}

void light_poll() {
    if (!s_ok) return;
    uint32_t now = millis();

    if (now - s_ps_ms >= 100) {
        s_ps_ms = now;
        uint8_t b[2];
        if (read_regs(REG_PS_DATA, b, 2)) {
            s_ps = b[0] | ((b[1] & 0x07) << 8);
            if (s_ps_base < 0) s_ps_base = s_ps;
            bool above = s_ps > s_ps_base + PS_NEAR_DELTA;
            s_near_count = above ? s_near_count + 1 : 0;
            // Baseline follows the reading slowly while nothing is near (drift,
            // reflections from a case); quickly when the reading drops
            if (!above) s_ps_base += (s_ps - s_ps_base) * (s_ps < s_ps_base ? 0.3f : 0.02f);
        }
    }

    if (now - s_als_ms >= 1000) {
        s_als_ms = now;
        uint8_t b[4];
        if (read_regs(REG_ALS_DATA, b, 4)) {
            uint16_t ch1 = b[0] | (b[1] << 8);
            uint16_t ch0 = b[2] | (b[3] << 8);
            // Saturated (bright daylight on the sensor) → clearly the top of the scale
            float lux = (ch0 == 0xFFFF || ch1 == 0xFFFF) ? 10000 : compute_lux(ch0, ch1) / (ALS_GAIN * ALS_INT_X);
            s_lux = (s_lux < 0) ? lux : s_lux + (lux - s_lux) * 0.4f;
            static uint32_t last_log_ms = 0;
            if (last_log_ms == 0 || now - last_log_ms >= 600000) {
                last_log_ms = now;
                Log.printf("[Light] %.2f lux (ch0 %u ch1 %u), proximity %d (empty %.0f)\n",
                           s_lux, ch0, ch1, s_ps, s_ps_base);
            }
        }
    }
}

float light_lux()           { return s_lux; }
bool  light_near()          { return s_ok && s_near_count >= 2; }
int   light_proximity_raw() { return s_ps; }
