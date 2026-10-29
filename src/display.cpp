// ─────────────────────────────────────────────────────────────────────────────
// display.cpp — Display rendering using Arduino_GFX
// ─────────────────────────────────────────────────────────────────────────────
// We use Arduino_GFX (not TFT_eSPI) because the T-Display S3 Pro's panel
// has a 49-pixel column offset that TFT_eSPI doesn't expose.
// ─────────────────────────────────────────────────────────────────────────────

#include "display.h"
#include "../include/devlog.h"
#include "../include/config.h"
#include "../include/tramli_fonts.h"
#include "../include/matrix_font.h"
#include "../include/flap_digits.h"
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
// What a portfolio cell shows; status -1 = needs paint. session: market dot state.
struct StockCellState { int8_t status; float price; float pct; int8_t session; String currency; };
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
// Letters beyond Latin-1 from Central/Eastern Europe (Latin Extended-A:
// Polish ł ś ą ż, Czech č ř š, Hungarian ő …) become their base letter —
// "Łódź" → "Lódz" reads better than "?ód?".
static const char EXT_A_BASE[] =          // U+0100–U+017F, 16 per row
    "AaAaAaCcCcCcCcDd" "DdEeEeEeEeEeGgGg" "GgGgHhHhIiIiIiIi" "IiIiJjKkkLlLlLlL"
    "lLlNnNnNnnNnOoOo" "OoOoRrRrRrSsSsSs" "SsTtTtTtUuUuUuUu" "UuUuWwYyYZzZzZzs";

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
            // U+00C0–U+00FF: À…ÿ (ä ö ü é ó ñ ç å …) — all in the font
            out += (char)((uint8_t)src[i + 1] + 0x40);
            i += 2;
        } else if ((b == 0xC4 || b == 0xC5) && i + 1 < src.length()) {
            out += EXT_A_BASE[((b - 0xC4) << 6) | ((uint8_t)src[i + 1] & 0x3F)];
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

    gfx->begin(DISPLAY_SPI_HZ);
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
    Log.println("[Display] Initialized (rotation=3, ips=true)");
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

// ── Train icon (same footprint as the bus) ───────────────────────────────────
static void draw_train_icon(int x, int y, uint16_t color) {
    gfx->fillRect(x + 3,  y + 1,  22, 1,  color);  // rounded roof
    gfx->fillRect(x + 1,  y + 2,  26, 1,  color);
    gfx->fillRect(x,      y + 3,  28, 21, color);  // body
    gfx->fillRect(x + 3,  y + 5,  22, 9,  BLACK);  // big windshield
    gfx->fillRect(x + 12, y + 15, 4,  2,  BLACK);  // top light
    gfx->fillRect(x + 3,  y + 18, 5,  3,  BLACK);  // left headlight
    gfx->fillRect(x + 20, y + 18, 5,  3,  BLACK);  // right headlight
    gfx->fillRect(x - 2,  y + 24, 32, 2,  color);  // buffer beam
    gfx->fillRect(x + 3,  y + 26, 4,  1,  color);  // bogies
    gfx->fillRect(x + 21, y + 26, 4,  1,  color);
    gfx->fillRect(x - 4,  y + 27, 36, 1,  color);  // rail
}

// ── Tram icon (same footprint as the bus) ────────────────────────────────────
static void draw_tram_icon(int x, int y, uint16_t color) {
    gfx->fillRect(x + 6,  y,      16, 1,  color);  // pantograph
    gfx->fillRect(x + 8,  y + 1,  2,  1,  color);
    gfx->fillRect(x + 18, y + 1,  2,  1,  color);
    gfx->fillRect(x + 10, y + 2,  2,  1,  color);
    gfx->fillRect(x + 16, y + 2,  2,  1,  color);
    gfx->fillRect(x + 12, y + 3,  4,  2,  color);
    gfx->fillRect(x + 1,  y + 5,  26, 1,  color);  // roof
    gfx->fillRect(x,      y + 6,  28, 19, color);  // body
    gfx->fillRect(x + 3,  y + 7,  22, 3,  BLACK);  // destination sign
    gfx->fillRect(x + 3,  y + 11, 22, 7,  BLACK);  // windshield
    gfx->fillRect(x + 3,  y + 20, 5,  2,  BLACK);  // left headlight
    gfx->fillRect(x + 20, y + 20, 5,  2,  BLACK);  // right headlight
    gfx->fillRect(x + 2,  y + 25, 6,  2,  color);  // wheels
    gfx->fillRect(x + 20, y + 25, 6,  2,  color);
    gfx->fillRect(x - 4,  y + 27, 36, 1,  color);  // rail
}

// "Leaving now" icon for the kind of vehicle
static void draw_vehicle_icon(Vehicle v, int x, int y, uint16_t color) {
    switch (v) {
        case VEHICLE_TRAIN: draw_train_icon(x, y, color); break;
        case VEHICLE_TRAM:  draw_tram_icon(x, y, color);  break;
        default:            draw_bus_icon(x, y, color);   break;
    }
}

// ── Day / night brightness ───────────────────────────────────────────────────
// Day and night each have their own level, cycled by the brightness button and
// saved separately — adjusting at night no longer changes the daytime level.
// With a light sensor, the cycle starts with "auto" (level 0): the backlight
// follows the room light — at night never brighter than the top night level.
static const uint8_t AUTO = 0;
static const uint8_t DAY_LEVELS[]   = { AUTO, 100, 80, 60, 40, 20 };
static const uint8_t NIGHT_LEVELS[] = { AUTO, 30, 20, 15, 10, 5 };
static uint8_t s_day_brightness    = DAY_BRIGHTNESS;
static uint8_t s_night_brightness  = NIGHT_BRIGHTNESS;
static bool    s_last_night_state  = false;  // last known night/day, to detect transitions
static bool    s_screen_on         = true;
static int     s_night_start_min   = NIGHT_START_HOUR * 60;  // minutes after midnight
static int     s_night_end_min     = NIGHT_END_HOUR * 60;
static bool    s_have_sensor       = false;
static bool    s_saved_day = false, s_saved_night = false;   // levels came from NVS
static float   s_auto_pct          = -1;     // smoothed automatic level; -1 = no reading yet
static int     s_auto_applied      = -1;     // last automatic level sent to the backlight
static bool    s_auto_paused       = false;  // an alert flash owns the backlight

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

// Backlight level for the current day/night state, resolving "auto"
static uint8_t current_level() {
    bool night = display_is_night();
    uint8_t level = night ? s_night_brightness : s_day_brightness;
    if (level != AUTO) return level;
    float p = (s_auto_pct >= 0) ? s_auto_pct : (night ? NIGHT_BRIGHTNESS : DAY_BRIGHTNESS);
    if (night && p > NIGHT_LEVELS[1]) p = NIGHT_LEVELS[1];
    return (uint8_t)lroundf(p);
}

// Re-apply the level for the current day/night state (after a pulse, wake, …)
void display_restore_brightness() {
    if (!s_screen_on) return;
    uint8_t level = current_level();
    s_auto_applied = level;
    display_set_brightness(level);
}

// Called once per second from every draw function. Switches level only at the
// day/night transition so manual adjustments stick until the next transition.
static void apply_night_brightness() {
    bool night = display_is_night();
    if (night != s_last_night_state) {
        s_last_night_state = night;
        display_restore_brightness();
        Log.printf("[Display] Auto brightness: %s mode\n", night ? "night" : "day");
    }
}

void display_init_brightness() {
    Preferences prefs;
    prefs.begin("tramli", true);
    s_saved_day   = prefs.isKey("bright_day");
    s_saved_night = prefs.isKey("bright_night");
    int d = prefs.getInt("bright_day",   DAY_BRIGHTNESS);
    int n = prefs.getInt("bright_night", NIGHT_BRIGHTNESS);
    prefs.end();
    if (d >= 0 && d <= 100) s_day_brightness   = (uint8_t)d;
    if (n >= 0 && n <= 100) s_night_brightness = (uint8_t)n;
    // "auto" needs the sensor — known after display_enable_auto_brightness()
    if (s_day_brightness   == AUTO) s_day_brightness   = DAY_BRIGHTNESS;
    if (s_night_brightness == AUTO) s_night_brightness = NIGHT_BRIGHTNESS;
    s_last_night_state = display_is_night();
    display_restore_brightness();
    Log.printf("[Display] Brightness loaded: day %d%%, night %d%% (%s)\n",
                  s_day_brightness, s_night_brightness, s_last_night_state ? "night" : "day");
}

void display_enable_auto_brightness() {
    s_have_sensor = true;
    // Saved "auto", or never adjusted → automatic. The first start with this
    // feature switches both to auto once (levels picked before had no "auto"
    // to choose); later choices with the button stick.
    Preferences prefs;
    prefs.begin("tramli", false);
    if (!prefs.isKey("auto_intro")) {
        prefs.putInt("bright_day", AUTO);
        prefs.putInt("bright_night", AUTO);
        prefs.putBool("auto_intro", true);
    }
    if (!s_saved_day   || prefs.getInt("bright_day",   -1) == AUTO) s_day_brightness   = AUTO;
    if (!s_saved_night || prefs.getInt("bright_night", -1) == AUTO) s_night_brightness = AUTO;
    prefs.end();
    Log.printf("[Display] Light sensor: day %s, night %s\n",
               s_day_brightness == AUTO ? "auto" : String(s_day_brightness).c_str(),
               s_night_brightness == AUTO ? "auto" : String(s_night_brightness).c_str());
}

bool display_auto_brightness() {
    return (display_is_night() ? s_night_brightness : s_day_brightness) == AUTO;
}

void display_pause_auto_brightness(bool paused) { s_auto_paused = paused; }

// Room light → backlight, on a log scale (the eye's response). The sensor sits
// behind the glass and sees only a fraction of the light, so its "lux" is far
// below the room's: measured 0.05 under a lamp in the evening, ~70 with a phone
// flashlight on it. Mapped from AMB_DARK (and below: 8 %) to AMB_BRIGHT (100 %):
// lamp-lit room ~43 %, daylight indoors ~75 %. Smoothed, so a passing shadow
// doesn't flicker the screen.
static const float AMB_DARK   = 0.002f;
static const float AMB_BRIGHT = 10.0f;

void display_set_ambient_light(float lux) {
    if (lux < 0) return;
    float pos = (log10f(fmaxf(lux, AMB_DARK)) - log10f(AMB_DARK)) /
                (log10f(AMB_BRIGHT) - log10f(AMB_DARK));
    float target = 8.0f + 92.0f * fminf(pos, 1.0f);
    s_auto_pct = (s_auto_pct < 0) ? target : s_auto_pct + (target - s_auto_pct) * 0.3f;
    if (!s_screen_on || s_auto_paused || !display_auto_brightness()) return;
    int level = current_level();
    if (abs(level - s_auto_applied) < 2) return;   // ignore tiny changes
    s_auto_applied = level;
    display_set_brightness(level);
}

// Next level in the cycle (wraps); unknown levels jump to the first entry.
// Without a sensor, "auto" is skipped.
static uint8_t next_level(const uint8_t* levels, int n, uint8_t cur) {
    int start = s_have_sensor ? 0 : 1;
    for (int i = start; i < n; i++)
        if (levels[i] == cur) return levels[i + 1 < n ? i + 1 : start];
    return levels[start];
}

uint8_t display_step_brightness() {
    bool night = display_is_night();
    uint8_t& level = night ? s_night_brightness : s_day_brightness;
    level = night ? next_level(NIGHT_LEVELS, sizeof(NIGHT_LEVELS), level)
                  : next_level(DAY_LEVELS,   sizeof(DAY_LEVELS),   level);
    display_restore_brightness();
    Preferences prefs;
    prefs.begin("tramli", false);
    prefs.putInt(night ? "bright_night" : "bright_day", level);
    prefs.end();
    Log.printf("[Display] %s brightness → %s\n", night ? "Night" : "Day",
               level == AUTO ? "auto" : (String(level) + "%").c_str());
    return level;
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
    Log.printf("[Display] Screen %s\n", on ? "on" : "off");
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
        h = (h ^ (uint32_t)shown[i].vehicle) * 16777619u;
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

        // Time or vehicle icon
        int mins = departure_minutes(dep, now);
        if (mins <= 0) {
            draw_vehicle_icon(dep.vehicle, SCREEN_W - PAD - 32, text_y - 2, c_row0);
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
// ── Gesture hint ─────────────────────────────────────────────────────────────
// While a hint is showing it takes over the footer of whatever page is on
// screen (every page draws its footer through the two functions below).
static String   s_hint;
static uint32_t s_hint_until = 0;

static bool hint_active() { return s_hint.length() > 0; }

void display_show_hint(const char* text, uint32_t duration_ms) {
    s_hint       = to_latin1(text);
    s_hint_until = millis() + duration_ms;
    display_invalidate();   // footers repaint with the hint
}

void display_hint_tick() {
    if (hint_active() && (int32_t)(millis() - s_hint_until) >= 0) {
        s_hint = "";
        display_invalidate();   // and back to the normal footer
    }
}

static void draw_footer_static(int fy, const char* weather_str, const char* uv_str,
                                bool rain_today, int rain_pct,
                                bool snow_today, bool clear_today,
                                bool large_font) {
    if (hint_active()) {
        gfx->fillRect(0, fy, SCREEN_W, FOOTER_H, BLACK);
        gfx->drawFastHLine(0, fy, SCREEN_W, c_meta);
        draw_text_safe(s_hint, std::max(PAD, (SCREEN_W - text_w(s_hint, 2)) / 2),
                       fy + (FOOTER_H - 16) / 2, 2, c_dim, 0, SCREEN_W);
        return;
    }
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
    if (hint_active()) return;   // the hint uses the whole footer
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
                draw_vehicle_icon(leg.vehicle, SCREEN_W - PAD - 32, text_y - 2, c_row0);
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
                draw_vehicle_icon(leg.vehicle, SCREEN_W - PAD - 32, text_y - 2, c_row0);
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

// ── Portfolio cells ──────────────────────────────────────────────────────────
// Columns fit the page like a table: the % and price columns get exactly the
// width of their widest entry on the page, the ticker name gets the rest, and
// the mini chart takes whatever is left in between (none if there's no room).
// Columns stay aligned from row to row.

struct StockText {
    String   sym, price, cur, pct;     // pct "" = none (no price yet)
    uint16_t sym_col, price_col, pct_col;
};

static const int CUR_GAP = 8;          // between price and currency

static StockText stock_text(const StockQuote& q) {
    StockText t;
    t.sym     = to_latin1(stocks_display_symbol(q.symbol));
    t.sym_col = (q.status == STOCK_NOT_FOUND) ? c_dim : c_row0;
    if (q.status == STOCK_OK)             { t.price = format_stock_price(q.price); t.price_col = c_row0; }
    else if (q.status == STOCK_NOT_FOUND) { t.price = "n/a";                       t.price_col = c_dim;  }
    else                                  { t.price = "--";                        t.price_col = c_meta; }
    if (q.status != STOCK_OK) return t;
    t.cur = q.currency;
    // Colour by the value as displayed, so "-0.00%" never shows up in red
    long hundredths = lroundf(q.change_pct * 100.0f);
    char pbuf[12];
    if (hundredths == 0) {
        snprintf(pbuf, sizeof(pbuf), "0.00%%");
        t.pct_col = c_dim;
    } else {
        snprintf(pbuf, sizeof(pbuf), "%+.2f%%", hundredths / 100.0f);
        t.pct_col = (hundredths > 0) ? COLOR_STOCK_UP : COLOR_STOCK_DOWN;
    }
    t.pct = pbuf;
    return t;
}

// Price + currency as drawn
static int price_block_w(const StockText& t, uint8_t fsz) {
    return text_w(t.price, fsz) + (t.cur.length() ? CUR_GAP + text_w(t.cur, 2) : 0);
}

// Widest entry of each column on the page. show_cur = false: the currency
// labels are left out on this page so the names fit (compact layout); the
// currency still shows in the big layout and the detail view.
struct StockCols {
    int  sym_w = 0, price_w = 0, pct_w = 0;
    bool show_cur = true;
    bool operator!=(const StockCols& o) const {
        return sym_w != o.sym_w || price_w != o.price_w || pct_w != o.pct_w || show_cur != o.show_cur;
    }
};
static StockCols s_stock_cols;

// Room in a cell from the ticker name to the right edge (the dot is in front)
static int stock_text_room(bool big) {
    return big ? SCREEN_W - 2 * PAD - 16 : SCREEN_W / 2 - (PAD + 4) - PAD - 12;
}

static StockCols measure_stock_cols(const StockQuote quotes[], int first, int n, bool big) {
    const uint8_t fsz = big ? 3 : 2;
    const int     gap = big ? 16 : 8;
    StockCols c;
    int price_only = 0;
    for (int i = first; i < first + n; i++) {
        StockText t = stock_text(quotes[i]);
        c.sym_w    = std::max(c.sym_w,    (int)text_w(t.sym, fsz));
        c.price_w  = std::max(c.price_w,  price_block_w(t, fsz));
        c.pct_w    = std::max(c.pct_w,    (int)text_w(t.pct, fsz));
        price_only = std::max(price_only, (int)text_w(t.price, fsz));
    }
    int needed = c.sym_w + 8 + c.price_w + (c.pct_w > 0 ? gap + c.pct_w : 0);
    if (needed > stock_text_room(big)) {   // names first: drop the currency labels
        c.show_cur = false;
        c.price_w  = price_only;
    }
    return c;
}

// Repaints one cell. Bottom pixel row is left alone (row separator lives there).
// session: 1 = its market is trading (green dot), 0 = closed (red), -1 = unknown (none)
static void draw_stock_cell(int slot, int per_page, const StockQuote& q, int session,
                            const StockCols& cols) {
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
    const int     gap    = big ? 16 : 8;                    // between columns
    StockText t = stock_text(q);

    // Right to left: % change, then price + currency, each column as wide as
    // its widest entry on the page
    const int price_block_right = right - (cols.pct_w > 0 ? cols.pct_w + gap : 0);
    const int price_block_left  = price_block_right - cols.price_w;

    if (t.pct.length()) draw_text(t.pct, right - text_w(t.pct, fsz), text_y, fsz, t.pct_col);

    // Currency at the right of its column, price just before it
    int cur_w = (cols.show_cur && t.cur.length()) ? text_w(t.cur, 2) : 0;
    if (cur_w) draw_text(t.cur, price_block_right - cur_w,
                         text_y + (big ? LARGE_ASCENT - SMALL_ASCENT : 0), 2, c_dim);
    int price_right = price_block_right - (cur_w ? cur_w + CUR_GAP : 0);
    draw_text(t.price, price_right - text_w(t.price, fsz), text_y, fsz, t.price_col);

    // Market dot in front of the ticker: trading now = filled green dot, closed =
    // red ring — the shape tells them apart without colour (red-green colour blindness)
    const int dot_r    = big ? 4 : 3;
    const int sym_left = left + 2 * dot_r + (big ? 8 : 6);
    const int dot_cx   = left + dot_r, dot_cy = text_y + yadv / 2;
    if (session > 0) {
        gfx->fillCircle(dot_cx, dot_cy, dot_r, COLOR_STOCK_UP);
    } else if (session == 0) {
        gfx->drawCircle(dot_cx, dot_cy, dot_r, COLOR_STOCK_DOWN);
        if (big) gfx->drawCircle(dot_cx, dot_cy, dot_r - 1, COLOR_STOCK_DOWN);   // 2 px ring
    }

    // Name: everything up to the price column (shortened only if it really doesn't fit)
    String sym = t.sym;
    int sym_max = price_block_left - 8 - sym_left;
    while (sym.length() > 1 && text_w(sym, fsz) > sym_max)
        sym = sym.substring(0, sym.length() - 1);
    draw_text(sym, sym_left, text_y, fsz, t.sym_col);

    // Mini chart in what's left between the name column and the price column
    if (q.status != STOCK_OK) return;
    const int spark_x0 = sym_left + std::min(cols.sym_w, sym_max) + (big ? 14 : 8);
    const int spark_x1 = price_block_left - (big ? 14 : 8);
    const int spark_h  = big ? 18 : 14;
    draw_sparkline(q, spark_x0, spark_x1, y + (h - 1 - spark_h) / 2, spark_h, t.pct_col);
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

    // Column widths follow what's on the page; when they change, every cell moves
    StockCols cols = measure_stock_cols(quotes, first, on_page, per_page <= STOCK_ROWS);
    if (cols != s_stock_cols) {
        s_stock_cols = cols;
        for (auto& c : s_stock_cells) c.status = -1;
    }

    // Cells: repaint only those whose displayed values changed
    for (int s = 0; s < on_page; s++) {
        const StockQuote& q = quotes[first + s];
        StockCellState& c = s_stock_cells[s];
        int8_t session = (int8_t)stocks_session_state(q, now_t);   // dot turns at open/close
        if (c.status == (int8_t)q.status && c.price == q.price && c.pct == q.change_pct &&
            c.session == session && c.currency == q.currency) continue;
        c = { (int8_t)q.status, q.price, q.change_pct, session, q.currency };
        draw_stock_cell(s, per_page, q, session, cols);
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

            String ps = format_stock_price(q.price);
            draw_text(ps, PAD, top + 22, 4, c_row0);
            if (q.currency.length())   // small and dim, on the price's baseline
                draw_text(q.currency, PAD + text_w(ps, 4) + 10, top + 22 + XLARGE_ASCENT - SMALL_ASCENT,
                          2, c_dim);
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
                        const char* bottom_hint, const FooterInfo& f) {
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
    mix(bottom_hint ? bottom_hint : "");
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

    // Bottom line: what a tap does
    int bottom = SCREEN_H - 4;
    if (bottom_hint && *bottom_hint) {
        String h = to_latin1(bottom_hint);
        draw_text(h, (SCREEN_W - text_w(h, 2)) / 2, SCREEN_H - 19, 2, c_meta);
        bottom = SCREEN_H - 22;
    }

    // Quote of the day fills the space below the rows
    if (quote.length()) {
        int qy = top + n_rows * line_h + 6;
        gfx->drawFastHLine(PAD, qy - 5, SCREEN_W - 2 * PAD, c_meta);
        int max_lines = std::max(1, (bottom - qy) / 18 - 1);   // keep a line for the author
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
    if (esp_qrcode_generate(&cfg, text) != ESP_OK) Log.printf("[QR] failed for '%s'\n", text);
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

// ── Weather animations (rain / snow / thunderstorm) ──────────────────────────
// Two uses of the same falling-particle field:
//  • a pop-up: a few seconds over the whole screen with a message in the middle
//    ("Rain in 10 min"), then the page comes back (display_invalidate);
//  • the night clock's background while it rains/snows/storms — dimmer, fewer
//    particles, lightning without the sky flash (no light in the bedroom).
// Frames are drawn off-screen and sent in one go, so nothing flickers.
struct Particle { int16_t x, y; int8_t vx, vy; uint8_t size; };

// speed: 1 at ~25 frames/s; higher for slower frame rates (same speed on screen)
static void parts_init(Particle* ps, int n, WeatherAnim kind, float speed) {
    for (int i = 0; i < n; i++) {
        Particle& p = ps[i];
        p.x = random(-40, SCREEN_W);
        p.y = random(-SCREEN_H, SCREEN_H);
        if (kind == WX_ANIM_SNOW) {
            p.vx = 0; p.vy = (int8_t)(random(1, 4) * speed);
            p.size = (random(0, 6) == 0) ? 9 : random(1, 3);
        } else {
            bool storm = (kind == WX_ANIM_STORM);
            p.vx = (int8_t)((storm ? 4 : 2) * speed);
            p.vy = (int8_t)(random(storm ? 13 : 9, storm ? 19 : 15) * speed);
            p.size = random(7, 13);
        }
    }
}

// Move every particle one frame and draw it (drops as slanted lines, snow as
// dots and the odd snowflake)
static void parts_draw(Particle* ps, int n, WeatherAnim kind, uint32_t frame,
                       uint16_t col, uint16_t flake_col) {
    bool snow = (kind == WX_ANIM_SNOW);
    for (int i = 0; i < n; i++) {
        Particle& p = ps[i];
        p.y += p.vy;
        p.x += snow ? (((frame + i * 7) / 12) % 2 ? 1 : -1) : p.vx;   // snow sways
        if (p.y > SCREEN_H + 12 || p.x > SCREEN_W + 20 || p.x < -40) {
            p.y = random(-40, -4);
            p.x = random(-40, SCREEN_W);
        }
        if (snow) {
            if (p.size == 9) draw_snowflake(p.x, p.y, flake_col);
            else             gfx->fillRect(p.x, p.y, p.size + 1, p.size + 1, col);
        } else {
            gfx->drawLine(p.x, p.y, p.x - p.vx, p.y - p.size, col);
        }
    }
}

// Lightning: a zig-zag bolt that stays a few frames. Returns true on the frame
// a new bolt appears (the pop-up lights the sky then).
struct Bolt { int16_t pts[8][2]; int frames = 0; };
static bool bolt_step(Bolt& b, int chance_pct) {
    if (b.frames > 0 || random(0, 100) >= chance_pct) return false;
    int x = random(80, SCREEN_W - 80), y = 0;
    for (int k = 0; k < 8; k++) {
        b.pts[k][0] = x; b.pts[k][1] = y;
        x += random(-28, 29); y += random(16, 30);
    }
    b.frames = 4;
    return true;
}
static void bolt_draw(Bolt& b, uint16_t bright, uint16_t fading) {
    if (b.frames <= 0) return;
    for (int k = 0; k < 7; k++)
        for (int t = -1; t <= 1; t++)
            gfx->drawLine(b.pts[k][0] + t, b.pts[k][1], b.pts[k + 1][0] + t, b.pts[k + 1][1],
                          b.frames > 2 ? bright : fading);
    b.frames--;
}

// ── Pop-up ───────────────────────────────────────────────────────────────────
static Particle  s_parts[90];
static int       s_n_parts       = 0;
static int       s_anim_kind     = -1;     // WeatherAnim, -1 = none
static String    s_anim_text;
static uint32_t  s_anim_start_ms = 0, s_anim_frame_ms = 0;
static Bolt      s_bolt;
static const uint32_t ANIM_MS  = 5000;
static const uint32_t FRAME_MS = 40;       // ~25 frames per second

void display_weather_anim_start(WeatherAnim kind, const char* text) {
    s_anim_kind     = kind;
    s_anim_text     = to_latin1(text);
    s_anim_start_ms = millis();
    s_anim_frame_ms = 0;
    s_bolt.frames   = 0;
    s_n_parts = (kind == WX_ANIM_SNOW) ? 60 : (kind == WX_ANIM_STORM) ? 90 : 70;
    parts_init(s_parts, s_n_parts, kind, 1.0f);
}

bool display_weather_anim_active() { return s_anim_kind >= 0; }

bool display_weather_anim_tick() {
    if (s_anim_kind < 0 || !gfx) return false;
    uint32_t now = millis();
    if (now - s_anim_start_ms >= ANIM_MS) {
        s_anim_kind = -1;
        display_invalidate();   // the page repaints completely
        return false;
    }
    if (now - s_anim_frame_ms < FRAME_MS) return true;
    s_anim_frame_ms = now;
    WeatherAnim kind = (WeatherAnim)s_anim_kind;

    bool flash = (kind == WX_ANIM_STORM) && bolt_step(s_bolt, 5);
    FrameBuffered fb;
    gfx->fillScreen(flash ? c_meta : BLACK);
    bolt_draw(s_bolt, 0xFFFF, c_row0);
    parts_draw(s_parts, s_n_parts, kind, (now - s_anim_start_ms) / FRAME_MS, c_rows, c_row0);

    // The message, on a black plate so the drops don't run through it
    int tw = text_w(s_anim_text, 3);
    int bx = (SCREEN_W - tw) / 2 - 14, by = SCREEN_H / 2 - 24;
    gfx->fillRoundRect(bx, by, tw + 28, 48, 8, BLACK);
    gfx->drawRoundRect(bx, by, tw + 28, 48, 8, c_meta);
    draw_text(s_anim_text, bx + 14, by + 10, 3, c_row0);
    return true;
}

// ── Clock (night clock, and the Today page's clock) ──────────────────────────
// The digits sit on a scene for the weather outside: rain/snow/storm (the
// particle field above), sun, clouds, partly cloudy, or the moon — in its real
// phase — with stars, partly behind clouds. Animated, or one still frame a minute.
static const int      CLOCK_PARTS    = 45;
static const uint32_t CLOCK_FRAME_MS = 66;   // ~15 frames per second
static Particle  s_clock_parts[CLOCK_PARTS];
static int       s_clock_scene    = -2;      // scene now drawn (-1 = none, -2 = not yet)
static uint32_t  s_clock_frame_ms = 0, s_clock_frame = 0;
static Bolt      s_clock_bolt;

struct Cloud { float x; int16_t y, size; float speed; };
static Cloud     s_clouds[4];
static int       s_n_clouds = 0;
static int16_t   s_stars[28][2];

// Sun or moon in the top right corner, above the last digit (not behind it)
static const int SKY_CX = SCREEN_W - 32, SKY_CY = 30, SKY_R = 22;

// Moon phase 0…1 (0 new, 0.25 first quarter, 0.5 full, 0.75 last quarter):
// synodic months since a known new moon (2000-01-06 18:14 UTC)
static float moon_phase(time_t now) {
    double days = (double)(now - 947182440L) / 86400.0;
    double p = fmod(days / 29.530588853, 1.0);
    return (float)(p < 0 ? p + 1 : p);
}

// The lit part of the disc row by row (northern hemisphere: waxing lit on the
// right); the dark part is a faint outline
static void draw_moon(int cx, int cy, int r, float phase, uint16_t lit, uint16_t dark) {
    gfx->drawCircle(cx, cy, r, dark);
    float c = cosf(2 * PI * phase);
    for (int dy = -r; dy <= r; dy++) {
        float w  = sqrtf((float)(r * r - dy * dy));
        float x0 = (phase < 0.5f) ? w * c : -w;
        float x1 = (phase < 0.5f) ? w     : -w * c;
        if (x1 - x0 >= 0.5f)
            gfx->drawFastHLine(cx + (int)lroundf(x0), cy + dy, (int)lroundf(x1 - x0) + 1, lit);
    }
}

static void draw_sun_rays(int cx, int cy, uint32_t frame, uint16_t col) {
    gfx->fillCircle(cx, cy, SKY_R - 4, col);
    float a0 = frame * 0.015f;                    // rays turn slowly
    for (int k = 0; k < 12; k++) {
        float a = a0 + k * (PI / 6);
        int r1 = SKY_R + 2, r2 = SKY_R + ((k % 2) ? 10 : 15);
        for (int t = 0; t <= 1; t++)
            gfx->drawLine(cx + cosf(a) * r1 + t, cy + sinf(a) * r1,
                          cx + cosf(a) * r2 + t, cy + sinf(a) * r2, col);
    }
}

static void draw_cloud(int x, int y, int s, uint16_t col) {
    gfx->fillRoundRect(x, y, 4 * s, s + s / 2, s / 2, col);
    gfx->fillCircle(x + s,           y,         s,         col);
    gfx->fillCircle(x + 2 * s + s/3, y - s / 3, s + s / 3, col);
    gfx->fillCircle(x + 3 * s,       y + s / 4, s - s / 4, col);
}

// Rain/snow at night and sunny showers: one dark cloud in front of the moon/sun
static bool scene_sky_cloud(int scene) {
    return scene == SCENE_RAIN_NIGHT || scene == SCENE_SNOW_NIGHT ||
           scene == SCENE_STORM_NIGHT || scene == SCENE_SHOWERS;
}

// What falls in a scene (-1 = nothing)
static int scene_particles(int scene) {
    switch (scene) {
        case SCENE_RAIN:  case SCENE_RAIN_NIGHT: case SCENE_SHOWERS: return WX_ANIM_RAIN;
        case SCENE_SNOW:  case SCENE_SNOW_NIGHT:                     return WX_ANIM_SNOW;
        case SCENE_STORM: case SCENE_STORM_NIGHT:                    return WX_ANIM_STORM;
        default:                                                     return -1;
    }
}

static void clouds_init(int scene) {
    if (scene_sky_cloud(scene)) {   // stays put across the moon/sun
        s_n_clouds = 1;
        s_clouds[0] = { (float)(SKY_CX - 54), (int16_t)(SKY_CY + 4), 16, 0.0f };
        return;
    }
    bool overcast = (scene == SCENE_CLOUDS);
    s_n_clouds = overcast ? 4 : 2;
    for (int i = 0; i < s_n_clouds; i++) {
        Cloud& c = s_clouds[i];
        c.size  = overcast ? random(14, 22) : random(10, 15);
        c.y     = overcast ? 20 + i * 40 + random(0, 20) : SKY_CY + random(-6, 14);
        c.x     = overcast ? random(-60, SCREEN_W) : SKY_CX - 120 - i * 140;
        c.speed = 0.25f + random(0, 30) / 100.0f;
    }
    if (!overcast) s_clouds[0].x = SKY_CX - 40;   // one cloud starts across the sun/moon
}

static void clouds_draw(bool move, uint16_t col) {
    for (int i = 0; i < s_n_clouds; i++) {
        Cloud& c = s_clouds[i];
        if (move) c.x += c.speed;
        if (c.x > SCREEN_W + 10) c.x = -5 * c.size;
        draw_cloud((int)c.x, c.y, c.size, col);
    }
}

// Half as bright (each RGB565 channel halved) — for things that must stay faint
static uint16_t half565(uint16_t c) { return (c >> 1) & 0x7BEF; }

// Fog: long soft bands of mist drifting slowly sideways, each its own way
struct Mist { float x; int16_t y, len; float speed; };
static Mist s_mist[7];

static void mist_init() {
    for (int i = 0; i < 7; i++)
        s_mist[i] = { (float)random(0, SCREEN_W), (int16_t)(26 + i * 28 + random(-4, 5)),
                      (int16_t)random(90, 170), (random(0, 2) ? 1 : -1) * (0.2f + random(0, 30) / 100.0f) };
}

static void mist_draw(bool move, uint16_t col) {
    for (auto& m : s_mist) {
        if (move) m.x += m.speed;
        int period = m.len + 40;
        int x0 = ((int)m.x % period + period) % period - period;   // repeat across the screen
        for (int x = x0; x < SCREEN_W; x += period) gfx->fillRoundRect(x, m.y, m.len, 7, 3, col);
    }
}

static void stars_init() {
    for (auto& s : s_stars) { s[0] = random(4, SCREEN_W - 4); s[1] = random(4, SCREEN_H - 30); }
}

static void stars_draw(uint32_t frame, uint16_t col, uint16_t twinkle) {
    for (int i = 0; i < 28; i++) {
        bool bright = ((frame + i * 13) % 45) < 3;
        gfx->fillRect(s_stars[i][0], s_stars[i][1], bright ? 2 : 1, bright ? 2 : 1, bright ? twinkle : col);
    }
}

// ── Split-flap clock ─────────────────────────────────────────────────────────
// Four flap cards like an old station board. The digits (include/flap_digits.h)
// are smoothed — 16 levels blended onto the flap colour — and written straight
// into the off-screen canvas. When a digit changes, its top half folds down
// (darkening as it turns) and the new bottom half swings down after it; the
// cards flip one after another.
static int s_clock_style = CLOCK_DIGITAL;

void display_set_clock_style(int style) {
    s_clock_style  = (style >= CLOCK_DIGITAL && style < CLOCK_STYLE_COUNT) ? style : CLOCK_DIGITAL;
    s_clock_minute = -1;   // repaint in the new style
}

static const int      FLAP_GAP = 8, FLAP_COLON = 34, FLAP_TOP = 52;
static const int      FLAP_X0  = (SCREEN_W - (4 * FLAP_W + 2 * FLAP_GAP + FLAP_COLON)) / 2;
static const uint32_t FLAP_FLIP_MS = 400, FLAP_FRAME_MS = 30, FLAP_STAGGER_MS = 60;
struct FlapCard { int8_t shown = -1, from = -1; uint32_t start = 0; };
static FlapCard s_flap[4];

static int flap_x(int i) {
    return FLAP_X0 + i * (FLAP_W + FLAP_GAP) + (i >= 2 ? FLAP_COLON - FLAP_GAP : 0);
}

// a + (b - a) * k / 15, per RGB565 channel
static uint16_t mix565(uint16_t a, uint16_t b, int k) {
    int r = ((a >> 11) * (15 - k) + (b >> 11) * k) / 15;
    int g = (((a >> 5) & 63) * (15 - k) + ((b >> 5) & 63) * k) / 15;
    int bl = ((a & 31) * (15 - k) + (b & 31) * k) / 15;
    return (r << 11) | (g << 5) | bl;
}

// One half of a card, squashed toward the split line (squash 1 = flat on the
// board, 0 = edge-on) and darkened by shade (0…1) while it turns
static void flap_half(int x, int digit, bool top, float squash, float shade, uint16_t bg, uint16_t fg) {
    static const uint8_t CORNER[5] = { 4, 2, 1, 1, 0 };   // rounded outer corners
    const int half = FLAP_H / 2;
    int h = std::max(1, (int)lroundf(half * squash));
    int lit = (int)lroundf(15 * (1 - shade));
    uint16_t pal[16];
    uint16_t b = mix565(BLACK, bg, lit), f = mix565(BLACK, fg, lit);
    for (int k = 0; k < 16; k++) pal[k] = mix565(b, f, k);
    uint16_t* fb = s_canvas ? s_canvas->getFramebuffer() : nullptr;
    const uint8_t* glyph = FLAP_DIGITS[digit];
    for (int i = 0; i < h; i++) {
        int sy = top ? (i * half / h) : (half + i * half / h);
        int dy = top ? (FLAP_TOP + half - h + i) : (FLAP_TOP + half + i);
        if (dy < 0 || dy >= SCREEN_H) continue;
        int edge  = top ? sy : (FLAP_H - 1 - sy);
        int inset = edge < 5 ? CORNER[edge] : 0;
        const uint8_t* src = glyph + sy * (FLAP_W / 2);
        for (int px = inset; px < FLAP_W - inset; px++) {
            uint8_t v = src[px >> 1];
            uint16_t c = pal[(px & 1) ? (v & 15) : (v >> 4)];
            if (fb) fb[dy * SCREEN_W + x + px] = c;
            else    gfx->drawPixel(x + px, dy, c);
        }
    }
}

// Shown digits for the time; changed ones start flipping (not on a fresh paint)
static bool flap_update(const int digits[4], uint32_t now, bool fresh) {
    bool active = false;
    for (int i = 0; i < 4; i++) {
        FlapCard& c = s_flap[i];
        if (fresh || c.shown < 0) { c.shown = digits[i]; c.from = -1; }
        else if (digits[i] != c.shown) {
            c.from = c.shown; c.shown = digits[i]; c.start = now + i * FLAP_STAGGER_MS;
        }
        if (c.from >= 0 && (int32_t)(now - c.start) < (int32_t)FLAP_FLIP_MS) active = true;
    }
    return active;
}

static void flap_card(int i, uint32_t now, uint16_t bg_top, uint16_t bg_bot, uint16_t fg) {
    FlapCard& c = s_flap[i];
    int x = flap_x(i);
    float t = 1;
    if (c.from >= 0) t = constrain((float)(int32_t)(now - c.start) / FLAP_FLIP_MS, 0.0f, 1.0f);
    if (t >= 1) {
        c.from = -1;
        flap_half(x, c.shown, true,  1, 0, bg_top, fg);
        flap_half(x, c.shown, false, 1, 0, bg_bot, fg);
    } else if (t < 0.5f) {   // the old top folds down, the new top behind it
        float k = 1 - t / 0.5f;
        flap_half(x, c.shown, true,  1, 0, bg_top, fg);
        flap_half(x, c.from,  false, 1, 0, bg_bot, fg);
        flap_half(x, c.from,  true,  k, (1 - k) * 0.6f, bg_top, fg);
    } else {                 // the new bottom swings down over the old one
        float k = (t - 0.5f) / 0.5f;
        flap_half(x, c.shown, true,  1, 0, bg_top, fg);
        flap_half(x, c.from,  false, 1, 0, bg_bot, fg);
        flap_half(x, c.shown, false, k, (1 - k) * 0.6f, bg_bot, fg);
    }
    int sy = FLAP_TOP + FLAP_H / 2;   // split line and the hinges at its ends
    gfx->fillRect(x, sy - 1, FLAP_W, 2, BLACK);
    gfx->fillRect(x - 1, sy - 4, 3, 8, BLACK);
    gfx->fillRect(x + FLAP_W - 2, sy - 4, 3, 8, BLACK);
}

// Bottom line of the clock for a few seconds (what a swipe/tap does)
static String   s_clock_hint;
static uint32_t s_clock_hint_until = 0;

void display_clock_hint(const char* text, uint32_t duration_ms) {
    s_clock_hint       = to_latin1(text);
    s_clock_hint_until = millis() + duration_ms;
    s_clock_minute     = -1;
}

static void matrix_clock_draw(bool animate, bool bright);   // below, with the boot rain

void display_draw_night_clock(const char* subtitle, int scene, bool animate, bool bright) {
    if (!gfx) return;
    apply_night_brightness();
    if (s_clock_style == CLOCK_MATRIX) { matrix_clock_draw(animate, bright); return; }   // no weather

    time_t now_t = time(nullptr);
    struct tm t;
    localtime_r(&now_t, &t);
    int minute = t.tm_hour * 60 + t.tm_min;
    if (scene != s_clock_scene) {
        s_clock_scene  = scene;
        s_clock_minute = -1;
        if (scene_particles(scene) >= 0)
            parts_init(s_clock_parts, CLOCK_PARTS, (WeatherAnim)scene_particles(scene), 1.6f);
        clouds_init(scene);
        stars_init();
        mist_init();
    }
    if (s_clock_hint.length() && (int32_t)(millis() - s_clock_hint_until) >= 0) {
        s_clock_hint   = "";
        s_clock_minute = -1;   // repaint without it
    }
    bool fresh = (s_clock_minute < 0);
    bool flap  = (s_clock_style == CLOCK_FLAP);
    int  digits[4] = { t.tm_hour / 10, t.tm_hour % 10, t.tm_min / 10, t.tm_min % 10 };
    bool flipping = flap && now_t > 1700000000 && flap_update(digits, millis(), fresh);

    // Pace: flipping cards ~30 frames/s, moving scene ~15, otherwise once a minute
    if (flipping) {
        if (!fresh && millis() - s_clock_frame_ms < FLAP_FRAME_MS) return;
        s_clock_frame_ms = millis();
        if (animate) s_clock_frame++;
    } else if (animate && scene >= 0) {
        if (millis() - s_clock_frame_ms < CLOCK_FRAME_MS) return;
        s_clock_frame_ms = millis();
        s_clock_frame++;
    } else if (minute == s_clock_minute) {
        return;
    }
    s_clock_minute = minute;

    // Day clock: brighter; night clock: dim, nothing that lights up the room
    uint16_t digit = bright ? c_row0 : c_dim;
    uint16_t fg    = bright ? c_dim  : c_meta;    // clouds, drops, stars
    uint16_t body  = bright ? c_rows : c_dim;     // sun, moon
    bool     move  = animate;

    FrameBuffered frame;
    gfx->fillScreen(BLACK);
    switch (scene) {
        case SCENE_RAIN: case SCENE_SNOW: case SCENE_STORM:   // by day (night: see below)
            if (scene == SCENE_STORM && move) bolt_step(s_clock_bolt, 2);
            bolt_draw(s_clock_bolt, bright ? c_row0 : c_dim, fg);
            parts_draw(s_clock_parts, CLOCK_PARTS, (WeatherAnim)scene, s_clock_frame, fg, fg);
            break;
        case SCENE_SUN:
            draw_sun_rays(SKY_CX, SKY_CY, s_clock_frame, body);
            break;
        case SCENE_PARTLY_SUN:
            draw_sun_rays(SKY_CX, SKY_CY, s_clock_frame, body);
            clouds_draw(move, fg);
            break;
        case SCENE_CLOUDS:
            clouds_draw(move, fg);
            break;
        case SCENE_MOON:
        case SCENE_PARTLY_MOON:
            stars_draw(s_clock_frame, c_meta, c_dim);
            draw_moon(SKY_CX, SKY_CY, SKY_R, moon_phase(now_t), body, c_meta);
            if (scene == SCENE_PARTLY_MOON) clouds_draw(move, c_meta);
            break;
        case SCENE_RAIN_NIGHT: case SCENE_SNOW_NIGHT: case SCENE_STORM_NIGHT:
            // The moon peeks out from behind the cloud the rain/snow falls from
            draw_moon(SKY_CX, SKY_CY, SKY_R, moon_phase(now_t), body, c_meta);
            if (scene == SCENE_STORM_NIGHT && move) bolt_step(s_clock_bolt, 2);
            bolt_draw(s_clock_bolt, bright ? c_row0 : c_dim, fg);
            clouds_draw(false, c_meta);
            parts_draw(s_clock_parts, CLOCK_PARTS, (WeatherAnim)scene_particles(scene), s_clock_frame, fg, fg);
            break;
        case SCENE_SHOWERS:   // sunshine and a shower: sun behind the rain cloud
            draw_sun_rays(SKY_CX, SKY_CY, s_clock_frame, body);
            clouds_draw(false, fg);
            parts_draw(s_clock_parts, CLOCK_PARTS, WX_ANIM_RAIN, s_clock_frame, fg, fg);
            break;
        case SCENE_FOG:   // mist at half the dimmest level: the digits stay readable
            mist_draw(move, half565(c_meta));
            break;
        case SCENE_FOG_NIGHT:   // the moon only a faint glow through the mist
            draw_moon(SKY_CX, SKY_CY, SKY_R, moon_phase(now_t), c_meta, c_meta);
            gfx->drawCircle(SKY_CX, SKY_CY, SKY_R + 6, c_meta);
            mist_draw(move, half565(c_meta));
            break;
        default: break;
    }

    // Date + temperature on top
    if (subtitle && *subtitle) {
        String s = to_latin1(subtitle);
        draw_text(s, (SCREEN_W - text_w(s, 2)) / 2, 8, 2, bright ? c_dim : c_meta);
    }

    if (flap && now_t > 1700000000) {
        // Split-flap: dark flaps (lighter top half), dimmer at night
        uint16_t bg_top = bright ? 0x18C3 : 0x0861;   // ~#1A1A1A / #0E0E0E
        uint16_t bg_bot = bright ? 0x1082 : 0x0841;   // ~#121212 / #080808
        uint32_t now_ms = millis();
        for (int i = 0; i < 4; i++) flap_card(i, now_ms, bg_top, bg_bot, digit);
        int cx = FLAP_X0 + 2 * FLAP_W + FLAP_GAP + FLAP_COLON / 2;
        gfx->fillRoundRect(cx - 5, FLAP_TOP + FLAP_H / 3 - 5,     10, 10, 2, digit);
        gfx->fillRoundRect(cx - 5, FLAP_TOP + 2 * FLAP_H / 3 - 5, 10, 10, 2, digit);
    } else if (now_t > 1700000000) {
        // H H : M M below — 78×140 digits, 13 px segments; leading hour zero left blank
        const int dw = 78, dh = 140, th = 13, gap = 14, colon_w = 52;
        const int total = 4 * dw + 2 * gap + colon_w;
        int x = (SCREEN_W - total) / 2, y = 62;
        if (t.tm_hour >= 10) draw_7seg(x, y, dw, dh, th, t.tm_hour / 10, digit);
        draw_7seg(x + dw + gap, y, dw, dh, th, t.tm_hour % 10, digit);
        int cx = x + 2 * dw + gap + colon_w / 2;
        gfx->fillRoundRect(cx - th / 2, y + dh / 3 - th / 2,     th, th, th / 2, digit);
        gfx->fillRoundRect(cx - th / 2, y + 2 * dh / 3 - th / 2, th, th, th / 2, digit);
        int mx = x + 2 * dw + gap + colon_w;
        draw_7seg(mx, y, dw, dh, th, t.tm_min / 10, digit);
        draw_7seg(mx + dw + gap, y, dw, dh, th, t.tm_min % 10, digit);
    }

    if (s_clock_hint.length())
        draw_text(s_clock_hint, (SCREEN_W - text_w(s_clock_hint, 2)) / 2, SCREEN_H - 18, 2, c_meta);
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
// Two versions: the "digital rain" (default) and the original one ("classic",
// BOOT_ANIM_CLASSIC in config.h), kept unchanged to switch back easily.
static volatile bool  s_anim_running      = false;
static TaskHandle_t   s_anim_task_handle  = nullptr;

// Classic: 4-character trails, drawn straight to the screen; random column wipe at the end
static void anim_task_classic(void*) {
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

// ── Digital rain ─────────────────────────────────────────────────────────────
// Whole frames drawn off-screen (~30/s): mirrored katakana, trails of 6–20
// symbols fading over 16 shades, symbols that stay where they fell and only
// now and then change, columns at different depths (near = faster, brighter,
// longer), and a glowing near-white head. Ending: the rain drains off the
// bottom while "Wake up, <name>..." is typed in the middle; a touch skips it.
struct RainCol { float head, speed; uint8_t len, bright; uint8_t chars[16]; };
static RainCol         s_rain[64];
static String          s_wake_name;
static volatile bool   s_anim_ending = true;    // false: stop without the ending
static volatile bool   s_anim_skip   = false;   // ending skipped by a touch/button

static const int      RAIN_COLS = SCREEN_W / MATRIX_FONT_W;   // 60
static const int      RAIN_ROWS = SCREEN_H / MATRIX_FONT_H;   // 13
static const uint32_t RAIN_FRAME_MS = 33;

static void rain_new_col(RainCol& c, bool initial) {
    float depth = random(1000) / 1000.0f;                 // 0 far … 1 near
    c.head   = initial ? -random(RAIN_ROWS * 15) / 10.0f : -random(60) / 10.0f;
    c.speed  = 7 + depth * 16;                            // rows per second
    c.len    = 6 + (uint8_t)(depth * 14);
    c.bright = 7 + (uint8_t)(depth * 8);                  // 7…15 of 15
    for (auto& ch : c.chars) ch = random(94);
}

// One katakana glyph (font index 0–93) mirrored, straight into the canvas
static void rain_glyph(uint16_t* fb, int x, int y, int idx, uint16_t col) {
    const GFXglyph* g = &MatrixCodeGlyphs[idx];
    const uint8_t*  bm = MatrixCodeBitmaps + pgm_read_word(&g->bitmapOffset);
    int w = pgm_read_byte(&g->width), h = pgm_read_byte(&g->height);
    int xo = (int8_t)pgm_read_byte(&g->xOffset), yo = (int8_t)pgm_read_byte(&g->yOffset);
    uint8_t bits = 0;
    for (int i = 0, n = 0; i < h; i++)
        for (int j = 0; j < w; j++, n++) {
            if (!(n & 7)) bits = pgm_read_byte(bm++);
            if (bits & 0x80) {
                int px = x + MATRIX_FONT_W - 1 - (xo + j), py = y + MATRIX_FONT_ASC + yo + i;
                if (px >= 0 && px < SCREEN_W && py >= 0 && py < SCREEN_H) fb[py * SCREEN_W + px] = col;
            }
            bits <<= 1;
        }
}

// TramliLarge text into the canvas (the message), width by the font's advances
static int rain_text_w(const String& s) {
    int w = 0;
    for (size_t i = 0; i < s.length(); i++) w += glyph_xadvance(&TramliLarge, s[i]);
    return w;
}

static void anim_task_rain(void*) {
    Arduino_GFX* screen = gfx;
    if (!s_canvas) {
        s_canvas = new Arduino_Canvas(SCREEN_W, SCREEN_H, screen);
        if (!s_canvas->begin(GFX_SKIP_OUTPUT_BEGIN)) { delete s_canvas; s_canvas = nullptr; }
    }
    if (!s_canvas) { anim_task_classic(nullptr); return; }   // no memory for frames: the old one
    uint16_t* fb = s_canvas->getFramebuffer();

    uint16_t shade[16];
    for (int k = 0; k < 16; k++) shade[k] = mix565(BLACK, c_row0, k);
    const uint16_t HEAD = 0xFF9B;                          // warm white

    for (int i = 0; i < RAIN_COLS; i++) rain_new_col(s_rain[i], true);
    String text = s_wake_name.length() ? "Wake up, " + to_latin1(s_wake_name) + "..." : String("Wake up...");
    uint32_t last = millis(), end_start = 0;
    bool ending = false;

    for (;;) {
        uint32_t now = millis();
        if (!s_anim_running && !ending) {
            if (!s_anim_ending) break;
            ending = true; end_start = now;
        }
        if (ending && s_anim_skip) break;
        float dt = std::min(0.05f, (now - last) / 1000.0f);
        last = now;

        s_canvas->fillScreen(BLACK);
        int alive = 0;
        for (int ci = 0; ci < RAIN_COLS; ci++) {
            RainCol& c = s_rain[ci];
            int prev = (int)floorf(c.head);
            c.head += c.speed * dt;
            int hr = (int)floorf(c.head);
            for (int r = prev + 1; r <= hr; r++)               // a new symbol where the head lands
                if (r >= 0 && r < RAIN_ROWS) c.chars[r] = random(94);
            if (hr - c.len > RAIN_ROWS) {                     // fallen off the bottom
                if (ending) continue;                         // the rain drains away
                rain_new_col(c, false);
                hr = (int)floorf(c.head);
            }
            alive++;
            int x = ci * MATRIX_FONT_W;
            for (int d = 1; d <= c.len; d++) {                // trail, fading out
                int r = hr - d;
                if (r < 0 || r >= RAIN_ROWS) continue;
                if (random(100) < 2) c.chars[r] = random(94); // now and then a symbol changes
                int k = std::max(1, (int)lroundf(c.bright * (1.0f - (float)d / (c.len + 1))));
                rain_glyph(fb, x, r * MATRIX_FONT_H, c.chars[r], shade[k]);
            }
            if (hr >= 0 && hr < RAIN_ROWS) {                  // glowing head
                uint16_t halo = shade[std::max(1, c.bright * 6 / 15)];
                int y = hr * MATRIX_FONT_H;
                rain_glyph(fb, x - 1, y, c.chars[hr], halo);
                rain_glyph(fb, x + 1, y, c.chars[hr], halo);
                rain_glyph(fb, x, y - 1, c.chars[hr], halo);
                rain_glyph(fb, x, y + 1, c.chars[hr], halo);
                rain_glyph(fb, x, y, c.chars[hr], HEAD);
            }
        }

        if (ending) {   // "Wake up, <name>..." typed with a block cursor
            uint32_t et = now - end_start;
            int typed = (int)std::min<uint32_t>(text.length(), et > 300 ? (et - 300) / 75 : 0);
            int tw = rain_text_w(text), x0 = (SCREEN_W - tw) / 2, y = SCREEN_H / 2 - 14;
            s_canvas->fillRect(x0 - 12, y - 6, tw + 32, 40, BLACK);
            String s = text.substring(0, typed);
            s_canvas->setFont(&TramliLarge);
            s_canvas->setTextColor(c_row0);
            s_canvas->setCursor(x0, y + LARGE_ASCENT);
            s_canvas->print(s);
            int cx = x0 + rain_text_w(s) + 2;
            if (typed < (int)text.length() || (et / 400) % 2 == 0) s_canvas->fillRect(cx, y + 2, 11, 24, c_row0);
            if (typed == (int)text.length() && et > 300 + text.length() * 75 + 1000) break;
        }

        screen->draw16bitRGBBitmap(0, 0, fb, SCREEN_W, SCREEN_H);
        int32_t wait = RAIN_FRAME_MS - (int32_t)(millis() - now);
        vTaskDelay(pdMS_TO_TICKS(std::max<int32_t>(1, wait)));
    }
    screen->fillScreen(BLACK);
    s_anim_task_handle = nullptr;
    vTaskDelete(nullptr);
}

void display_boot_animation_start() {
    if (!gfx) return;
    s_anim_running = true;
    s_anim_skip    = false;
    xTaskCreatePinnedToCore(BOOT_ANIM_CLASSIC ? anim_task_classic : anim_task_rain, "boot_anim",
                            6144, nullptr, 1, &s_anim_task_handle, 1);
}

void display_boot_animation_set_name(const String& name) { s_wake_name = name; }

// ── Matrix clock ─────────────────────────────────────────────────────────────
// Its own world, no weather: the boot animation's rain slowed to a fifth and
// dimmed, and the time made of the same symbols — 5×7 dot-matrix digits, each
// dot two characters wide, centred on the 60×13 grid. The time's symbols glow
// brighter, change every 0.6–3 s, and flash when a drop passes. A new minute
// simply appears. ~15 frames/s on USB, ~5 on battery (the rain is slow).
static RainCol  s_mrain[64];
static uint8_t  s_mchar[RAIN_COLS * RAIN_ROWS];
static uint32_t s_mchange[RAIN_COLS * RAIN_ROWS];   // when a clock symbol changes next
static float    s_mlit[RAIN_COLS * RAIN_ROWS];      // rain brightness this frame (2 = head)
static bool     s_matrix_ready = false;
static uint32_t s_matrix_frame_ms = 0;

static const uint8_t MATRIX_DIGITS[10][7] = {   // 5×7, bit 4 = leftmost dot
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
};
static const int MATRIX_ROW0 = 3, MATRIX_COLON = 29;
static const int MATRIX_DIG_COL[4] = { 5, 17, 33, 45 };

static void matrix_clock_draw(bool animate, bool bright) {
    uint32_t now = millis();
    if (s_clock_hint.length() && (int32_t)(now - s_clock_hint_until) >= 0) s_clock_hint = "";
    if (s_matrix_ready && now - s_matrix_frame_ms < (animate ? 66u : 200u)) return;
    float dt = s_matrix_ready ? std::min(0.3f, (now - s_matrix_frame_ms) / 1000.0f) : 0;
    s_matrix_frame_ms = now;
    if (!s_matrix_ready) {
        for (int i = 0; i < RAIN_COLS; i++) {
            rain_new_col(s_mrain[i], true);
            s_mrain[i].speed *= 0.2f;
        }
        for (int i = 0; i < RAIN_COLS * RAIN_ROWS; i++) { s_mchar[i] = random(94); s_mchange[i] = now + random(3000); }
        s_matrix_ready = true;
    }

    // Colours: rain in 16 amber shades; the time a quarter toward warm white
    uint16_t full  = bright ? c_row0 : mix565(BLACK, c_row0, 8);
    uint16_t shade[16];
    for (int k = 0; k < 16; k++) shade[k] = mix565(BLACK, full, k);
    uint16_t digit = mix565(full, bright ? 0xFF55 : mix565(BLACK, 0xFF55, 8), 4);
    uint16_t flash = bright ? 0xFF9B : mix565(BLACK, 0xFF9B, 8);

    // Rain: move, land new symbols, brightness per cell
    for (auto& v : s_mlit) v = 0;
    for (int ci = 0; ci < RAIN_COLS; ci++) {
        RainCol& c = s_mrain[ci];
        int prev = (int)floorf(c.head);
        c.head += c.speed * dt;
        int hr = (int)floorf(c.head);
        for (int r = prev + 1; r <= hr; r++)
            if (r >= 0 && r < RAIN_ROWS) s_mchar[r * RAIN_COLS + ci] = random(94);
        if (hr - c.len > RAIN_ROWS) { rain_new_col(c, false); c.speed *= 0.2f; hr = (int)floorf(c.head); }
        for (int d = 0; d <= c.len; d++) {
            int r = hr - d;
            if (r < 0 || r >= RAIN_ROWS) continue;
            float v = (d == 0) ? 2.0f : (c.bright / 15.0f) * (1.0f - (float)d / (c.len + 1));
            float& l = s_mlit[r * RAIN_COLS + ci];
            if (v > l) l = v;
        }
    }

    // Which cells make up the time (leading zero shown, like a terminal)
    static bool clock_cell[RAIN_COLS * RAIN_ROWS];
    memset(clock_cell, 0, sizeof(clock_cell));
    time_t now_t = time(nullptr);
    if (now_t > 1700000000) {
        struct tm t; localtime_r(&now_t, &t);
        int ds[4] = { t.tm_hour / 10, t.tm_hour % 10, t.tm_min / 10, t.tm_min % 10 };
        for (int i = 0; i < 4; i++)
            for (int r = 0; r < 7; r++)
                for (int b = 0; b < 5; b++)
                    if (MATRIX_DIGITS[ds[i]][r] & (0x10 >> b)) {
                        int cell = (MATRIX_ROW0 + r) * RAIN_COLS + MATRIX_DIG_COL[i] + 2 * b;
                        clock_cell[cell] = clock_cell[cell + 1] = true;
                    }
        for (int r : { MATRIX_ROW0 + 2, MATRIX_ROW0 + 4 })
            clock_cell[r * RAIN_COLS + MATRIX_COLON] = clock_cell[r * RAIN_COLS + MATRIX_COLON + 1] = true;
    }

    FrameBuffered frame;
    uint16_t* fb = s_canvas ? s_canvas->getFramebuffer() : nullptr;
    gfx->fillScreen(BLACK);
    if (fb) {
        int change_pct = std::max(1, (int)lroundf(18.0f * dt));   // background symbols: ~0.18 changes/s
        for (int i = 0; i < RAIN_COLS * RAIN_ROWS; i++) {
            int x = (i % RAIN_COLS) * MATRIX_FONT_W, y = (i / RAIN_COLS) * MATRIX_FONT_H;
            float l = s_mlit[i];
            if (clock_cell[i]) {
                if ((int32_t)(now - s_mchange[i]) >= 0) { s_mchar[i] = random(94); s_mchange[i] = now + 600 + random(2400); }
                uint16_t halo = shade[5];
                rain_glyph(fb, x - 1, y, s_mchar[i], halo);
                rain_glyph(fb, x + 1, y, s_mchar[i], halo);
                rain_glyph(fb, x, y, s_mchar[i], l >= 2 ? flash : digit);
            } else if (l > 0) {
                if (random(1000) < change_pct) s_mchar[i] = random(94);
                int k = (l >= 2) ? 7 : std::max(1, (int)lroundf(6 * l));   // rain at ~40 % at most
                rain_glyph(fb, x, y, s_mchar[i], shade[k]);
            }
        }
    }
    if (s_clock_hint.length()) {
        int tw = text_w(s_clock_hint, 2);
        gfx->fillRect((SCREEN_W - tw) / 2 - 6, SCREEN_H - 20, tw + 12, 20, BLACK);
        draw_text(s_clock_hint, (SCREEN_W - tw) / 2, SCREEN_H - 18, 2, c_meta);
    }
}

void display_boot_animation_stop(bool ending, bool (*skip_requested)()) {
    s_anim_ending  = ending;
    s_anim_running = false;
    while (s_anim_task_handle != nullptr) {
        if (skip_requested && skip_requested()) s_anim_skip = true;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool display_boot_animation_skipped() { return s_anim_skip; }

// ── Pager UI ──────────────────────────────────────────────────────────────────

static void pager_draw_header(const char* title) {
    gfx->fillScreen(BLACK);
    String t = to_latin1(title);
    int tw = text_w(t, 2);
    draw_text(t, (SCREEN_W - tw) / 2, (HEADER_H - 16) / 2, 2, c_row0);
    gfx->drawFastHLine(0, HEADER_H - 1, SCREEN_W, c_meta);
}

// Pixel-art icons for the 10 activity slots, ~44×36 px.
// cx/cy = center of the icon drawing area.

// Line drawn t×t pixels thick (limbs, frames)
static void thick_line(int x0, int y0, int x1, int y1, uint16_t col, int t = 2) {
    for (int i = 0; i < t; i++)
        for (int j = 0; j < t; j++) gfx->drawLine(x0 + i, y0 + j, x1 + i, y1 + j, col);
}

static void pager_icon(int idx, int cx, int cy, uint16_t col) {
    switch (idx) {
    case 0: // Beer: glass mug with ribs, rounded handle, bubbly foam
        gfx->fillRoundRect(cx - 11, cy - 8, 20, 23, 3, col);              // glass
        gfx->fillRoundRect(cx + 7, cy - 4, 10, 15, 4, col);               // handle
        gfx->fillRoundRect(cx + 10, cy - 1, 4, 9, 1, BLACK);              //   and its hole
        gfx->fillRect(cx - 7, cy - 3, 2, 14, BLACK);                      // ribs
        gfx->fillRect(cx - 1, cy - 3, 2, 14, BLACK);
        gfx->fillRect(cx + 5, cy - 3, 1, 14, BLACK);
        gfx->fillCircle(cx - 8, cy - 11, 4, col);                         // foam
        gfx->fillCircle(cx - 1, cy - 13, 5, col);
        gfx->fillCircle(cx + 6, cy - 11, 4, col);
        gfx->fillRect(cx - 12, cy - 10, 22, 3, col);
        gfx->fillCircle(cx - 11, cy - 6, 2, col);                         // a drip
        gfx->drawPixel(cx - 2, cy - 14, BLACK);                           // bubbles
        gfx->drawPixel(cx + 5, cy - 12, BLACK);
        gfx->drawPixel(cx - 8, cy - 12, BLACK);
        break;
    case 1: // Coffee: rounded cup, ring handle, saucer, wavy steam
        for (int base : { cx - 5, cx + 2 })
            for (int y = cy - 18; y < cy - 8; y++) {
                int x = base + (int)lroundf(2 * sinf((y - cy) * 0.75f));
                gfx->drawFastHLine(x, y, 2, col);
            }
        gfx->fillRoundRect(cx - 12, cy - 6, 21, 15, 5, col);              // cup
        gfx->fillRect(cx - 12, cy - 6, 21, 4, col);                       //   flat rim
        gfx->drawFastHLine(cx - 10, cy - 4, 17, BLACK);                   //   coffee line
        gfx->drawCircle(cx + 10, cy + 1, 5, col);                         // handle
        gfx->drawCircle(cx + 10, cy + 1, 4, col);
        gfx->fillRoundRect(cx - 16, cy + 10, 32, 4, 2, col);              // saucer
        break;
    case 2: // Movie: clapperboard with its striped clapper tilted open
        gfx->fillRoundRect(cx - 15, cy - 3, 30, 19, 2, col);              // board
        gfx->drawFastHLine(cx - 12, cy + 3, 24, BLACK);
        gfx->drawFastHLine(cx - 12, cy + 8, 24, BLACK);
        gfx->drawFastVLine(cx - 3, cy + 3, 10, BLACK);
        for (int t = 0; t < 6; t++)                                       // clapper
            gfx->drawLine(cx - 15, cy - 6 - t, cx + 13, cy - 14 - t, col);
        for (int k = 0; k < 5; k++) {                                     // its stripes
            int x0 = cx - 12 + k * 6, dy = (int)(k * 1.6f);
            thick_line(x0, cy - 7 - dy, x0 + 4, cy - 12 - dy, BLACK);
        }
        gfx->fillCircle(cx - 14, cy - 6, 2, col);                         // hinge
        break;
    case 3: // Gym: dumbbell, two rounded plates a side
        gfx->fillRect(cx - 12, cy - 2, 24, 4, col);
        gfx->fillRoundRect(cx - 16, cy - 11, 6, 22, 2, col);
        gfx->fillRoundRect(cx + 10, cy - 11, 6, 22, 2, col);
        gfx->fillRoundRect(cx - 22, cy - 7, 5, 14, 2, col);
        gfx->fillRoundRect(cx + 17, cy - 7, 5, 14, 2, col);
        break;
    case 4: // Run: runner mid-stride, speed lines behind
        gfx->fillCircle(cx + 6, cy - 13, 4, col);                         // head
        thick_line(cx + 3, cy - 7, cx - 1, cy + 3, col, 3);               // body
        thick_line(cx + 2, cy - 6, cx - 6, cy - 2, col);                  // back arm
        thick_line(cx - 6, cy - 2, cx - 9, cy + 3, col);
        thick_line(cx + 3, cy - 6, cx + 9, cy - 2, col);                  // front arm
        thick_line(cx + 9, cy - 2, cx + 13, cy - 7, col);
        thick_line(cx, cy + 3, cx + 7, cy + 8, col);                      // front leg
        thick_line(cx + 7, cy + 8, cx + 5, cy + 15, col);
        thick_line(cx, cy + 3, cx - 6, cy + 9, col);                      // back leg
        thick_line(cx - 6, cy + 9, cx - 13, cy + 8, col);
        gfx->drawFastHLine(cx - 21, cy - 8, 7, col);                      // speed lines
        gfx->drawFastHLine(cx - 23, cy - 3, 8, col);
        gfx->drawFastHLine(cx - 21, cy + 2, 6, col);
        break;
    case 5: // Bike: thick wheels with hubs, frame, saddle, handlebar
        for (int wx : { cx - 12, cx + 12 }) {
            gfx->drawCircle(wx, cy + 6, 9, col);
            gfx->drawCircle(wx, cy + 6, 8, col);
            gfx->fillCircle(wx, cy + 6, 1, col);
        }
        thick_line(cx - 12, cy + 6, cx - 3, cy - 4, col);
        thick_line(cx - 3,  cy - 4, cx + 7, cy - 4, col);
        thick_line(cx + 7,  cy - 4, cx + 12, cy + 6, col);
        thick_line(cx - 12, cy + 6, cx + 1, cy + 6, col);
        thick_line(cx + 1,  cy + 6, cx - 3, cy - 4, col);
        thick_line(cx + 1,  cy + 6, cx + 7, cy - 4, col);
        gfx->fillRoundRect(cx - 8, cy - 8, 8, 3, 1, col);                 // saddle
        thick_line(cx + 7, cy - 4, cx + 6, cy - 10, col);
        gfx->fillRoundRect(cx + 3, cy - 11, 8, 2, 1, col);                // handlebar
        break;
    case 6: { // Phone: handset with ringing waves
        const int ox = cx + 6, oy = cy - 6, r = 15;
        for (int a = 90; a <= 180; a += 3) {                              // curved grip
            float t = a * DEG_TO_RAD;
            gfx->fillCircle(ox + (int)lroundf(r * cosf(t)), oy + (int)lroundf(r * sinf(t)), 3, col);
        }
        gfx->fillRoundRect(cx - 14, cy - 13, 9, 11, 3, col);              // earpiece
        gfx->fillRoundRect(cx + 1,  cy + 5, 11, 9, 3, col);               // mouthpiece
        for (int rr : { 8, 12 })                                          // ringing
            for (int a = -80; a <= -10; a += 2) {
                float t = a * DEG_TO_RAD;
                int x = cx + 3 + (int)lroundf(rr * cosf(t)), y = cy - 1 + (int)lroundf(rr * sinf(t));
                gfx->drawFastHLine(x, y, 2, col);
            }
        break;
    }
    case 7: // Dinner: plate between fork and knife
        gfx->fillCircle(cx, cy + 1, 12, col);                             // plate
        gfx->fillCircle(cx, cy + 1, 10, BLACK);
        gfx->drawCircle(cx, cy + 1, 6, col);
        for (int k = 0; k < 3; k++) gfx->fillRect(cx - 22 + k * 2, cy - 12, 1, 8, col);   // fork
        gfx->fillRect(cx - 22, cy - 5, 5, 3, col);
        gfx->fillRoundRect(cx - 21, cy - 3, 3, 18, 1, col);
        gfx->fillRoundRect(cx + 17, cy - 12, 5, 14, 2, col);              // knife blade
        gfx->fillRect(cx + 17, cy - 12, 2, 14, col);
        gfx->fillRoundRect(cx + 18, cy + 1, 3, 14, 1, col);               //   and handle
        break;
    case 8: // Lunch: burger
        gfx->fillCircle(cx, cy, 15, col);                                 // top bun
        gfx->fillRect(cx - 16, cy, 33, 17, BLACK);
        gfx->fillRect(cx - 7, cy - 8, 2, 1, BLACK);                       // sesame
        gfx->fillRect(cx,     cy - 11, 2, 1, BLACK);
        gfx->fillRect(cx + 7, cy - 7, 2, 1, BLACK);
        gfx->fillRect(cx - 2, cy - 5, 2, 1, BLACK);
        gfx->fillRect(cx + 4, cy - 3, 2, 1, BLACK);
        for (int x = cx - 15; x <= cx + 15; x++)                          // lettuce
            gfx->drawPixel(x, cy + 2 + (((x - cx + 16) / 2) % 2), col);
        gfx->fillRoundRect(cx - 15, cy + 5, 30, 6, 2, col);               // patty
        gfx->fillRoundRect(cx - 14, cy + 12, 28, 5, 2, col);              // bottom bun
        break;
    case 9: // Love: round heart with a small shine
        gfx->fillCircle(cx - 6, cy - 4, 8, col);
        gfx->fillCircle(cx + 6, cy - 4, 8, col);
        gfx->fillTriangle(cx - 14, cy - 1, cx + 14, cy - 1, cx, cy + 14, col);
        gfx->fillRect(cx - 10, cy - 8, 2, 4, BLACK);
        gfx->drawPixel(cx - 8, cy - 9, BLACK);
        gfx->drawPixel(cx - 7, cy - 10, BLACK);
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
