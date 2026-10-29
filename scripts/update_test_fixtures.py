"""
update_test_fixtures.py — download current sample responses for the parser tests

Fetches one response from every data provider with the same request the device
makes, and writes them into test/test_parsers/fixtures.h. Run it before a
release, then run the tests (see test/README.md): a provider that changed its
response format makes a test fail on the PC instead of a blank page on the device.

If a download fails (e.g. Yahoo rate-limits), the previous sample is kept.

Usage:  python scripts/update_test_fixtures.py
"""
import os, re, shutil, subprocess, sys, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT  = os.path.join(ROOT, "test", "test_parsers", "fixtures.h")
UA   = ("Mozilla/5.0 (Linux; Android 10) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/120.0.0.0 Mobile Safari/537.36")

LAT, LON = 47.376, 8.541   # Zürich
SB_FIELDS = "".join("&fields[]=stationboard/" + f for f in (
    "number", "category", "to", "stop/departure", "stop/prognosis/departure",
    "stop/platform", "stop/prognosis/platform"))
CN_FIELDS = "".join("&fields[]=connections/sections/" + f for f in (
    "journey/name", "journey/category", "journey/number", "departure/station/name",
    "departure/departure", "arrival/station/name", "arrival/arrival"))

# name → URL (same parameters as the firmware)
SOURCES = {
    "FX_STATIONBOARD": "http://transport.opendata.ch/v1/stationboard?station=Z%C3%BCrich%20HB&limit=20" + SB_FIELDS,
    "FX_CONNECTIONS":  "http://transport.opendata.ch/v1/connections?from=Z%C3%BCrich%20HB&to=Bern&limit=5" + CN_FIELDS,
    "FX_WEATHER": ("http://api.open-meteo.com/v1/forecast?latitude=%.3f&longitude=%.3f"
                   "&current=temperature_2m,weather_code,uv_index,precipitation"
                   "&daily=weather_code,precipitation_probability_max,temperature_2m_max,uv_index_max"
                   "&minutely_15=precipitation&forecast_minutely_15=9"
                   "&timezone=Europe%%2FZurich&timeformat=unixtime&forecast_days=1") % (LAT, LON),
    "FX_WEATHER_DETAIL": ("http://api.open-meteo.com/v1/forecast?latitude=%.3f&longitude=%.3f"
                          "&current=temperature_2m,weather_code,uv_index,apparent_temperature,"
                          "wind_speed_10m,wind_gusts_10m"
                          "&hourly=temperature_2m,precipitation_probability&forecast_hours=12"
                          "&daily=temperature_2m_min,temperature_2m_max,sunrise,sunset,precipitation_probability_max"
                          "&timezone=Europe%%2FZurich&timeformat=unixtime&forecast_days=1") % (LAT, LON),
    "FX_AIR": ("http://air-quality-api.open-meteo.com/v1/air-quality?latitude=%.3f&longitude=%.3f"
               "&current=european_aqi,alder_pollen,birch_pollen,grass_pollen,"
               "mugwort_pollen,olive_pollen,ragweed_pollen") % (LAT, LON),
    "FX_YAHOO_CHART":    "https://query1.finance.yahoo.com/v8/finance/chart/NESN.SW?range=1d&interval=15m",
    "FX_YAHOO_CHART_GBP": "https://query1.finance.yahoo.com/v8/finance/chart/VOD.L?range=1d&interval=15m",
    "FX_YAHOO_SPARK":    "https://query1.finance.yahoo.com/v8/finance/spark?range=1d&interval=15m&symbols=NESN.SW,AAPL",
    "FX_LAKE":  "https://tecdottir.metaodi.ch/measurements/tiefenbrunnen?sort=timestamp_cet%20desc&limit=1",
    "FX_RIVER": "https://api.existenz.ch/apiv1/hydro/latest?locations=2243&parameters=temperature&app=busli",
    "FX_QUOTE": "https://zenquotes.io/api/today",
}

DELIM = "FIXTURE"


def fetch(url):
    # curl first: Yahoo answers Python's own HTTP client with "429 Too Many
    # Requests" but serves curl (and the device) normally
    curl = shutil.which("curl")
    if curl:
        r = subprocess.run([curl, "-sS", "-f", "--max-time", "20", "-A", UA,
                            "-H", "Accept: application/json", url],
                           capture_output=True)
        if r.returncode != 0:
            raise RuntimeError(r.stderr.decode("utf-8", "replace").strip() or "curl failed")
        return r.stdout.decode("utf-8")
    req = urllib.request.Request(url, headers={"User-Agent": UA, "Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return r.read().decode("utf-8")


def old_fixtures():
    if not os.path.exists(OUT):
        return {}
    with open(OUT, encoding="utf-8") as f:
        text = f.read()
    pat = re.compile(r'static const char (\w+)\[\] = R"%s\((.*?)\)%s";' % (DELIM, DELIM), re.S)
    return {m.group(1): m.group(2) for m in pat.finditer(text)}


def main():
    old = old_fixtures()
    out, failed = {}, []
    for name, url in SOURCES.items():
        try:
            body = fetch(url)
            if ")" + DELIM in body:
                raise ValueError("delimiter in body")
            out[name] = body
            print("  ok   %-20s %6d bytes" % (name, len(body)))
        except Exception as e:
            failed.append(name)
            if name in old:
                out[name] = old[name]
                print("  KEPT %-20s (%s) — using the previous sample" % (name, e))
            else:
                print("  FAIL %-20s (%s) — no previous sample" % (name, e))

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("// Generated by scripts/update_test_fixtures.py — sample responses for the parser tests.\n")
        f.write("// Don't edit by hand; run the script again to refresh them.\n#pragma once\n\n")
        # A sample that was never downloaded is written empty: its test is skipped
        for name in SOURCES:
            f.write('static const char %s[] = R"%s(%s)%s";\n\n' % (name, DELIM, out.get(name, ""), DELIM))
    print("Wrote %s (%d of %d samples)" % (os.path.relpath(OUT, ROOT), len(out), len(SOURCES)))
    if failed:
        print("Not refreshed: " + ", ".join(failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
