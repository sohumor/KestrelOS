/* KestrelOS: host test harness for libtls/{bignum,rsa,ecc,x509}.
 *
 * These four files are pure computation with no I/O and no syscalls, so
 * they can be compiled for the build host and hammered without booting
 * anything. That is what this program does.
 *
 *   gcc -Wall -Wextra -O2 -fsanitize=address,undefined -Ilibtls \
 *       -o build/test_cryptoasym tools/test_cryptoasym.c \
 *       libtls/bignum.c libtls/rsa.c libtls/ecc.c libtls/x509.c
 *   ./build/test_cryptoasym [fixture-dir]
 *
 * On startup it writes two generator scripts into the fixture directory
 * and runs them: a shell script that drives `openssl` to build a real
 * certificate hierarchy (root, intermediate, leaf with SANs, expired,
 * wrong-hostname, non-CA intermediate, path-length violation, weak key,
 * PSS, EC) plus raw RSA and ECDSA signature vectors, and a Python script
 * that emits arbitrary-precision reference answers for the bignum
 * operations and for X25519 and P-256 scalar multiplication. Nothing is
 * checked against libtls itself; every oracle is external.
 *
 * The digests are implemented here rather than pulled from libtls/hash.c
 * so that this harness stays independent of another agent's file, and so
 * that a bug in the hashes cannot cancel out a bug in the verifier.
 */

#define _GNU_SOURCE     /* memmem, strtok_r */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>

#include "bignum.h"
#include "rsa.h"
#include "ecc.h"
#include "x509.h"

/* ================================================================== */
/* Test bookkeeping                                                   */
/* ================================================================== */

static int g_pass, g_fail;
static char g_section[128];

static void section(const char *name)
{
    snprintf(g_section, sizeof(g_section), "%s", name);
    printf("\n--- %s\n", name);
}

static void report(int ok, const char *fmt, ...)
{
    va_list ap;

    if (ok) {
        g_pass++;
        return;
    }
    g_fail++;
    printf("  FAIL [%s] ", g_section);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

#define CHECK(cond, ...) report((cond) ? 1 : 0, __VA_ARGS__)

/* ================================================================== */
/* SHA-1 and SHA-2, from FIPS 180-4                                   */
/* ================================================================== */

static uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static uint64_t rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

static void sha1_compress(uint32_t h[5], const uint8_t b[64])
{
    uint32_t w[80], a, bb, c, d, e;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)b[4 * i] << 24) | ((uint32_t)b[4 * i + 1] << 16) |
               ((uint32_t)b[4 * i + 2] << 8) | b[4 * i + 3];
    for (i = 16; i < 80; i++)
        w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = h[0]; bb = h[1]; c = h[2]; d = h[3]; e = h[4];
    for (i = 0; i < 80; i++) {
        uint32_t f, k, t;
        if (i < 20)      { f = (bb & c) | (~bb & d);          k = 0x5a827999; }
        else if (i < 40) { f = bb ^ c ^ d;                    k = 0x6ed9eba1; }
        else if (i < 60) { f = (bb & c) | (bb & d) | (c & d); k = 0x8f1bbcdc; }
        else             { f = bb ^ c ^ d;                    k = 0xca62c1d6; }
        t = rotl32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rotl32(bb, 30); bb = a; a = t;
    }
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e;
}

static void sha1(const void *data, size_t len, uint8_t out[20])
{
    uint32_t h[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476,
                      0xc3d2e1f0 };
    const uint8_t *p = (const uint8_t *)data;
    uint8_t tail[128];
    size_t n = len, i;
    uint64_t bits = (uint64_t)len * 8;
    size_t tl;

    while (n >= 64) { sha1_compress(h, p); p += 64; n -= 64; }
    memcpy(tail, p, n);
    tail[n] = 0x80;
    tl = n + 1;
    while ((tl % 64) != 56) tail[tl++] = 0;
    for (i = 0; i < 8; i++) tail[tl++] = (uint8_t)(bits >> (56 - 8 * i));
    for (i = 0; i < tl; i += 64) sha1_compress(h, tail + i);
    for (i = 0; i < 5; i++) {
        out[4 * i]     = (uint8_t)(h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8);
        out[4 * i + 3] = (uint8_t)h[i];
    }
}

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void sha256_compress(uint32_t h[8], const uint8_t b[64])
{
    uint32_t w[64], v[8];
    int i;

    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)b[4 * i] << 24) | ((uint32_t)b[4 * i + 1] << 16) |
               ((uint32_t)b[4 * i + 2] << 8) | b[4 * i + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^
                      (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^
                      (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (i = 0; i < 8; i++) v[i] = h[i];
    for (i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(v[4], 6) ^ rotr32(v[4], 11) ^ rotr32(v[4], 25);
        uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
        uint32_t t1 = v[7] + S1 + ch + K256[i] + w[i];
        uint32_t S0 = rotr32(v[0], 2) ^ rotr32(v[0], 13) ^ rotr32(v[0], 22);
        uint32_t mj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
        uint32_t t2 = S0 + mj;
        v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
        v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
    }
    for (i = 0; i < 8; i++) h[i] += v[i];
}

static void sha256(const void *data, size_t len, uint8_t out[32])
{
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    const uint8_t *p = (const uint8_t *)data;
    uint8_t tail[128];
    size_t n = len, i, tl;
    uint64_t bits = (uint64_t)len * 8;

    while (n >= 64) { sha256_compress(h, p); p += 64; n -= 64; }
    memcpy(tail, p, n);
    tail[n] = 0x80;
    tl = n + 1;
    while ((tl % 64) != 56) tail[tl++] = 0;
    for (i = 0; i < 8; i++) tail[tl++] = (uint8_t)(bits >> (56 - 8 * i));
    for (i = 0; i < tl; i += 64) sha256_compress(h, tail + i);
    for (i = 0; i < 8; i++) {
        out[4 * i]     = (uint8_t)(h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8);
        out[4 * i + 3] = (uint8_t)h[i];
    }
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

static void sha512_compress(uint64_t h[8], const uint8_t b[128])
{
    uint64_t w[80], v[8];
    int i, j;

    for (i = 0; i < 16; i++) {
        uint64_t x = 0;
        for (j = 0; j < 8; j++)
            x = (x << 8) | b[8 * i + j];
        w[i] = x;
    }
    for (i = 16; i < 80; i++) {
        uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^
                      (w[i - 15] >> 7);
        uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^
                      (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (i = 0; i < 8; i++) v[i] = h[i];
    for (i = 0; i < 80; i++) {
        uint64_t S1 = rotr64(v[4], 14) ^ rotr64(v[4], 18) ^ rotr64(v[4], 41);
        uint64_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
        uint64_t t1 = v[7] + S1 + ch + K512[i] + w[i];
        uint64_t S0 = rotr64(v[0], 28) ^ rotr64(v[0], 34) ^ rotr64(v[0], 39);
        uint64_t mj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
        uint64_t t2 = S0 + mj;
        v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
        v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
    }
    for (i = 0; i < 8; i++) h[i] += v[i];
}

static void sha512_core(const void *data, size_t len, const uint64_t iv[8],
                        uint8_t *out, int outlen)
{
    uint64_t h[8];
    const uint8_t *p = (const uint8_t *)data;
    uint8_t tail[256];
    size_t n = len, i, tl;
    uint64_t bits = (uint64_t)len * 8;

    for (i = 0; i < 8; i++) h[i] = iv[i];
    while (n >= 128) { sha512_compress(h, p); p += 128; n -= 128; }
    memcpy(tail, p, n);
    tail[n] = 0x80;
    tl = n + 1;
    while ((tl % 128) != 112) tail[tl++] = 0;
    for (i = 0; i < 8; i++) tail[tl++] = 0;      /* high 64 bits of the length */
    for (i = 0; i < 8; i++) tail[tl++] = (uint8_t)(bits >> (56 - 8 * i));
    for (i = 0; i < tl; i += 128) sha512_compress(h, tail + i);
    for (i = 0; i < (size_t)outlen; i++)
        out[i] = (uint8_t)(h[i / 8] >> (56 - 8 * (i % 8)));
}

static const uint64_t IV512[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
    0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
};
static const uint64_t IV384[8] = {
    0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL, 0x9159015a3070dd17ULL,
    0x152fecd8f70e5939ULL, 0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL,
    0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL
};

static void sha384(const void *d, size_t n, uint8_t *o)
{
    sha512_core(d, n, IV384, o, 48);
}

static void sha512(const void *d, size_t n, uint8_t *o)
{
    sha512_core(d, n, IV512, o, 64);
}

static const struct crypto_md md_sha1   = { CRYPTO_MD_SHA1,   "sha1",   20,
                                            sha1 };
static const struct crypto_md md_sha256 = { CRYPTO_MD_SHA256, "sha256", 32,
                                            sha256 };
static const struct crypto_md md_sha384 = { CRYPTO_MD_SHA384, "sha384", 48,
                                            sha384 };
static const struct crypto_md md_sha512 = { CRYPTO_MD_SHA512, "sha512", 64,
                                            sha512 };

/* ================================================================== */
/* Small helpers                                                      */
/* ================================================================== */

static char g_dir[512];

static const char *fixpath(const char *name)
{
    static char buf[640];
    snprintf(buf, sizeof(buf), "%s/%s", g_dir, name);
    return buf;
}

static uint8_t *read_file(const char *name, size_t *len)
{
    FILE *f = fopen(fixpath(name), "rb");
    uint8_t *buf;
    long n;

    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (1 << 26)) { fclose(f); return 0; }
    buf = (uint8_t *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return 0; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(buf); return 0;
    }
    fclose(f);
    buf[n] = 0;
    *len = (size_t)n;
    return buf;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* hex string -> bytes; returns the byte count or -1 */
static int unhex(const char *s, uint8_t *out, int cap)
{
    int n = 0;
    size_t len = strlen(s), i;

    if (len % 2)
        return -1;
    for (i = 0; i < len; i += 2) {
        int a = hexval(s[i]), b = hexval(s[i + 1]);
        if (a < 0 || b < 0 || n >= cap)
            return -1;
        out[n++] = (uint8_t)((a << 4) | b);
    }
    return n;
}

static void tohex(const uint8_t *b, size_t n, char *out)
{
    static const char *d = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        out[2 * i] = d[b[i] >> 4];
        out[2 * i + 1] = d[b[i] & 15];
    }
    out[2 * n] = 0;
}

/* Strip leading zero bytes so a bignum compares equal to python's hex. */
static void bn_hex(const struct bn *a, char *out)
{
    uint8_t buf[BN_LIMBS * 8];
    int n = bn_bytes(a);

    if (n == 0) {
        strcpy(out, "0");
        return;
    }
    bn_to_bytes(a, buf, (size_t)n);
    tohex(buf, (size_t)n, out);
    /* python's '%x' has no leading zero */
    if (out[0] == '0' && out[1])
        memmove(out, out + 1, strlen(out));
}

static int str_has(const char *hay, const char *needle)
{
    return hay && strstr(hay, needle) != 0;
}

/* ================================================================== */
/* Fixture generation                                                 */
/* ================================================================== */

static const char *const GEN_SH =
"#!/bin/sh\n"
"set -e\n"
"cd \"$1\"\n"
"selfcnf() {\n"
"  printf '[req]\\ndistinguished_name = dn\\nprompt = no\\nx509_extensions = v3\\n[dn]\\nCN = %s\\nO = KestrelOS Test\\n[v3]\\n' \"$2\" > \"$1\"\n"
"  cat \"$3\" >> \"$1\"\n"
"}\n"
"reqcnf() {\n"
"  printf '[req]\\ndistinguished_name = dn\\nprompt = no\\n[dn]\\nCN = %s\\nO = KestrelOS Test\\n' \"$2\" > \"$1\"\n"
"}\n"
"cat > ca_ext.txt <<'EOF'\n"
"basicConstraints = critical,CA:TRUE\n"
"keyUsage = critical,keyCertSign,cRLSign\n"
"subjectKeyIdentifier = hash\n"
"EOF\n"
"cat > leaf_ext.cnf <<'EOF'\n"
"basicConstraints = critical,CA:FALSE\n"
"keyUsage = critical,digitalSignature,keyEncipherment\n"
"extendedKeyUsage = serverAuth\n"
"subjectAltName = DNS:example.test,DNS:*.wild.test,IP:10.1.2.3\n"
"EOF\n"
"openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out root.key 2>/dev/null\n"
"selfcnf root.cnf 'Kestrel Test Root RSA' ca_ext.txt\n"
"openssl req -x509 -new -key root.key -config root.cnf -days 3650 -sha256 -out root.pem\n"
"openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out int.key 2>/dev/null\n"
"reqcnf int.cnf 'Kestrel Test Intermediate'\n"
"openssl req -new -key int.key -config int.cnf -out int.csr\n"
"cat > int_ext.cnf <<'EOF'\n"
"basicConstraints = critical,CA:TRUE,pathlen:0\n"
"keyUsage = critical,keyCertSign,cRLSign\n"
"EOF\n"
"openssl x509 -req -in int.csr -CA root.pem -CAkey root.key -CAcreateserial -days 3650 -sha256 -extfile int_ext.cnf -out int.pem 2>/dev/null\n"
"openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out leaf.key 2>/dev/null\n"
"reqcnf leaf.cnf 'example.test'\n"
"openssl req -new -key leaf.key -config leaf.cnf -out leaf.csr\n"
"openssl x509 -req -in leaf.csr -CA int.pem -CAkey int.key -CAcreateserial -days 3650 -sha256 -extfile leaf_ext.cnf -out leaf.pem 2>/dev/null\n"
"reqcnf other.cnf 'other.test'\n"
"openssl req -new -key leaf.key -config other.cnf -out other.csr\n"
"cat > other_ext.cnf <<'EOF'\n"
"basicConstraints = critical,CA:FALSE\n"
"subjectAltName = DNS:other.test\n"
"EOF\n"
"openssl x509 -req -in other.csr -CA int.pem -CAkey int.key -CAcreateserial -days 3650 -sha256 -extfile other_ext.cnf -out other.pem 2>/dev/null\n"
"openssl x509 -req -in leaf.csr -CA int.pem -CAkey int.key -CAcreateserial -not_before 20200101000000Z -not_after 20210101000000Z -sha256 -extfile leaf_ext.cnf -out expired.pem 2>/dev/null\n"
"openssl x509 -req -in leaf.csr -CA int.pem -CAkey int.key -CAcreateserial -not_before 20900101000000Z -not_after 20950101000000Z -sha256 -extfile leaf_ext.cnf -out future.pem 2>/dev/null\n"
"openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out nonca.key 2>/dev/null\n"
"reqcnf nonca.cnf 'Kestrel Not A CA'\n"
"openssl req -new -key nonca.key -config nonca.cnf -out nonca.csr\n"
"cat > nonca_ext.cnf <<'EOF'\n"
"basicConstraints = critical,CA:FALSE\n"
"EOF\n"
"openssl x509 -req -in nonca.csr -CA root.pem -CAkey root.key -CAcreateserial -days 3650 -sha256 -extfile nonca_ext.cnf -out nonca.pem 2>/dev/null\n"
"openssl x509 -req -in leaf.csr -CA nonca.pem -CAkey nonca.key -CAcreateserial -days 3650 -sha256 -extfile leaf_ext.cnf -out under_nonca.pem 2>/dev/null\n"
"openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out int2.key 2>/dev/null\n"
"reqcnf int2.cnf 'Kestrel Test Sub Intermediate'\n"
"openssl req -new -key int2.key -config int2.cnf -out int2.csr\n"
"cat > int2_ext.cnf <<'EOF'\n"
"basicConstraints = critical,CA:TRUE\n"
"keyUsage = critical,keyCertSign,cRLSign\n"
"EOF\n"
"openssl x509 -req -in int2.csr -CA int.pem -CAkey int.key -CAcreateserial -days 3650 -sha256 -extfile int2_ext.cnf -out int2.pem 2>/dev/null\n"
"openssl x509 -req -in leaf.csr -CA int2.pem -CAkey int2.key -CAcreateserial -days 3650 -sha256 -extfile leaf_ext.cnf -out leaf2.pem 2>/dev/null\n"
"openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:1024 -out weak.key 2>/dev/null\n"
"selfcnf weak.cnf 'Kestrel Weak Root' ca_ext.txt\n"
"openssl req -x509 -new -key weak.key -config weak.cnf -days 3650 -sha256 -out weakroot.pem\n"
"openssl x509 -req -in leaf.csr -CA weakroot.pem -CAkey weak.key -CAcreateserial -days 3650 -sha256 -extfile leaf_ext.cnf -out weakleaf.pem 2>/dev/null\n"
"openssl x509 -req -in leaf.csr -CA int.pem -CAkey int.key -CAcreateserial -days 3650 -sha256 -sigopt rsa_padding_mode:pss -sigopt rsa_pss_saltlen:32 -sigopt rsa_mgf1_md:sha256 -extfile leaf_ext.cnf -out pssleaf.pem 2>/dev/null || rm -f pssleaf.pem\n"
"openssl x509 -req -in leaf.csr -CA int.pem -CAkey int.key -CAcreateserial -days 3650 -sha384 -extfile leaf_ext.cnf -out leaf384.pem 2>/dev/null\n"
"cat leaf_ext.cnf > crit_ext.cnf\n"
"printf '1.2.3.4.5.6.7.8 = critical,DER:05:00\\n' >> crit_ext.cnf\n"
"openssl x509 -req -in leaf.csr -CA int.pem -CAkey int.key -CAcreateserial -days 3650 -sha256 -extfile crit_ext.cnf -out critleaf.pem 2>/dev/null\n"
"cat leaf_ext.cnf > longoid_ext.cnf\n"
"printf '1.2.3.4.5.6.7.8.9.10.11.12.13.14.15.16.17.18.19.20.21.22.23.24.25.26.27.28.29.30 = critical,DER:05:00\\n' >> longoid_ext.cnf\n"
"openssl x509 -req -in leaf.csr -CA int.pem -CAkey int.key -CAcreateserial -days 3650 -sha256 -extfile longoid_ext.cnf -out longoid.pem 2>/dev/null\n"
"printf 'extendedKeyUsage = clientAuth\\n' > noneku_ext.cnf\n"
"printf 'basicConstraints = critical,CA:FALSE\\nsubjectAltName = DNS:example.test\\n' >> noneku_ext.cnf\n"
"openssl x509 -req -in leaf.csr -CA int.pem -CAkey int.key -CAcreateserial -days 3650 -sha256 -extfile noneku_ext.cnf -out clientonly.pem 2>/dev/null\n"
"openssl ecparam -name prime256v1 -genkey -noout -out ecroot.key 2>/dev/null\n"
"selfcnf ecroot.cnf 'Kestrel Test Root EC' ca_ext.txt\n"
"openssl req -x509 -new -key ecroot.key -config ecroot.cnf -days 3650 -sha256 -out ecroot.pem\n"
"openssl ecparam -name prime256v1 -genkey -noout -out ecleaf.key 2>/dev/null\n"
"reqcnf ecleaf.cnf 'ec.example.test'\n"
"openssl req -new -key ecleaf.key -config ecleaf.cnf -out ecleaf.csr\n"
"cat > ecleaf_ext.cnf <<'EOF'\n"
"basicConstraints = critical,CA:FALSE\n"
"keyUsage = critical,digitalSignature\n"
"extendedKeyUsage = serverAuth\n"
"subjectAltName = DNS:ec.example.test\n"
"EOF\n"
"openssl x509 -req -in ecleaf.csr -CA ecroot.pem -CAkey ecroot.key -CAcreateserial -days 3650 -sha256 -extfile ecleaf_ext.cnf -out ecleaf.pem 2>/dev/null\n"
"openssl x509 -req -in ecleaf.csr -CA int.pem -CAkey int.key -CAcreateserial -days 3650 -sha256 -extfile ecleaf_ext.cnf -out ecmixed.pem 2>/dev/null\n"
"openssl rsa -in leaf.key -pubout -RSAPublicKey_out -outform DER -out rsapub.der 2>/dev/null\n"
"printf 'kestrel rsa signature vector' > msg.bin\n"
"openssl dgst -sha256 -sign leaf.key -out sig_pkcs1_sha256.bin msg.bin\n"
"openssl dgst -sha384 -sign leaf.key -out sig_pkcs1_sha384.bin msg.bin\n"
"openssl dgst -sha512 -sign leaf.key -out sig_pkcs1_sha512.bin msg.bin\n"
"openssl dgst -sha1 -sign leaf.key -out sig_pkcs1_sha1.bin msg.bin\n"
"openssl dgst -sha256 -sign leaf.key -sigopt rsa_padding_mode:pss -sigopt rsa_pss_saltlen:32 -out sig_pss32.bin msg.bin\n"
"openssl dgst -sha256 -sign leaf.key -sigopt rsa_padding_mode:pss -sigopt rsa_pss_saltlen:0 -out sig_pss0.bin msg.bin\n"
"i=0\n"
"while [ $i -lt 32 ]; do\n"
"  openssl ecparam -name prime256v1 -genkey -noout -out v$i.key 2>/dev/null\n"
"  openssl ec -in v$i.key -pubout -outform DER -conv_form uncompressed -out vpub$i.der 2>/dev/null\n"
"  printf 'kestrel ecdsa vector %d' $i > vmsg$i.bin\n"
"  openssl dgst -sha256 -sign v$i.key -out vsig$i.der vmsg$i.bin\n"
"  i=$((i + 1))\n"
"done\n"
"for f in root int leaf other expired future nonca under_nonca int2 leaf2 weakroot weakleaf leaf384 ecroot ecleaf ecmixed pssleaf critleaf clientonly longoid; do\n"
"  if [ -f $f.pem ]; then openssl x509 -in $f.pem -outform DER -out $f.der; fi\n"
"done\n"
"cat root.pem int.pem ecroot.pem > roots.pem\n"
/* Last, so that "now" is after the notBefore of every certificate above:
 * generating a few RSA keys takes long enough for a timestamp taken at
 * the start to land before the certificates written at the end. */
"date +%s > now.txt\n"
"echo OK > fixtures_ready.txt\n";

static const char *const GEN_PY =
"import random, sys, os\n"
"d = sys.argv[1]\n"
"random.seed(20260726)\n"
"def h(x):\n"
"    return format(x, 'x')\n"
"f = open(os.path.join(d, 'bnvec.txt'), 'w')\n"
"def rnd(bits):\n"
"    if bits <= 0: return 0\n"
"    return random.getrandbits(bits)\n"
"edge = [0, 1, 2, 3, (1<<64)-1, 1<<64, (1<<64)+1, (1<<128)-1, 1<<255,\n"
"        (1<<2048)-1, 1<<4095, (1<<4096)-1]\n"
"cases = []\n"
"for a in edge:\n"
"    for b in edge:\n"
"        if a.bit_length() + b.bit_length() <= 8192:\n"
"            cases.append((a, b))\n"
"for i in range(3000):\n"
"    ab = random.randint(0, 4096); bb = random.randint(0, 4096)\n"
"    if ab + bb > 8192: bb = 8192 - ab\n"
"    cases.append((rnd(ab), rnd(bb)))\n"
"for a, b in cases:\n"
"    f.write('MUL %s %s %s\\n' % (h(a), h(b), h(a*b)))\n"
"dcases = []\n"
"for a in edge:\n"
"    for b in edge:\n"
"        if b and b.bit_length() <= 4096:\n"
"            dcases.append((a, b))\n"
"for i in range(3000):\n"
"    ab = random.randint(0, 4096); bb = random.randint(1, 4096)\n"
"    a = rnd(ab); b = rnd(bb)\n"
"    if b == 0: b = 1\n"
"    dcases.append((a, b))\n"
"for a, b in dcases:\n"
"    f.write('DIVMOD %s %s %s %s\\n' % (h(a), h(b), h(a//b), h(a%b)))\n"
/* A dividend twice as wide as anything that can be imported -- the case
 * Montgomery setup hits when it reduces 2^(128k) -- can only be built
 * inside the library, so the vector gives the two factors instead. */
"for i in range(1500):\n"
"    a = rnd(random.randint(0, 4096)); b = rnd(random.randint(0, 4096))\n"
"    m = rnd(random.randint(1, 4096)) | 1\n"
"    f.write('MULMOD %s %s %s %s %s\\n' % (h(a), h(b), h(m), h(a*b//m), h(a*b%m)))\n"
"for i in range(400):\n"
"    mb = random.choice([65, 128, 256, 512, 1024, 2048, 3072, 4096])\n"
"    m = rnd(mb) | 1 | (1 << (mb-1))\n"
"    base = rnd(random.randint(1, mb))\n"
"    e = rnd(random.randint(1, 64))\n"
"    f.write('MODEXP %s %s %s %s\\n' % (h(base), h(e), h(m), h(pow(base, e, m))))\n"
"for i in range(20):\n"
"    mb = random.choice([1024, 2048])\n"
"    m = rnd(mb) | 1 | (1 << (mb-1))\n"
"    base = rnd(mb); e = rnd(mb)\n"
"    f.write('MODEXP %s %s %s %s\\n' % (h(base), h(e), h(m), h(pow(base, e, m))))\n"
"f.close()\n"
"P = 2**255 - 19\n"
"A24 = 121665\n"
"def cswap(sw, x2, x3):\n"
"    dummy = sw * ((x2 - x3) % P)\n"
"    return (x2 - dummy) % P, (x3 + dummy) % P\n"
"def x25519(kb, ub):\n"
"    k = bytearray(kb); k[0] &= 248; k[31] &= 127; k[31] |= 64\n"
"    kk = int.from_bytes(bytes(k), 'little')\n"
"    x1 = int.from_bytes(ub, 'little') & ((1 << 255) - 1)\n"
"    x2, z2, x3, z3, sw = 1, 0, x1, 1, 0\n"
"    for t in range(254, -1, -1):\n"
"        kt = (kk >> t) & 1\n"
"        sw ^= kt\n"
"        x2, x3 = cswap(sw, x2, x3); z2, z3 = cswap(sw, z2, z3)\n"
"        sw = kt\n"
"        A = (x2 + z2) % P; AA = A * A % P\n"
"        B = (x2 - z2) % P; BB = B * B % P\n"
"        E = (AA - BB) % P\n"
"        C = (x3 + z3) % P; D = (x3 - z3) % P\n"
"        DA = D * A % P; CB = C * B % P\n"
"        x3 = (DA + CB) ** 2 % P; z3 = x1 * ((DA - CB) ** 2) % P\n"
"        x2 = AA * BB % P; z2 = E * (AA + A24 * E) % P\n"
"    x2, x3 = cswap(sw, x2, x3); z2, z3 = cswap(sw, z2, z3)\n"
"    return ((x2 * pow(z2, P - 2, P)) % P).to_bytes(32, 'little')\n"
"f = open(os.path.join(d, 'x25519vec.txt'), 'w')\n"
"for i in range(200):\n"
"    kb = bytes(random.getrandbits(8) for _ in range(32))\n"
"    ub = bytes(random.getrandbits(8) for _ in range(32))\n"
"    f.write('%s %s %s\\n' % (kb.hex(), ub.hex(), x25519(kb, ub).hex()))\n"
"base = bytes([9] + [0]*31)\n"
"for i in range(50):\n"
"    kb = bytes(random.getrandbits(8) for _ in range(32))\n"
"    f.write('%s %s %s\\n' % (kb.hex(), base.hex(), x25519(kb, base).hex()))\n"
"f.close()\n"
"pp = 2**256 - 2**224 + 2**192 + 2**96 - 1\n"
"aa = -3 % pp\n"
"Gx = 0x6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296\n"
"Gy = 0x4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5\n"
"nn = 0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551\n"
"def padd(Pt, Q):\n"
"    if Pt is None: return Q\n"
"    if Q is None: return Pt\n"
"    x1, y1 = Pt; x2, y2 = Q\n"
"    if x1 == x2 and (y1 + y2) % pp == 0: return None\n"
"    if Pt == Q: lam = (3*x1*x1 + aa) * pow(2*y1, -1, pp) % pp\n"
"    else: lam = (y2 - y1) * pow(x2 - x1, -1, pp) % pp\n"
"    x3 = (lam*lam - x1 - x2) % pp\n"
"    return (x3, (lam*(x1 - x3) - y1) % pp)\n"
"def pmul(k, Pt):\n"
"    R = None\n"
"    for i in range(k.bit_length() - 1, -1, -1):\n"
"        R = padd(R, R)\n"
"        if (k >> i) & 1: R = padd(R, Pt)\n"
"    return R\n"
"f = open(os.path.join(d, 'p256vec.txt'), 'w')\n"
"ks = [1, 2, 3, 4, 5, nn-1, nn-2, nn//2, 2**255, 2**255+1]\n"
"ks += [random.randrange(1, nn) for _ in range(60)]\n"
"for k in ks:\n"
"    x, y = pmul(k, (Gx, Gy))\n"
"    f.write('BASE %064x %064x %064x\\n' % (k, x, y))\n"
"for i in range(30):\n"
"    d1 = random.randrange(1, nn); d2 = random.randrange(1, nn)\n"
"    Pt = pmul(d1, (Gx, Gy))\n"
"    S = pmul(d2, Pt)\n"
"    f.write('ECDH %064x %064x %064x %064x\\n' % (d2, Pt[0], Pt[1], S[0]))\n"
"f.close()\n"
"open(os.path.join(d, 'pyvec_ready.txt'), 'w').write('OK\\n')\n";

static int write_file(const char *name, const char *data)
{
    FILE *f = fopen(fixpath(name), "wb");

    if (!f)
        return -1;
    fwrite(data, 1, strlen(data), f);
    fclose(f);
    return 0;
}

static int generate_fixtures(void)
{
    char cmd[1400];
    int rc;

    if (write_file("gen.sh", GEN_SH) < 0 ||
        write_file("gen.py", GEN_PY) < 0) {
        printf("cannot write generator scripts into %s\n", g_dir);
        return -1;
    }
    snprintf(cmd, sizeof(cmd), "sh '%s/gen.sh' '%s' >/dev/null 2>&1",
             g_dir, g_dir);
    rc = system(cmd);
    if (rc != 0)
        printf("  note: the openssl fixture script exited %d\n", rc);
    snprintf(cmd, sizeof(cmd), "python3 '%s/gen.py' '%s' >/dev/null 2>&1",
             g_dir, g_dir);
    rc = system(cmd);
    if (rc != 0)
        printf("  note: the python fixture script exited %d\n", rc);
    return 0;
}

/* ================================================================== */
/* bignum                                                             */
/* ================================================================== */

static int parse_hex_bn(const char *s, struct bn *out)
{
    uint8_t buf[BN_LIMBS * 8];
    size_t len = strlen(s), i, n = 0;
    int odd = (int)(len & 1);

    if (len > sizeof(buf) * 2)
        return -1;
    for (i = 0; i < len; i++) {
        int v = hexval(s[i]);
        if (v < 0)
            return -1;
        if (i == 0 && odd) {
            buf[n++] = (uint8_t)v;
            continue;
        }
        if ((i - (size_t)odd) % 2 == 0)
            buf[n] = (uint8_t)(v << 4);
        else
            buf[n++] |= (uint8_t)v;
    }
    return bn_from_bytes(out, buf, n);
}

static void test_bignum(void)
{
    size_t len;
    uint8_t *data;
    char *line, *save;
    int nmul = 0, ndiv = 0, nexp = 0, nwide = 0, bad = 0;

    section("bignum against python's arbitrary-precision integers");
    data = read_file("bnvec.txt", &len);
    if (!data) {
        CHECK(0, "bnvec.txt was not generated (is python3 installed?)");
        return;
    }
    for (line = strtok_r((char *)data, "\n", &save); line;
         line = strtok_r(0, "\n", &save)) {
        char *op = strtok(line, " ");
        char *a_s = strtok(0, " "), *b_s = strtok(0, " ");
        char *c_s = strtok(0, " "), *d_s = strtok(0, " ");
        char *e_s = strtok(0, " ");
        struct bn a, b, r1, r2;
        char got[2100], *e;

        if (!op || !a_s || !b_s || !c_s)
            continue;
        if (parse_hex_bn(a_s, &a) < 0 || parse_hex_bn(b_s, &b) < 0)
            continue;

        if (strcmp(op, "MUL") == 0) {
            nmul++;
            if (bn_mul(&r1, &a, &b) < 0) {
                if (bad++ < 4) CHECK(0, "bn_mul refused a valid product");
                continue;
            }
            bn_hex(&r1, got);
            if (strcmp(got, c_s) != 0 && bad++ < 4)
                CHECK(0, "MUL %.24s * %.24s -> %.32s want %.32s",
                      a_s, b_s, got, c_s);
        } else if (strcmp(op, "DIVMOD") == 0) {
            ndiv++;
            if (bn_divmod(&r1, &r2, &a, &b) < 0) {
                if (bad++ < 4) CHECK(0, "bn_divmod refused a valid division");
                continue;
            }
            bn_hex(&r1, got);
            if (strcmp(got, c_s) != 0 && bad++ < 4)
                CHECK(0, "DIV %.24s / %.24s -> %.32s want %.32s",
                      a_s, b_s, got, c_s);
            bn_hex(&r2, got);
            if (d_s && strcmp(got, d_s) != 0 && bad++ < 4)
                CHECK(0, "MOD %.24s %% %.24s -> %.32s want %.32s",
                      a_s, b_s, got, d_s);
        } else if (strcmp(op, "MULMOD") == 0) {
            struct bn m, prod;
            nwide++;
            if (!e_s || parse_hex_bn(c_s, &m) < 0)
                continue;
            if (bn_mul(&prod, &a, &b) < 0) {
                if (bad++ < 4) CHECK(0, "bn_mul refused a wide product");
                continue;
            }
            if (bn_divmod(&r1, &r2, &prod, &m) < 0) {
                if (bad++ < 4) CHECK(0, "bn_divmod refused a wide dividend");
                continue;
            }
            bn_hex(&r1, got);
            if (strcmp(got, d_s) != 0 && bad++ < 4)
                CHECK(0, "(a*b)/m -> %.32s want %.32s", got, d_s);
            bn_hex(&r2, got);
            if (strcmp(got, e_s) != 0 && bad++ < 4)
                CHECK(0, "(a*b)%%m -> %.32s want %.32s", got, e_s);
        } else if (strcmp(op, "MODEXP") == 0) {
            struct bn_mont mt;
            struct bn m;
            nexp++;
            if (parse_hex_bn(c_s, &m) < 0)
                continue;
            if (bn_mont_init(&mt, &m) < 0) {
                if (bad++ < 4) CHECK(0, "bn_mont_init refused an odd modulus");
                continue;
            }
            if (bn_modexp(&r1, &a, &b, &mt) < 0) {
                if (bad++ < 4) CHECK(0, "bn_modexp failed");
                continue;
            }
            bn_hex(&r1, got);
            e = d_s;
            if (e && strcmp(got, e) != 0 && bad++ < 4)
                CHECK(0, "MODEXP %.20s ^ %.12s mod %.20s -> %.32s want %.32s",
                      a_s, b_s, c_s, got, e);
        }
    }
    free(data);
    CHECK(bad == 0, "%d bignum vectors disagreed with python", bad);
    printf("  %d multiplications, %d divisions, %d wide (8192-bit dividend) "
           "divisions,\n  %d modular exponentiations checked\n",
           nmul, ndiv, nwide, nexp);
    CHECK(nmul > 2000 && ndiv > 2000 && nexp > 300 && nwide > 1000,
          "too few vectors were generated (%d/%d/%d/%d)", nmul, ndiv, nwide,
          nexp);

    /* Boundary behaviour that python cannot express for us. */
    {
        struct bn a, b, r;
        struct bn_mont mt;
        uint8_t big[BN_MAX_BYTES + 1];

        memset(big, 0xff, sizeof(big));
        CHECK(bn_from_bytes(&a, big, sizeof(big)) < 0,
              "a %d-byte import should be refused", (int)sizeof(big));
        CHECK(bn_from_bytes(&a, big, BN_MAX_BYTES) == 0,
              "a %d-byte import should be accepted", BN_MAX_BYTES);
        CHECK(bn_bits(&a) == BN_MAX_BITS, "bn_bits of all-ones is %d",
              bn_bits(&a));
        bn_copy(&b, &a);
        CHECK(bn_mul(&r, &a, &b) == 0, "4096x4096 product must fit");
        CHECK(bn_bits(&r) == 8192, "the square of all-ones is %d bits",
              bn_bits(&r));
        bn_set_u64(&b, 0);
        CHECK(bn_divmod(0, &r, &a, &b) < 0, "division by zero must fail");
        bn_set_u64(&b, 4);
        CHECK(bn_mont_init(&mt, &b) < 0,
              "Montgomery setup must refuse an even modulus");
        bn_set_u64(&a, 0);
        CHECK(bn_to_bytes(&a, big, 1) == 0 && big[0] == 0,
              "zero exports as a zero byte");
        bn_set_u64(&a, 0x1234);
        CHECK(bn_to_bytes(&a, big, 1) < 0,
              "an export that does not fit must fail");
    }
}

/* ================================================================== */
/* X25519                                                             */
/* ================================================================== */

static void test_x25519(void)
{
    /* RFC 7748 section 5.2 and 6.1, quoted as published test vectors. */
    static const char *v1k =
        "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4";
    static const char *v1u =
        "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c";
    static const char *v1o =
        "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552";
    static const char *v2k =
        "4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d";
    static const char *v2u =
        "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493";
    static const char *v2o =
        "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957";
    static const char *alice =
        "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a";
    static const char *alice_pub =
        "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a";
    static const char *bob =
        "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb";
    static const char *bob_pub =
        "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f";
    static const char *shared =
        "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742";
    /* section 5.2: one and one thousand iterations of the loop */
    static const char *iter1 =
        "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079";
    static const char *iter1000 =
        "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51";

    uint8_t k[32], u[32], o[32], want[32];
    char hex[80];
    size_t len;
    uint8_t *data;
    int i, bad = 0, n = 0;

    section("X25519 (RFC 7748)");
    unhex(v1k, k, 32); unhex(v1u, u, 32); unhex(v1o, want, 32);
    x25519(o, k, u);
    CHECK(memcmp(o, want, 32) == 0, "section 5.2 vector 1");
    unhex(v2k, k, 32); unhex(v2u, u, 32); unhex(v2o, want, 32);
    x25519(o, k, u);
    CHECK(memcmp(o, want, 32) == 0, "section 5.2 vector 2");

    unhex(alice, k, 32);
    x25519_base(o, k);
    unhex(alice_pub, want, 32);
    CHECK(memcmp(o, want, 32) == 0, "section 6.1 Alice's public key");
    unhex(bob, k, 32);
    x25519_base(o, k);
    unhex(bob_pub, want, 32);
    CHECK(memcmp(o, want, 32) == 0, "section 6.1 Bob's public key");
    unhex(alice, k, 32); unhex(bob_pub, u, 32);
    x25519(o, k, u);
    unhex(shared, want, 32);
    CHECK(memcmp(o, want, 32) == 0, "section 6.1 shared secret (A side)");
    unhex(bob, k, 32); unhex(alice_pub, u, 32);
    x25519(o, k, u);
    CHECK(memcmp(o, want, 32) == 0, "section 6.1 shared secret (B side)");

    /* The iterated test: k and u both start at the standard base value. */
    memset(k, 0, 32); k[0] = 9;
    memcpy(u, k, 32);
    for (i = 0; i < 1000; i++) {
        uint8_t out[32];
        x25519(out, k, u);
        memcpy(u, k, 32);
        memcpy(k, out, 32);
        if (i == 0) {
            unhex(iter1, want, 32);
            CHECK(memcmp(k, want, 32) == 0, "section 5.2 after 1 iteration");
        }
    }
    unhex(iter1000, want, 32);
    CHECK(memcmp(k, want, 32) == 0, "section 5.2 after 1000 iterations");

    /* Small-order points must be reported, not silently used. */
    memset(u, 0, 32);
    memset(k, 0x5a, 32);
    CHECK(x25519(o, k, u) == -1, "an all-zero peer value must be rejected");
    memset(u, 0, 32); u[0] = 1;
    CHECK(x25519(o, k, u) == -1, "the order-1 point must be rejected");

    /* Random cross-check against the python reference. */
    data = read_file("x25519vec.txt", &len);
    if (!data) {
        CHECK(0, "x25519vec.txt was not generated");
        return;
    }
    {
        char *line, *save;
        for (line = strtok_r((char *)data, "\n", &save); line;
             line = strtok_r(0, "\n", &save)) {
            char *ks = strtok(line, " "), *us = strtok(0, " "),
                 *os = strtok(0, " ");
            if (!ks || !us || !os)
                continue;
            if (unhex(ks, k, 32) != 32 || unhex(us, u, 32) != 32)
                continue;
            x25519(o, k, u);
            tohex(o, 32, hex);
            n++;
            if (strcmp(hex, os) != 0)
                bad++;
        }
    }
    free(data);
    CHECK(bad == 0, "%d of %d random X25519 vectors disagreed", bad, n);
    printf("  %d random scalar multiplications cross-checked\n", n);
}

/* ================================================================== */
/* P-256                                                              */
/* ================================================================== */

static void test_p256(void)
{
    size_t len;
    uint8_t *data;
    char *line, *save;
    int nbase = 0, necdh = 0, bad = 0;

    section("NIST P-256 group arithmetic");
    data = read_file("p256vec.txt", &len);
    if (!data) {
        CHECK(0, "p256vec.txt was not generated");
        return;
    }
    for (line = strtok_r((char *)data, "\n", &save); line;
         line = strtok_r(0, "\n", &save)) {
        char *op = strtok(line, " ");
        char *a = strtok(0, " "), *b = strtok(0, " "), *c = strtok(0, " ");
        char *d = strtok(0, " ");
        uint8_t k[32], x[32], y[32], out[32];
        struct p256_point pt;
        char hex[80];

        if (!op || !a || !b || !c)
            continue;
        if (strcmp(op, "BASE") == 0) {
            nbase++;
            if (unhex(a, k, 32) != 32 || unhex(b, x, 32) != 32 ||
                unhex(c, y, 32) != 32)
                continue;
            if (p256_base_mul(&pt, k) < 0) {
                bad++;
                continue;
            }
            if (memcmp(pt.x, x, 32) != 0 || memcmp(pt.y, y, 32) != 0) {
                if (bad++ < 4) {
                    tohex(pt.x, 32, hex);
                    CHECK(0, "k*G for k=%.16s gave x=%.32s want %.32s",
                          a, hex, b);
                }
            }
            /* round-trip the encoding, uncompressed and compressed */
            {
                uint8_t enc[65], comp[33];
                struct p256_point back;
                p256_point_to_bytes(&pt, enc);
                CHECK(p256_point_from_bytes(&back, enc, 65) == 0 &&
                      memcmp(back.x, pt.x, 32) == 0 &&
                      memcmp(back.y, pt.y, 32) == 0,
                      "uncompressed point round-trip");
                comp[0] = (uint8_t)(0x02 | (pt.y[31] & 1));
                memcpy(comp + 1, pt.x, 32);
                CHECK(p256_point_from_bytes(&back, comp, 33) == 0 &&
                      memcmp(back.x, pt.x, 32) == 0 &&
                      memcmp(back.y, pt.y, 32) == 0,
                      "compressed point decompresses to the same y");
            }
        } else if (strcmp(op, "ECDH") == 0 && d) {
            necdh++;
            if (unhex(a, k, 32) != 32 || unhex(b, x, 32) != 32 ||
                unhex(c, y, 32) != 32)
                continue;
            memcpy(pt.x, x, 32);
            memcpy(pt.y, y, 32);
            if (p256_ecdh(out, k, &pt) < 0) {
                bad++;
                continue;
            }
            {
                uint8_t want[32];
                unhex(d, want, 32);
                if (memcmp(out, want, 32) != 0 && bad++ < 4)
                    CHECK(0, "ECDH shared x disagreed for d=%.16s", a);
            }
        }
    }
    free(data);
    CHECK(bad == 0, "%d P-256 vectors disagreed with python", bad);
    printf("  %d base-point multiplications and %d ECDH values checked\n",
           nbase, necdh);

    /* Points that are not on the curve, and malformed encodings. */
    {
        struct p256_point pt;
        uint8_t enc[65];
        memset(enc, 0, sizeof(enc));
        enc[0] = 0x04;
        CHECK(p256_point_from_bytes(&pt, enc, 65) < 0,
              "(0,0) is not a valid point");
        memset(enc + 1, 0xaa, 64);
        CHECK(p256_point_from_bytes(&pt, enc, 65) < 0,
              "a random (x,y) is not on the curve");
        enc[0] = 0x05;
        CHECK(p256_point_from_bytes(&pt, enc, 65) < 0,
              "an unknown point format byte is rejected");
        enc[0] = 0x04;
        CHECK(p256_point_from_bytes(&pt, enc, 64) < 0,
              "a truncated point is rejected");
        memset(enc, 0xff, 65);
        enc[0] = 0x04;
        CHECK(p256_point_from_bytes(&pt, enc, 65) < 0,
              "coordinates at or above p are rejected");
    }
    {
        struct p256_point pt;
        uint8_t d[32];
        memset(d, 0, 32);
        CHECK(p256_base_mul(&pt, d) < 0, "a zero private scalar is rejected");
        memset(d, 0xff, 32);
        CHECK(p256_base_mul(&pt, d) < 0,
              "a private scalar at or above n is rejected");
    }
}

/* ================================================================== */
/* ECDSA against openssl-produced signatures                          */
/* ================================================================== */

static void test_ecdsa(void)
{
    int i, checked = 0;

    section("P-256 ECDSA verification against openssl signatures");
    for (i = 0; i < 32; i++) {
        char name[64];
        uint8_t *spki, *sig, *msg;
        size_t spki_len, sig_len, msg_len;
        struct p256_point pub;
        struct der d, seq;
        struct der_tlv t;
        const uint8_t *r, *s;
        size_t rlen, slen;
        uint8_t digest[32];

        snprintf(name, sizeof(name), "vpub%d.der", i);
        spki = read_file(name, &spki_len);
        snprintf(name, sizeof(name), "vsig%d.der", i);
        sig = read_file(name, &sig_len);
        snprintf(name, sizeof(name), "vmsg%d.bin", i);
        msg = read_file(name, &msg_len);
        if (!spki || !sig || !msg) {
            free(spki); free(sig); free(msg);
            continue;
        }
        /* The uncompressed point is the tail of a P-256 SPKI. */
        CHECK(spki_len > 65 && spki[spki_len - 65] == 0x04,
              "vector %d: SPKI does not end in an uncompressed point", i);
        if (spki_len > 65 &&
            p256_point_from_bytes(&pub, spki + spki_len - 65, 65) == 0) {
            sha256(msg, msg_len, digest);
            der_init(&d, sig, sig_len);
            if (der_read_expect(&d, &t, DER_SEQUENCE) == 0) {
                der_enter(&t, &seq);
                if (der_read_uint(&seq, &r, &rlen) == 0 &&
                    der_read_uint(&seq, &s, &slen) == 0) {
                    checked++;
                    CHECK(p256_ecdsa_verify(&pub, digest, 32, r, rlen,
                                            s, slen) == 1,
                          "vector %d: a valid signature was rejected", i);
                    /* every single-bit change to the digest must break it */
                    digest[i % 32] ^= 0x01;
                    CHECK(p256_ecdsa_verify(&pub, digest, 32, r, rlen,
                                            s, slen) == 0,
                          "vector %d: a flipped digest bit still verified", i);
                    digest[i % 32] ^= 0x01;
                    /* r and s must be rejected outside [1, n-1] */
                    {
                        uint8_t zero[32];
                        memset(zero, 0, 32);
                        CHECK(p256_ecdsa_verify(&pub, digest, 32, zero, 32,
                                                s, slen) == 0,
                              "vector %d: r = 0 was accepted", i);
                        CHECK(p256_ecdsa_verify(&pub, digest, 32, r, rlen,
                                                zero, 32) == 0,
                              "vector %d: s = 0 was accepted", i);
                        memset(zero, 0xff, 32);
                        CHECK(p256_ecdsa_verify(&pub, digest, 32, zero, 32,
                                                s, slen) == 0,
                              "vector %d: r >= n was accepted", i);
                    }
                    /* swapping r and s must not verify */
                    CHECK(p256_ecdsa_verify(&pub, digest, 32, s, slen,
                                            r, rlen) == 0,
                          "vector %d: r and s swapped still verified", i);
                }
            }
        }
        free(spki); free(sig); free(msg);
    }
    CHECK(checked >= 30, "only %d ECDSA vectors were usable", checked);
    printf("  %d openssl ECDSA signatures verified, each with 5 negatives\n",
           checked);
}

/* ================================================================== */
/* RSA against openssl-produced signatures                            */
/* ================================================================== */

static void rsa_one(const struct rsa_pubkey *key, const char *file, int md_id,
                    const void *msg, size_t msg_len, int pss, int salt)
{
    uint8_t *sig;
    size_t sig_len;
    uint8_t digest[CRYPTO_MD_MAX_LEN];
    const struct crypto_md *md = crypto_md_get(md_id);
    int ok;

    sig = read_file(file, &sig_len);
    if (!sig || !md) {
        CHECK(0, "%s is missing", file);
        free(sig);
        return;
    }
    md->hash(msg, msg_len, digest);
    if (pss)
        ok = rsa_verify_pss(key, md_id, md_id, salt, digest,
                            (size_t)md->digest_len, sig, sig_len);
    else
        ok = rsa_verify_pkcs1_v15(key, md_id, digest,
                                  (size_t)md->digest_len, sig, sig_len);
    CHECK(ok == 1, "%s: a valid signature was rejected", file);

    /* Every single-bit change anywhere in the signature must break it. */
    {
        int i, accepted = 0;
        for (i = 0; i < (int)sig_len * 8; i += 37) {
            sig[i / 8] ^= (uint8_t)(1 << (i % 8));
            if (pss)
                ok = rsa_verify_pss(key, md_id, md_id, salt, digest,
                                    (size_t)md->digest_len, sig, sig_len);
            else
                ok = rsa_verify_pkcs1_v15(key, md_id, digest,
                                          (size_t)md->digest_len, sig,
                                          sig_len);
            if (ok)
                accepted++;
            sig[i / 8] ^= (uint8_t)(1 << (i % 8));
        }
        CHECK(accepted == 0, "%s: %d single-bit corruptions still verified",
              file, accepted);
    }
    /* A digest bit flip must break it too. */
    digest[0] ^= 0x80;
    if (pss)
        ok = rsa_verify_pss(key, md_id, md_id, salt, digest,
                            (size_t)md->digest_len, sig, sig_len);
    else
        ok = rsa_verify_pkcs1_v15(key, md_id, digest,
                                  (size_t)md->digest_len, sig, sig_len);
    CHECK(ok == 0, "%s: a flipped digest bit still verified", file);
    /* A short or long signature must be refused outright. */
    if (pss)
        ok = rsa_verify_pss(key, md_id, md_id, salt, digest,
                            (size_t)md->digest_len, sig, sig_len - 1);
    else
        ok = rsa_verify_pkcs1_v15(key, md_id, digest,
                                  (size_t)md->digest_len, sig, sig_len - 1);
    CHECK(ok == 0, "%s: a truncated signature was accepted", file);
    free(sig);
}

static void test_rsa(void)
{
    uint8_t *pub, *msg;
    size_t pub_len, msg_len;
    struct rsa_pubkey key;

    section("RSA verification against openssl signatures");
    pub = read_file("rsapub.der", &pub_len);
    msg = read_file("msg.bin", &msg_len);
    if (!pub || !msg) {
        CHECK(0, "rsapub.der / msg.bin are missing");
        free(pub); free(msg);
        return;
    }
    CHECK(rsa_pubkey_from_der(&key, pub, pub_len) == 0,
          "the RSAPublicKey DER did not parse");
    CHECK(key.bits == 2048, "expected a 2048-bit key, got %d", key.bits);

    rsa_one(&key, "sig_pkcs1_sha256.bin", CRYPTO_MD_SHA256, msg, msg_len, 0, 0);
    rsa_one(&key, "sig_pkcs1_sha384.bin", CRYPTO_MD_SHA384, msg, msg_len, 0, 0);
    rsa_one(&key, "sig_pkcs1_sha512.bin", CRYPTO_MD_SHA512, msg, msg_len, 0, 0);
    rsa_one(&key, "sig_pkcs1_sha1.bin",   CRYPTO_MD_SHA1,   msg, msg_len, 0, 0);
    rsa_one(&key, "sig_pss32.bin", CRYPTO_MD_SHA256, msg, msg_len, 1, 32);
    rsa_one(&key, "sig_pss0.bin",  CRYPTO_MD_SHA256, msg, msg_len, 1, 0);
    /* the automatic salt-length path must accept both */
    {
        uint8_t *sig;
        size_t sl;
        uint8_t dg[32];
        sha256(msg, msg_len, dg);
        sig = read_file("sig_pss32.bin", &sl);
        if (sig) {
            CHECK(rsa_verify_pss(&key, CRYPTO_MD_SHA256, CRYPTO_MD_SHA256,
                                 -1, dg, 32, sig, sl) == 1,
                  "PSS with a recovered salt length (32) failed");
            CHECK(rsa_verify_pss(&key, CRYPTO_MD_SHA256, CRYPTO_MD_SHA256,
                                 20, dg, 32, sig, sl) == 0,
                  "PSS accepted the wrong declared salt length");
            free(sig);
        }
        sig = read_file("sig_pss0.bin", &sl);
        if (sig) {
            CHECK(rsa_verify_pss(&key, CRYPTO_MD_SHA256, CRYPTO_MD_SHA256,
                                 -1, dg, 32, sig, sl) == 1,
                  "PSS with a recovered salt length (0) failed");
            free(sig);
        }
    }

    /* A signature must not verify under the wrong digest algorithm. */
    {
        uint8_t *sig;
        size_t sl;
        uint8_t dg[64];
        sig = read_file("sig_pkcs1_sha256.bin", &sl);
        if (sig) {
            sha512(msg, msg_len, dg);
            CHECK(rsa_verify_pkcs1_v15(&key, CRYPTO_MD_SHA512, dg, 64,
                                       sig, sl) == 0,
                  "a SHA-256 signature verified as SHA-512");
            sha256(msg, msg_len, dg);
            CHECK(rsa_verify_pkcs1_v15(&key, CRYPTO_MD_SHA1, dg, 20,
                                       sig, sl) == 0,
                  "a SHA-256 signature verified as SHA-1");
            free(sig);
        }
    }

    /* Forgeries of the shape that a lax parser accepts. */
    {
        uint8_t forged[256];
        uint8_t dg[32];
        int di_at;
        sha256(msg, msg_len, dg);
        /* 00 01 FF... 00 || DigestInfo, but with the DigestInfo pushed to
         * the front and the rest of the block filled with garbage -- the
         * classic "check the prefix, find the hash, ignore the tail". */
        memset(forged, 0xff, sizeof(forged));
        forged[0] = 0x00;
        forged[1] = 0x01;
        forged[2] = 0x00;
        di_at = 3;
        forged[di_at + 0] = 0x30; forged[di_at + 1] = 0x31;
        forged[di_at + 2] = 0x30; forged[di_at + 3] = 0x0d;
        forged[di_at + 4] = 0x06; forged[di_at + 5] = 0x09;
        memcpy(forged + di_at + 6,
               "\x60\x86\x48\x01\x65\x03\x04\x02\x01", 9);
        forged[di_at + 15] = 0x05; forged[di_at + 16] = 0x00;
        forged[di_at + 17] = 0x04; forged[di_at + 18] = 0x20;
        memcpy(forged + di_at + 19, dg, 32);
        CHECK(rsa_verify_pkcs1_v15(&key, CRYPTO_MD_SHA256, dg, 32,
                                   forged, 256) == 0,
              "a left-aligned DigestInfo forgery was accepted");
    }

    /* Malformed public keys. */
    {
        struct rsa_pubkey k2;
        uint8_t buf[64];
        static const uint8_t even_mod[] = {
            0x30, 0x0a, 0x02, 0x05, 0x00, 0xff, 0xff, 0xff, 0xfe,
            0x02, 0x01, 0x03
        };
        static const uint8_t even_exp[] = {
            0x30, 0x08, 0x02, 0x03, 0x01, 0x00, 0x01, 0x02, 0x01, 0x04
        };
        CHECK(rsa_pubkey_from_der(&k2, even_mod, sizeof(even_mod)) < 0,
              "a tiny/even modulus must be refused");
        CHECK(rsa_pubkey_from_der(&k2, even_exp, sizeof(even_exp)) < 0,
              "an even exponent must be refused");
        memset(buf, 0, sizeof(buf));
        CHECK(rsa_pubkey_from_der(&k2, buf, sizeof(buf)) < 0,
              "an all-zero buffer must not parse as a key");
        CHECK(rsa_pubkey_from_der(&k2, pub, pub_len - 1) < 0,
              "a truncated key must not parse");
        {
            uint8_t *withtail = malloc(pub_len + 1);
            memcpy(withtail, pub, pub_len);
            withtail[pub_len] = 0x00;
            CHECK(rsa_pubkey_from_der(&k2, withtail, pub_len + 1) < 0,
                  "trailing bytes after the key must be refused");
            free(withtail);
        }
    }
    free(pub);
    free(msg);
}

/* ================================================================== */
/* DER strictness                                                     */
/* ================================================================== */

static void test_der(void)
{
    struct der d;
    struct der_tlv t;
    uint32_t v32;
    const uint8_t *p;
    size_t l;

    section("DER decoding strictness");

    { static const uint8_t x[] = { 0x02, 0x01, 0x2a };
      der_init(&d, x, sizeof(x));
      CHECK(der_read_u32(&d, &v32) == 0 && v32 == 42, "INTEGER 42"); }
    { static const uint8_t x[] = { 0x02, 0x02, 0x00, 0x2a };
      der_init(&d, x, sizeof(x));
      CHECK(der_read_u32(&d, &v32) < 0, "non-minimal INTEGER padding"); }
    { static const uint8_t x[] = { 0x02, 0x01, 0xff };
      der_init(&d, x, sizeof(x));
      CHECK(der_read_u32(&d, &v32) < 0, "a negative INTEGER"); }
    { static const uint8_t x[] = { 0x02, 0x00 };
      der_init(&d, x, sizeof(x));
      CHECK(der_read_u32(&d, &v32) < 0, "a zero-length INTEGER"); }
    { static const uint8_t x[] = { 0x02, 0x02, 0x00, 0x80 };
      der_init(&d, x, sizeof(x));
      CHECK(der_read_u32(&d, &v32) == 0 && v32 == 0x80,
            "a legitimate leading zero"); }
    { static const uint8_t x[] = { 0x30, 0x80, 0x02, 0x01, 0x01, 0x00, 0x00 };
      der_init(&d, x, sizeof(x));
      CHECK(der_read(&d, &t) < 0, "indefinite length"); }
    { static const uint8_t x[] = { 0x04, 0x81, 0x01, 0x41 };
      der_init(&d, x, sizeof(x));
      CHECK(der_read(&d, &t) < 0, "long-form length under 128"); }
    { static const uint8_t x[] = { 0x04, 0x82, 0x00, 0x80 };
      der_init(&d, x, sizeof(x));
      CHECK(der_read(&d, &t) < 0, "long-form length with a leading zero"); }
    { static const uint8_t x[] = { 0x04, 0x05, 0x41 };
      der_init(&d, x, sizeof(x));
      CHECK(der_read(&d, &t) < 0, "a length past the end of the buffer"); }
    { static const uint8_t x[] = { 0x04, 0x7f };
      der_init(&d, x, sizeof(x));
      CHECK(der_read(&d, &t) < 0, "a 127-byte value with no bytes"); }
    { static const uint8_t x[] = { 0x01, 0x01, 0x01 };
      int b;
      der_init(&d, x, sizeof(x));
      CHECK(der_read_bool(&d, &b) < 0, "a BOOLEAN that is not 00 or FF"); }
    { static const uint8_t x[] = { 0x01, 0x01, 0xff };
      int b;
      der_init(&d, x, sizeof(x));
      CHECK(der_read_bool(&d, &b) == 0 && b == 1, "BOOLEAN TRUE"); }
    { static const uint8_t x[] = { 0x06, 0x03, 0x55, 0x04, 0x03 };
      der_init(&d, x, sizeof(x));
      CHECK(der_read_oid(&d, &p, &l) == 0 && l == 3, "OID 2.5.4.3"); }
    { static const uint8_t x[] = { 0x06, 0x03, 0x55, 0x80, 0x03 };
      der_init(&d, x, sizeof(x));
      CHECK(der_read_oid(&d, &p, &l) < 0,
            "an OID subidentifier with a leading 0x80"); }
    { static const uint8_t x[] = { 0x06, 0x02, 0x55, 0x84 };
      der_init(&d, x, sizeof(x));
      CHECK(der_read_oid(&d, &p, &l) < 0, "an OID that ends mid-subid"); }
    { static const uint8_t x[] = { 0x06, 0x00 };
      der_init(&d, x, sizeof(x));
      CHECK(der_read_oid(&d, &p, &l) < 0, "an empty OID"); }
    { static const uint8_t x[] = { 0x03, 0x03, 0x00, 0xab, 0xcd };
      der_init(&d, x, sizeof(x));
      CHECK(der_read_bitstring(&d, &p, &l) == 0 && l == 2,
            "a whole-octet BIT STRING"); }
    { static const uint8_t x[] = { 0x03, 0x03, 0x04, 0xab, 0xc0 };
      der_init(&d, x, sizeof(x));
      CHECK(der_read_bitstring(&d, &p, &l) < 0,
            "a BIT STRING with unused bits is not a key"); }
    { static const uint8_t x[] = { 0x03, 0x02, 0x05, 0xa0 };
      uint32_t f = 0;
      int rc = der_read_bitflags((der_init(&d, x, sizeof(x)), &d), &f);
      CHECK(rc == 0 && f == 0x05, "keyUsage bit ordering (rc=%d got 0x%x)",
            rc, f); }
    { static const uint8_t x[] = { 0x03, 0x02, 0x05, 0xa9 };
      uint32_t f;
      der_init(&d, x, sizeof(x));
      CHECK(der_read_bitflags(&d, &f) < 0,
            "a BIT STRING with non-zero unused bits"); }
    { /* digitalSignature + keyEncipherment, as a real leaf carries it */
      static const uint8_t x[] = { 0x03, 0x02, 0x05, 0xa0 };
      uint32_t f = 0;
      der_init(&d, x, sizeof(x));
      der_read_bitflags(&d, &f);
      CHECK((f & X509_KU_DIGITAL_SIGNATURE) && (f & X509_KU_KEY_ENCIPHERMENT),
            "keyUsage flag names line up with the bit numbers (0x%x)", f); }
    { static const uint8_t x[] = { 0x03, 0x02, 0x08, 0xff };
      uint32_t f;
      der_init(&d, x, sizeof(x));
      CHECK(der_read_bitflags(&d, &f) < 0, "more than seven unused bits"); }
    { static const uint8_t x[] = { 0x1f, 0x80, 0x01, 0x00 };
      der_init(&d, x, sizeof(x));
      CHECK(der_read(&d, &t) < 0, "a high tag with a leading 0x80"); }

    /* Times */
    { static const uint8_t x[] = "\x17\x0d" "700101000000Z";
      uint32_t u;
      der_init(&d, x, 15);
      CHECK(der_read_time(&d, &u) == 0 && u == 0, "UTCTime at the epoch"); }
    { static const uint8_t x[] = "\x17\x0d" "260726123456Z";
      uint32_t u = 0;
      int rc = (der_init(&d, x, 15), der_read_time(&d, &u));
      CHECK(rc == 0 && u == 1785069296u,
            "UTCTime 2026-07-26 12:34:56 -> rc=%d %u", rc, u); }
    { static const uint8_t x[] = "\x18\x0f" "20260726123456Z";
      uint32_t u = 0;
      int rc = (der_init(&d, x, 17), der_read_time(&d, &u));
      CHECK(rc == 0 && u == 1785069296u,
            "GeneralizedTime same instant -> rc=%d %u", rc, u); }
    { static const uint8_t x[] = "\x17\x0d" "491231235959Z";
      uint32_t u = 0;
      int rc = (der_init(&d, x, 15), der_read_time(&d, &u));
      CHECK(rc == 0 && u == 2524607999u,
            "UTCTime year 49 means 2049 -> rc=%d %u", rc, u); }
    { static const uint8_t x[] = "\x17\x0d" "500101000000Z";
      uint32_t u = 0;
      int rc = (der_init(&d, x, 15), der_read_time(&d, &u));
      CHECK(rc < 0, "UTCTime year 50 means 1950, before the epoch (rc=%d)",
            rc); }
    { static const uint8_t x[] = "\x17\x0d" "260732123456Z";
      uint32_t u;
      der_init(&d, x, 15);
      CHECK(der_read_time(&d, &u) < 0, "day 32"); }
    { static const uint8_t x[] = "\x17\x0d" "260229123456Z";
      uint32_t u;
      der_init(&d, x, 15);
      CHECK(der_read_time(&d, &u) < 0, "29 February in a common year"); }
    { static const uint8_t x[] = "\x18\x0f" "20240229123456Z";
      uint32_t u;
      der_init(&d, x, 17);
      CHECK(der_read_time(&d, &u) == 0, "29 February in a leap year"); }
    { static const uint8_t x[] = "\x17\x0b" "2607261234Z";
      uint32_t u;
      der_init(&d, x, 13);
      CHECK(der_read_time(&d, &u) < 0, "UTCTime without seconds"); }
    { static const uint8_t x[] = "\x17\x0d" "260726123456+";
      uint32_t u;
      der_init(&d, x, 15);
      CHECK(der_read_time(&d, &u) < 0, "a non-Z timezone"); }
    { char buf[16];
      x509_format_date(1785069296u, buf, sizeof(buf));
      CHECK(strcmp(buf, "2026-07-26") == 0, "date formatting gave %s", buf); }
    { char buf[16];
      x509_format_date(0, buf, sizeof(buf));
      CHECK(strcmp(buf, "1970-01-01") == 0, "the epoch formats as %s", buf); }
    { char buf[16];
      x509_format_date(1709164800u, buf, sizeof(buf));
      CHECK(strcmp(buf, "2024-02-29") == 0, "a leap day formats as %s", buf); }

    /* Structural nesting */
    {
        uint8_t deep[512];
        int i;
        for (i = 0; i < 200; i++) {
            deep[2 * i] = 0x30;
            deep[2 * i + 1] = (uint8_t)(2 * (200 - i - 1));
        }
        {
            struct x509_cert c;
            char err[X509_ERR_LEN];
            CHECK(x509_parse(&c, deep, 400, err, sizeof(err)) < 0,
                  "200 levels of nesting must be refused, not recursed");
        }
    }
}

/* ================================================================== */
/* PEM                                                                */
/* ================================================================== */

static void test_pem(void)
{
    struct pem_iter it;
    uint8_t buf[4096];
    char label[80];
    size_t n;
    int rc;

    section("PEM framing");
    {
        static const char *pem =
            "leading noise\n"
            "-----BEGIN CERTIFICATE-----\n"
            "SGVsbG8sIEtlc3RyZWwh\n"
            "-----END CERTIFICATE-----\n"
            "-----BEGIN OTHER-----\n"
            "QUJD\n"
            "-----END OTHER-----\n";
        pem_iter_init(&it, pem, strlen(pem));
        rc = pem_next(&it, label, sizeof(label), buf, sizeof(buf), &n);
        CHECK(rc == 1 && n == 15 && memcmp(buf, "Hello, Kestrel!", 15) == 0 &&
              strcmp(label, "CERTIFICATE") == 0,
              "first block (rc=%d n=%d label=%s)", rc, (int)n, label);
        rc = pem_next(&it, label, sizeof(label), buf, sizeof(buf), &n);
        CHECK(rc == 1 && n == 3 && memcmp(buf, "ABC", 3) == 0 &&
              strcmp(label, "OTHER") == 0, "second block");
        rc = pem_next(&it, label, sizeof(label), buf, sizeof(buf), &n);
        CHECK(rc == 0, "end of input");
    }
    {
        static const char *bad =
            "-----BEGIN CERTIFICATE-----\nSGVsb!8=\n-----END CERTIFICATE-----\n";
        pem_iter_init(&it, bad, strlen(bad));
        CHECK(pem_next(&it, label, sizeof(label), buf, sizeof(buf), &n) < 0,
              "an invalid base64 character is rejected");
    }
    {
        static const char *bad =
            "-----BEGIN CERTIFICATE-----\nQUJD\n";
        pem_iter_init(&it, bad, strlen(bad));
        CHECK(pem_next(&it, label, sizeof(label), buf, sizeof(buf), &n) < 0,
              "a missing END marker is rejected");
    }
    {
        static const char *bad =
            "-----BEGIN CERTIFICATE-----\nQQ==QQ==\n-----END CERTIFICATE-----\n";
        pem_iter_init(&it, bad, strlen(bad));
        CHECK(pem_next(&it, label, sizeof(label), buf, sizeof(buf), &n) < 0,
              "data after the padding is rejected");
    }
    {
        static const char *good =
            "-----BEGIN CERTIFICATE-----\nQ  U\tJ\nD\n-----END CERTIFICATE-----";
        pem_iter_init(&it, good, strlen(good));
        CHECK(pem_next(&it, label, sizeof(label), buf, sizeof(buf), &n) == 1 &&
              n == 3, "whitespace inside the body is ignored");
    }
    {
        static const char *big =
            "-----BEGIN CERTIFICATE-----\nQUJD\n-----END CERTIFICATE-----\n";
        pem_iter_init(&it, big, strlen(big));
        CHECK(pem_next(&it, label, sizeof(label), buf, 1, &n) < 0,
              "an output buffer that is too small is an error, not an overflow");
    }
}

/* ================================================================== */
/* Certificates and chains                                            */
/* ================================================================== */

struct blob {
    uint8_t *p;
    size_t   n;
};

static struct blob load(const char *name)
{
    struct blob b;
    b.p = read_file(name, &b.n);
    if (!b.p)
        b.n = 0;
    return b;
}

static uint32_t g_now;

static void test_certs(void)
{
    struct blob root = load("root.der"), inter = load("int.der");
    struct blob leaf = load("leaf.der");
    struct x509_cert c;
    char err[X509_ERR_LEN];

    section("certificate parsing");
    if (!leaf.p || !inter.p || !root.p) {
        CHECK(0, "the certificate fixtures are missing");
        return;
    }
    CHECK(x509_parse(&c, leaf.p, leaf.n, err, sizeof(err)) == 0,
          "leaf did not parse: %s", err);
    CHECK(c.version == 3, "leaf version is %d", c.version);
    CHECK(strcmp(c.subject_cn, "example.test") == 0,
          "leaf subject CN is '%s'", c.subject_cn);
    CHECK(strcmp(c.issuer_cn, "Kestrel Test Intermediate") == 0,
          "leaf issuer CN is '%s'", c.issuer_cn);
    CHECK(c.sig_alg == X509_SIG_RSA_PKCS1 && c.sig_md == CRYPTO_MD_SHA256,
          "leaf signature algorithm (%d/%d)", c.sig_alg, c.sig_md);
    CHECK(c.pk_alg == X509_PK_RSA, "leaf key algorithm is %d", c.pk_alg);
    CHECK(c.n_san == 3, "leaf has %d SAN entries, expected 3", c.n_san);
    CHECK(c.has_basic_constraints && !c.is_ca, "leaf is marked CA:FALSE");
    CHECK(c.has_key_usage &&
          (c.key_usage & X509_KU_DIGITAL_SIGNATURE) != 0,
          "leaf keyUsage 0x%x", c.key_usage);
    CHECK(c.has_eku && (c.eku & X509_EKU_SERVER_AUTH) != 0,
          "leaf extKeyUsage 0x%x", c.eku);
    CHECK(!c.self_signed, "leaf must not look self-signed");
    CHECK(c.not_after > c.not_before, "leaf validity ordering");

    CHECK(x509_parse(&c, inter.p, inter.n, err, sizeof(err)) == 0,
          "intermediate did not parse: %s", err);
    CHECK(c.has_basic_constraints && c.is_ca && c.path_len == 0,
          "intermediate CA=%d pathlen=%d", c.is_ca, c.path_len);

    CHECK(x509_parse(&c, root.p, root.n, err, sizeof(err)) == 0,
          "root did not parse: %s", err);
    CHECK(c.self_signed, "root must be self-signed");
    CHECK(c.is_ca && c.path_len == -1, "root CA=%d pathlen=%d", c.is_ca,
          c.path_len);

    /* truncation must never be fatal */
    {
        size_t i;
        for (i = 1; i < leaf.n; i += 7) {
            struct x509_cert t;
            if (x509_parse(&t, leaf.p, i, 0, 0) == 0 && i != leaf.n)
                CHECK(0, "a %d-byte prefix of the leaf parsed as complete",
                      (int)i);
        }
        CHECK(1, "truncation");
    }
    free(root.p); free(inter.p); free(leaf.p);
}

static void test_hostname(void)
{
    struct blob leaf = load("leaf.der");
    struct x509_cert c;

    section("hostname matching");
    if (!leaf.p || x509_parse(&c, leaf.p, leaf.n, 0, 0) < 0) {
        CHECK(0, "leaf.der is missing");
        free(leaf.p);
        return;
    }
    CHECK(x509_check_hostname(&c, "example.test") == 1, "exact SAN");
    CHECK(x509_check_hostname(&c, "EXAMPLE.TEST") == 1, "case insensitive");
    CHECK(x509_check_hostname(&c, "example.test.") == 1, "trailing dot");
    CHECK(x509_check_hostname(&c, "a.wild.test") == 1, "wildcard one label");
    CHECK(x509_check_hostname(&c, "A.WILD.TEST") == 1, "wildcard, upper case");
    CHECK(x509_check_hostname(&c, "wild.test") == 0,
          "a wildcard must not match the bare domain");
    CHECK(x509_check_hostname(&c, "a.b.wild.test") == 0,
          "a wildcard must not match across a dot");
    CHECK(x509_check_hostname(&c, ".wild.test") == 0, "an empty first label");
    CHECK(x509_check_hostname(&c, "xexample.test") == 0, "prefix confusion");
    CHECK(x509_check_hostname(&c, "example.test.evil.com") == 0,
          "suffix confusion");
    CHECK(x509_check_hostname(&c, "other.test") == 0, "an unrelated name");
    CHECK(x509_check_hostname(&c, "10.1.2.3") == 1, "an IP SAN");
    CHECK(x509_check_hostname(&c, "10.1.2.4") == 0, "the wrong IP");
    CHECK(x509_check_hostname(&c, "010.1.2.3") == 1,
          "a zero-padded octet is still the same address");
    CHECK(x509_check_hostname(&c, "") == 0, "the empty hostname");
    CHECK(x509_check_hostname(&c, "*.wild.test") == 0,
          "a wildcard in the *request* must not match");
    free(leaf.p);
}

/* Build a chain array from named DER files, parse each, and verify. */
static int verify_files(struct x509_store *roots, const char *hostname,
                        uint32_t now, char *err, int errlen,
                        const char *f0, const char *f1, const char *f2)
{
    struct x509_cert chain[3];
    struct blob b[3];
    const char *names[3];
    int n = 0, i, rc;

    names[0] = f0; names[1] = f1; names[2] = f2;
    for (i = 0; i < 3; i++) {
        b[i].p = 0;
        b[i].n = 0;
    }
    for (i = 0; i < 3 && names[i]; i++) {
        b[i] = load(names[i]);
        if (!b[i].p) {
            snprintf(err, (size_t)errlen, "fixture %s is missing", names[i]);
            for (i = 0; i < 3; i++) free(b[i].p);
            return -2;
        }
        if (x509_parse(&chain[n], b[i].p, b[i].n, err, (size_t)errlen) < 0) {
            for (i = 0; i < 3; i++) free(b[i].p);
            return -1;
        }
        n++;
    }
    rc = x509_verify_chain(chain, n, roots, hostname, now, err, errlen);
    for (i = 0; i < 3; i++)
        free(b[i].p);
    return rc;
}

static void test_chains(void)
{
    struct x509_store *store = malloc(sizeof(*store));
    struct x509_store *empty = malloc(sizeof(*empty));
    struct blob root = load("root.der"), ecroot = load("ecroot.der");
    struct blob weak = load("weakroot.der");
    char err[X509_ERR_LEN];
    int rc;

    section("chain verification");
    x509_store_init(store);
    x509_store_init(empty);
    if (!root.p || !ecroot.p) {
        CHECK(0, "root fixtures are missing");
        goto done;
    }
    CHECK(x509_store_add_der(store, root.p, root.n) == 0, "add the RSA root");
    CHECK(x509_store_add_der(store, ecroot.p, ecroot.n) == 0, "add the EC root");
    if (weak.p)
        x509_store_add_der(store, weak.p, weak.n);

    rc = verify_files(store, "example.test", g_now, err, sizeof(err),
                      "leaf.der", "int.der", 0);
    CHECK(rc == 1, "leaf -> intermediate -> root should verify: %s", err);

    rc = verify_files(store, "a.wild.test", g_now, err, sizeof(err),
                      "leaf.der", "int.der", 0);
    CHECK(rc == 1, "the wildcard name should verify: %s", err);

    rc = verify_files(store, "ec.example.test", g_now, err, sizeof(err),
                      "ecleaf.der", 0, 0);
    CHECK(rc == 1, "the P-256 ECDSA chain should verify: %s", err);

    rc = verify_files(store, "ec.example.test", g_now, err, sizeof(err),
                      "ecmixed.der", "int.der", 0);
    CHECK(rc == 1, "an EC leaf under an RSA CA should verify: %s", err);

    rc = verify_files(store, "example.test", g_now, err, sizeof(err),
                      "leaf384.der", "int.der", 0);
    CHECK(rc == 1, "a SHA-384 signature should verify: %s", err);

    rc = verify_files(store, "example.test", g_now, err, sizeof(err),
                      "pssleaf.der", "int.der", 0);
    if (rc == -2)
        printf("  note: no PSS fixture, skipping\n");
    else
        CHECK(rc == 1, "an RSASSA-PSS signature should verify: %s", err);

    /* ---- everything below must be rejected, with the right reason ---- */

    rc = verify_files(store, "wrong.test", g_now, err, sizeof(err),
                      "leaf.der", "int.der", 0);
    CHECK(rc == 0 && str_has(err, "not 'wrong.test'"),
          "wrong hostname -> '%s'", err);

    rc = verify_files(store, "other.test", g_now, err, sizeof(err),
                      "other.der", "int.der", 0);
    CHECK(rc == 1, "the other.test certificate is valid for its own name: %s",
          err);
    rc = verify_files(store, "example.test", g_now, err, sizeof(err),
                      "other.der", "int.der", 0);
    CHECK(rc == 0 && str_has(err, "not 'example.test'"),
          "a certificate for another name -> '%s'", err);

    rc = verify_files(store, "example.test", g_now, err, sizeof(err),
                      "expired.der", "int.der", 0);
    CHECK(rc == 0 && str_has(err, "expired on 20"),
          "an expired certificate -> '%s'", err);

    rc = verify_files(store, "example.test", g_now, err, sizeof(err),
                      "future.der", "int.der", 0);
    CHECK(rc == 0 && str_has(err, "not valid until"),
          "a not-yet-valid certificate -> '%s'", err);

    rc = verify_files(store, "example.test", g_now, err, sizeof(err),
                      "under_nonca.der", "nonca.der", 0);
    CHECK(rc == 0 && str_has(err, "not marked as a CA"),
          "a non-CA intermediate -> '%s'", err);

    rc = verify_files(store, "example.test", g_now, err, sizeof(err),
                      "leaf2.der", "int2.der", "int.der");
    CHECK(rc == 0 && str_has(err, "intermediate"),
          "a path length violation -> '%s'", err);

    rc = verify_files(store, "example.test", g_now, err, sizeof(err),
                      "weakleaf.der", "weakroot.der", 0);
    CHECK(rc == 0 && str_has(err, "below the 2048 bit minimum"),
          "a 1024-bit issuer key -> '%s'", err);

    rc = verify_files(store, "example.test", g_now, err, sizeof(err),
                      "leaf.der", 0, 0);
    CHECK(rc == 0 && str_has(err, "no trusted root"),
          "a missing intermediate -> '%s'", err);

    rc = verify_files(empty, "example.test", g_now, err, sizeof(err),
                      "leaf.der", "int.der", 0);
    CHECK(rc == 0 && str_has(err, "no trusted root certificates are loaded"),
          "an empty trust store -> '%s'", err);

    /* The wrong issuer: leaf under the intermediate but with the EC root
     * offered as the parent. */
    rc = verify_files(store, "example.test", g_now, err, sizeof(err),
                      "leaf.der", "ecroot.der", 0);
    CHECK(rc == 0 && str_has(err, "not the subject of the next"),
          "a mismatched issuer name -> '%s'", err);

    /* Dates: verifying the good chain far in the past and far in the
     * future must fail on validity, not on anything else. */
    rc = verify_files(store, "example.test", 100000000u, err, sizeof(err),
                      "leaf.der", "int.der", 0);
    CHECK(rc == 0 && str_has(err, "not valid until"),
          "verification in 1973 -> '%s'", err);
    rc = verify_files(store, "example.test", 4000000000u, err, sizeof(err),
                      "leaf.der", "int.der", 0);
    CHECK(rc == 0 && str_has(err, "expired on"),
          "verification in 2096 -> '%s'", err);

    /* Corrupt one bit of the signature in every position we can afford. */
    {
        struct blob leaf = load("leaf.der"), inter = load("int.der");
        struct x509_cert chain[2];
        int i, accepted = 0, tried = 0;
        if (leaf.p && inter.p &&
            x509_parse(&chain[0], leaf.p, leaf.n, 0, 0) == 0 &&
            x509_parse(&chain[1], inter.p, inter.n, 0, 0) == 0) {
            uint8_t *sig = (uint8_t *)chain[0].sig;   /* points into leaf.p */
            for (i = 0; i < (int)chain[0].sig_len * 8; i += 11) {
                sig[i / 8] ^= (uint8_t)(1 << (i % 8));
                tried++;
                if (x509_verify_chain(chain, 2, store, "example.test", g_now,
                                      err, sizeof(err)) == 1)
                    accepted++;
                sig[i / 8] ^= (uint8_t)(1 << (i % 8));
            }
            CHECK(accepted == 0,
                  "%d of %d single-bit signature corruptions still verified",
                  accepted, tried);
            /* and the same for the signed body */
            accepted = 0; tried = 0;
            for (i = 0; i < (int)chain[0].tbs_len * 8; i += 419) {
                uint8_t *tbs = (uint8_t *)chain[0].tbs;
                tbs[i / 8] ^= (uint8_t)(1 << (i % 8));
                tried++;
                if (x509_verify_chain(chain, 2, store, "example.test", g_now,
                                      err, sizeof(err)) == 1)
                    accepted++;
                tbs[i / 8] ^= (uint8_t)(1 << (i % 8));
            }
            CHECK(accepted == 0,
                  "%d of %d single-bit body corruptions still verified",
                  accepted, tried);
            printf("  every single-bit corruption of the signature and body "
                   "was rejected\n");
        }
        free(leaf.p); free(inter.p);
    }

    rc = verify_files(store, "example.test", g_now, err, sizeof(err),
                      "critleaf.der", "int.der", 0);
    CHECK(rc == 0 && str_has(err, "critical extension") &&
          str_has(err, "1.2.3.4.5.6.7.8"),
          "an unknown critical extension -> '%s'", err);

    /* The same, with an OID whose dotted form is longer than the buffer
     * the error message formats it into. */
    rc = verify_files(store, "example.test", g_now, err, sizeof(err),
                      "longoid.der", "int.der", 0);
    CHECK(rc == 0 && str_has(err, "critical extension"),
          "an over-long critical extension OID -> '%s'", err);

    rc = verify_files(store, "example.test", g_now, err, sizeof(err),
                      "clientonly.der", "int.der", 0);
    CHECK(rc == 0 && str_has(err, "not permitted to authenticate a TLS "
                                  "server"),
          "a client-auth-only extKeyUsage -> '%s'", err);

    /* Structural tampering that the parser itself must catch. */
    {
        struct blob leaf = load("leaf.der");
        struct x509_cert c;
        if (leaf.p && x509_parse(&c, leaf.p, leaf.n, err, sizeof(err)) == 0) {
            size_t alg_at = (size_t)(c.tbs - leaf.p) + c.tbs_len;
            /* The outer signatureAlgorithm must byte-match the copy inside
             * the signed body; say sha384WithRSA outside and sha256WithRSA
             * inside and the certificate is refused before any signature
             * maths happens. The last OID byte is what distinguishes them. */
            CHECK(leaf.p[alg_at + 12] == 0x0b,
                  "expected sha256WithRSAEncryption at the outer algorithm");
            leaf.p[alg_at + 12] = 0x0c;
            CHECK(x509_parse(&c, leaf.p, leaf.n, err, sizeof(err)) < 0 &&
                  str_has(err, "signature algorithm differs"),
                  "a mismatched outer signature algorithm -> '%s'", err);
            leaf.p[alg_at + 12] = 0x0b;
        }
        /* An embedded NUL inside a dNSName must reject the whole
         * certificate, not silently truncate the name. */
        if (leaf.p) {
            uint8_t *at = (uint8_t *)memmem(leaf.p, leaf.n, "*.wild.test", 11);
            if (at) {
                at[3] = 0x00;
                CHECK(x509_parse(&c, leaf.p, leaf.n, err, sizeof(err)) < 0 &&
                      str_has(err, "extension"),
                      "a NUL inside a dNSName -> '%s'", err);
                at[3] = 'i';
            } else {
                CHECK(0, "could not find the wildcard SAN to tamper with");
            }
        }
        free(leaf.p);
    }

    /* An over-long chain is refused before anything is verified. */
    {
        struct blob leaf = load("leaf.der");
        struct x509_cert big[X509_MAX_CHAIN + 1];
        int i;
        if (leaf.p && x509_parse(&big[0], leaf.p, leaf.n, 0, 0) == 0) {
            for (i = 1; i <= X509_MAX_CHAIN; i++)
                big[i] = big[0];
            CHECK(x509_verify_chain(big, X509_MAX_CHAIN + 1, store,
                                    "example.test", g_now, err,
                                    sizeof(err)) == 0 &&
                  str_has(err, "more than the"),
                  "an over-long chain -> '%s'", err);
        }
        free(leaf.p);
    }

    /* The PEM path into the store. */
    {
        struct x509_store *ps = malloc(sizeof(*ps));
        size_t n;
        uint8_t *pem = read_file("roots.pem", &n);
        x509_store_init(ps);
        if (pem) {
            int added = x509_store_add_pem(ps, (const char *)pem, n);
            CHECK(added == 3, "loaded %d roots from PEM, expected 3", added);
            rc = verify_files(ps, "example.test", g_now, err, sizeof(err),
                              "leaf.der", "int.der", 0);
            CHECK(rc == 1, "a PEM-loaded store verifies the chain: %s", err);
            free(pem);
        } else {
            CHECK(0, "roots.pem is missing");
        }
        x509_store_free(ps);
        free(ps);
    }

done:
    x509_store_free(store);
    x509_store_free(empty);
    free(store);
    free(empty);
    free(root.p);
    free(ecroot.p);
    free(weak.p);
}

/* ================================================================== */
/* Fuzzing                                                            */
/* ================================================================== */

static uint64_t rng_state = 0x243f6a8885a308d3ULL;

static uint64_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static void test_fuzz(void)
{
    struct blob leaf = load("leaf.der"), inter = load("int.der");
    struct blob root = load("root.der");
    struct x509_store *store = malloc(sizeof(*store));
    uint8_t *copy;
    int i, parsed = 0, verified = 0;
    const int rounds = 20000;

    section("DER fuzzing");
    x509_store_init(store);
    if (!leaf.p || !inter.p || !root.p) {
        CHECK(0, "fixtures are missing");
        goto done;
    }
    x509_store_add_der(store, root.p, root.n);
    copy = malloc(leaf.n + 64);

    for (i = 0; i < rounds; i++) {
        struct x509_cert c, chain[2];
        char err[X509_ERR_LEN];
        size_t len = leaf.n;
        int muts, j;

        memcpy(copy, leaf.p, leaf.n);
        muts = 1 + (int)(rng() % 8);
        for (j = 0; j < muts; j++) {
            size_t at = (size_t)(rng() % len);
            switch (rng() % 4) {
            case 0: copy[at] = (uint8_t)rng(); break;
            case 1: copy[at] ^= (uint8_t)(1 << (rng() % 8)); break;
            case 2: copy[at] = 0xff; break;
            default: copy[at] = 0x00; break;
            }
        }
        if (rng() % 8 == 0)
            len = 1 + (size_t)(rng() % leaf.n);      /* truncate too */
        /* A mutation that happens to write back the same byte -- setting
         * an already-zero byte to zero, say -- leaves a genuine
         * certificate behind, which of course still verifies. Skip those
         * so the "nothing forged verifies" assertion means something. */
        if (len == leaf.n && memcmp(copy, leaf.p, len) == 0)
            continue;

        if (x509_parse(&c, copy, len, err, sizeof(err)) == 0) {
            parsed++;
            /* Anything that parses must also survive the whole verifier. */
            if (x509_parse(&chain[0], copy, len, 0, 0) == 0 &&
                x509_parse(&chain[1], inter.p, inter.n, 0, 0) == 0) {
                if (x509_verify_chain(chain, 2, store, "example.test",
                                      g_now, err, sizeof(err)) == 1)
                    verified++;
            }
            x509_check_hostname(&c, "example.test");
            x509_check_hostname(&c, "*.a.b.c.d");
        }
    }
    CHECK(verified == 0,
          "%d mutated certificates verified against a real root", verified);
    printf("  %d mutated certificates: %d still parsed, none verified\n",
           rounds, parsed);

    /* Random bytes straight into the parser. */
    {
        uint8_t buf[512];
        int n;
        for (i = 0; i < 20000; i++) {
            struct x509_cert c;
            size_t len = 1 + (size_t)(rng() % sizeof(buf));
            for (n = 0; n < (int)len; n++)
                buf[n] = (uint8_t)rng();
            x509_parse(&c, buf, len, 0, 0);
        }
        CHECK(1, "20000 random buffers parsed without a crash");
    }

    /* Random bytes into the DER primitives. */
    {
        uint8_t buf[64];
        for (i = 0; i < 40000; i++) {
            struct der d;
            struct der_tlv t;
            uint32_t v;
            const uint8_t *p;
            size_t l, len = 1 + (size_t)(rng() % sizeof(buf));
            int n2;
            for (n2 = 0; n2 < (int)len; n2++)
                buf[n2] = (uint8_t)rng();
            der_init(&d, buf, len);
            while (der_read(&d, &t) == 0)
                ;
            der_init(&d, buf, len); der_read_u32(&d, &v);
            der_init(&d, buf, len); der_read_oid(&d, &p, &l);
            der_init(&d, buf, len); der_read_bitstring(&d, &p, &l);
            der_init(&d, buf, len); der_read_bitflags(&d, &v);
            der_init(&d, buf, len); der_read_time(&d, &v);
        }
        CHECK(1, "40000 random buffers through the DER primitives");
    }

    /* Random bytes into the PEM reader. */
    {
        char text[256];
        uint8_t out[256];
        char label[64];
        size_t n2;
        for (i = 0; i < 20000; i++) {
            struct pem_iter it;
            size_t len = 1 + (size_t)(rng() % (sizeof(text) - 1)), j;
            const char *alpha =
                "-BEGINCERTIFICATE\nAB=+/ \tEND";
            for (j = 0; j < len; j++)
                text[j] = alpha[rng() % strlen(alpha)];
            text[len] = 0;
            pem_iter_init(&it, text, len);
            while (pem_next(&it, label, sizeof(label), out, sizeof(out),
                            &n2) == 1)
                ;
        }
        CHECK(1, "20000 random PEM-ish buffers");
    }
    free(copy);
done:
    x509_store_free(store);
    free(store);
    free(leaf.p); free(inter.p); free(root.p);
}

/* ================================================================== */
/* Timing                                                             */
/* ================================================================== */

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void bench(void)
{
    struct blob leaf = load("leaf.der"), inter = load("int.der");
    struct x509_cert lc, ic;
    uint8_t *pub;
    size_t pub_len;
    struct rsa_pubkey key;
    double t0, t1;
    int i, n;

    section("timing (this host, -O2)");
    pub = read_file("rsapub.der", &pub_len);
    if (pub && rsa_pubkey_from_der(&key, pub, pub_len) == 0) {
        size_t sl = 0, ml = 0;
        uint8_t *sig = read_file("sig_pkcs1_sha256.bin", &sl);
        uint8_t *msg = read_file("msg.bin", &ml);
        uint8_t dg[32];
        if (sig && msg) {
            sha256(msg, ml, dg);
            n = 200;
            t0 = now_sec();
            for (i = 0; i < n; i++)
                rsa_verify_pkcs1_v15(&key, CRYPTO_MD_SHA256, dg, 32, sig,
                                     (size_t)key.k);
            t1 = now_sec();
            printf("  2048-bit RSA verify (e=65537): %.3f ms each "
                   "(%d runs)\n", (t1 - t0) * 1000.0 / n, n);
        }
        free(sig);
        free(msg);
    }
    free(pub);

    /* A full-width modexp: 2048-bit base and exponent, 2048-bit modulus. */
    {
        struct bn m, b, e, r;
        struct bn_mont mt;
        uint8_t buf[256];
        for (i = 0; i < 256; i++)
            buf[i] = (uint8_t)(rng() | (i == 0 ? 0x80 : 0));
        buf[255] |= 1;
        bn_from_bytes(&m, buf, 256);
        for (i = 0; i < 256; i++)
            buf[i] = (uint8_t)rng();
        bn_from_bytes(&b, buf, 256);
        for (i = 0; i < 256; i++)
            buf[i] = (uint8_t)rng();
        bn_from_bytes(&e, buf, 256);
        if (bn_mont_init(&mt, &m) == 0) {
            n = 10;
            t0 = now_sec();
            for (i = 0; i < n; i++)
                bn_modexp(&r, &b, &e, &mt);
            t1 = now_sec();
            printf("  2048-bit modexp, full 2048-bit exponent: %.1f ms each\n",
                   (t1 - t0) * 1000.0 / n);
        }
    }
    /* And a 4096-bit one, the worst case this build accepts. */
    {
        struct bn m, b, e, r;
        struct bn_mont mt;
        uint8_t buf[512];
        for (i = 0; i < 512; i++)
            buf[i] = (uint8_t)(rng() | (i == 0 ? 0x80 : 0));
        buf[511] |= 1;
        bn_from_bytes(&m, buf, 512);
        for (i = 0; i < 512; i++)
            buf[i] = (uint8_t)rng();
        bn_from_bytes(&b, buf, 512);
        bn_set_u64(&e, 65537);
        if (bn_mont_init(&mt, &m) == 0) {
            n = 50;
            t0 = now_sec();
            for (i = 0; i < n; i++)
                bn_modexp(&r, &b, &e, &mt);
            t1 = now_sec();
            printf("  4096-bit modexp, e=65537: %.3f ms each\n",
                   (t1 - t0) * 1000.0 / n);
        }
    }

    /* X25519 and P-256, the handshake operations. */
    {
        uint8_t k[32], u[32], o[32];
        for (i = 0; i < 32; i++) { k[i] = (uint8_t)rng(); u[i] = (uint8_t)rng(); }
        u[31] &= 0x7f;
        n = 200;
        t0 = now_sec();
        for (i = 0; i < n; i++)
            x25519(o, k, u);
        t1 = now_sec();
        printf("  X25519 scalar multiplication: %.3f ms each\n",
               (t1 - t0) * 1000.0 / n);
    }
    {
        struct p256_point pt;
        uint8_t d[32];
        for (i = 0; i < 32; i++) d[i] = (uint8_t)rng();
        d[0] &= 0x7f;
        n = 100;
        t0 = now_sec();
        for (i = 0; i < n; i++)
            p256_base_mul(&pt, d);
        t1 = now_sec();
        printf("  P-256 base point multiplication: %.3f ms each\n",
               (t1 - t0) * 1000.0 / n);
    }
    if (leaf.p && inter.p &&
        x509_parse(&lc, leaf.p, leaf.n, 0, 0) == 0 &&
        x509_parse(&ic, inter.p, inter.n, 0, 0) == 0) {
        char err[X509_ERR_LEN];
        n = 200;
        t0 = now_sec();
        for (i = 0; i < n; i++)
            x509_verify_signature(&lc, &ic, err, sizeof(err));
        t1 = now_sec();
        printf("  one certificate signature check (parse key + verify): "
               "%.3f ms\n", (t1 - t0) * 1000.0 / n);
        n = 2000;
        t0 = now_sec();
        for (i = 0; i < n; i++)
            x509_parse(&lc, leaf.p, leaf.n, 0, 0);
        t1 = now_sec();
        printf("  certificate parse: %.4f ms\n", (t1 - t0) * 1000.0 / n);
    }
    free(leaf.p);
    free(inter.p);
}

/* ================================================================== */

int main(int argc, char **argv)
{
    size_t n;
    uint8_t *nowtxt;

    snprintf(g_dir, sizeof(g_dir), "%s",
             argc > 1 ? argv[1] : "build/cryptoasym-fixtures");
    {
        char cmd[600];
        snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", g_dir);
        if (system(cmd) != 0)
            printf("could not create %s\n", g_dir);
    }

    crypto_md_clear();
    if (crypto_md_register(&md_sha1) < 0 ||
        crypto_md_register(&md_sha256) < 0 ||
        crypto_md_register(&md_sha384) < 0 ||
        crypto_md_register(&md_sha512) < 0) {
        printf("digest registration failed\n");
        return 1;
    }
    /* the registry must reject a duplicate id and accept a repeat */
    {
        static const struct crypto_md dup = { CRYPTO_MD_SHA256, "dup", 32,
                                              sha256 };
        section("digest registry");
        CHECK(crypto_md_register(&dup) < 0, "a duplicate id is refused");
        CHECK(crypto_md_register(&md_sha256) == 0, "re-registering is a no-op");
        CHECK(crypto_md_get(CRYPTO_MD_SHA256) == &md_sha256, "lookup by id");
        CHECK(crypto_md_get(99) == 0, "an unknown id returns NULL");
        {
            uint8_t d[32];
            static const uint8_t want[32] = {
                0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
                0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
                0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
                0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
            };
            sha256("abc", 3, d);
            CHECK(memcmp(d, want, 32) == 0,
                  "the harness's own SHA-256 matches FIPS 180-4");
        }
    }

    printf("generating fixtures in %s (openssl + python3)...\n", g_dir);
    if (generate_fixtures() < 0)
        return 1;
    nowtxt = read_file("now.txt", &n);
    g_now = nowtxt ? (uint32_t)strtoul((char *)nowtxt, 0, 10) : 1784000000u;
    free(nowtxt);

    test_bignum();
    test_x25519();
    test_p256();
    test_ecdsa();
    test_rsa();
    test_der();
    test_pem();
    test_certs();
    test_hostname();
    test_chains();
    test_fuzz();
    bench();

    printf("\n================================\n");
    printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
