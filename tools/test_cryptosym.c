/* Host test harness for libtls/hash.c and libtls/aead.c.
 *
 * Everything in those two files is pure computation with no I/O, so it can
 * be compiled for the host and checked exhaustively without booting the
 * OS. Build and run:
 *
 *   gcc -Wall -Wextra -O2 -Ilibtls -o /tmp/tcs \
 *       libtls/hash.c libtls/aead.c tools/test_cryptosym.c && /tmp/tcs
 *
 * Add -fsanitize=address,undefined for the memory-safety pass, and
 * -DHAVE_LIBC_SHA256 plus a separately compiled libc/sha256.o to include
 * the cross-check that libtls's SHA-256 agrees with libc's.
 *
 * Every expected value in the "published vectors" sections below is a
 * literal from FIPS 180-4, FIPS 197, RFC 2104/4231, RFC 5869, RFC 8439,
 * RFC 8446/8448 or the NIST GCM specification, and each one was
 * independently confirmed against a separate implementation before being
 * written here -- transcription errors are otherwise indistinguishable
 * from implementation errors.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <hash.h>
#include <aead.h>

#ifdef HAVE_LIBC_SHA256
/* Declared by hand rather than by including libc/include/sha256.h, whose
 * directory also holds a stdio.h that would shadow the host's. */
void sha256_hex(const void *data, unsigned long len, char out[65]);
#endif

static unsigned long checks;
static unsigned long failures;
static const char *section = "";

static void begin(const char *name)
{
    section = name;
    printf("\n== %s ==\n", name);
}

static int unhex1(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse a hex string, ignoring spaces. Returns the byte count. */
static unsigned long unhex(const char *s, uint8_t *out, unsigned long max)
{
    unsigned long n = 0;
    int hi = -1;

    while (*s) {
        int v;

        if (*s == ' ' || *s == '\n') { s++; continue; }
        v = unhex1(*s++);
        if (v < 0) {
            printf("  BAD HEX in %s\n", section);
            exit(2);
        }
        if (hi < 0) {
            hi = v;
        } else {
            if (n >= max) { printf("  HEX TOO LONG in %s\n", section); exit(2); }
            out[n++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }
    if (hi >= 0) { printf("  ODD HEX in %s\n", section); exit(2); }
    return n;
}

static void printhex(const uint8_t *p, unsigned long n)
{
    unsigned long i;

    for (i = 0; i < n; i++)
        printf("%02x", p[i]);
}

/* Compare a produced buffer against an expected hex string. */
static int chk(const char *name, const uint8_t *got, unsigned long gotlen,
               const char *want)
{
    static uint8_t exp[8192];
    unsigned long explen = unhex(want, exp, sizeof(exp));

    checks++;
    if (gotlen != explen || memcmp(got, exp, gotlen) != 0) {
        failures++;
        printf("  FAIL %s\n", name);
        printf("       got  (%lu) ", gotlen); printhex(got, gotlen); printf("\n");
        printf("       want (%lu) ", explen); printhex(exp, explen); printf("\n");
        return 0;
    }
    printf("  ok   %s\n", name);
    return 1;
}

/* Boolean assertion, for tests whose answer is a yes or a no. */
static int chk_bool(const char *name, int cond)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL %s\n", name);
        return 0;
    }
    printf("  ok   %s\n", name);
    return 1;
}

/* Quiet form, for the loops that run thousands of iterations. */
static int quiet_ok = 1;

static void q(int cond)
{
    if (!cond)
        quiet_ok = 0;
}

/* Deterministic PRNG, so a failure is always reproducible. */
static uint64_t rng = 0x9e3779b97f4a7c15ULL;

static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng >> 32);
}

static void rndbuf(uint8_t *p, unsigned long n)
{
    unsigned long i;

    for (i = 0; i < n; i++)
        p[i] = (uint8_t)rnd();
}

/* ================= 1. SHA-2 published vectors ================= */

/* The four classic FIPS 180-4 messages: the empty string, "abc", the
 * 448-bit message and one million 'a'. */
static const char *M448 =
    "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
static const char *M896 =
    "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
    "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";

static void digest_of_million_a(int alg, uint8_t *out)
{
    struct hash_ctx c;
    static uint8_t chunk[1000];
    int i;

    memset(chunk, 'a', sizeof(chunk));
    hash_init(&c, alg);
    for (i = 0; i < 1000; i++)
        hash_update(&c, chunk, sizeof(chunk));
    hash_final(&c, out);
}

static void test_sha_vectors(void)
{
    uint8_t d[HASH_MAX_DIGEST];

    begin("SHA-2 published vectors (FIPS 180-4)");

    hash_oneshot(HASH_SHA256, "", 0, d);
    chk("SHA-256(\"\")", d, 32,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    hash_oneshot(HASH_SHA256, "abc", 3, d);
    chk("SHA-256(\"abc\")", d, 32,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    hash_oneshot(HASH_SHA256, M448, strlen(M448), d);
    chk("SHA-256(448-bit msg)", d, 32,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    digest_of_million_a(HASH_SHA256, d);
    chk("SHA-256(1e6 x 'a')", d, 32,
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    hash_oneshot(HASH_SHA384, "", 0, d);
    chk("SHA-384(\"\")", d, 48,
        "38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da"
        "274edebfe76f65fbd51ad2f14898b95b");
    hash_oneshot(HASH_SHA384, "abc", 3, d);
    chk("SHA-384(\"abc\")", d, 48,
        "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
        "8086072ba1e7cc2358baeca134c825a7");
    hash_oneshot(HASH_SHA384, M448, strlen(M448), d);
    chk("SHA-384(448-bit msg)", d, 48,
        "3391fdddfc8dc7393707a65b1b4709397cf8b1d162af05abfe8f450de5f36bc6"
        "b0455a8520bc4e6f5fe95b1fe3c8452b");
    hash_oneshot(HASH_SHA384, M896, strlen(M896), d);
    chk("SHA-384(896-bit msg)", d, 48,
        "09330c33f71147e83d192fc782cd1b4753111b173b3b05d22fa08086e3b0f712"
        "fcc7c71a557e2db966c3e9fa91746039");
    digest_of_million_a(HASH_SHA384, d);
    chk("SHA-384(1e6 x 'a')", d, 48,
        "9d0e1809716474cb086e834e310a4a1ced149e9c00f248527972cec5704c2a5b"
        "07b8b3dc38ecc4ebae97ddd87f3d8985");

    hash_oneshot(HASH_SHA512, "", 0, d);
    chk("SHA-512(\"\")", d, 64,
        "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
        "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
    hash_oneshot(HASH_SHA512, "abc", 3, d);
    chk("SHA-512(\"abc\")", d, 64,
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    hash_oneshot(HASH_SHA512, M448, strlen(M448), d);
    chk("SHA-512(448-bit msg)", d, 64,
        "204a8fc6dda82f0a0ced7beb8e08a41657c16ef468b228a8279be331a703c335"
        "96fd15c13b1b07f9aa1d3bea57789ca031ad85c7a71dd70354ec631238ca3445");
    hash_oneshot(HASH_SHA512, M896, strlen(M896), d);
    chk("SHA-512(896-bit msg)", d, 64,
        "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
        "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909");
    digest_of_million_a(HASH_SHA512, d);
    chk("SHA-512(1e6 x 'a')", d, 64,
        "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973eb"
        "de0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b");
}

/* ================= 2. SHA-2 streaming behaviour ================= */

static void test_sha_streaming(void)
{
    static uint8_t msg[4096];
    uint8_t one[HASH_MAX_DIGEST], many[HASH_MAX_DIGEST];
    int algs[3] = { HASH_SHA256, HASH_SHA384, HASH_SHA512 };
    int a;
    unsigned long trial;

    begin("SHA-2 streaming, block boundaries and forking");

    /* Split the same message at every possible offset and at random
     * offsets; the digest must not depend on how it was fed. This is what
     * catches an off-by-one in the partial-block path, which is exactly
     * where the padding rule bites. */
    quiet_ok = 1;
    rndbuf(msg, sizeof(msg));
    for (a = 0; a < 3; a++) {
        unsigned long len;

        /* Every length across three block boundaries, split in two. */
        for (len = 0; len <= 300; len++) {
            unsigned long split;

            hash_oneshot(algs[a], msg, len, one);
            for (split = 0; split <= len; split++) {
                struct hash_ctx c;

                hash_init(&c, algs[a]);
                hash_update(&c, msg, split);
                hash_update(&c, msg + split, len - split);
                hash_final(&c, many);
                q(memcmp(one, many, hash_digest_len(algs[a])) == 0);
            }
        }
        /* Random many-way splits of a longer message. */
        for (trial = 0; trial < 200; trial++) {
            struct hash_ctx c;
            unsigned long off = 0;

            hash_oneshot(algs[a], msg, sizeof(msg), one);
            hash_init(&c, algs[a]);
            while (off < sizeof(msg)) {
                unsigned long take = rnd() % 200 + 1;

                if (take > sizeof(msg) - off)
                    take = sizeof(msg) - off;
                hash_update(&c, msg + off, take);
                off += take;
            }
            hash_final(&c, many);
            q(memcmp(one, many, hash_digest_len(algs[a])) == 0);
        }
    }
    chk_bool("chunked update == one-shot (all splits 0..300, 3 algs)", quiet_ok);

    /* Lengths straddling the point where the length field no longer fits
     * in the final block: 55/56/57 for SHA-256, 111/112/113 for SHA-512.
     * Covered by the sweep above, but assert the exact digests too. */
    memset(msg, 'a', 120);
    hash_oneshot(HASH_SHA256, msg, 55, one);
    chk("SHA-256(55 x 'a') last block just fits", one, 32,
        "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    hash_oneshot(HASH_SHA256, msg, 56, one);
    chk("SHA-256(56 x 'a') forces an extra block", one, 32,
        "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    hash_oneshot(HASH_SHA256, msg, 57, one);
    chk("SHA-256(57 x 'a') second block", one, 32,
        "f13b2d724659eb3bf47f2dd6af1accc87b81f09f59f2b75e5c0bed6589dfe8c6");
    hash_oneshot(HASH_SHA512, msg, 111, one);
    chk("SHA-512(111 x 'a') last block just fits", one, 64,
        "fa9121c7b32b9e01733d034cfc78cbf67f926c7ed83e82200ef86818196921760"
        "b4beff48404df811b953828274461673c68d04e297b0eb7b2b4d60fc6b566a2");
    hash_oneshot(HASH_SHA512, msg, 112, one);
    chk("SHA-512(112 x 'a') forces an extra block", one, 64,
        "c01d080efd492776a1c43bd23dd99d0a2e626d481e16782e75d54c2503b5dc32"
        "bd05f0f1ba33e568b88fd2d970929b719ecbb152f58f130a407c8830604b70ca");
    hash_oneshot(HASH_SHA512, msg, 113, one);
    chk("SHA-512(113 x 'a') second block", one, 64,
        "55ddd8ac210a6e18ba1ee055af84c966e0dbff091c43580ae1be703bdb85da31"
        "acf6948cf5bd90c55a20e5450f22fb89bd8d0085e39f85a86cc46abbca75e24d");

    /* hash_copy()/hash_peek(): the transcript hash TLS needs. */
    {
        struct hash_ctx c, fork;
        uint8_t at3[HASH_MAX_DIGEST], at3ref[HASH_MAX_DIGEST];
        uint8_t at8[HASH_MAX_DIGEST], at8ref[HASH_MAX_DIGEST];

        hash_init(&c, HASH_SHA384);
        hash_update(&c, "abc", 3);
        hash_peek(&c, at3);
        hash_copy(&fork, &c);
        hash_final(&fork, at3ref);
        hash_update(&c, "defghijk", 8);
        hash_peek(&c, at8);
        hash_final(&c, at8ref);

        hash_oneshot(HASH_SHA384, "abc", 3, at3ref);
        chk_bool("hash_peek at 3 bytes == SHA-384(\"abc\")",
                 memcmp(at3, at3ref, 48) == 0);
        hash_oneshot(HASH_SHA384, "abcdefghijk", 11, at8ref);
        chk_bool("hash_peek after more input == SHA-384(\"abcdefghijk\")",
                 memcmp(at8, at8ref, 48) == 0);
    }

    /* Unknown algorithms are refused rather than silently mishandled. */
    {
        struct hash_ctx c;

        chk_bool("hash_init rejects a bad algorithm",
                 hash_init(&c, 99) == -1 && hash_init(&c, -1) == -1);
        chk_bool("hash_digest_len(bad) == 0", hash_digest_len(99) == 0);
    }

#ifdef HAVE_LIBC_SHA256
    /* libc/sha256.c is the package manager's copy and stays where it is.
     * This proves the two cannot have drifted apart. */
    {
        static uint8_t buf[3000];
        unsigned long len;

        quiet_ok = 1;
        rndbuf(buf, sizeof(buf));
        for (len = 0; len <= 600; len++) {
            char hex[65];
            char mine[65];
            unsigned long i;
            static const char hexd[] = "0123456789abcdef";

            sha256_hex(buf, len, hex);
            hash_oneshot(HASH_SHA256, buf, len, one);
            for (i = 0; i < 32; i++) {
                mine[i * 2] = hexd[(one[i] >> 4) & 0xf];
                mine[i * 2 + 1] = hexd[one[i] & 0xf];
            }
            mine[64] = '\0';
            q(strcmp(hex, mine) == 0);
        }
        chk_bool("libtls SHA-256 == libc SHA-256 (lengths 0..600)", quiet_ok);
    }
#endif
}

/* ================= 3. HMAC, RFC 4231 ================= */

struct hmac_case {
    const char *keyhex;
    const char *data;             /* NULL means use datahex */
    const char *datahex;
    const char *e256;
    const char *e384;
    const char *e512;
};

static const struct hmac_case hmac_cases[] = {
    /* Case 1 */
    { "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", "Hi There", NULL,
      "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
      "afd03944d84895626b0825f4ab46907f15f9dadbe4101ec682aa034c7cebc59c"
      "faea9ea9076ede7f4af152e8b2fa9cb6",
      "87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cde"
      "daa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854" },
    /* Case 2: a key shorter than the digest */
    { "4a656665", "what do ya want for nothing?", NULL,
      "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
      "af45d2e376484031617f78d2b58a6b1b9c7ef464f5a01b47e42ec3736322445e"
      "8e2240ca5e69e2c78b3239ecfab21649",
      "164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea250554"
      "9758bf75c05a994a6d034f65f8f0e6fdcaeab1a34d4a6b4b636e070a38bce737" },
    /* Case 3 */
    { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", NULL,
      "dddddddddddddddddddddddddddddddddddddddddddddddddd"
      "dddddddddddddddddddddddddddddddddddddddddddddddddd",
      "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe",
      "88062608d3e6ad8a0aa2ace014c8a86f0aa635d947ac9febe83ef4e55966144b"
      "2a5ab39dc13814b94e3ab6e101a34f27",
      "fa73b0089d56a284efb0f0756c890be9b1b5dbdd8ee81a3655f83e33b2279d39"
      "bf3e848279a722c806b485a47e67c807b946a337bee8942674278859e13292fb" },
    /* Case 4 */
    { "0102030405060708090a0b0c0d0e0f10111213141516171819", NULL,
      "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd"
      "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd",
      "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b",
      "3e8a69b7783c25851933ab6290af6ca77a9981480850009cc5577c6e1f573b4e"
      "6801dd23c4a7d679ccf8a386c674cffb",
      "b0ba465637458c6990e5a8c5f61d4af7e576d97ff94b872de76f8050361ee3db"
      "a91ca5c11aa25eb4d679275cc5788063a5f19741120c4f2de2adebeb10a298dd" },
    /* Case 6: a key longer than the block, so it gets hashed first */
    { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "aaaaaa",
      "Test Using Larger Than Block-Size Key - Hash Key First", NULL,
      "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
      "4ece084485813e9088d2c63a041bc5b44f9ef1012a2b588f3cd11f05033ac4c6"
      "0c2ef6ab4030fe8296248df163f44952",
      "80b24263c7c1a3ebb71493c1dd7be8b49b46d1f41b4aeec1121b013783f8f352"
      "6b56d037e05f2598bd0fd2215d6a1e5295e64f73f63f0aec8b915a985d786598" },
    /* Case 7: long key and long message */
    { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "aaaaaa",
      "This is a test using a larger than block-size key and a larger "
      "than block-size data. The key needs to be hashed before being "
      "used by the HMAC algorithm.", NULL,
      "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2",
      "6617178e941f020d351e2f254e8fd32c602420feb0b8fb9adccebb82461e99c5"
      "a678cc31e799176d3860e6110c46523e",
      "e37b6a775dc87dbaa4dfa9f96e5e3ffddebd71f8867289865df5a32d20cdc944"
      "b6022cac3c4982b10d5eeb55c3e4de15134676fb6de0446065c97440fa8c6a58" }
};

static void test_hmac(void)
{
    static uint8_t key[256], data[256];
    uint8_t mac[HASH_MAX_DIGEST];
    unsigned long i;

    begin("HMAC, RFC 4231 test cases");

    for (i = 0; i < sizeof(hmac_cases) / sizeof(hmac_cases[0]); i++) {
        const struct hmac_case *tc = &hmac_cases[i];
        unsigned long klen = unhex(tc->keyhex, key, sizeof(key));
        unsigned long dlen;
        char name[96];

        if (tc->data) {
            dlen = strlen(tc->data);
            memcpy(data, tc->data, dlen);
        } else {
            dlen = unhex(tc->datahex, data, sizeof(data));
        }

        sprintf(name, "HMAC-SHA-256 case %lu (key %lu, msg %lu)", i + 1, klen, dlen);
        hmac(HASH_SHA256, key, klen, data, dlen, mac);
        chk(name, mac, 32, tc->e256);

        sprintf(name, "HMAC-SHA-384 case %lu", i + 1);
        hmac(HASH_SHA384, key, klen, data, dlen, mac);
        chk(name, mac, 48, tc->e384);

        sprintf(name, "HMAC-SHA-512 case %lu", i + 1);
        hmac(HASH_SHA512, key, klen, data, dlen, mac);
        chk(name, mac, 64, tc->e512);
    }

    /* Streaming HMAC must equal one-shot HMAC. */
    quiet_ok = 1;
    {
        static uint8_t msg[2000];
        unsigned long len;

        rndbuf(msg, sizeof(msg));
        rndbuf(key, 64);
        for (len = 0; len <= 400; len++) {
            struct hmac_ctx c;
            uint8_t a[HASH_MAX_DIGEST], b[HASH_MAX_DIGEST];
            unsigned long split = len ? (rnd() % len) : 0;

            hmac(HASH_SHA384, key, 64, msg, len, a);
            hmac_init(&c, HASH_SHA384, key, 64);
            hmac_update(&c, msg, split);
            hmac_update(&c, msg + split, len - split);
            hmac_final(&c, b);
            q(memcmp(a, b, 48) == 0);
        }
    }
    chk_bool("streaming HMAC == one-shot HMAC (lengths 0..400)", quiet_ok);

    /* A key of exactly one block, and one byte over, take different paths. */
    {
        uint8_t k64[128];
        uint8_t a[32], b[32];

        memset(k64, 0x5a, sizeof(k64));
        hmac(HASH_SHA256, k64, 64, "x", 1, a);
        hmac(HASH_SHA256, k64, 65, "x", 1, b);
        chk_bool("HMAC key at and over the block size differ",
                 memcmp(a, b, 32) != 0);
        /* A key over the block is replaced by its digest, so those agree. */
        {
            uint8_t kd[32];

            hash_oneshot(HASH_SHA256, k64, 65, kd);
            hmac(HASH_SHA256, kd, 32, "x", 1, a);
            chk_bool("oversized key == HMAC with its digest",
                     memcmp(a, b, 32) == 0);
        }
    }
    chk_bool("hmac_init rejects a bad algorithm",
             hmac(42, key, 4, "x", 1, mac) == -1);
}

/* ================= 4. HKDF, RFC 5869 ================= */

static void test_hkdf(void)
{
    static uint8_t ikm[128], salt[128], info[128], okm[256];
    uint8_t prk[HASH_MAX_DIGEST];
    unsigned long i;

    begin("HKDF, RFC 5869 appendix A");

    /* A.1: SHA-256 with all inputs present. */
    unhex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", ikm, sizeof(ikm));
    unhex("000102030405060708090a0b0c", salt, sizeof(salt));
    unhex("f0f1f2f3f4f5f6f7f8f9", info, sizeof(info));
    hkdf_extract(HASH_SHA256, salt, 13, ikm, 22, prk);
    chk("A.1 PRK", prk, 32,
        "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5");
    hkdf_expand(HASH_SHA256, prk, 32, info, 10, okm, 42);
    chk("A.1 OKM (L=42)", okm, 42,
        "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
        "34007208d5b887185865");

    /* A.2: inputs longer than one block, output spanning many T(n). */
    for (i = 0; i < 80; i++) {
        ikm[i] = (uint8_t)i;
        salt[i] = (uint8_t)(0x60 + i);
        info[i] = (uint8_t)(0xb0 + i);
    }
    hkdf_extract(HASH_SHA256, salt, 80, ikm, 80, prk);
    chk("A.2 PRK", prk, 32,
        "06a6b88c5853361a06104c9ceb35b45cef760014904671014a193f40c15fc244");
    hkdf_expand(HASH_SHA256, prk, 32, info, 80, okm, 82);
    chk("A.2 OKM (L=82)", okm, 82,
        "b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c"
        "59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71"
        "cc30c58179ec3e87c14c01d5c1f3434f1d87");

    /* A.3: zero-length salt and info -- the salt default matters here. */
    unhex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", ikm, sizeof(ikm));
    hkdf_extract(HASH_SHA256, NULL, 0, ikm, 22, prk);
    chk("A.3 PRK (empty salt)", prk, 32,
        "19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04");
    hkdf_expand(HASH_SHA256, prk, 32, NULL, 0, okm, 42);
    chk("A.3 OKM (empty info)", okm, 42,
        "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d"
        "9d201395faa4b61a96c8");

    /* An empty salt must be identical to an explicit HashLen of zeros. */
    {
        uint8_t z[64], p2[HASH_MAX_DIGEST];

        memset(z, 0, sizeof(z));
        hkdf_extract(HASH_SHA384, z, 48, ikm, 22, p2);
        hkdf_extract(HASH_SHA384, NULL, 0, ikm, 22, prk);
        chk_bool("empty salt == HashLen zeros (SHA-384)",
                 memcmp(prk, p2, 48) == 0);
    }

    /* Output-length limits: 255*HashLen is the maximum reachable. */
    {
        static uint8_t big[255 * 64];

        hkdf_extract(HASH_SHA256, NULL, 0, ikm, 22, prk);
        chk_bool("hkdf_expand accepts L = 255*HashLen",
                 hkdf_expand(HASH_SHA256, prk, 32, NULL, 0, big, 255 * 32) == 0);
        chk_bool("hkdf_expand refuses L = 255*HashLen + 1",
                 hkdf_expand(HASH_SHA256, prk, 32, NULL, 0, big, 255 * 32 + 1) == -1);
        chk_bool("hkdf_expand refuses a short PRK",
                 hkdf_expand(HASH_SHA256, prk, 31, NULL, 0, big, 32) == -1);
        chk_bool("hkdf_expand refuses a bad algorithm",
                 hkdf_expand(7, prk, 32, NULL, 0, big, 32) == -1);
    }

    /* Every prefix of a long expansion must match a shorter expansion:
     * the T(n) chain has to be built the same way regardless of L. */
    quiet_ok = 1;
    {
        static uint8_t full[200], part[200];

        hkdf_expand(HASH_SHA256, prk, 32, info, 10, full, 200);
        for (i = 1; i <= 200; i++) {
            hkdf_expand(HASH_SHA256, prk, 32, info, 10, part, i);
            q(memcmp(full, part, i) == 0);
        }
    }
    chk_bool("hkdf_expand output is prefix-consistent (L=1..200)", quiet_ok);
}

/* ================= 5. TLS 1.3 key schedule ================= */

static void test_tls_schedule(void)
{
    uint8_t zero[64], early[HASH_MAX_DIGEST], derived[HASH_MAX_DIGEST];
    uint8_t out[64];

    begin("TLS 1.3 key schedule, RFC 8446 7.1 / RFC 8448");

    memset(zero, 0, sizeof(zero));

    /* The early secret with no PSK, and the "derived" secret computed over
     * an empty transcript -- both appear in the RFC 8448 trace. */
    hkdf_extract(HASH_SHA256, zero, 32, zero, 32, early);
    chk("early_secret (SHA-256, no PSK)", early, 32,
        "33ad0a1c607ec03b09e6cd9893680ce210adf300aa1f2660e1b22e10f170f92a");
    hkdf_derive_secret(HASH_SHA256, early, "derived", "", 0, derived);
    chk("Derive-Secret(early, \"derived\", \"\")", derived, 32,
        "6f2615a108c702c5678f54fc9dbab69716c076189c48250cebeac3576c3611ba");

    /* The SHA-384 half of the schedule, which is what AES-256-GCM suites
     * use. Cross-checked against an independent HKDF implementation rather
     * than lifted from a published trace. */
    hkdf_extract(HASH_SHA384, zero, 48, zero, 48, early);
    chk("early_secret (SHA-384, no PSK)", early, 48,
        "7ee8206f5570023e6dc7519eb1073bc4e791ad37b5c382aa10ba18e2357e7169"
        "71f9362f2c2fe2a76bfd78dfec4ea9b5");
    hkdf_derive_secret(HASH_SHA384, early, "derived", "", 0, derived);
    chk("Derive-Secret(early384, \"derived\", \"\")", derived, 48,
        "1591dac5cbbf0330a4a84de9c753330e92d01f0a88214b4464972fd668049e93"
        "e52f2b16fad922fdc0584478428f282b");

    /* The two expansions the record layer performs for every traffic key. */
    hkdf_extract(HASH_SHA256, zero, 32, zero, 32, early);
    hkdf_expand_label(HASH_SHA256, early, "key", NULL, 0, out, 16);
    chk("HKDF-Expand-Label(early, \"key\", \"\", 16)", out, 16,
        "ebbf95bddc9e43bd09465c5516ab2d5f");
    hkdf_expand_label(HASH_SHA256, early, "iv", NULL, 0, out, 12);
    chk("HKDF-Expand-Label(early, \"iv\", \"\", 12)", out, 12,
        "a7bf78a10cf9feb156a93f7a");

    /* Derive-Secret is Expand-Label over the transcript hash, so the two
     * entry points must agree. */
    {
        uint8_t th[HASH_MAX_DIGEST], viahash[HASH_MAX_DIGEST];

        hash_oneshot(HASH_SHA256, "ClientHello...", 14, th);
        hkdf_derive_secret(HASH_SHA256, early, "c hs traffic",
                           "ClientHello...", 14, derived);
        hkdf_derive_secret_hash(HASH_SHA256, early, "c hs traffic", th, viahash);
        chk_bool("Derive-Secret == Derive-Secret over a precomputed hash",
                 memcmp(derived, viahash, 32) == 0);
    }

    /* Label and context budgets, and the length field's range. */
    {
        static char longlab[300];
        static uint8_t ctx[300];

        memset(longlab, 'x', sizeof(longlab));
        memset(ctx, 0, sizeof(ctx));
        longlab[TLS_LABEL_MAX] = '\0';
        chk_bool("Expand-Label accepts a 249-byte label",
                 hkdf_expand_label(HASH_SHA256, early, longlab, NULL, 0,
                                   out, 32) == 0);
        longlab[TLS_LABEL_MAX] = 'x';
        longlab[TLS_LABEL_MAX + 1] = '\0';
        chk_bool("Expand-Label refuses a 250-byte label",
                 hkdf_expand_label(HASH_SHA256, early, longlab, NULL, 0,
                                   out, 32) == -1);
        chk_bool("Expand-Label accepts a 255-byte context",
                 hkdf_expand_label(HASH_SHA256, early, "x", ctx, 255,
                                   out, 32) == 0);
        chk_bool("Expand-Label refuses a 256-byte context",
                 hkdf_expand_label(HASH_SHA256, early, "x", ctx, 256,
                                   out, 32) == -1);
        chk_bool("Expand-Label refuses a zero output length",
                 hkdf_expand_label(HASH_SHA256, early, "x", NULL, 0,
                                   out, 0) == -1);
    }

    /* The label prefix really is "tls13 ": expanding with label L must
     * equal a raw expansion whose info is the hand-built HkdfLabel. */
    {
        uint8_t manual[64], info[32];
        unsigned long n = 0;

        info[n++] = 0; info[n++] = 32;             /* length */
        info[n++] = 6 + 3;                         /* "tls13 " + "key" */
        memcpy(info + n, "tls13 key", 9); n += 9;
        info[n++] = 0;                             /* empty context */
        hkdf_expand(HASH_SHA256, early, 32, info, n, manual, 32);
        hkdf_expand_label(HASH_SHA256, early, "key", NULL, 0, out, 32);
        chk_bool("Expand-Label builds the HkdfLabel structure as specified",
                 memcmp(manual, out, 32) == 0);
    }
}

/* ================= 6. ChaCha20, RFC 8439 ================= */

static const char *KEY8439 =
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
static const char *PT242 =
    "Ladies and Gentlemen of the class of '99: If I could offer you "
    "only one tip for the future, sunscreen would be it.";

static void test_chacha(void)
{
    uint8_t key[32], nonce[12];
    static uint8_t buf[256], buf2[256];

    begin("ChaCha20, RFC 8439");

    unhex(KEY8439, key, sizeof(key));

    /* 2.3.2: one keystream block at counter 1. */
    unhex("000000090000004a00000000", nonce, sizeof(nonce));
    chacha20_block(key, 1, nonce, buf);
    chk("2.3.2 block function", buf, 64,
        "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
        "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e");

    /* 2.4.2: the 114-byte encryption, which is not a block multiple. */
    unhex("000000000000004a00000000", nonce, sizeof(nonce));
    chacha20_xor(key, 1, nonce, (const uint8_t *)PT242, strlen(PT242), buf);
    chk("2.4.2 encryption (114 bytes)", buf, 114,
        "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0b"
        "f91b65c5524733ab8f593dabcd62b3571639d624e65152ab8f530c359f0861d8"
        "07ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab7793736"
        "5af90bbf74a35be6b40b8eedf2785e42874d");

    /* 2.6.2: the Poly1305 key derivation block. */
    {
        uint8_t k2[32], n2[12];

        unhex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f",
              k2, sizeof(k2));
        unhex("000000000001020304050607", n2, sizeof(n2));
        chacha20_block(k2, 0, n2, buf);
        chk("2.6.2 Poly1305 key generation", buf, 32,
            "8ad5a08b905f81cc815040274ab29471a833b637e3fd0da508dbb8e2fdd1a646");
    }

    /* XOR is an involution, so decryption is the same call. */
    unhex("000000000000004a00000000", nonce, sizeof(nonce));
    chacha20_xor(key, 1, nonce, (const uint8_t *)PT242, 114, buf);
    chacha20_xor(key, 1, nonce, buf, 114, buf2);
    chk_bool("ChaCha20 decrypt(encrypt(m)) == m",
             memcmp(buf2, PT242, 114) == 0);

    /* Every length up to four blocks must be a prefix of the keystream --
     * this catches a mishandled short final block. */
    quiet_ok = 1;
    {
        static uint8_t ks[256], zero[256];
        unsigned long len;

        memset(zero, 0, sizeof(zero));
        chacha20_xor(key, 7, nonce, zero, 256, ks);
        for (len = 0; len <= 256; len++) {
            static uint8_t part[256];

            memset(part, 0xff, sizeof(part));
            chacha20_xor(key, 7, nonce, zero, len, part);
            q(memcmp(part, ks, len) == 0);
            if (len < 256)
                q(part[len] == 0xff);             /* no overrun */
        }
    }
    chk_bool("ChaCha20 keystream is prefix-consistent (0..256 bytes)", quiet_ok);

    /* In-place operation. */
    {
        static uint8_t inplace[114];

        memcpy(inplace, PT242, 114);
        unhex("000000000000004a00000000", nonce, sizeof(nonce));
        chacha20_xor(key, 1, nonce, inplace, 114, inplace);
        chacha20_xor(key, 1, nonce, (const uint8_t *)PT242, 114, buf);
        chk_bool("ChaCha20 in place == out of place",
                 memcmp(inplace, buf, 114) == 0);
    }

    /* The 32-bit block counter is not allowed to wrap. */
    {
        static uint8_t small[64];

        chk_bool("ChaCha20 refuses to wrap the block counter",
                 chacha20_xor(key, 0xffffffffU, nonce, small, 65, buf) == -1);
        chk_bool("ChaCha20 allows the last block",
                 chacha20_xor(key, 0xffffffffU, nonce, small, 64, buf) == 0);
    }
}

/* ================= 7. Poly1305, RFC 8439 ================= */

static void test_poly1305(void)
{
    uint8_t key[32], tag[16];
    static uint8_t msg[512];

    begin("Poly1305, RFC 8439");

    /* 2.5.2 */
    unhex("85d6be7857556d337f4452fe42d506a8"
          "0103808afb0db2fd4abff6af4149f51b", key, sizeof(key));
    memcpy(msg, "Cryptographic Forum Research Group", 34);
    poly1305_mac(key, msg, 34, tag);
    chk("2.5.2 MAC", tag, 16, "a8061dc1305136c6c22b8baf0c0127a9");

    /* A.3 #1: r = 0 and s = 0, so the tag is zero however long the
     * message is. This is the degenerate case a naive reduction gets
     * wrong. */
    memset(key, 0, sizeof(key));
    memset(msg, 0, 64);
    poly1305_mac(key, msg, 64, tag);
    chk("A.3 #1 (r=0, s=0)", tag, 16, "00000000000000000000000000000000");

    /* A.3 #2: r = 0, so the tag is exactly s. */
    {
        static const char *m2 =
            "Any submission to the IETF intended by the Contributor for "
            "publication as all or part of an IETF Internet-Draft or RFC "
            "and any statement made within the context of an IETF activity "
            "is considered an \"IETF Contribution\". Such statements include "
            "oral statements in IETF sessions, as well as written and "
            "electronic communications made at any time or place, which are "
            "addressed to";

        memset(key, 0, 16);
        unhex("36e5f6b5c5e06070f0efca96227a863e", key + 16, 16);
        poly1305_mac(key, m2, strlen(m2), tag);
        chk("A.3 #2 (r=0, tag == s)", tag, 16,
            "36e5f6b5c5e06070f0efca96227a863e");

        /* A.3 #3: s = 0, so the tag is the bare polynomial. */
        unhex("36e5f6b5c5e06070f0efca96227a863e", key, 16);
        memset(key + 16, 0, 16);
        poly1305_mac(key, m2, strlen(m2), tag);
        chk("A.3 #3 (s=0, bare polynomial)", tag, 16,
            "f3477e7cd95417af89a6b8794c310cf0");
    }

    /* Streaming must equal one-shot at every split, including splits that
     * land inside a block. */
    quiet_ok = 1;
    {
        unsigned long len;

        rndbuf(key, 32);
        rndbuf(msg, sizeof(msg));
        for (len = 0; len <= 200; len++) {
            unsigned long split;

            poly1305_mac(key, msg, len, tag);
            for (split = 0; split <= len; split++) {
                struct poly1305_ctx c;
                uint8_t t2[16];

                poly1305_init(&c, key);
                poly1305_update(&c, msg, split);
                poly1305_update(&c, msg + split, len - split);
                poly1305_final(&c, t2);
                q(memcmp(tag, t2, 16) == 0);
            }
        }
    }
    chk_bool("streaming Poly1305 == one-shot (all splits 0..200)", quiet_ok);

    /* A message that is an exact block multiple takes the full-block path
     * on its last block; one byte more takes the padded path. */
    {
        uint8_t t16[16], t17[16];

        rndbuf(key, 32);
        poly1305_mac(key, msg, 16, t16);
        poly1305_mac(key, msg, 17, t17);
        chk_bool("16- and 17-byte messages take different paths",
                 memcmp(t16, t17, 16) != 0);
    }
}

/* ================= 8. ChaCha20-Poly1305 AEAD, RFC 8439 2.8.2 ========= */

static void test_chacha_aead(void)
{
    uint8_t key[32], nonce[12], aad[12];
    static uint8_t out[256], back[256];
    unsigned long ptlen = strlen(PT242);

    begin("ChaCha20-Poly1305 AEAD, RFC 8439 2.8.2");

    unhex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f",
          key, sizeof(key));
    unhex("070000004041424344454647", nonce, sizeof(nonce));
    unhex("50515253c0c1c2c3c4c5c6c7", aad, sizeof(aad));

    chk_bool("aead_seal returns 0",
             aead_seal(AEAD_CHACHA20_POLY1305, key, nonce, aad, 12,
                       (const uint8_t *)PT242, ptlen, out) == 0);
    chk("2.8.2 ciphertext", out, ptlen,
        "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
        "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
        "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
        "3ff4def08e4b7a9de576d26586cec64b6116");
    chk("2.8.2 tag", out + ptlen, 16, "1ae10b594f09e26a7e902ecbd0600691");

    chk_bool("aead_open accepts the genuine record",
             aead_open(AEAD_CHACHA20_POLY1305, key, nonce, aad, 12,
                       out, ptlen + 16, back) == 0);
    chk_bool("aead_open recovers the plaintext",
             memcmp(back, PT242, ptlen) == 0);
}

/* ================= 9. AES and GCM ================= */

static void test_aes_gcm(void)
{
    uint8_t key[32], nonce[12], block[16];
    static uint8_t aad[64], pt[64], out[128], back[128];
    struct aes_ctx aes;
    unsigned long n;

    begin("AES (FIPS 197) and AES-GCM (NIST GCM specification)");

    /* FIPS 197 appendix C single-block known answers. */
    unhex("000102030405060708090a0b0c0d0e0f", key, sizeof(key));
    unhex("00112233445566778899aabbccddeeff", pt, sizeof(pt));
    aes_init(&aes, key, 16);
    aes_encrypt_block(&aes, pt, block);
    chk("FIPS 197 C.1 AES-128", block, 16, "69c4e0d86a7b0430d8cdb78070b4c55a");

    unhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
          key, sizeof(key));
    aes_init(&aes, key, 32);
    aes_encrypt_block(&aes, pt, block);
    chk("FIPS 197 C.3 AES-256", block, 16, "8ea2b7ca516745bfeafc49904b496089");

    chk_bool("aes_init refuses a 24-byte key (AES-192 is not implemented)",
             aes_init(&aes, key, 24) == -1);

    /* H = E_K(0^128) for the all-zero key, the value the GCM spec quotes. */
    memset(key, 0, 32);
    memset(block, 0, 16);
    aes_init(&aes, key, 16);
    aes_encrypt_block(&aes, block, block);
    chk("GCM H for the all-zero AES-128 key", block, 16,
        "66e94bd4ef8a2c3b884cfa59ca342b2e");

    /* Test case 1: empty plaintext, empty AAD, zero key and IV. */
    memset(key, 0, 32);
    memset(nonce, 0, 12);
    chk_bool("tc1 seal", aead_seal(AEAD_AES_128_GCM, key, nonce, NULL, 0,
                                   NULL, 0, out) == 0);
    chk("GCM test case 1 tag", out, 16, "58e2fccefa7e3061367f1d57a4e7455a");

    /* Test case 2: a single zero block. */
    memset(pt, 0, 16);
    aead_seal(AEAD_AES_128_GCM, key, nonce, NULL, 0, pt, 16, out);
    chk("GCM test case 2 ciphertext", out, 16,
        "0388dace60b6a392f328c2b971b2fe78");
    chk("GCM test case 2 tag", out + 16, 16,
        "ab6e47d42cec13bdf53a67b21257bddf");

    /* Test cases 3 and 4: 64- and 60-byte plaintexts, the latter with AAD. */
    unhex("feffe9928665731c6d6a8f9467308308", key, sizeof(key));
    unhex("cafebabefacedbaddecaf888", nonce, sizeof(nonce));
    n = unhex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
              "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255",
              pt, sizeof(pt));
    chk_bool("GCM test plaintext is 64 bytes", n == 64);
    aead_seal(AEAD_AES_128_GCM, key, nonce, NULL, 0, pt, 64, out);
    chk("GCM test case 3 ciphertext", out, 64,
        "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
        "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091473f5985");
    chk("GCM test case 3 tag", out + 64, 16,
        "4d5c2af327cd64a62cf35abd2ba6fab4");

    unhex("feedfacedeadbeeffeedfacedeadbeefabaddad2", aad, sizeof(aad));
    aead_seal(AEAD_AES_128_GCM, key, nonce, aad, 20, pt, 60, out);
    chk("GCM test case 4 ciphertext", out, 60,
        "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
        "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091");
    chk("GCM test case 4 tag", out + 60, 16,
        "5bc94fbc3221a5db94fae95ae7121a47");
    chk_bool("GCM test case 4 opens",
             aead_open(AEAD_AES_128_GCM, key, nonce, aad, 20, out, 76, back) == 0
             && memcmp(back, pt, 60) == 0);

    /* Test cases 13 to 16: the AES-256 half. */
    memset(key, 0, 32);
    memset(nonce, 0, 12);
    aead_seal(AEAD_AES_256_GCM, key, nonce, NULL, 0, NULL, 0, out);
    chk("GCM test case 13 tag", out, 16, "530f8afbc74536b9a963b4f1c4cb738b");

    memset(back, 0, 16);
    aead_seal(AEAD_AES_256_GCM, key, nonce, NULL, 0, back, 16, out);
    chk("GCM test case 14 ciphertext", out, 16,
        "cea7403d4d606b6e074ec5d3baf39d18");
    chk("GCM test case 14 tag", out + 16, 16,
        "d0d1c8a799996bf0265b98b5d48ab919");

    unhex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308",
          key, sizeof(key));
    unhex("cafebabefacedbaddecaf888", nonce, sizeof(nonce));
    aead_seal(AEAD_AES_256_GCM, key, nonce, NULL, 0, pt, 64, out);
    chk("GCM test case 15 ciphertext", out, 64,
        "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
        "8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662898015ad");
    chk("GCM test case 15 tag", out + 64, 16,
        "b094dac5d93471bdec1a502270e3cc6c");

    aead_seal(AEAD_AES_256_GCM, key, nonce, aad, 20, pt, 60, out);
    chk("GCM test case 16 ciphertext", out, 60,
        "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
        "8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662");
    chk("GCM test case 16 tag", out + 60, 16,
        "76fc6ece0f4e1768cddf8853bb2d551b");
}

/* ================= 10. GHASH: table path vs reference ================= */

/* Recompute a GCM tag using only the bitwise reference multiply, so the
 * 4-bit table path inside aead.c has something independent to fail
 * against. This mirrors the spec directly:
 *   S = GHASH_H(A || pad || C || pad || [len(A)]64 || [len(C)]64)
 *   T = S xor E_K(J0)
 */
static void ref_ghash_feed(uint8_t y[16], const uint8_t h[16],
                           const uint8_t *data, unsigned long len)
{
    while (len) {
        uint8_t blk[16];
        unsigned long take = (len < 16) ? len : 16;
        int i;

        memset(blk, 0, sizeof(blk));
        memcpy(blk, data, take);
        for (i = 0; i < 16; i++)
            y[i] ^= blk[i];
        ghash_mul(y, h);
        data += take;
        len -= take;
    }
}

static void ref_gcm_tag(const uint8_t *key, unsigned long keylen,
                        const uint8_t nonce[12],
                        const uint8_t *aad, unsigned long aadlen,
                        const uint8_t *ct, unsigned long ctlen,
                        uint8_t tag[16])
{
    struct aes_ctx aes;
    uint8_t h[16], y[16], j0[16], lens[16], ek[16];
    int i;

    aes_init(&aes, key, keylen);
    memset(h, 0, 16);
    aes_encrypt_block(&aes, h, h);

    memset(y, 0, 16);
    ref_ghash_feed(y, h, aad, aadlen);
    ref_ghash_feed(y, h, ct, ctlen);

    memset(lens, 0, 16);
    for (i = 0; i < 8; i++) {
        lens[7 - i] = (uint8_t)(((uint64_t)aadlen * 8) >> (8 * i));
        lens[15 - i] = (uint8_t)(((uint64_t)ctlen * 8) >> (8 * i));
    }
    ref_ghash_feed(y, h, lens, 16);

    memcpy(j0, nonce, 12);
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
    aes_encrypt_block(&aes, j0, ek);
    for (i = 0; i < 16; i++)
        tag[i] = (uint8_t)(y[i] ^ ek[i]);
}

static void test_ghash(void)
{
    uint8_t a[16], b[16], c[16], one[16];
    unsigned long trial;

    begin("GHASH: GF(2^128) reference vs the table-driven path");

    /* The multiplicative identity in GCM's bit order is 0x80 followed by
     * fifteen zeros. Multiplying by it must change nothing. */
    quiet_ok = 1;
    memset(one, 0, 16);
    one[0] = 0x80;
    for (trial = 0; trial < 500; trial++) {
        rndbuf(a, 16);
        memcpy(b, a, 16);
        ghash_mul(b, one);
        q(memcmp(a, b, 16) == 0);
    }
    chk_bool("x * 1 == x in GF(2^128)", quiet_ok);

    /* The field is commutative. */
    quiet_ok = 1;
    for (trial = 0; trial < 500; trial++) {
        rndbuf(a, 16);
        rndbuf(b, 16);
        memcpy(c, a, 16);
        ghash_mul(c, b);           /* c = a*b */
        ghash_mul(b, a);           /* b = b*a */
        q(memcmp(b, c, 16) == 0);
    }
    chk_bool("multiplication is commutative", quiet_ok);

    /* And the library's fast path must agree with the reference on
     * arbitrary AAD and ciphertext lengths, including the odd tails that
     * exercise zero padding. */
    quiet_ok = 1;
    {
        static uint8_t key[32], nonce[12], aad[300], plain[300], out[320];
        uint8_t want[16];

        for (trial = 0; trial < 400; trial++) {
            unsigned long aadlen = rnd() % 301;
            unsigned long ptlen = rnd() % 301;
            unsigned long klen = (trial & 1) ? 32 : 16;
            int alg = (trial & 1) ? AEAD_AES_256_GCM : AEAD_AES_128_GCM;

            rndbuf(key, 32);
            rndbuf(nonce, 12);
            rndbuf(aad, sizeof(aad));
            rndbuf(plain, sizeof(plain));
            aead_seal(alg, key, nonce, aad, aadlen, plain, ptlen, out);
            ref_gcm_tag(key, klen, nonce, aad, aadlen, out, ptlen, want);
            q(memcmp(want, out + ptlen, 16) == 0);
        }
    }
    chk_bool("table GHASH == bitwise reference (400 random GCM records)",
             quiet_ok);
}

/* ================= 11. AEAD round trips ================= */

#define BIGLEN 16384

static uint8_t g_plain[BIGLEN];
static uint8_t g_sealed[BIGLEN + 32];
static uint8_t g_opened[BIGLEN + 32];
static uint8_t g_aad[512];

static void test_roundtrip(void)
{
    static const int algs[3] = { AEAD_CHACHA20_POLY1305, AEAD_AES_128_GCM,
                                 AEAD_AES_256_GCM };
    static const unsigned long extra[] = {
        255, 256, 257, 511, 512, 513, 1023, 1024, 1025, 4095, 4096, 4097,
        8192, 16383, BIGLEN
    };
    uint8_t key[32], nonce[12];
    int a;

    begin("AEAD round trips");

    rndbuf(g_plain, sizeof(g_plain));
    rndbuf(g_aad, sizeof(g_aad));

    for (a = 0; a < 3; a++) {
        int alg = algs[a];
        unsigned long i;
        char name[96];

        quiet_ok = 1;

        /* Every length from 0 to 300, with a rotating AAD length so the
         * AAD padding path is exercised at every residue too. */
        for (i = 0; i <= 300; i++) {
            unsigned long aadlen = i % 40;

            rndbuf(key, 32);
            rndbuf(nonce, 12);
            memset(g_sealed, 0xa5, sizeof(g_sealed));
            q(aead_seal(alg, key, nonce, g_aad, aadlen, g_plain, i,
                        g_sealed) == 0);
            /* Nothing beyond the ciphertext and tag may be touched. */
            q(g_sealed[i + 16] == 0xa5);
            memset(g_opened, 0x5a, sizeof(g_opened));
            q(aead_open(alg, key, nonce, g_aad, aadlen, g_sealed, i + 16,
                        g_opened) == 0);
            q(memcmp(g_opened, g_plain, i) == 0);
            q(g_opened[i] == 0x5a);
        }

        /* Buffer-boundary and large lengths. */
        for (i = 0; i < sizeof(extra) / sizeof(extra[0]); i++) {
            unsigned long len = extra[i];

            rndbuf(key, 32);
            rndbuf(nonce, 12);
            q(aead_seal(alg, key, nonce, g_aad, 137, g_plain, len,
                        g_sealed) == 0);
            q(aead_open(alg, key, nonce, g_aad, 137, g_sealed, len + 16,
                        g_opened) == 0);
            q(memcmp(g_opened, g_plain, len) == 0);
        }

        sprintf(name, "%s: seal/open round trip, lengths 0..300 and 255..16384",
                aead_name(alg));
        chk_bool(name, quiet_ok);

        /* In-place sealing and opening, which is what a record layer wants. */
        quiet_ok = 1;
        for (i = 0; i <= 200; i++) {
            rndbuf(key, 32);
            rndbuf(nonce, 12);
            memcpy(g_sealed, g_plain, i);
            q(aead_seal(alg, key, nonce, g_aad, 17, g_sealed, i,
                        g_sealed) == 0);
            q(aead_seal(alg, key, nonce, g_aad, 17, g_plain, i,
                        g_opened) == 0);
            q(memcmp(g_sealed, g_opened, i + 16) == 0);
            q(aead_open(alg, key, nonce, g_aad, 17, g_sealed, i + 16,
                        g_sealed) == 0);
            q(memcmp(g_sealed, g_plain, i) == 0);
        }
        sprintf(name, "%s: in-place seal and open", aead_name(alg));
        chk_bool(name, quiet_ok);

        /* The context form must match the one-shot form exactly. */
        quiet_ok = 1;
        {
            struct aead_ctx ctx;

            rndbuf(key, 32);
            q(aead_init(&ctx, alg, key) == 0);
            for (i = 0; i <= 100; i++) {
                static uint8_t viactx[200];

                rndbuf(nonce, 12);
                q(aead_ctx_seal(&ctx, nonce, g_aad, 9, g_plain, i, viactx) == 0);
                q(aead_seal(alg, key, nonce, g_aad, 9, g_plain, i,
                            g_sealed) == 0);
                q(memcmp(viactx, g_sealed, i + 16) == 0);
            }
        }
        sprintf(name, "%s: aead_ctx_seal == aead_seal", aead_name(alg));
        chk_bool(name, quiet_ok);
    }

    /* Key, nonce and AAD all have to matter. */
    quiet_ok = 1;
    for (a = 0; a < 3; a++) {
        int alg = algs[a];
        uint8_t key2[32], nonce2[12];

        rndbuf(key, 32);
        rndbuf(nonce, 12);
        aead_seal(alg, key, nonce, g_aad, 20, g_plain, 64, g_sealed);

        memcpy(key2, key, 32);
        key2[0] ^= 1;
        q(aead_open(alg, key2, nonce, g_aad, 20, g_sealed, 80, g_opened) == -1);

        memcpy(nonce2, nonce, 12);
        nonce2[11] ^= 1;
        q(aead_open(alg, key, nonce2, g_aad, 20, g_sealed, 80, g_opened) == -1);

        q(aead_open(alg, key, nonce, g_aad, 19, g_sealed, 80, g_opened) == -1);
        q(aead_open(alg, key, nonce, NULL, 0, g_sealed, 80, g_opened) == -1);
    }
    chk_bool("wrong key, nonce or AAD length is rejected", quiet_ok);
}

/* ================= 12. Tamper rejection ================= */

/* Flip every single bit of the ciphertext, the tag and the AAD in turn and
 * require that aead_open refuses every one of them. */
static void tamper_one(int alg, unsigned long ptlen, unsigned long aadlen)
{
    uint8_t key[32], nonce[12];
    unsigned long total = ptlen + 16;
    unsigned long i, bit;
    unsigned long rejected = 0, expected = (total + aadlen) * 8;
    char name[128];

    rndbuf(key, 32);
    rndbuf(nonce, 12);
    aead_seal(alg, key, nonce, g_aad, aadlen, g_plain, ptlen, g_sealed);

    /* Sanity: the untampered record must open. */
    if (aead_open(alg, key, nonce, g_aad, aadlen, g_sealed, total,
                  g_opened) != 0) {
        printf("  FAIL untampered record did not open (%s)\n", aead_name(alg));
        failures++;
        checks++;
        return;
    }

    for (i = 0; i < total; i++) {
        for (bit = 0; bit < 8; bit++) {
            g_sealed[i] ^= (uint8_t)(1u << bit);
            if (aead_open(alg, key, nonce, g_aad, aadlen, g_sealed, total,
                          g_opened) == -1)
                rejected++;
            g_sealed[i] ^= (uint8_t)(1u << bit);
        }
    }
    for (i = 0; i < aadlen; i++) {
        for (bit = 0; bit < 8; bit++) {
            g_aad[i] ^= (uint8_t)(1u << bit);
            if (aead_open(alg, key, nonce, g_aad, aadlen, g_sealed, total,
                          g_opened) == -1)
                rejected++;
            g_aad[i] ^= (uint8_t)(1u << bit);
        }
    }

    sprintf(name, "%s: all %lu single-bit flips rejected (pt=%lu aad=%lu)",
            aead_name(alg), expected, ptlen, aadlen);
    chk_bool(name, rejected == expected);
}

static void test_tamper(void)
{
    static const int algs[3] = { AEAD_CHACHA20_POLY1305, AEAD_AES_128_GCM,
                                 AEAD_AES_256_GCM };
    int a;

    begin("Tamper rejection: every single-bit flip in ciphertext, tag and AAD");

    for (a = 0; a < 3; a++) {
        tamper_one(algs[a], 64, 20);
        tamper_one(algs[a], 0, 0);      /* tag-only record */
        tamper_one(algs[a], 1, 1);      /* shortest non-empty */
        tamper_one(algs[a], 17, 33);    /* both past a block boundary */
    }

    /* Truncation and extension must fail too. */
    quiet_ok = 1;
    for (a = 0; a < 3; a++) {
        uint8_t key[32], nonce[12];

        rndbuf(key, 32);
        rndbuf(nonce, 12);
        aead_seal(algs[a], key, nonce, g_aad, 8, g_plain, 64, g_sealed);
        q(aead_open(algs[a], key, nonce, g_aad, 8, g_sealed, 79, g_opened) == -1);
        q(aead_open(algs[a], key, nonce, g_aad, 8, g_sealed, 16, g_opened) == -1);
        g_sealed[80] = 0;
        q(aead_open(algs[a], key, nonce, g_aad, 8, g_sealed, 81, g_opened) == -1);
        /* A record shorter than the tag is a length error, not a forgery. */
        q(aead_open(algs[a], key, nonce, g_aad, 8, g_sealed, 15, g_opened) == -1);
        q(aead_open(algs[a], key, nonce, g_aad, 8, g_sealed, 0, g_opened) == -1);
    }
    chk_bool("truncated, extended and undersized records are rejected",
             quiet_ok);

    /* A tag that differs in exactly one bit at each position, built by
     * hand rather than by flipping a sealed record, to be sure the
     * comparison is not short-circuiting on the first byte. */
    quiet_ok = 1;
    {
        uint8_t key[32], nonce[12];
        unsigned long i;

        rndbuf(key, 32);
        rndbuf(nonce, 12);
        aead_seal(AEAD_CHACHA20_POLY1305, key, nonce, NULL, 0, g_plain, 32,
                  g_sealed);
        for (i = 0; i < 16; i++) {
            g_sealed[32 + i] ^= 0x80;
            q(aead_open(AEAD_CHACHA20_POLY1305, key, nonce, NULL, 0,
                        g_sealed, 48, g_opened) == -1);
            g_sealed[32 + i] ^= 0x80;
        }
    }
    chk_bool("a flip in any tag byte is caught, including the last", quiet_ok);
}

/* ================= 13. API surface and bounds ================= */

static void test_api(void)
{
    uint8_t key[32], nonce[12], out[64];
    struct aead_ctx ctx;

    begin("API surface, error paths and bounds");

    memset(key, 0, sizeof(key));
    memset(nonce, 0, sizeof(nonce));

    chk_bool("aead_key_len is 32/16/32",
             aead_key_len(AEAD_CHACHA20_POLY1305) == 32 &&
             aead_key_len(AEAD_AES_128_GCM) == 16 &&
             aead_key_len(AEAD_AES_256_GCM) == 32);
    chk_bool("aead_nonce_len is 12 everywhere",
             aead_nonce_len(AEAD_CHACHA20_POLY1305) == 12 &&
             aead_nonce_len(AEAD_AES_128_GCM) == 12 &&
             aead_nonce_len(AEAD_AES_256_GCM) == 12);
    chk_bool("aead_tag_len is 16 everywhere",
             aead_tag_len(AEAD_CHACHA20_POLY1305) == 16 &&
             aead_tag_len(AEAD_AES_128_GCM) == 16 &&
             aead_tag_len(AEAD_AES_256_GCM) == 16);
    chk_bool("unknown algorithms report zero lengths",
             aead_key_len(9) == 0 && aead_nonce_len(9) == 0 &&
             aead_tag_len(9) == 0);
    chk_bool("aead_init rejects a bad algorithm",
             aead_init(&ctx, 9, key) == -1 && aead_init(&ctx, -1, key) == -1);
    chk_bool("aead_init rejects a null key",
             aead_init(&ctx, AEAD_AES_128_GCM, NULL) == -1);
    chk_bool("aead_seal rejects a bad algorithm",
             aead_seal(9, key, nonce, NULL, 0, NULL, 0, out) == -1);
    chk_bool("aead_open rejects a bad algorithm",
             aead_open(9, key, nonce, NULL, 0, out, 16, out) == -1);

    chk_bool("hash_digest_len is 32/48/64",
             hash_digest_len(HASH_SHA256) == 32 &&
             hash_digest_len(HASH_SHA384) == 48 &&
             hash_digest_len(HASH_SHA512) == 64);
    chk_bool("hash_block_len is 64/128/128",
             hash_block_len(HASH_SHA256) == 64 &&
             hash_block_len(HASH_SHA384) == 128 &&
             hash_block_len(HASH_SHA512) == 128);
    chk_bool("hash_name reports the algorithm",
             strcmp(hash_name(HASH_SHA384), "SHA-384") == 0 &&
             strcmp(hash_name(99), "?") == 0);
    chk_bool("aead_name reports the algorithm",
             strcmp(aead_name(AEAD_AES_256_GCM), "AES-256-GCM") == 0 &&
             strcmp(aead_name(9), "?") == 0);

    /* Oversized inputs must be refused rather than wrapped. */
    chk_bool("aead_seal refuses an over-budget AAD length",
             aead_seal(AEAD_AES_128_GCM, key, nonce, out, AEAD_MAX_AAD + 1,
                       NULL, 0, out) == -1);

    /* RFC 8446 5.3 nonce construction. */
    {
        uint8_t iv[12], n[12];

        unhex("000102030405060708090a0b", iv, sizeof(iv));
        aead_nonce(iv, 12, 0, n);
        chk("aead_nonce(seq=0) == iv", n, 12, "000102030405060708090a0b");
        aead_nonce(iv, 12, 1, n);
        chk("aead_nonce(seq=1)", n, 12, "000102030405060708090a0a");
        aead_nonce(iv, 12, 0x0102030405060708ULL, n);
        chk("aead_nonce(seq=0x0102030405060708)", n, 12,
            "00010203050705030d0f0d03");
    }
}

/* ================= main ================= */

int main(void)
{
    printf("libtls symmetric crypto: hash.c and aead.c\n");
    printf("sizeof(struct hash_ctx)=%lu  hmac_ctx=%lu  aead_ctx=%lu"
           "  poly1305_ctx=%lu\n",
           (unsigned long)sizeof(struct hash_ctx),
           (unsigned long)sizeof(struct hmac_ctx),
           (unsigned long)sizeof(struct aead_ctx),
           (unsigned long)sizeof(struct poly1305_ctx));

    test_sha_vectors();
    test_sha_streaming();
    test_hmac();
    test_hkdf();
    test_tls_schedule();
    test_chacha();
    test_poly1305();
    test_chacha_aead();
    test_aes_gcm();
    test_ghash();
    test_roundtrip();
    test_tamper();
    test_api();

    printf("\n=============================================\n");
    printf("%lu checks, %lu failures\n", checks, failures);
    printf("%s\n", failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
