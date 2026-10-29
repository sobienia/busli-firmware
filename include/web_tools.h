// ─────────────────────────────────────────────────────────────────────────────
// web_tools.h — extra settings pages: device log, settings backup/restore
// ─────────────────────────────────────────────────────────────────────────────
//   /log         the device log (this run + the end of the previous one)
//   /log.txt     the same as a text file
//   /backup      all settings as a JSON file
//   /restore     POST: load such a file (?mode=all | copy), then restart
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <Arduino.h>
#include <WebServer.h>

// Called by config_portal with its server and its pairing check
void web_tools_register(WebServer& server, bool (*authorized)());

// Links for the bottom of the settings page
String web_tools_links();
