/* KestrelOS libtls: fixed-capacity big integers.
 *
 * Schoolbook multiplication, Knuth algorithm D for division, and CIOS
 * Montgomery multiplication, all written from the published descriptions
 * of the algorithms rather than from anyone's code.
 *
 * The one environmental constraint that shapes the code: userspace links
 * no libgcc, so an expression like `(__uint128_t)x / y` is a link error
 * (__udivti3). 64x64->128 *multiplication* is fine -- it is one mulq --
 * so __int128 is used freely for products and never for quotients. The
 * 128/64 divide that algorithm D needs to estimate a quotient digit is
 * div2by1() below, a 64-step restoring division.
 */

#include "bignum.h"

/* ---------------- helpers ---------------- */

static void bn_norm(struct bn *a)
{
    int i = BN_LIMBS;
    while (i > 0 && a->v[i - 1] == 0)
        i--;
    a->n = i;
}

static void limbs_clear(uint64_t *v, int n)
{
    int i;
    for (i = 0; i < n; i++)
        v[i] = 0;
}

void bn_zero(struct bn *a)
{
    limbs_clear(a->v, BN_LIMBS);
    a->n = 0;
}

void bn_set_u64(struct bn *a, uint64_t x)
{
    bn_zero(a);
    a->v[0] = x;
    a->n = x ? 1 : 0;
}

void bn_copy(struct bn *dst, const struct bn *src)
{
    int i;
    for (i = 0; i < BN_LIMBS; i++)
        dst->v[i] = src->v[i];
    dst->n = src->n;
}

int bn_is_zero(const struct bn *a)
{
    return a->n == 0;
}

int bn_is_odd(const struct bn *a)
{
    return a->n > 0 && (a->v[0] & 1);
}

int bn_bits(const struct bn *a)
{
    uint64_t t;
    int b;

    if (a->n == 0)
        return 0;
    t = a->v[a->n - 1];
    b = (a->n - 1) * 64;
    while (t) {
        b++;
        t >>= 1;
    }
    return b;
}

int bn_bytes(const struct bn *a)
{
    int b = bn_bits(a);
    return (b + 7) / 8;
}

int bn_get_bit(const struct bn *a, int i)
{
    if (i < 0 || i >= BN_LIMBS * 64)
        return 0;
    return (int)((a->v[i / 64] >> (i % 64)) & 1);
}

int bn_cmp(const struct bn *a, const struct bn *b)
{
    int i;

    if (a->n != b->n)
        return a->n < b->n ? -1 : 1;
    for (i = a->n - 1; i >= 0; i--) {
        if (a->v[i] != b->v[i])
            return a->v[i] < b->v[i] ? -1 : 1;
    }
    return 0;
}

/* ---------------- import / export ---------------- */

int bn_from_bytes(struct bn *a, const uint8_t *b, size_t len)
{
    size_t i;

    bn_zero(a);
    while (len > 0 && *b == 0) {
        b++;
        len--;
    }
    if (len > (size_t)BN_MAX_LIMBS * 8)
        return -1;
    /* Walk from the least significant byte upwards. */
    for (i = 0; i < len; i++) {
        uint64_t byte = b[len - 1 - i];
        a->v[i / 8] |= byte << ((i % 8) * 8);
    }
    bn_norm(a);
    return 0;
}

int bn_to_bytes(const struct bn *a, uint8_t *out, size_t len)
{
    size_t i;

    if ((size_t)bn_bytes(a) > len)
        return -1;
    for (i = 0; i < len; i++) {
        size_t k = len - 1 - i;          /* byte index from the bottom */
        if (k / 8 < (size_t)BN_LIMBS)
            out[i] = (uint8_t)(a->v[k / 8] >> ((k % 8) * 8));
        else
            out[i] = 0;
    }
    return 0;
}

/* ---------------- add / sub ---------------- */

int bn_add(struct bn *r, const struct bn *a, const struct bn *b)
{
    int i, n = a->n > b->n ? a->n : b->n;
    uint64_t carry = 0;

    if (n > BN_LIMBS - 1)
        return -1;
    for (i = 0; i < n; i++) {
        __uint128_t s = (__uint128_t)a->v[i] + b->v[i] + carry;
        r->v[i] = (uint64_t)s;
        carry = (uint64_t)(s >> 64);
    }
    r->v[n] = carry;
    for (i = n + 1; i < BN_LIMBS; i++)
        r->v[i] = 0;
    bn_norm(r);
    return 0;
}

int bn_sub(struct bn *r, const struct bn *a, const struct bn *b)
{
    int i;
    int64_t borrow = 0;

    if (bn_cmp(a, b) < 0)
        return -1;
    for (i = 0; i < BN_LIMBS; i++) {
        __int128 t = (__int128)a->v[i] - (__int128)b->v[i] + borrow;
        r->v[i] = (uint64_t)t;
        borrow = (int64_t)(t >> 64);     /* 0 or -1 */
    }
    bn_norm(r);
    return 0;
}

/* ---------------- multiply ---------------- */

int bn_mul(struct bn *r, const struct bn *a, const struct bn *b)
{
    uint64_t t[BN_LIMBS];
    int i, j;

    if (a->n == 0 || b->n == 0) {
        bn_zero(r);
        return 0;
    }
    if (a->n + b->n > BN_LIMBS)
        return -1;
    limbs_clear(t, BN_LIMBS);
    for (i = 0; i < a->n; i++) {
        uint64_t carry = 0;
        for (j = 0; j < b->n; j++) {
            __uint128_t p = (__uint128_t)a->v[i] * b->v[j] + t[i + j] + carry;
            t[i + j] = (uint64_t)p;
            carry = (uint64_t)(p >> 64);
        }
        t[i + b->n] += carry;            /* cannot overflow: room reserved */
    }
    for (i = 0; i < BN_LIMBS; i++)
        r->v[i] = t[i];
    bn_norm(r);
    return 0;
}

/* ---------------- shifts ---------------- */

int bn_shl(struct bn *r, const struct bn *a, int bits)
{
    uint64_t t[BN_LIMBS];
    int words, sh, i;

    if (bits < 0)
        return bn_shr(r, a, -bits);
    words = bits / 64;
    sh = bits % 64;
    if (a->n == 0) {
        bn_zero(r);
        return 0;
    }
    if (a->n + words + (sh ? 1 : 0) > BN_LIMBS)
        return -1;
    limbs_clear(t, BN_LIMBS);
    for (i = a->n - 1; i >= 0; i--) {
        uint64_t lo = a->v[i] << sh;
        uint64_t hi = sh ? (a->v[i] >> (64 - sh)) : 0;
        t[i + words] |= lo;
        if (sh)
            t[i + words + 1] |= hi;
    }
    for (i = 0; i < BN_LIMBS; i++)
        r->v[i] = t[i];
    bn_norm(r);
    return 0;
}

int bn_shr(struct bn *r, const struct bn *a, int bits)
{
    uint64_t t[BN_LIMBS];
    int words, sh, i;

    if (bits < 0)
        return bn_shl(r, a, -bits);
    words = bits / 64;
    sh = bits % 64;
    limbs_clear(t, BN_LIMBS);
    for (i = 0; i + words < a->n; i++) {
        uint64_t lo = a->v[i + words] >> sh;
        uint64_t hi = (sh && i + words + 1 < a->n)
                          ? (a->v[i + words + 1] << (64 - sh)) : 0;
        t[i] = lo | hi;
    }
    for (i = 0; i < BN_LIMBS; i++)
        r->v[i] = t[i];
    bn_norm(r);
    return 0;
}

/* ---------------- division ---------------- */

/* (hi:lo) / d with the divisor normalized (top bit set) and hi < d, so the
 * quotient is guaranteed to fit in 64 bits. Restoring division, 64 steps.
 * Done by hand because dividing an __int128 would call __udivti3. */
static uint64_t div2by1(uint64_t hi, uint64_t lo, uint64_t d, uint64_t *rem)
{
    uint64_t q = 0;
    int i;

    for (i = 0; i < 64; i++) {
        int top = (int)(hi >> 63);
        hi = (hi << 1) | (lo >> 63);
        lo <<= 1;
        q <<= 1;
        if (top || hi >= d) {
            hi -= d;
            q |= 1;
        }
    }
    *rem = hi;
    return q;
}

static int nlz64(uint64_t x)
{
    int n = 0;
    if (x == 0)
        return 64;
    while (!(x & 0x8000000000000000ULL)) {
        x <<= 1;
        n++;
    }
    return n;
}

int bn_divmod(struct bn *q, struct bn *r, const struct bn *a,
              const struct bn *b)
{
    uint64_t u[BN_LIMBS + 1], v[BN_MAX_LIMBS + 1], qq[BN_LIMBS];
    int an = a->n, bn = b->n, s, i, j;
    uint64_t vtop;

    if (bn == 0)
        return -1;
    if (bn > BN_MAX_LIMBS)
        return -1;

    if (bn_cmp(a, b) < 0) {
        if (r)
            bn_copy(r, a);
        if (q)
            bn_zero(q);
        return 0;
    }

    limbs_clear(qq, BN_LIMBS);

    if (bn == 1) {
        /* Short division: one 128/64 step per limb, remainder carried. */
        uint64_t d = b->v[0], rem = 0;
        int sh = nlz64(d);
        uint64_t dn = d << sh;
        for (i = an - 1; i >= 0; i--) {
            /* Normalize the running pair the same way div2by1 expects. */
            uint64_t hi = sh ? ((rem << sh) | (a->v[i] >> (64 - sh)))
                             : rem;
            uint64_t lo = a->v[i] << sh;
            uint64_t rr;
            qq[i] = div2by1(hi, lo, dn, &rr);
            rem = rr >> sh;
        }
        if (q) {
            for (i = 0; i < BN_LIMBS; i++)
                q->v[i] = qq[i];
            bn_norm(q);
        }
        if (r)
            bn_set_u64(r, rem);
        return 0;
    }

    /* D1: normalize so the divisor's top bit is set. */
    s = nlz64(b->v[bn - 1]);
    for (i = 0; i < bn; i++) {
        v[i] = b->v[i] << s;
        if (s && i > 0)
            v[i] |= b->v[i - 1] >> (64 - s);
    }
    for (i = 0; i < an; i++) {
        u[i] = a->v[i] << s;
        if (s && i > 0)
            u[i] |= a->v[i - 1] >> (64 - s);
    }
    u[an] = s ? (a->v[an - 1] >> (64 - s)) : 0;
    vtop = v[bn - 1];

    /* D2..D7: one quotient limb per iteration, from the top down. */
    for (j = an - bn; j >= 0; j--) {
        uint64_t qhat, rhat;
        int rhat_overflow = 0;
        __int128 t;
        int64_t borrow;
        uint64_t carry;

        if (u[j + bn] >= vtop) {
            /* qhat would not fit; the correction loop below fixes it. */
            qhat = ~(uint64_t)0;
            rhat = u[j + bn - 1] + vtop;
            rhat_overflow = (rhat < vtop);
        } else {
            qhat = div2by1(u[j + bn], u[j + bn - 1], vtop, &rhat);
        }
        while (!rhat_overflow &&
               (__uint128_t)qhat * v[bn - 2] >
                   (((__uint128_t)rhat << 64) | u[j + bn - 2])) {
            uint64_t nr;
            qhat--;
            nr = rhat + vtop;
            rhat_overflow = (nr < rhat);
            rhat = nr;
        }

        /* D4: u[j..j+bn] -= qhat * v */
        borrow = 0;
        carry = 0;
        for (i = 0; i < bn; i++) {
            __uint128_t p = (__uint128_t)qhat * v[i] + carry;
            carry = (uint64_t)(p >> 64);
            t = (__int128)u[i + j] - (__int128)(uint64_t)p + borrow;
            u[i + j] = (uint64_t)t;
            borrow = (int64_t)(t >> 64);
        }
        t = (__int128)u[j + bn] - (__int128)carry + borrow;
        u[j + bn] = (uint64_t)t;
        borrow = (int64_t)(t >> 64);

        qq[j] = qhat;
        if (borrow) {
            /* D6: qhat was one too large. Add the divisor back. */
            uint64_t c = 0;
            qq[j]--;
            for (i = 0; i < bn; i++) {
                __uint128_t sum = (__uint128_t)u[i + j] + v[i] + c;
                u[i + j] = (uint64_t)sum;
                c = (uint64_t)(sum >> 64);
            }
            u[j + bn] += c;
        }
    }

    if (q) {
        for (i = 0; i < BN_LIMBS; i++)
            q->v[i] = qq[i];
        bn_norm(q);
    }
    if (r) {
        /* D8: undo the normalizing shift. */
        bn_zero(r);
        for (i = 0; i < bn; i++) {
            uint64_t lo = u[i] >> s;
            uint64_t hi = (s && i + 1 < bn) ? (u[i + 1] << (64 - s)) : 0;
            r->v[i] = lo | hi;
        }
        bn_norm(r);
    }
    return 0;
}

int bn_mod(struct bn *r, const struct bn *a, const struct bn *m)
{
    return bn_divmod(0, r, a, m);
}

/* ---------------- Montgomery ---------------- */

/* -n^-1 mod 2^64 by Newton iteration: x <- x*(2 - n*x) doubles the number
 * of correct low bits each round, and x = 1 is already correct mod 2. */
static uint64_t mont_n0(uint64_t n)
{
    uint64_t x = 1;
    int i;

    for (i = 0; i < 6; i++)
        x *= 2 - n * x;
    return ~x + 1;                       /* -x mod 2^64 */
}

int bn_mont_init(struct bn_mont *mt, const struct bn *m)
{
    struct bn one, rr;
    int i;

    if (m->n == 0 || m->n > BN_MAX_LIMBS)
        return -1;
    if (!(m->v[0] & 1))
        return -1;                       /* Montgomery needs an odd modulus */

    mt->k = m->n;
    mt->n0 = mont_n0(m->v[0]);
    for (i = 0; i < BN_MAX_LIMBS; i++)
        mt->n[i] = (i < m->n) ? m->v[i] : 0;

    /* rr = 2^(128*k) mod n */
    bn_set_u64(&one, 1);
    if (bn_shl(&rr, &one, 128 * mt->k) < 0)
        return -1;
    if (bn_mod(&rr, &rr, m) < 0)
        return -1;
    for (i = 0; i < BN_MAX_LIMBS; i++)
        mt->rr[i] = (i < rr.n) ? rr.v[i] : 0;
    return 0;
}

void bn_mont_mul(struct bn *r, const struct bn *a, const struct bn *b,
                 const struct bn_mont *mt)
{
    uint64_t t[BN_MAX_LIMBS + 2];
    int k = mt->k, i, j;

    limbs_clear(t, k + 2);
    for (i = 0; i < k; i++) {
        uint64_t bi = (i < BN_LIMBS) ? b->v[i] : 0;
        uint64_t carry = 0, m;
        __uint128_t p;

        for (j = 0; j < k; j++) {
            p = (__uint128_t)a->v[j] * bi + t[j] + carry;
            t[j] = (uint64_t)p;
            carry = (uint64_t)(p >> 64);
        }
        p = (__uint128_t)t[k] + carry;
        t[k] = (uint64_t)p;
        t[k + 1] = (uint64_t)(p >> 64);

        m = t[0] * mt->n0;
        p = (__uint128_t)m * mt->n[0] + t[0];
        carry = (uint64_t)(p >> 64);
        for (j = 1; j < k; j++) {
            p = (__uint128_t)m * mt->n[j] + t[j] + carry;
            t[j - 1] = (uint64_t)p;
            carry = (uint64_t)(p >> 64);
        }
        p = (__uint128_t)t[k] + carry;
        t[k - 1] = (uint64_t)p;
        t[k] = t[k + 1] + (uint64_t)(p >> 64);
    }

    /* t < 2n at this point; one conditional subtraction finishes it. */
    {
        int ge = 0;
        if (t[k]) {
            ge = 1;
        } else {
            for (j = k - 1; j >= 0; j--) {
                if (t[j] != mt->n[j]) {
                    ge = t[j] > mt->n[j];
                    break;
                }
                if (j == 0)
                    ge = 1;              /* exactly equal */
            }
        }
        if (ge) {
            int64_t borrow = 0;
            for (j = 0; j < k; j++) {
                __int128 d = (__int128)t[j] - (__int128)mt->n[j] + borrow;
                t[j] = (uint64_t)d;
                borrow = (int64_t)(d >> 64);
            }
        }
    }

    bn_zero(r);
    for (j = 0; j < k; j++)
        r->v[j] = t[j];
    bn_norm(r);
}

int bn_to_mont(struct bn *r, const struct bn *a, const struct bn_mont *mt)
{
    struct bn rr;
    int i;

    bn_zero(&rr);
    for (i = 0; i < mt->k; i++)
        rr.v[i] = mt->rr[i];
    bn_norm(&rr);
    bn_mont_mul(r, a, &rr, mt);
    return 0;
}

void bn_from_mont(struct bn *r, const struct bn *a, const struct bn_mont *mt)
{
    struct bn one;

    bn_set_u64(&one, 1);
    bn_mont_mul(r, a, &one, mt);
}

/* The sliding-window table. File scope on purpose: eight big integers is
 * 8 KiB and the user stack is only 64 KiB. Single-threaded userspace, so
 * this is safe, but bn_modexp is therefore not reentrant.
 *
 * Four bits, not five: a five-bit window would halve the table's marginal
 * cost on a full-width exponent but double the static footprint, and the
 * exponent this is actually asked to raise things to is 65537, which has
 * two set bits and does not care. */
#define BN_WIN_MAX 4
static struct bn bn_win[1 << (BN_WIN_MAX - 1)];

int bn_modexp(struct bn *r, const struct bn *base, const struct bn *exp,
              const struct bn_mont *mt)
{
    struct bn m, bm, acc, sq;
    int ebits, w, i, tsize;

    bn_zero(&m);
    for (i = 0; i < mt->k; i++)
        m.v[i] = mt->n[i];
    bn_norm(&m);

    ebits = bn_bits(exp);
    if (ebits == 0) {
        /* x^0 = 1 mod n (and 1 mod 1 is 0, which bn_mod gives us). */
        bn_set_u64(r, 1);
        return bn_mod(r, r, &m);
    }

    if (bn_mod(&bm, base, &m) < 0)
        return -1;
    if (bn_to_mont(&bm, &bm, mt) < 0)
        return -1;

    w = ebits <= 24 ? 2 : ebits <= 96 ? 3 : 4;
    tsize = 1 << (w - 1);

    /* win[i] = base^(2i+1) in Montgomery form. */
    bn_copy(&bn_win[0], &bm);
    bn_mont_mul(&sq, &bm, &bm, mt);
    for (i = 1; i < tsize; i++)
        bn_mont_mul(&bn_win[i], &bn_win[i - 1], &sq, mt);

    /* acc = R mod n, the Montgomery representation of 1. */
    bn_set_u64(&acc, 1);
    if (bn_to_mont(&acc, &acc, mt) < 0)
        return -1;

    i = ebits - 1;
    while (i >= 0) {
        if (!bn_get_bit(exp, i)) {
            bn_mont_mul(&acc, &acc, &acc, mt);
            i--;
            continue;
        }
        /* Longest run of at most w bits ending in a 1. */
        {
            int lo = i - w + 1, j, val = 0, len;
            if (lo < 0)
                lo = 0;
            while (!bn_get_bit(exp, lo))
                lo++;
            len = i - lo + 1;
            for (j = i; j >= lo; j--)
                val = (val << 1) | bn_get_bit(exp, j);
            for (j = 0; j < len; j++)
                bn_mont_mul(&acc, &acc, &acc, mt);
            bn_mont_mul(&acc, &acc, &bn_win[val >> 1], mt);
            i = lo - 1;
        }
    }

    bn_from_mont(r, &acc, mt);
    return 0;
}
