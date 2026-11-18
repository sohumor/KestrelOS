/* libjs/crypto.c - standalone Web Crypto API and FIPS 180-4 SHA-1/SHA-2.
 *
 * Implements SHA-1, SHA-256, SHA-384, and SHA-512 using circular 16-word
 * schedules so every stack frame remains well under 768 bytes and zero heap
 * memory is needed during hashing. Works identically in freestanding
 * KestrelOS guest builds and host C11 builds without linking libtls.
 */

#define JS_INTERNAL
#if defined(JS_HOST) && defined(_WIN32)
#define _CRT_RAND_S
#endif
#include "js.h"

#include <stdlib.h>
#include <string.h>
#ifdef JS_HOST
#include <stdio.h>
#include <time.h>
#else
#include <kestrel.h>
#endif

enum {
    ALG_SHA1 = 1,
    ALG_SHA256,
    ALG_SHA384,
    ALG_SHA512
};

struct crypto_hash {
    int alg;
    uint64_t nlo;
    uint64_t nhi;
    unsigned long used;
    union {
        uint32_t w[8];
        uint64_t d[8];
    } h;
    uint8_t buf[128];
};

static uint32_t rotl32(uint32_t x, unsigned int n)
{
    return (x << n) | (x >> (32 - n));
}

static uint32_t rotr32(uint32_t x, unsigned int n)
{
    return (x >> n) | (x << (32 - n));
}

static uint64_t rotr64(uint64_t x, unsigned int n)
{
    return (x >> n) | (x << (64 - n));
}

static uint32_t load_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t load_be64(const uint8_t *p)
{
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8) | (uint64_t)p[7];
}

static void store_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void store_be64(uint8_t *p, uint64_t v)
{
    p[0] = (uint8_t)(v >> 56);
    p[1] = (uint8_t)(v >> 48);
    p[2] = (uint8_t)(v >> 40);
    p[3] = (uint8_t)(v >> 32);
    p[4] = (uint8_t)(v >> 24);
    p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >> 8);
    p[7] = (uint8_t)v;
}

static void sha1_block(uint32_t state[5], const uint8_t block[64])
{
    uint32_t w[16];
    uint32_t a, b, c, d, e;
    unsigned int i;

    for (i = 0; i < 16; i++)
        w[i] = load_be32(block + i * 4);
    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];
    for (i = 0; i < 80; i++) {
        uint32_t f, k, tmp;

        if (i >= 16)
            w[i & 15] = rotl32(w[(i - 3) & 15] ^ w[(i - 8) & 15] ^
                               w[(i - 14) & 15] ^ w[(i - 16) & 15], 1);
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5a827999U;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1U;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdcU;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6U;
        }
        tmp = rotl32(a, 5) + f + e + k + w[i & 15];
        e = d;
        d = c;
        c = rotl32(b, 30);
        b = a;
        a = tmp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

static const uint32_t K256[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static void sha256_block(uint32_t state[8], const uint8_t block[64])
{
    uint32_t w[16];
    uint32_t a, b, c, d, e, f, g, h;
    unsigned int i;

    for (i = 0; i < 16; i++)
        w[i] = load_be32(block + i * 4);
    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];
    for (i = 0; i < 64; i++) {
        uint32_t bs1, ch, t1, bs0, maj, t2;

        if (i >= 16) {
            uint32_t w15 = w[(i - 15) & 15];
            uint32_t w2  = w[(i - 2) & 15];
            uint32_t s0  = rotr32(w15, 7) ^ rotr32(w15, 18) ^ (w15 >> 3);
            uint32_t s1  = rotr32(w2, 17) ^ rotr32(w2, 19) ^ (w2 >> 10);
            w[i & 15] = s1 + w[(i - 7) & 15] + s0 + w[(i - 16) & 15];
        }
        bs1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        ch  = (e & f) ^ (~e & g);
        t1  = h + bs1 + ch + K256[i] + w[i & 15];
        bs0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        maj = (a & b) ^ (a & c) ^ (b & c);
        t2  = bs0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL,
    0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL,
    0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL, 0x2de92c6f592b0275ULL,
    0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL,
    0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL,
    0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL,
    0x92722c851482353bULL, 0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL,
    0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL,
    0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL,
    0xc67178f2e372532bULL, 0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL,
    0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

static void sha512_block(uint64_t state[8], const uint8_t block[128])
{
    uint64_t w[16];
    uint64_t a, b, c, d, e, f, g, h;
    unsigned int i;

    for (i = 0; i < 16; i++)
        w[i] = load_be64(block + i * 8);
    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];
    for (i = 0; i < 80; i++) {
        uint64_t bs1, ch, t1, bs0, maj, t2;

        if (i >= 16) {
            uint64_t w15 = w[(i - 15) & 15];
            uint64_t w2  = w[(i - 2) & 15];
            uint64_t s0  = rotr64(w15, 1) ^ rotr64(w15, 8) ^ (w15 >> 7);
            uint64_t s1  = rotr64(w2, 19) ^ rotr64(w2, 61) ^ (w2 >> 6);
            w[i & 15] = s1 + w[(i - 7) & 15] + s0 + w[(i - 16) & 15];
        }
        bs1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
        ch  = (e & f) ^ (~e & g);
        t1  = h + bs1 + ch + K512[i] + w[i & 15];
        bs0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
        maj = (a & b) ^ (a & c) ^ (b & c);
        t2  = bs0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static unsigned long alg_block_len(int alg)
{
    return (alg == ALG_SHA1 || alg == ALG_SHA256) ? 64UL : 128UL;
}

static unsigned long alg_digest_len(int alg)
{
    switch (alg) {
    case ALG_SHA1:   return 20UL;
    case ALG_SHA256: return 32UL;
    case ALG_SHA384: return 48UL;
    case ALG_SHA512: return 64UL;
    default:         return 0UL;
    }
}

static void hash_compress(struct crypto_hash *c, const uint8_t *block)
{
    if (c->alg == ALG_SHA1)
        sha1_block(c->h.w, block);
    else if (c->alg == ALG_SHA256)
        sha256_block(c->h.w, block);
    else
        sha512_block(c->h.d, block);
}

static void hash_init(struct crypto_hash *c, int alg)
{
    memset(c, 0, sizeof(*c));
    c->alg = alg;
    if (alg == ALG_SHA1) {
        c->h.w[0] = 0x67452301U; c->h.w[1] = 0xefcdab89U;
        c->h.w[2] = 0x98badcfeU; c->h.w[3] = 0x10325476U;
        c->h.w[4] = 0xc3d2e1f0U;
    } else if (alg == ALG_SHA256) {
        c->h.w[0] = 0x6a09e667U; c->h.w[1] = 0xbb67ae85U;
        c->h.w[2] = 0x3c6ef372U; c->h.w[3] = 0xa54ff53aU;
        c->h.w[4] = 0x510e527fU; c->h.w[5] = 0x9b05688cU;
        c->h.w[6] = 0x1f83d9abU; c->h.w[7] = 0x5be0cd19U;
    } else if (alg == ALG_SHA384) {
        c->h.d[0] = 0xcbbb9d5dc1059ed8ULL; c->h.d[1] = 0x629a292a367cd507ULL;
        c->h.d[2] = 0x9159015a3070dd17ULL; c->h.d[3] = 0x152fecd8f70e5939ULL;
        c->h.d[4] = 0x67332667ffc00b31ULL; c->h.d[5] = 0x8eb44a8768581511ULL;
        c->h.d[6] = 0xdb0c2e0d64f98fa7ULL; c->h.d[7] = 0x47b5481dbefa4fa4ULL;
    } else {
        c->h.d[0] = 0x6a09e667f3bcc908ULL; c->h.d[1] = 0xbb67ae8584caa73bULL;
        c->h.d[2] = 0x3c6ef372fe94f82bULL; c->h.d[3] = 0xa54ff53a5f1d36f1ULL;
        c->h.d[4] = 0x510e527fade682d1ULL; c->h.d[5] = 0x9b05688c2b3e6c1fULL;
        c->h.d[6] = 0x1f83d9abfb41bd6bULL; c->h.d[7] = 0x5be0cd19137e2179ULL;
    }
}

static void hash_update(struct crypto_hash *c, const uint8_t *data,
                        unsigned long len)
{
    unsigned long blk = alg_block_len(c->alg);

    if (c->nlo + (uint64_t)len < c->nlo)
        c->nhi++;
    c->nlo += (uint64_t)len;
    while (len > 0) {
        unsigned long avail = blk - c->used;
        unsigned long take = len < avail ? len : avail;

        memcpy(c->buf + c->used, data, take);
        c->used += take;
        data += take;
        len -= take;
        if (c->used == blk) {
            hash_compress(c, c->buf);
            c->used = 0;
        }
    }
}

static void hash_final(struct crypto_hash *c, uint8_t out[64])
{
    unsigned long blk = alg_block_len(c->alg);
    unsigned long len_bytes = (blk == 64UL) ? 8UL : 16UL;
    uint64_t bits_lo = c->nlo << 3;
    uint64_t bits_hi = (c->nhi << 3) | (c->nlo >> 61);
    unsigned int i;

    c->buf[c->used++] = 0x80u;
    if (c->used > blk - len_bytes) {
        memset(c->buf + c->used, 0, blk - c->used);
        hash_compress(c, c->buf);
        c->used = 0;
    }
    memset(c->buf + c->used, 0, blk - len_bytes - c->used);
    if (len_bytes == 8UL) {
        store_be64(c->buf + 56, bits_lo);
    } else {
        store_be64(c->buf + 112, bits_hi);
        store_be64(c->buf + 120, bits_lo);
    }
    hash_compress(c, c->buf);

    if (c->alg == ALG_SHA1) {
        for (i = 0; i < 5; i++)
            store_be32(out + i * 4, c->h.w[i]);
    } else if (c->alg == ALG_SHA256) {
        for (i = 0; i < 8; i++)
            store_be32(out + i * 4, c->h.w[i]);
    } else if (c->alg == ALG_SHA384) {
        for (i = 0; i < 6; i++)
            store_be64(out + i * 8, c->h.d[i]);
    } else {
        for (i = 0; i < 8; i++)
            store_be64(out + i * 8, c->h.d[i]);
    }
}

static void hash_oneshot(int alg, const uint8_t *data, unsigned long len,
                         uint8_t out[64])
{
    struct crypto_hash c;

    hash_init(&c, alg);
    if (data && len)
        hash_update(&c, data, len);
    hash_final(&c, out);
}

/* ---------------- Entropy / host fallback PRNG ---------------- */

#ifdef JS_HOST
static uint64_t prng_state = 0x9e3779b97f4a7c15ULL;

static uint64_t splitmix64_next(uint64_t *state)
{
    uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static void prng_fill(void *seed_ptr, uint8_t *out, unsigned long len)
{
    unsigned long off = 0;
    prng_state ^= (uint64_t)(uintptr_t)seed_ptr ^ ((uint64_t)clock() << 16) ^
                  (uint64_t)(uintptr_t)out;
    while (off < len) {
        uint64_t v = splitmix64_next(&prng_state);
        unsigned long take = len - off;

        if (take > sizeof(v))
            take = sizeof(v);
        memcpy(out + off, &v, take);
        off += take;
    }
}
#endif

static int crypto_fill_bytes(js_ctx *ctx, uint8_t *out, unsigned long len)
{
    if (!len)
        return 0;
    if (ctx && ctx->cfg.random_bytes)
        return ctx->cfg.random_bytes(ctx->cfg.user, out, len);
#if !defined(JS_HOST)
    {
        unsigned long off = 0;
        while (off < len) {
            long n = getrandom(out + off, len - off, 0);
            if (n <= 0 || (unsigned long)n > len - off)
                return -1;
            off += (unsigned long)n;
        }
        return 0;
    }
#elif defined(_WIN32)
    {
        unsigned long off = 0;
        while (off < len) {
            unsigned int v = 0;
            unsigned long take = len - off;
            if (rand_s(&v) != 0) {
                prng_fill(ctx, out + off, len - off);
                return 0;
            }
            if (take > sizeof(v))
                take = sizeof(v);
            memcpy(out + off, &v, take);
            off += take;
        }
        return 0;
    }
#else
    {
        FILE *f = fopen("/dev/urandom", "rb");
        unsigned long off = 0;

        if (f) {
            while (off < len) {
                size_t got = fread(out + off, 1, len - off, f);
                if (got == 0)
                    break;
                off += (unsigned long)got;
            }
            fclose(f);
        }
        if (off < len)
            prng_fill(ctx, out + off, len - off);
        return 0;
    }
#endif
}

/* ---------------- JS Web Crypto bindings ---------------- */

static int is_integer_typedarray(int cls)
{
    return cls >= JS_CLASS_INT8ARRAY && cls <= JS_CLASS_UINT32ARRAY;
}

static int crypto_get_random_values(js_ctx *ctx, js_value this_val,
                                    int argc, js_value *argv, js_value *out)
{
    js_object *ta;
    uint8_t *buf;
    unsigned long len;

    (void)this_val;
    if (argc < 1 || !js_is_typedarray(argv[0]) ||
        !is_integer_typedarray(argv[0].u.obj->cls))
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "getRandomValues requires an integer TypedArray");
    ta = argv[0].u.obj;
    len = ta->byte_length;
    if (len > 65536UL)
        return js_throw_error(ctx, JS_ERR_RANGE,
                              "getRandomValues quota exceeded");
    if (!ta->view_buffer || !ta->view_buffer->buffer)
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "invalid TypedArray storage");
    buf = ta->view_buffer->buffer + ta->byte_offset;
    if (len && crypto_fill_bytes(ctx, buf, len) != 0)
        return js_throw_error(ctx, JS_ERR_ERROR,
                              "secure random source is unavailable");
    *out = argv[0];
    return JS_OK;
}

static int crypto_random_uuid(js_ctx *ctx, js_value this_val,
                              int argc, js_value *argv, js_value *out)
{
    static const char hex[] = "0123456789abcdef";
    uint8_t bytes[16];
    char uuid[37];
    unsigned int i, w = 0;

    (void)this_val; (void)argc; (void)argv;
    if (crypto_fill_bytes(ctx, bytes, sizeof(bytes)) != 0)
        return js_throw_error(ctx, JS_ERR_ERROR,
                              "secure random source is unavailable");
    bytes[6] = (uint8_t)((bytes[6] & 0x0f) | 0x40);
    bytes[8] = (uint8_t)((bytes[8] & 0x3f) | 0x80);
    for (i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            uuid[w++] = '-';
        uuid[w++] = hex[bytes[i] >> 4];
        uuid[w++] = hex[bytes[i] & 15];
    }
    uuid[w] = 0;
    *out = js_mkstring(ctx, uuid, w);
    return js_fatal(ctx) ? JS_THROW : JS_OK;
}

static int ascii_fold_eq(const char *s, unsigned long len, const char *lit)
{
    unsigned long i, n = strlen(lit);

    if (len != n)
        return 0;
    for (i = 0; i < len; i++) {
        int a = (unsigned char)s[i];
        int b = (unsigned char)lit[i];
        if (a >= 'A' && a <= 'Z')
            a += 'a' - 'A';
        if (a != b)
            return 0;
    }
    return 1;
}

static int crypto_make_rejected(js_ctx *ctx, js_value reason, js_value *out)
{
    js_value p = js_promise_new(ctx);

    if (!js_is_promise(p))
        return JS_THROW;
    if (js_promise_reject(ctx, p, reason) != JS_OK)
        return JS_THROW;
    *out = p;
    return JS_OK;
}

static int crypto_reject_named(js_ctx *ctx, int errkind, const char *name,
                               const char *msg, js_value *out)
{
    js_object *e = js_new_error(ctx, errkind, msg);

    if (!e)
        return JS_THROW;
    if (name && js_define(ctx, e, "name", js_mkcstring(ctx, name)) != JS_OK)
        return JS_THROW;
    return crypto_make_rejected(ctx, js_object_value(e), out);
}

static int crypto_reject_pending(js_ctx *ctx, js_value *out)
{
    js_value exc;

    if (js_fatal(ctx))
        return JS_THROW;
    exc = js_exception(ctx);
    js_clear_exception(ctx);
    return crypto_make_rejected(ctx, exc, out);
}

static int crypto_parse_alg(js_ctx *ctx, js_value alg_val, int *alg_out)
{
    js_value name_val = alg_val, str_val;
    const char *name;
    unsigned long len;

    if (js_is_object(alg_val)) {
        if (js_get(ctx, alg_val, "name", &name_val) != JS_OK)
            return JS_THROW;
    }
    if (js_to_string(ctx, name_val, &str_val) != JS_OK)
        return JS_THROW;
    name = js_string_bytes(str_val, &len);
    if (ascii_fold_eq(name, len, "sha-1") || ascii_fold_eq(name, len, "sha1"))
        *alg_out = ALG_SHA1;
    else if (ascii_fold_eq(name, len, "sha-256") || ascii_fold_eq(name, len, "sha256"))
        *alg_out = ALG_SHA256;
    else if (ascii_fold_eq(name, len, "sha-384") || ascii_fold_eq(name, len, "sha384"))
        *alg_out = ALG_SHA384;
    else if (ascii_fold_eq(name, len, "sha-512") || ascii_fold_eq(name, len, "sha512"))
        *alg_out = ALG_SHA512;
    else
        *alg_out = -1;
    return JS_OK;
}

static int crypto_subtle_digest(js_ctx *ctx, js_value this_val,
                                int argc, js_value *argv, js_value *out)
{
    int alg = -1;
    const uint8_t *data;
    unsigned long len = 0, dlen;
    uint8_t digest[64];
    js_value ab, p;

    (void)this_val;
    if (argc < 2)
        return crypto_reject_named(ctx, JS_ERR_TYPE, 0,
                                   "subtle.digest requires algorithm and data",
                                   out);
    if (crypto_parse_alg(ctx, argv[0], &alg) != JS_OK)
        return crypto_reject_pending(ctx, out);
    if (alg < 0)
        return crypto_reject_named(ctx, JS_ERR_ERROR, "NotSupportedError",
                                   "unsupported digest algorithm", out);
    data = (const uint8_t *)js_buffersource_data(argv[1], &len);
    if (!data)
        return crypto_reject_named(ctx, JS_ERR_TYPE, 0,
                                   "digest data must be an ArrayBuffer, TypedArray, or DataView",
                                   out);
    dlen = alg_digest_len(alg);
    hash_oneshot(alg, data, len, digest);
    ab = js_arraybuffer_new(ctx, digest, dlen);
    if (!js_is_arraybuffer(ab))
        return JS_THROW;
    p = js_promise_new(ctx);
    if (!js_is_promise(p))
        return JS_THROW;
    if (js_promise_resolve(ctx, p, ab) != JS_OK)
        return JS_THROW;
    *out = p;
    return JS_OK;
}

int js_init_crypto(js_ctx *ctx)
{
    js_object *crypto_obj = js_new_object(ctx);
    js_object *subtle_obj = js_new_object(ctx);

    if (!crypto_obj || !subtle_obj)
        return 0;
    if (ctx->sym_tostringtag.type == JS_SYMBOL) {
        if (js_define_symbol_prop(ctx, crypto_obj, ctx->sym_tostringtag,
                                  js_mkcstring(ctx, "Crypto"),
                                  JS_P_CONFIG) != JS_OK ||
            js_define_symbol_prop(ctx, subtle_obj, ctx->sym_tostringtag,
                                  js_mkcstring(ctx, "SubtleCrypto"),
                                  JS_P_CONFIG) != JS_OK)
            return 0;
    }
    if (js_define_native(ctx, crypto_obj, "getRandomValues",
                         crypto_get_random_values, 1) != JS_OK ||
        js_define_native(ctx, crypto_obj, "randomUUID",
                         crypto_random_uuid, 0) != JS_OK ||
        js_define_native(ctx, subtle_obj, "digest",
                         crypto_subtle_digest, 2) != JS_OK ||
        js_define(ctx, crypto_obj, "subtle",
                  js_object_value(subtle_obj)) != JS_OK ||
        js_define(ctx, ctx->global, "crypto",
                  js_object_value(crypto_obj)) != JS_OK)
        return 0;
    return 1;
}
