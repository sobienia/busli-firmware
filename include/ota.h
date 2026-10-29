#pragma once
#include <Arduino.h>

// Set by background task when a newer firmware is found at OTA_VERSION_URL.
// Cleared by main loop after applying (or failing) the update.
extern volatile bool g_ota_pending;
extern String        g_ota_url;
extern String        g_ota_remote_version;

// Check version.json at OTA_VERSION_URL. Returns true + download URL if a
// newer firmware is available. No-op and returns false when URL is empty.
// Takes the http_lock internally — do not call while already holding it.
bool ota_check(String& out_url);

// Download the binary at url and flash it via the OTA partition.
// Returns true on success; caller should ESP.restart() afterward.
// progress_cb (optional): called with 0-100 percent during the download.
// Takes the http_lock internally.
bool ota_apply(const String& url, void (*progress_cb)(int percent) = nullptr);

// ── Safety net: automatic rollback of a broken update ───────────────────────
// After an update (downloaded or uploaded) the new firmware runs "on trial".
// It confirms itself once it has worked for a while (ota_trial_tick); if it
// crashes or restarts before that, the bootloader starts the previous version
// again. That version then skips the failed one in future checks.

// Call early in setup(). Returns the version that just failed and was rolled
// back ("" normally) — worth telling the user.
String ota_boot_check();

// True while this firmware is on trial
bool ota_on_trial();

// Call regularly from loop(). data_ok = some data source has loaded since start.
// Confirms the update once it ran OTA_TRIAL_MIN_MS with data, or after
// OTA_TRIAL_MAX_MS without Wi-Fi (the network can't be judged then); rolls back
// if it was online that long but nothing loaded.
void ota_trial_tick(bool data_ok, bool wifi_ok);

// A version that failed here earlier ("" = none); ota_check() skips it
String ota_skipped_version();
