/* Host regression test for KestrelOS libtls asymmetric crypto:
 * bignum.c (RSA-grade fixed-capacity big integers) and ecc.c
 * (X25519 + NIST P-256).  Everything is pure computation, so the real
 * objects link straight into a host program.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -g -Ilibtls \
 *      tools/test_kcrypto.c libtls/bignum.c libtls/ecc.c \
 *      -o build/host-tests/test_kcrypto
 *
 * Oracles: host __int128 arithmetic for small operands, algebraic
 * identities (q*b + r == a, Fermat little theorem, textbook RSA), and
 * RFC 7748 X25519 / generated P-256 ECDSA known-answer vectors.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bignum.h"
#include "ecc.h"

/* ------------------------- bignum helpers ------------------------------ */

static void expect_u64(const struct bn *a, uint64_t v)
{
    if (v == 0) {
        assert(bn_is_zero(a));
        return;
    }
    assert(a->n == 1);
    assert(a->v[0] == v);
    assert(bn_bits(a) == 64 - __builtin_clzll(v));
}

static void bn_set_bytes(struct bn *a, uint8_t hi, int nbytes)
{
    /* a = hi repeated nbytes big-endian (mostly for boundary values). */
    static uint8_t buf[BN_MAX_BYTES];
    memset(buf, hi, nbytes);
    assert(bn_from_bytes(a, buf, nbytes) == 0);
}

static void test_bn_basics(void)
{
    struct bn a, b;

    bn_zero(&a);
    assert(bn_is_zero(&a) && a.n == 0 && bn_bits(&a) == 0 && bn_bytes(&a) == 0);
    assert(!bn_is_odd(&a));
    bn_set_u64(&a, 0);                  /* u64 zero == bn_zero */
    assert(bn_is_zero(&a));

    bn_set_u64(&a, 0xdeadbeef);
    expect_u64(&a, 0xdeadbeef);
    assert(bn_bits(&a) == 32);
    assert(bn_bytes(&a) == 4);
    assert(bn_is_odd(&a));              /* ...eef is odd */
    assert(bn_get_bit(&a, 0) == 1 && bn_get_bit(&a, 31) == 1);
    bn_set_u64(&b, 0xdeadbee0);
    assert(!bn_is_odd(&b));
    bn_set_u64(&b, 0xdeadbeef);
    assert(bn_cmp(&a, &b) == 0);
    bn_set_u64(&b, 0xdeadbee0);
    assert(bn_cmp(&a, &b) > 0);
    assert(bn_cmp(&b, &a) < 0);

    /* A two-limb number compares above every one-limb number. */
    bn_set_u64(&a, 1);
    assert(bn_shl(&b, &a, 64) == 0);    /* b = 2^64 -> 2 limbs */
    assert(b.n == 2 && b.v[1] == 1 && b.v[0] == 0);
    assert(bn_bits(&b) == 65);
    bn_set_u64(&a, 0xffffffffffffffff);
    assert(bn_cmp(&b, &a) > 0);

    /* Big-endian import/export, leading zeros, exact padding. */
    {
        static const uint8_t enc[] = {0, 0, 0, 0x12, 0x34, 0xab, 0xcd};
        uint8_t out[8];
        assert(bn_from_bytes(&a, enc, sizeof(enc)) == 0);
        expect_u64(&a, 0x1234abcd);
        assert(bn_to_bytes(&a, out, 8) == 0);
        assert(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0 &&
               out[4] == 0x12 && out[5] == 0x34 && out[6] == 0xab &&
               out[7] == 0xcd);
        /* Does not fit in fewer bytes. */
        assert(bn_to_bytes(&a, out, 3) == -1);
        assert(bn_to_bytes(&a, out, 4) == 0);
    }
    /* More than BN_MAX_BYTES nonzero bytes is too large. */
    {
        static uint8_t big[BN_MAX_BYTES + 1];
        memset(big, 0, sizeof(big));
        big[0] = 1;                     /* 2^4096 */
        assert(bn_from_bytes(&a, big, sizeof(big)) == -1);
        big[0] = 0;                     /* ...but all-zero is fine */
        assert(bn_from_bytes(&a, big, sizeof(big)) == 0);
        assert(bn_is_zero(&a));
    }
}

static void test_bn_arith(void)
{
    struct bn a, b, r, q, m;

    /* add/sub against host arithmetic. */
    bn_set_u64(&a, 0xffffffffffffffff);
    bn_set_u64(&b, 0xffffffffffffffff);
    assert(bn_add(&r, &a, &b) == 0);    /* 2^65 - 2 */
    assert(r.n == 2 && r.v[0] == 0xfffffffffffffffe && r.v[1] == 1);
    assert(bn_sub(&r, &a, &b) == 0 && bn_is_zero(&r));
    bn_set_u64(&b, 1);
    assert(bn_sub(&r, &b, &a) == -1);   /* underflow refused */

    /* 2^4096-1 + 1 = 2^4096 fits the product-width result. */
    bn_set_bytes(&a, 0xff, BN_MAX_BYTES);
    assert(a.n == 64 && bn_bits(&a) == 4096);
    bn_set_u64(&b, 1);
    assert(bn_add(&r, &a, &b) == 0);
    assert(bn_bits(&r) == 4097 && r.n == 65 && r.v[64] == 1 &&
           r.v[0] == 0 && r.v[63] == 0);

    /* mul: (2^64-1)^2 checked with host __int128. */
    {
        __uint128_t x = 0xffffffffffffffffULL;
        __uint128_t want = x * x;       /* 0xfffffffffffffffe_0000000000000001 */
        bn_set_u64(&a, (uint64_t)x);
        assert(bn_mul(&r, &a, &a) == 0);
        assert(r.v[0] == (uint64_t)want && r.v[1] == (uint64_t)(want >> 64));
        assert(r.n == 2);
        bn_set_u64(&b, 0);
        assert(bn_mul(&r, &a, &b) == 0 && bn_is_zero(&r));
        bn_set_u64(&b, 1);
        assert(bn_mul(&r, &a, &b) == 0 && bn_cmp(&r, &a) == 0);
    }

    /* shl/shr round trips, including limb-crossing shifts. */
    bn_set_u64(&a, 0x8000000000000000);
    assert(bn_shl(&r, &a, 1) == 0);     /* 2^64 */
    assert(r.n == 2 && r.v[1] == 1);
    assert(bn_shr(&r, &r, 1) == 0 && bn_cmp(&r, &a) == 0);
    assert(bn_shr(&r, &a, 63) == 0);    /* 2^63 >> 63 = 1 */
    expect_u64(&r, 1);
    assert(bn_shr(&r, &a, 64) == 0 && bn_is_zero(&r));
    bn_set_u64(&a, 0x0123456789abcdef);
    assert(bn_shl(&r, &a, 100) == 0);
    assert(bn_bits(&r) == 157);
    assert(bn_shr(&r, &r, 100) == 0 && bn_cmp(&r, &a) == 0);

    /* divmod: identity (a/b)*b + a%b == a over multi-limb operands,
     * remainder strictly below the divisor, div-by-zero refused. */
    bn_set_bytes(&a, 0xa5, 40);         /* a = 0xa5a5... (320 bits) */
    bn_set_u64(&b, 0x0102030405060708);
    assert(bn_divmod(&q, &m, &a, &b) == 0);
    assert(bn_cmp(&m, &b) < 0);         /* r < b */
    assert(bn_mul(&r, &q, &b) == 0);
    assert(bn_add(&r, &r, &m) == 0);
    assert(bn_cmp(&r, &a) == 0);
    /* q or r may be NULL. */
    assert(bn_divmod(NULL, &m, &a, &b) == 0);
    assert(bn_divmod(&q, NULL, &a, &b) == 0);
    assert(bn_divmod(NULL, NULL, &a, &b) == 0);
    bn_zero(&b);
    assert(bn_divmod(&q, &m, &a, &b) == -1);
    /* a < b -> q = 0, r = a. */
    bn_set_u64(&a, 7);
    bn_set_u64(&b, 1000);
    assert(bn_divmod(&q, &m, &a, &b) == 0);
    assert(bn_is_zero(&q) && bn_cmp(&m, &a) == 0);
    /* bn_mod agrees with divmod's remainder. */
    bn_set_bytes(&a, 0x5a, 20);
    bn_set_u64(&b, 0x0123456789abcdef);
    assert(bn_mod(&m, &a, &b) == 0);
    assert(bn_divmod(&q, &r, &a, &b) == 0 && bn_cmp(&m, &r) == 0);
    bn_zero(&b);
    assert(bn_mod(&m, &a, &b) == -1);
}

static void test_bn_mont_modexp(void)
{
    struct bn base, ex, n, r, one;
    struct bn_mont mt;

    /* Montgomery requires an odd modulus. */
    bn_set_u64(&n, 0x100);              /* even */
    assert(bn_mont_init(&mt, &n) == -1);
    bn_set_u64(&n, 3233);               /* RSA toy modulus 61*53 */
    assert(bn_mont_init(&mt, &n) == 0 && mt.k == 1);

    /* to_mont/from_mont round trip; mont_mul == mul+mod. */
    bn_set_u64(&one, 1);
    {
        struct bn a, am, bm, prod, want, t;
        bn_set_u64(&a, 1000);
        bn_set_u64(&t, 2000);
        assert(bn_to_mont(&am, &a, &mt) == 0);
        assert(bn_to_mont(&bm, &t, &mt) == 0);
        bn_from_mont(&r, &am, &mt);
        assert(bn_cmp(&r, &a) == 0);
        bn_mont_mul(&prod, &am, &bm, &mt);
        bn_from_mont(&prod, &prod, &mt);        /* 2000000 mod 3233 */
        assert(bn_mul(&want, &a, &t) == 0);
        assert(bn_mod(&want, &want, &n) == 0);
        assert(bn_cmp(&prod, &want) == 0);
        /* r may alias an operand. */
        bn_mont_mul(&am, &am, &bm, &mt);
        bn_from_mont(&r, &am, &mt);
        assert(bn_cmp(&r, &want) == 0);
    }

    /* Textbook RSA: 65^17 mod 3233 = 2790, inverse 2790^2753 = 65. */
    bn_set_u64(&base, 65);
    bn_set_u64(&ex, 17);
    assert(bn_modexp(&r, &base, &ex, &mt) == 0);
    expect_u64(&r, 2790);
    bn_set_u64(&base, 2790);
    bn_set_u64(&ex, 2753);
    assert(bn_modexp(&r, &base, &ex, &mt) == 0);
    expect_u64(&r, 65);
    /* x^0 = 1, 0^x = 0. */
    bn_set_u64(&ex, 0);
    assert(bn_modexp(&r, &base, &ex, &mt) == 0);
    expect_u64(&r, 1);
    bn_set_u64(&base, 0);
    bn_set_u64(&ex, 7);
    assert(bn_modexp(&r, &base, &ex, &mt) == 0);
    assert(bn_is_zero(&r));

    /* Fermat: a^(p-1) == 1 mod p for the Mersenne prime 2^61-1. */
    bn_set_u64(&n, 0x1fffffffffffffff); /* 2^61 - 1, prime */
    assert(bn_mont_init(&mt, &n) == 0);
    bn_set_u64(&base, 123456789);
    bn_set_u64(&ex, 0x1ffffffffffffffe);/* p - 1 */
    assert(bn_modexp(&r, &base, &ex, &mt) == 0);
    expect_u64(&r, 1);
}

/* ------------------------- X25519 (RFC 7748) --------------------------- */

static void unhex(uint8_t *out, const char *s)
{
    for (int i = 0; s[2*i]; i++) {
        unsigned v;
        sscanf(s + 2*i, "%02x", &v);
        out[i] = (uint8_t)v;
    }
}

static void test_x25519(void)
{
    uint8_t sc[32], pt[32], out[32], want[32];

    /* RFC 7748 5.2 test vectors (little-endian fields). */
    unhex(sc, "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4");
    unhex(pt, "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c");
    unhex(want, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
    assert(x25519(out, sc, pt) == 0);
    assert(memcmp(out, want, 32) == 0);

    unhex(sc, "4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d");
    unhex(pt, "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493");
    unhex(want, "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957");
    assert(x25519(out, sc, pt) == 0);
    assert(memcmp(out, want, 32) == 0);

    /* Basepoint: k * 9, scalar clamped internally. */
    unhex(sc, "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    unhex(want, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    assert(x25519_base(out, sc) == 0);
    assert(memcmp(out, want, 32) == 0);

    /* Small-order points must be rejected (all-zero shared secret). */
    memset(pt, 0, 32);
    assert(x25519(out, sc, pt) == -1);
    pt[0] = 1;                          /* u = 1 is also low order */
    assert(x25519(out, sc, pt) == -1);
}

/* ------------------------- P-256 ---------------------------------------- */

static void test_p256(void)
{
    uint8_t enc[65], out[32];
    struct p256_point pub, pub2, peer;

    /* Generator point, uncompressed form. */
    enc[0] = 0x04;
    unhex(enc + 1,
          "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296");
    unhex(enc + 33,
          "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5");
    assert(p256_point_from_bytes(&pub, enc, 65) == 0);
    {
        uint8_t rt[65];
        p256_point_to_bytes(&pub, rt);
        assert(memcmp(rt, enc, 65) == 0);
    }
    /* Compressed encodings of the generator also parse. Gy is odd, so
     * 0x03 recovers it exactly and 0x02 recovers the even root p - y. */
    enc[0] = 0x02;
    assert(p256_point_from_bytes(&peer, enc, 33) == 0);
    assert(memcmp(peer.x, pub.x, 32) == 0 && memcmp(peer.y, pub.y, 32) != 0);
    enc[0] = 0x03;
    assert(p256_point_from_bytes(&peer, enc, 33) == 0);
    assert(memcmp(peer.x, pub.x, 32) == 0 && memcmp(peer.y, pub.y, 32) == 0);
    /* A point not on the curve is refused. */
    enc[0] = 0x04;
    enc[33] ^= 0xff;
    assert(p256_point_from_bytes(&peer, enc, 65) == -1);
    /* Wrong prefix / truncated input refused. */
    enc[33] ^= 0xff; enc[0] = 0x07;
    assert(p256_point_from_bytes(&peer, enc, 65) == -1);
    assert(p256_point_from_bytes(&peer, enc, 20) == -1);

    /* ECDH: dA*G and dB*G derive the same shared secret both ways. */
    {
        uint8_t da[32], db[32], sa[32], sb[32];
        memset(da, 0, 32); da[31] = 7;
        memset(db, 0, 32); db[31] = 13;
        assert(p256_base_mul(&pub, da) == 0);
        assert(p256_base_mul(&pub2, db) == 0);
        assert(p256_ecdh(sa, da, &pub2) == 0);
        assert(p256_ecdh(sb, db, &pub) == 0);
        assert(memcmp(sa, sb, 32) == 0);
        /* d = 0 and d = n are out of range. */
        memset(db, 0, 32);
        assert(p256_base_mul(&pub, db) == -1);
        assert(p256_ecdh(sa, db, &pub2) == -1);
    }

    /* ECDSA over a generated key/signature pair (oracle: python EC).
     * d = 0x1234...cdef, msg = sha256("kestrel ecdsa test"). */
    {
        static const uint8_t pk_x[32] = {
            0x47,0x1c,0x3e,0x75,0x8c,0x49,0x04,0x28,
            0x5b,0xba,0x7e,0x53,0x11,0x8e,0xd0,0xf5,
            0x24,0xad,0xeb,0x07,0x57,0xd2,0x5b,0xd2,
            0xf8,0xe7,0xb0,0xd7,0x6d,0xfa,0x71,0x4c };
        static const uint8_t pk_y[32] = {
            0xdd,0x52,0x0f,0x7a,0xca,0x8a,0x8b,0x91,
            0x7a,0xcc,0x37,0xf5,0x1d,0xe8,0xf0,0xc9,
            0xbb,0xe3,0xad,0x85,0x83,0x82,0xe7,0x02,
            0xdc,0x25,0xa1,0x2d,0x09,0xf7,0xa8,0x58 };
        static const uint8_t z[32] = {  /* sha256("kestrel ecdsa test") */
            0x33,0x75,0xd2,0x94,0x4e,0xc0,0xd2,0x48,
            0x40,0x2e,0xb4,0x07,0x3a,0x45,0xbc,0xa2,
            0xca,0x95,0x3b,0x5c,0xbb,0xb6,0xde,0xe0,
            0x34,0x11,0x45,0xab,0x41,0xf1,0x1b,0xe8 };
        static const uint8_t sig_r[32] = {
            0x08,0xbc,0xb5,0xae,0x0e,0xc1,0x33,0x3f,
            0xe8,0x27,0xcb,0x4c,0x4f,0x6b,0x20,0xdf,
            0x28,0x09,0x44,0xa7,0x16,0xa3,0x3d,0xad,
            0xc3,0x09,0x24,0x09,0xf9,0xd8,0x7e,0x52 };
        static const uint8_t sig_s[32] = {
            0xbf,0x51,0x35,0x61,0x5b,0xea,0x43,0xe0,
            0xce,0x28,0x06,0xbb,0xd8,0x23,0xa0,0x74,
            0xc2,0x90,0xe1,0x49,0x6d,0xc2,0x60,0x56,
            0x24,0xe6,0x3b,0x15,0xf2,0xda,0x48,0xd3 };
        uint8_t bad[32];

        memcpy(pub.x, pk_x, 32);
        memcpy(pub.y, pk_y, 32);
        assert(p256_ecdsa_verify(&pub, z, 32, sig_r, 32, sig_s, 32) == 1);
        /* A signature for a different message must not verify. */
        memcpy(bad, z, 32); bad[0] ^= 1;
        assert(p256_ecdsa_verify(&pub, bad, 32, sig_r, 32, sig_s, 32) == 0);
        /* Tampered s and r rejected. */
        memcpy(bad, sig_s, 32); bad[31] ^= 1;
        assert(p256_ecdsa_verify(&pub, z, 32, sig_r, 32, bad, 32) == 0);
        memcpy(bad, sig_r, 32); bad[0] ^= 1;
        assert(p256_ecdsa_verify(&pub, z, 32, bad, 32, sig_s, 32) == 0);
        /* r = 0 or s = 0 is not a valid signature. */
        memset(bad, 0, 32);
        assert(p256_ecdsa_verify(&pub, z, 32, bad, 32, sig_s, 32) == 0);
        assert(p256_ecdsa_verify(&pub, z, 32, sig_r, 32, bad, 32) == 0);
        /* Longer digest input truncates to 256 bits per FIPS 186-4. */
        {
            uint8_t z64[64];
            memcpy(z64, z, 32); memset(z64 + 32, 0xee, 32);
            assert(p256_ecdsa_verify(&pub, z64, 64, sig_r, 32, sig_s, 32) == 1);
        }
    }
    (void)out;
}

int main(void)
{
    test_bn_basics();
    test_bn_arith();
    test_bn_mont_modexp();
    test_x25519();
    test_p256();
    puts("kcrypto: bignum construct/import/arith/divmod/Montgomery/"
         "modexp(RSA+Fermat), X25519 RFC7748 KAT + low-order reject, "
         "P-256 point parse/ECDH symmetry/ECDSA verify+tamper PASS");
    return 0;
}
