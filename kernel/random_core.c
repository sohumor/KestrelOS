#include "random_core.h"

static uint32_t rotr32(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32 - n));
}

static uint32_t load_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void store_be32(uint8_t *p, uint32_t x)
{
    p[0] = (uint8_t)(x >> 24);
    p[1] = (uint8_t)(x >> 16);
    p[2] = (uint8_t)(x >> 8);
    p[3] = (uint8_t)x;
}

static void store_le32(uint8_t *p, uint32_t x)
{
    p[0] = (uint8_t)x;
    p[1] = (uint8_t)(x >> 8);
    p[2] = (uint8_t)(x >> 16);
    p[3] = (uint8_t)(x >> 24);
}

static void bytes_copy(uint8_t *dst, const uint8_t *src, size_t n)
{
    while (n--)
        *dst++ = *src++;
}

static void bytes_zero(uint8_t *dst, size_t n)
{
    while (n--)
        *dst++ = 0;
}

static const uint32_t sha_k[64] = {
    0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5,
    0x3956C25B, 0x59F111F1, 0x923F82A4, 0xAB1C5ED5,
    0xD807AA98, 0x12835B01, 0x243185BE, 0x550C7DC3,
    0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174,
    0xE49B69C1, 0xEFBE4786, 0x0FC19DC6, 0x240CA1CC,
    0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA,
    0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7,
    0xC6E00BF3, 0xD5A79147, 0x06CA6351, 0x14292967,
    0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13,
    0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85,
    0xA2BFE8A1, 0xA81A664B, 0xC24B8B70, 0xC76C51A3,
    0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070,
    0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5,
    0x391C0CB3, 0x4ED8AA4A, 0x5B9CCA4F, 0x682E6FF3,
    0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208,
    0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2,
};

static void sha256_compress(struct random_sha256 *s, const uint8_t block[64])
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;

    for (int i = 0; i < 16; i++)
        w[i] = load_be32(block + i * 4);
    for (int i = 16; i < 64; i++) {
        uint32_t x = w[i - 15];
        uint32_t y = w[i - 2];
        uint32_t s0 = rotr32(x, 7) ^ rotr32(x, 18) ^ (x >> 3);
        uint32_t s1 = rotr32(y, 17) ^ rotr32(y, 19) ^ (y >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + sha_k[i] + w[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

void random_sha256_init(struct random_sha256 *s)
{
    static const uint32_t initial[8] = {
        0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
        0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19,
    };

    for (int i = 0; i < 8; i++)
        s->h[i] = initial[i];
    s->bytes = 0;
    s->used = 0;
    bytes_zero(s->block, sizeof(s->block));
}

void random_sha256_update(struct random_sha256 *s, const void *data,
                          size_t len)
{
    const uint8_t *p = data;

    s->bytes += len;
    while (len) {
        size_t take = 64 - s->used;
        if (take > len)
            take = len;
        bytes_copy(s->block + s->used, p, take);
        s->used += (uint32_t)take;
        p += take;
        len -= take;
        if (s->used == 64) {
            sha256_compress(s, s->block);
            s->used = 0;
        }
    }
}

void random_sha256_final(struct random_sha256 *s, uint8_t out[32])
{
    uint64_t bits = s->bytes * 8;

    s->block[s->used++] = 0x80;
    if (s->used > 56) {
        while (s->used < 64)
            s->block[s->used++] = 0;
        sha256_compress(s, s->block);
        s->used = 0;
    }
    while (s->used < 56)
        s->block[s->used++] = 0;
    for (int i = 7; i >= 0; i--)
        s->block[s->used++] = (uint8_t)(bits >> (i * 8));
    sha256_compress(s, s->block);
    for (int i = 0; i < 8; i++)
        store_be32(out + i * 4, s->h[i]);
    bytes_zero((uint8_t *)s, sizeof(*s));
}

static uint32_t rotl32(uint32_t x, unsigned n)
{
    return (x << n) | (x >> (32 - n));
}

#define QR(a, b, c, d) do { \
    a += b; d ^= a; d = rotl32(d, 16); \
    c += d; b ^= c; b = rotl32(b, 12); \
    a += b; d ^= a; d = rotl32(d, 8);  \
    c += d; b ^= c; b = rotl32(b, 7);  \
} while (0)

void random_chacha20_block(uint8_t out[64], const uint8_t key[32],
                           uint32_t counter, const uint8_t nonce[12])
{
    uint32_t x[16];
    uint32_t initial[16];

    initial[0] = 0x61707865;
    initial[1] = 0x3320646E;
    initial[2] = 0x79622D32;
    initial[3] = 0x6B206574;
    for (int i = 0; i < 8; i++)
        initial[4 + i] = load_le32(key + i * 4);
    initial[12] = counter;
    initial[13] = load_le32(nonce);
    initial[14] = load_le32(nonce + 4);
    initial[15] = load_le32(nonce + 8);
    for (int i = 0; i < 16; i++)
        x[i] = initial[i];

    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8],  x[12]);
        QR(x[1], x[5], x[9],  x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8],  x[13]);
        QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; i++)
        store_le32(out + i * 4, x[i] + initial[i]);
    bytes_zero((uint8_t *)x, sizeof(x));
    bytes_zero((uint8_t *)initial, sizeof(initial));
}

#undef QR
