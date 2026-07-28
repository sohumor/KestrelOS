#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Independent inputs are domain-separated before SHA-256 pool mixing. */
enum entropy_source {
    ENTROPY_BOOT = 1,
    ENTROPY_RDSEED,
    ENTROPY_RDRAND,
    ENTROPY_TIMER,
    ENTROPY_KEYBOARD,
    ENTROPY_MOUSE,
    ENTROPY_DISK,
    ENTROPY_NETWORK,
    ENTROPY_SERIAL,
    ENTROPY_SOURCE_MAX
};

#define RANDOM_NONBLOCK 0x01u
#define RANDOM_STRONG   0x02u

void random_init(uint32_t boot_seed);
void entropy_pool_add(enum entropy_source source, const void *data, size_t len,
                      unsigned credit_bits);
void entropy_pool_add_timing(enum entropy_source source, uint64_t sample);
void entropy_pool_add_interrupt(enum entropy_source source, uint32_t detail);

/* Returns 0 after filling the buffer, or -1 for a nonblocking request before
 * the pool has accumulated its initialization threshold. */
int random_get_bytes(void *buf, size_t len, unsigned flags);
uint32_t random_u32(void);
bool random_ready(void);
unsigned random_entropy_bits(void);
bool random_has_hardware_source(void);
