// ─────────────────────────────────────────────────────────────────────────────
// input.cpp — buttons, touch gestures and color themes
// ─────────────────────────────────────────────────────────────────────────────

#include "app.h"

// Button debounce state — BOOT button
static bool     button_was_pressed    = false;
static uint32_t button_press_start_ms = 0;
// Config-hold prompt: 0 = none, 1 = "Hold to configure...", 2 = "Release to configure".
// While non-zero, loop() skips page redraws so the prompt stays on screen.
int      g_config_hint_stage     = 0;
static const uint32_t CONFIG_HINT_MS = 1000;  // show "Hold to configure..."
static const uint32_t CONFIG_HOLD_MS = 3000;  // releasing after this opens the portal

// Button debounce state — bottom-right button (brightness cycle)
// ≥50ms hold required to filter GPIO glitches and touch-IRQ pulses.
static bool     s_btn_bright_was_pressed  = false;
static uint32_t s_btn_bright_press_start  = 0;

// Button debounce state — circular button (GPIO 38): long press = flip orientation
static bool     s_btn_zoom_was_pressed    = false;
static uint32_t s_btn_zoom_press_start    = 0;

// Display orientation state (persisted to NVS)
bool g_display_flipped = false;

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  INPUT HANDLERS                                                           ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

// When flipped 180°, the top-left (GPIO 0) and bottom-right (GPIO 12) buttons
// physically swap roles so the function stays at the same visual corner.
// PIN_BOOT_BUTTON (GPIO 0) = nav/config in normal; brightness in flipped.
// PIN_BTN_BRIGHT  (GPIO 12) = brightness in normal; nav/config in flipped.
void check_button() {
    int pin = g_display_flipped ? PIN_BTN_BRIGHT : PIN_BOOT_BUTTON;
    bool pressed = (digitalRead(pin) == LOW);

    if (pressed && !button_was_pressed) {
        button_press_start_ms = millis();
        g_config_hint_stage     = 0;
    } else if (pressed && button_was_pressed) {
        // Still held — prompt the user, then tell them when it's safe to let go
        uint32_t held_ms = millis() - button_press_start_ms;
        if (held_ms >= CONFIG_HOLD_MS && g_config_hint_stage < 2) {
            g_config_hint_stage = 2;
            display_show_status("Release for the\nsetup hotspot");
        } else if (held_ms >= CONFIG_HINT_MS && g_config_hint_stage < 1) {
            g_config_hint_stage = 1;
            if (WiFi.status() == WL_CONNECTED)
                display_show_status("Release now: settings QR code\n\nKeep holding:\nsetup hotspot");
            else
                display_show_status("Keep holding for the\nsetup hotspot...");
        }
    } else if (!pressed && button_was_pressed) {
        uint32_t held_ms = millis() - button_press_start_ms;
        int stage = g_config_hint_stage;
        if (stage > 0) {
            // Prompt covered the screen — repaint whatever was showing
            g_last_redraw_ms = 0;
            g_pager_dirty  = true;
        }
        g_config_hint_stage = 0;
        if (held_ms >= CONFIG_HOLD_MS) {
            Log.println("[Button] 3s hold → config portal");
            run_setup_portal();
        } else if (stage == 1) {
            Log.println("[Button] 1–3 s hold → settings QR code");
            show_settings_qr();
        } else if (held_ms >= 50) {
            Log.println("[Button] Short press → prev page");
            switch_page((g_current_page - 1 + g_num_pages) % g_num_pages);
        }
    }
    button_was_pressed = pressed;
}

void check_btn_bright() {
    int pin = g_display_flipped ? PIN_BOOT_BUTTON : PIN_BTN_BRIGHT;
    bool pressed = (digitalRead(pin) == LOW);
    if (pressed && !s_btn_bright_was_pressed) {
        s_btn_bright_press_start = millis();
    } else if (!pressed && s_btn_bright_was_pressed) {
        uint32_t held = millis() - s_btn_bright_press_start;
        if (held >= 2000) {
            pager_ui_toggle();   // long press opens/cancels the pager
        } else if (held >= 50 && !pager_ui_active()) {
            uint8_t level = display_step_brightness();
            // With a light sensor the cycle includes "auto" — say which one it is
            if (light_available()) {
                char msg[24];
                if (level == 0) snprintf(msg, sizeof(msg), "Brightness: auto");
                else            snprintf(msg, sizeof(msg), "Brightness: %d%%", level);
                show_overlay(msg, 1200);
            }
        }
    }
    s_btn_bright_was_pressed = pressed;
}

// GPIO 38 button — currently diagnostic only (short press prints to Serial).
// Orientation flip has been moved to GPIO 16 long press (theme button).
void check_btn_zoom() {
    bool pressed = (digitalRead(PIN_BTN_ZOOM) == LOW);
    if (pressed && !s_btn_zoom_was_pressed) {
        s_btn_zoom_press_start = millis();
        Log.println("[GPIO38] DOWN — button detected");
    } else if (!pressed && s_btn_zoom_was_pressed) {
        uint32_t held_ms = millis() - s_btn_zoom_press_start;
        Log.printf("[GPIO38] UP — held %lu ms\n", held_ms);
    }
    s_btn_zoom_was_pressed = pressed;
}

// ── Color themes ──────────────────────────────────────────────────────────────
// RGB565 values: R=5bit G=6bit B=5bit.
// Each theme has 4 brightness levels: 100%/80%/50%/30%.
struct Theme {
    const char* name;
    uint16_t row0, rows, dim, meta;
};

// Palette derivation (all verified bit-by-bit):
//  Zürich Amber  base #F09000 (R30 G36 B0):  G/R≈0.6  → warm amber
//  Matrix Green  base #00FC00 (R0  G63 B0):  pure phosphor green
//  Tron Blue     base #00A0F8 (R0  G40 B31): electric cyan-blue
//  Stranger Red  base #F81800 (R31 G6  B0):  deep neon red
static const Theme THEMES[] = {
    { "Zurich Amber",   0xF480, 0xC3A0, 0x7A40, 0x4960 },
    { "Matrix Green",   0x07E0, 0x0640, 0x0400, 0x0260 },
    { "Tron Blue",      0x051F, 0x0419, 0x0290, 0x0189 },
    { "Stranger Red",   0xF8C0, 0xC8A0, 0x7860, 0x4840 },
};
static const int N_THEMES = sizeof(THEMES) / sizeof(THEMES[0]);
static int g_theme_idx = 0;

static void apply_theme(int idx) {
    g_theme_idx = idx;
    const Theme& t = THEMES[idx];
    display_set_theme(t.row0, t.rows, t.dim, t.meta);
    display_invalidate();
    Log.printf("[Theme] %s\n", t.name);
}

void load_theme() {
    Preferences prefs;
    prefs.begin("tramli", true);
    int idx = prefs.getInt("theme", 0);
    prefs.end();
    if (idx < 0 || idx >= N_THEMES) idx = 0;
    g_theme_idx = idx;
    const Theme& t = THEMES[idx];
    display_set_theme(t.row0, t.rows, t.dim, t.meta);
    Log.printf("[Theme] Loaded: %s\n", t.name);
}

// Orientation and font size, as left by the buttons/double tap
void load_display_prefs() {
    Preferences prefs;
    prefs.begin("tramli", true);
    g_display_flipped = prefs.getBool("flipped", false);
    g_large_font      = prefs.getBool("large_font", false);
    prefs.end();
    if (g_display_flipped) display_set_flipped(true);
}

static void save_theme(int idx) {
    Preferences prefs;
    prefs.begin("tramli", false);
    prefs.putInt("theme", idx);
    prefs.end();
}

// ── Theme button (GPIO 16) ────────────────────────────────────────────────────
static bool     s_btn_theme_was_pressed = false;
static uint32_t s_btn_theme_press_start = 0;

void check_btn_theme() {
    bool pressed = (digitalRead(PIN_BTN_THEME) == LOW);
    if (pressed && !s_btn_theme_was_pressed) {
        s_btn_theme_press_start = millis();
    } else if (!pressed && s_btn_theme_was_pressed) {
        uint32_t held = millis() - s_btn_theme_press_start;
        if (held >= TOUCH_LONG_PRESS_MS) {
            // Long press → flip orientation 180° and persist
            g_display_flipped = !g_display_flipped;
            display_set_flipped(g_display_flipped);
            Preferences prefs;
            prefs.begin("tramli", false);
            prefs.putBool("flipped", g_display_flipped);
            prefs.end();
            Log.printf("[Theme] Long press → orientation flipped=%d\n", g_display_flipped);
        } else if (held >= 50) {
            // Short press → cycle color theme
            int new_idx = (g_theme_idx + 1) % N_THEMES;
            apply_theme(new_idx);
            save_theme(new_idx);
        }
    }
    s_btn_theme_was_pressed = pressed;
}

// gesture comes from touch_poll() in loop(). While the pager is open its own
// tap handling owns the screen, so gestures are ignored here.
void check_touch(TouchGesture gesture) {
    if (pager_ui_active()) return;
    switch (gesture) {
        case TOUCH_SWIPE_LEFT:
            Log.println("[Touch] Swipe left → next page");
            switch_page((g_current_page + 1) % g_num_pages);
            break;
        case TOUCH_SWIPE_RIGHT:
            Log.println("[Touch] Swipe right → prev page");
            switch_page((g_current_page - 1 + g_num_pages) % g_num_pages);
            break;
        case TOUCH_LONG_PRESS:
            Log.println("[Touch] Long press → force refresh");
            force_refresh_current_page();
            break;
        case TOUCH_DOUBLE_TAP:
            // Font size only applies to the departure board; on the portfolio a
            // quick double tap is just an impatient tap
            if (g_pages[g_current_page].type == PageInfo::STOCKS) { handle_stock_tap(); break; }
            Log.println("[Touch] Double tap → toggle font size");
            g_large_font = !g_large_font;
            {
                Preferences prefs;
                prefs.begin("tramli", false);
                prefs.putBool("large_font", g_large_font);
                prefs.end();
            }
            display_invalidate();
            break;
        case TOUCH_SWIPE_DOWN:
            if (g_pages[g_current_page].type == PageInfo::TRANSIT) {
                // Cycle forward: BOARD → FLIGHT0 → FLIGHT1 → BOARD
                if (g_flight_count > 0) {
                    View nv;
                    if      (g_view == VIEW_BOARD)  nv = VIEW_FLIGHT0;
                    else if (g_view == VIEW_FLIGHT0) nv = (g_flight_count > 1) ? VIEW_FLIGHT1 : VIEW_BOARD;
                    else                             nv = VIEW_BOARD;
                    Log.printf("[Touch] Swipe down → flight view %d\n", (int)nv);
                    if (nv != g_view) { g_view = nv; display_invalidate(); }
                }
            } else if (g_pages[g_current_page].type == PageInfo::STOCKS) {
                if (g_stock_detail >= 0) {
                    set_stock_detail(g_stock_detail + 1);       // next ticker's details
                } else {
                    set_stock_sub_page(g_stock_sub_page + 1);
                    Log.printf("[Touch] Stocks → sub-page %d\n", g_stock_sub_page);
                }
            } else if (g_pages[g_current_page].type == PageInfo::WEATHER) {
                step_weather_slot(+1);
            } else if (is_commute_page()) {
                // Commute: advance to next connection
                CommuteData& cd = (g_pages[g_current_page].type == PageInfo::COMMUTE_HOME)
                                  ? g_commute_home : g_commute_work;
                if (cd.connection_count > 1) {
                    g_commute_conn_idx = (g_commute_conn_idx + 1) % cd.connection_count;
                    Log.printf("[Touch] Commute → connection %d\n", g_commute_conn_idx);
                    display_invalidate();
                }
            }
            break;
        case TOUCH_SWIPE_UP:
            if (g_pages[g_current_page].type == PageInfo::TRANSIT) {
                // Cycle backward: BOARD → FLIGHT1 → FLIGHT0 → BOARD
                if (g_flight_count > 0) {
                    View nv;
                    if      (g_view == VIEW_BOARD)   nv = (g_flight_count > 1) ? VIEW_FLIGHT1 : VIEW_FLIGHT0;
                    else if (g_view == VIEW_FLIGHT1)  nv = VIEW_FLIGHT0;
                    else                              nv = VIEW_BOARD;
                    Log.printf("[Touch] Swipe up → flight view %d\n", (int)nv);
                    if (nv != g_view) { g_view = nv; display_invalidate(); }
                }
            } else if (g_pages[g_current_page].type == PageInfo::STOCKS) {
                if (g_stock_detail >= 0) {
                    set_stock_detail(g_stock_detail - 1);       // previous ticker's details
                } else {
                    set_stock_sub_page(g_stock_sub_page - 1);
                    Log.printf("[Touch] Stocks → sub-page %d\n", g_stock_sub_page);
                }
            } else if (g_pages[g_current_page].type == PageInfo::WEATHER) {
                step_weather_slot(-1);
            } else if (is_commute_page()) {
                // Commute: go back to previous connection
                CommuteData& cd = (g_pages[g_current_page].type == PageInfo::COMMUTE_HOME)
                                  ? g_commute_home : g_commute_work;
                if (cd.connection_count > 1) {
                    g_commute_conn_idx = (g_commute_conn_idx - 1 + cd.connection_count) % cd.connection_count;
                    Log.printf("[Touch] Commute → connection %d\n", g_commute_conn_idx);
                    display_invalidate();
                }
            }
            break;
        case TOUCH_TAP:
            if (g_pages[g_current_page].type == PageInfo::STOCKS) handle_stock_tap();
            if (g_pages[g_current_page].type == PageInfo::DEVICE) show_settings_qr();
            break;
        default:
            break;
    }
}
