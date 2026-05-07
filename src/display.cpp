// ─────────────────────────────────────────────────────────────────────────────
// display.cpp — Display rendering using Arduino_GFX
// ─────────────────────────────────────────────────────────────────────────────
// We use Arduino_GFX (not TFT_eSPI) because the T-Display S3 Pro's panel
// has a 49-pixel column offset that TFT_eSPI doesn't expose.
// ─────────────────────────────────────────────────────────────────────────────

#include "display.h"
#include "../include/config.h"
#include "../include/tramli_fonts.h"
#include "../include/matrix_font.h"
#include <Arduino_GFX_Library.h>

static const int SMALL_ASCENT  = 15;   // TramliSmall  16px
static const int LARGE_ASCENT  = 22;   // TramliLarge  24px
static const int XLARGE_ASCENT = 26;   // TramliXLarge 28px

// ── Pin assignments ───────────────────────────────────────────────────────────
#define BOARD_TFT_DC    9
#define BOARD_TFT_CS    39
#define BOARD_TFT_RST   47
#define BOARD_TFT_BL    48
#define BOARD_SPI_SCK   18
#define BOARD_SPI_MOSI  17
#define BOARD_SPI_MISO   8

// ── Display objects — declared as pointers, initialized in display_init() ─────
// IMPORTANT: Never use 'new' at global scope on ESP32 — it runs before
// the hardware is ready and will crash. We declare null pointers here and
// assign them inside display_init() after setup() has started.
static Arduino_DataBus *bus = nullptr;
static Arduino_GFX     *gfx = nullptr;

// ── Partial-redraw state ──────────────────────────────────────────────────────
// Header redraws on minute change, rows on new fetch data, footer is split:
//   static region (weather/UV) redraws on weather change (~15 min)
//   dynamic region (age + dot) redraws every second
static time_t    s_last_fetch_time    = -1;
static int       s_last_clock_minute  = -1;
static uint32_t  s_last_weather_hash  = 0xFFFFFFFF;
static bool      s_footer_static_drawn = false;
// Flight screen partial-redraw state
static int    s_flight_last_slot   = -1;
static int    s_flight_last_minute = -1;
static time_t s_flight_last_fetch  = -1;

// ── Backlight PWM ─────────────────────────────────────────────────────────────
#define BL_PWM_CHANNEL    0
#define BL_PWM_FREQ       5000
#define BL_PWM_RESOLUTION 8

// ── Color helper (RGB888 → RGB565) ────────────────────────────────────────────
static uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// ── UTF-8 → Latin-1 conversion ───────────────────────────────────────────────
// TramliSmall/Large cover 0x20–0xFF (Latin-1), so we map UTF-8 multibyte
// sequences to their Latin-1 equivalents instead of falling back to ASCII.
static String to_latin1(const String& src) {
    String out;
    out.reserve(src.length());
    size_t i = 0;
    while (i < src.length()) {
        uint8_t b = (uint8_t)src[i];
        if (b < 0x80) {
            out += (char)b;
            i++;
        } else if (b == 0xC3 && i + 1 < src.length()) {
            uint8_t b2 = (uint8_t)src[i + 1];
            char c = '?';
            switch (b2) {
                case 0xA4: c = '\xE4'; break;  // ä
                case 0xB6: c = '\xF6'; break;  // ö
                case 0xBC: c = '\xFC'; break;  // ü
                case 0x84: c = '\xC4'; break;  // Ä
                case 0x96: c = '\xD6'; break;  // Ö
                case 0x9C: c = '\xDC'; break;  // Ü
                case 0x9F: c = '\xDF'; break;  // ß
                case 0xA9: c = '\xE9'; break;  // é
                case 0xA8: c = '\xE8'; break;  // è
                case 0xAA: c = '\xEA'; break;  // ê
                case 0xA0: c = '\xE0'; break;  // à
                default:   c = '?';    break;
            }
            out += c;
            i += 2;
        } else if (b >= 0xC0 && b < 0xE0) { out += '?'; i += 2; }
        else if (b >= 0xE0 && b < 0xF0)   { out += '?'; i += 3; }
        else                               { out += '?'; i++; }
    }
    return out;
}

// ── Text width helper ─────────────────────────────────────────────────────────
static int16_t text_w(const String& s, uint8_t size) {
    if      (size == 2) gfx->setFont(&TramliSmall);
    else if (size == 4) gfx->setFont(&TramliXLarge);
    else                gfx->setFont(&TramliLarge);
    gfx->setTextSize(1);
    int16_t x1, y1; uint16_t w, h;
    gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
    return (int16_t)w;
}

// ── Draw text at x,y (y = top of the character cell) ─────────────────────────
static void draw_text(const String& s, int16_t x, int16_t y,
                      uint8_t size, uint16_t color) {
    int asc;
    if      (size == 2) { gfx->setFont(&TramliSmall);  asc = SMALL_ASCENT;  }
    else if (size == 4) { gfx->setFont(&TramliXLarge); asc = XLARGE_ASCENT; }
    else                { gfx->setFont(&TramliLarge);  asc = LARGE_ASCENT;  }
    gfx->setTextSize(1);
    gfx->setTextColor(color);
    gfx->setCursor(x, y + asc);   // GFX fonts use baseline as cursor origin
    gfx->print(s);
}

// ── Weather hash for footer static redraw guard ───────────────────────────────
static uint32_t weather_hash(const char* weather_str, const char* uv_str,
                              bool rain_today, int rain_pct) {
    uint32_t h = (rain_today ? 1u : 0u) ^ ((uint32_t)(rain_pct & 0xFF) << 1);
    for (const char* p = weather_str; p && *p; p++) h ^= (uint32_t)*p << 8;
    for (const char* p = uv_str;      p && *p; p++) h ^= (uint32_t)*p << 16;
    return h;
}

// ═════════════════════════════════════════════════════════════════════════════
//  PUBLIC INTERFACE
// ═════════════════════════════════════════════════════════════════════════════

void display_init() {
    // Create objects HERE (inside a function, after setup() has started).
    // This is the correct place — hardware is ready, PSRAM is available.
    bus = new Arduino_ESP32SPI(
        BOARD_TFT_DC,  BOARD_TFT_CS,
        BOARD_SPI_SCK, BOARD_SPI_MOSI, BOARD_SPI_MISO
    );

    // ST7796, 49-pixel column offset (required for this specific Pro panel).
    // Without the offset of 49, drawing is shifted and the factory boot
    // image bleeds through at the bottom.
    gfx = new Arduino_ST7796(
        bus,           // SPI bus
        BOARD_TFT_RST, // reset pin
        0,             // rotation 0 (portrait) — we call setRotation(3) below
        true,          // IPS panel
        222, 480,      // native width x height
        49, 0          // column offset, row offset
    );

    gfx->begin();
    gfx->setRotation(3);    // landscape, USB-C on the left — confirmed correct
    // Note: ips=true in the constructor already handles color inversion for
    // this IPS panel. Do NOT call invertDisplay() — it would double-invert.
    gfx->fillScreen(BLACK);

    // Backlight — digitalWrite HIGH first for V1.0 reliability, then PWM
    pinMode(BOARD_TFT_BL, OUTPUT);
    digitalWrite(BOARD_TFT_BL, HIGH);
    ledcSetup(BL_PWM_CHANNEL, BL_PWM_FREQ, BL_PWM_RESOLUTION);
    ledcAttachPin(BOARD_TFT_BL, BL_PWM_CHANNEL);
    display_set_brightness(DAY_BRIGHTNESS);
    Serial.println("[Display] Initialized (rotation=3, ips=true)");
}

void display_set_brightness(uint8_t percent) {
    if (percent > 100) percent = 100;
    uint8_t duty = (uint16_t)percent * 255 / 100;
    ledcWrite(BL_PWM_CHANNEL, duty);
}

void display_show_status(const char* message) {
    if (!gfx) return;
    s_last_fetch_time     = -1;
    s_last_clock_minute   = -1;
    s_footer_static_drawn = false;
    gfx->fillScreen(BLACK);
    String s = to_latin1(message);
    int16_t w = text_w(s, 3);
    draw_text(s, (SCREEN_W - w) / 2, SCREEN_H / 2 - 14, 3, COLOR_ROW0);
}

void display_invalidate() {
    s_last_fetch_time     = -1;
    s_last_clock_minute   = -1;
    s_footer_static_drawn = false;
    s_flight_last_slot    = -1;
    s_flight_last_minute  = -1;
    s_flight_last_fetch   = -1;
}

// ── Umbrella icon ─────────────────────────────────────────────────────────────
static void draw_umbrella(int x, int y, uint16_t color) {
    gfx->fillRect(x + 5, y,      2, 2, color);  // tip
    gfx->fillRect(x + 3, y + 2,  6, 2, color);  // canopy top
    gfx->fillRect(x + 1, y + 4, 10, 2, color);  // canopy mid
    gfx->fillRect(x,     y + 6, 12, 2, color);  // canopy wide
    gfx->fillRect(x + 5, y + 8,  2, 5, color);  // handle
    gfx->fillRect(x + 2, y + 13, 4, 2, color);  // crook
}

// ── Countdown icon ────────────────────────────────────────────────────────────
static void draw_countdown_icon(int icon, int x, int y, uint16_t col) {
    if (icon != 2) return;
    // Calendar (12×12): box outline, header bar, two rings, 3×2 date grid
    gfx->drawRect(x, y + 2, 12, 10, col);
    gfx->fillRect(x + 1, y + 3, 10, 3, col);
    gfx->fillRect(x + 3, y,     2,  4, col);
    gfx->fillRect(x + 7, y,     2,  4, col);
    for (int r = 0; r < 2; r++)
        for (int c = 0; c < 3; c++)
            gfx->fillRect(x + 2 + c * 4, y + 7 + r * 3, 2, 2, col);
}

// ── Bus icon ──────────────────────────────────────────────────────────────────
static void draw_bus_icon(int x, int y, uint16_t color) {
    gfx->fillRect(x - 4,  y + 7,  4, 4, color);  // left mirror
    gfx->fillRect(x + 28, y + 7,  4, 4, color);  // right mirror
    gfx->fillRect(x,      y + 3, 28, 22, color);  // body
    gfx->fillRect(x + 2,  y + 3, 24,  3, BLACK);  // destination sign
    gfx->fillRect(x + 2,  y + 8, 24, 10, BLACK);  // windshield
    gfx->fillRect(x,      y + 18,28,  2, color);  // belt
    gfx->fillRect(x + 2,  y + 20, 8,  3, BLACK);  // left headlight
    gfx->fillRect(x + 18, y + 20, 8,  3, BLACK);  // right headlight
    gfx->fillRect(x,      y + 25, 7,  3, color);  // left wheel
    gfx->fillRect(x + 21, y + 25, 7,  3, color);  // right wheel
}

// ── Night-mode brightness ─────────────────────────────────────────────────────
static void apply_night_brightness() {
    time_t now = time(nullptr);
    if (now < 1700000000) return;   // clock not synced yet, skip
    struct tm t;
    localtime_r(&now, &t);
    int h = t.tm_hour;
    bool is_night = (NIGHT_START_HOUR > NIGHT_END_HOUR)
                    ? (h >= NIGHT_START_HOUR || h < NIGHT_END_HOUR)
                    : (h >= NIGHT_START_HOUR && h < NIGHT_END_HOUR);
    display_set_brightness(is_night ? NIGHT_BRIGHTNESS : DAY_BRIGHTNESS);
}

// ── Header ────────────────────────────────────────────────────────────────────
static void draw_header(const char* stop_name, int stop_index, int stop_count,
                        bool rain_active, bool large_font) {
    gfx->fillRect(0, 0, SCREEN_W, HEADER_H, BLACK);

    uint8_t fsz = large_font ? 3 : 2;
    int     fh  = large_font ? 28 : 16;   // yAdvance for vertical centering
    int     ty  = (HEADER_H - fh) / 2;

    // Clock — show "--:--" until NTP sync
    char clock_buf[6] = "--:--";
    time_t now = time(nullptr);
    if (now > 1700000000) {
        struct tm t;
        localtime_r(&now, &t);
        snprintf(clock_buf, sizeof(clock_buf), "%02d:%02d", t.tm_hour, t.tm_min);
    }
    String clock_str = clock_buf;

    // Build right cluster right-to-left: clock, umbrella, dots
    int16_t clock_w = text_w(clock_str, fsz);
    int rx = SCREEN_W - PAD - clock_w;
    int clock_x = rx;

    int umbrella_x = -1;
    if (rain_active) { rx -= 16; umbrella_x = rx; }

    int dots_w = stop_count * 12;
    int dots_x = (dots_w > 0) ? (rx -= 8, rx -= dots_w, rx) : rx;

    // Stop label — truncate if too wide
    String label = to_latin1(stop_name);
    int label_max = dots_x - PAD - 8;
    if (text_w(label, fsz) > label_max) {
        int comma = label.indexOf(", ");
        if (comma >= 0) label = label.substring(comma + 2);
        while (label.length() > 1 && text_w(label, fsz) > label_max)
            label = label.substring(0, label.length() - 1);
    }

    draw_text(label, PAD, ty, fsz, COLOR_ROW0);
    draw_text(clock_str, clock_x, ty, fsz, COLOR_ROW0);

    if (umbrella_x >= 0)
        draw_umbrella(umbrella_x, ty, COLOR_ROW0);

    // Stop indicator dots — center them vertically in header
    int dot_y = (HEADER_H - 6) / 2;
    for (int i = 0; i < stop_count; i++) {
        uint16_t c = (i == stop_index) ? COLOR_ROW0 : COLOR_META;
        gfx->fillRect(dots_x + i * 12, dot_y, 6, 6, c);
    }

    gfx->drawFastHLine(0, HEADER_H - 1, SCREEN_W, COLOR_META);
}

// ── Rows ──────────────────────────────────────────────────────────────────────
static void draw_rows(const std::vector<Departure>& departures, time_t fetch_time,
                      bool large_font) {
    int rows_top = HEADER_H;
    int rows_h   = SCREEN_H - FOOTER_H - rows_top;
    int max_rows = large_font ? 2 : ROW_COUNT;
    int row_h    = rows_h / max_rows;

    gfx->fillRect(0, rows_top, SCREEN_W, rows_h, BLACK);

    if (departures.empty()) {
        String msg = (fetch_time > 0) ? "No more departures" : "Loading...";
        uint8_t fsz = large_font ? 3 : 2;
        draw_text(msg, (SCREEN_W - text_w(msg, fsz)) / 2,
                  rows_top + rows_h / 2 - 14, fsz, COLOR_META);
        return;
    }

    // Column widths — scaled up for the bigger font in large mode
    uint8_t row_fsz    = large_font ? 4 : 3;
    int     row_yadv   = large_font ? 32 : 28;  // yAdvance for vertical centering
    int num_col_end    = large_font ? (PAD + 65) : (PAD + 54);
    int dest_start     = num_col_end + (large_font ? 16 : 14);
    int right_margin   = large_font ? 75 : 64;

    int count = std::min((int)departures.size(), max_rows);
    for (int i = 0; i < count; i++) {
        const Departure& dep = departures[i];
        int y      = rows_top + i * row_h;
        int text_y = y + (row_h - row_yadv) / 2;

        bool disrupted = (dep.delay >= 2);
        uint16_t color = disrupted ? COLOR_DIM : COLOR_ROW0;

        // Line number — right-aligned in number column
        String ln = to_latin1(dep.line);
        draw_text(ln, num_col_end - text_w(ln, row_fsz), text_y, row_fsz, color);

        // Destination — truncated to fit, leaving room for delay badge + time
        String dest    = to_latin1(dep.destination);
        int dest_max_w = SCREEN_W - dest_start - right_margin - PAD;
        if (disrupted) dest_max_w -= 52;
        while (dest.length() > 1 && text_w(dest, row_fsz) > dest_max_w)
            dest = dest.substring(0, dest.length() - 1);
        draw_text(dest, dest_start, text_y, row_fsz, color);

        // Delay badge (smaller font, between dest and time)
        if (disrupted && dep.delay > 0) {
            char badge[8];
            snprintf(badge, sizeof(badge), "+%dm", dep.delay);
            String bs = badge;
            int bx = SCREEN_W - PAD - right_margin - text_w(bs, 2) - 4;
            draw_text(bs, bx, text_y + 4, 2, COLOR_DIM);
        }

        // Time or bus icon
        if (dep.minutes == 0) {
            draw_bus_icon(SCREEN_W - PAD - 32, text_y - 2, color);
        } else {
            char tbuf[8];
            snprintf(tbuf, sizeof(tbuf), "%d'", dep.minutes);
            String ts = tbuf;
            draw_text(ts, SCREEN_W - PAD - text_w(ts, row_fsz), text_y, row_fsz, color);
        }

        // Row separator
        if (i < count - 1)
            gfx->drawFastHLine(0, y + row_h - 1, SCREEN_W, rgb(20, 12, 0));
    }
}

// ── Footer — static region (weather, UV, umbrella) ────────────────────────────
// Erases + redraws only the left portion (SCREEN_W - FOOTER_AGE_REGION_W wide).
// Called only when weather data changes or after a full-screen clear.
static void draw_footer_static(int fy, const char* weather_str, const char* uv_str,
                                bool rain_today, int rain_pct, bool large_font) {
    gfx->fillRect(0, fy, SCREEN_W - FOOTER_AGE_REGION_W, FOOTER_H, BLACK);
    gfx->drawFastHLine(0, fy, SCREEN_W, rgb(20, 12, 0));  // full-width separator

    uint8_t fsz = large_font ? 3 : 2;
    int     fh  = large_font ? 28 : 16;
    int     ty  = fy + (FOOTER_H - fh) / 2;
    int     uby = fy + (FOOTER_H - 15) / 2;  // umbrella icon vertical center
    int     x   = PAD;

    if (weather_str && strlen(weather_str) > 0) {
        String s = to_latin1(weather_str);
        draw_text(s, x, ty, fsz, COLOR_ROW0);
        x += text_w(s, fsz) + 14;
    }
    if (rain_today) {
        if (rain_pct > 0) {
            char pct_buf[6];
            snprintf(pct_buf, sizeof(pct_buf), "%d%%", rain_pct);
            String ps = pct_buf;
            draw_text(ps, x, ty, fsz, COLOR_ROW0);
            x += text_w(ps, fsz) + 10;
        }
        draw_umbrella(x, uby, COLOR_ROW0);
        x += 28;
    }
    if (uv_str && strlen(uv_str) > 0) {
        String s = to_latin1(uv_str);
        draw_text(s, x, ty, fsz, COLOR_ROW0);
    }
}

// ── Footer — dynamic region (age counter + status dot) ────────────────────────
// Erases + redraws only the right FOOTER_AGE_REGION_W pixels every second.
// Starts erase at fy+1 to preserve the separator line drawn by draw_footer_static.
static void draw_footer_dynamic(int fy, bool from_cache, bool wifi_ok,
                                int age_seconds, bool large_font,
                                time_t countdown_target, int countdown_icon) {
    gfx->fillRect(SCREEN_W - FOOTER_AGE_REGION_W, fy + 1,
                  FOOTER_AGE_REGION_W, FOOTER_H - 1, BLACK);

    uint8_t fsz = large_font ? 3 : 2;
    int     fh  = large_font ? 28 : 16;
    int     ty  = fy + (FOOTER_H - fh) / 2;
    int     uby = fy + (FOOTER_H - 15) / 2;

    char text_buf[16];
    time_t now = time(nullptr);
    long   secs_left = (countdown_target > 0) ? (long)(countdown_target - now) : -1;

    if (secs_left > 0) {
        if (secs_left >= 86400)
            snprintf(text_buf, sizeof(text_buf), "%ldd%ldh",
                     secs_left / 86400, (secs_left % 86400) / 3600);
        else if (secs_left >= 3600)
            snprintf(text_buf, sizeof(text_buf), "%ldh%ldm",
                     secs_left / 3600, (secs_left % 3600) / 60);
        else if (secs_left >= 60)
            snprintf(text_buf, sizeof(text_buf), "%ldm%lds",
                     secs_left / 60, secs_left % 60);
        else
            snprintf(text_buf, sizeof(text_buf), "%lds", secs_left);
    } else {
        if (from_cache)            snprintf(text_buf, sizeof(text_buf), "cached");
        else if (age_seconds < 60) snprintf(text_buf, sizeof(text_buf), "now");
        else                       snprintf(text_buf, sizeof(text_buf), "%dm", age_seconds / 60);
    }

    String ts = text_buf;
    int text_x = SCREEN_W - PAD - 14 - text_w(ts, fsz);
    draw_text(ts, text_x, ty, fsz, COLOR_ROW0);

    if (secs_left > 0 && countdown_icon > 0) {
        int icon_x = text_x - 4 - 12;
        int icon_y = fy + (FOOTER_H - 10) / 2;
        if (icon_x >= SCREEN_W - FOOTER_AGE_REGION_W)
            draw_countdown_icon(countdown_icon, icon_x, icon_y, COLOR_ROW0);
    }

    uint16_t dot;
    if (!wifi_ok) dot = ((millis() / 500) % 2 == 0) ? rgb(180, 30, 0) : BLACK;
    else if (from_cache) dot = COLOR_DIM;
    else                 dot = COLOR_META;
    gfx->fillRect(SCREEN_W - PAD - 6, uby + 4, 6, 6, dot);
}

// ── Partial board redraw (called every second from main loop) ─────────────────
// Header:        redraws only on minute change
// Rows:          redraws only when fetch_time changes (new data arrived)
// Footer static: redraws only when weather changes (~15 min)
// Footer dynamic: redraws every call (age counter increments every second)
void display_draw_board(
    const char* stop_name, int stop_index, int stop_count,
    const std::vector<Departure>& departures,
    const char* weather_str, const char* uv_str,
    bool rain_today, int rain_pct,
    bool from_cache, bool wifi_ok, int age_seconds,
    time_t fetch_time, bool large_font_mode,
    time_t countdown_target, int countdown_icon)
{
    if (!gfx) return;
    apply_night_brightness();

    time_t now_t = time(nullptr);
    struct tm tm_now;
    localtime_r(&now_t, &tm_now);
    int cur_minute = tm_now.tm_hour * 60 + tm_now.tm_min;

    if (cur_minute != s_last_clock_minute) {
        s_last_clock_minute = cur_minute;
        draw_header(stop_name, stop_index, stop_count, false, large_font_mode);
    }

    if (fetch_time != s_last_fetch_time) {
        s_last_fetch_time = fetch_time;
        draw_rows(departures, fetch_time, large_font_mode);
    }

    int fy = SCREEN_H - FOOTER_H;
    uint32_t wh = weather_hash(weather_str, uv_str, rain_today, rain_pct);
    if (wh != s_last_weather_hash || !s_footer_static_drawn) {
        s_last_weather_hash   = wh;
        s_footer_static_drawn = true;
        draw_footer_static(fy, weather_str, uv_str, rain_today, rain_pct, large_font_mode);
    }
    draw_footer_dynamic(fy, from_cache, wifi_ok, age_seconds, large_font_mode, countdown_target, countdown_icon);
}

// ── Flight screen ─────────────────────────────────────────────────────────────

// Plane icon pointing right, centered at (cx, cy), ~14×10 px
static void draw_plane_right(int cx, int cy, uint16_t col) {
    gfx->fillRect(cx - 6, cy - 1, 12, 3, col);  // fuselage
    gfx->fillRect(cx + 5, cy,      2, 1, col);  // nose tip
    gfx->fillRect(cx - 1, cy - 5,  3, 11, col); // main wing
    gfx->fillRect(cx - 6, cy - 3,  3, 7, col);  // tail body
}

// Heading arrow: simple 6×6 compass indicator
static const char* heading_label(float deg) {
    if (deg < 22.5f || deg >= 337.5f)  return "N";
    if (deg < 67.5f)  return "NE";
    if (deg < 112.5f) return "E";
    if (deg < 157.5f) return "SE";
    if (deg < 202.5f) return "S";
    if (deg < 247.5f) return "SW";
    if (deg < 292.5f) return "W";
    return "NW";
}

static void draw_flight_footer(time_t now_t, const FlightInfo& fi, bool wifi_ok) {
    int fy = SCREEN_H - FOOTER_H;
    gfx->drawFastHLine(0, fy, SCREEN_W, rgb(20, 12, 0));

    char age_buf[16] = "no data";
    if (fi.fetched_at > 0 && now_t >= fi.fetched_at) {
        int age_s = (int)(now_t - fi.fetched_at);
        if (age_s < 60)        snprintf(age_buf, sizeof(age_buf), "now");
        else if (age_s < 3600) snprintf(age_buf, sizeof(age_buf), "%dm", age_s / 60);
        else                   snprintf(age_buf, sizeof(age_buf), "%dh", age_s / 3600);
    }
    String age_str = age_buf;
    int ty = fy + (FOOTER_H - 16) / 2;
    int16_t aw = text_w(age_str, 2);
    draw_text(age_str, SCREEN_W - PAD - 14 - aw, ty, 2, COLOR_ROW0);

    String hint = "swipe left for stops";
    draw_text(hint, PAD, ty, 2, COLOR_META);

    uint16_t dot = wifi_ok ? COLOR_META : ((millis() / 500) % 2 == 0 ? rgb(180,30,0) : BLACK);
    gfx->fillRect(SCREEN_W - PAD - 6, fy + (FOOTER_H - 6) / 2, 6, 6, dot);
}

static void draw_flight_content(time_t now_t, const FlightInfo& fi) {
    const int CTY    = HEADER_H + 4;
    const int CTH    = SCREEN_H - FOOTER_H - HEADER_H - 4;
    const int APT_Y  = CTY + 4;
    const int BAR_Y  = CTY + 60;
    const int STATS_Y = CTY + 86;
    const int TIMES_Y = CTY + 112;

    if (fi.callsign.isEmpty()) {
        String msg = "No flight configured";
        draw_text(msg, (SCREEN_W - text_w(msg, 2)) / 2, CTY + CTH / 2 - 8, 2, COLOR_META);
        return;
    }
    if (!fi.valid) {
        String msg = "Searching...";
        draw_text(msg, (SCREEN_W - text_w(msg, 2)) / 2, CTY + CTH / 2 - 8, 2, COLOR_META);
        return;
    }

    // Airport codes
    String dep = fi.dep_icao.length() > 0 ? fi.dep_icao : "????";
    String arr = fi.arr_icao.length() > 0 ? fi.arr_icao : "????";
    int16_t dep_w = text_w(dep, 3);
    int16_t arr_w = text_w(arr, 3);
    draw_text(dep, PAD,                        APT_Y, 3, COLOR_ROW0);
    draw_text(arr, SCREEN_W - PAD - arr_w,     APT_Y, 3, COLOR_ROW0);

    // Status label (centered, below airport codes)
    String status_str;
    if (fi.airborne)                                    status_str = "In flight";
    else if (fi.arr_time > 0 && now_t > fi.arr_time)   status_str = "Landed";
    else if (fi.dep_time > 0 && now_t < fi.dep_time)   status_str = "Pre-flight";
    else if (!fi.on_ground)                             status_str = "In flight";
    else                                                status_str = "On ground";
    draw_text(status_str, (SCREEN_W - text_w(status_str, 2)) / 2, APT_Y + 30, 2, COLOR_META);

    // Progress bar + plane marker
    float progress = 0.0f;
    if (fi.dep_time > 0 && fi.arr_time > 0) {
        if (now_t <= fi.dep_time)      progress = 0.0f;
        else if (now_t >= fi.arr_time) progress = 1.0f;
        else progress = (float)(now_t - fi.dep_time) / (float)(fi.arr_time - fi.dep_time);
    } else if (fi.dep_time > 0 && fi.arr_time == 0 && fi.airborne) {
        long elapsed = (long)(now_t - fi.dep_time);
        progress = elapsed / (10.0f * 3600.0f);
        if (progress > 0.95f) progress = 0.95f;
    }

    const int BAR_LX = PAD + dep_w + 14;
    const int BAR_RX = SCREEN_W - PAD - arr_w - 14;
    const int BAR_W  = BAR_RX - BAR_LX;
    if (BAR_W > 20) {
        gfx->drawFastHLine(BAR_LX, BAR_Y + 1, BAR_W, COLOR_META);
        int plane_cx = BAR_LX + (int)(progress * BAR_W);
        if (plane_cx < BAR_LX + 7) plane_cx = BAR_LX + 7;
        if (plane_cx > BAR_RX - 7) plane_cx = BAR_RX - 7;
        draw_plane_right(plane_cx, BAR_Y, COLOR_ROW0);
    }

    // Stats row: altitude, speed, heading (only when airborne + data present)
    if (fi.airborne && fi.alt_ft > 0) {
        char alt_buf[16], spd_buf[12], hdg_buf[6];
        snprintf(alt_buf, sizeof(alt_buf), "%dft",  (int)roundf(fi.alt_ft / 100.0f) * 100);
        snprintf(spd_buf, sizeof(spd_buf), "%dkm/h", (int)roundf(fi.speed_kmh));
        snprintf(hdg_buf, sizeof(hdg_buf), "%s",     heading_label(fi.heading));
        String alt_s = alt_buf, spd_s = spd_buf, hdg_s = hdg_buf;
        int gap = 22;
        int total_w = text_w(alt_s, 2) + gap + text_w(spd_s, 2) + gap + text_w(hdg_s, 2);
        int sx = (SCREEN_W - total_w) / 2;
        draw_text(alt_s, sx, STATS_Y, 2, COLOR_ROW0);  sx += text_w(alt_s, 2) + gap;
        draw_text(spd_s, sx, STATS_Y, 2, COLOR_ROW0);  sx += text_w(spd_s, 2) + gap;
        draw_text(hdg_s, sx, STATS_Y, 2, COLOR_ROW0);
    }

    // Times row: dep_time > arr_time
    if (fi.dep_time > 0 || fi.arr_time > 0) {
        char dep_tbuf[12] = "--:--";
        char arr_tbuf[14] = "--:--";
        if (fi.dep_time > 0) {
            struct tm td; localtime_r(&fi.dep_time, &td);
            snprintf(dep_tbuf, sizeof(dep_tbuf), "%02d:%02d", td.tm_hour, td.tm_min);
        }
        if (fi.arr_time > 0) {
            struct tm ta; localtime_r(&fi.arr_time, &ta);
            if (fi.dep_time > 0) {
                struct tm td2; localtime_r(&fi.dep_time, &td2);
                int delta_days = ta.tm_yday - td2.tm_yday;
                if (delta_days != 0)
                    snprintf(arr_tbuf, sizeof(arr_tbuf), "%02d:%02d+%d",
                             ta.tm_hour, ta.tm_min, delta_days);
                else
                    snprintf(arr_tbuf, sizeof(arr_tbuf), "%02d:%02d", ta.tm_hour, ta.tm_min);
            } else {
                snprintf(arr_tbuf, sizeof(arr_tbuf), "%02d:%02d", ta.tm_hour, ta.tm_min);
            }
        }
        String dep_ts = dep_tbuf, arr_ts = arr_tbuf, arrow = " > ";
        int16_t dw = text_w(dep_ts, 2), arw = text_w(arrow, 2), aw2 = text_w(arr_ts, 2);
        int tx = (SCREEN_W - dw - arw - aw2) / 2;
        draw_text(dep_ts, tx,       TIMES_Y, 2, COLOR_ROW0); tx += dw;
        draw_text(arrow,  tx,       TIMES_Y, 2, COLOR_META); tx += arw;
        draw_text(arr_ts, tx,       TIMES_Y, 2, COLOR_ROW0);
    } else {
        String nd = "No schedule data";
        draw_text(nd, (SCREEN_W - text_w(nd, 2)) / 2, TIMES_Y, 2, COLOR_META);
    }
}

void display_draw_flight(int slot, int flight_count, const FlightInfo& fi, bool wifi_ok) {
    if (!gfx) return;
    apply_night_brightness();

    time_t now_t = time(nullptr);
    struct tm tm_now;
    localtime_r(&now_t, &tm_now);
    int cur_minute = tm_now.tm_hour * 60 + tm_now.tm_min;

    bool slot_changed   = (slot != s_flight_last_slot);
    bool minute_changed = (cur_minute != s_flight_last_minute);
    bool data_changed   = (fi.fetched_at != s_flight_last_fetch);
    if (!slot_changed && !minute_changed && !data_changed) return;

    s_flight_last_slot   = slot;
    s_flight_last_minute = cur_minute;
    s_flight_last_fetch  = fi.fetched_at;

    gfx->fillScreen(BLACK);

    // Header
    {
        char clock_buf[6] = "--:--";
        if (now_t > 1700000000)
            snprintf(clock_buf, sizeof(clock_buf), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
        String clock_str = clock_buf;
        int clock_x = SCREEN_W - PAD - text_w(clock_str, 2);
        int dots_x  = clock_x - 8 - flight_count * 12;
        int dot_y   = (HEADER_H - 6) / 2;
        for (int i = 0; i < flight_count; i++)
            gfx->fillRect(dots_x + i * 12, dot_y, 6, 6, (i == slot) ? COLOR_ROW0 : COLOR_META);
        String cs = fi.callsign.length() > 0 ? fi.callsign : "Flight";
        draw_plane_right(PAD + 7, HEADER_H / 2, COLOR_ROW0);
        draw_text(cs,         PAD + 18, (HEADER_H - 16) / 2, 2, COLOR_ROW0);
        draw_text(clock_str,  clock_x,  (HEADER_H - 16) / 2, 2, COLOR_ROW0);
        gfx->drawFastHLine(0, HEADER_H - 1, SCREEN_W, COLOR_META);
    }

    draw_flight_content(now_t, fi);
    draw_flight_footer(now_t, fi, wifi_ok);
}

// ── Boot animation — runs as a FreeRTOS task so setup() can proceed alongside ──
static volatile bool  s_anim_running      = false;
static TaskHandle_t   s_anim_task_handle  = nullptr;

static const uint16_t TRAIL[] = { COLOR_ROW0, COLOR_ROWS, COLOR_DIM, COLOR_META };

static void anim_task_fn(void*) {
    const int CW   = MATRIX_FONT_W;
    const int CH   = MATRIX_FONT_H;
    const int ASC  = MATRIX_FONT_ASC;
    const int COLS = SCREEN_W / CW;
    const int ROWS = SCREEN_H / CH;

    gfx->setFont(&MatrixCode);
    gfx->setTextSize(1);
    gfx->fillScreen(BLACK);

    int8_t heads[64];
    for (int i = 0; i < COLS && i < 64; i++)
        heads[i] = (int8_t)(-random(ROWS * 2));

    uint32_t last = 0;
    while (s_anim_running) {
        uint32_t now = millis();
        if (now - last < 70) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }
        last = now;

        gfx->setFont(&MatrixCode);
        gfx->setTextSize(1);

        for (int c = 0; c < COLS && c < 64; c++) {
            int x = c * CW;
            for (int t = 0; t < 4; t++) {
                int row = heads[c] - 1 - t;
                if (row >= 0 && row < ROWS) {
                    gfx->setTextColor(TRAIL[t], BLACK);
                    gfx->setCursor(x, row * CH + ASC);
                    gfx->print((char)(33 + random(94)));
                }
            }
            int pe = heads[c] - 6;
            if (pe >= 0 && pe < ROWS)
                gfx->fillRect(x, pe * CH, CW, CH, BLACK);
            if (heads[c] >= 0 && heads[c] < ROWS) {
                gfx->setTextColor(WHITE, BLACK);
                gfx->setCursor(x, heads[c] * CH + ASC);
                gfx->print((char)(33 + random(94)));
            }
            if (++heads[c] > ROWS + 6)
                heads[c] = (int8_t)(-random(ROWS / 2));
        }

    }

    // Wipe with random column order then signal done
    int order[64];
    for (int i = 0; i < COLS && i < 64; i++) order[i] = i;
    for (int i = COLS - 1; i > 0; i--) {
        int j = random(i + 1);
        int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
    }
    for (int i = 0; i < COLS && i < 64; i++) {
        gfx->fillRect(order[i] * CW, 0, CW, SCREEN_H, BLACK);
        vTaskDelay(pdMS_TO_TICKS(8));
    }

    s_anim_task_handle = nullptr;
    vTaskDelete(nullptr);
}

void display_boot_animation_start() {
    if (!gfx) return;
    s_anim_running = true;
    xTaskCreatePinnedToCore(anim_task_fn, "boot_anim", 4096, nullptr,
                            1, &s_anim_task_handle, 1);
}

void display_boot_animation_stop() {
    s_anim_running = false;
    while (s_anim_task_handle != nullptr) vTaskDelay(pdMS_TO_TICKS(10));
}
