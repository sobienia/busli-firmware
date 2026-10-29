// ─────────────────────────────────────────────────────────────────────────────
// main.cpp — Tramli entry point
// ─────────────────────────────────────────────────────────────────────────────


#include "app.h"

// loop() makes HTTPS calls (pager send, config-portal Check/Test buttons). A TLS
// handshake needs more stack than the default 8 KB loopTask — overflowing it
// crashes with "Double exception". Measured: a portal stock Check uses ~9 KB.
// 16 KB matches FETCH_TASK_STACK.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  ARDUINO ENTRY POINTS                                                     ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

void setup() {
    setCpuFrequencyMhz(80);   // 80 MHz is the minimum with WiFi; saves ~100 mA vs 240 MHz
    Serial.begin(115200);
    log_init(esp_reset_reason());   // restart reason: Device page + log
    delay(500);
    g_ota_failed_version = ota_boot_check();   // new firmware on trial / rolled back?
    Log.println();
    Log.println("============================================");
    Log.println("  Busli — T-Display S3 Pro");
    Log.println("============================================");

    pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
    pinMode(PIN_BTN_BRIGHT,  INPUT_PULLUP);
    pinMode(PIN_BTN_ZOOM,    INPUT_PULLUP);
    pinMode(PIN_BTN_THEME,   INPUT_PULLUP);

    display_init();
    config_load_night(g_night);
    display_set_night_schedule(g_night.start_min, g_night.end_min);
    display_init_brightness();
    config_web_set_pairing_callback(on_pairing_code);
    load_theme();
    load_display_prefs();
    Wire.begin(TOUCH_SDA_PIN, TOUCH_SCL_PIN);
    touch_init();
    battery_init();
    if (light_init()) display_enable_auto_brightness();
    display_boot_animation_start();

    setup_stops();

    if (!connect_wifi()) {
        // No saved network works here (first start, new place): open the setup
        // hotspot with its QR codes right away. Saving reboots; if nobody sets it
        // up within PORTAL_TIMEOUT_MS, boot continues offline and loop() keeps
        // retrying Wi-Fi in the background (the button still opens setup).
        display_boot_animation_stop();
        Log.println("[WiFi] No network — starting setup hotspot");
        run_setup_portal();
        display_show_status("No Wi-Fi yet - retrying\nHold the button 3 s\nfor the setup hotspot");
        delay(4000);
        display_invalidate();
    }
    // Modem sleep (battery profile): the radio powers down between beacon
    // intervals, cutting idle WiFi draw ~30-40% at the cost of some latency.
    // Switched off on USB power — see set_power_profile().
    apply_wifi_power_save();

    sync_clock();

    {
        String cd_label, cd_target_str;
        if (config_load_countdown(cd_label, cd_target_str, g_countdown_icon) && cd_target_str.length() >= 16) {
            // configTzTime was called in sync_clock() so mktime() now uses Swiss local time
            struct tm t = {};
            t.tm_year  = cd_target_str.substring(0,  4).toInt() - 1900;
            t.tm_mon   = cd_target_str.substring(5,  7).toInt() - 1;
            t.tm_mday  = cd_target_str.substring(8,  10).toInt();
            t.tm_hour  = cd_target_str.substring(11, 13).toInt();
            t.tm_min   = cd_target_str.substring(14, 16).toInt();
            t.tm_isdst = -1;
            g_countdown_target = mktime(&t);
            Log.printf("[Countdown] '%s' → %s → epoch %ld\n",
                          cd_label.c_str(), cd_target_str.c_str(), (long)g_countdown_target);
        }
    }

    // Load commute station names from NVS and hand them to the background task
    {
        CommuteConfig cc;
        if (config_load_commute(cc)) {
            g_home_station = cc.home_station;
            g_work_station = cc.work_station;
            Log.printf("[Commute] Home: '%s'  Work: '%s'\n",
                          g_home_station.c_str(), g_work_station.c_str());
        }
        fetch_task_set_commute(g_home_station, g_work_station);
    }

    if (config_load_stocks(g_stocks_cfg))
        Log.printf("[Stocks] %d ticker(s), layout %d, rotate %ds\n",
                      g_stocks_cfg.count, g_stocks_cfg.layout, g_stocks_cfg.rotate_sec);

    setup_pages();
    set_initial_page();
    update_weather_place();   // before the fetch task's first weather request

    http_lock_init();
    fetch_task_start(g_stop_configs, g_num_stops);
    fetch_task_set_ota_enabled(config_load_ota_enabled());
    fetch_task_set_stocks(g_stocks_cfg.symbols, g_stocks_cfg.count);
    fetch_task_set_today_enabled(config_load_today_page());
    config_load_kiosk(g_kiosk);
    {
        RemoteHooks hooks;
        hooks.state   = remote_state;
        hooks.command = remote_command;
        config_web_set_remote(hooks);
    }
    {
        WeatherLocations wl;
        config_load_weather_locations(wl);
        fetch_task_set_weather_locations(wl);
        g_wx_count = (wl.device ? 1 : 0) + wl.n_towns;
    }
    {
        String pt_tracking;
        if (config_load_parcel(pt_tracking, g_parcel_pulses)) {
            fetch_task_set_parcel(pt_tracking, g_parcel_pulses);
            Log.printf("[Parcel] Tracking: %s (%d pulses)\n",
                          pt_tracking.c_str(), g_parcel_pulses);
        }
    }
    pager_ui_init();
    restore_saved_page();   // overrides the time-based initial page

    {
        FlightEntry fl_entries[2];
        g_flight_count = config_load_flights(fl_entries);
        if (g_flight_count > 0) {
            Log.printf("[Flights] %d flight(s) configured\n", g_flight_count);
            String osky_u, osky_p;
            if (config_load_opensky(osky_u, osky_p))
                flight_tracker_set_opensky_auth(osky_u, osky_p);
            // Registers entries and schedules initial fetch on core 0 — setup() doesn't block
            fetch_task_init_flights(fl_entries, g_flight_count);
        }
    }

    // Weather and commute are now fetched by the background task immediately after
    // it starts, so setup() can complete without blocking on those HTTP calls.
    // Configuration complete → the fetch task loads every data source once. The
    // boot animation keeps running until that's done (or BOOT_WAIT_MAX_MS), so the
    // first page appears fully populated instead of filling in piece by piece.
    // Full CPU speed meanwhile: each HTTPS handshake is CPU-bound and takes ~3 s
    // at the power-saving 80 MHz; back to 80 MHz once the first page is ready.
    // Never stuck here: at most BOOT_WAIT_MAX_MS (no internet, a slow source …),
    // and any touch or button skips the wait — data still loading fills in later.
    setCpuFrequencyMhz(240);
    fetch_task_config_done();   // also loads the data remembered from the last run
    // Recent remembered data → show the pages almost at once; it's refreshed in
    // the background (departures within a second or two)
    uint32_t max_wait = fetch_task_snapshot_fresh() ? BOOT_WAIT_CACHED_MS : BOOT_WAIT_MAX_MS;
    uint32_t wait_start = millis();
    bool skipped = false;
    while (!fetch_task_startup_done() && millis() - wait_start < max_wait) {
        touch_poll();
        if (any_input_down()) { skipped = true; break; }
        delay(50);
    }
    Log.printf("[Boot] %s after %lu ms\n",
                  fetch_task_startup_done()   ? "Data ready"
                  : skipped                   ? "Wait skipped"
                  : fetch_task_snapshot_fresh() ? "Showing remembered data, refreshing"
                                              : "Wait timed out",
                  (unsigned long)(millis() - wait_start));
    if (skipped) g_input_swallowed = true;   // the skipping press does nothing else
    power_end_boot_boost();

    g_setup_done_ms = millis();
    display_boot_animation_stop();
    ota_report_rollback();
}

void loop() {
    // Touch is polled once per loop; power_tick() may swallow the press that
    // wakes the screen so it doesn't also act as a tap/swipe/button press.
    TouchGesture gesture = touch_poll();
    bool swallow_input   = power_tick();
    if (!swallow_input) {
        check_button();
        check_btn_bright();
        check_btn_zoom();
        check_btn_theme();
        check_touch(gesture);
    }
    save_page_tick();
    kiosk_tick();
    wifi_reconnect_tick();
    config_web_poll();
    battery_tick();
    ota_prompt_tick();

    // The pager owns the screen while it's open
    if (!pager_ui_tick(swallow_input)) {
        views_pull_data();
        draw_current_page(millis());
    }
    delay(20);
}

