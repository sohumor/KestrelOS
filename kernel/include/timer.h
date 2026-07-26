#pragma once

#include <stdint.h>

#define TIMER_HZ 100

void timer_init(uint32_t hz);
uint64_t timer_ticks(void);
void timer_sleep(uint64_t ticks);
