#pragma once

// Global HTTP mutex -- serializes all HTTP operations across cores.
// Call http_lock_init() once from setup() before fetch_task_start().
// Wrap every HTTP session: http_lock_take() -> http.begin()...http.end() -> http_lock_give().
void http_lock_init();
void http_lock_take();
void http_lock_give();
