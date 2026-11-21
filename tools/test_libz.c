/* Host regression test for KestrelOS libz (DEFLATE/zlib/gzip + crc32/adler32).
 *
 * libz is pure computation, so the real objects link straight into a host
 * program.  Fixtures are python-zlib output baked in as byte arrays (the
 * compressed forms are tiny); plaintexts are rebuilt at run time.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -g -Ilibz \
 *      tools/test_libz.c libz/inflate.c libz/adler32.c libz/crc32.c \
 *      -o build/host-tests/test_libz
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inflate.h"

#define P2_LINE "The quick brown fox jumps over the lazy dog. KestrelOS libz test.\n"
#define P2_REPS 300
#define P2_LEN  (sizeof(P2_LINE) - 1) * P2_REPS   /* 19800 */
#define P3_LEN  2048

/* ---------------- fixtures (python zlib, level 9 / level 0 stored) ------- */

static const char p1_plain[] =
    "hello hello hello world, hello hello hello world!";

static const unsigned char p1_zlib[27] = {
    0x78,0xda,0xcb,0x48,0xcd,0xc9,0xc9,0x57,0xc8,0x40,0x22,0xcb,0xf3,0x8b,0x72,0x52,
    0x74,0x50,0x84,0x90,0x24,0x14,0x01,0xc5,0x71,0x11,0xf6,
};
static const unsigned char p1_gzip[39] = {
    0x1f,0x8b,0x08,0x00,0x00,0x00,0x00,0x00,0x02,0x0a,0xcb,0x48,0xcd,0xc9,0xc9,0x57,
    0xc8,0x40,0x22,0xcb,0xf3,0x8b,0x72,0x52,0x74,0x50,0x84,0x90,0x24,0x14,0x01,0x68,
    0x59,0x13,0xbf,0x31,0x00,0x00,0x00,
};
static const unsigned char p1_raw[21] = {
    0xcb,0x48,0xcd,0xc9,0xc9,0x57,0xc8,0x40,0x22,0xcb,0xf3,0x8b,0x72,0x52,0x74,0x50,
    0x84,0x90,0x24,0x14,0x01,
};
static const unsigned char p1_stored[54] = {
    0x01,0x31,0x00,0xce,0xff,0x68,0x65,0x6c,0x6c,0x6f,0x20,0x68,0x65,0x6c,0x6c,0x6f,
    0x20,0x68,0x65,0x6c,0x6c,0x6f,0x20,0x77,0x6f,0x72,0x6c,0x64,0x2c,0x20,0x68,0x65,
    0x6c,0x6c,0x6f,0x20,0x68,0x65,0x6c,0x6c,0x6f,0x20,0x68,0x65,0x6c,0x6c,0x6f,0x20,
    0x77,0x6f,0x72,0x6c,0x64,0x21,
};

static const unsigned char p2_zlib[157] = {
    0x78,0xda,0xed,0xcc,0xbb,0x11,0x82,0x40,0x14,0x40,0xd1,0xdc,0x2a,0x5e,0x05,0x34,
    0x62,0x60,0x20,0x0d,0x88,0xae,0x8a,0xae,0xae,0xc2,0xe2,0x87,0xea,0xa5,0x0b,0x93,
    0x13,0xde,0xb9,0x33,0xa7,0x3d,0xa7,0x78,0x4e,0xfd,0xfe,0x1a,0xdd,0x50,0xde,0xf7,
    0x38,0x96,0x4f,0x5c,0xa6,0xdb,0x63,0x8c,0xf2,0x4a,0x43,0xd4,0x65,0xe7,0xdd,0xfc,
    0x8d,0x43,0x39,0x35,0xb1,0x4e,0x63,0x1d,0x52,0xde,0x6c,0x23,0xf7,0xdd,0x1c,0x75,
    0xc9,0x66,0xd5,0x12,0x08,0x04,0x02,0x81,0x40,0x20,0x10,0x08,0x04,0x02,0x81,0x40,
    0x20,0x10,0x08,0x04,0x02,0x81,0x40,0x20,0x10,0x08,0x04,0x02,0x81,0x40,0x20,0x10,
    0x08,0x04,0x02,0x81,0x40,0x20,0x10,0x08,0x04,0x02,0x81,0x40,0x20,0x10,0x08,0x04,
    0x02,0x81,0x40,0x20,0x10,0x08,0x04,0x02,0x81,0x40,0x20,0x10,0x08,0x04,0x02,0x81,
    0x40,0x20,0x10,0x08,0x04,0xc2,0x5f,0x85,0x1f,0x48,0x48,0x99,0xa6,
};
static const unsigned char p2_gzip[169] = {
    0x1f,0x8b,0x08,0x00,0x00,0x00,0x00,0x00,0x02,0x0a,0xed,0xcc,0xbb,0x11,0x82,0x40,
    0x14,0x40,0xd1,0xdc,0x2a,0x5e,0x05,0x34,0x62,0x60,0x20,0x0d,0x88,0xae,0x8a,0xae,
    0xae,0xc2,0xe2,0x87,0xea,0xa5,0x0b,0x93,0x13,0xde,0xb9,0x33,0xa7,0x3d,0xa7,0x78,
    0x4e,0xfd,0xfe,0x1a,0xdd,0x50,0xde,0xf7,0x38,0x96,0x4f,0x5c,0xa6,0xdb,0x63,0x8c,
    0xf2,0x4a,0x43,0xd4,0x65,0xe7,0xdd,0xfc,0x8d,0x43,0x39,0x35,0xb1,0x4e,0x63,0x1d,
    0x52,0xde,0x6c,0x23,0xf7,0xdd,0x1c,0x75,0xc9,0x66,0xd5,0x12,0x08,0x04,0x02,0x81,
    0x40,0x20,0x10,0x08,0x04,0x02,0x81,0x40,0x20,0x10,0x08,0x04,0x02,0x81,0x40,0x20,
    0x10,0x08,0x04,0x02,0x81,0x40,0x20,0x10,0x08,0x04,0x02,0x81,0x40,0x20,0x10,0x08,
    0x04,0x02,0x81,0x40,0x20,0x10,0x08,0x04,0x02,0x81,0x40,0x20,0x10,0x08,0x04,0x02,
    0x81,0x40,0x20,0x10,0x08,0x04,0x02,0x81,0x40,0x20,0x10,0x08,0x04,0xc2,0x5f,0x85,
    0x1f,0xec,0xc3,0x5e,0x29,0x58,0x4d,0x00,0x00,
};
static const unsigned char p2_raw[151] = {
    0xed,0xcc,0xbb,0x11,0x82,0x40,0x14,0x40,0xd1,0xdc,0x2a,0x5e,0x05,0x34,0x62,0x60,
    0x20,0x0d,0x88,0xae,0x8a,0xae,0xae,0xc2,0xe2,0x87,0xea,0xa5,0x0b,0x93,0x13,0xde,
    0xb9,0x33,0xa7,0x3d,0xa7,0x78,0x4e,0xfd,0xfe,0x1a,0xdd,0x50,0xde,0xf7,0x38,0x96,
    0x4f,0x5c,0xa6,0xdb,0x63,0x8c,0xf2,0x4a,0x43,0xd4,0x65,0xe7,0xdd,0xfc,0x8d,0x43,
    0x39,0x35,0xb1,0x4e,0x63,0x1d,0x52,0xde,0x6c,0x23,0xf7,0xdd,0x1c,0x75,0xc9,0x66,
    0xd5,0x12,0x08,0x04,0x02,0x81,0x40,0x20,0x10,0x08,0x04,0x02,0x81,0x40,0x20,0x10,
    0x08,0x04,0x02,0x81,0x40,0x20,0x10,0x08,0x04,0x02,0x81,0x40,0x20,0x10,0x08,0x04,
    0x02,0x81,0x40,0x20,0x10,0x08,0x04,0x02,0x81,0x40,0x20,0x10,0x08,0x04,0x02,0x81,
    0x40,0x20,0x10,0x08,0x04,0x02,0x81,0x40,0x20,0x10,0x08,0x04,0x02,0x81,0x40,0x20,
    0x10,0x08,0x04,0xc2,0x5f,0x85,0x1f,
};

/* ---------------- helpers ---------------------------------------------- */

static void build_p2(char *dst)
{
    for (int i = 0; i < P2_REPS; i++)
        memcpy(dst + i * (sizeof(P2_LINE) - 1), P2_LINE, sizeof(P2_LINE) - 1);
}

static void build_p3(unsigned char *dst)
{
    for (int i = 0; i < P3_LEN; i++)
        dst[i] = (unsigned char)(i & 0xff);
}

/* Wrap src in a raw-DEFLATE stored block: BFINAL=1, BTYPE=00, LEN + ~LEN.
 * n must fit in 16 bits. Returns the stream length (n + 5). */
static unsigned long make_stored(unsigned char *dst, const void *src,
                                 unsigned long n)
{
    dst[0] = 0x01;
    dst[1] = (unsigned char)n;
    dst[2] = (unsigned char)(n >> 8);
    dst[3] = (unsigned char)~n;
    dst[4] = (unsigned char)(~(n >> 8));
    memcpy(dst + 5, src, n);
    return n + 5;
}

/* One-shot round trip: returns a fresh buffer that must equal `want`. */
static void roundtrip(const unsigned char *src, unsigned long slen, int wrapper,
                      const void *want, unsigned long wlen, const char *name)
{
    void *out = NULL;
    unsigned long olen = 0;
    int rc = inflate_buf(src, slen, &out, &olen, wrapper);
    assert(rc == INFLATE_OK);
    assert(out != NULL);
    assert(olen == wlen);
    assert(((const unsigned char *)out)[olen] == 0);  /* NUL past the end */
    assert(memcmp(out, want, wlen) == 0);
    free(out);
    (void)name;
}

/* ---------------- crc32 / adler32 -------------------------------------- */

static void test_hashes(void)
{
    assert(crc32_buf("", 0) == 0);
    assert(crc32_buf("123456789", 9) == 0xCBF43926u);
    assert(crc32_buf("The quick brown fox jumps over the lazy dog", 43)
           == 0x414FA339u);
    /* Chunked update == one shot (start at 0). */
    {
        const char *s = "123456789";
        uint32_t c = 0;
        c = crc32_update(c, s, 3);
        c = crc32_update(c, s + 3, 0);      /* empty chunk is a no-op */
        c = crc32_update(c, s + 3, 6);
        assert(c == 0xCBF43926u);
    }
    assert(adler32_buf("", 0) == 1);        /* adler of empty input is 1 */
    assert(adler32_buf("Wikipedia", 9) == 0x11E60398u);
    assert(adler32_buf("123456789", 9) == 0x091E01DEu);
    {
        const char *s = "Wikipedia";
        uint32_t a = 1;                     /* adler starts at 1 */
        a = adler32_update(a, s, 4);
        a = adler32_update(a, s + 4, 5);
        assert(a == 0x11E60398u);
    }
}

/* ---------------- one-shot wrappers ------------------------------------- */

static void test_oneshot(void)
{
    static char p2[P2_LEN];
    static unsigned char p3[P3_LEN];
    build_p2(p2);
    build_p3(p3);

    roundtrip(p1_zlib, sizeof(p1_zlib), INFLATE_ZLIB,
              p1_plain, sizeof(p1_plain) - 1, "zlib");
    roundtrip(p1_gzip, sizeof(p1_gzip), INFLATE_GZIP,
              p1_plain, sizeof(p1_plain) - 1, "gzip");
    roundtrip(p1_raw, sizeof(p1_raw), INFLATE_RAW,
              p1_plain, sizeof(p1_plain) - 1, "raw");
    roundtrip(p1_stored, sizeof(p1_stored), INFLATE_RAW,
              p1_plain, sizeof(p1_plain) - 1, "stored");

    /* Dynamic-Huffman payload: 19.8 KiB -> 157 byte zlib stream, so the
     * decoder must emit long matches across its history window. */
    roundtrip(p2_zlib, sizeof(p2_zlib), INFLATE_ZLIB, p2, P2_LEN, "dyn zlib");
    roundtrip(p2_gzip, sizeof(p2_gzip), INFLATE_GZIP, p2, P2_LEN, "dyn gzip");
    roundtrip(p2_raw, sizeof(p2_raw), INFLATE_RAW, p2, P2_LEN, "dyn raw");

    /* Incompressible data shipped as a stored block. */
    {
        static unsigned char stored[P3_LEN + 5];
        unsigned long slen = make_stored(stored, p3, P3_LEN);
        void *out = NULL;
        unsigned long olen = 0;
        assert(inflate_buf(stored, slen, &out, &olen, INFLATE_RAW)
               == INFLATE_OK);
        assert(olen == P3_LEN && memcmp(out, p3, P3_LEN) == 0);
        free(out);
    }

    /* AUTO sniffs gzip, then zlib, else raw. */
    roundtrip(p1_gzip, sizeof(p1_gzip), INFLATE_AUTO,
              p1_plain, sizeof(p1_plain) - 1, "auto gzip");
    roundtrip(p1_zlib, sizeof(p1_zlib), INFLATE_AUTO,
              p1_plain, sizeof(p1_plain) - 1, "auto zlib");
    roundtrip(p1_raw, sizeof(p1_raw), INFLATE_AUTO,
              p1_plain, sizeof(p1_plain) - 1, "auto raw");

    /* Huffman tables were actually exercised on the dynamic stream. */
    assert(inflate_dbg_lens_used > 0);
    assert(inflate_dbg_dist_used > 0);
}

/* ---------------- streaming --------------------------------------------- */

static void test_streaming(void)
{
    static char p2[P2_LEN];
    static unsigned char acc[P2_LEN];
    struct inflate_stream *s;
    unsigned long off, got, plen;
    const unsigned char *peek;
    build_p2(p2);

    /* Feed the 157-byte stream in odd 13-byte chunks, draining output as
     * it appears; the reassembled bytes must equal the plaintext. */
    s = inflate_begin(INFLATE_ZLIB, 0);
    assert(s != NULL);
    off = got = 0;
    for (;;) {
        unsigned long n = sizeof(p2_zlib) - off;
        int rc;
        if (n > 13)
            n = 13;
        rc = inflate_push(s, p2_zlib + off, n);
        off += n;
        assert(rc == INFLATE_MORE || rc == INFLATE_DONE);
        for (;;) {
            peek = inflate_peek(s, &plen);
            if (!plen)
                break;
            memcpy(acc + got, peek, plen);   /* never drained early: <=32KiB kept */
            got += plen;
            inflate_drain(s, plen);
        }
        if (off == sizeof(p2_zlib))
            break;
    }
    assert(got == P2_LEN);
    assert(memcmp(acc, p2, P2_LEN) == 0);
    assert(inflate_total_in(s) == sizeof(p2_zlib));
    assert(inflate_total_out(s) == P2_LEN);
    {
        void *out = NULL;
        unsigned long olen = 0;
        assert(inflate_finish(s, &out, &olen) == INFLATE_OK);
        assert(olen == 0);              /* everything was already drained */
        free(out);
    }
    inflate_end(s);
    inflate_end(NULL);                  /* safe on NULL */

    /* inflate_peek coalesces undrained output. */
    s = inflate_begin(INFLATE_ZLIB, 0);
    assert(s != NULL);
    {
        int rc = inflate_push(s, p2_zlib, sizeof(p2_zlib));
        assert(rc == INFLATE_DONE || rc == INFLATE_MORE);
    }
    peek = inflate_peek(s, &plen);
    assert(plen == P2_LEN);
    assert(memcmp(peek, p2, 5000) == 0);
    inflate_drain(s, 5000);
    peek = inflate_peek(s, &plen);
    assert(plen == P2_LEN - 5000);
    assert(memcmp(peek, p2 + 5000, plen) == 0);
    assert(inflate_finish(s, NULL, NULL) == INFLATE_OK);
    inflate_end(s);
}

/* ---------------- errors ------------------------------------------------- */

static void test_errors(void)
{
    void *out;
    unsigned long olen;
    unsigned char buf[64];
    struct inflate_stream *s;

    /* Argument validation. */
    out = NULL;
    assert(inflate_buf(NULL, 4, &out, &olen, INFLATE_ZLIB) == INFLATE_ERR_ARG);
    assert(inflate_buf(p1_zlib, sizeof(p1_zlib), NULL, &olen, INFLATE_ZLIB)
           == INFLATE_ERR_ARG);
    assert(inflate_buf(p1_zlib, sizeof(p1_zlib), &out, NULL, INFLATE_ZLIB)
           == INFLATE_ERR_ARG);
    assert(inflate_begin(-1, 0) == NULL);
    assert(inflate_begin(4, 0) == NULL);
    assert(inflate_push(NULL, p1_raw, sizeof(p1_raw)) == INFLATE_ERR_ARG);
    assert(inflate_finish(NULL, &out, &olen) == INFLATE_ERR_ARG);

    /* Truncated mid-stream and mid-trailer. */
    out = NULL;
    assert(inflate_buf(p1_zlib, sizeof(p1_zlib) - 2, &out, &olen,
                       INFLATE_ZLIB) == INFLATE_ERR_TRUNCATED);
    assert(out == NULL);
    out = NULL;
    assert(inflate_buf(p1_gzip, sizeof(p1_gzip) - 5, &out, &olen,
                       INFLATE_GZIP) == INFLATE_ERR_TRUNCATED);
    /* Streaming finish on an incomplete stream is TRUNCATED too. */
    s = inflate_begin(INFLATE_ZLIB, 0);
    assert(s != NULL);
    assert(inflate_push(s, p1_zlib, 10) == INFLATE_MORE);
    assert(inflate_finish(s, &out, &olen) == INFLATE_ERR_TRUNCATED);
    /* Errors are sticky: the stream cannot be used afterwards. */
    assert(inflate_push(s, p1_zlib, 1) == INFLATE_ERR_TRUNCATED);
    assert(inflate_error(s) == INFLATE_ERR_TRUNCATED);
    inflate_end(s);

    /* Corrupt the adler-32 trailer. */
    memcpy(buf, p1_zlib, sizeof(p1_zlib));
    buf[sizeof(p1_zlib) - 1] ^= 0xff;
    out = NULL;
    assert(inflate_buf(buf, sizeof(p1_zlib), &out, &olen, INFLATE_ZLIB)
           == INFLATE_ERR_CHECKSUM);
    /* Corrupt the gzip CRC-32 trailer. */
    memcpy(buf, p1_gzip, sizeof(p1_gzip));
    buf[sizeof(p1_gzip) - 6] ^= 0xff;
    out = NULL;
    assert(inflate_buf(buf, sizeof(p1_gzip), &out, &olen, INFLATE_GZIP)
           == INFLATE_ERR_CHECKSUM);
    /* Corrupt the gzip ISIZE. */
    memcpy(buf, p1_gzip, sizeof(p1_gzip));
    buf[sizeof(p1_gzip) - 1] ^= 0xff;
    out = NULL;
    assert(inflate_buf(buf, sizeof(p1_gzip), &out, &olen, INFLATE_GZIP)
           == INFLATE_ERR_CHECKSUM);
    /* Bad zlib CMF/FLG check bytes. */
    memcpy(buf, p1_zlib, sizeof(p1_zlib));
    buf[1] ^= 0xff;
    out = NULL;
    assert(inflate_buf(buf, sizeof(p1_zlib), &out, &olen, INFLATE_ZLIB)
           == INFLATE_ERR_HEADER);
    /* Stored block LEN/NLEN mismatch. */
    memcpy(buf, p1_stored, sizeof(p1_stored));
    buf[3] ^= 0xff;                      /* NLEN no longer ~LEN */
    out = NULL;
    assert(inflate_buf(buf, sizeof(p1_stored), &out, &olen, INFLATE_RAW)
           == INFLATE_ERR_BLOCK);
    /* Reserved block type (BTYPE=3). */
    memcpy(buf, p1_raw, sizeof(p1_raw));
    buf[0] = (unsigned char)((buf[0] & 0xf8) | 0x06);
    out = NULL;
    assert(inflate_buf(buf, sizeof(p1_raw), &out, &olen, INFLATE_RAW)
           == INFLATE_ERR_BLOCK);
    /* Not a gzip/zlib stream under AUTO falls back to raw and fails sanely. */
    out = NULL;
    assert(inflate_buf("\x1f\x8b", 2, &out, &olen, INFLATE_AUTO)
           == INFLATE_ERR_TRUNCATED);

    /* Output cap: a 19.8 KiB plaintext may not pass a 1000-byte limit. */
    out = NULL;
    assert(inflate_buf_limit(p2_zlib, sizeof(p2_zlib), &out, &olen,
                             INFLATE_ZLIB, 1000) == INFLATE_ERR_LIMIT);
    assert(out == NULL);
    /* But it passes at exactly the real length. */
    {
        static char p2[P2_LEN];
        build_p2(p2);
        out = NULL;
        assert(inflate_buf_limit(p2_zlib, sizeof(p2_zlib), &out, &olen,
                                 INFLATE_ZLIB, P2_LEN) == INFLATE_OK);
        assert(olen == P2_LEN && memcmp(out, p2, P2_LEN) == 0);
        free(out);
    }

    /* Every error code has a human readable name. */
    {
        int codes[] = {INFLATE_OK, INFLATE_DONE, INFLATE_ERR_ARG,
                       INFLATE_ERR_MEMORY, INFLATE_ERR_HEADER,
                       INFLATE_ERR_BLOCK, INFLATE_ERR_HUFFMAN,
                       INFLATE_ERR_CODE, INFLATE_ERR_DISTANCE,
                       INFLATE_ERR_TRUNCATED, INFLATE_ERR_CHECKSUM,
                       INFLATE_ERR_LIMIT, INFLATE_ERR_STATE};
        for (unsigned i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
            const char *m = inflate_strerror(codes[i]);
            assert(m != NULL && m[0] != '\0');
        }
    }
}

int main(void)
{
    test_hashes();
    test_oneshot();
    test_streaming();
    test_errors();
    puts("libz: crc32/adler32 vectors + chunks, zlib/gzip/raw/auto round trips, "
         "stored + dynamic-huffman blocks, 13-byte-chunk streaming with "
         "peek/drain, truncation/checksum/header/block/limit/state errors PASS");
    return 0;
}
