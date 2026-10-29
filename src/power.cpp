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

void update_battery() {
    battery_update();
    const BatteryStatus& b = battery_status();
    g_battery_pct      = b.pct;
    g_battery_charging = b.charging;
    set_power_profile(b.usb_power);
    display_set_battery_estimate(b.hours_left);

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
