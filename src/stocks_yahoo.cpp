// ─────────────────────────────────────────────────────────────────────────────
// stocks_yahoo.cpp — Yahoo Finance quote provider (unofficial chart API)
// ─────────────────────────────────────────────────────────────────────────────
// API: https://query1.finance.yahoo.com/v8/finance/chart/{symbol}?range=1d&interval=1d
// No key needed. ~1.3 KB per symbol; we only read chart.result[0].meta:
//   regularMarketPrice  — last price (or last close outside trading hours)
//   chartPreviousClose  — previous session close → daily % change
//   currency            — "GBp" means the price is in pence
// Unknown symbols return HTTP 404 with chart.error.code = "Not Found".
//
// Yahoo is unofficial and has changed before. If it breaks, replace this file
// with another implementation of stocks_fetch_quote() (see stocks.h).
// ─────────────────────────────────────────────────────────────────────────────

#include "../include/stocks.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

static const char* BASE_URL   = "https://query1.finance.yahoo.com/v8/finance/chart/";
static const char* USER_AGENT = "Mozilla/5.0 (Linux; Android 10) AppleWebKit/537.36 "
                                "(KHTML, like Gecko) Chrome/120.0.0.0 Mobile Safari/537.36";
static const int   TIMEOUT_MS = 10000;

// Symbols can contain '^' (indices) or '=' (FX), which must be escaped in the path.
static String url_encode(const String& s) {
    String out;
    out.reserve(s.length() + 8);
    for (int i = 0; i < (int)s.length(); i++) {
        char c = s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", (uint8_t)c);
            out += buf;
        }
    }
    return out;
}

StockFetchResult stocks_fetch_quote(const String& symbol, StockQuote& out) {
    String url = String(BASE_URL) + url_encode(symbol) + "?range=1d&interval=1d";

    WiFiClientSecure wc;
    wc.setInsecure();
    HTTPClient http;
    http.setTimeout(TIMEOUT_MS);
    http.setReuse(false);
    if (!http.begin(wc, url)) {
        Serial.printf("[Stocks] %s: http.begin() failed\n", symbol.c_str());
        return STOCK_FETCH_ERROR;
    }
    http.addHeader("User-Agent", USER_AGENT);
    http.addHeader("Accept",     "application/json");

    int code = http.GET();
    if (code == 404) {
        http.end();
        Serial.printf("[Stocks] %s: not found\n", symbol.c_str());
        return STOCK_FETCH_NOT_FOUND;
    }
    if (code != 200) {
        http.end();
        Serial.printf("[Stocks] %s: HTTP %d\n", symbol.c_str(), code);
        return STOCK_FETCH_ERROR;
    }
    String body = http.getString();  // small (~1.3 KB), String is fine here
    http.end();

    // Only keep the meta fields we need — keeps the JSON document tiny.
    JsonDocument filter;
    JsonObject fm = filter["chart"]["result"][0]["meta"].to<JsonObject>();
    fm["regularMarketPrice"] = true;
    fm["chartPreviousClose"] = true;
    fm["currency"]           = true;
    filter["chart"]["error"]["code"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    if (err) {
        Serial.printf("[Stocks] %s: JSON error: %s\n", symbol.c_str(), err.c_str());
        return STOCK_FETCH_ERROR;
    }

    if (String(doc["chart"]["error"]["code"] | "") == "Not Found") return STOCK_FETCH_NOT_FOUND;

    JsonObject meta = doc["chart"]["result"][0]["meta"];
    if (meta.isNull() || !meta["regularMarketPrice"].is<float>()) {
        Serial.printf("[Stocks] %s: no price in response\n", symbol.c_str());
        return STOCK_FETCH_ERROR;
    }

    float  price    = meta["regularMarketPrice"];
    float  prev     = meta["chartPreviousClose"] | 0.0f;
    String currency = meta["currency"] | "";

    // London quotes many instruments in pence ("GBp"/"GBX") — convert to pounds.
    // The % change is unaffected because price and previous close scale together.
    if (currency == "GBp" || currency == "GBX") {
        price   /= 100.0f;
        prev    /= 100.0f;
        currency = "GBP";
    }

    out.price      = price;
    out.change_pct = (prev > 0) ? (price - prev) / prev * 100.0f : 0.0f;
    out.currency   = currency;

    Serial.printf("[Stocks] %s: %.2f %s (%+.2f%%)\n",
                  symbol.c_str(), out.price, out.currency.c_str(), out.change_pct);
    return STOCK_FETCH_OK;
}
