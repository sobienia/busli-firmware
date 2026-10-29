// ─────────────────────────────────────────────────────────────────────────────
// devlog.h — device log, viewable in the browser (Settings → Device log)
// ─────────────────────────────────────────────────────────────────────────────
// Use Log exactly like Serial (Log.printf, Log.println, …). Everything still goes
// to the USB serial port, and is also kept in memory:
//   • the last ~16 KB since this start (PSRAM), lines prefixed with the time;
//   • the last ~3 KB in RTC memory, which survives a crash or software restart
//     (not a power cut) — after a crash it shows what led up to it.
// Crashes (panic, watchdog) are counted per firmware version in NVS.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <Arduino.h>
#include <esp_system.h>

class LogTee : public Print {
public:
    size_t write(uint8_t c) override;
    size_t write(const uint8_t* buf, size_t len) override;
    using Print::write;
};
extern LogTee Log;

// First thing in setup(), after Serial.begin()
void   log_init(esp_reset_reason_t reason);

String log_recent();           // since this start (oldest lines drop out first)
String log_before_restart();   // the end of the previous run; "" after a power cut
int    log_crash_count();      // crashes since this firmware version was installed

// "power on", "crash (panic)", … — problem = it wasn't a normal restart
const char* log_reset_reason_text(esp_reset_reason_t r, bool& problem);
esp_reset_reason_t log_reset_reason();
