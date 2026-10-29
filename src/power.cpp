// ─────────────────────────────────────────────────────────────────────────────
// power.cpp — USB/battery power profile and battery warnings
// ─────────────────────────────────────────────────────────────────────────────

#include "app.h"

// Battery — cached from battery_status() once a minute (see update_battery())
int        g_battery_pct     = -1;
bool       g_battery_charging = false;
static uint32_t   last_battery_ms   = 0;
static int        g_battery_warned_at = 101;  // lowest warning level already shown

// ── Power profile ────────────────────────────────────────────────────────────
// On USB power the device runs at full speed: CPU 240 MHz (HTTPS handshakes
// ~2× faster, quicker redraws) and Wi-Fi power saving off (no 100–300 ms nap
// delay per request). On battery: 80 MHz + modem sleep. Checked with the
// battery reading (once a minute), which also debounces a wobbly cable.
static bool g_perf_mode = false;

void apply_wifi_power_save() {
    esp_wifi_set_ps(g_perf_mode ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
}

void set_power_profile(bool usb) {
    if (usb == g_perf_mode) return;
    g_perf_mode = usb;
    setCpuFrequencyMhz(usb ? 240 : 80);
    if (WiFi.status() == WL_CONNECTED) apply_wifi_power_save();
    Log.printf("[Power] %s\n", usb ? "USB: 240 MHz, Wi-Fi power saving off"
                                      : "Battery: 80 MHz, Wi-Fi power saving on");
}

// ── Measured battery life ────────────────────────────────────────────────────
// While on battery, the run's start (time, %) and every RUN_SAVE_SEC the last
// time alive (time, %) are kept in NVS ("batt"). If the next start is a power-on
// (the battery ran empty, then was charged) and the last note was at ≤ 15 %, the
// run counts: its hours, scaled up to a full charge when it started part-charged.
// Plugging in ends a run without counting it. The last RUNS_KEPT runs are kept.
static const long RUN_SAVE_SEC   = 600;
static const int  RUNS_KEPT      = 5;
static const int  RUN_MIN_START  = 50;    // started below this %: too partial to judge
static bool   s_run_active = false;
static time_t s_run_saved  = 0;

static void run_clear(Preferences& p) {
    p.remove("rs"); p.remove("rp"); p.remove("la"); p.remove("lp");
    s_run_active = false;
}

void battery_life_boot() {
    Preferences p;
    p.begin("batt", false);
    time_t rs = p.getLong("rs", 0), la = p.getLong("la", 0);
    int    rp = p.getInt("rp", -1), lp = p.getInt("lp", 100);
    if (rs > 0) {
        esp_reset_reason_t r = log_reset_reason();
        bool power_lost = (r == ESP_RST_POWERON || r == ESP_RST_BROWNOUT);
        if (power_lost && lp <= 15 && la > rs && rp >= RUN_MIN_START) {
            float hours = (la - rs) / 3600.0f * 100.0f / rp;   // per full charge
            String runs = p.isKey("runs") ? p.getString("runs", "") : "";
            runs = String(hours, 1) + (runs.length() ? "," + runs : "");
            int commas = 0;   // keep the newest RUNS_KEPT
            for (int i = 0; i < (int)runs.length(); i++)
                if (runs[i] == ',' && ++commas == RUNS_KEPT) { runs = runs.substring(0, i); break; }
            p.putString("runs", runs);
            Log.printf("[Batt] Battery ran out: %.1f h from %d%% (%.1f h per full charge)\n",
                       (la - rs) / 3600.0f, rp, hours);
            run_clear(p);
        } else if (power_lost) {
            run_clear(p);              // switched off / unclear end: not a measurement
        } else {
            s_run_active = true;       // restarted on battery (update, crash …): same run
        }
    }
    p.end();
}

float battery_life_hours(int& runs) {
    Preferences p;
    p.begin("batt", true);
    String s = p.isKey("runs") ? p.getString("runs", "") : "";
    p.end();
    runs = 0;
    float sum = 0;
    int start = 0;
    for (int i = 0; i <= (int)s.length(); i++) {
        if (i == (int)s.length() || s[i] == ',') {
            if (i > start) { sum += s.substring(start, i).toFloat(); runs++; }
            start = i + 1;
        }
    }
    return runs ? sum / runs : -1;
}

static void run_track(const BatteryStatus& b) {
    time_t now = time(nullptr);
    if (now < 1700000000 || b.pct < 0) return;
    Preferences p;
    if (b.usb_power) {
        if (s_run_active) { p.begin("batt", false); run_clear(p); p.end(); }
        return;
    }
    if (!s_run_active) {
        p.begin("batt", false);
        p.putLong("rs", now); p.putInt("rp", b.pct);
        p.putLong("la", now); p.putInt("lp", b.pct);
        p.end();
        s_run_active = true;
        s_run_saved  = now;
        return;
    }
    if (now - s_run_saved >= RUN_SAVE_SEC) {
        p.begin("batt", false);
        p.putLong("la", now); p.putInt("lp", b.pct);
        p.end();
        s_run_saved = now;
    }
}

void update_battery() {
    battery_update();
    const BatteryStatus& b = battery_status();
    g_battery_pct      = b.pct;
    g_battery_charging = b.charging;
    set_power_profile(b.usb_power);
    display_set_battery_estimate(b.hours_left);
    run_track(b);

    // Low-battery warning, once per level while unplugged. Not while the screen
    // is off at night — it shows at the next wake-up instead of lighting the room.
    if (b.pct < 0 || b.charging) { g_battery_warned_at = 101; return; }
    if (!display_screen_on()) return;
    for (int level : BATTERY_WARN_LEVELS) {
        if (b.pct <= level && g_battery_warned_at > level) {
            g_battery_warned_at = level;
            char msg[64];
            if (b.hours_left >= 0)
                snprintf(msg, sizeof(msg), "Battery low: %d%%\nAbout %.0f h left\nPlease charge",
                         b.pct, b.hours_left < 1 ? 1.0f : b.hours_left);
            else
                snprintf(msg, sizeof(msg), "Battery low: %d%%\nPlease charge", b.pct);
            show_overlay(msg, BATTERY_WARN_SHOW_MS);
            Log.printf("[Batt] Low-battery warning at %d%%\n", b.pct);
            break;
        }
    }
}

// Reads the charger chip once a minute (I2C; no need for a higher rate)
void battery_tick() {
    uint32_t now = millis();
    if (last_battery_ms != 0 && now - last_battery_ms < 60000) return;
    last_battery_ms = now;
    update_battery();
}

// End of the boot boost: the normal profile for the power source takes over
void power_end_boot_boost() {
    setCpuFrequencyMhz(80);
    g_perf_mode = false;
    battery_update();
    set_power_profile(battery_status().usb_power);
}
