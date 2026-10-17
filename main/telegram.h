#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>

/* Telegram bot messages (plain text), sent from a queue by a background
 * task: at most one message per 3 s, retried on network errors and 429.
 * POST /api/telegram/test, GET /api/telegram (call after web_start). */
void telegram_init(void);

/* True if enabled and token + chat ID are set. */
bool telegram_enabled(void);

/* Queue a message; false if disabled or the queue is full. */
bool telegram_send(const char *text);
bool telegram_sendf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Result of the last send attempt, for the settings page. */
void telegram_status(char *out, size_t n);
