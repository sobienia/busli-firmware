#include "../include/http_lock.h"
#include <freertos/semphr.h>

static SemaphoreHandle_t s_main      = nullptr;
static SemaphoreHandle_t s_slow      = nullptr;
static TaskHandle_t      s_slow_task = nullptr;

static SemaphoreHandle_t lane() {
    return (s_slow_task && xTaskGetCurrentTaskHandle() == s_slow_task) ? s_slow : s_main;
}

void http_lock_init() {
    s_main = xSemaphoreCreateMutex();
    s_slow = xSemaphoreCreateMutex();
}
void http_lock_set_slow_task(TaskHandle_t task) { s_slow_task = task; }
void http_lock_take() { SemaphoreHandle_t m = lane(); if (m) xSemaphoreTake(m, portMAX_DELAY); }
void http_lock_give() { SemaphoreHandle_t m = lane(); if (m) xSemaphoreGive(m); }
