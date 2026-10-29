# Parser tests

These tests check that Busli still understands every data provider's responses:
departures, connections, weather, air quality, Yahoo quotes, water temperatures
and the daily quote. They run on the device itself, so they test exactly the
code that runs there.

Run them before a release.

## 1. Refresh the samples (optional, recommended)

```
python scripts/update_test_fixtures.py
```

This downloads a current response from each provider into
`test/test_parsers/fixtures.h`. If a download fails, the previous sample is kept.
A sample that was never downloaded makes its test show as skipped.

## 2. Run the tests (Busli connected by USB)

```
pio test -e test --upload-port COM5 --test-port COM5
```

This takes about 3 minutes. It builds a test firmware, flashes it and prints one
line per test. All tests should show `PASSED`. A failing "live" test means a
provider changed its response format; the message shows which value broke.

## 3. Put the normal firmware back

The test firmware replaces Busli's firmware, so flash it again afterwards:

```
pio run -t upload --upload-port COM5
```

Settings and saved data are not affected.

## What's tested

| Kind | What it checks |
|---|---|
| Live samples | Real responses parse and give plausible values (temperatures, prices, times, …) |
| Hand-written samples | Our own logic, with exact expected results: line and direction filters, delays, platform changes, departed trains, rain-alert times, snow/rain/clear days, pence to pounds, unknown symbols, connection legs |
