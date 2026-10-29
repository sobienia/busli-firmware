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
#include <Preferences.h>
#include <qrcode.h>   // ESP-IDF QR code generator (esp_qrcode_*)

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

// ── Runtime color palette — set by display_set_theme(), defaults to Zürich Amber ──
static uint16_t c_row0 = 0xF480;  // 100% — main text
static uint16_t c_rows = 0xC3A0;  //  80% — boot animation trail
static uint16_t c_dim  = 0x7A40;  //  50% — delays / stale
static uint16_t c_meta = 0x4960;  //  30% — separators / inactive dots
static uint16_t TRAIL[4] = { 0xF480, 0xC3A0, 0x7A40, 0x4960 };

// ── Partial-redraw state ──────────────────────────────────────────────────────
// Header redraws on minute change, rows on new fetch data, footer is split:
//   static region (weather/UV) redraws on weather change (~15 min)
//   dynamic region (age + dot) redraws every second
static time_t    s_last_fetch_time      = -1;
static uint32_t  s_last_rows_sig        = 0;    // fingerprint of the rows last drawn
static int       s_last_clock_minute    = -1;
static int       s_last_battery_pct     = -2;   // -2 = never drawn; triggers first paint
static bool      s_last_battery_charging = false;
static uint32_t  s_last_weather_hash    = 0xFFFFFFFF;
static bool      s_footer_static_drawn  = false;
// Flight screen partial-redraw state
static int    s_flight_last_slot   = -1;
static int    s_flight_last_minute = -1;
static time_t s_flight_last_fetch  = -1;
// Commute screen partial-redraw state
static time_t    s_commute_last_fetch  = -1;
static int       s_commute_last_minute = -1;
static int       s_commute_last_conn   = -2;
static uint32_t  s_commute_scroll_ms   = 0;   // millis() when current connection started (for marquee)
static uint32_t  s_commute_weather_hash = 0xFFFFFFFF;
// Stocks screen partial-redraw state — one entry per on-screen cell (max 10)
struct StockCellState { int8_t status; float price; float pct; };  // status -1 = needs paint
static StockCellState s_stock_cells[10];
static int       s_stocks_last_first    = -1;  // -1 forces a full repaint
static int       s_stocks_last_per_page = -1;
static int       s_stocks_last_minute   = -1;
static uint32_t  s_stocks_weather_hash  = 0xFFFFFFFF;
// Stock detail view: signature of what's drawn (-1 = full repaint needed)
static int       s_detail_idx_sig       = -1;
static int       s_detail_minute        = -1;
static uint32_t  s_detail_weather       = 0xFFFFFFFF;
// Weather page: fetch time of the data drawn (-1 = full repaint needed)
static time_t    s_wx_drawn_fetch       = -1;
static int       s_wx_minute            = -1;
// Info page / night clock: fingerprint of what's drawn (0 = repaint)
static uint32_t  s_info_sig             = 0;
static int       s_clock_minute         = -1;

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
        } else if (b == 0xC2 && i + 1 < src.length()) {
            // U+0080–U+00BF map 1:1 to Latin-1: ° ² ³ µ « » ±, non-breaking space …
            out += (char)(uint8_t)src[i + 1];
            i += 2;
        } else if (b >= 0xC0 && b < 0xE0) { out += '?'; i += 2; }
        else if (b >= 0xE0 && b < 0xF0)   { out += '?'; i += 3; }
        else                               { out += '?'; i++; }
    }
    return out;
}

// ── Station name trimmer — strips "City, " prefix ────────────────────────────
// "Zürich, ETH Hönggerberg" → "ETH Hönggerberg"
static String trim_city(const String& s) {
    int i = s.indexOf(", ");
    return (i >= 0) ? s.substring(i + 2) : s;
}

// ── Per-character xAdvance (PROGMEM safe, no heap) ───────────────────────────
static int glyph_xadvance(const GFXfont* font, char c) {
    uint8_t first = pgm_read_byte(&font->first);
    uint8_t last  = pgm_read_byte(&font->last);
    if ((uint8_t)c < first || (uint8_t)c > last) return 0;
    GFXglyph* g = &((GFXglyph*)pgm_read_ptr(&font->glyph))[(uint8_t)c - first];
    return (int)pgm_read_byte(&g->xAdvance);
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

// ── Marquee-safe text draw — clips to [x_min, x_max) ────────────────────────
// Drawing outside [0, SCREEN_W) corrupts the ST7796 address counter and deposits
// pixels at wrong screen positions. This function skips leading characters until
// the cursor is at x_min+1 (the +1 guards against glyphs with xOffset=-1 writing
// one pixel before the cursor) and stops before any character whose advance would
// push the cursor past x_max.  Defaults clip to the full screen width.
static void draw_text_safe(const String& s, int x_start, int text_y,
                            uint8_t fsz, uint16_t color,
                            int x_min = 0, int x_max = SCREEN_W) {
    if (s.length() == 0 || x_start >= x_max) return;
    const GFXfont* font = (fsz == 2) ? &TramliSmall
                        : (fsz == 4) ? &TramliXLarge
                        :               &TramliLarge;
    // Left-clip: advance past chars whose cursor would start before x_min.
    // No +1 guard here — Arduino_GFX clips individual pixels against its own
    // [0, width) window, so a single glyph pixel at x=-1 is silently dropped
    // by the library rather than corrupting the display controller address state.
    int cum = 0, ci = 0;
    while (ci < (int)s.length() && x_start + cum < x_min) {
        cum += glyph_xadvance(font, s[ci]);
        ci++;
    }
    if (ci >= (int)s.length() || x_start + cum >= x_max) return;
    // Right-clip: include a character only if its full advance fits within x_max.
    int ci_end = ci, cum_end = cum;
    while (ci_end < (int)s.length()) {
        int xa = glyph_xadvance(font, s[ci_end]);
        if (x_start + cum_end + xa > x_max) break;
        cum_end += xa;
        ci_end++;
    }
    if (ci_end <= ci) return;
    draw_text(s.substring(ci, ci_end), x_start + cum, text_y, fsz, color);
}

// ── Weather hash for footer static redraw guard ───────────────────────────────
static uint32_t weather_hash(const char* weather_str, const char* uv_str,
                              bool rain_today, int rain_pct,
                              bool snow_today, bool clear_today) {
    uint32_t h = (rain_today  ? 0x001u : 0u)
               ^ (snow_today  ? 0x002u : 0u)
               ^ (clear_today ? 0x004u : 0u)
               ^ ((uint32_t)(rain_pct & 0xFF) << 4);
    for (const char* p = weather_str; p && *p; p++) h ^= (uint32_t)*p << 8;
    for (const char* p = uv_str;      p && *p; p++) h ^= (uint32_t)*p << 16;
    return h;
}

// ── Off-screen rendering (flicker-free redraws) ──────────────────────────────
// Full-screen canvas in PSRAM (480×222×2 = 213 KB), created on first use. While a
// FrameBuffered object is alive, every draw helper (they all go through `gfx`)
// renders into the canvas; when it goes out of scope the band [y0, y0+h) is sent
// to the panel in one transfer — no visible clear-then-draw. Everything drawn
// inside the band must be fully repainted by the caller (the canvas holds stale
// pixels from other screens). Falls back to drawing directly if PSRAM runs out.
static Arduino_Canvas* s_canvas = nullptr;

class FrameBuffered {
public:
    explicit FrameBuffered(int y0 = 0, int h = SCREEN_H) : _y0(y0), _h(h) {
        if (!s_canvas) {
            s_canvas = new Arduino_Canvas(SCREEN_W, SCREEN_H, gfx);
            if (!s_canvas->begin(GFX_SKIP_OUTPUT_BEGIN)) { delete s_canvas; s_canvas = nullptr; }
        }
        if (s_canvas) { _screen = gfx; gfx = s_canvas; }
    }
    ~FrameBuffered() {
        if (!_screen) return;
        gfx = _screen;
        // Canvas rotation 0 → framebuffer rows are SCREEN_W pixels, so a band is contiguous
        gfx->draw16bitRGBBitmap(0, _y0, s_canvas->getFramebuffer() + _y0 * SCREEN_W, SCREEN_W, _h);
    }
private:
    Arduino_GFX* _screen = nullptr;
    int _y0, _h;
};

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
        49, 0,         // col_offset1, row_offset1 (rotation 0 and 3)
        49, 0          // col_offset2, row_offset2 (rotation 1 and 2)
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
    display_invalidate();   // screen is wiped — every view must fully repaint afterwards
    gfx->fillScreen(BLACK);

    // Split on '\n' and center each line independently.
    String full = to_latin1(message);
    const int LINE_H = 30;  // TramliLarge yAdvance (28) + 2px spacing
    int n_lines = 1;
    for (int i = 0; i < (int)full.length(); i++) if (full[i] == '\n') n_lines++;
    int y = (SCREEN_H - n_lines * LINE_H) / 2;
    int start = 0;
    for (int i = 0; i <= (int)full.length(); i++) {
        if (i == (int)full.length() || full[i] == '\n') {
            String line = full.substring(start, i);
            draw_text(line, (SCREEN_W - text_w(line, 3)) / 2, y, 3, c_row0);
            y += LINE_H;
            start = i + 1;
        }
    }
}

void display_show_ota_prompt(const char* cur_ver, const char* new_ver) {
    if (!gfx) return;
    gfx->fillScreen(BLACK);

    // Title — two lines to fit: "New firmware available." / "Update?"
    String title1 = "New firmware available.";
    String title2 = "Update?";
    draw_text(title1, (SCREEN_W - text_w(title1, 2)) / 2, 14, 2, c_row0);
    draw_text(title2, (SCREEN_W - text_w(title2, 3)) / 2, 34, 3, c_row0);

    // Version line  e.g. "v1.2.0 -> v1.3.1"
    String ver_line = String("v") + cur_ver + " -> v" + new_ver;
    draw_text(ver_line, (SCREEN_W - text_w(ver_line, 2)) / 2, 72, 2, c_dim);

    // Separator
    gfx->drawFastHLine(PAD, 120, SCREEN_W - PAD * 2, c_meta);

    // No button — left half
    const int BTN_Y = 135, BTN_H = 52;
    const int BTN_MARGIN = 16;
    int btn_w = SCREEN_W / 2 - BTN_MARGIN * 2;
    gfx->drawRect(BTN_MARGIN, BTN_Y, btn_w, BTN_H, c_meta);
    String lbl_no = "No";
    draw_text(lbl_no, BTN_MARGIN + (btn_w - text_w(lbl_no, 3)) / 2,
              BTN_Y + (BTN_H - 28) / 2, 3, c_dim);

    // Yes button — right half
    int btn_x2 = SCREEN_W / 2 + BTN_MARGIN;
    gfx->drawRect(btn_x2, BTN_Y, btn_w, BTN_H, c_row0);
    String lbl_yes = "Yes";
    draw_text(lbl_yes, btn_x2 + (btn_w - text_w(lbl_yes, 3)) / 2,
              BTN_Y + (BTN_H - 28) / 2, 3, c_row0);
}

void display_invalidate() {
    s_last_fetch_time       = -1;
    s_last_clock_minute     = -1;
    s_last_battery_pct      = -2;
    s_last_battery_charging = false;
    s_footer_static_drawn   = false;
    s_flight_last_slot      = -1;
    s_flight_last_minute    = -1;
    s_flight_last_fetch     = -1;
    s_commute_last_fetch    = -1;
    s_commute_last_minute   = -1;
    s_commute_last_conn     = -2;
    s_commute_scroll_ms     = 0;
    s_commute_weather_hash  = 0xFFFFFFFF;
    s_stocks_last_first     = -1;
    s_detail_idx_sig        = -1;
    s_wx_drawn_fetch        = -1;
    s_info_sig              = 0;
    s_clock_minute          = -1;
}

// ── Umbrella icon (12×15) ────────────────────────────────────────────────────
static void draw_umbrella(int x, int y, uint16_t color) {
    gfx->fillRect(x + 5, y,      2, 2, color);  // tip
    gfx->fillRect(x + 3, y + 2,  6, 2, color);  // canopy top
    gfx->fillRect(x + 1, y + 4, 10, 2, color);  // canopy mid
    gfx->fillRect(x,     y + 6, 12, 2, color);  // canopy wide
    gfx->fillRect(x + 5, y + 8,  2, 5, color);  // handle
    gfx->fillRect(x + 2, y + 13, 4, 2, color);  // crook
}

// ── Sun icon (11×11) ─────────────────────────────────────────────────────────
static void draw_sun(int x, int y, uint16_t color) {
    gfx->fillRect(x + 3, y + 3, 5, 5, color);   // core disc
    gfx->fillRect(x + 4, y,     3, 2, color);   // N ray
    gfx->fillRect(x + 4, y + 9, 3, 2, color);   // S ray
    gfx->fillRect(x,     y + 4, 2, 3, color);   // W ray
    gfx->fillRect(x + 9, y + 4, 2, 3, color);   // E ray
    gfx->fillRect(x + 1, y + 1, 2, 2, color);   // NW diagonal
    gfx->fillRect(x + 8, y + 1, 2, 2, color);   // NE diagonal
    gfx->fillRect(x + 1, y + 8, 2, 2, color);   // SW diagonal
    gfx->fillRect(x + 8, y + 8, 2, 2, color);   // SE diagonal
}

// ── Snowflake icon (9×9) ─────────────────────────────────────────────────────
static void draw_snowflake(int x, int y, uint16_t color) {
    gfx->drawFastHLine(x,   y + 4, 9, color);   // horizontal arm
    gfx->drawFastVLine(x + 4, y,   9, color);   // vertical arm
    // Short ticks on arms (lattice branches)
    gfx->fillRect(x + 2, y + 3, 1, 1, color);
    gfx->fillRect(x + 2, y + 5, 1, 1, color);
    gfx->fillRect(x + 6, y + 3, 1, 1, color);
    gfx->fillRect(x + 6, y + 5, 1, 1, color);
    gfx->fillRect(x + 3, y + 2, 1, 1, color);
    gfx->fillRect(x + 5, y + 2, 1, 1, color);
    gfx->fillRect(x + 3, y + 6, 1, 1, color);
    gfx->fillRect(x + 5, y + 6, 1, 1, color);
    // Corner diagonal pixels
    gfx->fillRect(x + 1, y + 1, 1, 1, color);
    gfx->fillRect(x + 7, y + 1, 1, 1, color);
    gfx->fillRect(x + 1, y + 7, 1, 1, color);
    gfx->fillRect(x + 7, y + 7, 1, 1, color);
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

// ── Day / night brightness ───────────────────────────────────────────────────
// Day and night each have their own level, cycled by the brightness button and
// saved separately — adjusting at night no longer changes the daytime level.
static const uint8_t DAY_LEVELS[]   = { 100, 80, 60, 40, 20 };
static const uint8_t NIGHT_LEVELS[] = { 30, 20, 15, 10, 5 };
static uint8_t s_day_brightness    = DAY_BRIGHTNESS;
static uint8_t s_night_brightness  = NIGHT_BRIGHTNESS;
static bool    s_last_night_state  = false;  // last known night/day, to detect transitions
static bool    s_screen_on         = true;
static int     s_night_start_min   = NIGHT_START_HOUR * 60;  // minutes after midnight
static int     s_night_end_min     = NIGHT_END_HOUR * 60;

bool display_is_night() {
    time_t now = time(nullptr);
    if (now < 1700000000) return false;
    struct tm t; localtime_r(&now, &t);
    int m = t.tm_hour * 60 + t.tm_min;
    if (s_night_start_min == s_night_end_min) return false;  // night mode disabled
    return (s_night_start_min > s_night_end_min)
        ? (m >= s_night_start_min || m < s_night_end_min)
        : (m >= s_night_start_min && m < s_night_end_min);
}

void display_set_night_schedule(int start_min, int end_min) {
    s_night_start_min  = start_min;
    s_night_end_min    = end_min;
    s_last_night_state = !display_is_night();  // force re-apply on next draw
}

// Re-apply the level for the current day/night state (after a pulse, wake, …)
void display_restore_brightness() {
    if (!s_screen_on) return;
    display_set_brightness(display_is_night() ? s_night_brightness : s_day_brightness);
}

// Called once per second from every draw function. Switches level only at the
// day/night transition so manual adjustments stick until the next transition.
static void apply_night_brightness() {
    bool night = display_is_night();
    if (night != s_last_night_state) {
        s_last_night_state = night;
        display_restore_brightness();
        Serial.printf("[Display] Auto brightness: %s mode\n", night ? "night" : "day");
    }
}

void display_init_brightness() {
    Preferences prefs;
    prefs.begin("tramli", true);
    int d = prefs.getInt("bright_day",   DAY_BRIGHTNESS);
    int n = prefs.getInt("bright_night", NIGHT_BRIGHTNESS);
    prefs.end();
    if (d >= 5 && d <= 100) s_day_brightness   = (uint8_t)d;
    if (n >= 5 && n <= 100) s_night_brightness = (uint8_t)n;
    s_last_night_state = display_is_night();
    display_restore_brightness();
    Serial.printf("[Display] Brightness loaded: day %d%%, night %d%% (%s)\n",
                  s_day_brightness, s_night_brightness, s_last_night_state ? "night" : "day");
}

// Next level in the cycle (wraps); unknown levels jump to the first entry
static uint8_t next_level(const uint8_t* levels, int n, uint8_t cur) {
    for (int i = 0; i < n; i++)
        if (levels[i] == cur) return levels[(i + 1) % n];
    return levels[0];
}

void display_step_brightness() {
    bool night = display_is_night();
    uint8_t& level = night ? s_night_brightness : s_day_brightness;
    level = night ? next_level(NIGHT_LEVELS, sizeof(NIGHT_LEVELS), level)
                  : next_level(DAY_LEVELS,   sizeof(DAY_LEVELS),   level);
    display_set_brightness(level);
    Preferences prefs;
    prefs.begin("tramli", false);
    prefs.putInt(night ? "bright_night" : "bright_day", level);
    prefs.end();
    Serial.printf("[Display] %s brightness → %d%%\n", night ? "Night" : "Day", level);
}

void display_set_screen_on(bool on) {
    if (on == s_screen_on || !gfx) return;
    s_screen_on = on;
    if (on) {
        gfx->displayOn();
        display_invalidate();        // panel content isn't trusted after sleep — repaint
        display_restore_brightness();
    } else {
        display_set_brightness(0);
        gfx->displayOff();           // panel sleep: backlight off alone still powers the driver
    }
    Serial.printf("[Display] Screen %s\n", on ? "on" : "off");
}

bool display_screen_on() { return s_screen_on; }

// Estimated battery runtime shown in the header when low (-1 = unknown)
static float s_battery_hours = -1;

void display_set_battery_estimate(float hours_left) { s_battery_hours = hours_left; }

// ── Header ────────────────────────────────────────────────────────────────────
static void draw_header(const char* stop_name, int stop_index, int stop_count,
                        bool rain_active, bool large_font, int battery_pct, bool battery_charging) {
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

    // Right cluster (right-to-left): clock | umbrella | dots
    int16_t clock_w = text_w(clock_str, fsz);
    int rx = SCREEN_W - PAD - clock_w;
    int clock_x = rx;

    int umbrella_x = -1;
    if (rain_active) { rx -= 16; umbrella_x = rx; }

    int dots_w = stop_count * 12;
    int dots_x = (dots_w > 0) ? (rx -= 8, rx -= dots_w, rx) : rx;

    // Left cluster: battery icon (22px) + % label + 6px gap + stop name.
    // When low and unplugged, the label adds the estimated time left: "18% ~2h".
    char pct_buf[16] = "";
    String pct_str;
    int label_x = PAD;
    if (battery_pct >= 0) {
        if (battery_charging)
            snprintf(pct_buf, sizeof(pct_buf), "%d%%+", battery_pct);
        else if (battery_pct <= BATTERY_SHOW_ESTIMATE_PCT && s_battery_hours >= 0)
            snprintf(pct_buf, sizeof(pct_buf), s_battery_hours < 1 ? "%d%% <1h" : "%d%% ~%.0fh",
                     battery_pct, s_battery_hours);
        else
            snprintf(pct_buf, sizeof(pct_buf), "%d%%", battery_pct);
        pct_str  = pct_buf;
        label_x  = PAD + 22 + 2 + text_w(pct_str, 2) + 6;
    }

    // Stop label — truncate if too wide
    String label = to_latin1(stop_name);
    int label_max = dots_x - label_x - 8;
    if (text_w(label, fsz) > label_max) {
        int comma = label.indexOf(", ");
        if (comma >= 0) label = label.substring(comma + 2);
        while (label.length() > 1 && text_w(label, fsz) > label_max)
            label = label.substring(0, label.length() - 1);
    }

    // Battery icon at top-left (before stop label)
    if (battery_pct >= 0) {
        int by = (HEADER_H - 10) / 2;
        uint16_t bat_col = (battery_pct <= 20) ? c_dim : c_row0;
        gfx->drawRect(PAD, by, 20, 10, bat_col);
        gfx->fillRect(PAD + 20, by + 3, 2, 4, bat_col);
        int fill_w = battery_pct * 18 / 100;
        if (fill_w > 0)
            gfx->fillRect(PAD + 1, by + 1, fill_w, 8, bat_col);
        draw_text(pct_str, PAD + 22 + 2, (HEADER_H - 16) / 2, 2, bat_col);
    }

    draw_text(label, label_x, ty, fsz, c_row0);
    draw_text(clock_str, clock_x, ty, fsz, c_row0);

    if (umbrella_x >= 0)
        draw_umbrella(umbrella_x, ty, c_row0);

    // Stop indicator dots — center them vertically in header
    int dot_y = (HEADER_H - 6) / 2;
    for (int i = 0; i < stop_count; i++) {
        uint16_t c = (i == stop_index) ? c_row0 : c_meta;
        gfx->fillRect(dots_x + i * 12, dot_y, 6, 6, c);
    }

    gfx->drawFastHLine(0, HEADER_H - 1, SCREEN_W, c_meta);
}

// ── Rows ──────────────────────────────────────────────────────────────────────
// A departure that left more than this long ago is no longer shown
static const int DEPARTED_GRACE_SEC = 30;

// Minutes until departure as shown on the board (0 = bus icon), or -1 if gone.
static int departure_minutes(const Departure& d, time_t now) {
    long secs = (long)(d.dep_time - now);
    if (secs < -DEPARTED_GRACE_SEC) return -1;
    int mins = (secs > 0) ? (int)(secs / 60) : 0;
    return (mins > 99) ? 99 : mins;
}

// Departures still to come, in order — what the board actually shows.
static std::vector<Departure> upcoming(const std::vector<Departure>& all, time_t now) {
    std::vector<Departure> out;
    for (const auto& d : all)
        if (departure_minutes(d, now) >= 0) out.push_back(d);
    return out;
}

// Fingerprint of what the rows would display: redraw only when it changes.
static uint32_t rows_signature(const std::vector<Departure>& shown, int max_rows, time_t now) {
    uint32_t h = 2166136261u;
    int n = std::min((int)shown.size(), max_rows);
    for (int i = 0; i < n; i++) {
        h = (h ^ (uint32_t)departure_minutes(shown[i], now)) * 16777619u;
        h = (h ^ (uint32_t)shown[i].delay) * 16777619u;
        h = (h ^ (uint32_t)shown[i].platform_changed) * 16777619u;
        for (char c : shown[i].platform)    h = (h ^ (uint8_t)c) * 16777619u;
        for (char c : shown[i].line)        h = (h ^ (uint8_t)c) * 16777619u;
        for (char c : shown[i].destination) h = (h ^ (uint8_t)c) * 16777619u;
    }
    return h ^ (uint32_t)n;
}

static void draw_rows(const std::vector<Departure>& departures, time_t fetch_time,
                      bool large_font, time_t now) {
    int rows_top = HEADER_H;
    int rows_h   = SCREEN_H - FOOTER_H - rows_top;
    int max_rows = large_font ? 2 : ROW_COUNT;
    int row_h    = rows_h / max_rows;

    gfx->fillRect(0, rows_top, SCREEN_W, rows_h, BLACK);

    if (departures.empty()) {
        String msg = (fetch_time > 0) ? "No more departures" : "Loading...";
        uint8_t fsz = large_font ? 3 : 2;
        draw_text(msg, (SCREEN_W - text_w(msg, fsz)) / 2,
                  rows_top + rows_h / 2 - 14, fsz, c_meta);
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

        // Line number — right-aligned in number column
        String ln = to_latin1(dep.line);
        draw_text(ln, num_col_end - text_w(ln, row_fsz), text_y, row_fsz, c_row0);

        // Right-hand extras, laid out right to left before the time column:
        //   [destination …] [Pl. 3] [+4m] [7']
        int extras_x = SCREEN_W - PAD - right_margin;   // left edge of the time column

        // Delay badge
        if (disrupted && dep.delay > 0) {
            char badge[8];
            snprintf(badge, sizeof(badge), "+%dm", dep.delay);
            String bs = badge;
            extras_x -= text_w(bs, 2) + 4;
            draw_text(bs, extras_x, text_y + 4, 2, c_row0);
            extras_x -= 6;
        }

        // Platform — small, dim; inverted (filled box) when it differs from the timetable
        if (dep.platform.length() > 0) {
            String ps = "Pl. " + to_latin1(dep.platform);
            int pw = text_w(ps, 2);
            extras_x -= pw + 6;
            if (dep.platform_changed) {
                gfx->fillRect(extras_x - 3, text_y + 3, pw + 6, 19, c_row0);
                draw_text(ps, extras_x, text_y + 4, 2, BLACK);
            } else {
                draw_text(ps, extras_x, text_y + 4, 2, c_dim);
            }
            extras_x -= 8;
        }

        // Destination — truncated to whatever room is left
        String dest    = to_latin1(dep.destination);
        int dest_max_w = extras_x - dest_start - 4;
        while (dest.length() > 1 && text_w(dest, row_fsz) > dest_max_w)
            dest = dest.substring(0, dest.length() - 1);
        draw_text(dest, dest_start, text_y, row_fsz, c_row0);

        // Time or bus icon
        int mins = departure_minutes(dep, now);
        if (mins <= 0) {
            draw_bus_icon(SCREEN_W - PAD - 32, text_y - 2, c_row0);
        } else {
            char tbuf[8];
            snprintf(tbuf, sizeof(tbuf), "%d'", mins);
            String ts = tbuf;
            draw_text(ts, SCREEN_W - PAD - text_w(ts, row_fsz), text_y, row_fsz, c_row0);
        }

        // Row separator
        if (i < count - 1)
            gfx->drawFastHLine(0, y + row_h - 1, SCREEN_W, c_meta);
    }
}

// ── Footer — static region (weather, UV, umbrella, sun/snowflake) ────────────
// Erases + redraws only the left portion (SCREEN_W - FOOTER_AGE_REGION_W wide).
// Called only when weather data changes or after a full-screen clear.
static void draw_footer_static(int fy, const char* weather_str, const char* uv_str,
                                bool rain_today, int rain_pct,
                                bool snow_today, bool clear_today,
                                bool large_font) {
    gfx->fillRect(0, fy, SCREEN_W - FOOTER_AGE_REGION_W, FOOTER_H, BLACK);
    gfx->drawFastHLine(0, fy, SCREEN_W, c_meta);  // full-width separator

    uint8_t fsz = large_font ? 3 : 2;
    int     fh  = large_font ? 28 : 16;
    int     ty  = fy + (FOOTER_H - fh) / 2;
    int     icy = fy + (FOOTER_H - 11) / 2;  // icon vertical center (11px icons)
    int     uby = fy + (FOOTER_H - 15) / 2;  // umbrella vertical center (15px)
    int     x   = PAD;

    // Condition icon: sun (clear) or snowflake (snowy), left of temperature
    if (clear_today && !snow_today && !rain_today) {
        draw_sun(x, icy, c_row0);
        x += 13;
    } else if (snow_today) {
        draw_snowflake(x, icy + 1, c_row0);  // +1 to center the 9px icon
        x += 11;
    }

    // Text never runs into the dynamic region (age + Wi-Fi dot) on the right
    const int x_max = SCREEN_W - FOOTER_AGE_REGION_W - 6;

    if (weather_str && strlen(weather_str) > 0) {
        String s = to_latin1(weather_str);
        draw_text_safe(s, x, ty, fsz, c_row0, 0, x_max);
        x += text_w(s, fsz) + 14;
    }

    // Precipitation: umbrella for rain, snowflake already shown for snow
    if (rain_today) {
        if (rain_pct > 0) {
            char pct_buf[6];
            snprintf(pct_buf, sizeof(pct_buf), "%d%%", rain_pct);
            String ps = pct_buf;
            draw_text(ps, x, ty, fsz, c_row0);
            x += text_w(ps, fsz) + 14;  // same gap as temperature → next item
        }
        draw_umbrella(x, uby, c_row0);
        x += 12 + 14;  // umbrella icon width (12px) + consistent gap
    }

    if (uv_str && strlen(uv_str) > 0 && x < x_max) {
        String s = to_latin1(uv_str);
        draw_text_safe(s, x, ty, fsz, c_row0, 0, x_max);
    }
}

// ── Footer — dynamic region (age counter + status dot) ────────────────────────
// Erases + redraws only the right FOOTER_AGE_REGION_W pixels every second.
// Starts erase at fy+1 to preserve the separator line drawn by draw_footer_static.
// status_text: shown instead of the age when set (e.g. "closed" on the portfolio page)
static void draw_footer_dynamic(int fy, bool from_cache, bool wifi_ok,
                                int age_seconds, bool large_font,
                                time_t countdown_target, int countdown_icon,
                                const char* status_text = nullptr) {
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
        if (status_text)           snprintf(text_buf, sizeof(text_buf), "%s", status_text);
        else if (from_cache)       snprintf(text_buf, sizeof(text_buf), "cached");
        else if (age_seconds < 60) snprintf(text_buf, sizeof(text_buf), "now");
        else                       snprintf(text_buf, sizeof(text_buf), "%dm", age_seconds / 60);
    }

    String ts = text_buf;
    int text_x = SCREEN_W - PAD - 14 - text_w(ts, fsz);
    draw_text(ts, text_x, ty, fsz, c_row0);

    if (secs_left > 0 && countdown_icon > 0) {
        int icon_x = text_x - 4 - 12;
        int icon_y = fy + (FOOTER_H - 10) / 2;
        if (icon_x >= SCREEN_W - FOOTER_AGE_REGION_W)
            draw_countdown_icon(countdown_icon, icon_x, icon_y, c_row0);
    }

    uint16_t dot;
    if (!wifi_ok) dot = ((millis() / 500) % 2 == 0) ? rgb(180, 30, 0) : BLACK;
    else if (from_cache) dot = c_dim;
    else                 dot = c_meta;
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
    time_t countdown_target, int countdown_icon,
    bool snow_today, bool clear_today,
    int battery_pct, bool battery_charging)
{
    if (!gfx) return;
    apply_night_brightness();

    time_t now_t = time(nullptr);
    struct tm tm_now;
    localtime_r(&now_t, &tm_now);
    int cur_minute = tm_now.tm_hour * 60 + tm_now.tm_min;

    // Each band (header / rows / footer) is drawn off-screen and swapped in at
    // once — the live countdown redraws rows several times a minute and a visible
    // clear-then-draw would flicker.
    if (cur_minute != s_last_clock_minute || battery_pct != s_last_battery_pct || battery_charging != s_last_battery_charging) {
        s_last_clock_minute     = cur_minute;
        s_last_battery_pct      = battery_pct;
        s_last_battery_charging = battery_charging;
        FrameBuffered band(0, HEADER_H);
        draw_header(stop_name, stop_index, stop_count, false, large_font_mode, battery_pct, battery_charging);
    }

    // Rows: minutes are computed from departure times every second, so the board
    // counts down between fetches (and departed entries drop off)
    std::vector<Departure> shown = upcoming(departures, now_t);
    uint32_t sig = rows_signature(shown, large_font_mode ? 2 : ROW_COUNT, now_t);
    if (fetch_time != s_last_fetch_time || sig != s_last_rows_sig) {
        s_last_fetch_time = fetch_time;
        s_last_rows_sig   = sig;
        FrameBuffered band(HEADER_H, SCREEN_H - FOOTER_H - HEADER_H);
        draw_rows(shown, fetch_time, large_font_mode, now_t);
    }

    // Footer band: the static part is only re-rendered when the weather changes,
    // but it lives in the same canvas band as the per-second age counter
    int fy = SCREEN_H - FOOTER_H;
    uint32_t wh = weather_hash(weather_str, uv_str, rain_today, rain_pct,
                               snow_today, clear_today);
    FrameBuffered band(fy, FOOTER_H);
    if (wh != s_last_weather_hash || !s_footer_static_drawn) {
        s_last_weather_hash   = wh;
        s_footer_static_drawn = true;
        draw_footer_static(fy, weather_str, uv_str, rain_today, rain_pct,
                           snow_today, clear_today, large_font_mode);
    }
    draw_footer_dynamic(fy, from_cache, wifi_ok, age_seconds, large_font_mode, countdown_target, countdown_icon);
}

// ── Commute screen ────────────────────────────────────────────────────────────

// full_redraw=true  → clear each row and redraw all columns (data/minute changed)
// full_redraw=false → skip static rows except their time column; marquee rows always scroll
static void draw_commute_rows(const CommuteData& data, int conn_idx, time_t now_t, bool full_redraw) {
    const int rows_top = HEADER_H;
    const int rows_h   = SCREEN_H - FOOTER_H - rows_top;
    const int row_h    = rows_h / ROW_COUNT;
    const int mask_h   = row_h - 1;  // preserve separator pixel at row bottom

    if (!data.valid || data.connection_count == 0) {
        if (full_redraw) {
            gfx->fillRect(0, rows_top, SCREEN_W, rows_h, BLACK);
            String msg = (data.fetch_time > 0) ? "Commute unavailable" : "Loading...";
            draw_text(msg, (SCREEN_W - text_w(msg, 2)) / 2,
                      rows_top + rows_h / 2 - 8, 2, c_meta);
        }
        return;
    }

    struct RowInfo { int conn; int leg; };
    RowInfo row_info[ROW_COUNT];
    int n_rows = 0;
    for (int ci = conn_idx; ci < data.connection_count && n_rows < ROW_COUNT; ci++) {
        for (int li = 0; li < data.connections[ci].leg_count && n_rows < ROW_COUNT; li++) {
            row_info[n_rows++] = { ci, li };
        }
    }

    const uint8_t fsz          = 3;
    const int     row_yadv     = 28;
    const int     num_col_end  = PAD + 54;
    const int     dest_start   = num_col_end + 14;
    const int     right_margin = 64;
    const int     dest_max_w   = SCREEN_W - dest_start - right_margin - PAD;
    const int     time_col_x   = dest_start + dest_max_w;
    const int     time_col_w   = SCREEN_W - time_col_x;

    const uint32_t PAUSE_MS = 2000;
    uint32_t elapsed = millis() - s_commute_scroll_ms;
    int scroll_px = 0;
    if (elapsed > PAUSE_MS)
        scroll_px = (int)((elapsed - PAUSE_MS) * 10 / 1000);

    for (int r = 0; r < n_rows; r++) {
        const CommuteLeg& leg = data.connections[row_info[r].conn].legs[row_info[r].leg];
        int y      = rows_top + r * row_h;
        int text_y = y + (row_h - row_yadv) / 2;

        int mins = (leg.dep_time > now_t) ? (int)((leg.dep_time - now_t) / 60) : 0;
        if (mins > 99) mins = 99;

        String ln  = to_latin1(leg.line);
        String mid = to_latin1(trim_city(leg.from) + " -> " + trim_city(leg.to));
        int mid_w  = text_w(mid, fsz);

        if (mid_w <= dest_max_w) {
            // Static row: on full_redraw clear and redraw line + destination;
            // always refresh time column (mins can cross boundary at any second)
            if (full_redraw) {
                gfx->fillRect(0, y, time_col_x, mask_h, BLACK);
                draw_text(ln,  num_col_end - text_w(ln, fsz), text_y, fsz, c_row0);
                draw_text(mid, dest_start,                    text_y, fsz, c_row0);
            }
            gfx->fillRect(time_col_x, y, time_col_w, mask_h, BLACK);
            if (mins == 0) {
                draw_bus_icon(SCREEN_W - PAD - 32, text_y - 2, c_row0);
            } else {
                char tbuf[8]; snprintf(tbuf, sizeof(tbuf), "%d'", mins);
                String ts = tbuf;
                draw_text(ts, SCREEN_W - PAD - text_w(ts, fsz), text_y, fsz, c_row0);
            }
        } else {
            // Marquee row: clear only the destination column, then draw both text
            // copies confined to [dest_start, time_col_x) via x_min/x_max clipping.
            // This prevents any marquee pixel from ever entering the side columns,
            // so the left column (line number) only needs clearing on full_redraw
            // and there is no full-row black flash every second.
            gfx->fillRect(dest_start, y, dest_max_w, mask_h, BLACK);

            const int GAP_PX = 40;
            int cycle  = mid_w + GAP_PX;
            int offset = scroll_px % cycle;
            int x1 = dest_start - offset;
            int x2 = x1 + cycle;

            draw_text_safe(mid, x1, text_y, fsz, c_row0, dest_start, time_col_x);
            draw_text_safe(mid, x2, text_y, fsz, c_row0, dest_start, time_col_x);

            if (full_redraw) {
                gfx->fillRect(0, y, dest_start, mask_h, BLACK);
                draw_text(ln, num_col_end - text_w(ln, fsz), text_y, fsz, c_row0);
            }

            gfx->fillRect(time_col_x, y, time_col_w, mask_h, BLACK);
            if (mins == 0) {
                draw_bus_icon(SCREEN_W - PAD - 32, text_y - 2, c_row0);
            } else {
                char tbuf[8]; snprintf(tbuf, sizeof(tbuf), "%d'", mins);
                String ts = tbuf;
                draw_text(ts, SCREEN_W - PAD - text_w(ts, fsz), text_y, fsz, c_row0);
            }
        }

        // Separator — always redraw (trivially fast, ensures correctness after any mask)
        if (r < n_rows - 1) {
            bool inter_conn = (row_info[r+1].conn != row_info[r].conn);
            gfx->drawFastHLine(0, y + row_h - 1, SCREEN_W,
                               inter_conn ? c_meta : c_meta);
        }
    }
}

void display_draw_commute(
    const char* direction_label,
    int page_idx, int page_count,
    const CommuteData& data,
    int connection_idx,
    bool wifi_ok,
    int battery_pct,
    bool battery_charging,
    const char* weather_str, const char* uv_str,
    bool rain_today, int rain_pct,
    bool snow_today, bool clear_today)
{
    if (!gfx) return;
    apply_night_brightness();

    time_t now_t = time(nullptr);
    struct tm tm_now;
    localtime_r(&now_t, &tm_now);
    int cur_minute = tm_now.tm_hour * 60 + tm_now.tm_min;

    bool minute_changed = (cur_minute != s_commute_last_minute);
    bool data_changed   = (data.fetch_time != s_commute_last_fetch || connection_idx != s_commute_last_conn);
    bool full_redraw    = minute_changed || data_changed;

    // Header: redraw on minute or battery change
    if (minute_changed || battery_pct != s_last_battery_pct || battery_charging != s_last_battery_charging) {
        s_commute_last_minute   = cur_minute;
        s_last_battery_pct      = battery_pct;
        s_last_battery_charging = battery_charging;
        draw_header(direction_label, page_idx, page_count, false, false, battery_pct, battery_charging);
    }

    // Reset marquee when connection or data changes
    if (data_changed) {
        s_commute_last_fetch = data.fetch_time;
        s_commute_last_conn  = connection_idx;
        s_commute_scroll_ms  = millis();
    }
    draw_commute_rows(data, connection_idx, now_t, full_redraw);

    int fy    = SCREEN_H - FOOTER_H;
    int age_s = (data.fetch_time > 0) ? (int)(now_t - data.fetch_time) : 0;

    uint32_t wh = weather_hash(weather_str, uv_str, rain_today, rain_pct, snow_today, clear_today);
    if (wh != s_commute_weather_hash || full_redraw) {
        s_commute_weather_hash = wh;
        draw_footer_static(fy, weather_str, uv_str, rain_today, rain_pct,
                           snow_today, clear_today, false);
    }
    draw_footer_dynamic(fy, false, wifi_ok, age_s, false, 0, 0);
}

// ── Stocks screen ─────────────────────────────────────────────────────────────
// Content area is 5 rows. per_page=5: one full-width column, large font.
// per_page=10: two 240px columns of 5, small font, divider at SCREEN_W/2.

static const int STOCK_ROWS = 5;

static void stock_cell_rect(int slot, int per_page, int& x, int& y, int& w, int& h) {
    const int rows_h = SCREEN_H - FOOTER_H - HEADER_H;
    h = rows_h / STOCK_ROWS;
    w = (per_page > STOCK_ROWS) ? SCREEN_W / 2 : SCREEN_W;
    x = (slot / STOCK_ROWS) * w;
    y = HEADER_H + (slot % STOCK_ROWS) * h;
}

// Price with precision that fits the column: 343.83 / 1234.5 / 12345
static String format_stock_price(float p) {
    char buf[16];
    if      (p >= 10000.0f) snprintf(buf, sizeof(buf), "%.0f", p);
    else if (p >=  1000.0f) snprintf(buf, sizeof(buf), "%.1f", p);
    else                    snprintf(buf, sizeof(buf), "%.2f", p);
    return buf;
}

// Mini intraday chart in [x0, x1] × [y_top, y_top + h). Scaled to the day's range
// including the previous close, which is drawn as a dotted zero line.
static void draw_sparkline(const StockQuote& q, int x0, int x1, int y_top, int h,
                           uint16_t color) {
    if (q.spark_n < 2 || x1 - x0 < 20) return;
    float lo = (q.prev_close > 0) ? q.prev_close : q.spark[0];
    float hi = lo;
    for (int i = 0; i < q.spark_n; i++) {
        lo = std::min(lo, q.spark[i]);
        hi = std::max(hi, q.spark[i]);
    }
    if (hi - lo < 1e-6f) hi = lo + 1.0f;   // flat day: draw a level line
    auto ypix = [&](float v) {
        return y_top + h - 1 - (int)lroundf((v - lo) / (hi - lo) * (h - 1));
    };
    if (q.prev_close > 0) {
        int yb = ypix(q.prev_close);
        for (int x = x0; x <= x1; x += 3) gfx->drawPixel(x, yb, c_meta);
    }
    int n = q.spark_n;
    for (int i = 1; i < n; i++) {
        int xa = x0 + (i - 1) * (x1 - x0) / (n - 1);
        int xb = x0 + i * (x1 - x0) / (n - 1);
        gfx->drawLine(xa, ypix(q.spark[i - 1]), xb, ypix(q.spark[i]), color);
    }
}

// Repaints one cell. Bottom pixel row is left alone (row separator lives there).
static void draw_stock_cell(int slot, int per_page, const StockQuote& q) {
    int x, y, w, h;
    stock_cell_rect(slot, per_page, x, y, w, h);
    gfx->fillRect(x, y, w, h - 1, BLACK);
    if (per_page > STOCK_ROWS) gfx->drawFastVLine(SCREEN_W / 2, y, h, c_meta);

    const bool    big    = (per_page <= STOCK_ROWS);
    const uint8_t fsz    = big ? 3 : 2;
    const int     yadv   = big ? 28 : 16;
    const int     text_y = y + (h - yadv) / 2;
    const int     left   = x + ((x > 0) ? PAD + 4 : PAD);  // clear the divider in column 2
    const int     right  = x + w - PAD;
    // Fixed-width % column so prices line up regardless of sign/digits
    const int     price_right = right - (big ? 120 : 66) - (big ? 16 : 8);

    String   price;
    uint16_t price_col;
    if (q.status == STOCK_OK)             { price = format_stock_price(q.price); price_col = c_row0; }
    else if (q.status == STOCK_NOT_FOUND) { price = "n/a";                       price_col = c_dim;  }
    else                                  { price = "--";                        price_col = c_meta; }
    int price_w = text_w(price, fsz);
    draw_text(price, price_right - price_w, text_y, fsz, price_col);

    String sym = to_latin1(stocks_display_symbol(q.symbol));
    int sym_max = price_right - price_w - 8 - left;
    while (sym.length() > 1 && text_w(sym, fsz) > sym_max)
        sym = sym.substring(0, sym.length() - 1);
    draw_text(sym, left, text_y, fsz, (q.status == STOCK_NOT_FOUND) ? c_dim : c_row0);

    if (q.status != STOCK_OK) return;
    // Colour by the value as displayed, so "-0.00%" never shows up in red
    long hundredths = lroundf(q.change_pct * 100.0f);
    char pbuf[12];
    uint16_t pct_col;
    if (hundredths == 0) {
        snprintf(pbuf, sizeof(pbuf), "0.00%%");
        pct_col = c_dim;
    } else {
        snprintf(pbuf, sizeof(pbuf), "%+.2f%%", hundredths / 100.0f);
        pct_col = (hundredths > 0) ? COLOR_STOCK_UP : COLOR_STOCK_DOWN;
    }
    String pct = pbuf;
    draw_text(pct, right - text_w(pct, fsz), text_y, fsz, pct_col);

    // Mini chart in a fixed column between ticker and price (aligned across
    // rows); pushed right if a long ticker needs the room, skipped if too narrow
    const int spark_x0 = std::max(left + (big ? 112 : 52), left + text_w(sym, fsz) + 8);
    const int spark_x1 = price_right - (big ? 120 : 62);
    const int spark_h  = big ? 18 : 14;
    draw_sparkline(q, spark_x0, spark_x1, y + (h - 1 - spark_h) / 2, spark_h, pct_col);
}

void display_draw_stocks(
    const char* label,
    int page_idx, int page_count,
    const StockQuote quotes[], int count,
    int first, int per_page,
    bool market_open,
    int age_seconds,
    bool wifi_ok,
    int battery_pct,
    bool battery_charging,
    const char* weather_str, const char* uv_str,
    bool rain_today, int rain_pct,
    bool snow_today, bool clear_today)
{
    if (!gfx) return;
    apply_night_brightness();

    time_t now_t = time(nullptr);
    struct tm tm_now;
    localtime_r(&now_t, &tm_now);
    int cur_minute = tm_now.tm_hour * 60 + tm_now.tm_min;

    int on_page = std::max(0, std::min(per_page, count - first));

    // Sub-page or layout changed → clear the content area and lay out separators
    bool full_redraw = (first != s_stocks_last_first || per_page != s_stocks_last_per_page);
    if (full_redraw) {
        s_stocks_last_first    = first;
        s_stocks_last_per_page = per_page;
        for (auto& c : s_stock_cells) c.status = -1;

        const int rows_h = SCREEN_H - FOOTER_H - HEADER_H;
        const int row_h  = rows_h / STOCK_ROWS;
        gfx->fillRect(0, HEADER_H, SCREEN_W, rows_h, BLACK);
        int rows_used = std::min(on_page, STOCK_ROWS);
        for (int r = 0; r < rows_used - 1; r++)
            gfx->drawFastHLine(0, HEADER_H + (r + 1) * row_h - 1, SCREEN_W, c_meta);
        if (on_page == 0) {
            String msg = "No tickers";
            draw_text(msg, (SCREEN_W - text_w(msg, 2)) / 2, HEADER_H + rows_h / 2 - 8, 2, c_meta);
        }
    }

    // Header: label carries the sub-page number, which only changes on a
    // sub-page switch (full_redraw); the clock needs a minute-change redraw
    if (full_redraw || cur_minute != s_stocks_last_minute ||
        battery_pct != s_last_battery_pct || battery_charging != s_last_battery_charging) {
        s_stocks_last_minute    = cur_minute;
        s_last_battery_pct      = battery_pct;
        s_last_battery_charging = battery_charging;
        draw_header(label, page_idx, page_count, false, false, battery_pct, battery_charging);
    }

    // Cells: repaint only those whose displayed values changed
    for (int s = 0; s < on_page; s++) {
        const StockQuote& q = quotes[first + s];
        StockCellState& c = s_stock_cells[s];
        if (c.status == (int8_t)q.status && c.price == q.price && c.pct == q.change_pct) continue;
        c = { (int8_t)q.status, q.price, q.change_pct };
        draw_stock_cell(s, per_page, q);
    }

    int fy = SCREEN_H - FOOTER_H;
    uint32_t wh = weather_hash(weather_str, uv_str, rain_today, rain_pct, snow_today, clear_today);
    if (wh != s_stocks_weather_hash || full_redraw) {
        s_stocks_weather_hash = wh;
        draw_footer_static(fy, weather_str, uv_str, rain_today, rain_pct,
                           snow_today, clear_today, false);
    }
    draw_footer_dynamic(fy, false, wifi_ok, age_seconds, false, 0, 0,
                        market_open ? nullptr : "closed");
}

int display_stock_cell_at(int x, int y, int per_page) {
    const int rows_h = SCREEN_H - FOOTER_H - HEADER_H;
    if (y < HEADER_H || y >= HEADER_H + rows_h) return -1;
    int row = (y - HEADER_H) / (rows_h / STOCK_ROWS);
    int col = (per_page > STOCK_ROWS && x >= SCREEN_W / 2) ? 1 : 0;
    return std::min(row, STOCK_ROWS - 1) + col * STOCK_ROWS;
}

// ── Stock detail view ────────────────────────────────────────────────────────
// Left: price, change, previous close / open / day range / 52-week range.
// Right: the day's chart with the previous close as a dotted line.
// Full repaint (off-screen) when the ticker, its data or the minute changes;
// otherwise only the footer's age counter.

static void draw_detail_chart(const StockChart& c, int x0, int y0, int w, int h, uint16_t color) {
    float lo = (c.prev_close > 0) ? c.prev_close : c.pts[0];
    float hi = lo;
    for (int i = 0; i < c.n; i++) { lo = std::min(lo, c.pts[i]); hi = std::max(hi, c.pts[i]); }
    if (hi - lo < 1e-6f) hi = lo + 1.0f;
    auto ypix = [&](float v) { return y0 + h - 1 - (int)lroundf((v - lo) / (hi - lo) * (h - 1)); };

    gfx->drawRect(x0, y0, w, h, c_meta);
    if (c.prev_close > 0) {
        int yb = ypix(c.prev_close);
        for (int x = x0 + 2; x < x0 + w - 2; x += 4) gfx->drawPixel(x, yb, c_dim);
    }
    for (int i = 1; i < c.n; i++) {
        int xa = x0 + 1 + (i - 1) * (w - 3) / (c.n - 1);
        int xb = x0 + 1 + i * (w - 3) / (c.n - 1);
        gfx->drawLine(xa, ypix(c.pts[i - 1]), xb, ypix(c.pts[i]), color);
        gfx->drawLine(xa, ypix(c.pts[i - 1]) + 1, xb, ypix(c.pts[i]) + 1, color);   // 2 px thick
    }
    // High / low of the chart in the corners
    String hs = format_stock_price(hi), ls = format_stock_price(lo);
    draw_text(hs, x0 + w - 4 - text_w(hs, 2), y0 + 3, 2, c_meta);
    draw_text(ls, x0 + w - 4 - text_w(ls, 2), y0 + h - 19, 2, c_meta);
}

void display_draw_stock_detail(const char* label, int page_idx, int page_count,
                               const StockQuote& q, const StockChart* chart,
                               bool market_open, int age_seconds,
                               const FooterInfo& f) {
    if (!gfx) return;
    apply_night_brightness();

    time_t now_t = time(nullptr);
    struct tm tm_now;
    localtime_r(&now_t, &tm_now);
    int cur_minute = tm_now.tm_hour * 60 + tm_now.tm_min;

    // Repaint when anything shown changes (ticker, quote, chart arrival, minute)
    uint32_t sig = 2166136261u;
    for (char ch : q.symbol) sig = (sig ^ (uint8_t)ch) * 16777619u;
    sig = (sig ^ (uint32_t)q.fetch_time) * 16777619u;
    sig = (sig ^ (uint32_t)(chart ? chart->n + 1 : 0)) * 16777619u;
    sig &= 0x7FFFFFFF;   // never equals the -1 "repaint" marker
    uint32_t wh = weather_hash(f.weather_str, f.uv_str, f.rain_today, f.rain_pct,
                               f.snow_today, f.clear_today);
    int fy = SCREEN_H - FOOTER_H;

    if ((int)sig != s_detail_idx_sig || cur_minute != s_detail_minute || wh != s_detail_weather) {
        s_detail_idx_sig = (int)sig;
        s_detail_minute  = cur_minute;
        s_detail_weather = wh;
        s_last_battery_pct = f.battery_pct;
        s_last_battery_charging = f.battery_charging;

        FrameBuffered frame;   // whole screen off-screen → no flicker
        gfx->fillScreen(BLACK);
        draw_header(label, page_idx, page_count, false, false, f.battery_pct, f.battery_charging);

        const int top = HEADER_H + 4;
        String name = to_latin1(q.name.length() ? q.name : q.symbol);
        draw_text_safe(name, PAD, top, 2, c_dim, 0, SCREEN_W - PAD);

        const int chart_x = 218, chart_y = top + 24, chart_w = SCREEN_W - PAD - chart_x;
        const int chart_h = fy - 6 - chart_y;

        if (q.status != STOCK_OK) {
            String msg = (q.status == STOCK_NOT_FOUND) ? "Symbol not found" : "Loading...";
            draw_text(msg, PAD, top + 40, 3, c_meta);
        } else {
            long hundredths = lroundf(q.change_pct * 100.0f);
            uint16_t col = (hundredths > 0) ? COLOR_STOCK_UP : (hundredths < 0) ? COLOR_STOCK_DOWN : c_dim;

            draw_text(format_stock_price(q.price), PAD, top + 22, 4, c_row0);
            char chg[32];
            snprintf(chg, sizeof(chg), "%+.2f (%+.2f%%)", q.price - q.prev_close, hundredths / 100.0f);
            draw_text(chg, PAD, top + 56, 2, col);

            // Figures: label in dim, value in main colour; ranges as "low-high"
            struct Row { const char* label; String value; };
            auto range = [](float a, float b) {
                return (a > 0 && b > 0) ? format_stock_price(a) + "-" + format_stock_price(b) : String("--");
            };
            Row rows[] = {
                { "Prev",  q.prev_close > 0 ? format_stock_price(q.prev_close) : String("--") },
                { "Open",  q.open > 0       ? format_stock_price(q.open)       : String("--") },
                { "Day",   range(q.day_low, q.day_high) },
                { "52w",   range(q.year_low, q.year_high) },
            };
            int y = top + 80;
            for (const Row& r : rows) {
                draw_text(r.label, PAD, y, 2, c_dim);
                draw_text_safe(r.value, PAD + 48, y, 2, c_row0, 0, chart_x - 6);
                y += 17;
            }

            if (!chart) {
                String msg = "Loading chart...";
                draw_text(msg, chart_x + (chart_w - text_w(msg, 2)) / 2, chart_y + chart_h / 2 - 8, 2, c_meta);
            } else if (chart->n < 2) {
                String msg = "No chart";
                draw_text(msg, chart_x + (chart_w - text_w(msg, 2)) / 2, chart_y + chart_h / 2 - 8, 2, c_meta);
            } else {
                draw_detail_chart(*chart, chart_x, chart_y, chart_w, chart_h, col);
            }
        }

        draw_footer_static(fy, f.weather_str, f.uv_str, f.rain_today, f.rain_pct,
                           f.snow_today, f.clear_today, false);
        draw_footer_dynamic(fy, false, f.wifi_ok, age_seconds, false, 0, 0,
                            market_open ? nullptr : "closed");
        return;
    }

    FrameBuffered band(fy, FOOTER_H);   // canvas band still holds the static footer
    draw_footer_dynamic(fy, false, f.wifi_ok, age_seconds, false, 0, 0,
                        market_open ? nullptr : "closed");
}

// ── Weather page ─────────────────────────────────────────────────────────────

// Footer texts: "Air: Good (32)" and "Pollen: Grass high, Birch low" / "none"
static void weather_footer_texts(const WeatherDetail& d, String& air, String& pollen) {
    air = (d.aqi >= 0) ? String("Air: ") + weather_aqi_text(d.aqi) + " (" + d.aqi + ")" : String("");

    // Up to two pollen types with the highest level
    int best[2] = { -1, -1 }, best_lvl[2] = { 0, 0 };
    bool reported = false;
    for (int i = 0; i < POLLEN_COUNT; i++) {
        if (d.pollen[i] < 0) continue;
        reported = true;
        int lvl = weather_pollen_level((Pollen)i, d.pollen[i]);
        if (lvl > best_lvl[0])      { best[1] = best[0]; best_lvl[1] = best_lvl[0]; best[0] = i; best_lvl[0] = lvl; }
        else if (lvl > best_lvl[1]) { best[1] = i; best_lvl[1] = lvl; }
    }
    if (!reported) { pollen = ""; return; }
    if (best[0] < 0) { pollen = "Pollen: none"; return; }
    pollen = String("Pollen: ") + weather_pollen_name((Pollen)best[0]) + " " + weather_level_text(best_lvl[0]);
    if (best[1] >= 0)
        pollen += String(", ") + weather_pollen_name((Pollen)best[1]) + " " + weather_level_text(best_lvl[1]);
}

// Next hours: rain-chance bars (dim) with the temperature as a line on top
static void draw_hourly_chart(const WeatherDetail& d, int x0, int y0, int w, int h) {
    if (d.hours < 2) return;
    const int label_h = 18;                 // hour labels below the plot
    const int plot_h  = h - label_h;
    const int slot    = w / d.hours;

    // Rain chance: 0–100 % over the full plot height
    for (int i = 0; i < d.hours; i++) {
        int bh = d.hour_rain_prob[i] * (plot_h - 2) / 100;
        if (bh > 0) gfx->fillRect(x0 + i * slot + 3, y0 + plot_h - bh, slot - 6, bh, c_meta);
    }
    gfx->drawFastHLine(x0, y0 + plot_h, w, c_meta);

    // Temperature: scaled to the period's range, kept clear of the labels above
    float lo = d.hour_temp[0], hi = lo;
    for (int i = 0; i < d.hours; i++) { lo = std::min(lo, d.hour_temp[i]); hi = std::max(hi, d.hour_temp[i]); }
    if (hi - lo < 2) { float mid = (hi + lo) / 2; lo = mid - 1; hi = mid + 1; }
    const int t_top = y0 + 20, t_bot = y0 + plot_h - 8;
    auto ty = [&](float v) { return t_bot - (int)lroundf((v - lo) / (hi - lo) * (t_bot - t_top)); };
    auto cx = [&](int i)   { return x0 + i * slot + slot / 2; };
    for (int i = 1; i < d.hours; i++) {
        gfx->drawLine(cx(i - 1), ty(d.hour_temp[i - 1]),     cx(i), ty(d.hour_temp[i]),     c_row0);
        gfx->drawLine(cx(i - 1), ty(d.hour_temp[i - 1]) + 1, cx(i), ty(d.hour_temp[i]) + 1, c_row0);
    }

    // Every third hour: temperature above the line, hour below the axis
    for (int i = 0; i < d.hours; i += 3) {
        char t[8], hr[4];
        snprintf(t, sizeof(t), "%d\xB0", (int)lroundf(d.hour_temp[i]));
        struct tm tm_h; localtime_r(&d.hour_time[i], &tm_h);
        snprintf(hr, sizeof(hr), "%02d", tm_h.tm_hour);
        String ts = t, hs = hr;
        gfx->fillRect(cx(i) - 2, ty(d.hour_temp[i]) - 2, 5, 5, c_row0);
        draw_text(ts, cx(i) - text_w(ts, 2) / 2, ty(d.hour_temp[i]) - 20, 2, c_row0);
        draw_text(hs, cx(i) - text_w(hs, 2) / 2, y0 + plot_h + 2, 2, c_dim);
    }
}

void display_draw_weather(const char* label, int page_idx, int page_count,
                          const WeatherDetail* d, bool not_found, const FooterInfo& f) {
    if (!gfx) return;
    apply_night_brightness();

    time_t now_t = time(nullptr);
    struct tm tm_now;
    localtime_r(&now_t, &tm_now);
    int cur_minute = tm_now.tm_hour * 60 + tm_now.tm_min;
    int fy = SCREEN_H - FOOTER_H;
    time_t fetched = d ? d->fetched : 0;
    int age_s = (fetched > 0) ? (int)(now_t - fetched) : 0;

    if (fetched != s_wx_drawn_fetch || cur_minute != s_wx_minute) {
        s_wx_drawn_fetch = fetched;
        s_wx_minute      = cur_minute;
        s_last_battery_pct      = f.battery_pct;
        s_last_battery_charging = f.battery_charging;

        FrameBuffered frame;   // whole screen off-screen → no flicker
        gfx->fillScreen(BLACK);
        draw_header(label, page_idx, page_count, false, false, f.battery_pct, f.battery_charging);

        if (!d) {
            String msg = not_found ? "Town not found" : "Loading weather...";
            draw_text(msg, (SCREEN_W - text_w(msg, 3)) / 2, HEADER_H + 50, 3, c_meta);
            if (not_found) {
                String hint = "Check the spelling in Settings";
                draw_text(hint, (SCREEN_W - text_w(hint, 2)) / 2, HEADER_H + 86, 2, c_meta);
            }
            draw_footer_static(fy, "", "", false, 0, false, false, false);
            draw_footer_dynamic(fy, false, f.wifi_ok, 0, false, 0, 0, "");
            return;
        }

        // Left: now + today (each line clipped so it never runs into the chart)
        const int top     = HEADER_H + 6;
        const int chart_x = 196;
        const int left_max = chart_x - 6;
        draw_text(weather_condition_text(d->weather_code), PAD, top, 2, c_dim);
        char buf[40];
        snprintf(buf, sizeof(buf), "%d\xB0" "C", (int)lroundf(d->temp_c));
        draw_text(buf, PAD, top + 20, 4, c_row0);
        snprintf(buf, sizeof(buf), "Feels like %d\xB0", (int)lroundf(d->feels_like_c));
        draw_text_safe(buf, PAD, top + 56, 2, c_row0, 0, left_max);
        snprintf(buf, sizeof(buf), "Low %d\xB0  High %d\xB0",
                 (int)lroundf(d->temp_min_c), (int)lroundf(d->temp_max_c));
        draw_text_safe(buf, PAD, top + 74, 2, c_row0, 0, left_max);
        snprintf(buf, sizeof(buf), "Rain %d%%   UV %d", d->rain_prob_max, d->uv_index);
        draw_text_safe(buf, PAD, top + 92, 2, c_row0, 0, left_max);
        // Wind; gusts only when they're notably stronger than the wind itself
        int wind = (int)lroundf(d->wind_kmh), gust = (int)lroundf(d->gust_kmh);
        if (gust >= wind + 15) snprintf(buf, sizeof(buf), "Wind %d, gusts %d", wind, gust);
        else                   snprintf(buf, sizeof(buf), "Wind %d km/h", wind);
        draw_text_safe(buf, PAD, top + 110, 2, c_row0, 0, left_max);
        // Next sun event: sunrise before dawn, sunset during the day
        if (d->sunrise > 0 && d->sunset > 0) {
            bool before_sunrise = now_t < d->sunrise;
            time_t ev = (before_sunrise || now_t >= d->sunset) ? d->sunrise : d->sunset;
            struct tm te; localtime_r(&ev, &te);
            snprintf(buf, sizeof(buf), "%s %02d:%02d",
                     (ev == d->sunrise) ? "Sunrise" : "Sunset", te.tm_hour, te.tm_min);
            draw_text_safe(buf, PAD, top + 128, 2, c_dim, 0, left_max);
        }

        // Right: next 12 hours
        draw_hourly_chart(*d, chart_x, top, SCREEN_W - PAD - chart_x, fy - 4 - top);

        String air, pollen;
        weather_footer_texts(*d, air, pollen);
        draw_footer_static(fy, air.c_str(), pollen.c_str(), false, 0, false, false, false);
        draw_footer_dynamic(fy, false, f.wifi_ok, age_s, false, 0, 0);
        return;
    }

    FrameBuffered band(fy, FOOTER_H);   // canvas band still holds the static footer
    draw_footer_dynamic(fy, false, f.wifi_ok, age_s, false, 0, 0, d ? nullptr : "");
}

// ── Device info page ─────────────────────────────────────────────────────────

static uint16_t info_value_color(const InfoRow& r) {
    if (r.color) return r.color;
    return (r.state == 2) ? COLOR_STOCK_DOWN : c_row0;
}

// Split text into lines that fit max_w at font size fsz (at most max_lines;
// the last one gets "..." if the text doesn't fit)
static int wrap_text(const String& text, int max_w, uint8_t fsz, String* lines, int max_lines) {
    int n = 0;
    String cur;
    int pos = 0;
    while (pos < (int)text.length() && n < max_lines) {
        int sp = text.indexOf(' ', pos);
        String word = (sp < 0) ? text.substring(pos) : text.substring(pos, sp);
        pos = (sp < 0) ? text.length() : sp + 1;
        String trial = cur.length() ? cur + " " + word : word;
        if (text_w(trial, fsz) <= max_w || cur.isEmpty()) {
            cur = trial;
        } else {
            lines[n++] = cur;
            cur = word;
        }
    }
    if (cur.length() && n < max_lines) lines[n++] = cur;
    if (pos < (int)text.length() && n == max_lines) {        // didn't all fit
        String& last = lines[max_lines - 1];
        while (last.length() > 1 && text_w(last + "...", fsz) > max_w) last.remove(last.length() - 1);
        last += "...";
    }
    return n;
}

void display_draw_today(const char* label, int page_idx, int page_count,
                        const InfoRow* rows, int n_rows,
                        const String& quote, const String& author,
                        const FooterInfo& f) {
    if (!gfx) return;
    apply_night_brightness();

    time_t now_t = time(nullptr);
    struct tm tm_now;
    localtime_r(&now_t, &tm_now);

    // Repaint only when the text (or the minute) changes
    uint32_t sig = 2166136261u;
    auto mix = [&](const String& s) { for (char c : s) sig = (sig ^ (uint8_t)c) * 16777619u; };
    for (int i = 0; i < n_rows; i++) { mix(rows[i].label); mix(rows[i].value); sig ^= rows[i].color; }
    mix(quote);
    sig ^= (uint32_t)(tm_now.tm_hour * 60 + tm_now.tm_min) << 7;
    sig ^= (uint32_t)(f.battery_pct + 1) << 20;
    if (sig == 0) sig = 1;
    if (sig == s_info_sig) return;
    s_info_sig = sig;
    s_last_battery_pct      = f.battery_pct;
    s_last_battery_charging = f.battery_charging;

    FrameBuffered frame;
    gfx->fillScreen(BLACK);
    draw_header(label, page_idx, page_count, false, false, f.battery_pct, f.battery_charging);

    // Rows: small dim label, large value (TramliLarge, 28 px line), label
    // vertically centred on the value
    const int top = HEADER_H + 6, line_h = 30, value_x = PAD + 96;
    for (int i = 0; i < n_rows; i++) {
        int y = top + i * line_h;
        draw_text(rows[i].label, PAD, y + 7, 2, c_dim);
        draw_text_safe(to_latin1(rows[i].value), value_x, y, 3, info_value_color(rows[i]), 0, SCREEN_W - PAD);
    }

    // Quote of the day fills the space below the rows
    if (quote.length()) {
        int qy = top + n_rows * line_h + 6;
        gfx->drawFastHLine(PAD, qy - 5, SCREEN_W - 2 * PAD, c_meta);
        int max_lines = std::max(1, (SCREEN_H - 4 - qy) / 18 - 1);   // keep a line for the author
        String lines[4];
        // Straight quotes: the fonts are Latin-1, curly quotes would show as "?"
        int n = wrap_text(to_latin1(String("\"") + quote + "\""), SCREEN_W - 2 * PAD, 2,
                          lines, std::min(max_lines, 4));
        for (int i = 0; i < n; i++) draw_text(lines[i], PAD, qy + i * 18, 2, c_dim);
        if (author.length()) {
            String a = to_latin1(String("- ") + author);
            draw_text(a, SCREEN_W - PAD - text_w(a, 2), qy + n * 18, 2, c_meta);
        }
    }
}

void display_draw_info(const char* label, int page_idx, int page_count,
                       const InfoRow* left, int n_left,
                       const InfoRow* right, int n_right,
                       const FooterInfo& f) {
    if (!gfx) return;
    apply_night_brightness();

    time_t now_t = time(nullptr);
    struct tm tm_now;
    localtime_r(&now_t, &tm_now);

    // Repaint only when any text (or the clock minute) changes
    uint32_t sig = 2166136261u;
    auto mix = [&](const String& s) { for (char c : s) sig = (sig ^ (uint8_t)c) * 16777619u; };
    for (int i = 0; i < n_left;  i++) { mix(left[i].value);  sig ^= left[i].state; }
    for (int i = 0; i < n_right; i++) { mix(right[i].value); sig ^= right[i].state << 3; }
    sig ^= (uint32_t)(tm_now.tm_hour * 60 + tm_now.tm_min) << 7;
    sig ^= (uint32_t)(f.battery_pct + 1) << 20;
    if (sig == 0) sig = 1;
    if (sig == s_info_sig) return;
    s_info_sig = sig;
    s_last_battery_pct      = f.battery_pct;
    s_last_battery_charging = f.battery_charging;

    FrameBuffered frame;
    gfx->fillScreen(BLACK);
    draw_header(label, page_idx, page_count, false, false, f.battery_pct, f.battery_charging);

    const int top = HEADER_H + 6, line_h = 18;
    auto column = [&](const InfoRow* rows, int n, int x, int value_x, int x_max) {
        for (int i = 0; i < n; i++) {
            int y = top + i * line_h;
            draw_text(rows[i].label, x, y, 2, c_dim);
            draw_text_safe(to_latin1(rows[i].value), value_x, y, 2, info_value_color(rows[i]), 0, x_max);
        }
    };
    column(left,  n_left,  PAD, PAD + 78, 250);
    gfx->drawFastVLine(256, top, line_h * std::max(n_left, n_right) - 4, c_meta);
    column(right, n_right, 266, 266 + 96, SCREEN_W - PAD);

    int fy = SCREEN_H - FOOTER_H;
    draw_footer_static(fy, f.weather_str, f.uv_str, false, 0, false, false, false);
    draw_footer_dynamic(fy, false, f.wifi_ok, 0, false, 0, 0, "");
}

// ── QR codes ─────────────────────────────────────────────────────────────────
// Generated with ESP-IDF's qrcode component (built into the framework). Drawn
// dark-on-white with the standard 4-module quiet zone — most phone scanners
// can't read inverted (light-on-dark) codes.

static int s_qr_x = 0, s_qr_y = 0, s_qr_scale = 4, s_qr_drawn = 0;

static void qr_draw_cb(esp_qrcode_handle_t qr) {
    int n = esp_qrcode_get_size(qr);
    int quiet = 4 * s_qr_scale;
    s_qr_drawn = n * s_qr_scale + 2 * quiet;
    gfx->fillRect(s_qr_x, s_qr_y, s_qr_drawn, s_qr_drawn, WHITE);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (esp_qrcode_get_module(qr, x, y))
                gfx->fillRect(s_qr_x + quiet + x * s_qr_scale, s_qr_y + quiet + y * s_qr_scale,
                              s_qr_scale, s_qr_scale, BLACK);
}

// Draws a QR code with its top-left corner at x,y. Returns its side length in px.
static int draw_qr(int x, int y, int scale, const char* text) {
    s_qr_x = x; s_qr_y = y; s_qr_scale = scale; s_qr_drawn = 0;
    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func       = qr_draw_cb;
    cfg.max_qrcode_version = 5;               // ≤ 37 modules — fits the screen
    cfg.qrcode_ecc_level   = ESP_QRCODE_ECC_LOW;
    if (esp_qrcode_generate(&cfg, text) != ESP_OK) Serial.printf("[QR] failed for '%s'\n", text);
    return s_qr_drawn;
}

void display_show_setup_qr(const char* ssid, const char* url) {
    if (!gfx) return;
    display_invalidate();
    FrameBuffered frame;
    gfx->fillScreen(BLACK);

    String title = "Set up Busli";
    draw_text(title, (SCREEN_W - text_w(title, 2)) / 2, 4, 2, c_row0);

    // Wi-Fi join code (open network): WIFI:T:nopass;S:<ssid>;;
    String wifi = String("WIFI:T:nopass;S:") + ssid + ";;";
    const int scale = 4, top = 26;
    int s1 = draw_qr(36, top, scale, wifi.c_str());
    int s2 = draw_qr(SCREEN_W - 36 - s1, top, scale, url);
    int cap_y = top + std::max(s1, s2) + 6;

    String c1 = "1. Join Wi-Fi", c2 = "2. Open settings";
    draw_text(c1, 36 + (s1 - text_w(c1, 2)) / 2, cap_y, 2, c_row0);
    draw_text(c2, SCREEN_W - 36 - s2 + (s2 - text_w(c2, 2)) / 2, cap_y, 2, c_row0);

    String words = String("or join ") + ssid + " and open " + (strstr(url, "//") ? strstr(url, "//") + 2 : url);
    draw_text_safe(words, (SCREEN_W - text_w(words, 2)) / 2, cap_y + 18, 2, c_dim, 0, SCREEN_W);
    String cancel = "Touch the screen to cancel";
    draw_text(cancel, (SCREEN_W - text_w(cancel, 2)) / 2, cap_y + 36, 2, c_meta);
}

void display_show_settings_qr(const char* url, const char* hostname, const char* ip) {
    if (!gfx) return;
    display_invalidate();
    FrameBuffered frame;
    gfx->fillScreen(BLACK);

    int side = draw_qr(16, (SCREEN_H - 185) / 2, 5, url);
    int tx = 16 + side + 20;
    int ty = (SCREEN_H - 150) / 2;
    draw_text("Scan to open", tx, ty, 3, c_row0);
    draw_text("settings", tx, ty + 30, 3, c_row0);
    draw_text("No code needed", tx, ty + 68, 2, c_dim);
    draw_text(String("or open ") + hostname + ".local", tx, ty + 98, 2, c_dim);
    draw_text(String("or ") + ip, tx, ty + 116, 2, c_dim);
    draw_text("Touch to close", tx, ty + 140, 2, c_meta);
}

// ── Night clock ──────────────────────────────────────────────────────────────
// Seven-segment digits drawn as rounded bars: crisp at any size (the fonts top
// out at 28 px) and calm to look at in the dark.

static void draw_7seg(int x, int y, int w, int h, int t, int digit, uint16_t col) {
    // Bits: a b c d e f g (top, top-right, bottom-right, bottom, bottom-left, top-left, middle)
    static const uint8_t SEG[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    if (digit < 0 || digit > 9) return;
    uint8_t s  = SEG[digit];
    int     vl = (h - 3 * t) / 2;   // length of a vertical segment
    int     r  = t / 2;
    if (s & 0x01) gfx->fillRoundRect(x + t, y,                  w - 2 * t, t, r, col);  // a
    if (s & 0x02) gfx->fillRoundRect(x + w - t, y + t,          t, vl, r, col);         // b
    if (s & 0x04) gfx->fillRoundRect(x + w - t, y + 2 * t + vl, t, vl, r, col);         // c
    if (s & 0x08) gfx->fillRoundRect(x + t, y + h - t,          w - 2 * t, t, r, col);  // d
    if (s & 0x10) gfx->fillRoundRect(x, y + 2 * t + vl,         t, vl, r, col);         // e
    if (s & 0x20) gfx->fillRoundRect(x, y + t,                  t, vl, r, col);         // f
    if (s & 0x40) gfx->fillRoundRect(x + t, y + t + vl,         w - 2 * t, t, r, col);  // g
}

void display_draw_night_clock(const char* subtitle) {
    if (!gfx) return;
    apply_night_brightness();

    time_t now_t = time(nullptr);
    struct tm t;
    localtime_r(&now_t, &t);
    int minute = t.tm_hour * 60 + t.tm_min;
    if (minute == s_clock_minute) return;
    s_clock_minute = minute;

    FrameBuffered frame;
    gfx->fillScreen(BLACK);

    // H H : M M — 84×150 digits, 14 px segments; leading hour zero left blank
    const int dw = 84, dh = 150, th = 14, gap = 16, colon_w = 56;
    const int total = 4 * dw + 2 * gap + colon_w;
    int x = (SCREEN_W - total) / 2, y = 14;
    uint16_t col = c_dim;
    if (now_t > 1700000000) {
        if (t.tm_hour >= 10) draw_7seg(x, y, dw, dh, th, t.tm_hour / 10, col);
        draw_7seg(x + dw + gap, y, dw, dh, th, t.tm_hour % 10, col);
        int cx = x + 2 * dw + gap + colon_w / 2;
        gfx->fillRoundRect(cx - th / 2, y + dh / 3 - th / 2,     th, th, th / 2, col);
        gfx->fillRoundRect(cx - th / 2, y + 2 * dh / 3 - th / 2, th, th, th / 2, col);
        int mx = x + 2 * dw + gap + colon_w;
        draw_7seg(mx, y, dw, dh, th, t.tm_min / 10, col);
        draw_7seg(mx + dw + gap, y, dw, dh, th, t.tm_min % 10, col);
    }

    if (subtitle && *subtitle) {
        String s = to_latin1(subtitle);
        draw_text(s, (SCREEN_W - text_w(s, 2)) / 2, y + dh + 22, 2, c_meta);
    }
}

// ── Flight screen ─────────────────────────────────────────────────────────────

// Small plane icon pointing right, centered at (cx, cy), ~14×10 px (header use)
static void draw_plane_right(int cx, int cy, uint16_t col) {
    gfx->fillRect(cx - 6, cy - 1, 12, 3, col);  // fuselage
    gfx->fillRect(cx + 5, cy,      2, 1, col);  // nose tip
    gfx->fillRect(cx - 1, cy - 5,  3, 11, col); // main wing
    gfx->fillRect(cx - 6, cy - 3,  3, 7, col);  // tail body
}

// Large plane icon pointing right, centered at (cx, cy).
// 4px fuselage so the body reads as distinct from the wings.
// Wings root at mid-fuselage so the nose protrudes ~22px past the wing leading edge.
static void draw_plane_large(int cx, int cy, uint16_t col) {
    // Fuselage: 4px body + tapered nose
    gfx->fillRect(cx - 18, cy - 2, 40, 4, col);   // main body  cx-18…cx+21
    gfx->fillRect(cx + 22, cy - 1,  4, 2, col);   // nose taper cx+22…cx+25, 2px
    gfx->fillRect(cx + 26, cy,      2, 1, col);   // nose tip   cx+26…cx+27, 1px

    // Main wings: 5 rows × 2px, root LE=cx+4, sweeps back 2px/row each edge, tapers slightly
    // Upper half (each row starts at cy-3 and steps up 2px):
    gfx->fillRect(cx -  6, cy -  3, 11, 2, col);  // root  cx-6…cx+4, chord=11
    gfx->fillRect(cx -  8, cy -  5, 10, 2, col);  //       cx-8…cx+1, chord=10
    gfx->fillRect(cx - 10, cy -  7,  9, 2, col);  //       cx-10…cx-2, chord=9
    gfx->fillRect(cx - 12, cy -  9,  8, 2, col);  //       cx-12…cx-5, chord=8
    gfx->fillRect(cx - 14, cy - 11,  6, 2, col);  // tip   cx-14…cx-9, chord=6
    // Lower half (mirror):
    gfx->fillRect(cx -  6, cy +  1, 11, 2, col);
    gfx->fillRect(cx -  8, cy +  3, 10, 2, col);
    gfx->fillRect(cx - 10, cy +  5,  9, 2, col);
    gfx->fillRect(cx - 12, cy +  7,  8, 2, col);
    gfx->fillRect(cx - 14, cy +  9,  6, 2, col);

    // Horizontal tail stabilizers — smaller, at rear, merges into fuselage edges
    gfx->fillRect(cx - 18, cy - 5,  8, 4, col);   // upper tail  (3px above fuselage)
    gfx->fillRect(cx - 18, cy + 1,  8, 4, col);   // lower tail  (3px below fuselage)
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
    gfx->drawFastHLine(0, fy, SCREEN_W, c_meta);

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
    draw_text(age_str, SCREEN_W - PAD - 14 - aw, ty, 2, c_row0);

    String hint = "swipe left for stops";
    draw_text(hint, PAD, ty, 2, c_meta);

    uint16_t dot = wifi_ok ? c_meta : ((millis() / 500) % 2 == 0 ? rgb(180,30,0) : BLACK);
    gfx->fillRect(SCREEN_W - PAD - 6, fy + (FOOTER_H - 6) / 2, 6, 6, dot);
}

static void draw_flight_content(time_t now_t, const FlightInfo& fi) {
    // Layout (Y = top of character cell):
    //   APT_Y    = 48  — ICAO codes, size=4 (28px ascent, 32px yAdv)
    //   BAR_CY   = 61  — route line + plane, inline with mid-height of codes
    //   TIME_Y   = 82  — dep/arr times below codes, size=2 (16px)
    //   STATUS_Y = 106 — status label, size=2
    //   STATS_Y  = 128 — alt/speed/heading, size=2 (airborne only)
    const int CTY      = HEADER_H + 4;   // 40
    const int APT_Y    = CTY + 8;        // 48
    const int BAR_CY   = APT_Y + XLARGE_ASCENT / 2;  // 61
    const int TIME_Y   = APT_Y + 34;    // 82 — 2px below yAdv of size=4 text
    const int STATUS_Y = CTY + 66;      // 106
    const int STATS_Y  = CTY + 88;      // 128

    if (fi.callsign.isEmpty()) {
        String msg = "No flight configured";
        draw_text(msg, (SCREEN_W - text_w(msg, 2)) / 2, CTY + 70, 2, c_meta);
        return;
    }
    if (!fi.valid) {
        String msg = "Searching...";
        draw_text(msg, (SCREEN_W - text_w(msg, 2)) / 2, CTY + 70, 2, c_meta);
        return;
    }

    bool no_signal = !fi.airborne && fi.dep_icao.isEmpty() && fi.arr_icao.isEmpty()
                     && fi.dep_time == 0 && fi.arr_time == 0;
    // Flight phase from vertical rate: ±2.5 m/s ≈ ±500 ft/min
    String status_str;
    if      (fi.airborne && fi.vert_rate >  2.5f)      status_str = "Climbing";
    else if (fi.airborne && fi.vert_rate < -2.5f)      status_str = "Descending";
    else if (fi.airborne)                              status_str = "Cruising";
    else if (fi.arr_time > 0 && now_t > fi.arr_time)  status_str = "Landed";
    else if (fi.dep_actual && fi.arr_time > 0)         status_str = "In flight";  // coverage gap
    else if (fi.dep_time > 0 && now_t < fi.dep_time)  status_str = "Pre-flight";
    else if (!fi.on_ground && fi.dep_time > 0)         status_str = "In flight";
    else if (no_signal)                                status_str = "No signal";
    else                                               status_str = "On ground";

    // Airport codes — size=4, dim when no live data
    uint16_t apt_col = no_signal ? c_meta : c_row0;
    String dep_iata = fi.dep_icao.length() > 0 ? fi.dep_icao : "---";
    String arr_iata = fi.arr_icao.length() > 0 ? fi.arr_icao : "---";
    int16_t dep_w = text_w(dep_iata, 4);
    int16_t arr_w = text_w(arr_iata, 4);
    draw_text(dep_iata, PAD,                      APT_Y, 4, apt_col);
    draw_text(arr_iata, SCREEN_W - PAD - arr_w,   APT_Y, 4, apt_col);

    // Departure / arrival times below airport codes
    // Left-aligned under departure code; right-aligned under arrival code.
    // "~" marks an estimated arrival (computed from live position).
    char dep_tbuf[12] = "--:--";
    char arr_tbuf[16] = "--:--";
    if (fi.dep_time > 0) {
        struct tm td; localtime_r(&fi.dep_time, &td);
        snprintf(dep_tbuf, sizeof(dep_tbuf), "%02d:%02d", td.tm_hour, td.tm_min);
    }
    if (fi.arr_time > 0) {
        struct tm ta; localtime_r(&fi.arr_time, &ta);
        const char* est = fi.arr_estimated ? "~" : "";
        if (fi.dep_time > 0) {
            struct tm td2; localtime_r(&fi.dep_time, &td2);
            int delta_days = ta.tm_yday - td2.tm_yday;
            if (delta_days != 0)
                snprintf(arr_tbuf, sizeof(arr_tbuf), "%s%02d:%02d+%d",
                         est, ta.tm_hour, ta.tm_min, delta_days);
            else
                snprintf(arr_tbuf, sizeof(arr_tbuf), "%s%02d:%02d", est, ta.tm_hour, ta.tm_min);
        } else {
            snprintf(arr_tbuf, sizeof(arr_tbuf), "%s%02d:%02d", est, ta.tm_hour, ta.tm_min);
        }
    }
    uint16_t time_col = no_signal ? c_meta : c_row0;
    String dep_ts = dep_tbuf, arr_ts = arr_tbuf;
    draw_text(dep_ts, PAD, TIME_Y, 2, time_col);
    draw_text(arr_ts, SCREEN_W - PAD - text_w(arr_ts, 2), TIME_Y, 2, time_col);

    // Route line + plane (inline with airport codes at BAR_CY)
    const int BAR_LX = PAD + dep_w + 12;
    const int BAR_RX = SCREEN_W - PAD - arr_w - 12;
    const int BAR_W  = BAR_RX - BAR_LX;

    if (no_signal) {
        if (BAR_W > 20)
            gfx->drawFastHLine(BAR_LX, BAR_CY, BAR_W, c_meta);
        draw_text(status_str,
                  (SCREEN_W - text_w(status_str, 2)) / 2, STATUS_Y, 2, c_meta);
        String nd = fi.dep_date.length() > 0
                    ? "No live data \x7E " + fi.dep_date
                    : "No live data";
        draw_text(nd, (SCREEN_W - text_w(nd, 2)) / 2, STATUS_Y + 22, 2, c_meta);
        return;
    }

    // Progress: time-based when both timestamps known; elapsed-fraction fallback
    float progress = 0.0f;
    if (fi.dep_time > 0 && fi.arr_time > 0) {
        if      (now_t <= fi.dep_time) progress = 0.0f;
        else if (now_t >= fi.arr_time) progress = 1.0f;
        else progress = (float)(now_t - fi.dep_time) / (float)(fi.arr_time - fi.dep_time);
    } else if (fi.dep_time > 0 && fi.airborne) {
        progress = (float)(now_t - fi.dep_time) / (10.0f * 3600.0f);
        if (progress > 0.95f) progress = 0.95f;
    }

    if (BAR_W > 20) {
        gfx->drawFastHLine(BAR_LX, BAR_CY, BAR_W, c_meta);
        int plane_cx = BAR_LX + (int)(progress * BAR_W);
        // Clamp: tail reaches cx-18, nose tip reaches cx+27
        if (plane_cx < BAR_LX + 18) plane_cx = BAR_LX + 18;
        if (plane_cx > BAR_RX - 27) plane_cx = BAR_RX - 27;
        draw_plane_large(plane_cx, BAR_CY, c_row0);
    }

    draw_text(status_str,
              (SCREEN_W - text_w(status_str, 2)) / 2, STATUS_Y, 2, c_row0);

    // Alt / speed / heading (airborne only)
    if (fi.airborne && fi.alt_ft > 0) {
        char alt_buf[16], spd_buf[12], hdg_buf[6];
        snprintf(alt_buf, sizeof(alt_buf), "%dft",   (int)roundf(fi.alt_ft / 100.0f) * 100);
        snprintf(spd_buf, sizeof(spd_buf), "%dkm/h",  (int)roundf(fi.speed_kmh));
        snprintf(hdg_buf, sizeof(hdg_buf), "%s",       heading_label(fi.heading));
        String alt_s = alt_buf, spd_s = spd_buf, hdg_s = hdg_buf;
        int gap = 22;
        int total_w = text_w(alt_s, 2) + gap + text_w(spd_s, 2) + gap + text_w(hdg_s, 2);
        int sx = (SCREEN_W - total_w) / 2;
        draw_text(alt_s, sx, STATS_Y, 2, c_row0);  sx += text_w(alt_s, 2) + gap;
        draw_text(spd_s, sx, STATS_Y, 2, c_row0);  sx += text_w(spd_s, 2) + gap;
        draw_text(hdg_s, sx, STATS_Y, 2, c_row0);
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

    // Render off-screen and push the finished frame in one go — the old
    // clear-then-draw flashed the whole screen once a minute.
    FrameBuffered fb;
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
            gfx->fillRect(dots_x + i * 12, dot_y, 6, 6, (i == slot) ? c_row0 : c_meta);
        String cs = fi.callsign.length() > 0 ? fi.callsign : "Flight";
        draw_plane_right(PAD + 7, HEADER_H / 2, c_row0);
        draw_text(cs,         PAD + 18, (HEADER_H - 16) / 2, 2, c_row0);
        draw_text(clock_str,  clock_x,  (HEADER_H - 16) / 2, 2, c_row0);
        gfx->drawFastHLine(0, HEADER_H - 1, SCREEN_W, c_meta);
    }

    draw_flight_content(now_t, fi);
    draw_flight_footer(now_t, fi, wifi_ok);
}

// ── Theme switching ───────────────────────────────────────────────────────────
void display_set_theme(uint16_t row0, uint16_t rows, uint16_t dim, uint16_t meta) {
    c_row0 = TRAIL[0] = row0;
    c_rows = TRAIL[1] = rows;
    c_dim  = TRAIL[2] = dim;
    c_meta = TRAIL[3] = meta;
}

// ── Display orientation ───────────────────────────────────────────────────────
// rotation 3 = landscape, USB-C left (normal)
// rotation 1 = landscape, USB-C right (flipped 180°)
void display_set_flipped(bool flipped) {
    if (!gfx) return;
    gfx->setRotation(flipped ? 1 : 3);
    gfx->fillScreen(BLACK);   // clear stale content before next frame renders
    display_invalidate();
}

// ── Boot animation — runs as a FreeRTOS task so setup() can proceed alongside ──
static volatile bool  s_anim_running      = false;
static TaskHandle_t   s_anim_task_handle  = nullptr;

static void anim_task_fn(void*) {
    const int CW   = MATRIX_FONT_W;   // 8
    const int CH   = MATRIX_FONT_H;   // 17
    const int ASC  = MATRIX_FONT_ASC; // 14
    const int COLS = SCREEN_W / CW;   // 60
    const int ROWS = SCREEN_H / CH;   // 13

    gfx->setFont(&MatrixCode);
    gfx->setTextSize(1);
    gfx->fillScreen(BLACK);

    int8_t   heads[64];
    uint32_t col_next[64];   // abs millis when each column should advance
    uint8_t  col_extra[64];  // random extra ms above base 60ms (range 0–20 → 60–80ms)

    {
        uint32_t t0 = millis();
        for (int i = 0; i < COLS && i < 64; i++) {
            heads[i]     = (int8_t)(-random(ROWS * 2));
            col_extra[i] = (uint8_t)random(36);  // 0–35 → interval 52–87ms (±25% of 70ms)
            col_next[i]  = t0 + random(87);      // stagger first fire
        }
    }

    while (s_anim_running) {
        uint32_t now = millis();

        bool any_due = false;
        for (int c = 0; c < COLS && c < 64; c++) {
            if ((int32_t)(now - col_next[c]) >= 0) { any_due = true; break; }
        }
        if (!any_due) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }

        gfx->setFont(&MatrixCode);
        gfx->setTextSize(1);

        for (int c = 0; c < COLS && c < 64; c++) {
            if ((int32_t)(now - col_next[c]) < 0) continue;

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
                gfx->setTextColor(0xFFFF, BLACK);  // white head
                gfx->setCursor(x, heads[c] * CH + ASC);
                gfx->print((char)(33 + random(94)));
            }
            if (++heads[c] > ROWS + 6) {
                heads[c]     = (int8_t)(-random(ROWS / 2));
                col_extra[c] = (uint8_t)random(36);
            }
            col_next[c] = now + 52 + col_extra[c];
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

// ── Pager UI ──────────────────────────────────────────────────────────────────

static void pager_draw_header(const char* title) {
    gfx->fillScreen(BLACK);
    String t = to_latin1(title);
    int tw = text_w(t, 2);
    draw_text(t, (SCREEN_W - tw) / 2, (HEADER_H - 16) / 2, 2, c_row0);
    gfx->drawFastHLine(0, HEADER_H - 1, SCREEN_W, c_meta);
}

// Pixel-art icons for the 10 activity slots.
// cx/cy = center of the icon drawing area.
static void pager_icon(int idx, int cx, int cy, uint16_t col) {
    switch (idx) {
    case 0: // Beer mug
        gfx->fillRect(cx - 8, cy - 9, 16, 20, col);          // mug body
        gfx->drawRect(cx + 8, cy - 3, 7, 10, col);            // handle
        gfx->fillRect(cx - 9, cy - 13, 18, 5, col);           // foam
        break;
    case 1: // Coffee cup + saucer + steam
        gfx->fillRect(cx - 8, cy - 5, 16, 15, col);           // cup
        gfx->fillRect(cx - 11, cy + 10, 22, 3, col);          // saucer
        gfx->drawFastVLine(cx - 4, cy - 10, 5, col);          // steam left
        gfx->drawFastVLine(cx,     cy - 12, 5, col);           // steam mid
        gfx->drawFastVLine(cx + 4, cy - 10, 5, col);          // steam right
        break;
    case 2: // Movie clapperboard
        gfx->fillRect(cx - 12, cy - 2, 24, 15, col);          // board body
        gfx->fillRect(cx - 12, cy - 9, 24, 8, col);           // top strip
        for (int i = 0; i < 4; i++)
            gfx->fillRect(cx - 11 + i * 6, cy - 9, 3, 8, BLACK); // stripe gaps
        break;
    case 3: // Gym barbell
        gfx->fillRect(cx - 14, cy - 2, 28, 4, col);           // bar
        gfx->fillRect(cx - 19, cy - 8, 6, 16, col);           // left plate
        gfx->fillRect(cx + 13, cy - 8, 6, 16, col);           // right plate
        break;
    case 4: // Running stick figure
        gfx->fillCircle(cx + 4, cy - 11, 4, col);             // head
        gfx->drawLine(cx + 2, cy - 7, cx, cy + 2, col);       // torso
        gfx->drawLine(cx + 1, cy - 4, cx - 7, cy,     col);   // arm back
        gfx->drawLine(cx + 1, cy - 4, cx + 8, cy - 8, col);   // arm fwd
        gfx->drawLine(cx,     cy + 2, cx - 7, cy + 10, col);  // leg back
        gfx->drawLine(cx,     cy + 2, cx + 7, cy + 8,  col);  // leg fwd
        break;
    case 5: // Bicycle
        gfx->drawCircle(cx - 11, cy + 4, 9, col);             // rear wheel
        gfx->drawCircle(cx + 11, cy + 4, 9, col);             // front wheel
        gfx->drawLine(cx - 11, cy + 4, cx,      cy - 5, col); // rear frame
        gfx->drawLine(cx,      cy - 5, cx + 11, cy + 4, col); // front fork
        gfx->drawLine(cx - 11, cy + 4, cx,      cy + 4, col); // chain stay
        gfx->drawLine(cx,      cy + 4, cx,      cy - 5, col); // seat tube
        gfx->drawFastHLine(cx + 9, cy - 3, 6, col);           // handlebar
        break;
    case 6: { // Classic digital phone (Nokia-style: body + antenna + screen + keypad)
        // Antenna (top-right, slightly angled)
        gfx->drawLine(cx + 5, cy - 20, cx + 7, cy - 12, col);
        gfx->drawLine(cx + 6, cy - 20, cx + 8, cy - 12, col);
        // Phone body
        gfx->fillRect(cx - 8, cy - 12, 16, 26, col);
        // Screen (dark inset, top portion)
        gfx->fillRect(cx - 5, cy - 10, 10, 7, BLACK);
        // Earpiece slit on screen
        gfx->fillRect(cx - 2, cy - 9, 5, 1, col);
        // Keypad: 3×4 grid of dark button squares
        for (int r = 0; r < 4; r++) {
            for (int c2 = 0; c2 < 3; c2++) {
                gfx->fillRect(cx - 5 + c2 * 4, cy - 1 + r * 4, 2, 2, BLACK);
            }
        }
        break;
    }
    case 7: // Dinner fork + knife
        gfx->fillRect(cx - 8, cy - 13, 3, 26, col);           // fork handle
        gfx->fillRect(cx - 10, cy - 13, 2, 9, col);           // left prong
        gfx->fillRect(cx - 6,  cy - 13, 2, 9, col);           // right prong
        gfx->fillRect(cx + 5, cy - 13, 3, 26, col);           // knife blade+handle
        gfx->fillTriangle(cx + 4, cy - 13, cx + 8, cy - 13, cx + 8, cy + 2, col); // taper
        break;
    case 8: // Lunch bowl
        gfx->fillCircle(cx, cy + 4, 12, col);                 // full circle
        gfx->fillRect(cx - 13, cy - 8, 26, 12, BLACK);        // erase top → bowl shape
        gfx->fillRect(cx - 13, cy + 15, 26, 3, col);          // base line
        break;
    case 9: // Heart (Love)
        gfx->fillCircle(cx - 5, cy - 5, 7, col);
        gfx->fillCircle(cx + 5, cy - 5, 7, col);
        gfx->fillTriangle(cx - 11, cy, cx + 11, cy, cx, cy + 11, col);
        break;
    default:
        break;
    }
}

// icon_idx >= 0: draw pixel-art icon + small label; -1: text-only tile (time selector)
static void pager_draw_tile(int x, int y, int w, int h,
                             const char* label, int icon_idx, bool selected) {
    uint16_t border = selected ? c_row0 : c_meta;
    uint16_t text_c = selected ? c_row0 : c_dim;
    gfx->drawRect(x + 2, y + 2, w - 4, h - 4, border);
    if (selected) gfx->drawRect(x + 3, y + 3, w - 6, h - 6, border);
    if (icon_idx >= 0) {
        uint16_t icon_c = selected ? c_row0 : c_rows;
        bool has_label = (label && label[0] != '\0');
        int icon_cy = has_label ? (y + h / 2 - 8) : (y + h / 2);
        pager_icon(icon_idx, x + w / 2, icon_cy, icon_c);
        if (has_label) {
            String s = to_latin1(label);
            int tw = text_w(s, 2);
            draw_text(s, x + (w - tw) / 2, y + h - 22, 2, text_c);
        }
    } else {
        String s = to_latin1(label);
        int tw = text_w(s, 2);
        draw_text(s, x + (w - tw) / 2, y + (h - 16) / 2, 2, text_c);
    }
}

void display_draw_pager_activities(const char* const acts[], int highlighted) {
    pager_draw_header("Send a message");
    int content_h = SCREEN_H - HEADER_H;
    int tile_w = SCREEN_W / 5;
    int tile_h = content_h / 2;
    for (int i = 0; i < 10; i++) {
        int col = i % 5;
        int row = i / 5;
        pager_draw_tile(col * tile_w, HEADER_H + row * tile_h,
                        tile_w, tile_h, acts[i], i, i == highlighted);
    }
}

void display_draw_pager_times(const char* activity_label,
                               const char* const times[], int highlighted) {
    char hdr[32];
    snprintf(hdr, sizeof(hdr), "%s — when?", activity_label);
    pager_draw_header(hdr);
    int content_h = SCREEN_H - HEADER_H;
    int tile_w = SCREEN_W / 3;
    int tile_h = content_h / 2;
    for (int i = 0; i < 6; i++) {
        int col = i % 3;
        int row = i / 3;
        pager_draw_tile(col * tile_w, HEADER_H + row * tile_h,
                        tile_w, tile_h, times[i], -1, i == highlighted);
    }
}

void display_draw_pager_friends(const char* header,
                                 const char* const names[], int n, int highlighted) {
    pager_draw_header(header);
    if (n == 0) {
        String msg = "No friends configured";
        int tw = text_w(msg, 2);
        draw_text(msg, (SCREEN_W - tw) / 2, HEADER_H + 70, 2, c_dim);
        return;
    }
    int content_h = SCREEN_H - HEADER_H;
    int rows  = (n <= 3) ? 1 : 2;
    int tile_w = SCREEN_W / 3;
    int tile_h = content_h / rows;
    for (int i = 0; i < n; i++) {
        int col = i % 3;
        int row = i / 3;
        pager_draw_tile(col * tile_w, HEADER_H + row * tile_h,
                        tile_w, tile_h, names[i], -1, i == highlighted);
    }
}

void display_draw_pager_incoming(const char* header,
                                  const char* text,
                                  int icon_idx,
                                  time_t sent_at,
                                  bool show_replies,
                                  int highlighted,
                                  const char* re_text,
                                  int re_icon_idx) {
    // ── Header bar ─────────────────────────────────────────────────────────────
    gfx->fillScreen(BLACK);
    String hdr = to_latin1(header);
    draw_text(hdr, PAD, (HEADER_H - 16) / 2, 2, c_row0);

    if (sent_at > 0) {
        int elapsed = (int)((time(nullptr) - sent_at) / 60);
        char buf[16];
        // Avoid 'j' — "just now" inflates text_w in TramliSmall due to bounding-box quirk
        if (elapsed < 1)       snprintf(buf, sizeof(buf), "now");
        else if (elapsed == 1) snprintf(buf, sizeof(buf), "1 min ago");
        else                   snprintf(buf, sizeof(buf), "%d min ago", elapsed);
        String sb = buf;
        int stw = text_w(sb, 2);
        draw_text(sb, SCREEN_W - stw - PAD, (HEADER_H - 16) / 2, 2, c_dim);
    }
    gfx->drawFastHLine(0, HEADER_H - 1, SCREEN_W, c_meta);

    int btn_y    = SCREEN_H - 70;
    int content_top = HEADER_H;

    // ── Quote block — original question shown above a reply ───────────────────
    bool has_quote = (re_text && re_text[0]);
    if (has_quote) {
        int qy = HEADER_H + 7;
        gfx->fillRect(PAD, HEADER_H + 4, 3, 22, c_meta);  // left amber bar
        String qs = to_latin1(re_text);
        int max_qw = SCREEN_W - PAD - 14 - PAD;
        if (text_w(qs, 2) > max_qw) {
            int ddw = text_w("..", 2);
            while (qs.length() > 1 && text_w(qs, 2) > max_qw - ddw)
                qs = qs.substring(0, qs.length() - 1);
            qs += "..";
        }
        draw_text(qs, PAD + 14, qy, 2, c_dim);
        gfx->drawFastHLine(0, HEADER_H + 32, SCREEN_W, c_meta);
        content_top = HEADER_H + 34;
    }

    // ── Icon + message text ─────────────────────────────────────────────────────
    // For reply messages (has_quote): show the original question's icon as context.
    // For invite messages: show the activity icon as before.
    int display_icon = has_quote ? re_icon_idx : icon_idx;

    String s = to_latin1(text);
    int max_w = SCREEN_W - 2 * PAD;
    int text_y;
    if (display_icon >= 0) {
        pager_icon(display_icon, SCREEN_W / 2, content_top + 26, c_rows);
        text_y = content_top + 58;
    } else {
        int msg_h = btn_y - content_top;
        int tw = text_w(s, 2);
        text_y = content_top + (msg_h - (tw > max_w ? 32 : 16)) / 2;
    }
    int tw = text_w(s, 2);
    if (tw <= max_w) {
        draw_text(s, (SCREEN_W - tw) / 2, text_y, 2, c_row0);
    } else {
        int mid = s.length() / 2;
        int sp  = s.lastIndexOf(' ', mid);
        if (sp < 0) sp = s.indexOf(' ', mid);
        if (sp > 0) {
            String l1 = s.substring(0, sp), l2 = s.substring(sp + 1);
            int tw1 = text_w(l1, 2), tw2 = text_w(l2, 2);
            draw_text(l1, (SCREEN_W - tw1) / 2, text_y,      2, c_row0);
            draw_text(l2, (SCREEN_W - tw2) / 2, text_y + 20, 2, c_row0);
        } else {
            draw_text(s, PAD, text_y, 2, c_row0);
        }
    }

    // ── Button row ─────────────────────────────────────────────────────────────
    gfx->drawFastHLine(0, btn_y, SCREEN_W, c_meta);
    if (show_replies) {
        // 4 buttons: Yes / No / Later / Close
        static const char* BTNS[] = { "Yes", "No", "Later", "Close" };
        int btn_w = SCREEN_W / 4;
        for (int i = 0; i < 4; i++) {
            bool sel = (i == highlighted);
            int bx = i * btn_w;
            if (sel) gfx->fillRect(bx, btn_y, btn_w, 70, c_meta >> 1);
            if (i > 0) gfx->drawFastVLine(bx, btn_y, 70, c_meta);
            String bs = to_latin1(BTNS[i]);
            int btw = text_w(bs, 2);
            draw_text(bs, bx + (btn_w - btw) / 2, btn_y + (70 - 16) / 2, 2,
                      sel ? c_row0 : c_rows);
        }
    } else {
        // Single full-width Close (reply already sent or not applicable)
        if (highlighted == 0) gfx->fillRect(0, btn_y, SCREEN_W, 70, c_meta >> 1);
        String cl = "Close";
        int ctw = text_w(cl, 2);
        draw_text(cl, (SCREEN_W - ctw) / 2, btn_y + (70 - 16) / 2, 2,
                  (highlighted == 0) ? c_row0 : c_rows);
    }
}
