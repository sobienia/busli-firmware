// ─────────────────────────────────────────────────────────────────────────────
// stocks_yahoo.cpp — Yahoo Finance quote provider (unofficial chart API)
// ─────────────────────────────────────────────────────────────────────────────
// API: https://query1.finance.yahoo.com/v8/finance/chart/{symbol}?range=1d&interval=15m|5m
// No key needed. ~3 KB per symbol (15m, list) / ~8 KB (5m, detail chart).
// We read chart.result[0]:
//   meta.regularMarketPrice        — last price (or last close outside trading hours)
//   meta.chartPreviousClose        — previous session close → daily % change
//   meta.currency                  — "GBp" means the price is in pence
//   meta.shortName, regularMarketDayHigh/Low, fiftyTwoWeekHigh/Low — detail view
//   indicators.quote[0].close/open — intraday bars of today's (or the last) session
// Unknown symbols return HTTP 404 with chart.error.code = "Not Found".
//
// Yahoo is unofficial and has changed before. If it breaks, replace this file
// with another implementation of stocks_fetch_quote()/stocks_fetch_chart().
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

// GET the chart endpoint at the given resolution ("15m", "5m") and parse the
// fields we use into doc. Shared by the list quote and the detail chart.
static StockFetchResult fetch_chart_doc(const String& symbol, const char* interval,
                                        JsonDocument& doc) {
    String url = String(BASE_URL) + url_encode(symbol) + "?range=1d&interval=" + interval;

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
    String body = http.getString();  // ~3 KB (15m) / ~8 KB (5m), String is fine here
    http.end();

    // Only keep the fields we need — keeps the JSON document small.
    JsonDocument filter;
    JsonObject fm = filter["chart"]["result"][0]["meta"].to<JsonObject>();
    for (const char* k : { "regularMarketPrice", "chartPreviousClose", "currency", "shortName",
                           "regularMarketDayHigh", "regularMarketDayLow",
                           "fiftyTwoWeekHigh", "fiftyTwoWeekLow" })
        fm[k] = true;
    filter["chart"]["result"][0]["indicators"]["quote"][0]["close"] = true;
    filter["chart"]["result"][0]["indicators"]["quote"][0]["open"]  = true;
    filter["chart"]["error"]["code"] = true;

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
    return STOCK_FETCH_OK;
}

// London quotes many instruments in pence ("GBp"/"GBX") — prices are scaled to
// pounds. Percentages are unaffected because everything scales together.
static float price_scale(const String& currency) {
    return (currency == "GBp" || currency == "GBX") ? 0.01f : 1.0f;
}

// Evenly downsample closes into out[max]; gaps (null) repeat the previous value
// so the line stays continuous. Returns the number of points written.
static int downsample(JsonArray closes, float scale, float start, float* out, int max) {
    int n = closes.size();
    if (n < 2) return 0;
    int points = (n < max) ? n : max;
    float last = start;
    for (int i = 0; i < points; i++) {
        int src = i * (n - 1) / (points - 1);
        JsonVariant v = closes[src];
        if (!v.isNull()) last = v.as<float>() * scale;
        out[i] = last;
    }
    return points;
}

StockFetchResult stocks_fetch_quote(const String& symbol, StockQuote& out) {
    JsonDocument doc;
    StockFetchResult r = fetch_chart_doc(symbol, "15m", doc);
    if (r != STOCK_FETCH_OK) return r;

    JsonObject meta   = doc["chart"]["result"][0]["meta"];
    JsonObject quote  = doc["chart"]["result"][0]["indicators"]["quote"][0];
    String currency   = meta["currency"] | "";
    float  scale      = price_scale(currency);
    if (scale != 1.0f) currency = "GBP";

    float price = meta["regularMarketPrice"].as<float>() * scale;
    float prev  = (meta["chartPreviousClose"] | 0.0f) * scale;

    out.price      = price;
    out.change_pct = (prev > 0) ? (price - prev) / prev * 100.0f : 0.0f;
    out.currency   = currency;
    out.prev_close = prev;
    out.name       = String(meta["shortName"] | "");
    out.day_high   = (meta["regularMarketDayHigh"] | 0.0f) * scale;
    out.day_low    = (meta["regularMarketDayLow"]  | 0.0f) * scale;
    out.year_high  = (meta["fiftyTwoWeekHigh"]     | 0.0f) * scale;
    out.year_low   = (meta["fiftyTwoWeekLow"]      | 0.0f) * scale;
    // Yahoo doesn't report the open in meta — it's the first bar's open
    JsonVariant first_open = quote["open"][0];
    out.open = first_open.isNull() ? 0.0f : first_open.as<float>() * scale;

    out.spark_n = downsample(quote["close"], scale, prev > 0 ? prev : price,
                             out.spark, STOCK_SPARK_POINTS);

    Serial.printf("[Stocks] %s: %.2f %s (%+.2f%%)\n",
                  symbol.c_str(), out.price, out.currency.c_str(), out.change_pct);
    return STOCK_FETCH_OK;
}

StockFetchResult stocks_fetch_chart(const String& symbol, StockChart& out) {
    JsonDocument doc;
    StockFetchResult r = fetch_chart_doc(symbol, "5m", doc);
    if (r != STOCK_FETCH_OK) return r;

    JsonObject meta = doc["chart"]["result"][0]["meta"];
    float scale     = price_scale(String(meta["currency"] | ""));
    float prev      = (meta["chartPreviousClose"] | 0.0f) * scale;
    float price     = meta["regularMarketPrice"].as<float>() * scale;
    out.prev_close  = prev;
    out.n = downsample(doc["chart"]["result"][0]["indicators"]["quote"][0]["close"], scale,
                       prev > 0 ? prev : price, out.pts, STOCK_CHART_POINTS);
    Serial.printf("[Stocks] %s: detail chart %d points\n", symbol.c_str(), out.n);
    return STOCK_FETCH_OK;
}
