// ─────────────────────────────────────────────────────────────────────────────
// page_views.cpp — drawing the current page (and pulling data for it)
// ─────────────────────────────────────────────────────────────────────────────

#include "app.h"

bool     g_large_font   = false;   // persisted in NVS ("large_font")
uint32_t g_last_redraw_ms    = 0;
time_t   g_countdown_target = 0;
int      g_countdown_icon   = 0;

// Weather — display-side cache; populated from background task via fetch_task_get_weather()
WeatherData g_weather    = {};
String      g_weather_city;        // town of the place the weather is for
uint32_t    g_setup_done_ms = 0;  // set at end of setup(); used to detect "still loading"

// Parcel tracking — display-side state
String   g_parcel_status;         // "" = not configured or not yet fetched
int      g_parcel_pulses    = 3;  // brightness flashes on status change
static bool     g_parcel_pulsing   = false;
static int      g_parcel_pulse_rem = 0;  // remaining flash cycles
static uint32_t g_parcel_pulse_ms  = 0;  // millis() when current half-cycle started
static bool     g_parcel_pulse_hi  = false; // true = currently at 100% brightness

// ── Parcel brightness pulse ───────────────────────────────────────────────────
// Called every loop() tick when g_parcel_pulsing is true.
// Each "pulse" = 250 ms at 100%, then 250 ms at dimmed level.
// After g_parcel_pulse_rem cycles, restores brightness via display_invalidate().
static void parcel_pulse_tick() {
    if (!g_parcel_pulsing) return;
    uint32_t now_ms = millis();
    if (now_ms - g_parcel_pulse_ms < 250) return;   // wait out the current half-cycle
    g_parcel_pulse_ms = now_ms;
    g_parcel_pulse_hi = !g_parcel_pulse_hi;
    display_set_brightness(g_parcel_pulse_hi ? 100 : 15);
    if (!g_parcel_pulse_hi) {
        // finished one full pulse (hi→lo)
        if (--g_parcel_pulse_rem <= 0) {
            g_parcel_pulsing = false;
            display_pause_auto_brightness(false);
            display_restore_brightness();  // back to the day/night level (was stuck at 15%)
        }
    }
}

// "Zürich 11°C/12°C" — town of the place on screen + current/max temperature
static String weather_temp_text() {
    char buf[48];
    snprintf(buf, sizeof(buf), "%s%s%d°C/%d°C",
             g_weather_city.c_str(), g_weather_city.isEmpty() ? "" : " ",
             (int)roundf(g_weather.temp_c), (int)roundf(g_weather.temp_max_c));
    return buf;
}

// Rain alert when rain is coming or falling, otherwise UV (which doesn't
// matter while it rains): "Rain from 17:15", "Rain until 18:30", "UV3/4"
static String weather_info_text() {
    char info[24];
    struct tm t;
    if (g_weather.raining_now && g_weather.rain_end > 0) {
        localtime_r(&g_weather.rain_end, &t);
        snprintf(info, sizeof(info), "Rain until %02d:%02d", t.tm_hour, t.tm_min);
    } else if (g_weather.raining_now) {
        snprintf(info, sizeof(info), "Raining");
    } else if (g_weather.rain_start > 0) {
        localtime_r(&g_weather.rain_start, &t);
        snprintf(info, sizeof(info), "Rain from %02d:%02d", t.tm_hour, t.tm_min);
    } else {
        snprintf(info, sizeof(info), "UV%d/%d", g_weather.uv_index, g_weather.uv_index_max);
    }
    return info;
}

// Footer text shared by all pages:
//   weather_str  "Zürich 10C/11C"  (town of the stop on screen + now/max temperature)
//   uv_str       a rain alert when rain is coming or falling, otherwise "UV3/4";
//                plus " | <parcel_prefix><status>" while a parcel is tracked
// Shows "No weather" once the first fetch is clearly overdue.
static void format_footer_weather(char* weather_str, size_t weather_len,
                                  char* uv_str, size_t uv_len,
                                  const char* parcel_prefix) {
    weather_str[0] = '\0';
    uv_str[0]      = '\0';
    if (g_weather.valid) {
        snprintf(weather_str, weather_len, "%s", weather_temp_text().c_str());
        String info = weather_info_text();
        if (g_parcel_status.length() > 0)
            snprintf(uv_str, uv_len, "%s | %s%s", info.c_str(), parcel_prefix, g_parcel_status.c_str());
        else
            snprintf(uv_str, uv_len, "%s", info.c_str());
    } else if (g_setup_done_ms > 0 && millis() - g_setup_done_ms > 90000) {
        strncpy(weather_str, "No weather", weather_len - 1);
        weather_str[weather_len - 1] = '\0';
    }
}

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  DEVICE INFO PAGE                                                         ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
// Everything you'd ask over the phone when helping someone: version, network,
// battery, why it last restarted, and which data sources work.

// "now", "4m", "2h", "3d" — how long ago t was
static String ago(time_t t, time_t now) {
    long s = (long)(now - t);
    if (s < 60)    return "now";
    if (s < 3600)  return String(s / 60) + "m";
    if (s < 86400) return String(s / 3600) + "h";
    return String(s / 86400) + "d";
}

static void draw_device_info_page() {
    time_t now = time(nullptr);
    InfoRow left[8];
    int nl = 0;

    String fw = "v" FIRMWARE_VERSION;
    if (g_ota_remote_version.length() > 0 && g_ota_remote_version != FIRMWARE_VERSION)
        fw += " (v" + g_ota_remote_version + " ready)";
    else if (ota_on_trial())
        fw += " (new, checking)";
    else if (g_ota_failed_version.length() > 0)
        fw += " (v" + g_ota_failed_version + " failed)";
    left[nl++] = { "Firmware", fw };

    if (WiFi.status() == WL_CONNECTED) {
        int rssi = WiFi.RSSI();
        const char* q = rssi >= -60 ? "good" : rssi >= -72 ? "ok" : "weak";
        left[nl++] = { "Wi-Fi", WiFi.SSID(), 0 };
        left[nl++] = { "Signal", String(rssi) + " dBm, " + q, (uint8_t)(rssi < -72 ? 2 : 0) };
        left[nl++] = { "Address", WiFi.localIP().toString() };
    } else {
        left[nl++] = { "Wi-Fi", "not connected", 2 };
    }

    // Uptime + CPU speed (240 MHz = USB performance mode, 80 MHz = battery saving)
    uint32_t up = millis() / 1000;
    char upbuf[32];
    if (up >= 86400) snprintf(upbuf, sizeof(upbuf), "%lud %luh, %lu MHz", (unsigned long)(up / 86400),
                              (unsigned long)(up % 86400 / 3600), (unsigned long)getCpuFrequencyMhz());
    else             snprintf(upbuf, sizeof(upbuf), "%luh %lum, %lu MHz", (unsigned long)(up / 3600),
                              (unsigned long)(up % 3600 / 60), (unsigned long)getCpuFrequencyMhz());
    left[nl++] = { "Uptime", upbuf };

    const BatteryStatus& b = battery_status();
    if (b.pct >= 0) {
        String bs = String(b.pct) + "%";
        if (b.charging)            bs += ", charging";
        else if (b.hours_left >= 0) bs += ", ~" + String((int)lroundf(b.hours_left)) + "h left";
        left[nl++] = { "Battery", bs, (uint8_t)(!b.charging && b.pct <= 20 ? 2 : 0) };
    }

    // Why it last restarted, plus how often it crashed since this version was
    // installed (details: Settings → Device log)
    bool problem;
    String rr = log_reset_reason_text(log_reset_reason(), problem);
    int crashes = log_crash_count();
    if (crashes > 0) rr += String(", ") + crashes + (crashes == 1 ? " crash" : " crashes");
    left[nl++] = { "Restart", rr, (uint8_t)(problem || crashes > 0 ? 2 : 0) };
    // Free internal RAM (what TLS etc. needs) + free PSRAM (big buffers)
    char mem[32];
    snprintf(mem, sizeof(mem), "%uK + %.1fM free",
             (unsigned)(ESP.getFreeHeap() / 1024), ESP.getFreePsram() / 1048576.0f);
    left[nl++] = { "Memory", mem };

    // Data sources that are in use: "ok 2m" / "failed 5m" since the last result
    static const char* NAMES[SRC_COUNT] = { "Departures", "Weather", "Commute", "Stocks", "Parcel", "Pager" };
    SourceStatus st[SRC_COUNT];
    fetch_task_get_source_status(st);
    InfoRow right[SRC_COUNT + 2];
    int nr = 0;
    for (int i = 0; i < SRC_COUNT; i++) {
        if (!st[i].used) continue;
        bool failing = st[i].last_fail > st[i].last_ok;
        String v = failing ? "failed " + ago(st[i].last_fail, now)
                           : "ok " + ago(st[i].last_ok, now);
        if (failing && st[i].last_ok > 0) v += " (ok " + ago(st[i].last_ok, now) + ")";
        right[nr++] = { NAMES[i], v, (uint8_t)(failing ? 2 : 1) };
    }
    // Light sensor: room light and whether the backlight follows it
    if (light_available() && light_lux() >= 0)
        right[nr++] = { "Light", String(light_lux(), light_lux() < 10 ? 2 : 0) + " lux" +
                                 (display_auto_brightness() ? ", auto" : "") };
    // Battery life: average of the last runs until empty, per full charge
    int runs;
    float life = battery_life_hours(runs);
    right[nr++] = { "Battery life", life < 0 ? String("not measured yet")
                                             : "~" + String((int)lroundf(life)) + " h (" + runs +
                                               (runs == 1 ? " run)" : " runs)") };

    String footer_left = String("Tap to configure  -  ") + WEB_HOSTNAME + ".local";
    FooterInfo f;
    f.weather_str      = footer_left.c_str();
    f.uv_str           = "";
    f.wifi_ok          = WiFi.status() == WL_CONNECTED;
    f.battery_pct      = g_battery_pct;
    f.battery_charging = g_battery_charging;
    display_draw_info("Device", g_current_page, g_num_pages, left, nl, right, nr, f);
}

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║  TODAY PAGE                                                               ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
// The morning overview: weather and rain alert for this location, water
// temperatures, the portfolio's average change, parcel status, and the quote
// of the day. Values are in the large font, so rows are kept short.

static void draw_today_page() {
    time_t now = time(nullptr);
    InfoRow rows[5];
    int n = 0;

    if (g_weather.valid) {
        // Rain gets its own row (too long next to the temperature at this size);
        // otherwise the UV index fits beside it
        bool rain = g_weather.raining_now || g_weather.rain_start > 0;
        rows[n++] = { "Weather", weather_temp_text() + (rain ? String("") : "  " + weather_info_text()) };
        if (rain) {
            String r = weather_info_text();          // "Rain from 17:15" / "Rain until …" / "Raining"
            if (r.startsWith("Rain ")) r = r.substring(5);
            else if (r == "Raining")   r = "now";
            rows[n++] = { "Rain", r };
        }
    } else {
        rows[n++] = { "Weather", "loading..." };
    }

    WaterTemps water;
    DailyQuote quote;
    fetch_task_get_today(water, quote);
    if (!isnan(water.lake_c) || !isnan(water.river_c)) {
        String v;
        if (!isnan(water.lake_c))  v += "Lake " + String(water.lake_c, 1) + "°C";
        if (!isnan(water.river_c)) v += String(v.length() ? "  " : "") + "Limmat " + String(water.river_c, 1) + "°C";
        rows[n++] = { "Water", v };
    } else {
        rows[n++] = { "Water", "loading..." };
    }

    // Portfolio: average daily change of the tickers (no quantities are stored)
    int count = fetch_task_get_stocks(g_stock_quotes);
    float sum = 0;
    int ok = 0, up = 0, down = 0;
    for (int i = 0; i < count; i++) {
        if (g_stock_quotes[i].status != STOCK_OK) continue;
        float p = g_stock_quotes[i].change_pct;
        sum += p; ok++;
        if (p > 0.005f) up++; else if (p < -0.005f) down++;
    }
    if (ok > 0) {
        float avg = sum / ok;
        char buf[48];
        snprintf(buf, sizeof(buf), "%+.2f%%  (%d up, %d down)", avg, up, down);
        uint16_t col = avg > 0.005f ? COLOR_STOCK_UP : avg < -0.005f ? COLOR_STOCK_DOWN : 0;
        rows[n++] = { "Portfolio", buf, 0, col };
    }

    if (g_parcel_status.length() > 0) rows[n++] = { "Parcel", g_parcel_status };

    char label[32] = "Today";
    if (now > 1700000000) {
        struct tm t; localtime_r(&now, &t);
        strftime(label, sizeof(label), "Today  %a %d %b", &t);
    }
    FooterInfo f;
    f.battery_pct      = g_battery_pct;
    f.battery_charging = g_battery_charging;
    display_draw_today(label, g_current_page, g_num_pages, rows, n, quote.text, quote.author,
                       "Tap to show a clock", f);
}

// ── Weather animations ───────────────────────────────────────────────────────
// For the place on screen: rain/snow 15 min before it starts ("Rain in 12 min"),
// rain/snow that starts without warning ("Raining"), and a thunderstorm (at most
// hourly). Only while the pages are visible — what happened meanwhile plays when
// the screen wakes.
static time_t s_anim_rain_at  = 0;      // rain start (or start time) already shown
static bool   s_anim_was_wet  = false;
static time_t s_anim_storm_at = 0;

static bool pages_visible() {
    return display_screen_on() && !g_clock_showing && g_overlay_until_ms == 0 &&
           g_config_hint_stage == 0 && !pager_ui_active() && !display_weather_anim_active();
}

void weather_anim_preview(int kind) {
    static const char* const TEXT[] = { "Rain in 10 min", "Snow in 10 min", "Thunderstorm" };
    if (kind < 0 || kind > 2) return;
    display_weather_anim_start((WeatherAnim)kind, TEXT[kind]);
}

static void weather_anim_check() {
    time_t now = time(nullptr);
    if (!g_weather.valid || now < 1700000000 || !pages_visible()) return;
    const WeatherData& w = g_weather;
    bool storm = weather_code_storm(w.code_now);
    bool snowy = weather_code_snow(w.code_now) || (w.snow_today && w.temp_c <= 2.0f);
    bool wet   = w.raining_now;
    char msg[32];
    if (storm && (s_anim_storm_at == 0 || now - s_anim_storm_at >= 3600)) {
        s_anim_storm_at = now;
        display_weather_anim_start(WX_ANIM_STORM, "Thunderstorm");
    } else if (!wet && w.rain_start > 0 && w.rain_start - now <= 900 &&
               (s_anim_rain_at == 0 || labs((long)(w.rain_start - s_anim_rain_at)) > 1800)) {
        s_anim_rain_at = w.rain_start;
        int mins = std::max(1, (int)((w.rain_start - now + 59) / 60));
        snprintf(msg, sizeof(msg), "%s in %d min", snowy ? "Snow" : "Rain", mins);
        display_weather_anim_start(snowy ? WX_ANIM_SNOW : WX_ANIM_RAIN, msg);
    } else if (wet && !s_anim_was_wet && (s_anim_rain_at == 0 || now - s_anim_rain_at > 1800)) {
        s_anim_rain_at = now;
        display_weather_anim_start(snowy ? WX_ANIM_SNOW : WX_ANIM_RAIN, snowy ? "Snowing" : "Raining");
    }
    s_anim_was_wet = wet;
}

void views_pull_data() {
    // Parcel pulse animation tick
    parcel_pulse_tick();

    // Pull latest parcel status — start pulse animation on status change
    {
        String new_parcel;
        bool   parcel_changed = false;
        if (fetch_task_get_parcel(new_parcel, parcel_changed)) {
            g_parcel_status = new_parcel;
            // No flashing while the screen is off at night — the new status is
            // visible in the footer at the next wake-up
            if (parcel_changed && g_parcel_pulses > 0 && !g_parcel_pulsing &&
                display_screen_on() && !g_clock_showing) {
                g_parcel_pulsing   = true;
                g_parcel_pulse_rem = g_parcel_pulses;
                g_parcel_pulse_hi  = true;
                g_parcel_pulse_ms  = millis();
                display_pause_auto_brightness(true);
                display_set_brightness(100);
            }
        }
    }

    // Pull latest weather + commute from background cache once per second
    fetch_task_get_weather(g_weather, g_weather_city);
    weather_anim_check();
    if (commute_configured()) {
        CommuteData new_home, new_work;
        if (fetch_task_get_commute(new_home, new_work)) {
            // Clamp connection index if the number of connections shrank
            if (new_home.connection_count != g_commute_home.connection_count ||
                new_work.connection_count  != g_commute_work.connection_count) {
                if (g_commute_conn_idx >= new_home.connection_count) g_commute_conn_idx = 0;
                if (g_commute_conn_idx >= new_work.connection_count)  g_commute_conn_idx = 0;
            }
            g_commute_home = new_home;
            g_commute_work = new_work;
        }
    }
}

// ── One function per page ────────────────────────────────────────────────────

// The scene behind the clock for the weather outside (-1 = none). At night the
// forecast is refreshed only hourly, so the expected rain start and end times
// tell whether it's wet at this moment; otherwise the current weather code:
// 0–1 clear, 2 partly cloudy, 3 overcast, 45/48 fog, 80–82 showers — each by
// day (sun) or night (moon).
static int clock_scene() {
    int preview = clock_preview_kind();
    if (preview >= 0) return preview;
    if (!g_weather.valid) return -1;
    const WeatherData& w = g_weather;
    time_t now = time(nullptr);
    int  code  = w.code_now;
    bool day   = w.is_day;
    bool wet   = w.raining_now ? (w.rain_end == 0 || now < w.rain_end)
                               : (w.rain_start > 0 && now >= w.rain_start);
    if (weather_code_storm(code)) return day ? SCENE_STORM : SCENE_STORM_NIGHT;
    if (wet || (code >= 51 && code <= 86)) {
        bool snowy = weather_code_snow(code) || (w.snow_today && w.temp_c <= 2.0f);
        if (snowy) return day ? SCENE_SNOW : SCENE_SNOW_NIGHT;
        if (day && code >= 80 && code <= 82) return SCENE_SHOWERS;
        return day ? SCENE_RAIN : SCENE_RAIN_NIGHT;
    }
    if (code == 45 || code == 48) return day ? SCENE_FOG : SCENE_FOG_NIGHT;
    if (code == 3) return SCENE_CLOUDS;
    if (code == 2) return day ? SCENE_PARTLY_SUN : SCENE_PARTLY_MOON;
    return day ? SCENE_SUN : SCENE_MOON;
}

// Moving scenes only on USB power — on battery the same scene, still, redrawn
// once a minute (15 frames a second would drain it overnight)
static bool clock_animated() {
    return clock_preview_kind() >= 0 || battery_status().usb_power;
}

// Today page: a tap shows the clock, another tap goes back
static bool s_today_clock = false;
void toggle_today_clock() {
    s_today_clock = !s_today_clock;
    g_last_redraw_ms = 0;
    display_invalidate();
    if (s_today_clock) display_clock_hint("Swipe: clock style  \xC2\xB7  Tap: back", 4000);
    Log.printf("[Today] Clock %s\n", s_today_clock ? "on" : "off");
}
void today_clock_off() { s_today_clock = false; }
bool today_clock_showing() {
    return s_today_clock && g_pages[g_current_page].type == PageInfo::TODAY;
}

// Swipe on the Today clock: next/previous style (digital → split-flap → Matrix),
// remembered in Settings
void switch_clock_style(int step) {
    static const char* const NAMES[CLOCK_STYLE_COUNT] = { "digital", "split-flap", "Matrix" };
    static int style = -1;
    if (style < 0) style = config_load_clock_style();
    style = ((style + step) % CLOCK_STYLE_COUNT + CLOCK_STYLE_COUNT) % CLOCK_STYLE_COUNT;
    config_save_clock_style(style);
    display_set_clock_style(style);
    Log.printf("[Clock] Style → %s\n", NAMES[style]);
}

// The clock: date, plus the temperature of the last place shown. The night
// clock is dim; the Today page's clock is bright during the day.
static void draw_night_clock() {
    char sub[40] = "";
    time_t now_t = time(nullptr);
    if (now_t > 1700000000) {
        struct tm t; localtime_r(&now_t, &t);
        strftime(sub, sizeof(sub), "%a %d %b", &t);
        if (g_weather.valid) {
            size_t len = strlen(sub);
            snprintf(sub + len, sizeof(sub) - len, "   %d°C", (int)lroundf(g_weather.temp_c));
        }
    }
    bool bright = (!g_clock_showing || clock_preview_kind() >= 0) && !display_is_night();
    display_draw_night_clock(sub, clock_scene(), clock_animated(), bright);
}

// Transit stop: the departure board, or one of the tracked flights
static void draw_transit_page(const PageInfo& page) {
    g_current_stop = page.transit_idx;
    if (g_view != VIEW_BOARD) {
        int slot = (g_view == VIEW_FLIGHT1) ? 1 : 0;
        FlightInfo fi;
        flight_tracker_get(slot, fi);
        display_draw_flight(slot, g_flight_count, fi, WiFi.status() == WL_CONNECTED);
        return;
    }
    std::vector<Departure> departures;
    time_t fetch_time;
    bool   from_cache;
    fetch_task_get(g_current_stop, departures, fetch_time, from_cache);

    const StopConfig& stop = g_stop_configs[g_current_stop];
    int age_s = (fetch_time > 0) ? (int)(time(nullptr) - fetch_time) : 0;

    char weather_str[40];
    char uv_str[56];
    format_footer_weather(weather_str, sizeof(weather_str),
                          uv_str, sizeof(uv_str), "Package: ");

    display_draw_board(
        stop.label,
        g_current_page,
        g_num_pages,
        departures,
        weather_str,
        uv_str,
        g_weather.valid && g_weather.rain_today,
        g_weather.valid ? g_weather.precip_prob_pct : 0,
        from_cache,
        WiFi.status() == WL_CONNECTED,
        age_s,
        fetch_time,
        g_large_font,
        g_countdown_target,
        g_countdown_icon,
        g_weather.valid && g_weather.snow_today,
        g_weather.valid && g_weather.clear_today,
        g_battery_pct,
        g_battery_charging
    );
}

// Portfolio: the ticker list (rotating through groups), or one ticker's details
static void draw_stocks_page(uint32_t now_ms) {
    int count    = fetch_task_get_stocks(g_stock_quotes);
    int per_page = stocks_per_page(g_stocks_cfg.layout, count);
    int n_sub    = stock_sub_page_count();
    bool market_open = stocks_market_open(time(nullptr));

    char weather_str[40];
    char uv_str[56];
    format_footer_weather(weather_str, sizeof(weather_str), uv_str, sizeof(uv_str), "");

    if (g_stock_detail >= count) set_stock_detail(-1);   // ticker list shrank
    if (g_stock_detail >= 0) {
        // Detail view of one ticker (no auto-rotation)
        const StockQuote& q = g_stock_quotes[g_stock_detail];
        char label[40];
        snprintf(label, sizeof(label), "%s  %d/%d",
                 stocks_display_symbol(q.symbol).c_str(), g_stock_detail + 1, count);
        StockChart chart;
        bool have_chart = fetch_task_get_stock_chart(g_stock_detail, chart);
        int age_s = (q.fetch_time > 0) ? (int)(time(nullptr) - q.fetch_time) : 0;

        FooterInfo f;
        f.weather_str = weather_str;
        f.uv_str      = uv_str;
        f.rain_today  = g_weather.valid && g_weather.rain_today;
        f.rain_pct    = g_weather.valid ? g_weather.precip_prob_pct : 0;
        f.snow_today  = g_weather.valid && g_weather.snow_today;
        f.clear_today = g_weather.valid && g_weather.clear_today;
        f.wifi_ok     = WiFi.status() == WL_CONNECTED;
        f.battery_pct = g_battery_pct;
        f.battery_charging = g_battery_charging;
        display_draw_stock_detail(label, g_current_page, g_num_pages, q,
                                  have_chart ? &chart : nullptr, market_open, age_s, f);
        return;
    }

    // Auto-rotate through ticker groups (swipe up/down resets the timer)
    if (n_sub > 1 && now_ms - g_stock_rotate_ms >= (uint32_t)g_stocks_cfg.rotate_sec * 1000)
        set_stock_sub_page(g_stock_sub_page + 1);
    if (g_stock_sub_page >= n_sub) g_stock_sub_page = 0;

    char label[32] = "Portfolio";
    if (n_sub > 1)
        snprintf(label, sizeof(label), "Portfolio %d/%d", g_stock_sub_page + 1, n_sub);

    // Footer age = time since the newest quote arrived
    time_t newest = 0;
    for (int i = 0; i < count; i++) newest = std::max(newest, g_stock_quotes[i].fetch_time);
    int age_s = (newest > 0) ? (int)(time(nullptr) - newest) : 0;

    display_draw_stocks(label, g_current_page, g_num_pages,
                        g_stock_quotes, count,
                        g_stock_sub_page * per_page, per_page, market_open, age_s,
                        WiFi.status() == WL_CONNECTED, g_battery_pct, g_battery_charging,
                        weather_str, uv_str,
                        g_weather.valid && g_weather.rain_today,
                        g_weather.valid ? g_weather.precip_prob_pct : 0,
                        g_weather.valid && g_weather.snow_today,
                        g_weather.valid && g_weather.clear_today);
}

static void draw_weather_page() {
    WeatherDetail wd;
    String name;
    bool not_found = false;
    bool have = fetch_task_get_weather_detail(g_wx_slot, wd, name, not_found);
    char label[56];
    if (g_wx_count > 1)
        snprintf(label, sizeof(label), "Weather  %s  %d/%d", name.c_str(), g_wx_slot + 1, g_wx_count);
    else
        snprintf(label, sizeof(label), "Weather  %s", name.c_str());
    FooterInfo f;
    f.wifi_ok          = WiFi.status() == WL_CONNECTED;
    f.battery_pct      = g_battery_pct;
    f.battery_charging = g_battery_charging;
    display_draw_weather(label, g_current_page, g_num_pages,
                         have ? &wd : nullptr, not_found, f);
}

static void draw_commute_page(const PageInfo& page) {
    bool is_home = (page.type == PageInfo::COMMUTE_HOME);
    CommuteData& cd = is_home ? g_commute_home : g_commute_work;
    const char* label = is_home ? "Work -> Home" : "Home -> Work";
    char weather_str[40];
    char uv_str[56];
    format_footer_weather(weather_str, sizeof(weather_str), uv_str, sizeof(uv_str), "");
    display_draw_commute(label, g_current_page, g_num_pages,
                         cd, g_commute_conn_idx,
                         WiFi.status() == WL_CONNECTED, g_battery_pct, g_battery_charging,
                         weather_str, uv_str,
                         g_weather.valid && g_weather.rain_today,
                         g_weather.valid ? g_weather.precip_prob_pct : 0,
                         g_weather.valid && g_weather.snow_today,
                         g_weather.valid && g_weather.clear_today);
}

// Redraw every second (clock + age counter; commute marquee advances from millis()),
// unless a prompt or overlay is covering the screen
void draw_current_page(uint32_t now_ms) {
    const PageInfo& page = g_pages[g_current_page];
    // The weather page's extra data is only fetched while someone can see it
    fetch_task_set_weather_page(page.type == PageInfo::WEATHER && display_screen_on() &&
                                !g_clock_showing, g_wx_slot);
    if (g_config_hint_stage != 0 || overlay_active() || !display_screen_on()) return;
    // The clock (night, or Today tapped): every loop while its scene moves —
    // it paces its own frames and redraws a still clock only once a minute
    if (g_clock_showing || today_clock_showing()) { draw_night_clock(); return; }
    if (now_ms - g_last_redraw_ms < 1000) return;
    g_last_redraw_ms = now_ms;

    switch (page.type) {
        case PageInfo::TRANSIT:      draw_transit_page(page);   break;
        case PageInfo::STOCKS:       draw_stocks_page(now_ms);  break;
        case PageInfo::WEATHER:      draw_weather_page();       break;
        case PageInfo::DEVICE:       draw_device_info_page();   break;
        case PageInfo::TODAY:        draw_today_page();         break;
        case PageInfo::COMMUTE_HOME:
        case PageInfo::COMMUTE_WORK: draw_commute_page(page);   break;
    }
}
