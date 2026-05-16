// ─────────────────────────────────────────────────────────────────────────────
// config_portal.cpp — AP-mode web config portal
// ─────────────────────────────────────────────────────────────────────────────
// Triggered by a 5-second hold of the BOOT button. Starts a WiFi AP named
// "Tramli-Config". Any device that joins and opens a browser (or is auto-
// redirected by the captive-portal DNS trick) gets a single config page:
//   • Up to 3 WiFi credentials
//   • Up to 6 stops (label, station name, optional line/direction filters)
// Saving writes to NVS and reboots.
// ─────────────────────────────────────────────────────────────────────────────

#include "../include/config_portal.h"
#include "../include/config.h"
#include "../include/pager.h"
#include "../include/swiss_stops.h"
#include "display.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
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
#define MAX_WIFI        3
#define MAX_STOPS       6
#define MAX_FLIGHTS     2

static WebServer  s_server(80);
static DNSServer  s_dns;
static bool       s_saved = false;

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
                          const PagerConfig& pager) {
    String h;
    h.reserve(10000);

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
           "<h1>Busli</h1>"
           "<p class='sub'>Connected to <b>Busli&#8209;Config</b> &mdash; "
           "save to reboot with new settings</p>"
           "<form method='POST' action='/save'>");

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
        h += "<label>Password</label>"
             "<input type='password' name='w" + String(i) + "p' value='"
             + html_encode(passes[i]) + "'>";
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
           "Both work. IATA is the number on your ticket.</p>");
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
         "<input type='password' name='osky_p' value='"
         + html_encode(opensky_pass) + "'>";
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

static void handle_root() {
    String page = build_page(s_ssids, s_passes, s_users, s_stops, s_n_stops,
                             s_cd_label, s_cd_target, s_cd_icon,
                             s_flights, s_n_flights,
                             s_opensky_user, s_opensky_pass,
                             s_commute, s_ota_enabled,
                             s_parcel_tracking, s_parcel_pulses,
                             s_pager);
    s_server.send(200, "text/html", page);
}

static void handle_save() {
    // Parse WiFi
    String new_ssids[MAX_WIFI], new_passes[MAX_WIFI], new_users[MAX_WIFI];
    int n_wifi = 0;
    for (int i = 0; i < MAX_WIFI; i++) {
        String ss = s_server.arg("w" + String(i) + "s");
        String pp = s_server.arg("w" + String(i) + "p");
        String uu = s_server.arg("w" + String(i) + "u");
        ss.trim(); uu.trim();
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

static void handle_not_found() {
    // Captive-portal redirect — iOS/Android auto-detect and open the browser
    s_server.sendHeader("Location", "http://192.168.4.1/", true);
    s_server.send(302, "text/plain", "");
}

// ── Station search (offline, from swiss_stops.h) ──────────────────────────────

static void handle_stations() {
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

// ── Public entry point ────────────────────────────────────────────────────────

void config_portal_run(uint32_t timeoutMs) {
    Serial.println("[Portal] Starting config portal...");
    display_show_status("Configure device");

    // Load current config to pre-populate the form
    int n_wifi = config_load_wifi(s_ssids, s_passes, s_users);
    // Fill empty slots so the form shows blanks
    for (int i = n_wifi; i < MAX_WIFI; i++) { s_ssids[i] = ""; s_passes[i] = ""; s_users[i] = ""; }
    s_n_stops  = config_load_stops(s_stops);
    config_load_countdown(s_cd_label, s_cd_target, s_cd_icon);
    s_n_flights = config_load_flights(s_flights);
    config_load_opensky(s_opensky_user, s_opensky_pass);
    s_commute = {};
    config_load_commute(s_commute);
    s_ota_enabled = config_load_ota_enabled();
    s_parcel_tracking = "";
    s_parcel_pulses   = 3;
    config_load_parcel(s_parcel_tracking, s_parcel_pulses);
    s_pager = {};
    config_load_pager(s_pager);

    // Start AP
    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(PORTAL_SSID);
    delay(200);

    IPAddress ip = WiFi.softAPIP();
    Serial.printf("[Portal] AP '%s' up at %s\n", PORTAL_SSID, ip.toString().c_str());

    // Captive-portal DNS: redirect every hostname to 192.168.4.1
    s_dns.start(53, "*", ip);

    // Web server routes
    s_server.on("/",           HTTP_GET,  handle_root);
    s_server.on("/save",       HTTP_POST, handle_save);
    s_server.on("/stations",   HTTP_GET,  handle_stations);
    s_server.on("/pager_test", HTTP_GET,  handle_pager_test);
    s_server.on("/update",     HTTP_GET,  handle_update_page);
    s_server.on("/update",     HTTP_POST, handle_update_finish, handle_update_upload);
    s_server.onNotFound(handle_not_found);
    s_server.begin();

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
            s_server.stop();
            s_dns.stop();
            WiFi.softAPdisconnect(true);
            return;
        }
        delay(5);
    }

    // Give browser time to receive the "Saved" page before rebooting
    uint32_t t = millis();
    while (millis() - t < 1500) {
        s_server.handleClient();
        delay(5);
    }
    ESP.restart();
}
