// ─────────────────────────────────────────────────────────────────────────────
// stocks_yahoo.h — Yahoo response parsers (used by stocks_yahoo.cpp and the tests)
// ─────────────────────────────────────────────────────────────────────────────
// The rest of the firmware only uses include/stocks.h; these exist so the
// parsing can be tested against saved responses (test/test_parsers).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include "stocks.h"

// A /v8/finance/chart response → quote fields (price, change, name, ranges, spark).
// Doesn't set detail_time.
StockFetchResult stocks_yahoo_parse_quote(const String& body, const String& symbol, StockQuote& out);

// A /v8/finance/spark response → price/change/spark of quotes[start, end) whose
// symbol is in it (found[i] = true). Returns how many were found, -1 = bad JSON.
int stocks_yahoo_parse_spark(const String& body, StockQuote quotes[], int start, int end, bool found[]);
