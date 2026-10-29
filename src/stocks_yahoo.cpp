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
#include "../include/stocks_yahoo.h"
#include "../include/devlog.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <algorithm>

static const char* BASE_URL   = "https://query1.finance.yahoo.com/v8/finance/chart/";
static const char* USER_AGENT = "Mozilla/5.0 (Linux; Android 10) AppleWebKit/537.36 "
                                "(KHTML, like Gecko) Chrome/120.0.0.0 Mobile Safari/537.36";
static const int   TIMEOUT_MS = 8000;

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

// ── One kept-alive connection to Yahoo ───────────────────────────────────────
// Consecutive requests (startup, details, detail chart) reuse the open HTTPS
// connection instead of a new TLS handshake (~1.5–3 s each). All callers hold
// http_lock, so the shared client is never used concurrently. If Yahoo closed
// the idle connection, the request is retried once on a fresh one.
static WiFiClientSecure* s_wc   = nullptr;
static HTTPClient*       s_http = nullptr;

static int yahoo_get(const String& url, String& body) {
    if (!s_wc) {
        s_wc = new WiFiClientSecure();
        s_wc->setInsecure();
        s_http = new HTTPClient();
        s_http->setReuse(true);
        s_http->setTimeout(TIMEOUT_MS);
    }
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!s_http->begin(*s_wc, url)) return -1;
        s_http->addHeader("User-Agent", USER_AGENT);
        s_http->addHeader("Accept",     "application/json");
        int code = s_http->GET();
        if (code > 0) {
            body = (code == 200) ? s_http->getString() : "";
            s_http->end();               // keeps the connection open for the next request
            return code;
        }
        s_http->end();
        s_wc->stop();                    // stale keep-alive socket — start fresh
    }
    return -1;
}

static StockFetchResult parse_chart_doc(const String& body, const String& symbol, JsonDocument& doc);
static void fill_quote(JsonDocument& doc, StockQuote& out);

// GET the chart endpoint at the given resolution ("15m", "5m") and parse the
// fields we use into doc. Shared by the details fetch and the detail chart.
static StockFetchResult fetch_chart_doc(const String& symbol, const char* interval,
                                        JsonDocument& doc) {
    String url = String(BASE_URL) + url_encode(symbol) + "?range=1d&interval=" + interval;
    String body;   // ~3 KB (15m) / ~8 KB (5m), String is fine here
    int code = yahoo_get(url, body);
    if (code == 404) {
        Log.printf("[Stocks] %s: not found\n", symbol.c_str());
        return STOCK_FETCH_NOT_FOUND;
    }
    if (code != 200) {
        Log.printf("[Stocks] %s: HTTP %d\n", symbol.c_str(), code);
        return STOCK_FETCH_ERROR;
    }
    return parse_chart_doc(body, symbol, doc);
}

// Parse a chart response into doc (only the fields we use)
static StockFetchResult parse_chart_doc(const String& body, const String& symbol, JsonDocument& doc) {
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
        Log.printf("[Stocks] %s: JSON error: %s\n", symbol.c_str(), err.c_str());
        return STOCK_FETCH_ERROR;
    }
    if (String(doc["chart"]["error"]["code"] | "") == "Not Found") return STOCK_FETCH_NOT_FOUND;

    JsonObject meta = doc["chart"]["result"][0]["meta"];
    if (meta.isNull() || !meta["regularMarketPrice"].is<float>()) {
        Log.printf("[Stocks] %s: no price in response\n", symbol.c_str());
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
    fill_quote(doc, out);
    out.detail_time = time(nullptr);
    Log.printf("[Stocks] %s: %.2f %s (%+.2f%%)\n",
                  symbol.c_str(), out.price, out.currency.c_str(), out.change_pct);
    return STOCK_FETCH_OK;
}

StockFetchResult stocks_yahoo_parse_quote(const String& body, const String& symbol, StockQuote& out) {
    JsonDocument doc;
    StockFetchResult r = parse_chart_doc(body, symbol, doc);
    if (r == STOCK_FETCH_OK) fill_quote(doc, out);
    return r;
}

// Quote fields from a parsed chart response (prices in pounds for London pence)
static void fill_quote(JsonDocument& doc, StockQuote& out) {
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
    out.price_scale = scale;
}

// ── Batch: many tickers per request ──────────────────────────────────────────
// /v8/finance/spark?symbols=A,B,…&range=1d&interval=15m returns, per symbol,
// "close" (15-min bars) and "chartPreviousClose" — ~0.7 KB per ticker, no
// currency/name. Unknown symbols are simply missing. London tickers need their
// pence/pound scale from an earlier details fetch, so they wait for that.

static const int BATCH_SIZE = 10;

static bool needs_scale(const StockQuote& q) {
    return q.price_scale == 0 && q.symbol.endsWith(".L");
}

int stocks_yahoo_parse_spark(const String& body, StockQuote quotes[], int start, int end, bool found[]) {
    JsonDocument doc;
    if (deserializeJson(doc, body)) return -1;
    int got = 0;
    for (int i = start; i < end; i++) {
        StockQuote& q = quotes[i];
        JsonObject o = doc[q.symbol];
        if (o.isNull() || needs_scale(q)) continue;
        float scale = q.price_scale > 0 ? q.price_scale : 1.0f;
        JsonArray closes = o["close"];
        float prev = o["chartPreviousClose"] | 0.0f;
        if (prev <= 0) prev = o["previousClose"] | 0.0f;
        prev *= scale;
        // Current price = latest bar's close (skipping empty bars)
        float price = 0;
        for (int k = (int)closes.size() - 1; k >= 0; k--)
            if (!closes[k].isNull()) { price = closes[k].as<float>() * scale; break; }
        if (price <= 0) continue;
        q.price      = price;
        q.prev_close = prev;
        q.change_pct = (prev > 0) ? (price - prev) / prev * 100.0f : 0.0f;
        q.spark_n    = downsample(closes, scale, prev > 0 ? prev : price, q.spark, STOCK_SPARK_POINTS);
        found[i] = true;
        got++;
    }
    return got;
}

StockFetchResult stocks_fetch_batch(StockQuote quotes[], int n, bool found[]) {
    for (int i = 0; i < n; i++) found[i] = false;
    bool any_ok = false;

    for (int start = 0; start < n; start += BATCH_SIZE) {
        int end = std::min(n, start + BATCH_SIZE);
        String syms;
        for (int i = start; i < end; i++) {
            if (needs_scale(quotes[i])) continue;
            if (syms.length()) syms += ",";
            syms += url_encode(quotes[i].symbol);
        }
        if (syms.isEmpty()) continue;

        String body;
        int code = yahoo_get("https://query1.finance.yahoo.com/v8/finance/spark?range=1d&interval=15m&symbols="
                             + syms, body);
        if (code != 200) { Log.printf("[Stocks] batch HTTP %d\n", code); continue; }
        int got = stocks_yahoo_parse_spark(body, quotes, start, end, found);
        if (got < 0) { Log.println("[Stocks] batch JSON error"); continue; }
        any_ok |= got > 0;
    }
    int got = 0;
    for (int i = 0; i < n; i++) got += found[i];
    Log.printf("[Stocks] batch: %d of %d tickers\n", got, n);
    return any_ok ? STOCK_FETCH_OK : STOCK_FETCH_ERROR;
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
    Log.printf("[Stocks] %s: detail chart %d points\n", symbol.c_str(), out.n);
    return STOCK_FETCH_OK;
}
