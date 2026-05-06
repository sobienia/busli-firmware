// ─────────────────────────────────────────────────────────────────────────────
// display.cpp — Display rendering using Arduino_GFX
// ─────────────────────────────────────────────────────────────────────────────
// We use Arduino_GFX (not TFT_eSPI) because the T-Display S3 Pro's panel
// has a 49-pixel column offset that TFT_eSPI doesn't expose.
// ─────────────────────────────────────────────────────────────────────────────

#include "display.h"
#include "../include/config.h"
#include "../include/tramli_fonts.h"
#include <Arduino_GFX_Library.h>

static const int SMALL_ASCENT = 15;   // TramliSmall 16px
static const int LARGE_ASCENT = 22;   // TramliLarge 24px

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
    if (size == 2) gfx->setFont(&TramliSmall);
    else           gfx->setFont(&TramliLarge);
    gfx->setTextSize(1);
    int16_t x1, y1; uint16_t w, h;
    gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
    return (int16_t)w;
}

// ── Draw text at x,y (y = top of the character cell) ─────────────────────────
static void draw_text(const String& s, int16_t x, int16_t y,
                      uint8_t size, uint16_t color) {
    int asc;
    if (size == 2) { gfx->setFont(&TramliSmall); asc = SMALL_ASCENT; }
    else           { gfx->setFont(&TramliLarge);  asc = LARGE_ASCENT; }
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
    int16_t w = text_w(s, 2);
    draw_text(s, (SCREEN_W - w) / 2, SCREEN_H / 2 - 8, 2, COLOR_ROW0);
}

void display_invalidate() {
    s_last_fetch_time     = -1;
    s_last_clock_minute   = -1;
    s_footer_static_drawn = false;
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

    // Column widths (size-3 font: each char = 18 px wide, bold)
    const int num_col_end  = PAD + 54;    // line number right edge  (= 62)
    const int dest_start   = num_col_end + 14;  //                   (= 76)
    const int right_margin = 64;          // reserved for time/icon

    int count = std::min((int)departures.size(), max_rows);
    for (int i = 0; i < count; i++) {
        const Departure& dep = departures[i];
        int y      = rows_top + i * row_h;
        int text_y = y + (row_h - 28) / 2;

        bool disrupted = (dep.delay >= 2);
        uint16_t color = disrupted ? COLOR_DIM : COLOR_ROW0;

        // Line number — right-aligned in number column
        String ln = to_latin1(dep.line);
        draw_text(ln, num_col_end - text_w(ln, 3), text_y, 3, color);

        // Destination — truncated to fit, leaving room for delay badge + time
        String dest    = to_latin1(dep.destination);
        int dest_max_w = SCREEN_W - dest_start - right_margin - PAD;
        if (disrupted) dest_max_w -= 52;
        while (dest.length() > 1 && text_w(dest, 3) > dest_max_w)
            dest = dest.substring(0, dest.length() - 1);
        draw_text(dest, dest_start, text_y, 3, color);

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
            draw_text(ts, SCREEN_W - PAD - text_w(ts, 3), text_y, 3, color);
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
                                int age_seconds, bool large_font) {
    gfx->fillRect(SCREEN_W - FOOTER_AGE_REGION_W, fy + 1,
                  FOOTER_AGE_REGION_W, FOOTER_H - 1, BLACK);

    uint8_t fsz = large_font ? 3 : 2;
    int     fh  = large_font ? 28 : 16;
    int     ty  = fy + (FOOTER_H - fh) / 2;
    int     uby = fy + (FOOTER_H - 15) / 2;

    char age_buf[16];
    if (from_cache)            snprintf(age_buf, sizeof(age_buf), "cached");
    else if (age_seconds < 60) snprintf(age_buf, sizeof(age_buf), "now");
    else                       snprintf(age_buf, sizeof(age_buf), "%dm", age_seconds / 60);
    String as = age_buf;
    draw_text(as, SCREEN_W - PAD - 14 - text_w(as, fsz), ty, fsz, COLOR_ROW0);

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
    time_t fetch_time, bool large_font_mode)
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
    draw_footer_dynamic(fy, from_cache, wifi_ok, age_seconds, large_font_mode);
}

// ── Boot animation ────────────────────────────────────────────────────────────
void display_boot_animation(uint16_t duration_ms) {
    if (!gfx) return;
    gfx->setFont((GFXfont*)nullptr);   // built-in font for the rain effect

    const int CW   = 12;            // char width at textSize 2
    const int CH   = 16;            // char height at textSize 2
    const int COLS = SCREEN_W / CW; // ~40 columns
    const int ROWS = SCREEN_H / CH; // ~13 rows

    gfx->fillScreen(BLACK);
    gfx->setTextSize(2);

    // Stagger column starts so they don't all fall together
    int8_t heads[64];
    for (int i = 0; i < COLS && i < 64; i++)
        heads[i] = (int8_t)(-random(ROWS * 2));

    uint32_t start = millis(), last = 0;
    while (millis() - start < duration_ms) {
        if (millis() - last < 80) { delay(5); continue; }
        last = millis();

        for (int c = 0; c < COLS && c < 64; c++) {
            int x = c * CW;

            // Fade old head to medium color
            int p1 = heads[c] - 1;
            if (p1 >= 0 && p1 < ROWS) {
                gfx->setTextColor(COLOR_ROWS, BLACK);
                gfx->setCursor(x, p1 * CH);
                gfx->print((char)(33 + random(94)));
            }
            // Older to dim
            int p3 = heads[c] - 3;
            if (p3 >= 0 && p3 < ROWS) {
                gfx->setTextColor(COLOR_DIM, BLACK);
                gfx->setCursor(x, p3 * CH);
                gfx->print((char)(33 + random(94)));
            }
            // Erase tail end
            int pe = heads[c] - 5;
            if (pe >= 0 && pe < ROWS)
                gfx->fillRect(x, pe * CH, CW, CH, BLACK);

            // Bright head
            if (heads[c] >= 0 && heads[c] < ROWS) {
                gfx->setTextColor(WHITE, BLACK);
                gfx->setCursor(x, heads[c] * CH);
                gfx->print((char)(33 + random(94)));
            }

            if (++heads[c] > ROWS + 5)
                heads[c] = (int8_t)(-random(ROWS / 2));
        }
    }

    // Wipe screen with random column order
    int order[64];
    for (int i = 0; i < COLS && i < 64; i++) order[i] = i;
    for (int i = COLS - 1; i > 0; i--) {
        int j = random(i + 1);
        int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
    }
    for (int i = 0; i < COLS && i < 64; i++) {
        gfx->fillRect(order[i] * CW, 0, CW, SCREEN_H, BLACK);
        delay(8);
    }
}
