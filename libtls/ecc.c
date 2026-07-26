/* KestrelOS libtls: X25519 and NIST P-256, written from the specs.
 *
 * Two independent field implementations, because the two curves want
 * different things:
 *
 *   Curve25519 uses a 5-limb radix-2^51 representation. The prime
 *   2^255-19 is so close to a power of two that a carry out of the top
 *   limb folds back into the bottom one as a multiply by 19, which is why
 *   this layout is the standard one and why no reduction step is needed
 *   beyond the carry chain.
 *
 *   P-256 uses 4-limb Montgomery arithmetic. Its prime has a Solinas
 *   shape that admits a faster reduction, but Montgomery is uniform,
 *   branch-free, and much harder to get subtly wrong -- and it happens to
 *   be nearly free here, because the prime ends in 0xffffffffffffffff, so
 *   -p^-1 mod 2^64 is exactly 1 and the per-limb multiply by n0 vanishes.
 *
 * The curve parameters below are the published constants of the two
 * curves. Everything that operates on them is written here.
 */

#include "ecc.h"

/* ================================================================== */
/* Curve25519 field: 5 limbs of 51 bits                               */
/* ================================================================== */

#define FE_MASK 0x7ffffffffffffULL       /* 2^51 - 1 */

typedef uint64_t fe[5];

static void fe_copy(fe h, const fe f)
{
    int i;
    for (i = 0; i < 5; i++)
        h[i] = f[i];
}

static void fe_zero(fe h)
{
    int i;
    for (i = 0; i < 5; i++)
        h[i] = 0;
}

static void fe_one(fe h)
{
    fe_zero(h);
    h[0] = 1;
}

/* Fold every limb back under 2^51, wrapping the top carry into limb 0
 * with the factor 19 that 2^255 == 19 (mod p) gives us. */
static void fe_carry(fe h)
{
    uint64_t c;

    c = h[0] >> 51; h[0] &= FE_MASK; h[1] += c;
    c = h[1] >> 51; h[1] &= FE_MASK; h[2] += c;
    c = h[2] >> 51; h[2] &= FE_MASK; h[3] += c;
    c = h[3] >> 51; h[3] &= FE_MASK; h[4] += c;
    c = h[4] >> 51; h[4] &= FE_MASK; h[0] += c * 19;
    c = h[0] >> 51; h[0] &= FE_MASK; h[1] += c;
}

static void fe_add(fe h, const fe f, const fe g)
{
    int i;
    for (i = 0; i < 5; i++)
        h[i] = f[i] + g[i];
    fe_carry(h);
}

/* h = f - g, computed as f + 2p - g so that no limb goes negative.
 * 2p has limbs (2^52-38, 2^52-2, 2^52-2, 2^52-2, 2^52-2). */
static void fe_sub(fe h, const fe f, const fe g)
{
    h[0] = f[0] + 0xfffffffffffdaULL - g[0];
    h[1] = f[1] + 0xffffffffffffeULL - g[1];
    h[2] = f[2] + 0xffffffffffffeULL - g[2];
    h[3] = f[3] + 0xffffffffffffeULL - g[3];
    h[4] = f[4] + 0xffffffffffffeULL - g[4];
    fe_carry(h);
}

static void fe_mul(fe h, const fe f, const fe g)
{
    __uint128_t t0, t1, t2, t3, t4;
    uint64_t g1_19 = 19 * g[1], g2_19 = 19 * g[2];
    uint64_t g3_19 = 19 * g[3], g4_19 = 19 * g[4];
    uint64_t c;

    t0 = (__uint128_t)f[0] * g[0] + (__uint128_t)f[1] * g4_19 +
         (__uint128_t)f[2] * g3_19 + (__uint128_t)f[3] * g2_19 +
         (__uint128_t)f[4] * g1_19;
    t1 = (__uint128_t)f[0] * g[1] + (__uint128_t)f[1] * g[0] +
         (__uint128_t)f[2] * g4_19 + (__uint128_t)f[3] * g3_19 +
         (__uint128_t)f[4] * g2_19;
    t2 = (__uint128_t)f[0] * g[2] + (__uint128_t)f[1] * g[1] +
         (__uint128_t)f[2] * g[0] + (__uint128_t)f[3] * g4_19 +
         (__uint128_t)f[4] * g3_19;
    t3 = (__uint128_t)f[0] * g[3] + (__uint128_t)f[1] * g[2] +
         (__uint128_t)f[2] * g[1] + (__uint128_t)f[3] * g[0] +
         (__uint128_t)f[4] * g4_19;
    t4 = (__uint128_t)f[0] * g[4] + (__uint128_t)f[1] * g[3] +
         (__uint128_t)f[2] * g[2] + (__uint128_t)f[3] * g[1] +
         (__uint128_t)f[4] * g[0];

    c = (uint64_t)(t0 >> 51); h[0] = (uint64_t)t0 & FE_MASK; t1 += c;
    c = (uint64_t)(t1 >> 51); h[1] = (uint64_t)t1 & FE_MASK; t2 += c;
    c = (uint64_t)(t2 >> 51); h[2] = (uint64_t)t2 & FE_MASK; t3 += c;
    c = (uint64_t)(t3 >> 51); h[3] = (uint64_t)t3 & FE_MASK; t4 += c;
    c = (uint64_t)(t4 >> 51); h[4] = (uint64_t)t4 & FE_MASK;
    h[0] += c * 19;
    c = h[0] >> 51; h[0] &= FE_MASK; h[1] += c;
}

static void fe_sq(fe h, const fe f)
{
    fe_mul(h, f, f);
}

/* The ladder's one scalar multiplication, by (A-2)/4 = 121665... the
 * constant the RFC writes as a24 + 1 in the formula below. */
static void fe_mul121666(fe h, const fe f)
{
    __uint128_t p[5];
    uint64_t c;
    int i;

    for (i = 0; i < 5; i++)
        p[i] = (__uint128_t)f[i] * 121666;
    c = (uint64_t)(p[0] >> 51); h[0] = (uint64_t)p[0] & FE_MASK; p[1] += c;
    c = (uint64_t)(p[1] >> 51); h[1] = (uint64_t)p[1] & FE_MASK; p[2] += c;
    c = (uint64_t)(p[2] >> 51); h[2] = (uint64_t)p[2] & FE_MASK; p[3] += c;
    c = (uint64_t)(p[3] >> 51); h[3] = (uint64_t)p[3] & FE_MASK; p[4] += c;
    c = (uint64_t)(p[4] >> 51); h[4] = (uint64_t)p[4] & FE_MASK;
    h[0] += c * 19;
    c = h[0] >> 51; h[0] &= FE_MASK; h[1] += c;
}

/* Exchange f and g when b is 1, using a mask so nothing branches. */
static void fe_cswap(fe f, fe g, uint64_t b)
{
    uint64_t mask = (uint64_t)0 - b;
    int i;

    for (i = 0; i < 5; i++) {
        uint64_t t = mask & (f[i] ^ g[i]);
        f[i] ^= t;
        g[i] ^= t;
    }
}

/* 1/z = z^(p-2). p-2 = 2^255 - 21, which in binary is every bit set from
 * 0 to 254 except bits 2 and 4; square-and-multiply straight off that. */
static void fe_invert(fe out, const fe z)
{
    fe t;
    int i;

    fe_copy(t, z);                       /* bit 254 */
    for (i = 253; i >= 0; i--) {
        fe_sq(t, t);
        if (i != 2 && i != 4)
            fe_mul(t, t, z);
    }
    fe_copy(out, t);
}

static void fe_frombytes(fe h, const uint8_t s[32])
{
    uint64_t t[4];
    int i, j;

    for (i = 0; i < 4; i++) {
        t[i] = 0;
        for (j = 7; j >= 0; j--)
            t[i] = (t[i] << 8) | s[i * 8 + j];
    }
    t[3] &= 0x7fffffffffffffffULL;        /* RFC 7748: ignore bit 255 */
    h[0] = t[0] & FE_MASK;
    h[1] = ((t[0] >> 51) | (t[1] << 13)) & FE_MASK;
    h[2] = ((t[1] >> 38) | (t[2] << 26)) & FE_MASK;
    h[3] = ((t[2] >> 25) | (t[3] << 39)) & FE_MASK;
    h[4] = (t[3] >> 12) & FE_MASK;
}

static void fe_tobytes(uint8_t s[32], const fe f)
{
    fe h;
    uint64_t q, c, t[4];
    int i, j;

    fe_copy(h, f);
    fe_carry(h);
    /* h is now under 2^255 but may still be at or above p; add 19 and see
     * whether that carries out of bit 254. */
    q = (h[0] + 19) >> 51;
    q = (h[1] + q) >> 51;
    q = (h[2] + q) >> 51;
    q = (h[3] + q) >> 51;
    q = (h[4] + q) >> 51;
    h[0] += 19 * q;
    c = h[0] >> 51; h[0] &= FE_MASK; h[1] += c;
    c = h[1] >> 51; h[1] &= FE_MASK; h[2] += c;
    c = h[2] >> 51; h[2] &= FE_MASK; h[3] += c;
    c = h[3] >> 51; h[3] &= FE_MASK; h[4] += c;
    h[4] &= FE_MASK;

    t[0] = h[0] | (h[1] << 51);
    t[1] = (h[1] >> 13) | (h[2] << 38);
    t[2] = (h[2] >> 26) | (h[3] << 25);
    t[3] = (h[3] >> 39) | (h[4] << 12);
    for (i = 0; i < 4; i++)
        for (j = 0; j < 8; j++)
            s[i * 8 + j] = (uint8_t)(t[i] >> (8 * j));
}

/* RFC 7748 section 5: the Montgomery ladder on the u-coordinate. */
static void x25519_scalarmult(uint8_t out[32], const uint8_t scalar[32],
                              const uint8_t point[32])
{
    uint8_t k[32];
    fe x1, x2, z2, x3, z3, a, aa, b, bb, e, c, d, da, cb, t;
    uint64_t swap = 0;
    int i;

    for (i = 0; i < 32; i++)
        k[i] = scalar[i];
    k[0] &= 248;                         /* clamp: clear the low 3 bits  */
    k[31] &= 127;                        /* clear bit 255                */
    k[31] |= 64;                         /* set bit 254                  */

    fe_frombytes(x1, point);
    fe_one(x2);
    fe_zero(z2);
    fe_copy(x3, x1);
    fe_one(z3);

    for (i = 254; i >= 0; i--) {
        uint64_t kt = (k[i / 8] >> (i & 7)) & 1;
        swap ^= kt;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = kt;

        fe_add(a, x2, z2);
        fe_sq(aa, a);
        fe_sub(b, x2, z2);
        fe_sq(bb, b);
        fe_sub(e, aa, bb);
        fe_add(c, x3, z3);
        fe_sub(d, x3, z3);
        fe_mul(da, d, a);
        fe_mul(cb, c, b);
        fe_add(t, da, cb);
        fe_sq(x3, t);
        fe_sub(t, da, cb);
        fe_sq(t, t);
        fe_mul(z3, x1, t);
        fe_mul(x2, aa, bb);
        /* z2 = E * (AA + a24*E) with a24 = 121665. Multiplying by 121666
         * and adding BB instead of AA gives the same value -- both come to
         * 121666*AA - 121665*BB -- and keeps the constant a single limb. */
        fe_mul121666(t, e);
        fe_add(t, t, bb);
        fe_mul(z2, e, t);
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe_invert(z2, z2);
    fe_mul(x2, x2, z2);
    fe_tobytes(out, x2);
}

int x25519(uint8_t out[X25519_BYTES], const uint8_t scalar[X25519_BYTES],
           const uint8_t point[X25519_BYTES])
{
    uint8_t acc = 0;
    int i;

    x25519_scalarmult(out, scalar, point);
    for (i = 0; i < 32; i++)
        acc |= out[i];
    return acc ? 0 : -1;
}

int x25519_base(uint8_t out[X25519_BYTES], const uint8_t scalar[X25519_BYTES])
{
    uint8_t base[32];
    int i;

    for (i = 0; i < 32; i++)
        base[i] = 0;
    base[0] = 9;
    return x25519(out, scalar, base);
}

/* ================================================================== */
/* 256-bit Montgomery arithmetic, used for both P-256 fields          */
/* ================================================================== */

struct m256 {
    uint64_t m[4];                       /* the modulus, top bit set   */
    uint64_t one[4];                     /* R mod m                    */
    uint64_t rr[4];                      /* R^2 mod m                  */
    uint64_t n0;                         /* -m^-1 mod 2^64             */
};

static void l_copy(uint64_t r[4], const uint64_t a[4])
{
    int i;
    for (i = 0; i < 4; i++)
        r[i] = a[i];
}

static void l_zero(uint64_t r[4])
{
    int i;
    for (i = 0; i < 4; i++)
        r[i] = 0;
}

static int l_is_zero(const uint64_t a[4])
{
    return (a[0] | a[1] | a[2] | a[3]) == 0;
}

static int l_eq(const uint64_t a[4], const uint64_t b[4])
{
    return ((a[0] ^ b[0]) | (a[1] ^ b[1]) |
            (a[2] ^ b[2]) | (a[3] ^ b[3])) == 0;
}

/* -1 / 0 / 1 */
static int l_cmp(const uint64_t a[4], const uint64_t b[4])
{
    int i;
    for (i = 3; i >= 0; i--) {
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

static void l_select(uint64_t r[4], const uint64_t a[4], const uint64_t b[4],
                     uint64_t bit)
{
    uint64_t mask = (uint64_t)0 - bit;   /* bit==1 -> take b */
    int i;
    for (i = 0; i < 4; i++)
        r[i] = (a[i] & ~mask) | (b[i] & mask);
}

static void m256_add(uint64_t r[4], const uint64_t a[4], const uint64_t b[4],
                     const struct m256 *mt)
{
    uint64_t t[4], d[4], carry = 0, mask;
    int64_t borrow = 0;
    int i;

    for (i = 0; i < 4; i++) {
        __uint128_t s = (__uint128_t)a[i] + b[i] + carry;
        t[i] = (uint64_t)s;
        carry = (uint64_t)(s >> 64);
    }
    for (i = 0; i < 4; i++) {
        __int128 x = (__int128)t[i] - (__int128)mt->m[i] + borrow;
        d[i] = (uint64_t)x;
        borrow = (int64_t)(x >> 64);
    }
    mask = (uint64_t)0 - (uint64_t)((carry | (uint64_t)(borrow + 1)) != 0);
    for (i = 0; i < 4; i++)
        r[i] = (t[i] & ~mask) | (d[i] & mask);
}

static void m256_sub(uint64_t r[4], const uint64_t a[4], const uint64_t b[4],
                     const struct m256 *mt)
{
    uint64_t t[4], mask, carry = 0;
    int64_t borrow = 0;
    int i;

    for (i = 0; i < 4; i++) {
        __int128 x = (__int128)a[i] - (__int128)b[i] + borrow;
        t[i] = (uint64_t)x;
        borrow = (int64_t)(x >> 64);
    }
    mask = (uint64_t)0 - (uint64_t)(borrow != 0);
    for (i = 0; i < 4; i++) {
        __uint128_t s = (__uint128_t)t[i] + (mt->m[i] & mask) + carry;
        r[i] = (uint64_t)s;
        carry = (uint64_t)(s >> 64);
    }
}

/* CIOS Montgomery multiplication, k fixed at 4. */
static void m256_mul(uint64_t r[4], const uint64_t a[4], const uint64_t b[4],
                     const struct m256 *mt)
{
    uint64_t t[6], d[4], mask;
    int64_t borrow = 0;
    int i, j;

    for (i = 0; i < 6; i++)
        t[i] = 0;
    for (i = 0; i < 4; i++) {
        uint64_t bi = b[i], carry = 0, mu;
        __uint128_t p;

        for (j = 0; j < 4; j++) {
            p = (__uint128_t)a[j] * bi + t[j] + carry;
            t[j] = (uint64_t)p;
            carry = (uint64_t)(p >> 64);
        }
        p = (__uint128_t)t[4] + carry;
        t[4] = (uint64_t)p;
        t[5] = (uint64_t)(p >> 64);

        mu = t[0] * mt->n0;
        p = (__uint128_t)mu * mt->m[0] + t[0];
        carry = (uint64_t)(p >> 64);
        for (j = 1; j < 4; j++) {
            p = (__uint128_t)mu * mt->m[j] + t[j] + carry;
            t[j - 1] = (uint64_t)p;
            carry = (uint64_t)(p >> 64);
        }
        p = (__uint128_t)t[4] + carry;
        t[3] = (uint64_t)p;
        t[4] = t[5] + (uint64_t)(p >> 64);
    }
    for (i = 0; i < 4; i++) {
        __int128 x = (__int128)t[i] - (__int128)mt->m[i] + borrow;
        d[i] = (uint64_t)x;
        borrow = (int64_t)(x >> 64);
    }
    mask = (uint64_t)0 - (uint64_t)((t[4] | (uint64_t)(borrow + 1)) != 0);
    for (i = 0; i < 4; i++)
        r[i] = (t[i] & ~mask) | (d[i] & mask);
}

static void m256_sqr(uint64_t r[4], const uint64_t a[4], const struct m256 *mt)
{
    m256_mul(r, a, a, mt);
}

static void m256_to(uint64_t r[4], const uint64_t a[4], const struct m256 *mt)
{
    m256_mul(r, a, mt->rr, mt);
}

static void m256_from(uint64_t r[4], const uint64_t a[4], const struct m256 *mt)
{
    uint64_t one[4];

    l_zero(one);
    one[0] = 1;
    m256_mul(r, a, one, mt);
}

/* r = a^e in the Montgomery domain. The exponent is always a public
 * constant here (p-2, (p+1)/4, n-2), so square-and-multiply leaks
 * nothing about the base. */
static void m256_exp(uint64_t r[4], const uint64_t a[4], const uint64_t e[4],
                     const struct m256 *mt)
{
    uint64_t acc[4];
    int i;

    l_copy(acc, mt->one);
    for (i = 255; i >= 0; i--) {
        m256_sqr(acc, acc, mt);
        if ((e[i / 64] >> (i % 64)) & 1)
            m256_mul(acc, acc, a, mt);
    }
    l_copy(r, acc);
}

static uint64_t m256_n0(uint64_t m0)
{
    uint64_t x = 1;
    int i;

    for (i = 0; i < 6; i++)
        x *= 2 - m0 * x;
    return ~x + 1;
}

/* The modulus must have its top bit set, which both P-256 moduli do.
 * That makes R mod m simply 2^256 - m. */
static void m256_init(struct m256 *mt, const uint64_t m[4])
{
    uint64_t carry = 0;
    int i;

    l_copy(mt->m, m);
    mt->n0 = m256_n0(m[0]);
    for (i = 0; i < 4; i++) {             /* one = 0 - m = 2^256 - m */
        __uint128_t s = (__uint128_t)(~m[i]) + carry + (i == 0 ? 1u : 0u);
        mt->one[i] = (uint64_t)s;
        carry = (uint64_t)(s >> 64);
    }
    /* R^2 = R * 2^256: double R mod m two hundred and fifty six times. */
    l_copy(mt->rr, mt->one);
    for (i = 0; i < 256; i++)
        m256_add(mt->rr, mt->rr, mt->rr, mt);
}

/* ================================================================== */
/* P-256                                                              */
/* ================================================================== */

/* The published domain parameters of secp256r1, as little-endian limbs.
 *   p = 2^256 - 2^224 + 2^192 + 2^96 - 1
 *   n = the group order
 *   b = the curve coefficient; a is fixed at -3
 *   G = the standard base point
 */
static const uint64_t P256_P[4] = {
    0xffffffffffffffffULL, 0x00000000ffffffffULL,
    0x0000000000000000ULL, 0xffffffff00000001ULL
};
static const uint64_t P256_N[4] = {
    0xf3b9cac2fc632551ULL, 0xbce6faada7179e84ULL,
    0xffffffffffffffffULL, 0xffffffff00000000ULL
};
static const uint64_t P256_B[4] = {
    0x3bce3c3e27d2604bULL, 0x651d06b0cc53b0f6ULL,
    0xb3ebbd55769886bcULL, 0x5ac635d8aa3a93e7ULL
};
static const uint64_t P256_GX[4] = {
    0xf4a13945d898c296ULL, 0x77037d812deb33a0ULL,
    0xf8bce6e563a440f2ULL, 0x6b17d1f2e12c4247ULL
};
static const uint64_t P256_GY[4] = {
    0xcbb6406837bf51f5ULL, 0x2bce33576b315eceULL,
    0x8ee7eb4a7c0f9e16ULL, 0x4fe342e2fe1a7f9bULL
};

struct jac {
    uint64_t x[4];
    uint64_t y[4];
    uint64_t z[4];                       /* z == 0 means the point at infinity */
};

static struct m256 fp_ctx, fn_ctx;
static uint64_t fp_b_mont[4];
static uint64_t fp_exp_inv[4];           /* p - 2        */
static uint64_t fp_exp_sqrt[4];          /* (p + 1) / 4  */
static uint64_t fn_exp_inv[4];           /* n - 2        */
static struct jac p256_g;
static int p256_ready;

/* r = a - small, for a 4-limb value that cannot underflow. */
static void l_sub_small(uint64_t r[4], const uint64_t a[4], uint64_t s)
{
    int64_t borrow = 0;
    int i;
    __int128 x = (__int128)a[0] - (__int128)s;

    r[0] = (uint64_t)x;
    borrow = (int64_t)(x >> 64);
    for (i = 1; i < 4; i++) {
        x = (__int128)a[i] + borrow;
        r[i] = (uint64_t)x;
        borrow = (int64_t)(x >> 64);
    }
}

static void l_shr(uint64_t r[4], const uint64_t a[4], int bits)
{
    int words = bits / 64, sh = bits % 64;
    int i;

    for (i = 0; i < 4; i++) {
        uint64_t lo = (i + words < 4) ? (a[i + words] >> sh) : 0;
        uint64_t hi = (sh && i + words + 1 < 4) ? (a[i + words + 1] << (64 - sh))
                                                : 0;
        r[i] = lo | hi;
    }
}

static void p256_init(void)
{
    uint64_t tmp[4];

    if (p256_ready)
        return;
    m256_init(&fp_ctx, P256_P);
    m256_init(&fn_ctx, P256_N);
    m256_to(fp_b_mont, P256_B, &fp_ctx);
    l_sub_small(fp_exp_inv, P256_P, 2);
    l_sub_small(fn_exp_inv, P256_N, 2);
    /* (p+1)/4: p ends in ...ffff, so p+1 cannot carry past limb 0 here
     * only if limb 0 is not all ones -- it is, so do it the long way. */
    {
        uint64_t carry = 1;
        int i;
        for (i = 0; i < 4; i++) {
            __uint128_t s = (__uint128_t)P256_P[i] + carry;
            tmp[i] = (uint64_t)s;
            carry = (uint64_t)(s >> 64);
        }
        l_shr(fp_exp_sqrt, tmp, 2);
    }
    m256_to(p256_g.x, P256_GX, &fp_ctx);
    m256_to(p256_g.y, P256_GY, &fp_ctx);
    l_copy(p256_g.z, fp_ctx.one);
    p256_ready = 1;
}

/* ---- byte conversion (big-endian, as X.509 and TLS use) ---- */

static void be_to_limbs(uint64_t r[4], const uint8_t in[32])
{
    int i, j;

    for (i = 0; i < 4; i++) {
        uint64_t v = 0;
        for (j = 0; j < 8; j++)
            v = (v << 8) | in[(3 - i) * 8 + j];
        r[i] = v;
    }
}

static void limbs_to_be(uint8_t out[32], const uint64_t a[4])
{
    int i, j;

    for (i = 0; i < 4; i++)
        for (j = 0; j < 8; j++)
            out[(3 - i) * 8 + j] = (uint8_t)(a[i] >> (56 - 8 * j));
}

/* ---- group law in Jacobian coordinates ---- */

static void jac_set_inf(struct jac *r)
{
    l_zero(r->x);
    l_zero(r->y);
    l_zero(r->z);
}

static int jac_is_inf(const struct jac *p)
{
    return l_is_zero(p->z);
}

static void jac_copy(struct jac *r, const struct jac *p)
{
    l_copy(r->x, p->x);
    l_copy(r->y, p->y);
    l_copy(r->z, p->z);
}

static void jac_select(struct jac *r, const struct jac *a, const struct jac *b,
                       uint64_t bit)
{
    l_select(r->x, a->x, b->x, bit);
    l_select(r->y, a->y, b->y, bit);
    l_select(r->z, a->z, b->z, bit);
}

/* "dbl-2001-b": the standard Jacobian doubling specialised to a == -3,
 * which is what lets alpha be 3*(X-Z^2)*(X+Z^2). */
static void jac_dbl(struct jac *r, const struct jac *p)
{
    uint64_t delta[4], gamma[4], beta[4], alpha[4], t[4], u[4], x3[4], y3[4],
             z3[4];
    const struct m256 *f = &fp_ctx;

    if (jac_is_inf(p)) {
        jac_set_inf(r);
        return;
    }
    m256_sqr(delta, p->z, f);
    m256_sqr(gamma, p->y, f);
    m256_mul(beta, p->x, gamma, f);
    m256_sub(t, p->x, delta, f);
    m256_add(u, p->x, delta, f);
    m256_mul(alpha, t, u, f);
    m256_add(t, alpha, alpha, f);
    m256_add(alpha, t, alpha, f);        /* alpha = 3*(X-delta)*(X+delta) */

    m256_sqr(x3, alpha, f);
    m256_add(t, beta, beta, f);          /* 2 beta  */
    m256_add(t, t, t, f);                /* 4 beta  */
    m256_add(u, t, t, f);                /* 8 beta  */
    m256_sub(x3, x3, u, f);

    m256_add(z3, p->y, p->z, f);
    m256_sqr(z3, z3, f);
    m256_sub(z3, z3, gamma, f);
    m256_sub(z3, z3, delta, f);

    m256_sub(y3, t, x3, f);              /* 4 beta - X3 */
    m256_mul(y3, alpha, y3, f);
    m256_sqr(u, gamma, f);               /* gamma^2 */
    m256_add(u, u, u, f);
    m256_add(u, u, u, f);
    m256_add(u, u, u, f);                /* 8 gamma^2 */
    m256_sub(y3, y3, u, f);

    l_copy(r->x, x3);
    l_copy(r->y, y3);
    l_copy(r->z, z3);
}

/* "add-2007-bl". The exceptional cases are handled explicitly; see the
 * constant-time note in ecc.h for why that is safe here. */
static void jac_add(struct jac *r, const struct jac *p, const struct jac *q)
{
    uint64_t z1z1[4], z2z2[4], u1[4], u2[4], s1[4], s2[4], h[4], i_[4], j[4],
             rr[4], v[4], x3[4], y3[4], z3[4], t[4];
    const struct m256 *f = &fp_ctx;

    if (jac_is_inf(p)) {
        jac_copy(r, q);
        return;
    }
    if (jac_is_inf(q)) {
        jac_copy(r, p);
        return;
    }
    m256_sqr(z1z1, p->z, f);
    m256_sqr(z2z2, q->z, f);
    m256_mul(u1, p->x, z2z2, f);
    m256_mul(u2, q->x, z1z1, f);
    m256_mul(s1, p->y, q->z, f);
    m256_mul(s1, s1, z2z2, f);
    m256_mul(s2, q->y, p->z, f);
    m256_mul(s2, s2, z1z1, f);
    m256_sub(h, u2, u1, f);
    m256_sub(rr, s2, s1, f);
    if (l_is_zero(h)) {
        if (l_is_zero(rr))
            jac_dbl(r, p);
        else
            jac_set_inf(r);
        return;
    }
    m256_add(rr, rr, rr, f);             /* r = 2*(S2-S1) */
    m256_add(i_, h, h, f);
    m256_sqr(i_, i_, f);                 /* I = (2H)^2    */
    m256_mul(j, h, i_, f);               /* J = H*I       */
    m256_mul(v, u1, i_, f);              /* V = U1*I      */

    m256_sqr(x3, rr, f);
    m256_sub(x3, x3, j, f);
    m256_add(t, v, v, f);
    m256_sub(x3, x3, t, f);

    m256_sub(y3, v, x3, f);
    m256_mul(y3, rr, y3, f);
    m256_mul(t, s1, j, f);
    m256_add(t, t, t, f);
    m256_sub(y3, y3, t, f);

    m256_add(z3, p->z, q->z, f);
    m256_sqr(z3, z3, f);
    m256_sub(z3, z3, z1z1, f);
    m256_sub(z3, z3, z2z2, f);
    m256_mul(z3, z3, h, f);

    l_copy(r->x, x3);
    l_copy(r->y, y3);
    l_copy(r->z, z3);
}

/* Jacobian -> affine, both still in the Montgomery domain. */
static int jac_affine(uint64_t ax[4], uint64_t ay[4], const struct jac *p)
{
    uint64_t zi[4], zi2[4], zi3[4];
    const struct m256 *f = &fp_ctx;

    if (jac_is_inf(p))
        return -1;
    m256_exp(zi, p->z, fp_exp_inv, f);
    m256_sqr(zi2, zi, f);
    m256_mul(zi3, zi2, zi, f);
    m256_mul(ax, p->x, zi2, f);
    m256_mul(ay, p->y, zi3, f);
    return 0;
}

/* y^2 == x^3 - 3x + b, all in the Montgomery domain. */
static int on_curve(const uint64_t x[4], const uint64_t y[4])
{
    uint64_t lhs[4], rhs[4], t[4];
    const struct m256 *f = &fp_ctx;

    m256_sqr(lhs, y, f);
    m256_sqr(rhs, x, f);
    m256_mul(rhs, rhs, x, f);
    m256_add(t, x, x, f);
    m256_add(t, t, x, f);                /* 3x */
    m256_sub(rhs, rhs, t, f);
    m256_add(rhs, rhs, fp_b_mont, f);
    return l_eq(lhs, rhs);
}

/* Rewrite k as k + n or k + 2n so the result is always exactly 257 bits.
 * The ladder then runs a fixed number of iterations whose first step is
 * unconditionally the "double infinity, add P" case, for every scalar. */
static void fixed_len_scalar(uint64_t t[5], const uint64_t k[4])
{
    uint64_t carry = 0;
    int i;

    for (i = 0; i < 4; i++) {
        __uint128_t s = (__uint128_t)k[i] + P256_N[i] + carry;
        t[i] = (uint64_t)s;
        carry = (uint64_t)(s >> 64);
    }
    t[4] = carry;
    if (t[4] == 0) {
        carry = 0;
        for (i = 0; i < 4; i++) {
            __uint128_t s = (__uint128_t)t[i] + P256_N[i] + carry;
            t[i] = (uint64_t)s;
            carry = (uint64_t)(s >> 64);
        }
        t[4] = carry;
    }
}

static void jac_mul_secret(struct jac *out, const uint64_t k[4],
                           const struct jac *p)
{
    uint64_t t[5];
    struct jac acc, tmp;
    int i;

    fixed_len_scalar(t, k);
    jac_set_inf(&acc);
    for (i = 256; i >= 0; i--) {
        uint64_t bit = (t[i / 64] >> (i % 64)) & 1;
        jac_dbl(&acc, &acc);
        jac_add(&tmp, &acc, p);
        jac_select(&acc, &acc, &tmp, bit);
    }
    jac_copy(out, &acc);
}

/* u1*G + u2*Q by Shamir's trick over a four-entry table. Public inputs
 * only -- this is the signature verification path. */
static void jac_mul2_public(struct jac *out, const uint64_t u1[4],
                            const uint64_t u2[4], const struct jac *q)
{
    struct jac tab[4], acc;
    int i;

    jac_set_inf(&tab[0]);
    jac_copy(&tab[1], &p256_g);
    jac_copy(&tab[2], q);
    jac_add(&tab[3], &p256_g, q);

    jac_set_inf(&acc);
    for (i = 255; i >= 0; i--) {
        int idx;
        jac_dbl(&acc, &acc);
        idx = (int)((u1[i / 64] >> (i % 64)) & 1) |
              (int)(((u2[i / 64] >> (i % 64)) & 1) << 1);
        if (idx)
            jac_add(&acc, &acc, &tab[idx]);
    }
    jac_copy(out, &acc);
}

/* ---- public API ---- */

int p256_point_from_bytes(struct p256_point *pt, const uint8_t *in, size_t len)
{
    uint64_t x[4], y[4], xm[4], ym[4];

    p256_init();
    if (!pt || !in)
        return -1;

    if (len == 65 && in[0] == 0x04) {
        be_to_limbs(x, in + 1);
        be_to_limbs(y, in + 33);
    } else if (len == 33 && (in[0] == 0x02 || in[0] == 0x03)) {
        uint64_t rhs[4], t[4], cand[4], sq[4], neg[4], plain[4];
        const struct m256 *f = &fp_ctx;

        be_to_limbs(x, in + 1);
        if (l_cmp(x, P256_P) >= 0)
            return -1;
        m256_to(xm, x, f);
        m256_sqr(rhs, xm, f);
        m256_mul(rhs, rhs, xm, f);
        m256_add(t, xm, xm, f);
        m256_add(t, t, xm, f);
        m256_sub(rhs, rhs, t, f);
        m256_add(rhs, rhs, fp_b_mont, f);
        m256_exp(cand, rhs, fp_exp_sqrt, f);
        m256_sqr(sq, cand, f);
        if (!l_eq(sq, rhs))
            return -1;                   /* x is not on the curve */
        m256_from(plain, cand, f);
        m256_sub(neg, fp_ctx.m, plain, f);
        if ((plain[0] & 1) != (uint64_t)(in[0] & 1))
            l_copy(plain, neg);
        l_copy(y, plain);
    } else {
        return -1;
    }

    if (l_cmp(x, P256_P) >= 0 || l_cmp(y, P256_P) >= 0)
        return -1;
    if (l_is_zero(x) && l_is_zero(y))
        return -1;                       /* not a valid encoding */
    m256_to(xm, x, &fp_ctx);
    m256_to(ym, y, &fp_ctx);
    if (!on_curve(xm, ym))
        return -1;
    limbs_to_be(pt->x, x);
    limbs_to_be(pt->y, y);
    return 0;
}

void p256_point_to_bytes(const struct p256_point *pt, uint8_t out[65])
{
    int i;

    out[0] = 0x04;
    for (i = 0; i < 32; i++) {
        out[1 + i] = pt->x[i];
        out[33 + i] = pt->y[i];
    }
}

static int scalar_in_range(uint64_t k[4], const uint8_t d[32])
{
    be_to_limbs(k, d);
    if (l_is_zero(k))
        return -1;
    if (l_cmp(k, P256_N) >= 0)
        return -1;
    return 0;
}

int p256_base_mul(struct p256_point *out, const uint8_t d[P256_BYTES])
{
    uint64_t k[4], ax[4], ay[4], px[4], py[4];
    struct jac r;

    p256_init();
    if (!out || !d)
        return -1;
    if (scalar_in_range(k, d) < 0)
        return -1;
    jac_mul_secret(&r, k, &p256_g);
    if (jac_affine(ax, ay, &r) < 0)
        return -1;
    m256_from(px, ax, &fp_ctx);
    m256_from(py, ay, &fp_ctx);
    limbs_to_be(out->x, px);
    limbs_to_be(out->y, py);
    return 0;
}

int p256_ecdh(uint8_t out[P256_BYTES], const uint8_t d[P256_BYTES],
              const struct p256_point *peer)
{
    uint64_t k[4], x[4], y[4], ax[4], ay[4], px[4];
    struct jac p, r;

    p256_init();
    if (!out || !d || !peer)
        return -1;
    if (scalar_in_range(k, d) < 0)
        return -1;
    be_to_limbs(x, peer->x);
    be_to_limbs(y, peer->y);
    if (l_cmp(x, P256_P) >= 0 || l_cmp(y, P256_P) >= 0)
        return -1;
    m256_to(p.x, x, &fp_ctx);
    m256_to(p.y, y, &fp_ctx);
    l_copy(p.z, fp_ctx.one);
    if (!on_curve(p.x, p.y))
        return -1;
    jac_mul_secret(&r, k, &p);
    if (jac_affine(ax, ay, &r) < 0)
        return -1;
    m256_from(px, ax, &fp_ctx);
    limbs_to_be(out, px);
    return 0;
}

/* FIPS 186-4 section 4.6, with the leftmost-bits rule of section 6.4. */
int p256_ecdsa_verify(const struct p256_point *pub,
                      const uint8_t *digest, size_t digest_len,
                      const uint8_t *r, size_t r_len,
                      const uint8_t *s, size_t s_len)
{
    uint8_t ebuf[32];
    uint64_t rl[4], sl[4], el[4], w[4], u1[4], u2[4], qx[4], qy[4];
    uint64_t rm[4], sm[4], em[4], xr[4];
    struct jac q, res;
    size_t take, i;

    p256_init();
    if (!pub || !digest || !r || !s)
        return 0;
    if (r_len == 0 || r_len > 32 || s_len == 0 || s_len > 32)
        return 0;
    if (digest_len == 0)
        return 0;

    /* r, s must be in [1, n-1]. */
    {
        uint8_t tmp[32];
        for (i = 0; i < 32; i++)
            tmp[i] = 0;
        for (i = 0; i < r_len; i++)
            tmp[32 - r_len + i] = r[i];
        be_to_limbs(rl, tmp);
        for (i = 0; i < 32; i++)
            tmp[i] = 0;
        for (i = 0; i < s_len; i++)
            tmp[32 - s_len + i] = s[i];
        be_to_limbs(sl, tmp);
    }
    if (l_is_zero(rl) || l_cmp(rl, P256_N) >= 0)
        return 0;
    if (l_is_zero(sl) || l_cmp(sl, P256_N) >= 0)
        return 0;

    /* e = the leftmost 256 bits of the digest, right-aligned when the
     * digest is shorter than the group order. */
    take = digest_len < 32 ? digest_len : 32;
    for (i = 0; i < 32; i++)
        ebuf[i] = 0;
    for (i = 0; i < take; i++)
        ebuf[32 - take + i] = digest[i];
    be_to_limbs(el, ebuf);
    if (l_cmp(el, P256_N) >= 0) {
        uint64_t t[4];
        int64_t borrow = 0;
        int j;
        for (j = 0; j < 4; j++) {
            __int128 xv = (__int128)el[j] - (__int128)P256_N[j] + borrow;
            t[j] = (uint64_t)xv;
            borrow = (int64_t)(xv >> 64);
        }
        l_copy(el, t);
    }

    /* Load and validate the public key. */
    be_to_limbs(qx, pub->x);
    be_to_limbs(qy, pub->y);
    if (l_cmp(qx, P256_P) >= 0 || l_cmp(qy, P256_P) >= 0)
        return 0;
    m256_to(q.x, qx, &fp_ctx);
    m256_to(q.y, qy, &fp_ctx);
    l_copy(q.z, fp_ctx.one);
    if (!on_curve(q.x, q.y))
        return 0;

    /* w = s^-1, u1 = e*w, u2 = r*w, all mod n. */
    m256_to(sm, sl, &fn_ctx);
    m256_exp(w, sm, fn_exp_inv, &fn_ctx);
    m256_to(em, el, &fn_ctx);
    m256_to(rm, rl, &fn_ctx);
    m256_mul(u1, em, w, &fn_ctx);
    m256_mul(u2, rm, w, &fn_ctx);
    m256_from(u1, u1, &fn_ctx);
    m256_from(u2, u2, &fn_ctx);

    jac_mul2_public(&res, u1, u2, &q);
    if (jac_is_inf(&res))
        return 0;
    {
        uint64_t ax[4], ay[4];
        if (jac_affine(ax, ay, &res) < 0)
            return 0;
        m256_from(xr, ax, &fp_ctx);
    }
    if (l_cmp(xr, P256_N) >= 0) {
        uint64_t t[4];
        int64_t borrow = 0;
        int j;
        for (j = 0; j < 4; j++) {
            __int128 xv = (__int128)xr[j] - (__int128)P256_N[j] + borrow;
            t[j] = (uint64_t)xv;
            borrow = (int64_t)(xv >> 64);
        }
        l_copy(xr, t);
    }
    return l_eq(xr, rl) ? 1 : 0;
}
