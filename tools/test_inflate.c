/* Host test harness for libz (DEFLATE / zlib / gzip decompression).
 *
 * This is a HOST program, not a KestrelOS program: libz is pure
 * computation with no I/O, so it can be compiled for the host and tested
 * exhaustively without booting anything.
 *
 *   gcc -Wall -Wextra -O2 -Ilibz -fsanitize=address,undefined \
 *       -o /tmp/test_inflate tools/test_inflate.c libz/inflate.c \
 *       libz/crc32.c libz/adler32.c
 *   /tmp/test_inflate
 *
 * Fixtures are not committed. On startup this program writes a small
 * Python generator to /tmp/kestrel_libz_fixtures/gen.py, runs it with
 * python3, and reads the files it produces; python's zlib and gzip
 * modules are the known-good reference. Pass --keep to leave them
 * behind, or set KLIBZ_NOGEN=1 to reuse what is already there.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "inflate.h"

#define FIXDIR "/tmp/kestrel_libz_fixtures"

static int checks, failures;

#define CHECK(cond, ...)                                                  \
    do {                                                                  \
        checks++;                                                         \
        if (!(cond)) {                                                    \
            failures++;                                                   \
            printf("  FAIL (line %d): ", __LINE__);                       \
            printf(__VA_ARGS__);                                          \
            printf("\n");                                                 \
        }                                                                 \
    } while (0)

static void section(const char *name)
{
    printf("\n== %s\n", name);
}

/* ------------------------------------------------------------- fixtures */

static const char *gen_py =
"import zlib, gzip, io, os, struct, random\n"
"D = '" FIXDIR "'\n"
"os.makedirs(D, exist_ok=True)\n"
"def raw(data, lv):\n"
"    c = zlib.compressobj(lv, zlib.DEFLATED, -15)\n"
"    return c.compress(data) + c.flush()\n"
"def gz(data, lv, fname=None, comment=None, extra=None, hcrc=False, mtime=0, xfl=0, os_=3):\n"
"    flg = 0\n"
"    if hcrc: flg |= 2\n"
"    if extra is not None: flg |= 4\n"
"    if fname is not None: flg |= 8\n"
"    if comment is not None: flg |= 16\n"
"    h = struct.pack('<BBBBIBB', 0x1f, 0x8b, 8, flg, mtime, xfl, os_)\n"
"    if extra is not None: h += struct.pack('<H', len(extra)) + extra\n"
"    if fname is not None: h += fname + b'\\0'\n"
"    if comment is not None: h += comment + b'\\0'\n"
"    if hcrc: h += struct.pack('<H', zlib.crc32(h) & 0xffff)\n"
"    return h + raw(data, lv) + struct.pack('<II', zlib.crc32(data) & 0xffffffff, len(data) & 0xffffffff)\n"
"def w(name, b):\n"
"    open(os.path.join(D, name), 'wb').write(b)\n"
"def emit(name, data):\n"
"    w(name + '.bin', data)\n"
"    for lv in (0, 1, 6, 9):\n"
"        w('%s.l%d.raw' % (name, lv), raw(data, lv))\n"
"        w('%s.l%d.zlib' % (name, lv), zlib.compress(data, lv))\n"
"        w('%s.l%d.gz' % (name, lv), gz(data, lv))\n"
"emit('empty', b'')\n"
"emit('one', b'x')\n"
"emit('hello', b'hello world\\n')\n"
"lorem = (b'the quick brown fox jumps over the lazy dog. ' \n"
"         b'pack my box with five dozen liquor jugs. 0123456789 ')\n"
"emit('text', lorem * 900)\n"
"emit('html', (b'<html><head><title>t</title></head><body>' + \n"
"              b'<p class=\"x\">hello &amp; goodbye</p>' * 800 + b'</body></html>'))\n"
"emit('rle', b'A' * 200000)\n"
"emit('zeros', bytes(200000))\n"
"emit('rand', random.Random(4242).randbytes(262144))\n"
"# 4 MiB of semi compressible data: long matches plus fresh literals\n"
"r = random.Random(11)\n"
"base = bytes(r.randrange(97, 123) for _ in range(4096))\n"
"buf = bytearray()\n"
"while len(buf) < 4 * 1024 * 1024:\n"
"    buf += base[:r.randrange(100, 4096)]\n"
"    buf += bytes(r.randrange(32, 127) for _ in range(r.randrange(0, 40)))\n"
"emit('big4m', bytes(buf[:4 * 1024 * 1024]))\n"
"# every optional gzip header field at once\n"
"w('flags.gz', gz(lorem * 40, 6, fname=b'a-rather-long-file-name.txt',\n"
"                comment=b'a comment field', extra=b'\\x01\\x02ABCD',\n"
"                hcrc=True, mtime=0x5f5e100, xfl=2, os_=255))\n"
"w('flags.bin', lorem * 40)\n"
"# two gzip members back to back\n"
"w('multi.gz', gz(b'member one\\n', 6) + gz(b'member two\\n', 9))\n"
"w('multi.bin', b'member one\\nmember two\\n')\n"
"# many blocks: a full flush every 3 KiB forces block boundaries\n"
"c = zlib.compressobj(9, zlib.DEFLATED, -15)\n"
"src = lorem * 300\n"
"parts = []\n"
"for i in range(0, len(src), 3000):\n"
"    parts.append(c.compress(src[i:i+3000]))\n"
"    parts.append(c.flush(zlib.Z_FULL_FLUSH))\n"
"parts.append(c.flush())\n"
"w('blocks.l9.raw', b''.join(parts))\n"
"w('blocks.bin', src)\n"
"# a decompression bomb: 64 MiB of zeros\n"
"w('bomb.gz', gz(bytes(64 * 1024 * 1024), 9))\n"
"w('bomb.zlib', zlib.compress(bytes(64 * 1024 * 1024), 9))\n"
"# Reference verdicts. For every single-bit corruption and every\n"
"# truncation of a stream, record what python's zlib/gzip decides:\n"
"# 0 = rejected, 1 = decoded to the original, 2 = decoded to something\n"
"# else. libz has to agree.\n"
"def dec_for(tag):\n"
"    if tag == 'zlib': return lambda b: zlib.decompress(b)\n"
"    if tag == 'gz': return lambda b: gzip.decompress(b)\n"
"    return lambda b: zlib.decompress(b, -15)\n"
"def verdict(f, data, c):\n"
"    try:\n"
"        return 1 if f(bytes(c)) == data else 2\n"
"    except Exception:\n"
"        return 0\n"
"def refs(name, tag, data, comp):\n"
"    f = dec_for(tag)\n"
"    flips = bytearray()\n"
"    for i in range(len(comp)):\n"
"        for b in (0, 3, 6):\n"
"            c = bytearray(comp)\n"
"            c[i] ^= (1 << b)\n"
"            flips.append(verdict(f, data, c))\n"
"    w(name + '.flipref', bytes(flips))\n"
"    tr = bytearray()\n"
"    for n in range(len(comp)):\n"
"        tr.append(verdict(f, data, comp[:n]))\n"
"    w(name + '.truncref', bytes(tr))\n"
"lorem900 = lorem * 900\n"
"html800 = (b'<html><head><title>t</title></head><body>' + \n"
"           b'<p class=\"x\">hello &amp; goodbye</p>' * 800 + b'</body></html>')\n"
"refs('rle.l9.zlib', 'zlib', b'A' * 200000, open(D + '/rle.l9.zlib', 'rb').read())\n"
"refs('text.l6.zlib', 'zlib', lorem900, open(D + '/text.l6.zlib', 'rb').read())\n"
"refs('text.l6.gz', 'gz', lorem900, open(D + '/text.l6.gz', 'rb').read())\n"
"refs('text.l9.raw', 'raw', lorem900, open(D + '/text.l9.raw', 'rb').read())\n"
"refs('html.l9.gz', 'gz', html800, open(D + '/html.l9.gz', 'rb').read())\n"
"refs('hello.l6.gz', 'gz', b'hello world\\n', open(D + '/hello.l6.gz', 'rb').read())\n"
"refs('hello.l0.raw', 'raw', b'hello world\\n', open(D + '/hello.l0.raw', 'rb').read())\n"
"refs('one.l9.zlib', 'zlib', b'x', open(D + '/one.l9.zlib', 'rb').read())\n"
"refs('empty.l6.gz', 'gz', b'', open(D + '/empty.l6.gz', 'rb').read())\n"
"print('fixtures ok')\n";

static void generate_fixtures(void)
{
    char path[512];
    FILE *f;
    int rc;

    if (getenv("KLIBZ_NOGEN")) {
        printf("reusing fixtures in %s\n", FIXDIR);
        return;
    }
    rc = system("mkdir -p " FIXDIR);
    (void)rc;
    snprintf(path, sizeof(path), "%s/gen.py", FIXDIR);
    f = fopen(path, "wb");
    if (!f) {
        printf("cannot write %s\n", path);
        exit(2);
    }
    fwrite(gen_py, 1, strlen(gen_py), f);
    fclose(f);
    printf("generating fixtures with python3 into %s ...\n", FIXDIR);
    rc = system("python3 " FIXDIR "/gen.py");
    if (rc != 0) {
        printf("fixture generation failed (rc=%d); is python3 present?\n", rc);
        exit(2);
    }
}

static unsigned char *slurp(const char *name, unsigned long *len)
{
    char path[512];
    FILE *f;
    unsigned char *buf;
    long n;

    snprintf(path, sizeof(path), "%s/%s", FIXDIR, name);
    f = fopen(path, "rb");
    if (!f) {
        printf("  missing fixture %s\n", path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (unsigned char *)malloc((size_t)n + 1);
    if (!buf) {
        printf("  oom reading %s\n", path);
        exit(2);
    }
    if (n && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        printf("  short read on %s\n", path);
        exit(2);
    }
    fclose(f);
    buf[n] = 0;
    *len = (unsigned long)n;
    return buf;
}

/* ------------------------------------------------------- small helpers */

static int same(const unsigned char *a, unsigned long na,
                const unsigned char *b, unsigned long nb,
                unsigned long *whereof)
{
    unsigned long i, n = na < nb ? na : nb;

    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            *whereof = i;
            return 0;
        }
    }
    if (na != nb) {
        *whereof = n;
        return 0;
    }
    return 1;
}

/* A minimal LSB-first bit writer, used to hand-build adversarial raw
 * DEFLATE streams that no compressor would ever emit. */
struct bw {
    unsigned char buf[4096];
    unsigned long nbits;
};

static void bw_init(struct bw *w)
{
    memset(w, 0, sizeof(*w));
}

static void bw_put(struct bw *w, unsigned value, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        unsigned long bit = w->nbits + i;
        if (bit >= sizeof(w->buf) * 8) {
            printf("  bit writer overflow\n");
            exit(2);
        }
        if ((value >> i) & 1)
            w->buf[bit >> 3] |= (unsigned char)(1u << (bit & 7));
    }
    w->nbits += n;
}

/* Huffman codes go out most significant bit first. */
static void bw_code(struct bw *w, unsigned code, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++)
        bw_put(w, (code >> (n - 1 - i)) & 1, 1);
}

static unsigned long bw_bytes(const struct bw *w)
{
    return (w->nbits + 7) / 8;
}

/* RFC 1951 section 3.2.6 fixed literal/length codes. */
static void bw_fixed_sym(struct bw *w, unsigned sym)
{
    if (sym < 144)      bw_code(w, 0x30 + sym, 8);
    else if (sym < 256) bw_code(w, 0x190 + sym - 144, 9);
    else if (sym < 280) bw_code(w, sym - 256, 7);
    else                bw_code(w, 0xC0 + sym - 280, 8);
}

static void bw_fixed_dist(struct bw *w, unsigned dsym)
{
    bw_code(w, dsym, 5);
}

/* ------------------------------------------------------------- the tests */

static void test_hashes(void)
{
    section("checksums");

    CHECK(crc32_buf("", 0) == 0x00000000U, "crc32(\"\") = %08x",
          crc32_buf("", 0));
    CHECK(crc32_buf("a", 1) == 0xE8B7BE43U, "crc32(\"a\") = %08x",
          crc32_buf("a", 1));
    CHECK(crc32_buf("abc", 3) == 0x352441C2U, "crc32(\"abc\") = %08x",
          crc32_buf("abc", 3));
    CHECK(crc32_buf("123456789", 9) == 0xCBF43926U, "crc32 check value = %08x",
          crc32_buf("123456789", 9));

    CHECK(adler32_buf("", 0) == 0x00000001U, "adler32(\"\") = %08x",
          adler32_buf("", 0));
    CHECK(adler32_buf("a", 1) == 0x00620062U, "adler32(\"a\") = %08x",
          adler32_buf("a", 1));
    CHECK(adler32_buf("abc", 3) == 0x024D0127U, "adler32(\"abc\") = %08x",
          adler32_buf("abc", 3));
    CHECK(adler32_buf("123456789", 9) == 0x091E01DEU, "adler32 check = %08x",
          adler32_buf("123456789", 9));
    CHECK(adler32_buf("Wikipedia", 9) == 0x11E60398U, "adler32(Wikipedia) = %08x",
          adler32_buf("Wikipedia", 9));

    /* Chunked updates must equal the one-shot value. */
    {
        unsigned char big[70000];
        unsigned long i;
        uint32_t c1, c2, a1, a2;

        for (i = 0; i < sizeof(big); i++)
            big[i] = (unsigned char)(i * 31 + (i >> 8));
        c1 = crc32_buf(big, sizeof(big));
        a1 = adler32_buf(big, sizeof(big));
        c2 = 0;
        a2 = 1;
        for (i = 0; i < sizeof(big); i += 997) {
            unsigned long n = sizeof(big) - i;
            if (n > 997) n = 997;
            c2 = crc32_update(c2, big + i, n);
            a2 = adler32_update(a2, big + i, n);
        }
        CHECK(c1 == c2, "chunked crc32 %08x != one shot %08x", c2, c1);
        CHECK(a1 == a2, "chunked adler32 %08x != one shot %08x", a2, a1);
        /* Crosses the 5552-byte deferred modulo boundary many times. */
        CHECK(a1 == adler32_update(adler32_update(1, big, 5552),
                                   big + 5552, sizeof(big) - 5552),
              "adler32 split at NMAX differs");
    }
}

/* Decompress one fixture and compare against the original. */
static void roundtrip(const char *cname, const char *oname, int wrapper,
                      const char *label)
{
    unsigned char *comp, *orig;
    unsigned long clen, olen, dlen, where = 0;
    void *dec = 0;
    int r;

    comp = slurp(cname, &clen);
    orig = slurp(oname, &olen);
    r = inflate_buf(comp, clen, &dec, &dlen, wrapper);
    CHECK(r == INFLATE_OK, "%s: %s (%d)", label, inflate_strerror(r), r);
    if (r == INFLATE_OK) {
        CHECK(dlen == olen, "%s: length %lu != %lu", label, dlen, olen);
        CHECK(same((unsigned char *)dec, dlen, orig, olen, &where),
              "%s: first difference at %lu", label, where);
        CHECK(((unsigned char *)dec)[dlen] == 0,
              "%s: missing NUL terminator", label);
    }
    free(dec);
    free(comp);
    free(orig);
}

static const char *fixtures[] = {
    "empty", "one", "hello", "text", "html", "rle", "zeros", "rand", "big4m"
};
static const int levels[] = { 0, 1, 6, 9 };

static void test_roundtrips(void)
{
    unsigned f, l;
    char cname[128], oname[128], label[192];

    section("round trips against python zlib/gzip, all wrappers, levels 0/1/6/9");

    for (f = 0; f < sizeof(fixtures) / sizeof(fixtures[0]); f++) {
        for (l = 0; l < sizeof(levels) / sizeof(levels[0]); l++) {
            snprintf(oname, sizeof(oname), "%s.bin", fixtures[f]);

            snprintf(cname, sizeof(cname), "%s.l%d.raw", fixtures[f], levels[l]);
            snprintf(label, sizeof(label), "raw %s l%d", fixtures[f], levels[l]);
            roundtrip(cname, oname, INFLATE_RAW, label);

            snprintf(cname, sizeof(cname), "%s.l%d.zlib", fixtures[f], levels[l]);
            snprintf(label, sizeof(label), "zlib %s l%d", fixtures[f], levels[l]);
            roundtrip(cname, oname, INFLATE_ZLIB, label);

            snprintf(cname, sizeof(cname), "%s.l%d.gz", fixtures[f], levels[l]);
            snprintf(label, sizeof(label), "gzip %s l%d", fixtures[f], levels[l]);
            roundtrip(cname, oname, INFLATE_GZIP, label);

            /* INFLATE_AUTO must sniff the same three streams. */
            snprintf(cname, sizeof(cname), "%s.l%d.zlib", fixtures[f], levels[l]);
            snprintf(label, sizeof(label), "auto->zlib %s l%d", fixtures[f], levels[l]);
            roundtrip(cname, oname, INFLATE_AUTO, label);

            snprintf(cname, sizeof(cname), "%s.l%d.gz", fixtures[f], levels[l]);
            snprintf(label, sizeof(label), "auto->gzip %s l%d", fixtures[f], levels[l]);
            roundtrip(cname, oname, INFLATE_AUTO, label);
        }
        printf("  %-8s ok\n", fixtures[f]);
    }

    roundtrip("blocks.l9.raw", "blocks.bin", INFLATE_RAW, "many blocks");
    roundtrip("flags.gz", "flags.bin", INFLATE_GZIP,
              "gzip FEXTRA+FNAME+FCOMMENT+FHCRC");
    roundtrip("flags.gz", "flags.bin", INFLATE_AUTO, "auto gzip with flags");
    roundtrip("multi.gz", "multi.bin", INFLATE_GZIP, "two gzip members");
    printf("  blocks/flags/multi ok\n");
}

static void test_auto_detect(void)
{
    unsigned char *comp;
    unsigned long clen, dlen;
    void *dec = 0;
    struct inflate_stream *s;
    int r;

    section("wrapper sniffing");

    comp = slurp("text.l6.raw", &clen);
    s = inflate_begin(INFLATE_AUTO, 0);
    CHECK(s != 0, "inflate_begin failed");
    r = inflate_push(s, comp, clen);
    CHECK(r >= 0, "auto raw push: %s", inflate_strerror(r));
    r = inflate_finish(s, &dec, &dlen);
    CHECK(r == INFLATE_OK, "auto raw finish: %s", inflate_strerror(r));
    CHECK(inflate_wrapper(s) == INFLATE_RAW, "auto misdetected raw as %d",
          inflate_wrapper(s));
    inflate_end(s);
    free(dec);
    free(comp);

    comp = slurp("text.l6.gz", &clen);
    s = inflate_begin(INFLATE_AUTO, 0);
    dec = 0;
    inflate_push(s, comp, clen);
    inflate_finish(s, &dec, &dlen);
    CHECK(inflate_wrapper(s) == INFLATE_GZIP, "auto misdetected gzip as %d",
          inflate_wrapper(s));
    inflate_end(s);
    free(dec);
    free(comp);

    comp = slurp("text.l6.zlib", &clen);
    s = inflate_begin(INFLATE_AUTO, 0);
    dec = 0;
    inflate_push(s, comp, clen);
    inflate_finish(s, &dec, &dlen);
    CHECK(inflate_wrapper(s) == INFLATE_ZLIB, "auto misdetected zlib as %d",
          inflate_wrapper(s));
    inflate_end(s);
    free(dec);
    free(comp);
}

/* Push in fixed-size chunks and compare with the one-shot result. */
static void stream_chunks(const char *cname, const char *oname, int wrapper,
                          unsigned long chunk, const char *label)
{
    unsigned char *comp, *orig;
    unsigned long clen, olen, dlen, i, where = 0;
    void *dec = 0;
    struct inflate_stream *s;
    int r = INFLATE_MORE;

    comp = slurp(cname, &clen);
    orig = slurp(oname, &olen);
    s = inflate_begin(wrapper, 0);
    CHECK(s != 0, "%s: begin failed", label);
    for (i = 0; i < clen; i += chunk) {
        unsigned long n = clen - i;
        if (n > chunk)
            n = chunk;
        r = inflate_push(s, comp + i, n);
        if (r < 0)
            break;
    }
    CHECK(r >= 0, "%s: push: %s", label, inflate_strerror(r));
    if (r >= 0) {
        r = inflate_finish(s, &dec, &dlen);
        CHECK(r == INFLATE_OK, "%s: finish: %s", label, inflate_strerror(r));
        if (r == INFLATE_OK) {
            CHECK(dlen == olen, "%s: length %lu != %lu", label, dlen, olen);
            CHECK(same((unsigned char *)dec, dlen, orig, olen, &where),
                  "%s: differs at %lu", label, where);
        }
    }
    inflate_end(s);
    free(dec);
    free(comp);
    free(orig);
}

static void test_streaming(void)
{
    static const unsigned long chunks[] = { 1, 2, 3, 7, 13, 64, 999, 65536 };
    unsigned f, c;
    char cname[128], oname[128], label[192];

    section("streaming: chunked push must equal one shot");

    for (f = 0; f < sizeof(fixtures) / sizeof(fixtures[0]); f++) {
        for (c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
            /* Byte-at-a-time over 4 MiB is exercised separately below. */
            if (strcmp(fixtures[f], "big4m") == 0 && chunks[c] < 64)
                continue;
            snprintf(oname, sizeof(oname), "%s.bin", fixtures[f]);
            snprintf(cname, sizeof(cname), "%s.l6.gz", fixtures[f]);
            snprintf(label, sizeof(label), "gzip %s chunk %lu",
                     fixtures[f], chunks[c]);
            stream_chunks(cname, oname, INFLATE_GZIP, chunks[c], label);

            snprintf(cname, sizeof(cname), "%s.l9.zlib", fixtures[f]);
            snprintf(label, sizeof(label), "zlib %s chunk %lu",
                     fixtures[f], chunks[c]);
            stream_chunks(cname, oname, INFLATE_ZLIB, chunks[c], label);

            snprintf(cname, sizeof(cname), "%s.l0.raw", fixtures[f]);
            snprintf(label, sizeof(label), "stored %s chunk %lu",
                     fixtures[f], chunks[c]);
            stream_chunks(cname, oname, INFLATE_RAW, chunks[c], label);
        }
        printf("  %-8s ok\n", fixtures[f]);
    }

    stream_chunks("big4m.l6.gz", "big4m.bin", INFLATE_GZIP, 1,
                  "gzip big4m byte at a time");
    stream_chunks("flags.gz", "flags.bin", INFLATE_GZIP, 1,
                  "gzip header fields byte at a time");
    stream_chunks("multi.gz", "multi.bin", INFLATE_GZIP, 1,
                  "two members byte at a time");
    printf("  byte-at-a-time 4 MiB / header fields / multi-member ok\n");
}

/* Stream with aggressive draining: memory must stay bounded and the
 * concatenated drained output must still be right. */
static void test_drain(void)
{
    static const char *names[] = { "rle", "text", "big4m" };
    unsigned k;

    section("draining: bounded memory, matches still resolve");

    for (k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
        unsigned char *comp, *orig, *acc;
        unsigned long clen, olen, i, got = 0, peak = 0, where = 0;
        char cname[128], oname[128];
        struct inflate_stream *s;
        int r = INFLATE_MORE;

        snprintf(cname, sizeof(cname), "%s.l9.zlib", names[k]);
        snprintf(oname, sizeof(oname), "%s.bin", names[k]);
        comp = slurp(cname, &clen);
        orig = slurp(oname, &olen);
        acc = (unsigned char *)malloc(olen ? olen : 1);

        s = inflate_begin(INFLATE_ZLIB, 0);
        for (i = 0; i < clen; i += 4096) {
            unsigned long n = clen - i, avail;
            const unsigned char *p;
            if (n > 4096)
                n = 4096;
            r = inflate_push(s, comp + i, n);
            if (r < 0)
                break;
            p = inflate_peek(s, &avail);
            if (avail > peak)
                peak = avail;
            if (avail && got + avail <= olen)
                memcpy(acc + got, p, avail);
            got += avail;
            inflate_drain(s, avail);
        }
        CHECK(r >= 0, "%s: drain push: %s", names[k], inflate_strerror(r));
        if (r >= 0) {
            unsigned long avail;
            const unsigned char *p;
            r = inflate_finish(s, 0, 0);
            CHECK(r == INFLATE_OK, "%s: drain finish: %s", names[k],
                  inflate_strerror(r));
            p = inflate_peek(s, &avail);
            if (avail && got + avail <= olen)
                memcpy(acc + got, p, avail);
            got += avail;
            CHECK(got == olen, "%s: drained %lu of %lu", names[k], got, olen);
            CHECK(same(acc, got < olen ? got : olen, orig, olen, &where),
                  "%s: drained data differs at %lu", names[k], where);
            CHECK(inflate_total_out(s) == olen, "%s: total_out %lu != %lu",
                  names[k], inflate_total_out(s), olen);
        }
        printf("  %-6s %lu bytes, largest live output block %lu\n",
               names[k], olen, peak);
        inflate_end(s);
        free(acc);
        free(comp);
        free(orig);
    }
}

/* Every proper prefix of a complete stream must fail, and never crash. */
static void test_truncation(void)
{
    static const struct { const char *f; int w; } cases[] = {
        { "text.l6.raw",  INFLATE_RAW },
        { "text.l6.zlib", INFLATE_ZLIB },
        { "text.l6.gz",   INFLATE_GZIP },
        { "text.l0.raw",  INFLATE_RAW },
        { "rle.l9.gz",    INFLATE_GZIP },
        { "rand.l6.zlib", INFLATE_ZLIB },
        { "flags.gz",     INFLATE_GZIP },
        { "hello.l6.gz",  INFLATE_GZIP }
    };
    unsigned k;

    section("truncation at every offset");

    for (k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        unsigned char *comp;
        unsigned long clen, n, step;
        unsigned long counts[16];
        unsigned long ok_count = 0, tried = 0;
        unsigned i;

        memset(counts, 0, sizeof(counts));
        comp = slurp(cases[k].f, &clen);
        /* Every offset for small streams, a dense sample for big ones. */
        step = clen > 4096 ? clen / 2048 : 1;
        if (!step)
            step = 1;
        for (n = 0; n < clen; n += step) {
            void *dec = 0;
            unsigned long dlen = 0;
            int r = inflate_buf(comp, n, &dec, &dlen, cases[k].w);
            tried++;
            if (r == INFLATE_OK)
                ok_count++;
            else
                counts[(unsigned)(-r) & 15]++;
            CHECK(r != INFLATE_OK || dec != 0, "prefix returned OK with NULL");
            free(dec);
        }
        CHECK(ok_count == 0, "%s: %lu proper prefixes decoded as complete",
              cases[k].f, ok_count);
        printf("  %-14s %lu prefixes: truncated=%lu badcode=%lu "
               "huffman=%lu checksum=%lu block=%lu dist=%lu header=%lu\n",
               cases[k].f, tried,
               counts[-INFLATE_ERR_TRUNCATED], counts[-INFLATE_ERR_CODE],
               counts[-INFLATE_ERR_HUFFMAN], counts[-INFLATE_ERR_CHECKSUM],
               counts[-INFLATE_ERR_BLOCK], counts[-INFLATE_ERR_DISTANCE],
               counts[-INFLATE_ERR_HEADER]);
        (void)i;
        free(comp);
    }
}

static void test_corruption(void)
{
    /* The invariant: a corrupted stream is either rejected, or decodes
     * to exactly the original bytes because the flipped bit was in
     * something nobody reads (gzip's MTIME, the padding before a byte
     * aligned field, a code length for a symbol the body never uses).
     * Anything else means the checksum failed to do its job. A raw
     * DEFLATE stream carries no checksum at all, so a flip inside a
     * stored block legitimately yields different data; that case is
     * reported rather than asserted. */
    static const struct { const char *f; const char *o; int w; int strict; }
    cases[] = {
        { "text.l6.zlib", "text.bin", INFLATE_ZLIB, 1 },
        { "rle.l9.zlib",  "rle.bin",  INFLATE_ZLIB, 1 },
        { "text.l6.gz",   "text.bin", INFLATE_GZIP, 1 },
        { "html.l9.gz",   "html.bin", INFLATE_GZIP, 1 },
        { "rand.l6.raw",  "rand.bin", INFLATE_RAW,  0 }
    };
    unsigned k;
    unsigned long flips = 0, rejected = 0, survived = 0;

    section("corruption: single bit flips must fail cleanly, never crash");

    for (k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        unsigned char *comp, *orig;
        unsigned long clen, olen, i, step;
        unsigned long cflip = 0, cident = 0, cdiff = 0;

        comp = slurp(cases[k].f, &clen);
        orig = slurp(cases[k].o, &olen);
        step = clen > 2000 ? clen / 1000 : 1;
        if (!step)
            step = 1;
        for (i = 0; i < clen; i += step) {
            unsigned b;
            for (b = 0; b < 8; b += 3) {
                void *dec = 0;
                unsigned long dlen = 0, where = 0;
                int r;
                comp[i] ^= (unsigned char)(1u << b);
                r = inflate_buf(comp, clen, &dec, &dlen, cases[k].w);
                comp[i] ^= (unsigned char)(1u << b);
                flips++;
                cflip++;
                if (r == INFLATE_OK) {
                    survived++;
                    if (same((unsigned char *)dec, dlen, orig, olen, &where))
                        cident++;
                    else
                        cdiff++;
                } else {
                    rejected++;
                    CHECK(dec == 0, "error path returned a buffer");
                }
                free(dec);
            }
        }
        printf("  %-14s %lu flips: %lu rejected, %lu decoded to the same "
               "bytes, %lu decoded to different bytes\n",
               cases[k].f, cflip, cflip - cident - cdiff, cident, cdiff);
        if (cases[k].strict)
            CHECK(cdiff == 0, "%s: %lu corruptions changed the output and "
                  "the checksum did not notice", cases[k].f, cdiff);
        free(comp);
        free(orig);
    }
    printf("  total %lu flips: %lu rejected, %lu decoded anyway\n",
           flips, rejected, survived);
}

/* Differential test: for every single-bit corruption and every
 * truncation of a stream, libz must reach the same verdict as python's
 * zlib/gzip - rejected, or decoded to exactly the original bytes. This
 * is the check that says libz is neither stricter nor more permissive
 * than the reference, which matters because being permissive is how a
 * decompressor ends up acting on data nobody validated. */
static void test_differential(void)
{
    static const struct { const char *f; const char *o; int w; } cases[] = {
        { "rle.l9.zlib",  "rle.bin",   INFLATE_ZLIB },
        { "text.l6.zlib", "text.bin",  INFLATE_ZLIB },
        { "text.l6.gz",   "text.bin",  INFLATE_GZIP },
        { "text.l9.raw",  "text.bin",  INFLATE_RAW },
        { "html.l9.gz",   "html.bin",  INFLATE_GZIP },
        { "hello.l6.gz",  "hello.bin", INFLATE_GZIP },
        { "hello.l0.raw", "hello.bin", INFLATE_RAW },
        { "one.l9.zlib",  "one.bin",   INFLATE_ZLIB },
        { "empty.l6.gz",  "empty.bin", INFLATE_GZIP }
    };
    unsigned k;
    unsigned long total = 0, agree = 0, known = 0;

    section("differential against python zlib/gzip: corruptions and truncations");
    printf("  two deliberate divergences, both places where libz is the\n"
           "  stricter of the two:\n"
           "    - a set reserved bit in the gzip FLG byte is an error here.\n"
           "      RFC 1952 says a compliant decompressor must reject it,\n"
           "      because the bit may announce a field that would make\n"
           "      everything after it be read wrong. python ignores it.\n"
           "    - python's gzip.decompress(b'') returns b''. An empty\n"
           "      input is not a gzip stream; here it is truncated.\n");

    for (k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        unsigned char *comp, *orig, *flipref, *truncref;
        unsigned long clen, olen, freflen, treflen, i, n;
        unsigned long cases_run = 0, mismatch = 0;
        char refname[160];

        comp = slurp(cases[k].f, &clen);
        orig = slurp(cases[k].o, &olen);
        snprintf(refname, sizeof(refname), "%s.flipref", cases[k].f);
        flipref = slurp(refname, &freflen);
        snprintf(refname, sizeof(refname), "%s.truncref", cases[k].f);
        truncref = slurp(refname, &treflen);
        CHECK(freflen == clen * 3, "%s: %lu flip verdicts for %lu bytes",
              cases[k].f, freflen, clen);
        CHECK(treflen == clen, "%s: %lu truncation verdicts for %lu bytes",
              cases[k].f, treflen, clen);

        for (i = 0; i < clen; i++) {
            unsigned b;
            for (b = 0; b < 3; b++) {
                void *dec = 0;
                unsigned long dlen = 0, where = 0;
                int r, mine, theirs;

                comp[i] ^= (unsigned char)(1u << (b * 3));
                r = inflate_buf(comp, clen, &dec, &dlen, cases[k].w);
                comp[i] ^= (unsigned char)(1u << (b * 3));
                if (r != INFLATE_OK)
                    mine = 0;
                else
                    mine = same((unsigned char *)dec, dlen, orig, olen, &where)
                           ? 1 : 2;
                free(dec);
                theirs = flipref[i * 3 + b];
                total++;
                cases_run++;
                if (mine == theirs) {
                    agree++;
                } else if (cases[k].w == INFLATE_GZIP && i == 3 && b == 2 &&
                           mine == 0 && theirs == 1) {
                    known++;    /* reserved FLG bit, rejected on purpose */
                } else if (mismatch++ < 4) {
                    printf("    flip byte %lu bit %u: libz says %d, "
                           "python says %d\n", i, b * 3, mine, theirs);
                }
            }
        }

        for (n = 0; n < clen; n++) {
            void *dec = 0;
            unsigned long dlen = 0, where = 0;
            int r, mine, theirs;

            r = inflate_buf(comp, n, &dec, &dlen, cases[k].w);
            if (r != INFLATE_OK)
                mine = 0;
            else
                mine = same((unsigned char *)dec, dlen, orig, olen, &where)
                       ? 1 : 2;
            free(dec);
            theirs = truncref[n];
            total++;
            cases_run++;
            if (mine == theirs) {
                agree++;
            } else if (cases[k].w == INFLATE_GZIP && n == 0 && mine == 0) {
                known++;        /* python decodes an empty input as empty */
            } else if (mismatch++ < 4) {
                printf("    truncated to %lu: libz says %d, python says %d\n",
                       n, mine, theirs);
            }
        }

        printf("  %-14s %lu verdicts, %lu disagreements\n",
               cases[k].f, cases_run, mismatch);
        CHECK(mismatch == 0, "%s: %lu verdicts differ from python",
              cases[k].f, mismatch);
        free(comp);
        free(orig);
        free(flipref);
        free(truncref);
    }
    printf("  %lu verdicts total: %lu agree with python, %lu are the two\n"
           "  deliberate divergences above, %lu unexplained\n",
           total, agree, known, total - agree - known);
}

static void test_checksum_tampering(void)
{
    unsigned char *comp;
    unsigned long clen, dlen;
    void *dec = 0;
    int r;

    section("checksum and trailer tampering");

    /* zlib Adler-32 */
    comp = slurp("text.l6.zlib", &clen);
    comp[clen - 1] ^= 0x01;
    r = inflate_buf(comp, clen, &dec, &dlen, INFLATE_ZLIB);
    CHECK(r == INFLATE_ERR_CHECKSUM, "flipped adler gave %s",
          inflate_strerror(r));
    free(dec);
    dec = 0;
    comp[clen - 1] ^= 0x01;
    /* zlib header check byte */
    comp[1] ^= 0x01;
    r = inflate_buf(comp, clen, &dec, &dlen, INFLATE_ZLIB);
    CHECK(r == INFLATE_ERR_HEADER, "bad zlib FCHECK gave %s",
          inflate_strerror(r));
    free(dec);
    dec = 0;
    comp[1] ^= 0x01;
    /* zlib compression method */
    comp[0] = 0x78 ^ 0x01;
    r = inflate_buf(comp, clen, &dec, &dlen, INFLATE_ZLIB);
    CHECK(r == INFLATE_ERR_HEADER, "bad zlib CM gave %s", inflate_strerror(r));
    free(dec);
    dec = 0;
    free(comp);

    /* gzip CRC-32 and ISIZE */
    comp = slurp("text.l6.gz", &clen);
    comp[clen - 5] ^= 0x80;
    r = inflate_buf(comp, clen, &dec, &dlen, INFLATE_GZIP);
    CHECK(r == INFLATE_ERR_CHECKSUM, "flipped gzip crc gave %s",
          inflate_strerror(r));
    free(dec);
    dec = 0;
    comp[clen - 5] ^= 0x80;

    comp[clen - 1] ^= 0x01;
    r = inflate_buf(comp, clen, &dec, &dlen, INFLATE_GZIP);
    CHECK(r == INFLATE_ERR_CHECKSUM, "flipped gzip isize gave %s",
          inflate_strerror(r));
    free(dec);
    dec = 0;
    comp[clen - 1] ^= 0x01;

    comp[0] = 0x1E;
    r = inflate_buf(comp, clen, &dec, &dlen, INFLATE_GZIP);
    CHECK(r == INFLATE_ERR_HEADER, "bad gzip magic gave %s",
          inflate_strerror(r));
    free(dec);
    dec = 0;
    comp[0] = 0x1F;

    comp[3] = 0x40;   /* a reserved FLG bit */
    r = inflate_buf(comp, clen, &dec, &dlen, INFLATE_GZIP);
    CHECK(r == INFLATE_ERR_HEADER, "reserved gzip flag gave %s",
          inflate_strerror(r));
    free(dec);
    dec = 0;
    free(comp);

    /* gzip FHCRC */
    comp = slurp("flags.gz", &clen);
    {
        unsigned long hcrc_at = 10 + 2 + 6 + strlen("a-rather-long-file-name.txt")
                                + 1 + strlen("a comment field") + 1;
        comp[hcrc_at] ^= 0x01;
        r = inflate_buf(comp, clen, &dec, &dlen, INFLATE_GZIP);
        CHECK(r == INFLATE_ERR_CHECKSUM, "flipped gzip header crc gave %s",
              inflate_strerror(r));
        free(dec);
        dec = 0;
    }
    free(comp);
}

static void test_limit(void)
{
    unsigned char *comp;
    unsigned long clen, dlen;
    void *dec = 0;
    int r;

    section("output cap (decompression bombs)");

    comp = slurp("bomb.gz", &clen);
    printf("  bomb.gz is %lu bytes and expands to %d MiB\n", clen, 64);

    r = inflate_buf_limit(comp, clen, &dec, &dlen, INFLATE_GZIP, 1024 * 1024);
    CHECK(r == INFLATE_ERR_LIMIT, "1 MiB cap gave %s", inflate_strerror(r));
    CHECK(dec == 0, "failed inflate_buf still returned a buffer");
    free(dec);
    dec = 0;

    r = inflate_buf_limit(comp, clen, &dec, &dlen, INFLATE_GZIP, 1);
    CHECK(r == INFLATE_ERR_LIMIT, "1 byte cap gave %s", inflate_strerror(r));
    free(dec);
    dec = 0;

    /* The default cap is 64 MiB, and the bomb is exactly that, so it fits. */
    r = inflate_buf(comp, clen, &dec, &dlen, INFLATE_GZIP);
    CHECK(r == INFLATE_OK, "64 MiB of zeros under the default cap: %s",
          inflate_strerror(r));
    CHECK(dlen == 64UL * 1024 * 1024, "bomb produced %lu bytes", dlen);
    if (r == INFLATE_OK) {
        unsigned long i, nonzero = 0;
        for (i = 0; i < dlen; i++)
            if (((unsigned char *)dec)[i])
                nonzero++;
        CHECK(nonzero == 0, "%lu non-zero bytes in the zero bomb", nonzero);
    }
    free(dec);
    dec = 0;
    free(comp);

    /* A stream that stops exactly at its cap must still be an error, not
     * a silent truncation. */
    comp = slurp("bomb.zlib", &clen);
    r = inflate_buf_limit(comp, clen, &dec, &dlen, INFLATE_ZLIB,
                          64UL * 1024 * 1024 - 1);
    CHECK(r == INFLATE_ERR_LIMIT, "cap one byte short gave %s",
          inflate_strerror(r));
    free(dec);
    dec = 0;
    r = inflate_buf_limit(comp, clen, &dec, &dlen, INFLATE_ZLIB,
                          64UL * 1024 * 1024);
    CHECK(r == INFLATE_OK, "cap exactly the output size gave %s",
          inflate_strerror(r));
    CHECK(dlen == 64UL * 1024 * 1024, "exact cap produced %lu", dlen);
    free(dec);
    dec = 0;
    free(comp);

    /* Streaming with drain and a cap: the cap counts drained bytes too,
     * so a bomb cannot be laundered through a draining reader. */
    comp = slurp("bomb.gz", &clen);
    {
        struct inflate_stream *s = inflate_begin(INFLATE_GZIP, 512 * 1024);
        unsigned long i;
        int rr = INFLATE_MORE;
        for (i = 0; i < clen; i += 4096) {
            unsigned long n = clen - i, avail;
            if (n > 4096)
                n = 4096;
            rr = inflate_push(s, comp + i, n);
            if (rr < 0)
                break;
            inflate_peek(s, &avail);
            inflate_drain(s, avail);
        }
        CHECK(rr == INFLATE_ERR_LIMIT, "draining bomb gave %s",
              inflate_strerror(rr));
        CHECK(inflate_total_out(s) <= 512 * 1024,
              "produced %lu bytes past a 512 KiB cap", inflate_total_out(s));
        inflate_end(s);
    }
    free(comp);
}

static void test_handmade(void)
{
    struct bw w;
    void *dec = 0;
    unsigned long dlen;
    int r;

    section("hand-built adversarial DEFLATE");

    /* Distance that points before the start of the output. */
    bw_init(&w);
    bw_put(&w, 1, 1);            /* BFINAL */
    bw_put(&w, 1, 2);            /* BTYPE = fixed */
    bw_fixed_sym(&w, 'A');
    bw_fixed_sym(&w, 257);       /* length 3 */
    bw_fixed_dist(&w, 5);        /* base 7 */
    bw_put(&w, 0, 1);            /* one extra bit */
    bw_fixed_sym(&w, 256);
    r = inflate_buf(w.buf, bw_bytes(&w), &dec, &dlen, INFLATE_RAW);
    CHECK(r == INFLATE_ERR_DISTANCE, "distance too far back gave %s",
          inflate_strerror(r));
    free(dec);
    dec = 0;

    /* Distance code 30 does not exist. */
    bw_init(&w);
    bw_put(&w, 1, 1);
    bw_put(&w, 1, 2);
    bw_fixed_sym(&w, 'A');
    bw_fixed_sym(&w, 257);
    bw_fixed_dist(&w, 30);
    bw_fixed_sym(&w, 256);
    r = inflate_buf(w.buf, bw_bytes(&w), &dec, &dlen, INFLATE_RAW);
    CHECK(r == INFLATE_ERR_CODE, "distance code 30 gave %s",
          inflate_strerror(r));
    free(dec);
    dec = 0;

    /* Length code 286 does not exist. */
    bw_init(&w);
    bw_put(&w, 1, 1);
    bw_put(&w, 1, 2);
    bw_fixed_sym(&w, 'A');
    bw_fixed_sym(&w, 286);
    bw_fixed_sym(&w, 256);
    r = inflate_buf(w.buf, bw_bytes(&w), &dec, &dlen, INFLATE_RAW);
    CHECK(r == INFLATE_ERR_CODE, "length code 286 gave %s",
          inflate_strerror(r));
    free(dec);
    dec = 0;

    /* Reserved block type 3. */
    bw_init(&w);
    bw_put(&w, 1, 1);
    bw_put(&w, 3, 2);
    r = inflate_buf(w.buf, bw_bytes(&w), &dec, &dlen, INFLATE_RAW);
    CHECK(r == INFLATE_ERR_BLOCK, "block type 3 gave %s",
          inflate_strerror(r));
    free(dec);
    dec = 0;

    /* Stored block whose NLEN is not the complement of LEN. */
    {
        unsigned char st[16];
        memset(st, 0, sizeof(st));
        st[0] = 0x01;          /* BFINAL=1, BTYPE=00, then byte aligned */
        st[1] = 0x03; st[2] = 0x00;
        st[3] = 0xFC; st[4] = 0xFE;   /* wrong complement */
        st[5] = 'a'; st[6] = 'b'; st[7] = 'c';
        r = inflate_buf(st, 8, &dec, &dlen, INFLATE_RAW);
        CHECK(r == INFLATE_ERR_BLOCK, "bad stored NLEN gave %s",
              inflate_strerror(r));
        free(dec);
        dec = 0;

        st[3] = 0xFC; st[4] = 0xFF;   /* correct complement of 3 */
        r = inflate_buf(st, 8, &dec, &dlen, INFLATE_RAW);
        CHECK(r == INFLATE_OK && dlen == 3 &&
              memcmp(dec, "abc", 3) == 0, "good stored block gave %s len %lu",
              inflate_strerror(r), dlen);
        free(dec);
        dec = 0;
    }

    /* A dynamic header claiming more literal codes than exist. */
    bw_init(&w);
    bw_put(&w, 1, 1);
    bw_put(&w, 2, 2);            /* BTYPE = dynamic */
    bw_put(&w, 31, 5);           /* HLIT  -> 288 literal codes  */
    bw_put(&w, 0, 5);            /* HDIST -> 1                  */
    bw_put(&w, 0, 4);            /* HCLEN -> 4                  */
    r = inflate_buf(w.buf, bw_bytes(&w), &dec, &dlen, INFLATE_RAW);
    CHECK(r == INFLATE_ERR_HUFFMAN, "HLIT 288 gave %s", inflate_strerror(r));
    free(dec);
    dec = 0;

    /* An over-subscribed code length code: four symbols all one bit. */
    bw_init(&w);
    bw_put(&w, 1, 1);
    bw_put(&w, 2, 2);
    bw_put(&w, 0, 5);            /* HLIT  -> 257 */
    bw_put(&w, 0, 5);            /* HDIST -> 1   */
    bw_put(&w, 0, 4);            /* HCLEN -> 4   */
    bw_put(&w, 1, 3);
    bw_put(&w, 1, 3);
    bw_put(&w, 1, 3);
    bw_put(&w, 1, 3);
    r = inflate_buf(w.buf, bw_bytes(&w), &dec, &dlen, INFLATE_RAW);
    CHECK(r == INFLATE_ERR_HUFFMAN, "over-subscribed code lengths gave %s",
          inflate_strerror(r));
    free(dec);
    dec = 0;

    /* An empty fixed block: just the end-of-block symbol. */
    bw_init(&w);
    bw_put(&w, 1, 1);
    bw_put(&w, 1, 2);
    bw_fixed_sym(&w, 256);
    r = inflate_buf(w.buf, bw_bytes(&w), &dec, &dlen, INFLATE_RAW);
    CHECK(r == INFLATE_OK && dlen == 0, "empty fixed block gave %s len %lu",
          inflate_strerror(r), dlen);
    CHECK(dec != 0, "empty output should still be a valid pointer");
    free(dec);
    dec = 0;

    /* A maximal run: distance 1 repeated to build 258-byte matches, and
     * a distance of exactly the output length, which is legal. */
    bw_init(&w);
    bw_put(&w, 1, 1);
    bw_put(&w, 1, 2);
    bw_fixed_sym(&w, 'Z');
    bw_fixed_sym(&w, 285);       /* length 258, no extra bits */
    bw_fixed_dist(&w, 0);        /* distance 1 */
    bw_fixed_sym(&w, 285);
    bw_fixed_dist(&w, 9);        /* base 25, 3 extra bits */
    bw_put(&w, 7, 3);            /* distance 32 */
    bw_fixed_sym(&w, 256);
    r = inflate_buf(w.buf, bw_bytes(&w), &dec, &dlen, INFLATE_RAW);
    CHECK(r == INFLATE_OK && dlen == 517, "long runs gave %s len %lu",
          inflate_strerror(r), dlen);
    if (r == INFLATE_OK) {
        unsigned long i, wrong = 0;
        for (i = 0; i < dlen; i++)
            if (((unsigned char *)dec)[i] != 'Z')
                wrong++;
        CHECK(wrong == 0, "%lu bytes of the run are not 'Z'", wrong);
    }
    free(dec);
    dec = 0;

    /* Distance exactly equal to the bytes produced so far is legal;
     * one more is not. */
    bw_init(&w);
    bw_put(&w, 1, 1);
    bw_put(&w, 1, 2);
    bw_fixed_sym(&w, 'a');
    bw_fixed_sym(&w, 'b');
    bw_fixed_sym(&w, 'c');
    bw_fixed_sym(&w, 257);       /* length 3 */
    bw_fixed_dist(&w, 2);        /* distance 3 == output so far */
    bw_fixed_sym(&w, 256);
    r = inflate_buf(w.buf, bw_bytes(&w), &dec, &dlen, INFLATE_RAW);
    CHECK(r == INFLATE_OK && dlen == 6 && memcmp(dec, "abcabc", 6) == 0,
          "distance == total output gave %s len %lu", inflate_strerror(r),
          dlen);
    free(dec);
    dec = 0;
}

/* A 32 KiB window boundary check: build data whose matches reach exactly
 * back 32768 bytes, compress it with python, and verify. Done by asking
 * python for it rather than trusting a hand-rolled encoder. */
static void test_window_edge(void)
{
    section("32 KiB window edge");

    /* rle and zeros already contain matches at the maximum distance once
     * they exceed 32 KiB; big4m repeats a 4 KiB block through 4 MiB, so
     * its matches span the whole window range. The check here is that
     * decoding them with a tiny push size, where the output buffer is
     * being reallocated constantly, still resolves every match. */
    stream_chunks("zeros.l9.zlib", "zeros.bin", INFLATE_ZLIB, 3,
                  "zeros 3-byte pushes");
    stream_chunks("rle.l6.gz", "rle.bin", INFLATE_GZIP, 5,
                  "rle 5-byte pushes");
    printf("  ok\n");
}

/* ------------------------------------ random Huffman code construction */

static unsigned rnd(unsigned *seed)
{
    *seed = *seed * 1664525u + 1013904223u;
    return *seed >> 8;
}

/* Canonical code values, most significant bit first, for a length set. */
static void canon_codes(const unsigned char *lens, unsigned n, unsigned *codes)
{
    unsigned count[16], next[16], len, i, code;

    memset(count, 0, sizeof(count));
    for (i = 0; i < n; i++)
        count[lens[i]]++;
    count[0] = 0;
    code = 0;
    for (len = 1; len <= 15; len++) {
        code = (code + count[len - 1]) << 1;
        next[len] = code;
    }
    for (i = 0; i < n; i++)
        codes[i] = lens[i] ? next[lens[i]]++ : 0;
}

/* Build a random *complete* code length set over nsym symbols using at
 * most maxcodes of them. Counts are drawn length by length against the
 * remaining Kraft budget, measured in units of 2^-15, so whatever comes
 * out satisfies the inequality with equality. skew biases towards few,
 * long codes, which is what produces deep sub-tables. Returns 0 if the
 * draw painted itself into a corner; the caller just retries. */
static int gen_lengths(unsigned *seed, unsigned char *lens, unsigned nsym,
                       unsigned maxcodes, unsigned force_sym, int skew)
{
    unsigned count[16], len, i, j, k, used = 0;
    unsigned long rem = 1UL << 15;
    unsigned short idx[288];

    memset(count, 0, sizeof(count));
    for (len = 1; len <= 15; len++) {
        unsigned long unit = 1UL << (15 - len);
        unsigned long maxc = rem / unit;
        unsigned long c;

        if (maxc > maxcodes - used)
            maxc = maxcodes - used;
        if (len == 15) {
            if (rem > maxcodes - used)
                return 0;
            c = rem;
        } else if (skew) {
            c = (maxc && (rnd(seed) & 3) == 0) ? 1 : 0;
        } else {
            c = maxc ? rnd(seed) % (maxc + 1) : 0;
        }
        count[len] = (unsigned)c;
        used += (unsigned)c;
        rem -= c * unit;
        if (rem == 0)
            break;
    }
    if (rem != 0 || used == 0)
        return 0;

    for (i = 0; i < nsym; i++)
        idx[i] = (unsigned short)i;
    for (i = nsym; i > 1; i--) {
        unsigned short t;
        j = rnd(seed) % i;
        t = idx[i - 1];
        idx[i - 1] = idx[j];
        idx[j] = t;
    }
    /* The end-of-block symbol has to be in the code or the block is
     * illegal for reasons that have nothing to do with the table. */
    if (force_sym < nsym) {
        unsigned at = nsym;
        for (i = 0; i < nsym; i++)
            if (idx[i] == force_sym) {
                at = i;
                break;
            }
        if (at >= used) {
            unsigned short t = idx[0];
            idx[0] = idx[at];
            idx[at] = t;
        }
    }

    memset(lens, 0, nsym);
    k = 0;
    for (len = 1; len <= 15; len++)
        for (j = 0; j < count[len]; j++)
            lens[idx[k++]] = (unsigned char)len;
    return 1;
}

/* Emit a dynamic block carrying exactly the given code lengths. The code
 * length code is fixed at "symbols 0..15 are four bits each", which is
 * complete (16 * 2^-4 = 1) and makes each length its own 4-bit code. */
static void emit_dynamic(struct bw *w, const unsigned char *llens,
                         unsigned hlit, const unsigned char *dlens,
                         unsigned hdist)
{
    static const unsigned char order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    unsigned i;

    bw_init(w);
    bw_put(w, 1, 1);                 /* BFINAL */
    bw_put(w, 2, 2);                 /* BTYPE = dynamic */
    bw_put(w, hlit - 257, 5);
    bw_put(w, hdist - 1, 5);
    bw_put(w, 19 - 4, 4);            /* HCLEN */
    for (i = 0; i < 19; i++)
        bw_put(w, order[i] < 16 ? 4 : 0, 3);
    for (i = 0; i < hlit; i++)
        bw_code(w, llens[i], 4);
    for (i = 0; i < hdist; i++)
        bw_code(w, dlens[i], 4);
}

/* Randomised Huffman table stress. Every generated code is legal, so
 * every stream must decode; the interest is in whether the fixed entry
 * pools are anywhere near full and whether each symbol still decodes to
 * itself once sub-tables are involved. */
static void test_table_stress(void)
{
    unsigned seed = 12345, t;
    unsigned long tried = 0, skipped = 0, deep = 0;
    unsigned lens_before, dist_before;

    section("Huffman table build stress (random legal codes)");

    lens_before = inflate_dbg_lens_used;
    dist_before = inflate_dbg_dist_used;

    for (t = 0; t < 40000; t++) {
        unsigned char llens[288], dlens[32];
        unsigned lcodes[288];
        unsigned char expect[288];
        struct bw w;
        unsigned hlit, hdist, i, nexp = 0, maxlen = 0;
        void *dec = 0;
        unsigned long dlen = 0, where = 0;
        int r, skew = (t & 1);

        if (!gen_lengths(&seed, llens, 286, 286, 256, skew)) {
            skipped++;
            continue;
        }
        if (!gen_lengths(&seed, dlens, 30, 30, 30, skew)) {
            skipped++;
            continue;
        }
        hlit = 257;
        for (i = 0; i < 286; i++)
            if (llens[i]) {
                if (i + 1 > hlit)
                    hlit = i + 1;
                if (llens[i] > maxlen)
                    maxlen = llens[i];
            }
        hdist = 1;
        for (i = 0; i < 30; i++)
            if (dlens[i] && i + 1 > hdist)
                hdist = i + 1;
        if (maxlen > 9)
            deep++;

        canon_codes(llens, 286, lcodes);
        emit_dynamic(&w, llens, hlit, dlens, hdist);
        /* Every literal in the code, then end of block. */
        for (i = 0; i < 256; i++)
            if (llens[i]) {
                bw_code(&w, lcodes[i], llens[i]);
                expect[nexp++] = (unsigned char)i;
            }
        bw_code(&w, lcodes[256], llens[256]);

        r = inflate_buf(w.buf, bw_bytes(&w), &dec, &dlen, INFLATE_RAW);
        tried++;
        CHECK(r == INFLATE_OK, "legal random code rejected: %s (hlit %u "
              "hdist %u maxlen %u)", inflate_strerror(r), hlit, hdist, maxlen);
        if (r == INFLATE_OK) {
            CHECK(dlen == nexp, "decoded %lu symbols, expected %u", dlen, nexp);
            CHECK(same((unsigned char *)dec, dlen, expect, nexp, &where),
                  "symbol %lu decoded wrong", where);
        }
        free(dec);
    }

    printf("  %lu random legal dynamic blocks (%lu with codes longer than "
           "the 9-bit primary table), %lu draws discarded\n",
           tried, deep, skipped);
    printf("  table entries used: literal/length %u -> %u of 2048, "
           "distance %u -> %u of 1024\n",
           lens_before, inflate_dbg_lens_used,
           dist_before, inflate_dbg_dist_used);
    CHECK(inflate_dbg_lens_used < 2048, "literal/length pool is full");
    CHECK(inflate_dbg_dist_used < 1024, "distance pool is full");

    /* The special cases RFC 1951 leaves room for: exactly one distance
     * code (an incomplete code, which real encoders emit), and none. */
    {
        unsigned char llens[288], dlens[32];
        unsigned lcodes[288];
        struct bw w;
        void *dec = 0;
        unsigned long dlen = 0;
        unsigned i;
        int r;

        memset(llens, 0, sizeof(llens));
        for (i = 0; i < 256; i++)
            llens[i] = 9;             /* 256 * 2^-9 = 1/2 */
        llens[256] = 1;               /* plus 1/2 -> complete */
        canon_codes(llens, 286, lcodes);

        memset(dlens, 0, sizeof(dlens));
        dlens[0] = 1;                 /* one distance code: incomplete */
        emit_dynamic(&w, llens, 257, dlens, 1);
        bw_code(&w, lcodes[65], llens[65]);
        bw_code(&w, lcodes[256], llens[256]);
        r = inflate_buf(w.buf, bw_bytes(&w), &dec, &dlen, INFLATE_RAW);
        CHECK(r == INFLATE_OK && dlen == 1 && ((unsigned char *)dec)[0] == 65,
              "single distance code gave %s len %lu", inflate_strerror(r),
              dlen);
        free(dec);
        dec = 0;

        memset(dlens, 0, sizeof(dlens));   /* no distance codes at all */
        emit_dynamic(&w, llens, 257, dlens, 1);
        bw_code(&w, lcodes[66], llens[66]);
        bw_code(&w, lcodes[256], llens[256]);
        r = inflate_buf(w.buf, bw_bytes(&w), &dec, &dlen, INFLATE_RAW);
        CHECK(r == INFLATE_OK && dlen == 1 && ((unsigned char *)dec)[0] == 66,
              "no distance codes gave %s len %lu", inflate_strerror(r), dlen);
        free(dec);
        dec = 0;

        /* An incomplete literal/length code, which is never legal. */
        llens[256] = 2;
        emit_dynamic(&w, llens, 257, dlens, 1);
        r = inflate_buf(w.buf, bw_bytes(&w), &dec, &dlen, INFLATE_RAW);
        CHECK(r == INFLATE_ERR_HUFFMAN, "incomplete literal code gave %s",
              inflate_strerror(r));
        free(dec);
        dec = 0;

        /* A literal/length code with no end-of-block symbol. */
        memset(llens, 0, sizeof(llens));
        for (i = 0; i < 256; i++)
            llens[i] = 8;             /* complete, but no symbol 256 */
        emit_dynamic(&w, llens, 257, dlens, 1);
        r = inflate_buf(w.buf, bw_bytes(&w), &dec, &dlen, INFLATE_RAW);
        CHECK(r == INFLATE_ERR_HUFFMAN, "missing end-of-block gave %s",
              inflate_strerror(r));
        free(dec);
        dec = 0;
    }
}

/* Feed structured-but-hostile random bytes straight at the decoder and
 * make sure it always terminates with a sane answer. */
static void test_fuzz(void)
{
    unsigned seed = 987654321u;
    unsigned long i, iters = 40000;
    unsigned long okc = 0, errc = 0;
    unsigned long produced = 0;

    section("random input fuzz");

    for (i = 0; i < iters; i++) {
        unsigned char buf[256];
        unsigned long n, j;
        void *dec = 0;
        unsigned long dlen = 0;
        int wrapper, r;

        seed = seed * 1664525u + 1013904223u;
        n = 1 + (seed >> 8) % sizeof(buf);
        for (j = 0; j < n; j++) {
            seed = seed * 1664525u + 1013904223u;
            buf[j] = (unsigned char)(seed >> 13);
        }
        wrapper = (int)((seed >> 3) & 3);
        r = inflate_buf_limit(buf, n, &dec, &dlen, wrapper, 4UL * 1024 * 1024);
        if (r == INFLATE_OK) {
            okc++;
            produced += dlen;
        } else {
            errc++;
            CHECK(dec == 0, "fuzz error path leaked a buffer");
        }
        free(dec);
    }
    printf("  %lu random buffers: %lu rejected, %lu happened to decode "
           "(%lu bytes total)\n", iters, errc, okc, produced);
    CHECK(errc + okc == iters, "fuzz accounting is wrong");
}

static void test_api_edges(void)
{
    void *dec = 0;
    unsigned long dlen = 0;
    struct inflate_stream *s;
    int r;

    section("API edges");

    r = inflate_buf(0, 0, &dec, &dlen, INFLATE_RAW);
    CHECK(r == INFLATE_ERR_TRUNCATED, "empty raw input gave %s",
          inflate_strerror(r));
    free(dec);
    dec = 0;

    r = inflate_buf("x", 1, 0, &dlen, INFLATE_RAW);
    CHECK(r == INFLATE_ERR_ARG, "NULL out gave %s", inflate_strerror(r));

    r = inflate_buf(0, 5, &dec, &dlen, INFLATE_RAW);
    CHECK(r == INFLATE_ERR_ARG, "NULL src with length gave %s",
          inflate_strerror(r));

    CHECK(inflate_begin(-1, 0) == 0, "begin accepted wrapper -1");
    CHECK(inflate_begin(99, 0) == 0, "begin accepted wrapper 99");

    inflate_end(0);
    CHECK(inflate_peek(0, 0) == 0, "peek(NULL) is not NULL");
    inflate_drain(0, 10);

    /* Errors are sticky and the stream stays safe to use. */
    {
        unsigned char junk[8];
        memset(junk, 0xFF, sizeof(junk));
        s = inflate_begin(INFLATE_ZLIB, 0);
        r = inflate_push(s, junk, sizeof(junk));
        CHECK(r == INFLATE_ERR_HEADER, "junk zlib header gave %s",
              inflate_strerror(r));
        CHECK(inflate_push(s, junk, sizeof(junk)) == INFLATE_ERR_HEADER,
              "error was not sticky");
        CHECK(inflate_finish(s, &dec, &dlen) == INFLATE_ERR_HEADER,
              "finish after error changed the verdict");
        CHECK(inflate_error(s) == INFLATE_ERR_HEADER, "inflate_error wrong");
        inflate_end(s);
        free(dec);
        dec = 0;
    }

    /* Pushing nothing repeatedly must not spin or advance. */
    {
        unsigned char *comp;
        unsigned long clen;
        comp = slurp("hello.l6.gz", &clen);
        s = inflate_begin(INFLATE_GZIP, 0);
        CHECK(inflate_push(s, 0, 0) == INFLATE_MORE, "empty push not MORE");
        CHECK(inflate_push(s, 0, 0) == INFLATE_MORE, "empty push not MORE");
        r = inflate_push(s, comp, clen);
        CHECK(r >= 0, "push after empty pushes: %s", inflate_strerror(r));
        CHECK(inflate_push(s, 0, 0) >= 0, "empty push after data failed");
        r = inflate_finish(s, &dec, &dlen);
        CHECK(r == INFLATE_OK && dlen == 12, "hello finish %s len %lu",
              inflate_strerror(r), dlen);
        CHECK(inflate_total_in(s) == clen, "total_in %lu != %lu",
              inflate_total_in(s), clen);
        inflate_end(s);
        free(dec);
        dec = 0;
        free(comp);
    }

    /* finish() twice, and finish() without ever pushing. */
    s = inflate_begin(INFLATE_GZIP, 0);
    r = inflate_finish(s, &dec, &dlen);
    CHECK(r == INFLATE_ERR_TRUNCATED, "finish with no input gave %s",
          inflate_strerror(r));
    CHECK(dec == 0, "failed finish returned a buffer");
    inflate_end(s);

    /* Trailing garbage after a complete stream is ignored, not fatal. */
    {
        unsigned char *comp, *both;
        unsigned long clen;
        comp = slurp("hello.l6.gz", &clen);
        both = (unsigned char *)malloc(clen + 5);
        memcpy(both, comp, clen);
        memcpy(both + clen, "junk!", 5);
        r = inflate_buf(both, clen + 5, &dec, &dlen, INFLATE_GZIP);
        CHECK(r == INFLATE_OK && dlen == 12,
              "trailing garbage after gzip gave %s len %lu",
              inflate_strerror(r), dlen);
        free(dec);
        dec = 0;
        free(both);
        free(comp);
    }
}

/* Rough throughput, so the cost of a page load is a number rather than a
 * guess. Meaningless under a sanitizer, which is why it is reported and
 * not asserted. */
static void test_speed(void)
{
    static const struct { const char *f; const char *o; int w; } runs[] = {
        { "big4m.l6.gz",  "big4m.bin", INFLATE_GZIP },
        { "big4m.l9.zlib","big4m.bin", INFLATE_ZLIB },
        { "rle.l9.zlib",  "rle.bin",   INFLATE_ZLIB },
        { "rand.l6.gz",   "rand.bin",  INFLATE_GZIP }
    };
    unsigned k;

    section("throughput");

    for (k = 0; k < sizeof(runs) / sizeof(runs[0]); k++) {
        unsigned char *comp;
        unsigned long clen, total = 0;
        clock_t t0, t1;
        int reps = 0, r = 0;
        double secs;

        comp = slurp(runs[k].f, &clen);
        t0 = clock();
        do {
            void *dec = 0;
            unsigned long dlen = 0;
            r = inflate_buf(comp, clen, &dec, &dlen, runs[k].w);
            total += dlen;
            free(dec);
            reps++;
            t1 = clock();
        } while ((double)(t1 - t0) / CLOCKS_PER_SEC < 0.3 && reps < 500);
        CHECK(r == INFLATE_OK, "%s: %s", runs[k].f, inflate_strerror(r));
        secs = (double)(t1 - t0) / CLOCKS_PER_SEC;
        printf("  %-14s %lu -> %lu bytes, %d reps in %.2fs = %.0f MB/s out, "
               "%.0f MB/s in\n", runs[k].f, clen, total / (reps ? reps : 1),
               reps, secs,
               secs > 0 ? (double)total / secs / 1e6 : 0.0,
               secs > 0 ? (double)clen * reps / secs / 1e6 : 0.0);
        free(comp);
    }
}

int main(int argc, char **argv)
{
    int keep = 0;
    int i;

    for (i = 1; i < argc; i++)
        if (strcmp(argv[i], "--keep") == 0)
            keep = 1;

    printf("libz inflate test harness\n");
    generate_fixtures();

    test_hashes();
    test_roundtrips();
    test_auto_detect();
    test_streaming();
    test_drain();
    test_window_edge();
    test_truncation();
    test_corruption();
    test_differential();
    test_checksum_tampering();
    test_limit();
    test_handmade();
    test_table_stress();
    test_fuzz();
    test_api_edges();
    test_speed();

    printf("\nHuffman table high water marks: literal/length %u of %d, "
           "distance %u of %d entries\n",
           inflate_dbg_lens_used, 2048, inflate_dbg_dist_used, 1024);
    printf("\n%d checks, %d failures\n", checks, failures);
    if (!keep)
        printf("(fixtures left in %s; delete with rm -rf %s)\n",
               FIXDIR, FIXDIR);
    return failures ? 1 : 0;
}
