#pragma once

/* KestrelOS libtls: elliptic curves.
 *
 *   X25519  (RFC 7748) -- key agreement for the TLS 1.3 handshake.
 *   P-256   (secp256r1) -- ECDH, and ECDSA *verification* for the
 *                          certificate chain, which is most of the web.
 *
 * Both curves are implemented on their own fixed-width field arithmetic
 * rather than on bignum.c: 255 and 256 bits are small enough that four or
 * five limbs with no length bookkeeping is both faster and much easier to
 * keep free of secret-dependent branches.
 *
 * Constant-time posture, stated honestly:
 *   - X25519 is a Montgomery ladder with conditional swaps done by mask.
 *     No branch and no memory index depends on the scalar.
 *   - P-256 scalar multiplication by a secret (p256_ecdh, p256_base_mul)
 *     is double-and-add-always over a scalar first rewritten to a fixed
 *     257-bit length, selecting the result with a mask. The point
 *     addition underneath does take an early exit for the exceptional
 *     cases (an operand at infinity, or P == +-Q). With the fixed-length
 *     scalar the infinity exit fires on exactly the first iteration for
 *     every scalar, and the doubling exit requires a partial product
 *     congruent to +-1 mod the group order, so neither is reachable in a
 *     way that depends on the secret.
 *   - ECDSA verification operates entirely on public values and makes no
 *     attempt to be constant time.
 *
 * Not audited. See docs/BROWSER-PLAN.md.
 */

#include <stdint.h>
#include <stddef.h>

/* ---------------- X25519 ---------------- */

#define X25519_BYTES 32

/* out = scalar * point on Curve25519, all values little-endian as RFC
 * 7748 defines them. The scalar is clamped internally, so the caller may
 * pass raw random bytes. Returns 0, or -1 if the result is the all-zero
 * value, which means the peer sent a small-order point and the shared
 * secret must not be used. */
int x25519(uint8_t out[X25519_BYTES], const uint8_t scalar[X25519_BYTES],
           const uint8_t point[X25519_BYTES]);

/* out = scalar * basepoint (u = 9). Same return convention; the failure
 * case cannot occur for a clamped scalar. */
int x25519_base(uint8_t out[X25519_BYTES], const uint8_t scalar[X25519_BYTES]);

/* ---------------- NIST P-256 ---------------- */

#define P256_BYTES 32

/* An affine point, big-endian, as X.509 and TLS carry them. */
struct p256_point {
    uint8_t x[P256_BYTES];
    uint8_t y[P256_BYTES];
};

/* Parse an ANSI X9.62 point: 0x04 || X || Y (65 bytes) or the compressed
 * 0x02/0x03 || X (33 bytes). The point is checked to be on the curve and
 * not the point at infinity. Returns 0 or -1. */
int p256_point_from_bytes(struct p256_point *pt, const uint8_t *in, size_t len);

/* Write the uncompressed encoding. Always 65 bytes. */
void p256_point_to_bytes(const struct p256_point *pt, uint8_t out[65]);

/* Q = d * G. `d` is big-endian and must be in [1, n-1]. Returns 0 or -1. */
int p256_base_mul(struct p256_point *out, const uint8_t d[P256_BYTES]);

/* The X coordinate of d * peer, which is the TLS ECDH shared secret.
 * Returns 0, or -1 if d is out of range or peer is not a valid point. */
int p256_ecdh(uint8_t out[P256_BYTES], const uint8_t d[P256_BYTES],
              const struct p256_point *peer);

/* ECDSA verification, FIPS 186-4. `r` and `s` are big-endian integers of
 * any length up to 32 bytes (they arrive as DER INTEGERs, so leading
 * zeroes have usually already been stripped). `digest` is the message
 * hash; if it is longer than 32 bytes only the leading 256 bits are used,
 * as the standard requires. Returns 1 for a good signature, 0 otherwise. */
int p256_ecdsa_verify(const struct p256_point *pub,
                      const uint8_t *digest, size_t digest_len,
                      const uint8_t *r, size_t r_len,
                      const uint8_t *s, size_t s_len);
