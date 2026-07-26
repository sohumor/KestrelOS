/* KestrelOS libc: SHA-256, implemented from FIPS 180-4.
 *
 * The message is absorbed 64 bytes at a time. Each block is expanded to a
 * 64-word schedule, then run through 64 rounds of the compression
 * function; the working variables are added back into the state. The tail
 * is padded with 0x80, zero bytes up to a 56-byte offset, and the message
 * length in bits as a big-endian 64-bit value.
 *
 * All constants below were derived, not copied: the initial state is the
 * first 32 fractional bits of the square roots of primes 2..19, and the
 * round constants the first 32 fractional bits of the cube roots of the
 * first 64 primes.
 */

#include <sha256.h>
#include <string.h>

static const uint32_t K[64] = {
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

static uint32_t rotr(uint32_t x, unsigned int n)
{
    return (x >> n) | (x << (32 - n));
}

static uint32_t ch(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) ^ (~x & z);
}

static uint32_t maj(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) ^ (x & z) ^ (y & z);
}

/* The two "big sigma" functions applied to the working variables. */
static uint32_t bsig0(uint32_t x)
{
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}

static uint32_t bsig1(uint32_t x)
{
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}

/* The two "small sigma" functions used to extend the message schedule. */
static uint32_t ssig0(uint32_t x)
{
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}

static uint32_t ssig1(uint32_t x)
{
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

/* Absorb exactly one 64-byte block into ctx->h. */
static void sha256_block(struct sha256_ctx *ctx, const uint8_t *p)
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (i = 16; i < 64; i++)
        w[i] = ssig1(w[i - 2]) + w[i - 7] + ssig0(w[i - 15]) + w[i - 16];

    a = ctx->h[0]; b = ctx->h[1]; c = ctx->h[2]; d = ctx->h[3];
    e = ctx->h[4]; f = ctx->h[5]; g = ctx->h[6]; h = ctx->h[7];

    for (i = 0; i < 64; i++) {
        uint32_t t1 = h + bsig1(e) + ch(e, f, g) + K[i] + w[i];
        uint32_t t2 = bsig0(a) + maj(a, b, c);

        h = g; g = f; f = e;
        e = d + t1;
        d = c; c = b; b = a;
        a = t1 + t2;
    }

    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

void sha256_init(struct sha256_ctx *ctx)
{
    ctx->h[0] = 0x6a09e667U;
    ctx->h[1] = 0xbb67ae85U;
    ctx->h[2] = 0x3c6ef372U;
    ctx->h[3] = 0xa54ff53aU;
    ctx->h[4] = 0x510e527fU;
    ctx->h[5] = 0x9b05688cU;
    ctx->h[6] = 0x1f83d9abU;
    ctx->h[7] = 0x5be0cd19U;
    ctx->nbits = 0;
    ctx->buflen = 0;
    memset(ctx->buf, 0, sizeof(ctx->buf));
}

void sha256_update(struct sha256_ctx *ctx, const void *data, unsigned long len)
{
    const uint8_t *p = (const uint8_t *)data;

    ctx->nbits += (uint64_t)len * 8;

    /* Top up a partial block first. */
    if (ctx->buflen) {
        unsigned long need = SHA256_BLOCK_LEN - ctx->buflen;

        if (len < need) {
            memcpy(ctx->buf + ctx->buflen, p, len);
            ctx->buflen += (uint32_t)len;
            return;
        }
        memcpy(ctx->buf + ctx->buflen, p, need);
        sha256_block(ctx, ctx->buf);
        ctx->buflen = 0;
        p += need;
        len -= need;
    }

    while (len >= SHA256_BLOCK_LEN) {
        sha256_block(ctx, p);
        p += SHA256_BLOCK_LEN;
        len -= SHA256_BLOCK_LEN;
    }

    if (len) {
        memcpy(ctx->buf, p, len);
        ctx->buflen = (uint32_t)len;
    }
}

void sha256_final(struct sha256_ctx *ctx, uint8_t out[SHA256_DIGEST_LEN])
{
    uint64_t nbits = ctx->nbits;
    uint8_t pad[SHA256_BLOCK_LEN * 2];
    unsigned long padlen;
    unsigned long rem;
    int i;

    /* 0x80, then zeros so that the length lands in the last 8 bytes. */
    rem = ctx->buflen % SHA256_BLOCK_LEN;
    padlen = (rem < 56) ? (56 - rem) : (120 - rem);
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    for (i = 0; i < 8; i++)
        pad[padlen + i] = (uint8_t)(nbits >> (56 - 8 * i));
    /* sha256_update() would add these bytes to the bit count, but the
     * count is already captured above, so the damage is harmless. */
    sha256_update(ctx, pad, padlen + 8);

    for (i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(ctx->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(ctx->h[i]);
    }
}

static void hexify(const uint8_t *d, unsigned long n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    unsigned long i;

    for (i = 0; i < n; i++) {
        out[i * 2]     = digits[(d[i] >> 4) & 0xf];
        out[i * 2 + 1] = digits[d[i] & 0xf];
    }
    out[n * 2] = '\0';
}

void sha256_hex(const void *data, unsigned long len, char out[SHA256_HEX_LEN + 1])
{
    struct sha256_ctx ctx;
    uint8_t d[SHA256_DIGEST_LEN];

    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, d);
    hexify(d, SHA256_DIGEST_LEN, out);
}

void sha256_password(const char *salt, const char *pw, unsigned long iters,
                     char out[SHA256_HEX_LEN + 1])
{
    struct sha256_ctx ctx;
    uint8_t d[SHA256_DIGEST_LEN];
    unsigned long saltlen = strlen(salt);
    unsigned long i;

    if (iters < 1)
        iters = 1;

    sha256_init(&ctx);
    sha256_update(&ctx, salt, saltlen);
    sha256_update(&ctx, pw, strlen(pw));
    sha256_final(&ctx, d);

    for (i = 1; i < iters; i++) {
        sha256_init(&ctx);
        sha256_update(&ctx, salt, saltlen);
        sha256_update(&ctx, d, sizeof(d));
        sha256_final(&ctx, d);
    }

    hexify(d, SHA256_DIGEST_LEN, out);
}
