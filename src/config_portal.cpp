// ─────────────────────────────────────────────────────────────────────────────
// config_portal.cpp — AP-mode web config portal
// ─────────────────────────────────────────────────────────────────────────────
// Triggered by a 3-second hold of the BOOT button. Starts a WiFi AP named
// "Busli-Config". Any device that joins and opens a browser (or is auto-
// redirected by the captive-portal DNS trick) gets a single config page:
//   • Up to 3 WiFi credentials
//   • Up to 6 stops (label, station name, optional line/direction filters)
//   • Countdown, flights, commute, parcel, pager, stocks, updates
// Saving writes to NVS and reboots.
//
// If the device is online when the portal starts, it stays connected (AP+STA)
// so buttons that need the internet — pager Test, stock Check — work.
//
// The same pages are also served on the normal Wi-Fi (http://busli.local or the
// device IP) via config_web_begin()/config_web_poll(). There, a browser must
// first pair with a 6-digit code shown on the device screen.
// ─────────────────────────────────────────────────────────────────────────────

#include "../include/config_portal.h"
#include "../include/config.h"
#include "../include/pager.h"
#include "../include/swiss_stops.h"
#include "../include/stocks.h"
#include "../include/http_lock.h"
#include "../include/geo.h"
#include "display.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Update.h>

#define PORTAL_SSID    "Busli-Config"
#define NVS_NAMESPACE       "busli"
#define NVS_KEY_WIFI        "wifi"
#define NVS_KEY_STOPS       "stops"
#define NVS_KEY_COUNTDOWN   "countdown"
#define NVS_KEY_FLIGHTS     "flights"
#define NVS_KEY_OPENSKY     "opensky"
#define NVS_KEY_COMMUTE     "commute"
#define NVS_KEY_OTA         "ota_en"
#define NVS_KEY_PARCEL      "parcel"
#define NVS_KEY_PAGER       "pager"
#define NVS_KEY_STOCKS      "stocks"
#define NVS_KEY_NIGHT       "night"
#define NVS_KEY_WX_PAGE     "wx_page"
#define NVS_KEY_WX_LOCS     "wx_locs"
#define MAX_WIFI        3
#define MAX_STOPS       6
#define MAX_FLIGHTS     2

static WebServer  s_server(80);
static DNSServer  s_dns;
static bool       s_saved = false;
static bool       s_routes_ready = false;  // server listening (routes registered)
static bool       s_web_enabled  = false;  // serving on the normal Wi-Fi too

// ── Access control ────────────────────────────────────────────────────────────
// Requests through the setup hotspot are trusted — starting it needs physical
// access (holding the button). On the normal Wi-Fi, a browser pairs once with a
// 6-digit code shown on the device and then gets a session cookie that lasts
// until the next reboot. Wrong codes are limited per code.
static const uint32_t PAIR_CODE_TTL_MS  = 120000;
static const int      PAIR_MAX_ATTEMPTS = 5;
static String   s_session_token;              // "" = no browser paired yet
static String   s_pair_code;                  // "" = no code showing
static uint32_t s_pair_started_ms = 0;
static int      s_pair_attempts   = 0;
static void   (*s_pair_cb)(const char* code) = nullptr;

static String random_hex(int bytes) {
    String s;
    for (int i = 0; i < bytes; i++) {
        char b[3];
        snprintf(b, sizeof(b), "%02x", (unsigned)(esp_random() & 0xFF));
        s += b;
    }
    return s;
}

static void pair_code_clear() {
    if (s_pair_code.isEmpty()) return;
    s_pair_code = "";
    if (s_pair_cb) s_pair_cb(nullptr);
}

static bool from_setup_hotspot() {
    return (WiFi.getMode() & WIFI_MODE_AP) &&
           s_server.client().localIP() == WiFi.softAPIP();
}

static bool authorized() {
    if (from_setup_hotspot()) return true;
    if (s_session_token.isEmpty()) return false;
    return s_server.header("Cookie").indexOf("busli_s=" + s_session_token) >= 0;
}

static void send_forbidden() {
    s_server.send(403, "application/json", F("{\"ok\":false,\"msg\":\"not paired\"}"));
}

// ── NVS helpers ───────────────────────────────────────────────────────────────

int config_load_wifi(String ssids[], String passes[], String users[]) {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    String json = prefs.getString(NVS_KEY_WIFI, "");
    prefs.end();
    if (json.isEmpty()) return 0;

    JsonDocument doc;
    if (deserializeJson(doc, json)) return 0;
    JsonArray arr = doc.as<JsonArray>();
    int n = 0;
    for (JsonObject o : arr) {
        if (n >= MAX_WIFI) break;
        ssids[n]  = o["s"] | "";
        passes[n] = o["p"] | "";
        users[n]  = o["u"] | "";
        if (ssids[n].length() > 0) n++;
    }
    return n;
}

int config_load_stops(StopEntry entries[]) {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    String json = prefs.getString(NVS_KEY_STOPS, "");
    prefs.end();
    if (json.isEmpty()) return 0;

    JsonDocument doc;
    if (deserializeJson(doc, json)) return 0;
    JsonArray arr = doc.as<JsonArray>();
    int n = 0;
    for (JsonObject o : arr) {
        if (n >= MAX_STOPS) break;
        entries[n].label     = o["l"] | "";
        entries[n].station   = o["n"] | "";
        entries[n].lines_csv = o["i"] | "";
        entries[n].dirs_csv  = o["d"] | "";
        if (entries[n].label.length() > 0) n++;
    }
    return n;
}

bool config_load_countdown(String& label, String& target_str, int& icon) {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    String json = prefs.getString(NVS_KEY_COUNTDOWN, "");
    prefs.end();
    if (json.isEmpty()) return false;

    JsonDocument doc;
    if (deserializeJson(doc, json)) return false;
    label      = doc["l"] | "";
    target_str = doc["t"] | "";
    icon       = doc["i"] | 0;
    return target_str.length() >= 16;
}

int config_load_flights(FlightEntry entries[]) {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    String json = prefs.getString(NVS_KEY_FLIGHTS, "");
    prefs.end();
    if (json.isEmpty()) return 0;

    JsonDocument doc;
    if (deserializeJson(doc, json)) return 0;
    JsonArray arr = doc.as<JsonArray>();
    int n = 0;
    for (JsonObject o : arr) {
        if (n >= MAX_FLIGHTS) break;
        entries[n].callsign     = o["c"]  | "";
        entries[n].dep_date     = o["d"]  | "";
        entries[n].dep_icao     = o["da"] | "";
        entries[n].arr_icao     = o["aa"] | "";
        entries[n].dep_time_str = o["dt"] | "";
        entries[n].arr_time_str = o["at"] | "";
        entries[n].callsign.trim();
        entries[n].dep_icao.toUpperCase();
        entries[n].arr_icao.toUpperCase();
        if (entries[n].callsign.length() > 0) n++;
    }
    return n;
}

bool config_load_opensky(String& user, String& pass) {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    String json = prefs.getString(NVS_KEY_OPENSKY, "");
    prefs.end();
    if (json.isEmpty()) return false;
    JsonDocument doc;
    if (deserializeJson(doc, json)) return false;
    user = doc["u"] | "";
    pass = doc["p"] | "";
    return user.length() > 0;
}

bool config_load_ota_enabled() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    bool val = prefs.getBool(NVS_KEY_OTA, true);
    prefs.end();
    return val;
}

bool config_load_parcel(String& out_tracking, int& out_pulses) {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    String json = prefs.getString(NVS_KEY_PARCEL, "");
    prefs.end();
    if (json.isEmpty()) return false;
    JsonDocument doc;
    if (deserializeJson(doc, json)) return false;
    out_tracking = doc["n"] | "";
    out_pulses   = doc["p"] | 3;
    return out_tracking.length() > 0;
}

bool config_load_pager(PagerConfig& out) {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    String json = prefs.getString(NVS_KEY_PAGER, "");
    prefs.end();

    bool needs_save = false;
    if (!json.isEmpty()) {
        JsonDocument doc;
        if (!deserializeJson(doc, json)) {
            out.name  = doc["n"] | "";
            out.topic = doc["t"] | "";
            out.n_friends = 0;
            JsonArray fa = doc["f"].as<JsonArray>();
            for (JsonObject fo : fa) {
                if (out.n_friends >= PAGER_MAX_FRIENDS) break;
                out.friends[out.n_friends].name  = fo["n"] | "";
                out.friends[out.n_friends].topic = fo["t"] | "";
                if (out.friends[out.n_friends].name.length() > 0 &&
                    out.friends[out.n_friends].topic.length() > 0)
                    out.n_friends++;
            }
        }
    }
    if (out.topic.isEmpty()) {
        out.topic  = pager_generate_topic();
        needs_save = true;
    }
    if (needs_save) config_save_pager(out);
    return out.topic.length() > 0;
}

void config_save_pager(const PagerConfig& cfg) {
    JsonDocument doc;
    doc["n"] = cfg.name;
    doc["t"] = cfg.topic;
    JsonArray fa = doc["f"].to<JsonArray>();
    for (int i = 0; i < cfg.n_friends; i++) {
        JsonObject fo = fa.add<JsonObject>();
        fo["n"] = cfg.friends[i].name;
        fo["t"] = cfg.friends[i].topic;
    }
    String json;
    serializeJson(doc, json);
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putString(NVS_KEY_PAGER, json);
    prefs.end();
}

bool config_load_weather_page() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    bool on = prefs.getBool(NVS_KEY_WX_PAGE, true);
    prefs.end();
    return on;
}

void config_load_weather_locations(WeatherLocations& out) {
    out = WeatherLocations();
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    String json = prefs.getString(NVS_KEY_WX_LOCS, "");
    prefs.end();
    if (json.isEmpty()) return;
    JsonDocument doc;
    if (deserializeJson(doc, json)) return;
    out.device = doc["d"] | true;
    for (const char* t : doc["t"].as<JsonArray>()) {
        if (out.n_towns >= WEATHER_TOWNS_MAX) break;
        if (t && *t) out.towns[out.n_towns++] = t;
    }
    if (!out.device && out.n_towns == 0) out.device = true;   // never an empty page
}

static void config_save_weather_locations(const WeatherLocations& w) {
    JsonDocument doc;
    doc["d"] = w.device;
    JsonArray t = doc["t"].to<JsonArray>();
    for (int i = 0; i < w.n_towns; i++) t.add(w.towns[i]);
    String json;
    serializeJson(doc, json);
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putString(NVS_KEY_WX_LOCS, json);
    prefs.end();
}

void config_load_night(NightConfig& out) {
    out = NightConfig();
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    String json = prefs.getString(NVS_KEY_NIGHT, "");
    prefs.end();
    if (json.isEmpty()) return;
    JsonDocument doc;
    if (deserializeJson(doc, json)) return;
    out.start_min  = constrain(doc["s"] | out.start_min, 0, 1439);
    out.end_min    = constrain(doc["e"] | out.end_min,   0, 1439);
    out.screen_off = doc["o"] | false;
}

static void config_save_night(const NightConfig& cfg) {
    JsonDocument doc;
    doc["s"] = cfg.start_min;
    doc["e"] = cfg.end_min;
    doc["o"] = cfg.screen_off;
    String json;
    serializeJson(doc, json);
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putString(NVS_KEY_NIGHT, json);
    prefs.end();
}

// "HH:MM" ↔ minutes after midnight (for <input type=time>)
static String hhmm(int minutes) {
    char buf[6];
    snprintf(buf, sizeof(buf), "%02d:%02d", minutes / 60, minutes % 60);
    return buf;
}

static int parse_hhmm(const String& s, int fallback) {
    if (s.length() < 5 || s[2] != ':') return fallback;
    int h = s.substring(0, 2).toInt(), m = s.substring(3, 5).toInt();
    if (h < 0 || h > 23 || m < 0 || m > 59) return fallback;
    return h * 60 + m;
}

bool config_load_stocks(StocksConfig& out) {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    String json = prefs.getString(NVS_KEY_STOCKS, "");
    prefs.end();
    out.count = 0;
    if (json.isEmpty()) return false;
    JsonDocument doc;
    if (deserializeJson(doc, json)) return false;
    for (const char* sym : doc["t"].as<JsonArray>()) {
        if (out.count >= STOCKS_MAX) break;
        if (sym && *sym) out.symbols[out.count++] = sym;
    }
    out.layout     = constrain(doc["l"] | (int)STOCKS_LAYOUT_AUTO, 0, 2);
    out.rotate_sec = constrain(doc["r"] | STOCKS_ROTATE_SEC_DEFAULT, 3, 300);
    return out.count > 0;
}

void config_save_stocks(const StocksConfig& cfg) {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    if (cfg.count > 0) {
        JsonDocument doc;
        JsonArray ta = doc["t"].to<JsonArray>();
        for (int i = 0; i < cfg.count; i++) ta.add(cfg.symbols[i]);
        doc["l"] = cfg.layout;
        doc["r"] = cfg.rotate_sec;
        String json;
        serializeJson(doc, json);
        prefs.putString(NVS_KEY_STOCKS, json);
    } else {
        prefs.remove(NVS_KEY_STOCKS);
    }
    prefs.end();
}

bool config_load_commute(CommuteConfig& out) {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    String json = prefs.getString(NVS_KEY_COMMUTE, "");
    prefs.end();
    if (json.isEmpty()) return false;
    JsonDocument doc;
    if (deserializeJson(doc, json)) return false;
    out.home_station = doc["hs"] | "";
    out.work_station = doc["ws"] | "";
    return out.home_station.length() > 0 || out.work_station.length() > 0;
}


static void save_to_nvs(String ssids[], String passes[], String users[], int n_wifi,
                         StopEntry stops[], int n_stops,
                         const String& cd_label, const String& cd_target, int cd_icon,
                         FlightEntry flights[], int n_flights,
                         const String& opensky_user, const String& opensky_pass,
                         const CommuteConfig& commute,
                         bool ota_enabled,
                         const String& parcel_tracking, int parcel_pulses) {
    // WiFi JSON
    JsonDocument wdoc;
    JsonArray warr = wdoc.to<JsonArray>();
    for (int i = 0; i < n_wifi; i++) {
        JsonObject o = warr.add<JsonObject>();
        o["s"] = ssids[i];
        o["p"] = passes[i];
        if (users[i].length() > 0) o["u"] = users[i];
    }
    String wifi_json;
    serializeJson(wdoc, wifi_json);

    // Stops JSON
    JsonDocument sdoc;
    JsonArray sarr = sdoc.to<JsonArray>();
    for (int i = 0; i < n_stops; i++) {
        JsonObject o = sarr.add<JsonObject>();
        o["l"] = stops[i].label;
        o["n"] = stops[i].station;
        o["i"] = stops[i].lines_csv;
        o["d"] = stops[i].dirs_csv;
    }
    String stops_json;
    serializeJson(sdoc, stops_json);

    // Countdown JSON
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putString(NVS_KEY_WIFI,  wifi_json);
    prefs.putString(NVS_KEY_STOPS, stops_json);
    if (cd_target.length() >= 16) {
        JsonDocument cddoc;
        cddoc["l"] = cd_label;
        cddoc["t"] = cd_target;
        cddoc["i"] = cd_icon;
        String cd_json;
        serializeJson(cddoc, cd_json);
        prefs.putString(NVS_KEY_COUNTDOWN, cd_json);
    } else {
        prefs.remove(NVS_KEY_COUNTDOWN);
    }

    // Flights JSON
    if (n_flights > 0) {
        JsonDocument fldoc;
        JsonArray flarr = fldoc.to<JsonArray>();
        for (int i = 0; i < n_flights; i++) {
            JsonObject o = flarr.add<JsonObject>();
            o["c"] = flights[i].callsign;
            o["d"] = flights[i].dep_date;
            if (flights[i].dep_icao.length()     > 0) o["da"] = flights[i].dep_icao;
            if (flights[i].arr_icao.length()     > 0) o["aa"] = flights[i].arr_icao;
            if (flights[i].dep_time_str.length() > 0) o["dt"] = flights[i].dep_time_str;
            if (flights[i].arr_time_str.length() > 0) o["at"] = flights[i].arr_time_str;
        }
        String fl_json;
        serializeJson(fldoc, fl_json);
        prefs.putString(NVS_KEY_FLIGHTS, fl_json);
    } else {
        prefs.remove(NVS_KEY_FLIGHTS);
    }

    if (opensky_user.length() > 0) {
        JsonDocument oskydoc;
        oskydoc["u"] = opensky_user;
        oskydoc["p"] = opensky_pass;
        String osky_json;
        serializeJson(oskydoc, osky_json);
        prefs.putString(NVS_KEY_OPENSKY, osky_json);
    } else {
        prefs.remove(NVS_KEY_OPENSKY);
    }

    // Commute JSON — inside the same open handle
    if (commute.home_station.length() > 0 || commute.work_station.length() > 0) {
        JsonDocument cmdoc;
        cmdoc["hs"] = commute.home_station;
        cmdoc["ws"] = commute.work_station;
        String cm_json;
        serializeJson(cmdoc, cm_json);
        prefs.putString(NVS_KEY_COMMUTE, cm_json);
    } else {
        prefs.remove(NVS_KEY_COMMUTE);
    }

    prefs.putBool(NVS_KEY_OTA, ota_enabled);

    // Parcel tracking
    if (parcel_tracking.length() > 0) {
        JsonDocument pldoc;
        pldoc["n"] = parcel_tracking;
        pldoc["p"] = parcel_pulses;
        String pl_json;
        serializeJson(pldoc, pl_json);
        prefs.putString(NVS_KEY_PARCEL, pl_json);
    } else {
        prefs.remove(NVS_KEY_PARCEL);
    }

    prefs.end();

    Serial.printf("[Portal] Saved %d WiFi, %d stops, %d flights to NVS, OTA=%s parcel=%s\n",
                  n_wifi, n_stops, n_flights, ota_enabled ? "on" : "off",
                  parcel_tracking.isEmpty() ? "off" : parcel_tracking.c_str());
}

// ── HTML builder ──────────────────────────────────────────────────────────────

static String html_encode(const String& s) {
    String out;
    out.reserve(s.length() + 8);
    for (char c : s) {
        if      (c == '"')  out += "&quot;";
        else if (c == '&')  out += "&amp;";
        else if (c == '<')  out += "&lt;";
        else if (c == '>')  out += "&gt;";
        else                out += c;
    }
    return out;
}

static String build_page(String ssids[], String passes[], String users[],
                          StopEntry stops[], int n_stops,
                          const String& cd_label, const String& cd_target, int cd_icon,
                          FlightEntry flights[], int n_flights,
                          const String& opensky_user, const String& opensky_pass,
                          const CommuteConfig& commute,
                          bool ota_enabled,
                          const String& parcel_tracking, int parcel_pulses,
                          const PagerConfig& pager,
                          const StocksConfig& stocks,
                          const NightConfig& night,
                          bool via_hotspot) {
    String h;
    h.reserve(16000);

    h += F("<!DOCTYPE html><html lang='en'><head>"
           "<meta charset='UTF-8'>"
           "<meta name='viewport' content='width=device-width,initial-scale=1'>"
           "<title>Busli</title><style>"
           "*{box-sizing:border-box}"
           "body{font:15px/1.5 sans-serif;max-width:500px;margin:0 auto;"
                "padding:16px 14px 48px;background:#111;color:#f7b500}"
           "h1{font-size:22px;margin:0 0 2px}"
           ".sub{color:#888;font-size:12px;margin:0 0 20px}"
           "h2{font-size:14px;text-transform:uppercase;letter-spacing:.05em;"
               "margin:24px 0 8px;border-bottom:1px solid #2a2a2a;padding-bottom:4px}"
           "label{display:block;margin-top:10px;font-size:12px;color:#999}"
           "input{width:100%;padding:9px 10px;margin-top:3px;background:#1a1a1a;"
                  "color:#eee;border:1px solid #383838;border-radius:5px;font-size:14px}"
           ".card{border:1px solid #252525;border-radius:8px;padding:12px 14px;"
                  "margin:8px 0;background:#161616}"
           ".ct{font-size:13px;font-weight:bold;color:#c8a000;margin-bottom:4px}"
           ".hint{font-size:11px;color:#555;margin-top:3px}"
           "button{display:block;width:100%;padding:15px;margin-top:28px;"
                   "background:#f7b500;color:#000;font-size:16px;font-weight:bold;"
                   "border:none;border-radius:8px;cursor:pointer}"
           ".tbtn{display:inline-block;width:auto;padding:6px 12px;margin-top:0;"
                  "background:#1a1a1a;color:#f7b500;font-size:12px;font-weight:bold;"
                  "border:1px solid #383838;border-radius:5px;cursor:pointer;white-space:nowrap}"
           ".tbtn:disabled{opacity:.5;cursor:default}"
           ".sta-wrap{position:relative}"
           ".drop{position:absolute;left:0;right:0;top:100%;background:#1a1a1a;"
                  "border:1px solid #444;border-radius:0 0 6px 6px;z-index:9;"
                  "display:none;max-height:200px;overflow-y:auto}"
           ".drop div{padding:10px 12px;cursor:pointer;color:#eee;font-size:14px;"
                      "border-top:1px solid #2a2a2a}"
           ".drop div:first-child{border-top:none}"
           ".drop div:active{background:#252525}"
           ".iprow{display:flex;gap:8px;margin-top:8px}"
           ".ip{display:flex;align-items:center;justify-content:center;"
               "width:52px;height:52px;border:2px solid #383838;border-radius:8px;"
               "cursor:pointer;font-size:26px;color:#888;transition:border-color .15s,"
               "color .15s}"
           ".ip.sel{border-color:#f7b500;color:#f7b500}"
           "</style></head><body>"
           "<h1>Busli</h1>");
    h += via_hotspot
        ? F("<p class='sub'>Connected to <b>Busli&#8209;Config</b> &mdash; "
            "save to reboot with new settings</p>")
        : F("<p class='sub'>Settings over your Wi-Fi &mdash; "
            "save to reboot with new settings</p>");
    h += F("<form method='POST' action='/save'>");

    // ── WiFi section ─────────────────────────────────────────────────────────
    h += F("<h2>WiFi Networks</h2>");
    for (int i = 0; i < MAX_WIFI; i++) {
        h += "<div class='card'><div class='ct'>Network ";
        h += String(i + 1);
        if (i > 0) h += " <span style='font-weight:normal;color:#666'>(optional)</span>";
        h += "</div>";
        h += "<label>SSID</label>"
             "<input type='text' name='w" + String(i) + "s' value='"
             + html_encode(ssids[i]) + "'>";
        // Saved passwords are never sent to the browser — blank keeps the current one
        h += "<label>Password</label>"
             "<input type='password' name='w" + String(i) + "p' value='' autocomplete='new-password' "
             "placeholder='" + String(passes[i].length() > 0 ? "saved — type to change" : "") + "'>";
        h += "<label>Username <span style='font-weight:normal;color:#666'>(leave blank for normal WPA2, fill in for enterprise/work networks)</span></label>"
             "<input type='text' name='w" + String(i) + "u' value='"
             + html_encode(users[i]) + "' autocomplete='off'>";
        h += "</div>";
    }

    // ── Stops section ────────────────────────────────────────────────────────
    h += F("<h2>Stops</h2>");
    for (int i = 0; i < MAX_STOPS; i++) {
        String lbl = (i < n_stops) ? stops[i].label     : "";
        String sta = (i < n_stops) ? stops[i].station   : "";
        String li  = (i < n_stops) ? stops[i].lines_csv : "";
        String di  = (i < n_stops) ? stops[i].dirs_csv  : "";

        h += "<div class='card'><div class='ct'>Stop ";
        h += String(i + 1);
        if (i >= n_stops) h += " <span style='font-weight:normal;color:#666'>(empty)</span>";
        h += "</div>";

        h += "<label>Display label</label>"
             "<input type='text' name='s" + String(i) + "l' value='"
             + html_encode(lbl) + "'>";
        h += "<label>Station name <span class='hint'>start typing to search&hellip;</span></label>"
             "<div class='sta-wrap'>"
             "<input type='text' name='s" + String(i) + "n' value='"
             + html_encode(sta) + "' oninput='st(this)' autocomplete='off'>"
             "<div class='drop'></div>"
             "</div>";
        h += "<label>Line filter <span class='hint'>comma-separated line numbers, blank&nbsp;=&nbsp;all &mdash; e.g. 2,20</span></label>"
             "<input type='text' name='s" + String(i) + "i' placeholder='e.g. 2,20' value='"
             + html_encode(li) + "'>";
        h += "<label>Direction filter <span class='hint'>end-station substrings, blank&nbsp;=&nbsp;both directions &mdash; e.g. Klusplatz,Altstetten</span></label>"
             "<input type='text' name='s" + String(i) + "d' placeholder='e.g. Klusplatz,Altstetten' value='"
             + html_encode(di) + "'>";
        h += "</div>";
    }

    // ── Countdown section ────────────────────────────────────────────────────
    h += F("<h2>Countdown</h2>"
           "<div class='card'>"
           "<div class='ct'>Event timer <span style='font-weight:normal;color:#666'>(optional)</span></div>"
           "<p class='hint' style='margin:4px 0 8px'>Shows a live countdown in the footer. Leave blank to disable.</p>");
    h += "<label>Label <span class='hint'>shown in config only</span></label>"
         "<input type='text' name='cd_label' placeholder='e.g. Flight to Berlin' value='"
         + html_encode(cd_label) + "'>";
    h += "<label>Target date &amp; time <span class='hint'>local time, format YYYY-MM-DD HH:MM</span></label>"
         "<input type='text' name='cd_target' placeholder='e.g. 2026-06-15 18:30' value='"
         + html_encode(cd_target) + "'>";
    h += F("<label>Icon</label>"
           "<div class='iprow' id='iprow'>"
           "<div class='ip' data-v='0' onclick='pickIco(this)'>&#8212;</div>"
           "<div class='ip' data-v='2' onclick='pickIco(this)'>&#128197;</div>"
           "</div>");
    h += "<input type='hidden' name='cd_icon' id='cd_icon' value='"
         + String(cd_icon) + "'>";
    h += F("</div>");

    // ── Flights section ──────────────────────────────────────────────────────
    h += F("<h2>Flights</h2>"
           "<p class='hint' style='margin:-4px 0 10px'>Swipe down/up on the device to see flight status. "
           "Enter the flight number (IATA like <b>LX161</b> or ICAO like <b>SWR161</b>). "
           "Both work. IATA is the number on your ticket. Live position is looked up from "
           "3&nbsp;h before departure until 1&nbsp;h after arrival &mdash; enter the times "
           "below to narrow that window (otherwise the whole departure day is searched).</p>");
    const char* swipe_label[] = { "swipe down", "swipe up" };
    for (int i = 0; i < MAX_FLIGHTS; i++) {
        String cs   = (i < n_flights) ? flights[i].callsign     : "";
        String dat  = (i < n_flights) ? flights[i].dep_date     : "";
        String depa = (i < n_flights) ? flights[i].dep_icao     : "";
        String arra = (i < n_flights) ? flights[i].arr_icao     : "";
        String dtm  = (i < n_flights) ? flights[i].dep_time_str : "";
        String atm  = (i < n_flights) ? flights[i].arr_time_str : "";
        h += "<div class='card'><div class='ct'>Flight ";
        h += String(i + 1);
        h += " <span style='font-weight:normal;color:#666'>(";
        h += swipe_label[i];
        h += ")</span></div>";
        h += "<label>Flight number <span class='hint'>IATA (e.g. LX161, TG971) or ICAO callsign (SWR161, THA971)</span></label>"
             "<input type='text' name='fl" + String(i) + "c' placeholder='e.g. LX161 or TG971' value='"
             + html_encode(cs) + "'>";
        h += "<label>Departure date</label>"
             "<input type='date' name='fl" + String(i) + "d' value='"
             + html_encode(dat) + "'>";
        h += "<label>Departure airport <span class='hint'>IATA code from your ticket, e.g. ZRH, ADD, JFK</span></label>"
             "<input type='text' name='fl" + String(i) + "da' maxlength='4' placeholder='ZRH' value='"
             + html_encode(depa) + "' style='text-transform:uppercase'>";
        h += "<label>Arrival airport <span class='hint'>IATA code from your ticket, e.g. BKK, LHR, DXB</span></label>"
             "<input type='text' name='fl" + String(i) + "aa' maxlength='4' placeholder='BKK' value='"
             + html_encode(arra) + "' style='text-transform:uppercase'>";
        h += "<label>Departure time <span class='hint'>local time from ticket — enables clock display and progress bar</span></label>"
             "<input type='time' name='fl" + String(i) + "dt' value='"
             + html_encode(dtm) + "'>";
        h += "<label>Arrival time <span class='hint'>local time from ticket</span></label>"
             "<input type='time' name='fl" + String(i) + "at' value='"
             + html_encode(atm) + "'>";
        h += "</div>";
    }
    h += F("<div class='card'><div class='ct'>OpenSky Network account</div>"
           "<p class='hint'>Required for live flight data. Without credentials, OpenSky ignores "
           "the callsign filter and returns all aircraft worldwide (~5 MB \xe2\x80\x94 unusable). "
           "Create a free account at <b>opensky-network.org</b> and enter your login here.</p>");
    h += "<label>Username</label>"
         "<input type='text' name='osky_u' autocomplete='off' value='"
         + html_encode(opensky_user) + "'>";
    h += "<label>Password</label>"
         "<input type='password' name='osky_p' value='' autocomplete='new-password' "
         "placeholder='" + String(opensky_pass.length() > 0 ? "saved — type to change" : "") + "'>";
    h += F("</div>");

    // ── Commute section ──────────────────────────────────────────────────────
    h += F("<h2>Commute</h2>"
           "<p class='hint' style='margin:-4px 0 10px'>Pick your nearest transit stop for home and work. "
           "Two extra pages appear in the rotation: <b>&gt; Home</b> and <b>&gt; Work</b> "
           "showing connections between them. Swipe down for later connections.</p>");
    const char* cm_labels[] = { "Home stop", "Work stop" };
    const String cm_stns[]  = { commute.home_station, commute.work_station };
    const char* cm_keys[]   = { "cm_hs", "cm_ws" };
    for (int i = 0; i < 2; i++) {
        h += "<div class='card'><div class='ct'>";
        h += cm_labels[i];
        h += "</div>";
        h += "<label>Station <span class='hint'>start typing to search&hellip;</span></label>"
             "<div class='sta-wrap'>"
             "<input type='text' name='" + String(cm_keys[i]) + "' value='"
             + html_encode(cm_stns[i]) + "' oninput='st(this)' autocomplete='off'>"
             "<div class='drop'></div>"
             "</div>";
        h += "</div>";
    }

    // ── Parcel tracking section ──────────────────────────────────────────────
    h += F("<h2>Parcel tracking</h2>"
           "<div class='card'>"
           "<div class='ct'>Swiss Post <span style='font-weight:normal;color:#666'>(optional)</span></div>"
           "<p class='hint' style='margin:4px 0 8px'>Status is checked once per hour and shown in the footer "
           "instead of UV info. When the status changes, the backlight pulses to alert you.</p>");
    h += "<label>Tracking number</label>"
         "<input type='text' name='pt_num' placeholder='99.00.000000.00000000' value='"
         + html_encode(parcel_tracking) + "' autocomplete='off'>";
    h += "<label>Alert pulses <span class='hint'>how many times to flash the screen when status changes (0&ndash;10)</span></label>"
         "<input type='number' name='pt_pulses' min='0' max='10' value='"
         + String(parcel_pulses) + "' style='width:80px'>";
    h += F("</div>");

    // ── Stocks section ───────────────────────────────────────────────────────
    h += F("<h2>Portfolio</h2>"
           "<p class='hint' style='margin:-4px 0 10px'>Adds a <b>Portfolio</b> page to the rotation showing "
           "price and daily % change. Use Yahoo symbols: <b>AAPL</b> (US), <b>NESN.SW</b> (SIX), "
           "<b>VWCE.DE</b> (Xetra), <b>VWRL.L</b> (London). Tap <b>Check</b> to verify a symbol.</p>"
           "<div class='card'><div class='ct'>Tickers <span style='font-weight:normal;color:#666'>(up to 20)</span></div>");
    for (int i = 0; i < STOCKS_MAX; i++) {
        String sym = (i < stocks.count) ? stocks.symbols[i] : "";
        h += "<div style='display:flex;gap:8px;margin-top:6px;align-items:center'>"
             "<span style='width:20px;color:#666;font-size:12px'>" + String(i + 1) + "</span>"
             "<input type='text' name='st" + String(i) + "' id='st" + String(i) + "' value='"
             + html_encode(sym) + "' placeholder='e.g. NESN.SW' autocomplete='off' "
             "spellcheck='false' autocapitalize='characters' style='flex:1;margin-top:0'>"
             "<button type='button' class='tbtn' onclick='checkStock(this," + String(i) + ")'>Check</button>"
             "<span id='sr" + String(i) + "' style='width:110px;font-size:12px;color:#999'></span>"
             "</div>";
    }
    h += F("</div><div class='card'><div class='ct'>Display</div>"
           "<label>Layout</label>"
           "<select name='st_l' style='width:100%;padding:9px 10px;margin-top:3px;background:#1a1a1a;"
           "color:#eee;border:1px solid #383838;border-radius:5px;font-size:14px'>");
    const char* layout_names[] = { "Auto (big for 1&ndash;5, compact for more)",
                                   "Always big (5 per page)",
                                   "Always compact (10 per page)" };
    for (int i = 0; i < 3; i++) {
        h += "<option value='" + String(i) + "'";
        if (stocks.layout == i) h += " selected";
        h += ">";
        h += layout_names[i];
        h += "</option>";
    }
    h += F("</select>");
    h += "<label>Page rotation <span class='hint'>seconds per page when tickers don't fit on one page "
         "(swipe up/down to flip manually)</span></label>"
         "<input type='number' name='st_r' min='3' max='300' value='"
         + String(stocks.rotate_sec) + "' style='width:80px'>";
    h += F("</div>");

    // ── Pager section ────────────────────────────────────────────────────────
    h += F("<h2>Pager</h2>"
           "<p class='hint' style='margin:-4px 0 10px'>Send activity invites to friends. "
           "Long-press the brightness button to open the send screen. "
           "Share your Topic ID with friends so they can message you.</p>");
    h += "<div class='card'><div class='ct'>This device</div>";
    h += "<label>Your name <span class='hint'>shown to friends when you send a message</span></label>"
         "<input type='text' name='pg_n' placeholder='e.g. AJ' maxlength='20' value='"
         + html_encode(pager.name) + "'>";
    h += "<label>Your topic ID <span class='hint'>share this with friends so they can add you</span></label>"
         "<input type='text' name='pg_t' value='"
         + html_encode(pager.topic) + "' autocomplete='off' spellcheck='false' style='font-size:12px'>";
    h += F("</div>");
    h += F("<div class='card'><div class='ct'>Friends</div>"
           "<p class='hint' style='margin:4px 0 10px'>Add up to 5 friends. "
           "Enter their name and the topic ID they shared with you.</p>");
    for (int i = 0; i < PAGER_MAX_FRIENDS; i++) {
        String fn = (i < pager.n_friends) ? pager.friends[i].name  : "";
        String ft = (i < pager.n_friends) ? pager.friends[i].topic : "";
        h += "<div style='margin-bottom:8px;padding-bottom:8px;border-bottom:1px solid #252525'>";
        h += "<label style='color:#777'>Friend " + String(i + 1) + "</label>";
        h += "<div style='display:flex;gap:8px;margin-top:4px;align-items:center'>"
             "<input type='text' name='pf" + String(i) + "n' id='pfn" + String(i) + "' placeholder='Name' "
             "value='" + html_encode(fn) + "' style='width:35%'>"
             "<input type='text' name='pf" + String(i) + "t' id='pft" + String(i) + "' placeholder='Topic ID' "
             "value='" + html_encode(ft) + "' style='font-size:12px;flex:1' "
             "autocomplete='off' spellcheck='false'>"
             "<button type='button' class='tbtn' onclick='testPager(this," + String(i) + ")'>Test</button>"
             "</div>";
        h += "</div>";
    }
    h += F("</div>");

    // ── Weather page section ─────────────────────────────────────────────────
    h += "<h2>Weather page</h2><div class='card'>"
         "<label style='display:flex;align-items:center;gap:10px;cursor:pointer;color:#ddd'>"
         "<input type='checkbox' name='wx_page' value='1' style='width:auto'"
         + String(config_load_weather_page() ? " checked" : "") + "><span>Show the weather page</span></label>"
         "<p class='hint' style='margin-top:6px'>Current weather, the next 12 hours, air quality and "
         "pollen. With several locations, swipe up/down on the page to switch.</p>";
    {
        WeatherLocations wl;
        config_load_weather_locations(wl);
        h += "<label style='display:flex;align-items:center;gap:10px;cursor:pointer;color:#ddd;margin-top:12px'>"
             "<input type='checkbox' name='wx_dev' value='1' style='width:auto'"
             + String(wl.device ? " checked" : "") + "><span>This device's location "
             "<span class='hint' style='margin:0'>(from its internet connection)</span></span></label>";
        h += F("<label>Towns <span class='hint'>up to 3, e.g. Bern, Gen&egrave;ve, Lugano</span></label>");
        for (int i = 0; i < WEATHER_TOWNS_MAX; i++) {
            String t = (i < wl.n_towns) ? wl.towns[i] : "";
            h += "<div style='display:flex;gap:8px;margin-top:6px;align-items:center'>"
                 "<input type='text' name='wt" + String(i) + "' id='wt" + String(i) + "' value='"
                 + html_encode(t) + "' placeholder='Town' autocomplete='off' style='flex:1;margin-top:0'>"
                 "<button type='button' class='tbtn' onclick='checkTown(this," + String(i) + ")'>Check</button>"
                 "<span id='wr" + String(i) + "' style='width:90px;font-size:12px;color:#999'></span></div>";
        }
    }
    h += F("</div>");

    // ── Night mode section ───────────────────────────────────────────────────
    h += F("<h2>Night mode</h2><div class='card'>"
           "<p class='hint' style='margin:0 0 8px'>During these hours the screen is dimmed or "
           "switched off, and departures refresh less often to save power. "
           "Set both times equal to disable.</p>"
           "<div style='display:flex;gap:12px'>");
    h += "<div style='flex:1'><label>From</label><input type='time' name='nt_s' value='"
         + hhmm(night.start_min) + "'></div>"
         "<div style='flex:1'><label>Until</label><input type='time' name='nt_e' value='"
         + hhmm(night.end_min) + "'></div></div>";
    h += "<label style='display:flex;align-items:center;gap:10px;margin-top:12px;color:#ddd'>"
         "<input type='radio' name='nt_o' value='0' style='width:auto'"
         + String(night.screen_off ? "" : " checked") + "> Dim the screen</label>"
         "<label style='display:flex;align-items:center;gap:10px;color:#ddd'>"
         "<input type='radio' name='nt_o' value='1' style='width:auto'"
         + String(night.screen_off ? " checked" : "") + "> Turn the screen off "
         "<span class='hint' style='margin:0'>(touch or any button wakes it for "
         + String(WAKE_ON_TOUCH_SEC) + "&nbsp;s)</span></label>";
    h += F("</div>");

    // ── Updates section ──────────────────────────────────────────────────────
    h += F("<h2>Updates</h2><div class='card'>");
    h += "<label style='display:flex;align-items:center;gap:10px;cursor:pointer'>"
         "<input type='checkbox' name='ota_en' value='1'";
    if (ota_enabled) h += " checked";
    h += F("><span>Automatic firmware updates</span></label>"
           "<p class='hint' style='margin-top:6px'>When enabled, the device checks for new firmware "
           "once a day and installs it automatically. Disable to update manually via the "
           "Firmware update page.</p></div>");

    h += F("<button type='submit'>&#128190;&nbsp; Save &amp; Reboot</button>"
           "</form>"
           "<p style='text-align:center;margin-top:20px'>"
           "<a href='/update' style='color:#999;font-size:13px'>&#9652; Firmware update</a>"
           "</p>"
           "<script>"
           "var _t;"
           "function st(el){"
             "clearTimeout(_t);"
             "var q=el.value.trim(),dr=el.nextElementSibling;"
             "if(q.length<2){dr.style.display='none';return;}"
             "_t=setTimeout(function(){"
               "fetch('/stations?q='+encodeURIComponent(q))"
               ".then(function(r){return r.json();})"
               ".then(function(d){"
                 "dr.innerHTML='';"
                 "if(!d.length){dr.style.display='none';return;}"
                 "for(var i=0;i<d.length;i++){"
                   "(function(n){"
                     "var row=document.createElement('div');"
                     "row.textContent=n;"
                     "row.onclick=function(){el.value=n;dr.style.display='none';};"
                     "dr.appendChild(row);"
                   "})(d[i]);"
                 "}"
                 "dr.style.display='block';"
               "})"
               ".catch(function(){});"
             "},400);"
           "}"
           "document.addEventListener('click',function(e){"
             "if(!e.target.closest('.sta-wrap')){"
               "var ds=document.querySelectorAll('.drop');"
               "for(var i=0;i<ds.length;i++)ds[i].style.display='none';"
             "}"
           "});"
           "function pickIco(el){"
             "document.querySelectorAll('#iprow .ip').forEach(function(e){e.classList.remove('sel');});"
             "el.classList.add('sel');"
             "document.getElementById('cd_icon').value=el.getAttribute('data-v');"
           "}"
           "(function(){"
             "var v=document.getElementById('cd_icon').value;"
             "document.querySelectorAll('#iprow .ip').forEach(function(e){"
               "if(e.getAttribute('data-v')===v)e.classList.add('sel');"
             "});"
           "})();"
           "function testPager(btn,i){"
             "var t=document.getElementById('pft'+i).value.trim();"
             "if(!t){btn.textContent='no topic';return;}"
             "var n=document.getElementById('pfn'+i).value.trim();"
             "btn.textContent='...';btn.disabled=true;"
             "fetch('/pager_test?topic='+encodeURIComponent(t)+'&name='+encodeURIComponent(n))"
               ".then(function(r){return r.json();})"
               ".then(function(d){"
                 "btn.textContent=d.ok?'✓ sent':'✗ fail';"
                 "btn.disabled=false;"
                 "setTimeout(function(){btn.textContent='Test';},3000);"
               "})"
               ".catch(function(){btn.textContent='✗ err';btn.disabled=false;"
                 "setTimeout(function(){btn.textContent='Test';},3000);});"
           "}"
           "function checkTown(btn,i){"
             "var q=document.getElementById('wt'+i).value.trim(),r=document.getElementById('wr'+i);"
             "if(!q){r.textContent='';return;}"
             "btn.disabled=true;r.style.color='#999';r.textContent='checking\\u2026';"
             "fetch('/town_check?q='+encodeURIComponent(q))"
               ".then(function(x){return x.json();})"
               ".then(function(d){btn.disabled=false;"
                 "r.style.color=d.ok?'#4c4':'#f55';r.textContent=d.ok?'\\u2713 found':'\\u2717 '+d.msg;})"
               ".catch(function(){btn.disabled=false;r.style.color='#f55';r.textContent='\\u2717 error';});"
           "}"
           "function checkStock(btn,i){"
             "var s=document.getElementById('st'+i).value.trim(),r=document.getElementById('sr'+i);"
             "if(!s){r.textContent='';return;}"
             "btn.disabled=true;r.style.color='#999';r.textContent='checking\\u2026';"
             "fetch('/stock_check?s='+encodeURIComponent(s))"
               ".then(function(x){return x.json();})"
               ".then(function(d){"
                 "btn.disabled=false;"
                 "if(d.ok){r.style.color=d.pct<0?'#f55':'#4c4';"
                   "r.textContent=d.price+' '+d.cur+' '+(d.pct>=0?'+':'')+d.pct.toFixed(2)+'%';}"
                 "else{r.style.color='#f55';r.textContent='\\u2717 '+d.msg;}"
               "})"
               ".catch(function(){btn.disabled=false;r.style.color='#f55';r.textContent='\\u2717 error';});"
           "}"
           "</script>"
           "</body></html>");
    return h;
}

// ── Request handlers ──────────────────────────────────────────────────────────

static String s_ssids[MAX_WIFI];
static String s_passes[MAX_WIFI];
static String s_users[MAX_WIFI];
static StopEntry s_stops[MAX_STOPS];
static int s_n_stops = 0;
static String s_cd_label;
static String s_cd_target;
static int    s_cd_icon = 0;
static FlightEntry s_flights[MAX_FLIGHTS];
static int         s_n_flights = 0;
static String      s_opensky_user;
static String      s_opensky_pass;
static CommuteConfig s_commute;
static bool          s_ota_enabled = true;
static String        s_parcel_tracking;
static int           s_parcel_pulses = 3;
static PagerConfig   s_pager;
static StocksConfig  s_stocks;
static NightConfig   s_night;

// Read the saved configuration into the s_* form state (fresh on every page view,
// so the normal-Wi-Fi page always shows what's stored).
static void load_form_state() {
    int n_wifi = config_load_wifi(s_ssids, s_passes, s_users);
    for (int i = n_wifi; i < MAX_WIFI; i++) { s_ssids[i] = ""; s_passes[i] = ""; s_users[i] = ""; }
    s_n_stops = config_load_stops(s_stops);
    s_cd_label = ""; s_cd_target = ""; s_cd_icon = 0;
    config_load_countdown(s_cd_label, s_cd_target, s_cd_icon);
    s_n_flights = config_load_flights(s_flights);
    s_opensky_user = ""; s_opensky_pass = "";
    config_load_opensky(s_opensky_user, s_opensky_pass);
    s_commute = {};
    config_load_commute(s_commute);
    s_ota_enabled = config_load_ota_enabled();
    s_parcel_tracking = "";
    s_parcel_pulses   = 3;
    config_load_parcel(s_parcel_tracking, s_parcel_pulses);
    s_pager = {};
    config_load_pager(s_pager);
    s_stocks = {};
    config_load_stocks(s_stocks);
    config_load_night(s_night);
}

static const char PAGE_HEAD[] PROGMEM =
    "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Busli</title><style>"
    "body{font:16px/1.5 sans-serif;max-width:420px;margin:0 auto;padding:32px 16px;"
    "background:#111;color:#f7b500;text-align:center}"
    "input{font-size:28px;letter-spacing:.3em;text-align:center;width:100%;padding:12px;"
    "background:#1a1a1a;color:#eee;border:1px solid #383838;border-radius:8px}"
    "button{width:100%;padding:15px;margin-top:16px;background:#f7b500;color:#000;"
    "font-size:16px;font-weight:bold;border:none;border-radius:8px}"
    "p{color:#999}.err{color:#f55}</style></head><body>";

// Pairing page for the normal-Wi-Fi settings: shows a code on the device screen
static void send_pair_page(const char* error) {
    if (s_pair_code.isEmpty() || millis() - s_pair_started_ms > PAIR_CODE_TTL_MS) {
        s_pair_code = String(100000 + esp_random() % 900000);
        s_pair_started_ms = millis();
        s_pair_attempts   = 0;
        if (s_pair_cb) s_pair_cb(s_pair_code.c_str());
        Serial.println("[Web] Pairing code shown on screen");
    }
    String h = FPSTR(PAGE_HEAD);
    h += F("<h1>Busli settings</h1>"
           "<p>Enter the 6-digit code shown on the Busli screen.</p>"
           "<form method='POST' action='/pair'>"
           "<input name='code' inputmode='numeric' maxlength='6' autofocus autocomplete='off'>"
           "<button type='submit'>Continue</button></form>");
    if (error) { h += "<p class='err'>"; h += error; h += "</p>"; }
    h += F("</body></html>");
    s_server.send(200, "text/html", h);
}

static void handle_pair() {
    String code = s_server.arg("code"); code.trim();
    bool expired = s_pair_code.isEmpty() || millis() - s_pair_started_ms > PAIR_CODE_TTL_MS;
    if (!expired && code == s_pair_code) {
        s_session_token = random_hex(16);
        pair_code_clear();
        Serial.println("[Web] Browser paired");
        s_server.sendHeader("Set-Cookie", "busli_s=" + s_session_token + "; Path=/; HttpOnly; SameSite=Strict");
        s_server.sendHeader("Location", "/", true);
        s_server.send(303, "text/plain", "");
        return;
    }
    if (!expired && ++s_pair_attempts >= PAIR_MAX_ATTEMPTS) {
        pair_code_clear();            // force a fresh code on the next try
        send_pair_page("Too many wrong codes &mdash; a new code is now on the screen.");
        return;
    }
    send_pair_page(expired ? "The code expired &mdash; a new one is on the screen."
                           : "Wrong code &mdash; please try again.");
}

static void handle_root() {
    if (!authorized()) { send_pair_page(nullptr); return; }
    load_form_state();
    String page = build_page(s_ssids, s_passes, s_users, s_stops, s_n_stops,
                             s_cd_label, s_cd_target, s_cd_icon,
                             s_flights, s_n_flights,
                             s_opensky_user, s_opensky_pass,
                             s_commute, s_ota_enabled,
                             s_parcel_tracking, s_parcel_pulses,
                             s_pager, s_stocks, s_night,
                             from_setup_hotspot());
    s_server.send(200, "text/html", page);
}

static void handle_save() {
    if (!authorized()) { send_forbidden(); return; }
    // Saved secrets, so a blank password field can keep the current one
    String old_ssids[MAX_WIFI], old_passes[MAX_WIFI], old_users[MAX_WIFI];
    int n_old = config_load_wifi(old_ssids, old_passes, old_users);
    String old_osky_user, old_osky_pass;
    config_load_opensky(old_osky_user, old_osky_pass);

    // Parse WiFi
    String new_ssids[MAX_WIFI], new_passes[MAX_WIFI], new_users[MAX_WIFI];
    int n_wifi = 0;
    for (int i = 0; i < MAX_WIFI; i++) {
        String ss = s_server.arg("w" + String(i) + "s");
        String pp = s_server.arg("w" + String(i) + "p");
        String uu = s_server.arg("w" + String(i) + "u");
        ss.trim(); uu.trim();
        // Blank password + same network name as before → keep the saved password
        if (pp.isEmpty())
            for (int j = 0; j < n_old; j++)
                if (old_ssids[j] == ss) { pp = old_passes[j]; break; }
        if (ss.length() > 0) {
            new_ssids[n_wifi]  = ss;
            new_passes[n_wifi] = pp;
            new_users[n_wifi]  = uu;
            n_wifi++;
        }
    }

    // Parse stops
    StopEntry new_stops[MAX_STOPS];
    int n_stops = 0;
    for (int i = 0; i < MAX_STOPS; i++) {
        String lbl = s_server.arg("s" + String(i) + "l");
        String sta = s_server.arg("s" + String(i) + "n");
        lbl.trim(); sta.trim();
        if (lbl.length() > 0 && sta.length() > 0) {
            new_stops[n_stops].label     = lbl;
            new_stops[n_stops].station   = sta;
            new_stops[n_stops].lines_csv = s_server.arg("s" + String(i) + "i");
            new_stops[n_stops].dirs_csv  = s_server.arg("s" + String(i) + "d");
            new_stops[n_stops].lines_csv.trim();
            new_stops[n_stops].dirs_csv.trim();
            n_stops++;
        }
    }

    String cd_label  = s_server.arg("cd_label");  cd_label.trim();
    String cd_target = s_server.arg("cd_target"); cd_target.trim();
    int    cd_icon   = s_server.arg("cd_icon").toInt();
    if (cd_icon < 0 || cd_icon > 3) cd_icon = 0;

    // Parse flights
    FlightEntry new_flights[MAX_FLIGHTS];
    int n_flights = 0;
    for (int i = 0; i < MAX_FLIGHTS; i++) {
        String cs  = s_server.arg("fl" + String(i) + "c"); cs.trim();
        String dat = s_server.arg("fl" + String(i) + "d"); dat.trim();
        String depa = s_server.arg("fl" + String(i) + "da"); depa.trim(); depa.toUpperCase();
        String arra = s_server.arg("fl" + String(i) + "aa"); arra.trim(); arra.toUpperCase();
        if (cs.length() > 0) {
            String dtstr = s_server.arg("fl" + String(i) + "dt"); dtstr.trim();
            String atstr = s_server.arg("fl" + String(i) + "at"); atstr.trim();
            new_flights[n_flights].callsign     = cs;
            new_flights[n_flights].dep_date     = dat;
            new_flights[n_flights].dep_icao     = depa;
            new_flights[n_flights].arr_icao     = arra;
            new_flights[n_flights].dep_time_str = dtstr;
            new_flights[n_flights].arr_time_str = atstr;
            n_flights++;
        }
    }

    String new_opensky_user = s_server.arg("osky_u"); new_opensky_user.trim();
    String new_opensky_pass = s_server.arg("osky_p");
    if (new_opensky_pass.isEmpty() && new_opensky_user == old_osky_user)
        new_opensky_pass = old_osky_pass;   // blank = keep the saved password

    // Parse commute
    CommuteConfig new_commute;
    new_commute.home_station = s_server.arg("cm_hs"); new_commute.home_station.trim();
    new_commute.work_station = s_server.arg("cm_ws"); new_commute.work_station.trim();

    bool new_ota_enabled = s_server.arg("ota_en") == "1";

    String new_parcel_tracking = s_server.arg("pt_num"); new_parcel_tracking.trim();
    int    new_parcel_pulses   = s_server.arg("pt_pulses").toInt();
    if (new_parcel_pulses < 0)  new_parcel_pulses = 0;
    if (new_parcel_pulses > 10) new_parcel_pulses = 10;

    // Parse pager config
    PagerConfig new_pager;
    new_pager.name  = s_server.arg("pg_n"); new_pager.name.trim();
    new_pager.topic = s_server.arg("pg_t"); new_pager.topic.trim();
    if (new_pager.topic.isEmpty()) new_pager.topic = s_pager.topic;  // keep existing
    new_pager.n_friends = 0;
    for (int i = 0; i < PAGER_MAX_FRIENDS; i++) {
        String fn = s_server.arg("pf" + String(i) + "n"); fn.trim();
        String ft = s_server.arg("pf" + String(i) + "t"); ft.trim();
        if (fn.length() > 0 && ft.length() > 0) {
            new_pager.friends[new_pager.n_friends].name  = fn;
            new_pager.friends[new_pager.n_friends].topic = ft;
            new_pager.n_friends++;
        }
    }
    config_save_pager(new_pager);

    // Parse stocks — blank fields are skipped so the list stays compact
    StocksConfig new_stocks;
    for (int i = 0; i < STOCKS_MAX; i++) {
        String sym = s_server.arg("st" + String(i)); sym.trim(); sym.toUpperCase();
        if (sym.length() > 0) new_stocks.symbols[new_stocks.count++] = sym;
    }
    new_stocks.layout     = constrain(s_server.arg("st_l").toInt(), 0, 2);
    new_stocks.rotate_sec = constrain(s_server.arg("st_r").toInt(), 3, 300);
    config_save_stocks(new_stocks);

    NightConfig new_night;
    new_night.start_min  = parse_hhmm(s_server.arg("nt_s"), new_night.start_min);
    new_night.end_min    = parse_hhmm(s_server.arg("nt_e"), new_night.end_min);
    new_night.screen_off = s_server.arg("nt_o") == "1";
    config_save_night(new_night);

    {
        Preferences prefs;
        prefs.begin(NVS_NAMESPACE, false);
        prefs.putBool(NVS_KEY_WX_PAGE, s_server.arg("wx_page") == "1");
        prefs.end();
    }
    {
        WeatherLocations wl;
        wl.device = s_server.arg("wx_dev") == "1";
        for (int i = 0; i < WEATHER_TOWNS_MAX; i++) {
            String t = s_server.arg("wt" + String(i)); t.trim();
            if (t.length() > 0) wl.towns[wl.n_towns++] = t;
        }
        config_save_weather_locations(wl);
    }

    save_to_nvs(new_ssids, new_passes, new_users, n_wifi, new_stops, n_stops,
                cd_label, cd_target, cd_icon, new_flights, n_flights,
                new_opensky_user, new_opensky_pass, new_commute, new_ota_enabled,
                new_parcel_tracking, new_parcel_pulses);

    s_server.send(200, "text/html",
        F("<!DOCTYPE html><html><head>"
          "<meta name='viewport' content='width=device-width,initial-scale=1'>"
          "<style>body{font:16px sans-serif;background:#111;color:#f7b500;"
          "text-align:center;padding:40px}</style></head><body>"
          "<h2>&#10003; Saved</h2><p>Rebooting&hellip;</p></body></html>"));
    s_saved = true;
}

// ── Firmware update via browser upload ───────────────────────────────────────

static void handle_update_page() {
    if (!authorized()) { s_server.sendHeader("Location", "/", true); s_server.send(303); return; }
    String page;
    page.reserve(1200);
    page += F("<!DOCTYPE html><html><head>"
              "<meta charset='UTF-8'>"
              "<meta name='viewport' content='width=device-width,initial-scale=1'>"
              "<title>Firmware Update</title>"
              "<style>"
              "*{box-sizing:border-box}"
              "body{font:15px/1.5 sans-serif;max-width:500px;margin:0 auto;"
                   "padding:16px;background:#111;color:#f7b500}"
              "h1{font-size:22px;margin:0 0 8px}"
              "p,small{color:#999;font-size:13px}"
              "input[type=file]{color:#ddd;margin:16px 0;display:block}"
              "button{display:block;width:100%;padding:15px;margin-top:8px;"
                      "background:#f7b500;color:#000;font-size:16px;font-weight:bold;"
                      "border:none;border-radius:8px;cursor:pointer}"
              "a{color:#555;font-size:13px}"
              ".ver{color:#666;font-size:12px;margin-bottom:16px}"
              "</style></head><body>"
              "<h1>&#9652; Firmware Update</h1>");
    page += "<p class='ver'>Current firmware: <b>v" FIRMWARE_VERSION "</b></p>";
    page += F("<p>Select a <b>.bin</b> file compiled for this device and click Upload. "
              "The device reboots automatically when the flash completes.</p>");
    if (strlen(FIRMWARE_RELEASE_URL) > 0) {
        page += "<p>&#11015; Download the latest firmware: "
                "<a href='" FIRMWARE_RELEASE_URL "' style='color:#f7b500'>"
                FIRMWARE_RELEASE_URL "</a></p>";
    }
    page += F("<form method='POST' action='/update' enctype='multipart/form-data'>"
              "<input type='file' name='firmware' accept='.bin'>"
              "<button type='submit'>Upload &amp; Flash</button>"
              "</form>"
              "<p style='margin-top:20px'><a href='/'>&#8592; Back to settings</a></p>"
              "</body></html>");
    s_server.send(200, "text/html", page);
}

static void handle_update_upload() {
    if (!authorized()) return;   // never flash anything from an unpaired browser
    HTTPUpload& up = s_server.upload();
    if (up.status == UPLOAD_FILE_START) {
        Serial.printf("[OTA] Upload start: %s\n", up.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN))
            Serial.println("[OTA] Update.begin failed");
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (Update.write(up.buf, up.currentSize) != up.currentSize)
            Serial.println("[OTA] Write error");
    } else if (up.status == UPLOAD_FILE_END) {
        if (Update.end(true))
            Serial.printf("[OTA] Flash complete: %u bytes\n", up.totalSize);
        else
            Serial.println("[OTA] Update.end failed");
    }
}

static void handle_update_finish() {
    if (!authorized()) { send_forbidden(); return; }
    bool ok = !Update.hasError();
    s_server.send(200, "text/html", ok
        ? F("<!DOCTYPE html><html><head>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<style>body{font:16px sans-serif;background:#111;color:#f7b500;"
            "text-align:center;padding:40px}</style></head><body>"
            "<h2>&#10003; Update complete</h2><p>Rebooting&hellip;</p></body></html>")
        : F("<!DOCTYPE html><html><head>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<style>body{font:16px sans-serif;background:#111;color:#e03030;"
            "text-align:center;padding:40px}</style></head><body>"
            "<h2>&#10007; Update failed</h2>"
            "<p><a href='/update' style='color:#f7b500'>Try again</a></p>"
            "</body></html>"));
    if (ok) { delay(500); ESP.restart(); }
}

static void handle_pager_test() {
    if (!authorized()) { send_forbidden(); return; }
    String topic = s_server.arg("topic"); topic.trim();
    String name  = s_server.arg("name");  name.trim();
    if (topic.isEmpty()) {
        s_server.send(400, "application/json", F("{\"ok\":false,\"msg\":\"no topic\"}"));
        return;
    }
    String from = name.isEmpty() ? "Busli" : name;
    bool ok = pager_publish(topic, from, s_pager.topic, "Test from Busli config portal");
    s_server.send(200, "application/json",
                  ok ? F("{\"ok\":true}") : F("{\"ok\":false,\"msg\":\"send failed\"}"));
}

// Fetch one quote so the user can verify a symbol/suffix before saving.
static void handle_stock_check() {
    if (!authorized()) { send_forbidden(); return; }
    String sym = s_server.arg("s"); sym.trim(); sym.toUpperCase();
    Serial.printf("[Portal] Stock check '%s' — STA status %d, IP %s\n", sym.c_str(),
                  (int)WiFi.status(), WiFi.localIP().toString().c_str());
    JsonDocument doc;
    if (sym.isEmpty()) {
        doc["ok"] = false; doc["msg"] = "empty";
    } else if (WiFi.status() != WL_CONNECTED) {
        doc["ok"] = false; doc["msg"] = "device offline";
    } else {
        StockQuote q;
        http_lock_take();
        StockFetchResult r = stocks_fetch_quote(sym, q);
        http_lock_give();
        if (r == STOCK_FETCH_OK) {
            char price[16];
            snprintf(price, sizeof(price), "%.2f", q.price);
            doc["ok"]    = true;
            doc["price"] = price;
            doc["cur"]   = q.currency;
            doc["pct"]   = q.change_pct;
        } else {
            doc["ok"]  = false;
            doc["msg"] = (r == STOCK_FETCH_NOT_FOUND) ? "not found" : "fetch failed";
        }
    }
    String out;
    serializeJson(doc, out);
    s_server.send(200, "application/json", out);
}

// Look up a town for the weather page so typos are caught before saving.
static void handle_town_check() {
    if (!authorized()) { send_forbidden(); return; }
    String q = s_server.arg("q"); q.trim();
    JsonDocument doc;
    GeoPlace p;
    if (q.isEmpty()) {
        doc["ok"] = false; doc["msg"] = "empty";
    } else if (WiFi.status() != WL_CONNECTED) {
        doc["ok"] = false; doc["msg"] = "offline";
    } else if (geo_resolve_city(q, p)) {
        doc["ok"] = true; doc["lat"] = p.lat; doc["lon"] = p.lon;
    } else {
        doc["ok"] = false; doc["msg"] = "not found";
    }
    String out;
    serializeJson(doc, out);
    s_server.send(200, "application/json", out);
}

static void handle_not_found() {
    if (!from_setup_hotspot()) { s_server.send(404, "text/plain", "Not found"); return; }
    // Captive-portal redirect — iOS/Android auto-detect and open the browser
    s_server.sendHeader("Location", "http://192.168.4.1/", true);
    s_server.send(302, "text/plain", "");
}

// ── Station search (offline, from swiss_stops.h) ──────────────────────────────

static void handle_stations() {
    if (!authorized()) { send_forbidden(); return; }
    String q = s_server.arg("q");
    q.trim();
    if (q.length() < 2 || SWISS_STOPS_COUNT == 0) {
        s_server.send(200, "application/json", "[]");
        return;
    }
    String q_lower = q;
    q_lower.toLowerCase();

    String result = "[";
    int found = 0;
    for (int i = 0; i < SWISS_STOPS_COUNT && found < 10; i++) {
        String name = String(SWISS_STOPS[i]);
        String name_lower = name;
        name_lower.toLowerCase();
        if (name_lower.indexOf(q_lower) == -1) continue;
        if (found > 0) result += ',';
        result += '"';
        for (int j = 0; j < (int)name.length(); j++) {
            char c = name[j];
            if (c == '"' || c == '\\') result += '\\';
            result += c;
        }
        result += '"';
        found++;
    }
    result += ']';
    s_server.send(200, "application/json", result);
}

// ── Public entry points ───────────────────────────────────────────────────────

// Register all pages once; the same server answers on the hotspot and the normal Wi-Fi.
static void ensure_routes() {
    if (s_routes_ready) return;
    static bool routes_added = false;   // handlers survive stop(); add them only once
    if (routes_added) { s_server.begin(); s_routes_ready = true; return; }
    routes_added = true;
    s_server.on("/",            HTTP_GET,  handle_root);
    s_server.on("/pair",        HTTP_POST, handle_pair);
    s_server.on("/save",        HTTP_POST, handle_save);
    s_server.on("/stations",    HTTP_GET,  handle_stations);
    s_server.on("/pager_test",  HTTP_GET,  handle_pager_test);
    s_server.on("/stock_check", HTTP_GET,  handle_stock_check);
    s_server.on("/town_check",  HTTP_GET,  handle_town_check);
    s_server.on("/update",      HTTP_GET,  handle_update_page);
    s_server.on("/update",      HTTP_POST, handle_update_finish, handle_update_upload);
    s_server.onNotFound(handle_not_found);
    const char* collect[] = { "Cookie" };
    s_server.collectHeaders(collect, 1);
    s_server.begin();
    s_routes_ready = true;
}

// Restart after a save, giving the browser time to receive the "Saved" page
static void finish_save_and_restart() {
    uint32_t t = millis();
    while (millis() - t < 1500) {
        s_server.handleClient();
        delay(5);
    }
    ESP.restart();
}

void config_web_set_pairing_callback(void (*cb)(const char* code)) { s_pair_cb = cb; }

void config_web_begin() {
    ensure_routes();
    s_web_enabled = true;
    if (MDNS.begin(WEB_HOSTNAME)) MDNS.addService("http", "tcp", 80);
    Serial.printf("[Web] Settings at http://%s.local or http://%s\n",
                  WEB_HOSTNAME, WiFi.localIP().toString().c_str());
}

void config_web_poll() {
    if (!s_web_enabled) return;
    s_server.handleClient();
    if (!s_pair_code.isEmpty() && millis() - s_pair_started_ms > PAIR_CODE_TTL_MS)
        pair_code_clear();
    if (s_saved) finish_save_and_restart();
}

void config_portal_run(uint32_t timeoutMs) {
    Serial.println("[Portal] Starting config portal...");
    display_show_status("Configure device");
    pair_code_clear();

    // Start AP. If we're online, keep the station link (AP+STA) so the portal can
    // reach the internet; the AP then runs on the router's channel. Modem sleep
    // doesn't work while the AP is up, so it's switched off for the portal's lifetime.
    bool sta_online = (WiFi.status() == WL_CONNECTED);
    if (sta_online) {
        esp_wifi_set_ps(WIFI_PS_NONE);
        WiFi.mode(WIFI_AP_STA);
    } else {
        WiFi.disconnect(true);   // stop reconnect scans — they'd disrupt the AP
        delay(100);
        WiFi.mode(WIFI_AP);
    }
    WiFi.softAP(PORTAL_SSID);
    delay(200);
    Serial.printf("[Portal] Internet via STA: %s\n", sta_online ? "yes" : "no");

    IPAddress ip = WiFi.softAPIP();
    Serial.printf("[Portal] AP '%s' up at %s\n", PORTAL_SSID, ip.toString().c_str());

    // Captive-portal DNS: redirect every hostname to 192.168.4.1
    s_dns.start(53, "*", ip);

    ensure_routes();

    // Show connection instructions on screen — two centered lines
    char portal_msg[120];
    snprintf(portal_msg, sizeof(portal_msg),
             "Connect to: %s\nThen open: %s\nFirmware %s",
             PORTAL_SSID, ip.toString().c_str(), FIRMWARE_VERSION);
    display_show_status(portal_msg);

    uint32_t start = millis();
    while (!s_saved) {
        s_dns.processNextRequest();
        s_server.handleClient();
        if (timeoutMs > 0 && millis() - start > timeoutMs) {
            Serial.println("[Portal] Timeout — returning to normal boot");
            s_dns.stop();
            WiFi.softAPdisconnect(true);   // back to STA-only; an existing link stays up
            if (sta_online) esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
            if (!s_web_enabled) { s_server.stop(); s_routes_ready = false; }
            return;
        }
        delay(5);
    }
    finish_save_and_restart();
}
