// ─────────────────────────────────────────────────────────────────────────────
// ota_ui.cpp — asking before installing a firmware update
// ─────────────────────────────────────────────────────────────────────────────

#include "app.h"

String g_ota_failed_version;   // update rolled back at this start ("" = none)

// Right after setup: tell the user if the last update didn't work out
void ota_report_rollback() {
    if (g_ota_failed_version.isEmpty()) return;
    char msg[96];
    snprintf(msg, sizeof(msg), "Update to v%s didn't work\nBack on v%s",
             g_ota_failed_version.c_str(), FIRMWARE_VERSION);
    show_overlay(msg, 15000);
}

// New firmware on trial: confirm it once some data has loaded (see ota.h)
static void trial_tick() {
    if (!ota_on_trial()) return;
    SourceStatus st[SRC_COUNT];
    fetch_task_get_source_status(st);
    bool data_ok = false;
    for (int i = 0; i < SRC_COUNT; i++) data_ok |= st[i].last_ok > 0;
    ota_trial_tick(data_ok, WiFi.status() == WL_CONNECTED);
}

void ota_prompt_tick() {
    trial_tick();
    // OTA update prompt — shown when background task finds a newer firmware version.
    if (g_ota_pending) {
        g_ota_pending = false;
        display_show_ota_prompt(FIRMWARE_VERSION, g_ota_remote_version.c_str());

        // Wait up to 60 s for the user to tap Yes (right) or No (left).
        // Physical buttons: right (PIN_BTN_BRIGHT) = Yes, left (PIN_BTN_ZOOM) = No.
        bool install = false;
        bool decided = false;
        bool was_pressed = false;
        uint32_t deadline = millis() + 60000;
        while (!decided && millis() < deadline) {
            if (digitalRead(PIN_BTN_BRIGHT) == LOW) { install = true;  decided = true; }
            if (digitalRead(PIN_BTN_ZOOM)   == LOW) { install = false; decided = true; }
            touch_poll();
            bool pressed = touch_is_pressed();
            if (was_pressed && !pressed) {
                install = (touch_last_screen_x() > SCREEN_W / 2);
                decided = true;
            }
            was_pressed = pressed;
            delay(20);
        }

        if (install && g_battery_pct >= 0 && g_battery_pct < 50 && !g_battery_charging) {
            display_show_status("Battery too low\nCharge to 50% first");
            delay(4000);
            display_invalidate();
            install = false;
        }

        if (install) {
            display_show_status("Installing firmware...\n0%");
            // HTTPS download needs more stack than loopTask's 8 KB — use a dedicated task.
            // Pass our own handle so the task can resume loop() if the download fails.
            TaskHandle_t loop_task = xTaskGetCurrentTaskHandle();
            xTaskCreate([](void* arg) {
                TaskHandle_t caller = static_cast<TaskHandle_t>(arg);
                auto progress = [](int pct) {
                    char msg[40];
                    snprintf(msg, sizeof(msg), "Installing firmware...\n%d%%", pct);
                    display_show_status(msg);
                };
                if (ota_apply(g_ota_url, progress)) {
                    display_show_status("Update complete!\nRestarting...");
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    ESP.restart();
                } else {
                    display_show_status("Update failed");
                    vTaskDelay(pdMS_TO_TICKS(3000));
                    display_invalidate();
                    vTaskResume(caller); // unblock loop() so buttons work again
                }
                vTaskDelete(nullptr);
            }, "ota_apply", 32768, (void*)loop_task, 5, nullptr);
            vTaskSuspend(nullptr); // pause loop() while OTA task runs
        } else {
            display_invalidate(); // user declined — resume normal display
        }
    }
}
