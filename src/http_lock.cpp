#include "../include/http_lock.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t s_mutex = nullptr;

void http_lock_init() { s_mutex = xSemaphoreCreateMutex(); }
void http_lock_take() { if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY); }
void http_lock_give() { if (s_mutex) xSemaphoreGive(s_mutex); }
