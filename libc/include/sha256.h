#pragma once

/* KestrelOS libc: SHA-256 (FIPS 180-4) and the account password hash.
 *
 * Written from the specification: the eight initial hash values are the
 * fractional parts of the square roots of the first eight primes, the 64
 * round constants the fractional parts of the cube roots of the first 64
 * primes, and the compression function is the published Ch/Maj/Sigma
 * round. See docs/users.md for how the password hash is layered on top.
 */

#include <stdint.h>

#define SHA256_DIGEST_LEN 32          /* raw digest bytes */
#define SHA256_HEX_LEN    64          /* hex digits, not counting the NUL */
#define SHA256_BLOCK_LEN  64          /* compression block size */

struct sha256_ctx {
    uint32_t h[8];                    /* running hash state */
    uint64_t nbits;                   /* message length so far, in bits */
    uint8_t buf[SHA256_BLOCK_LEN];    /* partial block */
    uint32_t buflen;                  /* bytes valid in buf */
};

void sha256_init(struct sha256_ctx *ctx);
void sha256_update(struct sha256_ctx *ctx, const void *data, unsigned long len);
void sha256_final(struct sha256_ctx *ctx, uint8_t out[SHA256_DIGEST_LEN]);

/* One-shot: lowercase hex of SHA-256(data[0..len)), NUL-terminated. */
void sha256_hex(const void *data, unsigned long len, char out[SHA256_HEX_LEN + 1]);

/* Account password hash, as stored in /etc/shadow.
 *
 *   d = SHA256(salt || password)
 *   repeat iters-1 times: d = SHA256(salt || d)
 *   out = lowercase hex of d
 *
 * `salt` is the NUL-terminated hex salt string from the shadow entry and
 * `pw` the NUL-terminated password; both are hashed without their NUL.
 * `iters` is clamped to at least 1. Deterministic, so tools/mkpasswd.py
 * reproduces exactly what this function computes. */
void sha256_password(const char *salt, const char *pw, unsigned long iters,
                     char out[SHA256_HEX_LEN + 1]);
