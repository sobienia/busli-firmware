// ─────────────────────────────────────────────────────────────────────────────
// stocks.cpp — Provider-independent stock helpers
// ─────────────────────────────────────────────────────────────────────────────
// Everything here is independent of where quotes come from. The HTTP/JSON side
// lives in stocks_yahoo.cpp.
// ─────────────────────────────────────────────────────────────────────────────

#include "../include/stocks.h"
#include "../include/config.h"

String stocks_display_symbol(const String& symbol) {
    int dot = symbol.lastIndexOf('.');
    return (dot > 0) ? symbol.substring(0, dot) : symbol;
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
