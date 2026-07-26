#pragma once

/* KestrelOS libtls: fixed-capacity big integers for RSA.
 *
 * Written from first principles. Limbs are 64-bit and little-endian in the
 * array (v[0] is least significant); products use the compiler's __int128,
 * which lowers to a single mulq and needs no runtime support library.
 * Nothing here divides an __int128 -- that would call libgcc's __udivti3,
 * which userspace does not link -- so the 128/64 division needed by the
 * long-division algorithm is a shift-and-subtract loop instead.
 *
 * Capacity is fixed at BN_MAX_BITS for operands so a 4096-bit RSA key
 * works, with room for a full double-width product. Every operation that
 * can exceed that returns -1 rather than truncating.
 *
 * NOT constant time. This is only ever used with public values: RSA
 * signature verification (public modulus, public exponent, public
 * signature). Secret-scalar arithmetic lives in ecc.c, which has its own
 * fixed-width constant-time field code.
 *
 * A struct bn is 1048 bytes, so these are passed by pointer and never
 * copied casually; bn_modexp's own frame is 5.2 KiB, the largest in the
 * library, against a 64 KiB user stack.
 */

#include <stdint.h>
#include <stddef.h>

#define BN_MAX_BITS   4096
#define BN_MAX_LIMBS  (BN_MAX_BITS / 64)          /* 64  -- operand width  */
#define BN_LIMBS      (2 * BN_MAX_LIMBS + 2)      /* 130 -- product width  */
#define BN_MAX_BYTES  (BN_MAX_BITS / 8)           /* 512                   */

struct bn {
    int      n;                 /* significant limbs; v[n-1] != 0, 0 == zero */
    uint64_t v[BN_LIMBS];
};

/* ---- construction ---- */

void bn_zero(struct bn *a);
void bn_set_u64(struct bn *a, uint64_t x);
void bn_copy(struct bn *dst, const struct bn *src);

/* Big-endian import. Leading zero bytes are ignored. -1 if too large. */
int  bn_from_bytes(struct bn *a, const uint8_t *b, size_t len);

/* Big-endian export, zero padded on the left to exactly `len` bytes.
 * -1 if the value does not fit in `len` bytes. */
int  bn_to_bytes(const struct bn *a, uint8_t *out, size_t len);

/* ---- inspection ---- */

int  bn_is_zero(const struct bn *a);
int  bn_bits(const struct bn *a);          /* position of highest set bit + 1 */
int  bn_bytes(const struct bn *a);         /* minimal big-endian byte length  */
int  bn_get_bit(const struct bn *a, int i);
int  bn_is_odd(const struct bn *a);
int  bn_cmp(const struct bn *a, const struct bn *b);   /* -1 / 0 / 1 */

/* ---- arithmetic ---- */

int  bn_add(struct bn *r, const struct bn *a, const struct bn *b);
int  bn_sub(struct bn *r, const struct bn *a, const struct bn *b); /* a >= b */
int  bn_mul(struct bn *r, const struct bn *a, const struct bn *b);
int  bn_shl(struct bn *r, const struct bn *a, int bits);
int  bn_shr(struct bn *r, const struct bn *a, int bits);

/* Truncated division. q and/or r may be NULL. -1 if b is zero. q and r
 * must not alias a or b. */
int  bn_divmod(struct bn *q, struct bn *r, const struct bn *a,
               const struct bn *b);
int  bn_mod(struct bn *r, const struct bn *a, const struct bn *m);

/* ---- Montgomery arithmetic ---- */

/* Precomputed form of an odd modulus. R = 2^(64*k) where k is the limb
 * count of the modulus. */
struct bn_mont {
    int      k;                       /* limbs in the modulus            */
    uint64_t n0;                      /* -n^-1 mod 2^64                  */
    uint64_t n[BN_MAX_LIMBS];         /* the modulus                     */
    uint64_t rr[BN_MAX_LIMBS];        /* R^2 mod n, for entering the form */
};

/* -1 if m is zero, even, or wider than BN_MAX_LIMBS. */
int  bn_mont_init(struct bn_mont *mt, const struct bn *m);

/* r = a * b * R^-1 mod n. Inputs must already be reduced mod n. r may
 * alias a or b. */
void bn_mont_mul(struct bn *r, const struct bn *a, const struct bn *b,
                 const struct bn_mont *mt);

int  bn_to_mont(struct bn *r, const struct bn *a, const struct bn_mont *mt);
void bn_from_mont(struct bn *r, const struct bn *a, const struct bn_mont *mt);

/* r = base^exp mod n, with n the modulus of mt. Sliding window over
 * Montgomery form. `base` is reduced first, so it may exceed n.
 *
 * NOT reentrant: the window table is a file-scope buffer so that a
 * 4096-bit exponentiation does not put 8 KiB on a 64 KiB user stack.
 */
int  bn_modexp(struct bn *r, const struct bn *base, const struct bn *exp,
               const struct bn_mont *mt);
