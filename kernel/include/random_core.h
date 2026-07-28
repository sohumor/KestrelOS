#pragma once

#include <stdint.h>
#include <stddef.h>

/* Small dependency-free primitives used by the kernel entropy pool.
 * Exposed separately so their published test vectors can run on the host. */

struct random_sha256 {
    uint32_t h[8];
    uint64_t bytes;
    uint32_t used;
    uint8_t block[64];
};

void random_sha256_init(struct random_sha256 *s);
void random_sha256_update(struct random_sha256 *s, const void *data,
                          size_t len);
void random_sha256_final(struct random_sha256 *s, uint8_t out[32]);

/* RFC 8439 ChaCha20 block: 256-bit key, 32-bit block counter, 96-bit nonce. */
void random_chacha20_block(uint8_t out[64], const uint8_t key[32],
                           uint32_t counter, const uint8_t nonce[12]);
