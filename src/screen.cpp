// ─────────────────────────────────────────────────────────────────────────────
// screen.cpp — overlays, setup hotspot and screen power (night mode)
// ─────────────────────────────────────────────────────────────────────────────

#include "app.h"

// Full-screen message overlay (low battery warning, …): page redraws pause until
// this millis() deadline or until the screen is touched. 0 = no overlay.
uint32_t   g_overlay_until_ms = 0;

// A press that already did its job (woke the screen, closed an overlay) is
// ignored until released, so it doesn't also tap/swipe the page underneath.
bool       g_input_swallowed  = false;

// Keep whatever was just drawn on screen for duration_ms (touch dismisses it).
void hold_overlay(uint32_t duration_ms) {
    g_overlay_until_ms = millis() + duration_ms;
    if (g_overlay_until_ms == 0) g_overlay_until_ms = 1;
}

// Show a message over the current page for duration_ms (touch dismisses it).
void show_overlay(const char* msg, uint32_t duration_ms) {
    display_set_screen_on(true);   // also during screen-off night mode
    display_show_status(msg);
    hold_overlay(duration_ms);
}

// QR code that opens the settings page already paired (BOOT 1–3 s, or a tap on
// the Device page). Closes itself once the phone has paired, or after a minute.
void show_settings_qr() {
    String url = config_web_pair_url();
    if (url.isEmpty()) {
        show_overlay("Not connected to Wi-Fi\nHold the button 3 s\nfor the setup hotspot", 8000);
        return;
    }
    display_set_screen_on(true);
    display_show_settings_qr(url.c_str(), WEB_HOSTNAME, WiFi.localIP().toString().c_str());
    hold_overlay(60000);
    Log.println("[Web] Settings QR code shown");
}

// Settings on the normal Wi-Fi: show the pairing code while a browser waits for it
void on_pairing_code(const char* code) {
    if (code) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Settings code\n%s\nEnter it in your browser", code);
        show_overlay(msg, 120000);
    } else if (g_overlay_until_ms != 0) {
        g_overlay_until_ms = 0;
        g_last_redraw_ms     = 0;
        g_pager_dirty      = true;
    }
}

bool overlay_active() {
    if (g_overlay_until_ms == 0) return false;
    bool touched = touch_is_pressed();
    if ((int32_t)(millis() - g_overlay_until_ms) >= 0 || touched) {
        g_overlay_until_ms = 0;
        g_last_redraw_ms     = 0;   // repaint the page right away
        g_pager_dirty      = true;
        if (touched) g_input_swallowed = true;   // the closing touch does nothing else
        return false;
    }
    return true;
}

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  SCREEN POWER — night mode (dim or off) and wake-on-touch                 ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
// In the "screen off" and "clock" night modes the normal pages are hidden at
// night; any touch or button shows them for WAKE_ON_TOUCH_SEC (each further
// interaction extends it). The press that wakes them is swallowed, so it
// doesn't also switch pages or open menus.

NightConfig g_night;
bool        g_woken           = false;
uint32_t    g_woke_at_ms      = 0;
bool        g_clock_showing   = false;   // night clock instead of the pages

bool any_input_down() {
    return touch_is_pressed() ||
           digitalRead(PIN_BOOT_BUTTON) == LOW ||
           digitalRead(PIN_BTN_BRIGHT)  == LOW ||
           digitalRead(PIN_BTN_THEME)   == LOW;
}

// Setup hotspot: any touch or button press goes back to normal operation
static bool portal_cancel_requested() {
    touch_poll();
    return any_input_down();
}

// Run the setup hotspot; afterwards repaint the page, and ignore the press that
// cancelled it until it's released
void run_setup_portal() {
    config_portal_run(PORTAL_TIMEOUT_MS, portal_cancel_requested);   // reboots on save
    if (WiFi.status() == WL_CONNECTED) apply_wifi_power_save();      // portal changed it
    if (any_input_down()) g_input_swallowed = true;
    g_last_redraw_ms = 0;
    g_pager_dirty  = true;
    display_invalidate();
}

// ── Night clock preview (Remote page): the clock with weather behind it ──────
static const uint32_t CLOCK_PREVIEW_MS = 20000;
static int      s_preview_kind     = -1;
static uint32_t s_preview_until_ms = 0;

void clock_preview(int kind) {
    if (kind < 0 || kind >= SCENE_COUNT) return;
    s_preview_kind     = kind;
    s_preview_until_ms = millis() + CLOCK_PREVIEW_MS;
}

int clock_preview_kind() {
    if (s_preview_kind >= 0 && (int32_t)(millis() - s_preview_until_ms) >= 0) s_preview_kind = -1;
    return s_preview_kind;
}

// Updates screen power and the fetch rate. Returns true while input must be
// ignored (from the waking press until it is released, inclusive).
bool power_tick() {
    bool night     = display_is_night();
    bool hide_mode = night && g_night.mode != NIGHT_DIM;   // pages hidden unless woken
    bool input     = any_input_down();

    // Light sensor: room light → backlight (auto mode), and a hand moving in
    // front of the screen wakes it like a touch. Only the moment it comes near
    // counts, so something left in front doesn't keep the screen on all night.
    light_poll();
    static uint32_t last_lux_ms = 0;
    if (light_available() && millis() - last_lux_ms >= 1000) {
        last_lux_ms = millis();
        display_set_ambient_light(light_lux());
    }
    static bool was_near = false;
    bool near     = light_near();
    bool approach = near && !was_near;
    was_near      = near;
    if (approach && hide_mode) Log.printf("[Light] Hand near (%d) — waking\n", light_proximity_raw());

    if ((input || approach) && hide_mode) {
        bool pages_visible = display_screen_on() && !g_clock_showing;
        if (input && !pages_visible) g_input_swallowed = true;   // a wave has nothing to swallow
        g_woken      = true;
        g_woke_at_ms = millis();
    }
    if (g_woken && millis() - g_woke_at_ms >= WAKE_ON_TOUCH_SEC * 1000UL) g_woken = false;

    // Overlays (pairing code, warnings) count as awake while they show
    bool idle    = hide_mode && !g_woken && g_overlay_until_ms == 0;
    bool preview = clock_preview_kind() >= 0;
    bool want_on = preview || !(idle && g_night.mode == NIGHT_OFF);
    bool clock   = preview || (idle && g_night.mode == NIGHT_CLOCK);

    if (want_on != display_screen_on()) {
        display_set_screen_on(want_on);
        if (want_on) {
            g_last_redraw_ms = 0;
            g_pager_dirty  = true;
        }
    }
    if (clock != g_clock_showing) {
        g_clock_showing = clock;
        display_invalidate();
        g_last_redraw_ms = 0;
        g_pager_dirty  = true;
    }
    // Pages becoming visible again → nothing was fetched for them meanwhile
    static bool was_idle = false;
    if (was_idle && !idle) fetch_task_wake();
    was_idle = idle;

    fetch_task_set_power_mode(idle  ? FETCH_POWER_SCREEN_OFF
                              : night ? FETCH_POWER_NIGHT
                                      : FETCH_POWER_NORMAL);

    bool swallow = g_input_swallowed;
    if (!input && swallow) {
        g_input_swallowed = false;
        touch_cancel_pending();   // its tap would otherwise arrive ~350 ms later
    }
    return swallow;
}
