#pragma once

#include <stddef.h>

void rs485_init(void);
void rs485_send(const char *buf, size_t len);
