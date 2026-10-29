// ─────────────────────────────────────────────────────────────────────────────
// stocks.h — Stock quote types and provider-agnostic fetch interface
// ─────────────────────────────────────────────────────────────────────────────
// The rest of the firmware only talks to this header. The data provider lives
// in its own file (currently stocks_yahoo.cpp). To switch provider, write a
// new stocks_<name>.cpp that implements stocks_fetch_quote() and delete the
// old one — nothing else needs to change.
//
// Symbols are stored in Yahoo format (AAPL, NESN.SW, VWCE.DE, VWRL.L). A
// different provider should translate them to its own format internally.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once
#include <Arduino.h>

#define STOCKS_MAX 20

enum StockStatus : uint8_t {
    STOCK_PENDING = 0,  // never fetched successfully yet
    STOCK_OK,           // price/change_pct are valid
    STOCK_NOT_FOUND,    // provider doesn't know this symbol (typo / wrong suffix)
};

struct StockQuote {
    String      symbol;            // as configured, e.g. "NESN.SW"
    StockStatus status     = STOCK_PENDING;
    float       price      = 0;    // major currency units (pence already converted to pounds)
    float       change_pct = 0;    // % change vs previous close
    String      currency;          // e.g. "CHF" — shown in the portal Check only
    time_t      fetch_time = 0;    // epoch of last successful fetch (0 = never)
};

enum StockFetchResult {
    STOCK_FETCH_OK,         // out is filled
    STOCK_FETCH_NOT_FOUND,  // symbol unknown — don't keep retrying
    STOCK_FETCH_ERROR,      // network/parse error — retry later, out unchanged
};

// Page layout on the device (stored in NVS as an int — keep values stable).
enum StocksLayout {
    STOCKS_LAYOUT_AUTO    = 0,  // 1–5 tickers big, otherwise compact
    STOCKS_LAYOUT_BIG     = 1,  // always 5 per page, large font
    STOCKS_LAYOUT_COMPACT = 2,  // always 10 per page, two columns
};

// ── Provider-specific (stocks_yahoo.cpp) ─────────────────────────────────────

// Fetch one quote. Blocking HTTPS call — the caller must hold http_lock.
// Fills price, change_pct and currency; leaves symbol/status/fetch_time alone.
StockFetchResult stocks_fetch_quote(const String& symbol, StockQuote& out);

// ── Generic helpers (stocks.cpp) ─────────────────────────────────────────────

// Ticker without exchange suffix for display: "NESN.SW" → "NESN".
String stocks_display_symbol(const String& symbol);

// True on weekdays between STOCKS_OPEN_HOUR and STOCKS_CLOSE_HOUR local time.
// Returns false while the clock isn't synced yet.
bool stocks_market_open(time_t now);

// Tickers per page for a layout: 5 (big, one column) or 10 (compact, two columns).
int stocks_per_page(int layout, int count);
