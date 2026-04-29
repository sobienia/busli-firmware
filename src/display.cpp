// ─────────────────────────────────────────────────────────────────────────────
// display.cpp — Display rendering using Arduino_GFX
// ─────────────────────────────────────────────────────────────────────────────
// We use Arduino_GFX (not TFT_eSPI) because the T-Display S3 Pro's panel
// has a 49-pixel column offset that TFT_eSPI doesn't expose.
// ─────────────────────────────────────────────────────────────────────────────

#include "display.h"
#include "../include/config.h"
#include <Arduino_GFX_Library.h>

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
// We avoid redrawing sections that haven't changed to eliminate per-second flicker.
static time_t s_last_fetch_time   = -1;  // rows redrawn when this changes
static int    s_last_clock_minute = -1;  // header redrawn when this changes

// ── Backlight PWM ─────────────────────────────────────────────────────────────
#define BL_PWM_CHANNEL    0
#define BL_PWM_FREQ       5000
#define BL_PWM_RESOLUTION 8

// ── Color helper (RGB888 → RGB565) ────────────────────────────────────────────
static uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// ── UTF-8 to ASCII transliteration ───────────────────────────────────────────
// Arduino_GFX's built-in font doesn't include ö, ü, ä etc.
// We convert them to closest ASCII equivalents before drawing.
static String to_ascii(const String& src) {
    String out;
    out.reserve(src.length());
    size_t i = 0;
    while (i < src.length()) {
        uint8_t b = (uint8_t)src[i];
        if (b < 0x80) {                         // plain ASCII
            out += (char)b;
            i++;
        } else if (b == 0xC3 && i + 1 < src.length()) {
            uint8_t b2 = (uint8_t)src[i + 1];
            switch (b2) {
                case 0xA4: out += "ae"; break;  // ä
                case 0xB6: out += "oe"; break;  // ö
                case 0xBC: out += "ue"; break;  // ü
                case 0x84: out += "Ae"; break;  // Ä
                case 0x96: out += "Oe"; break;  // Ö
                case 0x9C: out += "Ue"; break;  // Ü
                case 0x9F: out += "ss"; break;  // ß
                case 0xA9: out += "e";  break;  // é
                case 0xA8: out += "e";  break;  // è
                case 0xAA: out += "e";  break;  // ê
                case 0xA0: out += "a";  break;  // à
                default:   out += "?";  break;
            }
            i += 2;
        } else if (b >= 0xC0 && b < 0xE0) { out += '?'; i += 2; }
        else if (b >= 0xE0 && b < 0xF0)   { out += '-'; i += 3; }
        else                               { out += '?'; i++; }
    }
    return out;
}

// ── Text width helper ─────────────────────────────────────────────────────────
static int16_t text_w(const String& s, uint8_t size) {
    int16_t x1, y1;
    uint16_t w, h;
    gfx->setTextSize(size);
    gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
    return (int16_t)w;
}

// ── Draw text at x,y ─────────────────────────────────────────────────────────
static void draw_text(const String& s, int16_t x, int16_t y,
                      uint8_t size, uint16_t color) {
    gfx->setTextSize(size);
    gfx->setTextColor(color, BLACK);
    gfx->setCursor(x, y);
    gfx->print(s);
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
    s_last_fetch_time   = -1;   // force full redraw on next display_draw_board
    s_last_clock_minute = -1;
    gfx->fillScreen(BLACK);
    String s = to_ascii(message);
    int16_t w = text_w(s, 2);
    draw_text(s, (SCREEN_W - w) / 2, SCREEN_H / 2 - 8, 2, COLOR_ROWS);
}

void display_invalidate() {
    s_last_fetch_time   = -1;
    s_last_clock_minute = -1;
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
                        bool rain_active) {
    gfx->fillRect(0, 0, SCREEN_W, HEADER_H, BLACK);

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
    int16_t clock_w = text_w(clock_str, 2);
    int rx = SCREEN_W - PAD - clock_w;
    int clock_x = rx;

    int umbrella_x = -1;
    if (rain_active) { rx -= 16; umbrella_x = rx; }

    int dots_w  = stop_count * 12;
    int dots_x  = (dots_w > 0) ? (rx -= 8, rx -= dots_w, rx) : rx;

    // Stop label (strip "City, " prefix if too long)
    String label = to_ascii(stop_name);
    int label_max = dots_x - PAD - 8;
    if (text_w(label, 2) > label_max) {
        int comma = label.indexOf(", ");
        if (comma >= 0) label = label.substring(comma + 2);
        while (label.length() > 1 && text_w(label, 2) > label_max)
            label = label.substring(0, label.length() - 1);
    }
    draw_text(label, PAD, 4, 2, COLOR_ROWS);
    draw_text(clock_str, clock_x, 4, 2, COLOR_ROWS);

    if (umbrella_x >= 0)
        draw_umbrella(umbrella_x, 4, COLOR_ROWS);

    for (int i = 0; i < stop_count; i++) {
        uint16_t c = (i == stop_index) ? COLOR_ROWS : COLOR_META;
        gfx->fillRect(dots_x + i * 12, 8, 6, 6, c);
    }

    gfx->drawFastHLine(0, HEADER_H - 1, SCREEN_W, COLOR_META);
}

// ── Rows ──────────────────────────────────────────────────────────────────────
static void draw_rows(const std::vector<Departure>& departures) {
    int rows_top = HEADER_H;
    int rows_h   = SCREEN_H - FOOTER_H - rows_top;
    int row_h    = rows_h / ROW_COUNT;

    gfx->fillRect(0, rows_top, SCREEN_W, rows_h, BLACK);

    if (departures.empty()) {
        String msg = "Loading...";
        draw_text(msg, (SCREEN_W - text_w(msg, 2)) / 2,
                  rows_top + rows_h / 2 - 8, 2, COLOR_META);
        return;
    }

    // Column widths (text size 3 ≈ 18px wide × 24px tall per character)
    const int num_col_end  = PAD + 60;    // line number right edge
    const int dest_start   = num_col_end + 16;
    const int right_margin = 64;          // reserved for time/icon

    int count = std::min((int)departures.size(), ROW_COUNT);
    for (int i = 0; i < count; i++) {
        const Departure& dep = departures[i];
        int y      = rows_top + i * row_h;
        int text_y = y + (row_h - 24) / 2;

        bool disrupted = (dep.delay >= 2);
        uint16_t color = disrupted ? COLOR_DIM
                        : (i == 0  ? COLOR_ROW0 : COLOR_ROWS);

        // Line number — right-aligned in number column
        String ln = to_ascii(dep.line);
        draw_text(ln, num_col_end - text_w(ln, 3), text_y, 3, color);

        // Destination — truncated to fit, leaving room for delay badge + time
        String dest     = to_ascii(dep.destination);
        int dest_max_w  = SCREEN_W - dest_start - right_margin - PAD;
        if (disrupted) dest_max_w -= 52;  // extra room for "+Xm" badge
        while (dest.length() > 1 && text_w(dest, 3) > dest_max_w)
            dest = dest.substring(0, dest.length() - 1);
        draw_text(dest, dest_start, text_y, 3, color);

        // Delay badge (smaller font, placed between dest and time)
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

// ── Footer ────────────────────────────────────────────────────────────────────
static void draw_footer(const char* weather_str, const char* uv_str,
                        bool from_cache, bool wifi_ok, int age_seconds) {
    int fy = SCREEN_H - FOOTER_H;
    gfx->fillRect(0, fy, SCREEN_W, FOOTER_H, BLACK);
    gfx->drawFastHLine(0, fy, SCREEN_W, rgb(20, 12, 0));

    int x = PAD, ty = fy + 4;

    if (weather_str && strlen(weather_str) > 0) {
        String s = to_ascii(weather_str);
        draw_text(s, x, ty, 2, COLOR_ROWS);
        x += text_w(s, 2) + 16;
    }
    if (uv_str && strlen(uv_str) > 0) {
        String s = to_ascii(uv_str);
        draw_text(s, x, ty, 2, COLOR_ROWS);
        x += text_w(s, 2) + 16;
    }

    // Refresh age — right-aligned
    char age_buf[16];
    if (from_cache)       snprintf(age_buf, sizeof(age_buf), "cached");
    else if (age_seconds < 60) snprintf(age_buf, sizeof(age_buf), "now");
    else                  snprintf(age_buf, sizeof(age_buf), "%dm", age_seconds / 60);
    String as = age_buf;
    draw_text(as, SCREEN_W - PAD - 14 - text_w(as, 2), ty, 2, COLOR_ROWS);

    // Status dot
    uint16_t dot;
    if (!wifi_ok) dot = ((millis() / 500) % 2 == 0) ? rgb(180, 30, 0) : BLACK;
    else if (from_cache) dot = COLOR_DIM;
    else                 dot = COLOR_META;
    gfx->fillRect(SCREEN_W - PAD - 6, fy + 8, 6, 6, dot);
}

// ── Partial board redraw (called every second from main loop) ─────────────────
// Header redraws only on minute changes (clock digit flip).
// Rows redraw only when fetch_time advances (new API data arrived).
// Footer redraws every call — it's 22 px and shows the live age counter.
void display_draw_board(
    const char* stop_name, int stop_index, int stop_count,
    const std::vector<Departure>& departures,
    const char* weather_str, const char* uv_str,
    bool from_cache, bool wifi_ok, int age_seconds,
    time_t fetch_time)
{
    if (!gfx) return;
    apply_night_brightness();

    time_t now_t = time(nullptr);
    struct tm tm_now;
    localtime_r(&now_t, &tm_now);
    int cur_minute = tm_now.tm_hour * 60 + tm_now.tm_min;

    if (cur_minute != s_last_clock_minute) {
        s_last_clock_minute = cur_minute;
        draw_header(stop_name, stop_index, stop_count, false);
    }

    if (fetch_time != s_last_fetch_time) {
        s_last_fetch_time = fetch_time;
        draw_rows(departures);
    }

    draw_footer(weather_str, uv_str, from_cache, wifi_ok, age_seconds);
}

// ── Boot animation ────────────────────────────────────────────────────────────
void display_boot_animation(uint16_t duration_ms) {
    if (!gfx) return;

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
