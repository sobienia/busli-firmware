// ─────────────────────────────────────────────────────────────────────────────
// devlog.cpp — device log (see include/devlog.h)
// ─────────────────────────────────────────────────────────────────────────────

#include "../include/devlog.h"
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <freertos/semphr.h>
#include <Preferences.h>
#include <time.h>
#include "../include/config.h"

LogTee Log;

// ── Rings ────────────────────────────────────────────────────────────────────
struct Ring {
    char*  buf;
    size_t size;
    size_t head;      // next write position
    bool   wrapped;

    void put(char c) {
        buf[head++] = c;
        if (head >= size) { head = 0; wrapped = true; }
    }
    String text() const {
        String s;
        size_t n = wrapped ? size : head;
        if (!s.reserve(n + 1)) return s;
        if (wrapped) for (size_t i = head; i < size; i++) s += buf[i];
        for (size_t i = 0; i < head; i++) s += buf[i];
        return s;
    }
};

static const size_t RECENT_SIZE = 16384;
static Ring s_recent = { nullptr, RECENT_SIZE, 0, false };

// RTC memory isn't cleared by a crash or software restart. The magic number
// tells a previous run's log from the random contents after power-on.
static const uint32_t RTC_MAGIC = 0xB0511067;
static const size_t   RTC_SIZE  = 3072;
RTC_NOINIT_ATTR static uint32_t s_rtc_magic;
RTC_NOINIT_ATTR static uint32_t s_rtc_head;
RTC_NOINIT_ATTR static uint32_t s_rtc_wrapped;
RTC_NOINIT_ATTR static char     s_rtc_buf[RTC_SIZE];
static Ring s_rtc = { s_rtc_buf, RTC_SIZE, 0, false };

static SemaphoreHandle_t  s_lock       = nullptr;
static bool               s_line_start = true;
static String             s_before;            // previous run's tail
static int                s_crashes    = 0;
static esp_reset_reason_t s_reason     = ESP_RST_UNKNOWN;

static void put(char c) {
    s_recent.put(c);
    s_rtc.put(c);
}

static void put_str(const char* s) { while (*s) put(*s++); }

// Each line starts with the time ("14:03:27 ") or, before the clock is set,
// the seconds since start ("+12s ")
static void put_timestamp() {
    char ts[16];
    time_t now = time(nullptr);
    if (now > 1700000000) {
        struct tm t; localtime_r(&now, &t);
        strftime(ts, sizeof(ts), "%H:%M:%S ", &t);
    } else {
        snprintf(ts, sizeof(ts), "+%lus ", (unsigned long)(millis() / 1000));
    }
    put_str(ts);
}

size_t LogTee::write(uint8_t c) { return write(&c, 1); }

size_t LogTee::write(const uint8_t* buf, size_t len) {
    Serial.write(buf, len);
    if (!s_lock || !s_recent.buf) return len;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return len;
    for (size_t i = 0; i < len; i++) {
        char c = (char)buf[i];
        if (c == '\r') continue;
        if (s_line_start && c != '\n') put_timestamp();
        put(c);
        s_line_start = (c == '\n');
    }
    s_rtc_head    = s_rtc.head;
    s_rtc_wrapped = s_rtc.wrapped;
    xSemaphoreGive(s_lock);
    return len;
}

// ── Setup ────────────────────────────────────────────────────────────────────

static bool is_crash(esp_reset_reason_t r) {
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
}

void log_init(esp_reset_reason_t reason) {
    s_reason = reason;
    s_recent.buf = (char*)heap_caps_malloc(RECENT_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    // What the previous run logged last (kept through a crash or restart)
    if (s_rtc_magic == RTC_MAGIC && s_rtc_head < RTC_SIZE && reason != ESP_RST_POWERON) {
        s_rtc.head    = s_rtc_head;
        s_rtc.wrapped = s_rtc_wrapped != 0;
        s_before = s_rtc.text();
        for (size_t i = 0; i < s_before.length(); i++) {   // tolerate a torn last write
            char c = s_before[i];
            if ((c < 32 && c != '\n') || c == 127) s_before.setCharAt(i, '?');
        }
    }
    s_rtc_magic = RTC_MAGIC;
    s_rtc.head = 0; s_rtc.wrapped = false;
    s_rtc_head = 0; s_rtc_wrapped = 0;
    s_lock = xSemaphoreCreateMutex();

    // Crashes since this firmware was installed. On its first start the crash
    // (if any) happened in the version before — e.g. a failed update rolled back.
    Preferences prefs;
    prefs.begin("diag", false);
    String fw = prefs.isKey("fw") ? prefs.getString("fw", "") : "";
    bool first_start = (fw != FIRMWARE_VERSION);
    s_crashes = first_start ? 0 : prefs.getInt("crashes", 0);
    if (first_start) {
        prefs.putString("fw", FIRMWARE_VERSION);
        prefs.putInt("crashes", 0);
    } else if (is_crash(reason)) {
        prefs.putInt("crashes", ++s_crashes);
    }
    prefs.end();
}

// ── Readers ──────────────────────────────────────────────────────────────────

String log_recent() {
    if (!s_lock || !s_recent.buf) return "";
    xSemaphoreTake(s_lock, portMAX_DELAY);
    String s = s_recent.text();
    xSemaphoreGive(s_lock);
    return s;
}

String log_before_restart()        { return s_before; }
int    log_crash_count()           { return s_crashes; }
esp_reset_reason_t log_reset_reason() { return s_reason; }

const char* log_reset_reason_text(esp_reset_reason_t r, bool& problem) {
    problem = false;
    switch (r) {
        case ESP_RST_POWERON:   return "power on";
        case ESP_RST_SW:        return "restart (update/save)";
        case ESP_RST_EXT:       return "reset button";
        case ESP_RST_DEEPSLEEP: return "wake from sleep";
        case ESP_RST_PANIC:     problem = true; return "crash (panic)";
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:       problem = true; return "crash (watchdog)";
        case ESP_RST_BROWNOUT:  problem = true; return "low power (brownout)";
        default:                return "other";
    }
}
