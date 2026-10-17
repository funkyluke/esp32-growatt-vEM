#pragma once

#include <stdbool.h>
#include <time.h>

/* SNTP time with the timezone from the settings. */
void clock_init(void);

/* True once the wall clock has been set by SNTP. */
bool clock_valid(void);
