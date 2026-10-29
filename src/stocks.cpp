// ─────────────────────────────────────────────────────────────────────────────
// stocks.cpp — Provider-independent stock helpers
// ─────────────────────────────────────────────────────────────────────────────
// Everything here is independent of where quotes come from. The HTTP/JSON side
// lives in stocks_yahoo.cpp.
// ─────────────────────────────────────────────────────────────────────────────

#include "../include/stocks.h"
#include "../include/config.h"

// Coins are quoted as "<coin>-<fiat currency>" ("BTC-USD"); other tickers with
// a dash ("BRK-B") are shares
static int crypto_dash(const String& symbol) {
    int dash = symbol.lastIndexOf('-');
    if (dash <= 0) return -1;
    String fiat = symbol.substring(dash + 1);
    return (fiat == "USD" || fiat == "EUR" || fiat == "CHF" || fiat == "GBP") ? dash : -1;
}

bool stocks_is_crypto(const String& symbol) { return crypto_dash(symbol) > 0; }

String stocks_display_symbol(const String& symbol) {
    static const struct { const char* key; const char* name; } NAMES[] = {
        { "GC=F", "Gold" }, { "SI=F", "Silver" },
        { "BTC", "BTC" }, { "ETH", "ETH" }, { "XRP", "XRP" },
    };
    int dash = crypto_dash(symbol);
    String base = (dash > 0) ? symbol.substring(0, dash) : symbol;   // coin without its currency
    for (const auto& n : NAMES)
        if (base == n.key) return n.name;
    if (dash > 0) return base;
    if (symbol.endsWith("=X")) return symbol.substring(0, symbol.length() - 2);   // currency pair
    int dot = symbol.lastIndexOf('.');
    return (dot > 0) ? symbol.substring(0, dot) : symbol;
}

String stocks_fx_symbol(const String& from, const String& to) {
    return from + to + "=X";
}

int stocks_session_state(const StockQuote& q, time_t now) {
    if (q.always_open) return 1;
    if (q.session_open < 0 || q.session_close < 0 || now < 1700000000) return -1;
    time_t local = now + q.gmt_offset;            // the exchange's own clock
    struct tm t;
    gmtime_r(&local, &t);
    if (t.tm_wday == 0 || t.tm_wday == 6) return 0;   // weekend
    int m = t.tm_hour * 60 + t.tm_min;
    int open = q.session_open, close = q.session_close;
    if (close == 1439) close = 1440;              // "until 23:59" = all day
    return (open <= close) ? (m >= open && m < close) : (m >= open || m < close);
}

void stocks_convert(StockQuote& q, float rate, const char* currency) {
    q.price     *= rate;  q.prev_close *= rate;  q.open *= rate;
    q.day_low   *= rate;  q.day_high   *= rate;
    q.year_low  *= rate;  q.year_high  *= rate;
    for (int i = 0; i < q.spark_n; i++) q.spark[i] *= rate;
    q.currency = currency;
}

void stocks_convert_chart(StockChart& c, float rate) {
    c.prev_close *= rate;
    for (int i = 0; i < c.n; i++) c.pts[i] *= rate;
}

bool stocks_market_open(time_t now) {
    if (now < 1700000000) return false;  // clock not synced yet
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_wday == 0 || t.tm_wday == 6) return false;  // Sunday / Saturday
    return t.tm_hour >= STOCKS_OPEN_HOUR && t.tm_hour < STOCKS_CLOSE_HOUR;
}

int stocks_per_page(int layout, int count) {
    if (layout == STOCKS_LAYOUT_BIG)     return 5;
    if (layout == STOCKS_LAYOUT_COMPACT) return 10;
    return (count <= 5) ? 5 : 10;
}
