// ─────────────────────────────────────────────────────────────────────────────
// web_tools.cpp — device log and settings backup/restore pages
// ─────────────────────────────────────────────────────────────────────────────

#include "../include/web_tools.h"
#include "../include/devlog.h"
#include "../include/config.h"
#include <ArduinoJson.h>
#include <nvs.h>
#include <nvs_flash.h>

static WebServer* s_srv = nullptr;
static bool (*s_authorized)() = nullptr;

static bool check_auth() {
    if (s_authorized && s_authorized()) return true;
    s_srv->sendHeader("Location", "/", true);   // the settings page asks to pair
    s_srv->send(303);
    return false;
}

static const char TOOL_HEAD[] PROGMEM =
    "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>%TITLE%</title><style>"
    "*{box-sizing:border-box}"
    "body{font:15px/1.5 sans-serif;max-width:900px;margin:0 auto;padding:16px;background:#111;color:#f7b500}"
    "h1{font-size:22px;margin:0 0 8px}h2{font-size:17px;margin:22px 0 6px}"
    "p,small{color:#999;font-size:13px}a{color:#f7b500}"
    "pre{background:#1a1a1a;color:#ddd;border:1px solid #333;border-radius:8px;padding:10px;"
        "font-size:12px;line-height:1.4;white-space:pre-wrap;word-break:break-all;max-height:70vh;overflow:auto}"
    ".btn{display:inline-block;padding:10px 16px;margin:6px 6px 0 0;background:#f7b500;color:#000;"
        "font-weight:bold;border:none;border-radius:8px;text-decoration:none;font-size:15px;cursor:pointer}"
    ".bad{color:#f55}label{display:block;margin:6px 0;color:#ddd}"
    "</style></head><body>";

static String head(const char* title) {
    String h = FPSTR(TOOL_HEAD);
    h.replace("%TITLE%", title);
    return h;
}

static String html_escape(const String& s) {
    String out;
    out.reserve(s.length() + s.length() / 16 + 16);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if      (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '&') out += "&amp;";
        else out += c;
    }
    return out;
}

// ── Device log ───────────────────────────────────────────────────────────────

static String uptime_text() {
    uint32_t s = millis() / 1000;
    char buf[32];
    snprintf(buf, sizeof(buf), "%lud %luh %lum", (unsigned long)(s / 86400),
             (unsigned long)(s % 86400 / 3600), (unsigned long)(s % 3600 / 60));
    return buf;
}

static void handle_log() {
    if (!check_auth()) return;
    bool problem;
    const char* reason = log_reset_reason_text(log_reset_reason(), problem);
    int crashes = log_crash_count();
    String before = log_before_restart();

    String h = head("Busli log");
    h += F("<h1>Device log</h1><p>");
    h += "Firmware v" FIRMWARE_VERSION " &middot; up " + uptime_text() + " &middot; last restart: ";
    h += problem ? "<span class='bad'>" + String(reason) + "</span>" : String(reason);
    if (crashes > 0)
        h += " &middot; <span class='bad'>" + String(crashes) + (crashes == 1 ? " crash" : " crashes") +
             " since this version was installed</span>";
    h += F("</p><a class='btn' href='/log'>Refresh</a><a class='btn' href='/log.txt' download>Download</a>"
           "<a class='btn' href='/'>Settings</a>");
    if (before.length() > 0) {
        h += F("<h2>Before the last restart</h2>"
               "<p>The last lines logged before it restarted (oldest first).</p><pre>");
        h += html_escape(before);
        h += F("</pre>");
    }
    h += F("<h2>Since this start</h2><pre id='log'>");
    h += html_escape(log_recent());
    h += F("</pre><script>var l=document.getElementById('log');l.scrollTop=l.scrollHeight;</script>"
           "</body></html>");
    s_srv->send(200, "text/html", h);
}

static void handle_log_txt() {
    if (!check_auth()) return;
    bool problem;
    String t = "Busli v" FIRMWARE_VERSION ", up " + uptime_text() + ", last restart: " +
               log_reset_reason_text(log_reset_reason(), problem) + ", crashes since install: " +
               String(log_crash_count()) + "\n";
    String before = log_before_restart();
    if (before.length() > 0) t += "\n=== Before the last restart ===\n" + before + "\n";
    t += "\n=== Since this start ===\n" + log_recent();
    s_srv->sendHeader("Content-Disposition", "attachment; filename=busli-log.txt");
    s_srv->send(200, "text/plain; charset=utf-8", t);
}

// ── Settings backup / restore ────────────────────────────────────────────────
// Every value in the settings namespaces is saved with its type, so settings
// added in later versions are included without changing this code.
// "copy" mode (setting up another Busli) keeps that device's own Wi-Fi networks
// and pager address (its friends would otherwise message the wrong device).

static const char* const BACKUP_NS[] = { "busli", "tramli" };
static bool kept_on_copy(const char* ns, const char* key) {
    return strcmp(ns, "busli") == 0 && (strcmp(key, "wifi") == 0 || strcmp(key, "pager") == 0);
}

static const char* type_name(nvs_type_t t) {
    switch (t) {
        case NVS_TYPE_U8:  return "u8";  case NVS_TYPE_I8:  return "i8";
        case NVS_TYPE_U16: return "u16"; case NVS_TYPE_I16: return "i16";
        case NVS_TYPE_U32: return "u32"; case NVS_TYPE_I32: return "i32";
        case NVS_TYPE_STR: return "str";
        default:           return nullptr;   // blobs, 64-bit: not used for settings
    }
}

static void export_ns(const char* ns, JsonObject out) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return;
    nvs_iterator_t it = nvs_entry_find(NVS_DEFAULT_PART_NAME, ns, NVS_TYPE_ANY);
    while (it) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        it = nvs_entry_next(it);
        const char* tn = type_name(info.type);
        if (!tn) continue;
        JsonObject e = out[info.key].to<JsonObject>();
        e["t"] = tn;
        switch (info.type) {
            case NVS_TYPE_U8:  { uint8_t  v; nvs_get_u8 (h, info.key, &v); e["v"] = v; break; }
            case NVS_TYPE_I8:  { int8_t   v; nvs_get_i8 (h, info.key, &v); e["v"] = v; break; }
            case NVS_TYPE_U16: { uint16_t v; nvs_get_u16(h, info.key, &v); e["v"] = v; break; }
            case NVS_TYPE_I16: { int16_t  v; nvs_get_i16(h, info.key, &v); e["v"] = v; break; }
            case NVS_TYPE_U32: { uint32_t v; nvs_get_u32(h, info.key, &v); e["v"] = v; break; }
            case NVS_TYPE_I32: { int32_t  v; nvs_get_i32(h, info.key, &v); e["v"] = v; break; }
            case NVS_TYPE_STR: {
                size_t len = 0;
                if (nvs_get_str(h, info.key, nullptr, &len) != ESP_OK) break;
                char* buf = (char*)malloc(len);
                if (!buf) break;
                if (nvs_get_str(h, info.key, buf, &len) == ESP_OK) e["v"] = buf;   // copied
                free(buf);
                break;
            }
            default: break;
        }
    }
    nvs_close(h);
}

static void handle_backup() {
    if (!check_auth()) return;
    JsonDocument doc;
    doc["busli_backup"] = 1;
    doc["firmware"]     = FIRMWARE_VERSION;
    JsonObject nss = doc["ns"].to<JsonObject>();
    for (const char* ns : BACKUP_NS) export_ns(ns, nss[ns].to<JsonObject>());
    String out;
    serializeJsonPretty(doc, out);
    char name[64] = "busli-settings.json";
    time_t now = time(nullptr);
    if (now > 1700000000) {
        struct tm t; localtime_r(&now, &t);
        strftime(name, sizeof(name), "busli-settings-%Y-%m-%d.json", &t);
    }
    s_srv->sendHeader("Content-Disposition", String("attachment; filename=") + name);
    s_srv->send(200, "application/json", out);
    Log.println("[Web] Settings backup downloaded");
}

static bool import_ns(const char* ns, JsonObject in, bool copy) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
    // Remove what's there (settings the backup doesn't have go back to defaults)
    nvs_iterator_t it = nvs_entry_find(NVS_DEFAULT_PART_NAME, ns, NVS_TYPE_ANY);
    String keys[64];
    int nk = 0;
    while (it) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        it = nvs_entry_next(it);
        if (nk < 64) keys[nk++] = info.key;
    }
    if (it) nvs_release_iterator(it);
    for (int i = 0; i < nk; i++)
        if (!(copy && kept_on_copy(ns, keys[i].c_str()))) nvs_erase_key(h, keys[i].c_str());

    for (JsonPair p : in) {
        const char* key = p.key().c_str();
        if (strlen(key) > 15 || (copy && kept_on_copy(ns, key))) continue;
        String t = p.value()["t"] | "";
        JsonVariant v = p.value()["v"];
        if      (t == "u8")  nvs_set_u8 (h, key, v.as<uint8_t>());
        else if (t == "i8")  nvs_set_i8 (h, key, v.as<int8_t>());
        else if (t == "u16") nvs_set_u16(h, key, v.as<uint16_t>());
        else if (t == "i16") nvs_set_i16(h, key, v.as<int16_t>());
        else if (t == "u32") nvs_set_u32(h, key, v.as<uint32_t>());
        else if (t == "i32") nvs_set_i32(h, key, v.as<int32_t>());
        else if (t == "str") nvs_set_str(h, key, v.as<const char*>() ? v.as<const char*>() : "");
    }
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

static void handle_restore() {
    if (!s_authorized || !s_authorized()) {
        s_srv->send(403, "application/json", F("{\"ok\":false,\"msg\":\"not paired\"}"));
        return;
    }
    bool copy = s_srv->arg("mode") == "copy";
    JsonDocument doc;
    if (deserializeJson(doc, s_srv->arg("plain")) || (doc["busli_backup"] | 0) != 1) {
        s_srv->send(400, "application/json", F("{\"ok\":false,\"msg\":\"not a Busli settings file\"}"));
        return;
    }
    bool ok = true;
    for (const char* ns : BACKUP_NS) {
        JsonObject in = doc["ns"][ns];
        if (!in.isNull()) ok &= import_ns(ns, in, copy);
    }
    Log.printf("[Web] Settings restored (%s) from v%s: %s\n", copy ? "copy" : "all",
               (const char*)(doc["firmware"] | "?"), ok ? "ok" : "FAILED");
    s_srv->send(200, "application/json", ok ? F("{\"ok\":true}") : F("{\"ok\":false,\"msg\":\"write failed\"}"));
    if (ok) { delay(800); ESP.restart(); }
}

static void handle_backup_page() {
    if (!check_auth()) return;
    String h = head("Busli backup");
    h += F("<h1>Settings backup</h1>"
           "<p>Saves all settings (stops, pages, portfolio, Wi-Fi networks and passwords, &hellip;) "
           "to a file. Keep it private: it contains your Wi-Fi passwords.</p>"
           "<a class='btn' href='/backup'>Download backup</a>"
           "<h2>Restore</h2>"
           "<input type='file' id='f' accept='.json,application/json' style='color:#ddd;margin:8px 0'>"
           "<label><input type='radio' name='m' value='all' checked> Restore everything "
           "<small>(this device, e.g. after a reset)</small></label>"
           "<label><input type='radio' name='m' value='copy'> Copy to this Busli "
           "<small>(keeps this device's Wi-Fi networks and pager address)</small></label>"
           "<button class='btn' onclick='go()'>Restore &amp; restart</button>"
           "<p id='msg'></p><p><a href='/'>&#8592; Back to settings</a></p>"
           "<script>"
           "function go(){var f=document.getElementById('f').files[0],m=document.getElementById('msg');"
             "if(!f){m.textContent='Choose a backup file first.';return;}"
             "var mode=document.querySelector('input[name=m]:checked').value;"
             "f.text().then(function(t){m.textContent='Restoring...';"
               "return fetch('/restore?mode='+mode,{method:'POST',headers:{'Content-Type':'text/plain'},body:t});})"
             ".then(function(r){return r.json();})"
             ".then(function(j){m.textContent=j.ok?'Restored. Busli is restarting...':'Failed: '+j.msg;})"
             ".catch(function(){m.textContent='Restored. Busli is restarting...';});}"
           "</script></body></html>");
    s_srv->send(200, "text/html", h);
}

// ── Registration ─────────────────────────────────────────────────────────────

void web_tools_register(WebServer& server, bool (*authorized)()) {
    s_srv        = &server;
    s_authorized = authorized;
    server.on("/log",         HTTP_GET,  handle_log);
    server.on("/log.txt",     HTTP_GET,  handle_log_txt);
    server.on("/backup",      HTTP_GET,  handle_backup);
    server.on("/backup_page", HTTP_GET,  handle_backup_page);
    server.on("/restore",     HTTP_POST, handle_restore);
}

String web_tools_links() {
    return F("<a href='/log' style='color:#999;font-size:13px'>&#9776; Device log</a>"
             "&nbsp;&nbsp;&middot;&nbsp;&nbsp;"
             "<a href='/backup_page' style='color:#999;font-size:13px'>&#8645; Backup &amp; restore</a>"
             "&nbsp;&nbsp;&middot;&nbsp;&nbsp;");
}
