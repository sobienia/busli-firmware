// ─────────────────────────────────────────────────────────────────────────────
// test_main.cpp — parser tests, run on the device (see test/README.md)
// ─────────────────────────────────────────────────────────────────────────────
// Two kinds of tests:
//   • "live": real responses saved by scripts/update_test_fixtures.py — catch a
//     provider changing its format. Values change daily, so only their shape
//     and plausible ranges are checked.
//   • hand-written responses with known answers — check our own logic
//     (filters, delays, platform changes, rain alert, pence → pounds, …).
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <unity.h>
#include "../../src/api.h"
#include "../../src/weather.h"
#include "../../include/stocks_yahoo.h"
#include "../../include/today_data.h"
#include "../../include/commute.h"
#include "fixtures.h"

SET_LOOP_TASK_STACK_SIZE(16 * 1024);   // same as the firmware

// Skip a live test whose sample couldn't be downloaded yet
#define NEED(fx) do { if ((fx)[0] == '\0') { TEST_IGNORE_MESSAGE(#fx " not downloaded"); return; } } while (0)

static const time_t T0 = 1793293200;   // 2026-10-29 17:00:00 UTC = 18:00 in Zürich

void setUp() {}
void tearDown() {}

// ── Departures ───────────────────────────────────────────────────────────────

static const char BOARD[] = R"({"stationboard":[
 {"number":"4","category":"T","to":"Zürich, Altstetten",
  "stop":{"departure":"2026-10-29T18:00:00+0100","platform":null,
          "prognosis":{"departure":"2026-10-29T18:02:00+0100","platform":null}}},
 {"number":"31","category":"B","to":"Zürich, Hegibachplatz",
  "stop":{"departure":"2026-10-29T18:05:00+0100","platform":"A",
          "prognosis":{"departure":null,"platform":"C"}}},
 {"number":"12","category":"S","to":"Brugg AG",
  "stop":{"departure":"2026-10-29T18:10:00+0100","platform":"3",
          "prognosis":{"departure":null,"platform":"3"}}}]})";

static StopConfig stop_with(const char** lines, int nl, const char** dirs, int nd) {
    return { "Test", "Test", dirs, nd, lines, nl };
}

static void test_departures_all() {
    std::vector<Departure> d;
    StopConfig all = stop_with(nullptr, 0, nullptr, 0);
    TEST_ASSERT_TRUE(api_parse_departures(BOARD, strlen(BOARD), all, d, 10, 0));
    TEST_ASSERT_EQUAL(3, (int)d.size());
    TEST_ASSERT_EQUAL_STRING("4", d[0].line.c_str());
    TEST_ASSERT_EQUAL_STRING("Altstetten", d[0].destination.c_str());   // city prefix removed
    TEST_ASSERT_EQUAL(T0 + 120, (long)d[0].dep_time);                    // the live time
    TEST_ASSERT_EQUAL(2, d[0].delay);
    TEST_ASSERT_EQUAL_STRING("C", d[1].platform.c_str());                // live platform wins
    TEST_ASSERT_TRUE(d[1].platform_changed);
    TEST_ASSERT_EQUAL_STRING("S12", d[2].line.c_str());                  // train: category + number
    TEST_ASSERT_FALSE(d[2].platform_changed);
}

static void test_departures_filters() {
    std::vector<Departure> d;
    const char* s12[] = { "S12" };
    StopConfig by_line = stop_with(s12, 1, nullptr, 0);
    TEST_ASSERT_TRUE(api_parse_departures(BOARD, strlen(BOARD), by_line, d, 10, 0));
    TEST_ASSERT_EQUAL(1, (int)d.size());
    TEST_ASSERT_EQUAL_STRING("S12", d[0].line.c_str());

    const char* hegi[] = { "hegi" };                                     // case-insensitive part
    StopConfig by_dir = stop_with(nullptr, 0, hegi, 1);
    TEST_ASSERT_TRUE(api_parse_departures(BOARD, strlen(BOARD), by_dir, d, 10, 0));
    TEST_ASSERT_EQUAL(1, (int)d.size());
    TEST_ASSERT_EQUAL_STRING("31", d[0].line.c_str());
}

static void test_departures_past_and_limit() {
    std::vector<Departure> d;
    StopConfig all = stop_with(nullptr, 0, nullptr, 0);
    // 18:03: the tram (18:02) has left
    TEST_ASSERT_TRUE(api_parse_departures(BOARD, strlen(BOARD), all, d, 10, T0 + 180));
    TEST_ASSERT_EQUAL(2, (int)d.size());
    TEST_ASSERT_EQUAL_STRING("31", d[0].line.c_str());
    TEST_ASSERT_TRUE(api_parse_departures(BOARD, strlen(BOARD), all, d, 1, 0));
    TEST_ASSERT_EQUAL(1, (int)d.size());
}

static void test_departures_live() {
    NEED(FX_STATIONBOARD);
    std::vector<Departure> d;
    StopConfig all = stop_with(nullptr, 0, nullptr, 0);
    TEST_ASSERT_TRUE(api_parse_departures(FX_STATIONBOARD, strlen(FX_STATIONBOARD), all, d, 20, 0));
    TEST_ASSERT_GREATER_THAN(0, (int)d.size());
    for (const Departure& x : d) {
        TEST_ASSERT_TRUE(x.line.length() > 0);
        TEST_ASSERT_TRUE(x.destination.length() > 0);
        TEST_ASSERT_TRUE(x.dep_time > 1700000000);
    }
}

// ── Weather ──────────────────────────────────────────────────────────────────

static String weather_json(float current_mm, int daily_code, const char* slot_mm) {
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"current\":{\"temperature_2m\":11.4,\"weather_code\":3,\"uv_index\":1.6,\"precipitation\":%.1f},"
        "\"daily\":{\"weather_code\":[%d],\"precipitation_probability_max\":[40],"
        "\"temperature_2m_max\":[13.6],\"uv_index_max\":[2.4]},"
        "\"minutely_15\":{\"time\":[%ld,%ld,%ld],\"precipitation\":[%s]}}",
        current_mm, daily_code, (long)(T0 + 900), (long)(T0 + 1800), (long)(T0 + 2700), slot_mm);
    return buf;
}

static void test_weather_values_and_codes() {
    WeatherData w = {};
    TEST_ASSERT_TRUE(weather_parse(weather_json(0, 61, "0,0,0"), T0, w));
    TEST_ASSERT_TRUE(w.valid);
    TEST_ASSERT_FLOAT_WITHIN(0.01, 11.4, w.temp_c);
    TEST_ASSERT_FLOAT_WITHIN(0.01, 13.6, w.temp_max_c);
    TEST_ASSERT_EQUAL(2, w.uv_index);          // rounded
    TEST_ASSERT_EQUAL(2, w.uv_index_max);
    TEST_ASSERT_EQUAL(40, w.precip_prob_pct);
    TEST_ASSERT_TRUE(w.rain_today);            // code 61 = rain
    TEST_ASSERT_TRUE(weather_parse(weather_json(0, 73, "0,0,0"), T0, w));
    TEST_ASSERT_TRUE(w.snow_today);
    TEST_ASSERT_FALSE(w.rain_today);           // snow, not rain
    TEST_ASSERT_TRUE(weather_parse(weather_json(0, 0, "0,0,0"), T0, w));
    TEST_ASSERT_TRUE(w.clear_today);
}

static void test_weather_rain_alert() {
    WeatherData w = {};
    // Dry now, wet from the second slot (which covers 18:15–18:30) → "rain from 18:15"
    TEST_ASSERT_TRUE(weather_parse(weather_json(0, 3, "0,0.5,0.5"), T0, w));
    TEST_ASSERT_FALSE(w.raining_now);
    TEST_ASSERT_EQUAL((long)(T0 + 900), (long)w.rain_start);
    // Raining now, dry from the second slot → "rain until 18:15"
    TEST_ASSERT_TRUE(weather_parse(weather_json(0.3, 3, "0.2,0,0"), T0, w));
    TEST_ASSERT_TRUE(w.raining_now);
    TEST_ASSERT_EQUAL((long)(T0 + 900), (long)w.rain_end);
    TEST_ASSERT_EQUAL(0, (long)w.rain_start);
}

static void test_weather_live() {
    NEED(FX_WEATHER);
    WeatherData w = {};
    TEST_ASSERT_TRUE(weather_parse(FX_WEATHER, time(nullptr), w));
    TEST_ASSERT_TRUE(w.temp_c > -40 && w.temp_c < 50);
    TEST_ASSERT_TRUE(w.temp_max_c > -40 && w.temp_max_c < 50);
    TEST_ASSERT_TRUE(w.uv_index_max >= 0 && w.uv_index_max <= 15);
    TEST_ASSERT_TRUE(w.precip_prob_pct >= 0 && w.precip_prob_pct <= 100);
}

static void test_weather_detail_live() {
    NEED(FX_WEATHER_DETAIL);
    WeatherDetail d;
    TEST_ASSERT_TRUE(weather_parse_detail(FX_WEATHER_DETAIL, d));
    TEST_ASSERT_EQUAL(WEATHER_HOURS, d.hours);
    TEST_ASSERT_TRUE(d.sunrise > 1700000000 && d.sunrise < d.sunset);
    TEST_ASSERT_TRUE(d.temp_min_c <= d.temp_max_c);
    TEST_ASSERT_TRUE(d.hour_time[1] > d.hour_time[0]);
}

static void test_air_live() {
    NEED(FX_AIR);
    WeatherDetail d;
    for (float& p : d.pollen) p = -2;
    TEST_ASSERT_TRUE(weather_parse_air(FX_AIR, d));
    TEST_ASSERT_TRUE(d.aqi >= -1 && d.aqi < 500);
    for (float p : d.pollen) TEST_ASSERT_TRUE(p >= -1);   // every type read (or "not reported")
}

static void test_weather_bad_json() {
    WeatherData w = {};
    WeatherDetail d;
    TEST_ASSERT_FALSE(weather_parse("<html>Bad gateway</html>", T0, w));
    TEST_ASSERT_FALSE(weather_parse_detail("{\"error\":true}", d));
}

// ── Stocks (Yahoo) ───────────────────────────────────────────────────────────

static void test_stock_chart_live() {
    NEED(FX_YAHOO_CHART);
    StockQuote q;
    TEST_ASSERT_EQUAL(STOCK_FETCH_OK, stocks_yahoo_parse_quote(FX_YAHOO_CHART, "NESN.SW", q));
    TEST_ASSERT_TRUE(q.price > 0);
    TEST_ASSERT_EQUAL_STRING("CHF", q.currency.c_str());
    TEST_ASSERT_TRUE(q.name.length() > 0);
    TEST_ASSERT_TRUE(q.prev_close > 0);
    TEST_ASSERT_TRUE(q.year_low > 0 && q.year_low <= q.year_high);
    TEST_ASSERT_GREATER_THAN(1, q.spark_n);
    TEST_ASSERT_FLOAT_WITHIN(0.0001, 1.0, q.price_scale);
}

static void test_stock_chart_pence_live() {
    NEED(FX_YAHOO_CHART_GBP);
    StockQuote q;
    TEST_ASSERT_EQUAL(STOCK_FETCH_OK, stocks_yahoo_parse_quote(FX_YAHOO_CHART_GBP, "VOD.L", q));
    TEST_ASSERT_EQUAL_STRING("GBP", q.currency.c_str());   // pence → pounds
    TEST_ASSERT_FLOAT_WITHIN(0.0001, 0.01, q.price_scale);
    TEST_ASSERT_TRUE(q.price > 0 && q.price < 100);          // Vodafone trades around £1
}

static void test_stock_not_found() {
    StockQuote q;
    const char* body = "{\"chart\":{\"result\":null,\"error\":{\"code\":\"Not Found\","
                       "\"description\":\"No data found, symbol may be delisted\"}}}";
    TEST_ASSERT_EQUAL(STOCK_FETCH_NOT_FOUND, stocks_yahoo_parse_quote(body, "XXXXZZ", q));
    TEST_ASSERT_EQUAL(STOCK_FETCH_ERROR, stocks_yahoo_parse_quote("not json", "X", q));
}

static void test_stock_spark_live() {
    NEED(FX_YAHOO_SPARK);
    StockQuote q[3];
    q[0].symbol = "NESN.SW";
    q[1].symbol = "AAPL";
    q[2].symbol = "NOPE.SW";       // not in the response
    bool found[3];
    for (bool& f : found) f = false;
    TEST_ASSERT_EQUAL(2, stocks_yahoo_parse_spark(FX_YAHOO_SPARK, q, 0, 3, found));
    TEST_ASSERT_TRUE(found[0] && found[1]);
    TEST_ASSERT_FALSE(found[2]);
    TEST_ASSERT_TRUE(q[0].price > 0 && q[1].price > 0);
    TEST_ASSERT_GREATER_THAN(1, q[0].spark_n);
}

static void test_stock_spark_logic() {
    // Latest non-empty bar is the price; a London ticker without a known scale waits
    const char* body = "{\"ABC\":{\"close\":[10,11,null],\"chartPreviousClose\":10},"
                       "\"VOD.L\":{\"close\":[110,112],\"chartPreviousClose\":100}}";
    StockQuote q[2];
    q[0].symbol = "ABC";
    q[1].symbol = "VOD.L";         // price_scale 0 = never had its details fetched
    bool found[2] = { false, false };
    TEST_ASSERT_EQUAL(1, stocks_yahoo_parse_spark(body, q, 0, 2, found));
    TEST_ASSERT_FLOAT_WITHIN(0.001, 11, q[0].price);
    TEST_ASSERT_FLOAT_WITHIN(0.01, 10, q[0].change_pct);
    TEST_ASSERT_FALSE(found[1]);
    q[1].price_scale = 0.01f;      // known to be in pence → scaled
    TEST_ASSERT_EQUAL(2, stocks_yahoo_parse_spark(body, q, 0, 2, found));
    TEST_ASSERT_FLOAT_WITHIN(0.001, 1.12, q[1].price);
    TEST_ASSERT_FLOAT_WITHIN(0.01, 12, q[1].change_pct);
}

// ── Today page ───────────────────────────────────────────────────────────────

static void test_water_live() {
    NEED(FX_LAKE);
    NEED(FX_RIVER);
    float lake = water_parse_lake(FX_LAKE), river = water_parse_river(FX_RIVER);
    TEST_ASSERT_TRUE(lake > 0 && lake < 35);
    TEST_ASSERT_TRUE(river > 0 && river < 35);
    TEST_ASSERT_TRUE(isnan(water_parse_lake("{}")));
}

static void test_quote_live() {
    NEED(FX_QUOTE);
    DailyQuote q;
    TEST_ASSERT_TRUE(quote_parse(FX_QUOTE, q));
    TEST_ASSERT_TRUE(q.text.length() > 10);
    TEST_ASSERT_TRUE(q.author.length() > 0);
}

// ── Commute ──────────────────────────────────────────────────────────────────

static const char CONNECTIONS[] = R"({"connections":[{"sections":[
 {"journey":{"name":"IC 5","category":"IC","number":"5"},
  "departure":{"station":{"name":"Zürich HB"},"departure":"2026-10-29T18:02:00+0100"},
  "arrival":{"station":{"name":"Bern"},"arrival":"2026-10-29T18:58:00+0100"}},
 {"journey":null,
  "departure":{"station":{"name":"Bern"},"departure":null},
  "arrival":{"station":{"name":"Bern, Bahnhof"},"arrival":null}},
 {"journey":{"name":"41994","category":"B","number":"10"},
  "departure":{"station":{"name":"Bern, Bahnhof"},"departure":"2026-10-29T19:05:00+0100"},
  "arrival":{"station":{"name":"Bern, Zytglogge"},"arrival":"2026-10-29T19:09:00+0100"}}]}]})";

static void test_commute_legs() {
    CommuteData c = {};
    TEST_ASSERT_TRUE(commute_parse(CONNECTIONS, strlen(CONNECTIONS), T0, c));
    TEST_ASSERT_EQUAL(1, c.connection_count);
    TEST_ASSERT_EQUAL(2, c.connections[0].leg_count);                       // walk skipped
    TEST_ASSERT_EQUAL_STRING("IC5", c.connections[0].legs[0].line.c_str());
    TEST_ASSERT_EQUAL_STRING("10", c.connections[0].legs[1].line.c_str());  // not the trip no.
    TEST_ASSERT_EQUAL((long)(T0 + 120), (long)c.connections[0].dep_time);
    // An hour later it has left
    TEST_ASSERT_FALSE(commute_parse(CONNECTIONS, strlen(CONNECTIONS), T0 + 3600, c));
}

static void test_commute_live() {
    NEED(FX_CONNECTIONS);
    CommuteData c = {};
    TEST_ASSERT_TRUE(commute_parse(FX_CONNECTIONS, strlen(FX_CONNECTIONS), 0, c));
    TEST_ASSERT_GREATER_THAN(0, c.connection_count);
    TEST_ASSERT_GREATER_THAN(0, c.connections[0].leg_count);
}

// ── Runner ───────────────────────────────────────────────────────────────────

void setup() {
    delay(2000);   // give the PC time to open the USB serial port
    UNITY_BEGIN();
    RUN_TEST(test_departures_all);
    RUN_TEST(test_departures_filters);
    RUN_TEST(test_departures_past_and_limit);
    RUN_TEST(test_departures_live);
    RUN_TEST(test_weather_values_and_codes);
    RUN_TEST(test_weather_rain_alert);
    RUN_TEST(test_weather_live);
    RUN_TEST(test_weather_detail_live);
    RUN_TEST(test_air_live);
    RUN_TEST(test_weather_bad_json);
    RUN_TEST(test_stock_chart_live);
    RUN_TEST(test_stock_chart_pence_live);
    RUN_TEST(test_stock_not_found);
    RUN_TEST(test_stock_spark_live);
    RUN_TEST(test_stock_spark_logic);
    RUN_TEST(test_water_live);
    RUN_TEST(test_quote_live);
    RUN_TEST(test_commute_legs);
    RUN_TEST(test_commute_live);
    UNITY_END();
}

void loop() {}
