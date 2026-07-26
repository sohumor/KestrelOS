/* KestrelOS libtls: SHA-256/384/512, HMAC, HKDF, TLS 1.3 key schedule.
 *
 * FIPS 180-4 defines two compression functions. SHA-256 works on 32-bit
 * words, absorbs 64-byte blocks and runs 64 rounds; SHA-512 works on
 * 64-bit words, absorbs 128-byte blocks and runs 80 rounds with wider
 * rotations. SHA-384 is SHA-512 with a different initial state, truncated
 * to 48 bytes. Both are implemented below and dispatched on ctx->alg.
 *
 * The constants were derived rather than transcribed: the SHA-256 round
 * constants are the first 32 fractional bits of the cube roots of the
 * first 64 primes, the SHA-512 ones the first 64 fractional bits of the
 * cube roots of the first 80 primes; the SHA-256/512 initial states are
 * the fractional bits of the square roots of the first 8 primes, and
 * SHA-384's of the ninth through sixteenth.
 *
 * No dynamic allocation and no recursion anywhere in this file: every
 * buffer is a fixed-size automatic, and the largest is the 514-byte
 * HkdfLabel encoding in hkdf_expand_label().
 */

#include <hash.h>
#include <string.h>

/* ---------------- SHA-256 core ---------------- */

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

static uint32_t rotr32(uint32_t x, unsigned int n)
{
    return (x >> n) | (x << (32 - n));
}

static void sha256_block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, hh;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);

        w[i] = s1 + w[i - 7] + s0 + w[i - 16];
    }

    a = h[0]; b = h[1]; c = h[2]; d = h[3];
    e = h[4]; f = h[5]; g = h[6]; hh = h[7];

    for (i = 0; i < 64; i++) {
        uint32_t bs1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + bs1 + ch + K256[i] + w[i];
        uint32_t bs0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = bs0 + maj;

        hh = g; g = f; f = e;
        e = d + t1;
        d = c; c = b; b = a;
        a = t1 + t2;
    }

    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

/* ---------------- SHA-512 core ---------------- */

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

static uint64_t rotr64(uint64_t x, unsigned int n)
{
    return (x >> n) | (x << (64 - n));
}

static void sha512_block(uint64_t h[8], const uint8_t *p)
{
    uint64_t w[80];
    uint64_t a, b, c, d, e, f, g, hh;
    int i;

    for (i = 0; i < 16; i++) {
        int j = i * 8;

        w[i] = ((uint64_t)p[j] << 56) | ((uint64_t)p[j + 1] << 48) |
               ((uint64_t)p[j + 2] << 40) | ((uint64_t)p[j + 3] << 32) |
               ((uint64_t)p[j + 4] << 24) | ((uint64_t)p[j + 5] << 16) |
               ((uint64_t)p[j + 6] << 8) | (uint64_t)p[j + 7];
    }
    for (i = 16; i < 80; i++) {
        uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);

        w[i] = s1 + w[i - 7] + s0 + w[i - 16];
    }

    a = h[0]; b = h[1]; c = h[2]; d = h[3];
    e = h[4]; f = h[5]; g = h[6]; hh = h[7];

    for (i = 0; i < 80; i++) {
        uint64_t bs1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
        uint64_t ch = (e & f) ^ (~e & g);
        uint64_t t1 = hh + bs1 + ch + K512[i] + w[i];
        uint64_t bs0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
        uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint64_t t2 = bs0 + maj;

        hh = g; g = f; f = e;
        e = d + t1;
        d = c; c = b; b = a;
        a = t1 + t2;
    }

    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

/* ---------------- generic hash interface ---------------- */

unsigned long hash_digest_len(int alg)
{
    switch (alg) {
    case HASH_SHA256: return 32;
    case HASH_SHA384: return 48;
    case HASH_SHA512: return 64;
    default:          return 0;
    }
}

unsigned long hash_block_len(int alg)
{
    switch (alg) {
    case HASH_SHA256: return 64;
    case HASH_SHA384: return 128;
    case HASH_SHA512: return 128;
    default:          return 0;
    }
}

const char *hash_name(int alg)
{
    switch (alg) {
    case HASH_SHA256: return "SHA-256";
    case HASH_SHA384: return "SHA-384";
    case HASH_SHA512: return "SHA-512";
    default:          return "?";
    }
}

int hash_init(struct hash_ctx *c, int alg)
{
    if (!hash_digest_len(alg))
        return -1;

    c->alg = alg;
    c->nlo = 0;
    c->nhi = 0;
    c->buflen = 0;
    memset(c->buf, 0, sizeof(c->buf));

    if (alg == HASH_SHA256) {
        c->h.w[0] = 0x6a09e667U; c->h.w[1] = 0xbb67ae85U;
        c->h.w[2] = 0x3c6ef372U; c->h.w[3] = 0xa54ff53aU;
        c->h.w[4] = 0x510e527fU; c->h.w[5] = 0x9b05688cU;
        c->h.w[6] = 0x1f83d9abU; c->h.w[7] = 0x5be0cd19U;
    } else if (alg == HASH_SHA384) {
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
    return 0;
}

static void hash_compress(struct hash_ctx *c, const uint8_t *p)
{
    if (c->alg == HASH_SHA256)
        sha256_block(c->h.w, p);
    else
        sha512_block(c->h.d, p);
}

void hash_update(struct hash_ctx *c, const void *data, unsigned long len)
{
    const uint8_t *p = (const uint8_t *)data;
    unsigned long blk = hash_block_len(c->alg);

    /* 128-bit byte count: SHA-512 encodes the length in 128 bits, and even
     * though nothing here will ever hash 2^64 bytes, carrying the high half
     * keeps the padding exactly as specified. */
    if (c->nlo + len < c->nlo)
        c->nhi++;
    c->nlo += len;

    if (c->buflen) {
        unsigned long need = blk - c->buflen;

        if (len < need) {
            memcpy(c->buf + c->buflen, p, len);
            c->buflen += len;
            return;
        }
        memcpy(c->buf + c->buflen, p, need);
        hash_compress(c, c->buf);
        c->buflen = 0;
        p += need;
        len -= need;
    }

    while (len >= blk) {
        hash_compress(c, p);
        p += blk;
        len -= blk;
    }

    if (len) {
        memcpy(c->buf, p, len);
        c->buflen = len;
    }
}

void hash_final(struct hash_ctx *c, uint8_t *out)
{
    unsigned long blk = hash_block_len(c->alg);
    unsigned long lenfield = (c->alg == HASH_SHA256) ? 8 : 16;
    uint8_t pad[HASH_MAX_BLOCK * 2];
    unsigned long padlen, rem, i;
    uint64_t nlo = c->nlo, nhi = c->nhi;

    /* Append 0x80, then zeros, so the length field lands flush against the
     * end of the final block. */
    rem = c->buflen;
    padlen = (rem < blk - lenfield) ? (blk - lenfield - rem)
                                    : (2 * blk - lenfield - rem);
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;

    /* The bit count, big-endian, in the last lenfield bytes. */
    if (lenfield == 16) {
        uint64_t bhi = (nhi << 3) | (nlo >> 61);

        for (i = 0; i < 8; i++)
            pad[padlen + i] = (uint8_t)(bhi >> (56 - 8 * i));
        for (i = 0; i < 8; i++)
            pad[padlen + 8 + i] = (uint8_t)((nlo << 3) >> (56 - 8 * i));
    } else {
        for (i = 0; i < 8; i++)
            pad[padlen + i] = (uint8_t)((nlo << 3) >> (56 - 8 * i));
    }

    /* hash_update() will bump the byte count again, but the count that
     * matters was captured above, so that is harmless. */
    hash_update(c, pad, padlen + lenfield);

    if (c->alg == HASH_SHA256) {
        for (i = 0; i < 8; i++) {
            out[i * 4]     = (uint8_t)(c->h.w[i] >> 24);
            out[i * 4 + 1] = (uint8_t)(c->h.w[i] >> 16);
            out[i * 4 + 2] = (uint8_t)(c->h.w[i] >> 8);
            out[i * 4 + 3] = (uint8_t)(c->h.w[i]);
        }
    } else {
        uint8_t full[64];
        unsigned long dlen = hash_digest_len(c->alg);

        for (i = 0; i < 8; i++) {
            int j;

            for (j = 0; j < 8; j++)
                full[i * 8 + j] = (uint8_t)(c->h.d[i] >> (56 - 8 * j));
        }
        /* SHA-384 is SHA-512 truncated to its leftmost 48 bytes. */
        memcpy(out, full, dlen);
    }
}

void hash_copy(struct hash_ctx *dst, const struct hash_ctx *src)
{
    memcpy(dst, src, sizeof(*dst));
}

void hash_peek(const struct hash_ctx *c, uint8_t *out)
{
    struct hash_ctx tmp;

    hash_copy(&tmp, c);
    hash_final(&tmp, out);
}

int hash_oneshot(int alg, const void *data, unsigned long len, uint8_t *out)
{
    struct hash_ctx c;

    if (hash_init(&c, alg) < 0)
        return -1;
    hash_update(&c, data, len);
    hash_final(&c, out);
    return 0;
}

/* ---------------- HMAC (RFC 2104) ---------------- */

int hmac_init(struct hmac_ctx *c, int alg, const uint8_t *key,
              unsigned long keylen)
{
    unsigned long blk = hash_block_len(alg);
    unsigned long dlen = hash_digest_len(alg);
    uint8_t k[HASH_MAX_BLOCK];
    uint8_t pad[HASH_MAX_BLOCK];
    unsigned long i;

    if (!dlen)
        return -1;

    /* A key longer than the block is replaced by its own digest; a shorter
     * one is zero-padded up to the block. */
    memset(k, 0, sizeof(k));
    if (keylen > blk) {
        if (hash_oneshot(alg, key, keylen, k) < 0)
            return -1;
    } else if (keylen) {
        memcpy(k, key, keylen);
    }

    c->alg = alg;

    for (i = 0; i < blk; i++)
        pad[i] = (uint8_t)(k[i] ^ 0x36);
    hash_init(&c->inner, alg);
    hash_update(&c->inner, pad, blk);

    for (i = 0; i < blk; i++)
        pad[i] = (uint8_t)(k[i] ^ 0x5c);
    hash_init(&c->outer, alg);
    hash_update(&c->outer, pad, blk);

    /* Do not leave the padded key on the stack for the next frame. */
    memset(k, 0, sizeof(k));
    memset(pad, 0, sizeof(pad));
    return 0;
}

void hmac_update(struct hmac_ctx *c, const void *data, unsigned long len)
{
    hash_update(&c->inner, data, len);
}

void hmac_final(struct hmac_ctx *c, uint8_t *out)
{
    uint8_t d[HASH_MAX_DIGEST];

    hash_final(&c->inner, d);
    hash_update(&c->outer, d, hash_digest_len(c->alg));
    hash_final(&c->outer, out);
    memset(d, 0, sizeof(d));
}

int hmac(int alg, const uint8_t *key, unsigned long keylen,
         const void *data, unsigned long len, uint8_t *out)
{
    struct hmac_ctx c;

    if (hmac_init(&c, alg, key, keylen) < 0)
        return -1;
    hmac_update(&c, data, len);
    hmac_final(&c, out);
    memset(&c, 0, sizeof(c));
    return 0;
}

/* ---------------- HKDF (RFC 5869) ---------------- */

int hkdf_extract(int alg, const uint8_t *salt, unsigned long saltlen,
                 const uint8_t *ikm, unsigned long ikmlen, uint8_t *prk)
{
    uint8_t zeros[HASH_MAX_DIGEST];
    unsigned long dlen = hash_digest_len(alg);

    if (!dlen)
        return -1;

    /* "if not provided, it is set to a string of HashLen zeros" */
    if (!salt || !saltlen) {
        memset(zeros, 0, dlen);
        salt = zeros;
        saltlen = dlen;
    }
    /* PRK = HMAC-Hash(salt, IKM): the salt is the key, the IKM the message. */
    return hmac(alg, salt, saltlen, ikm, ikmlen, prk);
}

int hkdf_expand(int alg, const uint8_t *prk, unsigned long prklen,
                const uint8_t *info, unsigned long infolen,
                uint8_t *out, unsigned long outlen)
{
    unsigned long dlen = hash_digest_len(alg);
    uint8_t t[HASH_MAX_DIGEST];
    unsigned long done = 0;
    unsigned int counter = 1;

    if (!dlen || prklen < dlen)
        return -1;
    /* L <= 255 * HashLen, since the counter octet is what bounds it. */
    if (outlen > 255 * dlen)
        return -1;
    if (!outlen)
        return 0;

    while (done < outlen) {
        struct hmac_ctx c;
        uint8_t ctr = (uint8_t)counter;
        unsigned long take;

        if (hmac_init(&c, alg, prk, prklen) < 0)
            return -1;
        /* T(1) = HMAC(PRK, info || 0x01); T(n) = HMAC(PRK, T(n-1) || info || n) */
        if (counter > 1)
            hmac_update(&c, t, dlen);
        if (info && infolen)
            hmac_update(&c, info, infolen);
        hmac_update(&c, &ctr, 1);
        hmac_final(&c, t);
        memset(&c, 0, sizeof(c));

        take = outlen - done;
        if (take > dlen)
            take = dlen;
        memcpy(out + done, t, take);
        done += take;
        counter++;
    }

    memset(t, 0, sizeof(t));
    return 0;
}

/* ---------------- TLS 1.3 key schedule (RFC 8446 7.1) ---------------- */

/*   struct {
 *       uint16 length = Length;
 *       opaque label<7..255> = "tls13 " + Label;
 *       opaque context<0..255> = Context;
 *   } HkdfLabel;
 */
int hkdf_expand_label(int alg, const uint8_t *secret,
                      const char *label,
                      const uint8_t *context, unsigned long contextlen,
                      uint8_t *out, unsigned long outlen)
{
    static const char prefix[6] = { 't', 'l', 's', '1', '3', ' ' };
    uint8_t info[2 + 1 + 255 + 1 + 255];
    unsigned long dlen = hash_digest_len(alg);
    unsigned long llen, n = 0, i;

    if (!dlen || !label)
        return -1;
    if (outlen > 0xffff || !outlen)
        return -1;

    llen = strlen(label);
    if (llen > TLS_LABEL_MAX || contextlen > TLS_CONTEXT_MAX)
        return -1;

    info[n++] = (uint8_t)(outlen >> 8);
    info[n++] = (uint8_t)outlen;
    info[n++] = (uint8_t)(sizeof(prefix) + llen);
    for (i = 0; i < sizeof(prefix); i++)
        info[n++] = (uint8_t)prefix[i];
    for (i = 0; i < llen; i++)
        info[n++] = (uint8_t)label[i];
    info[n++] = (uint8_t)contextlen;
    if (context && contextlen) {
        memcpy(info + n, context, contextlen);
        n += contextlen;
    }

    return hkdf_expand(alg, secret, dlen, info, n, out, outlen);
}

int hkdf_derive_secret_hash(int alg, const uint8_t *secret, const char *label,
                            const uint8_t *thash, uint8_t *out)
{
    unsigned long dlen = hash_digest_len(alg);

    if (!dlen)
        return -1;
    return hkdf_expand_label(alg, secret, label, thash, dlen, out, dlen);
}

int hkdf_derive_secret(int alg, const uint8_t *secret, const char *label,
                       const void *msgs, unsigned long msglen, uint8_t *out)
{
    uint8_t thash[HASH_MAX_DIGEST];

    if (hash_oneshot(alg, msgs, msglen, thash) < 0)
        return -1;
    return hkdf_derive_secret_hash(alg, secret, label, thash, out);
}
