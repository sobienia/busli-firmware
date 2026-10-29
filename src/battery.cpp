// ─────────────────────────────────────────────────────────────────────────────
// battery.cpp — SY6970 battery reading, Li-ion curve, runtime estimate
// ─────────────────────────────────────────────────────────────────────────────

#include "../include/battery.h"
#include "../include/devlog.h"
#include "../include/config.h"
#include <Wire.h>
#define XPOWERS_CHIP_SY6970
#include <XPowersLib.h>

static XPowersPPM    s_pmu;
static bool          s_ok = false;
static BatteryStatus s_status;
static float         s_mv_smooth = 0;      // smoothed voltage (mV); 0 = no reading yet
static bool          s_last_charging = false;

// Resting Li-ion voltage → state of charge. Typical single-cell curve; the flat
// middle section is why a straight 3.2–4.2 V line reads far too low/high.
static const struct { uint16_t mv; uint8_t pct; } CURVE[] = {
    {4200, 100}, {4100, 90}, {4000, 80}, {3920, 70}, {3850, 60},
    {3790,  50}, {3750, 40}, {3710, 30}, {3670, 20}, {3610, 10},
    {3500,   5}, {3300,  0},
};
static const int CURVE_N = sizeof(CURVE) / sizeof(CURVE[0]);

// While charging the cell voltage sits above its resting level — compensate
// so the level doesn't jump up on plug-in and down on unplug.
static const int CHARGING_OFFSET_MV = 120;

static int pct_from_mv(float mv) {
    if (mv >= CURVE[0].mv) return 100;
    for (int i = 1; i < CURVE_N; i++) {
        if (mv >= CURVE[i].mv) {
            float t = (mv - CURVE[i].mv) / (float)(CURVE[i - 1].mv - CURVE[i].mv);
            return (int)lroundf(CURVE[i].pct + t * (CURVE[i - 1].pct - CURVE[i].pct));
        }
    }
    return 0;
}

// ── Runtime estimate ─────────────────────────────────────────────────────────
// Samples of (time, level) while discharging. The drain rate is measured over
// up to the last ESTIMATE_WINDOW_MIN minutes; an estimate needs at least
// ESTIMATE_MIN_SPAN_MIN minutes and a visible drop, otherwise it stays unknown.
static const int ESTIMATE_WINDOW_MIN   = 90;
static const int ESTIMATE_MIN_SPAN_MIN = 20;
static const float ESTIMATE_MIN_DROP   = 2.0f;   // percentage points

struct Sample { time_t t; float pct; };
static Sample s_samples[ESTIMATE_WINDOW_MIN];
static int    s_sample_count = 0;
static int    s_sample_next  = 0;

static void reset_samples() { s_sample_count = 0; s_sample_next = 0; }

static void add_sample(time_t t, float pct) {
    s_samples[s_sample_next] = { t, pct };
    s_sample_next = (s_sample_next + 1) % ESTIMATE_WINDOW_MIN;
    if (s_sample_count < ESTIMATE_WINDOW_MIN) s_sample_count++;
}

static float estimate_hours(time_t now, float pct_now) {
    if (s_sample_count < 2) return -1;
    int oldest = (s_sample_next - s_sample_count + ESTIMATE_WINDOW_MIN) % ESTIMATE_WINDOW_MIN;
    const Sample& o = s_samples[oldest];
    float span_h = (now - o.t) / 3600.0f;
    float drop   = o.pct - pct_now;
    if (span_h * 60 < ESTIMATE_MIN_SPAN_MIN || drop < ESTIMATE_MIN_DROP) return -1;
    return pct_now / (drop / span_h);
}

// ── Public API ───────────────────────────────────────────────────────────────

bool battery_init() {
    if (!s_pmu.begin(Wire, SY6970_SLAVE_ADDRESS, TOUCH_SDA_PIN, TOUCH_SCL_PIN)) {
        Log.println("[PMU] SY6970 not found — no battery indicator");
        return false;
    }
    s_ok = true;
    s_pmu.enableCharge();
    s_pmu.enableMeasure();  // continuous ADC so getBattVoltage() returns real data
    Log.println("[PMU] SY6970 OK");
    return true;
}

void battery_update() {
    if (!s_ok) { s_status = BatteryStatus(); return; }
    bool usb = s_pmu.isVbusIn();
    uint16_t mv = s_pmu.getBattVoltage();
    // getBattVoltage() returns 0 until the ADC has produced a reading
    if (mv == 0) { s_status = BatteryStatus(); s_status.usb_power = usb; return; }

    bool charging = s_pmu.isCharging();
    s_status.usb_power = usb;
    float resting = charging ? mv - CHARGING_OFFSET_MV : mv;

    // Smooth out load spikes (WiFi TX, backlight); restart on plug/unplug
    if (s_mv_smooth == 0 || charging != s_last_charging) s_mv_smooth = resting;
    else s_mv_smooth = s_mv_smooth * 0.7f + resting * 0.3f;

    if (charging != s_last_charging) reset_samples();
    s_last_charging = charging;

    s_status.charging = charging;
    s_status.pct      = pct_from_mv(s_mv_smooth);

    time_t now = time(nullptr);
    if (!charging && now > 1700000000) {
        add_sample(now, s_status.pct);
        s_status.hours_left = estimate_hours(now, s_status.pct);
    } else {
        s_status.hours_left = -1;
    }

    Log.printf("[Batt] %umV (smoothed %.0f) → %d%%%s%s\n", mv, s_mv_smooth, s_status.pct,
                  charging ? " (charging)" : "",
                  s_status.hours_left >= 0 ? (String(" ~") + String(s_status.hours_left, 1) + "h").c_str() : "");
}

const BatteryStatus& battery_status() { return s_status; }
