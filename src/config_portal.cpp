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
#include "../include/swiss_stops.h"
#include "display.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>

#define PORTAL_SSID    "Busli-Config"
#define NVS_NAMESPACE       "busli"
#define NVS_KEY_WIFI        "wifi"
#define NVS_KEY_STOPS       "stops"
#define NVS_KEY_COUNTDOWN   "countdown"
#define NVS_KEY_FLIGHTS     "flights"
#define MAX_WIFI        3
#define MAX_STOPS       6
#define MAX_FLIGHTS     2

static WebServer  s_server(80);
static DNSServer  s_dns;
static bool       s_saved = false;

// ── NVS helpers ───────────────────────────────────────────────────────────────

int config_load_wifi(String ssids[], String passes[]) {
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
        entries[n].callsign = o["c"] | "";
        entries[n].dep_date = o["d"] | "";
        entries[n].callsign.trim();
        if (entries[n].callsign.length() > 0) n++;
    }
    return n;
}

static void save_to_nvs(String ssids[], String passes[], int n_wifi,
                         StopEntry stops[], int n_stops,
                         const String& cd_label, const String& cd_target, int cd_icon,
                         FlightEntry flights[], int n_flights) {
    // WiFi JSON
    JsonDocument wdoc;
    JsonArray warr = wdoc.to<JsonArray>();
    for (int i = 0; i < n_wifi; i++) {
        JsonObject o = warr.add<JsonObject>();
        o["s"] = ssids[i];
        o["p"] = passes[i];
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
        }
        String fl_json;
        serializeJson(fldoc, fl_json);
        prefs.putString(NVS_KEY_FLIGHTS, fl_json);
    } else {
        prefs.remove(NVS_KEY_FLIGHTS);
    }
    prefs.end();

    Serial.printf("[Portal] Saved %d WiFi, %d stops, %d flights to NVS\n", n_wifi, n_stops, n_flights);
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

static String build_page(String ssids[], String passes[],
                          StopEntry stops[], int n_stops,
                          const String& cd_label, const String& cd_target, int cd_icon,
                          FlightEntry flights[], int n_flights) {
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
        String cs  = (i < n_flights) ? flights[i].callsign : "";
        String dat = (i < n_flights) ? flights[i].dep_date : "";
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
        h += "</div>";
    }

    h += F("<button type='submit'>&#128190;&nbsp; Save &amp; Reboot</button>"
           "</form>"
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
           "</script>"
           "</body></html>");
    return h;
}

// ── Request handlers ──────────────────────────────────────────────────────────

static String s_ssids[MAX_WIFI];
static String s_passes[MAX_WIFI];
static StopEntry s_stops[MAX_STOPS];
static int s_n_stops = 0;
static String s_cd_label;
static String s_cd_target;
static int    s_cd_icon = 0;
static FlightEntry s_flights[MAX_FLIGHTS];
static int         s_n_flights = 0;

static void handle_root() {
    String page = build_page(s_ssids, s_passes, s_stops, s_n_stops,
                             s_cd_label, s_cd_target, s_cd_icon,
                             s_flights, s_n_flights);
    s_server.send(200, "text/html", page);
}

static void handle_save() {
    // Parse WiFi
    String new_ssids[MAX_WIFI], new_passes[MAX_WIFI];
    int n_wifi = 0;
    for (int i = 0; i < MAX_WIFI; i++) {
        String ss = s_server.arg("w" + String(i) + "s");
        String pp = s_server.arg("w" + String(i) + "p");
        ss.trim();
        if (ss.length() > 0) {
            new_ssids[n_wifi]  = ss;
            new_passes[n_wifi] = pp;
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
        if (cs.length() > 0) {
            new_flights[n_flights].callsign = cs;
            new_flights[n_flights].dep_date = dat;
            n_flights++;
        }
    }

    save_to_nvs(new_ssids, new_passes, n_wifi, new_stops, n_stops,
                cd_label, cd_target, cd_icon, new_flights, n_flights);

    s_server.send(200, "text/html",
        F("<!DOCTYPE html><html><head>"
          "<meta name='viewport' content='width=device-width,initial-scale=1'>"
          "<style>body{font:16px sans-serif;background:#111;color:#f7b500;"
          "text-align:center;padding:40px}</style></head><body>"
          "<h2>&#10003; Saved</h2><p>Rebooting&hellip;</p></body></html>"));
    s_saved = true;
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
    int n_wifi = config_load_wifi(s_ssids, s_passes);
    // Fill empty slots so the form shows blanks
    for (int i = n_wifi; i < MAX_WIFI; i++) { s_ssids[i] = ""; s_passes[i] = ""; }
    s_n_stops  = config_load_stops(s_stops);
    config_load_countdown(s_cd_label, s_cd_target, s_cd_icon);
    s_n_flights = config_load_flights(s_flights);

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
    s_server.on("/",         HTTP_GET,  handle_root);
    s_server.on("/save",     HTTP_POST, handle_save);
    s_server.on("/stations", HTTP_GET,  handle_stations);
    s_server.onNotFound(handle_not_found);
    s_server.begin();

    display_show_status("Join: Busli-Config");

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
