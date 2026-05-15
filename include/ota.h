#pragma once
#include <Arduino.h>

// Set by background task when a newer firmware is found at OTA_VERSION_URL.
// Cleared by main loop after applying (or failing) the update.
extern volatile bool g_ota_pending;
extern String        g_ota_url;

// Check version.json at OTA_VERSION_URL. Returns true + download URL if a
// newer firmware is available. No-op and returns false when URL is empty.
// Takes the http_lock internally — do not call while already holding it.
bool ota_check(String& out_url);

// Download the binary at url and flash it via the OTA partition.
// Returns true on success; caller should ESP.restart() afterward.
// Takes the http_lock internally.
bool ota_apply(const String& url);
