/* libz: DEFLATE decompression, written from RFC 1951, 1950 and 1952.
 *
 * ---------------------------------------------------------------- shape
 *
 * The decoder is a resumable state machine driven by inf_run(). Input is
 * accumulated in a small pending buffer; on every call the machine
 * consumes as much of it as it can and then stops in one of two ways:
 * either the stream ended, or it needs more input.
 *
 * Rather than a state per bit field, which is where streaming inflate
 * usually turns into an unreadable mess, resumption uses rollback: a unit
 * of work that cannot be completed with the bits on hand restores the bit
 * position to where the unit started and asks for more input. The units
 * are small and bounded - one literal, one length/distance pair (48 bits
 * at worst), one whole dynamic block header (about 560 bytes at worst) -
 * so the pending buffer never has to hold more than roughly 600 bytes of
 * history beyond what the caller just pushed, and re-running a unit is
 * cheap.
 *
 * -------------------------------------------------------- Huffman codes
 *
 * Codes are canonical and at most 15 bits, and are read least
 * significant bit first, so a code's table index is its bit-reversal.
 * Decoding walks a table, not a tree: a primary table of 2^9 entries is
 * indexed by the next 9 bits, which resolves every code of 9 bits or
 * fewer in one lookup. Longer codes land on an entry that points at a
 * sub-table indexed by the remaining bits. Sub-tables are sized to the
 * longest code that actually shares their 9-bit prefix, which is the
 * smallest they can be.
 *
 * Both tables live in fixed pools inside the stream, so building a table
 * never allocates. The pools are far larger than any legal code needs
 * (see the numbers the test harness reports) and an overflow is caught
 * and reported rather than trusted.
 *
 * ------------------------------------------------------- output history
 *
 * There is no separate 32 KiB sliding window. History is the output
 * buffer itself, so a back reference is a plain backwards copy inside one
 * array and there is only one copy path to get wrong. inflate_drain()
 * keeps that true by never releasing the last 32 KiB, which bounds a
 * draining caller's memory without a second buffer or a ring wrap.
 *
 * Every allocation is bounded: output by the caller's cap, the pending
 * input by what the caller pushes minus what is consumed, the gzip
 * header by INF_GZ_HDR_MAX, and the Huffman tables not at all.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "inflate.h"

/* ---------------------------------------------------------- parameters */

#define INF_ROOT_BITS   9    /* primary table index width, len/dist codes */
#define INF_CROOT_BITS  7    /* ditto, code length code (max length is 7) */

/* Entry pools. A complete 288-symbol code with 15-bit codes needs well
 * under a thousand entries with minimal sub-tables; these leave room to
 * spare and inf_build() refuses to overrun them. */
#define INF_POOL_LENS   2048
#define INF_POOL_DISTS  1024
#define INF_POOL_CLENS   256

#define INF_MAXBITS     15   /* longest DEFLATE code                     */
#define INF_GZ_HDR_MAX  65536UL  /* sanity cap on a gzip header          */
#define INF_OUT_START   1024UL   /* initial output capacity              */

/* ------------------------------------------------------ decode entries */

#define HUFT_BAD 0   /* no code here (only possible in incomplete codes) */
#define HUFT_SYM 1   /* sym is a symbol, bits is what to consume         */
#define HUFT_SUB 2   /* sym is a sub-table base, bits its index width    */

struct inf_huft {
    uint16_t sym;
    uint8_t  bits;
    uint8_t  kind;
};

/* ------------------------------------------------------------- states */

enum {
    ST_AUTO,       /* sniff the wrapper                                 */
    ST_ZHEAD,      /* RFC 1950 CMF/FLG                                  */
    ST_GZMAGIC,    /* RFC 1952 fixed 10-byte header                     */
    ST_GZXLEN,
    ST_GZEXTRA,
    ST_GZNAME,
    ST_GZCOMMENT,
    ST_GZHCRC,
    ST_BLOCK,      /* three-bit block header                            */
    ST_STOREDLEN,  /* LEN/NLEN of a stored block                        */
    ST_STORED,     /* body of a stored block                            */
    ST_TABLE,      /* dynamic block's code lengths                      */
    ST_CODES,      /* compressed data                                   */
    ST_TRAILER,    /* Adler-32, or CRC-32 and ISIZE                     */
    ST_AFTER,      /* decide whether another gzip member follows        */
    ST_DONE
};

struct inflate_stream {
    int state;
    int err;              /* sticky */
    int wrapper;          /* resolved */
    int req_wrapper;      /* as asked for, possibly INFLATE_AUTO */
    int last_block;
    int fixed_tables;     /* the built tables are the fixed ones */

    /* pending input */
    unsigned char *in;
    unsigned long in_cap;
    unsigned long in_len;
    unsigned long bitpos;      /* bits consumed from in[0] */
    unsigned long in_base;     /* bytes dropped off the front of in */
    int eof;                   /* inflate_finish() was called */

    /* output; out[0 .. out_len) is live, out[0 .. out_read) already
     * handed to the caller but kept as history */
    unsigned char *out;
    unsigned long out_cap;
    unsigned long out_len;
    unsigned long out_read;
    unsigned long out_dropped;
    unsigned long max_out;

    /* per member */
    uint32_t adler;
    uint32_t crc;
    unsigned long member_out;

    /* gzip header */
    unsigned gz_flg;
    unsigned long gz_left;
    unsigned long gz_hdr_bytes;
    uint32_t gz_hcrc;

    unsigned long stored_left;

    unsigned char lens[320];   /* code lengths of a dynamic block */

    struct inf_huft lt[INF_POOL_LENS];
    struct inf_huft dt[INF_POOL_DISTS];
    struct inf_huft ct[INF_POOL_CLENS];
};

unsigned inflate_dbg_lens_used;
unsigned inflate_dbg_dist_used;

/* ------------------------------------------------- specification tables */

/* RFC 1951 section 3.2.5: length codes 257..285 and distance codes 0..29. */
static const unsigned short len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51,
    59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const unsigned char len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4,
    4, 5, 5, 5, 5, 0
};
static const unsigned short dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
    513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385,
    24577
};
static const unsigned char dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10,
    10, 11, 11, 12, 12, 13, 13
};
/* RFC 1951 section 3.2.7: the order the code length code's own lengths
 * are transmitted in. */
static const unsigned char clen_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/* ------------------------------------------------------------ bit input */

#define INF_MASK(n) (((uint32_t)1 << (n)) - 1)   /* n <= 31 */

static unsigned long inf_avail(const struct inflate_stream *s)
{
    return s->in_len * 8 - s->bitpos;
}

/* The next up to 57 bits, zero padded past the end of the input. Callers
 * must check inf_avail() before acting on bits they take from this. */
static uint64_t inf_peek64(const struct inflate_stream *s)
{
    unsigned long byte = s->bitpos >> 3;
    unsigned shift = (unsigned)(s->bitpos & 7);
    uint64_t v = 0;

    if (byte + 8 <= s->in_len) {
        const unsigned char *p = s->in + byte;
        /* Written out so a little endian compiler folds it to one load. */
        v = (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
            ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) |
            ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) |
            ((uint64_t)p[7] << 56);
    } else {
        unsigned i;
        for (i = 0; i < 8 && byte + i < s->in_len; i++)
            v |= (uint64_t)s->in[byte + i] << (i * 8);
    }
    return v >> shift;
}

static unsigned inf_bits(const struct inflate_stream *s, unsigned n)
{
    return (unsigned)(inf_peek64(s) & INF_MASK(n));
}

static void inf_drop(struct inflate_stream *s, unsigned n)
{
    s->bitpos += n;
}

static void inf_align(struct inflate_stream *s)
{
    s->bitpos = (s->bitpos + 7) & ~(unsigned long)7;
}

/* Byte oriented view, valid only where the stream is byte aligned. */
static unsigned long inf_bytes_avail(const struct inflate_stream *s)
{
    return s->in_len - (s->bitpos >> 3);
}

static const unsigned char *inf_bytep(const struct inflate_stream *s)
{
    return s->in + (s->bitpos >> 3);
}

static void inf_skip_bytes(struct inflate_stream *s, unsigned long n)
{
    s->bitpos += n * 8;
}

/* ------------------------------------------------------- table building */

static unsigned inf_bitrev(unsigned code, unsigned len)
{
    unsigned r = 0, i;

    for (i = 0; i < len; i++) {
        r = (r << 1) | (code & 1);
        code >>= 1;
    }
    return r;
}

/* Build a canonical decode table from code lengths.
 *
 * lens[0..nsym) gives each symbol's code length, 0 meaning absent. The
 * primary table occupies pool[0 .. 1<<root) and sub-tables follow it.
 * allow_incomplete permits a code that does not fill the space, which
 * RFC 1951 effectively requires for the "one distance code" case and
 * which real encoders also produce with no distance codes at all; the
 * unreachable entries decode as HUFT_BAD. Returns 0 or an error, and
 * stores the number of entries used. */
static int inf_build(const unsigned char *lens, unsigned nsym, unsigned root,
                     struct inf_huft *pool, unsigned pool_size,
                     int allow_incomplete, unsigned *used_out)
{
    unsigned count[INF_MAXBITS + 1];
    unsigned offs[INF_MAXBITS + 2];
    unsigned next_code[INF_MAXBITS + 1];
    unsigned walk[INF_MAXBITS + 1];
    unsigned short work[288];
    unsigned char pmax[1u << INF_ROOT_BITS];
    unsigned root_size, next, len, sym, n, i, idx, code, pfx;
    long left;

    if (nsym > 288 || root > INF_ROOT_BITS)
        return INFLATE_ERR_ARG;
    root_size = 1u << root;
    if (root_size > pool_size)
        return INFLATE_ERR_ARG;

    for (i = 0; i <= INF_MAXBITS; i++)
        count[i] = 0;
    for (sym = 0; sym < nsym; sym++) {
        if (lens[sym] > INF_MAXBITS)
            return INFLATE_ERR_HUFFMAN;
        count[lens[sym]]++;
    }

    /* Kraft inequality: a code is over-subscribed if the space goes
     * negative and incomplete if any is left over. */
    left = 1;
    for (len = 1; len <= INF_MAXBITS; len++) {
        left <<= 1;
        left -= (long)count[len];
        if (left < 0)
            return INFLATE_ERR_HUFFMAN;
    }
    if (left > 0) {
        unsigned ncodes = nsym - count[0];
        if (!allow_incomplete || ncodes > 1)
            return INFLATE_ERR_HUFFMAN;
    }

    /* HUFT_BAD is zero, so clearing marks everything unreachable. */
    memset(pool, 0, (unsigned long)root_size * sizeof(pool[0]));
    next = root_size;
    if (nsym == count[0]) {           /* no codes at all */
        *used_out = next;
        return 0;
    }

    /* Symbols in canonical order: by length, then by symbol. */
    offs[1] = 0;
    for (len = 1; len <= INF_MAXBITS; len++)
        offs[len + 1] = offs[len] + count[len];
    for (sym = 0; sym < nsym; sym++)
        if (lens[sym])
            work[offs[lens[sym]]++] = (unsigned short)sym;

    /* First code value of each length. count[0] plays no part. */
    code = 0;
    for (len = 1; len <= INF_MAXBITS; len++) {
        code = (code + (len == 1 ? 0 : count[len - 1])) << 1;
        next_code[len] = code;
        walk[len] = code;
    }

    /* Pass one: the longest code sharing each primary-table index. */
    memset(pmax, 0, root_size);
    for (len = root + 1; len <= INF_MAXBITS; len++) {
        for (n = 0; n < count[len]; n++) {
            pfx = inf_bitrev(walk[len]++, len) & (root_size - 1);
            if (len > pmax[pfx])
                pmax[pfx] = (unsigned char)len;
        }
    }

    /* Pass two: carve out a sub-table for each such index, exactly wide
     * enough for the longest code under it. */
    for (pfx = 0; pfx < root_size; pfx++) {
        unsigned sb, size;

        if (!pmax[pfx])
            continue;
        sb = (unsigned)pmax[pfx] - root;
        size = 1u << sb;
        if (next + size > pool_size)
            return INFLATE_ERR_HUFFMAN;
        memset(pool + next, 0, (unsigned long)size * sizeof(pool[0]));
        pool[pfx].kind = HUFT_SUB;
        pool[pfx].sym = (uint16_t)next;
        pool[pfx].bits = (uint8_t)sb;
        next += size;
    }

    /* Pass three: place every code. A code of len bits repeats through
     * its table with a stride of 2^len because the bits above it are
     * don't-cares. */
    i = 0;
    for (len = 1; len <= INF_MAXBITS; len++) {
        for (n = 0; n < count[len]; n++, i++) {
            unsigned rev = inf_bitrev(next_code[len]++, len);
            unsigned s2 = work[i];

            if (len <= root) {
                unsigned step = 1u << len;
                for (idx = rev; idx < root_size; idx += step) {
                    pool[idx].kind = HUFT_SYM;
                    pool[idx].sym = (uint16_t)s2;
                    pool[idx].bits = (uint8_t)len;
                }
            } else {
                unsigned base, ssize, step;

                pfx = rev & (root_size - 1);
                base = pool[pfx].sym;
                ssize = 1u << pool[pfx].bits;
                step = 1u << (len - root);
                for (idx = rev >> root; idx < ssize; idx += step) {
                    pool[base + idx].kind = HUFT_SYM;
                    pool[base + idx].sym = (uint16_t)s2;
                    pool[base + idx].bits = (uint8_t)(len - root);
                }
            }
        }
    }

    *used_out = next;
    return 0;
}

/* RFC 1951 section 3.2.6: the fixed codes. */
static int inf_build_fixed(struct inflate_stream *s)
{
    unsigned char l[288];
    unsigned i, used;
    int r;

    if (s->fixed_tables)
        return 0;
    for (i = 0; i < 144; i++) l[i] = 8;
    for (; i < 256; i++)      l[i] = 9;
    for (; i < 280; i++)      l[i] = 7;
    for (; i < 288; i++)      l[i] = 8;
    r = inf_build(l, 288, INF_ROOT_BITS, s->lt, INF_POOL_LENS, 0, &used);
    if (r)
        return r;
    if (used > inflate_dbg_lens_used)
        inflate_dbg_lens_used = used;

    for (i = 0; i < 32; i++) l[i] = 5;
    r = inf_build(l, 32, INF_ROOT_BITS, s->dt, INF_POOL_DISTS, 0, &used);
    if (r)
        return r;
    if (used > inflate_dbg_dist_used)
        inflate_dbg_dist_used = used;

    s->fixed_tables = 1;
    return 0;
}

/* --------------------------------------------------------- symbol decode */

/* 0 on success, 1 if more input is needed, negative on error. */
static int inf_decode(struct inflate_stream *s, const struct inf_huft *t,
                      unsigned root, unsigned *sym)
{
    unsigned long avail = inf_avail(s);
    uint64_t hold = inf_peek64(s);
    struct inf_huft e = t[hold & INF_MASK(root)];

    if (e.kind == HUFT_SUB) {
        struct inf_huft e2 =
            t[e.sym + (unsigned)((hold >> root) & INF_MASK(e.bits))];

        /* A sub-table is as wide as its longest code, but the code
         * actually sitting here may be shorter, so only the bits that
         * entry claims have to have arrived. An entry whose width is
         * covered by real bits is the right entry however the peek was
         * padded, because a code of width b is replicated through the
         * table with stride 2^b. */
        if (e2.kind != HUFT_SYM) {
            if (avail < (unsigned long)root + e.bits)
                return 1;
            return INFLATE_ERR_CODE;
        }
        if (avail < (unsigned long)root + e2.bits)
            return 1;
        s->bitpos += (unsigned long)root + e2.bits;
        *sym = e2.sym;
        return 0;
    }
    if (e.kind != HUFT_SYM) {
        /* Zero padding could be masking the real bits; only call it a
         * bad code once a full primary index has genuinely arrived. */
        if (avail < (unsigned long)root)
            return 1;
        return INFLATE_ERR_CODE;
    }
    if (avail < (unsigned long)e.bits)
        return 1;
    s->bitpos += e.bits;
    *sym = e.sym;
    return 0;
}

/* --------------------------------------------------------------- output */

static void inf_checksum(struct inflate_stream *s, const unsigned char *p,
                         unsigned long n)
{
    if (s->wrapper == INFLATE_ZLIB)
        s->adler = adler32_update(s->adler, p, n);
    else if (s->wrapper == INFLATE_GZIP)
        s->crc = crc32_update(s->crc, p, n);
}

static int inf_reserve(struct inflate_stream *s, unsigned long n)
{
    unsigned long need, cap;
    unsigned char *p;

    if (s->out_dropped + s->out_len + n > s->max_out)
        return INFLATE_ERR_LIMIT;
    need = s->out_len + n + 1;          /* +1 for the trailing NUL */
    if (need <= s->out_cap)
        return 0;

    cap = s->out_cap ? s->out_cap : INF_OUT_START;
    while (cap < need)
        cap *= 2;
    if (cap > s->max_out + 1)
        cap = s->max_out + 1;
    if (cap < need)
        cap = need;

    p = (unsigned char *)realloc(s->out, cap);
    if (!p)
        return INFLATE_ERR_MEMORY;
    s->out = p;
    s->out_cap = cap;
    return 0;
}

static int inf_emit(struct inflate_stream *s, const unsigned char *p,
                    unsigned long n)
{
    int r = inf_reserve(s, n);

    if (r)
        return r;
    memcpy(s->out + s->out_len, p, n);
    inf_checksum(s, s->out + s->out_len, n);
    s->out_len += n;
    s->member_out += n;
    return 0;
}

static int inf_emit_byte(struct inflate_stream *s, unsigned char c)
{
    int r = inf_reserve(s, 1);

    if (r)
        return r;
    s->out[s->out_len] = c;
    inf_checksum(s, s->out + s->out_len, 1);
    s->out_len++;
    s->member_out++;
    return 0;
}

/* A back reference. dist has already been checked against the total
 * output, and inflate_drain() only ever releases bytes beyond the last
 * 32 KiB, so a distance of at most 32768 is always still in the buffer.
 * That is an argument, not a guarantee, and reading before the start of
 * the heap block is the classic way an inflate implementation turns a
 * malformed stream into a security bug - so the bound is also checked
 * against the buffer itself. */
static int inf_emit_match(struct inflate_stream *s, unsigned long dist,
                          unsigned long len)
{
    unsigned char *d, *from;
    int r;

    if (dist > s->out_len)
        return INFLATE_ERR_DISTANCE;
    r = inf_reserve(s, len);
    if (r)
        return r;
    d = s->out + s->out_len;
    from = d - dist;
    if (dist == 1) {
        memset(d, from[0], len);            /* run length, very common */
    } else if (dist >= len) {
        memcpy(d, from, len);
    } else {
        unsigned long i;
        for (i = 0; i < len; i++)           /* overlapping: byte at a time */
            d[i] = from[i];
    }
    inf_checksum(s, d, len);
    s->out_len += len;
    s->member_out += len;
    return 0;
}

/* ---------------------------------------------------------- gzip header */

/* Stage: 0 after the fixed header, 1 after FEXTRA, 2 after FNAME,
 * 3 after FCOMMENT. */
static int inf_gz_after(const struct inflate_stream *s, int stage)
{
    if (stage <= 0 && (s->gz_flg & 0x04)) return ST_GZXLEN;
    if (stage <= 1 && (s->gz_flg & 0x08)) return ST_GZNAME;
    if (stage <= 2 && (s->gz_flg & 0x10)) return ST_GZCOMMENT;
    if (stage <= 3 && (s->gz_flg & 0x02)) return ST_GZHCRC;
    return ST_BLOCK;
}

/* Skip a NUL terminated header field, folding it into the header CRC. */
static int inf_gz_string(struct inflate_stream *s, int stage)
{
    unsigned long av = inf_bytes_avail(s);
    const unsigned char *p = inf_bytep(s);
    unsigned long i = 0;
    int found = 0;

    while (i < av) {
        i++;
        if (p[i - 1] == 0) {
            found = 1;
            break;
        }
    }
    s->gz_hcrc = crc32_update(s->gz_hcrc, p, i);
    s->gz_hdr_bytes += i;
    inf_skip_bytes(s, i);
    if (s->gz_hdr_bytes > INF_GZ_HDR_MAX)
        return INFLATE_ERR_HEADER;
    if (!found)
        return INFLATE_MORE;
    s->state = inf_gz_after(s, stage);
    return -1000;                 /* keep going */
}

/* ------------------------------------------------------- the state machine */

static int inf_block_end(struct inflate_stream *s)
{
    if (!s->last_block)
        return ST_BLOCK;
    if (s->wrapper == INFLATE_RAW)
        return ST_DONE;
    return ST_TRAILER;
}

static int inf_run(struct inflate_stream *s)
{
    for (;;) {
        switch (s->state) {

        case ST_AUTO: {
            unsigned long av = inf_bytes_avail(s);
            const unsigned char *p = inf_bytep(s);

            if (av < 2 && !s->eof)
                return INFLATE_MORE;
            if (av >= 2 && p[0] == 0x1F && p[1] == 0x8B) {
                s->wrapper = INFLATE_GZIP;
                s->state = ST_GZMAGIC;
            } else if (av >= 2 && (p[0] & 0x0F) == 8 && (p[0] >> 4) <= 7 &&
                       (((unsigned)p[0] * 256 + p[1]) % 31) == 0) {
                s->wrapper = INFLATE_ZLIB;
                s->state = ST_ZHEAD;
            } else {
                s->wrapper = INFLATE_RAW;
                s->state = ST_BLOCK;
            }
            break;
        }

        case ST_ZHEAD: {
            const unsigned char *p = inf_bytep(s);

            if (inf_bytes_avail(s) < 2)
                return INFLATE_MORE;
            if ((p[0] & 0x0F) != 8)                 /* CM must be deflate */
                return INFLATE_ERR_HEADER;
            if ((p[0] >> 4) > 7)                    /* window > 32 KiB    */
                return INFLATE_ERR_HEADER;
            if (((unsigned)p[0] * 256 + p[1]) % 31) /* header check       */
                return INFLATE_ERR_HEADER;
            if (p[1] & 0x20)                        /* preset dictionary  */
                return INFLATE_ERR_HEADER;
            inf_skip_bytes(s, 2);
            s->adler = 1;
            s->member_out = 0;
            s->state = ST_BLOCK;
            break;
        }

        case ST_GZMAGIC: {
            const unsigned char *p = inf_bytep(s);

            if (inf_bytes_avail(s) < 10)
                return INFLATE_MORE;
            if (p[0] != 0x1F || p[1] != 0x8B)
                return INFLATE_ERR_HEADER;
            if (p[2] != 8)                          /* CM must be deflate */
                return INFLATE_ERR_HEADER;
            if (p[3] & 0xE0)                        /* reserved flags     */
                return INFLATE_ERR_HEADER;
            s->gz_flg = p[3];
            s->gz_hcrc = crc32_update(0, p, 10);
            s->gz_hdr_bytes = 10;
            inf_skip_bytes(s, 10);
            s->crc = 0;
            s->member_out = 0;
            s->state = inf_gz_after(s, 0);
            break;
        }

        case ST_GZXLEN: {
            const unsigned char *p = inf_bytep(s);

            if (inf_bytes_avail(s) < 2)
                return INFLATE_MORE;
            s->gz_left = (unsigned long)p[0] | ((unsigned long)p[1] << 8);
            s->gz_hcrc = crc32_update(s->gz_hcrc, p, 2);
            s->gz_hdr_bytes += 2;
            inf_skip_bytes(s, 2);
            s->state = ST_GZEXTRA;
            break;
        }

        case ST_GZEXTRA: {
            unsigned long av = inf_bytes_avail(s);
            unsigned long n = (av < s->gz_left) ? av : s->gz_left;

            if (n) {
                s->gz_hcrc = crc32_update(s->gz_hcrc, inf_bytep(s), n);
                s->gz_hdr_bytes += n;
                inf_skip_bytes(s, n);
                s->gz_left -= n;
            }
            if (s->gz_left)
                return INFLATE_MORE;
            s->state = inf_gz_after(s, 1);
            break;
        }

        case ST_GZNAME: {
            int r = inf_gz_string(s, 2);
            if (r != -1000)
                return r;
            break;
        }

        case ST_GZCOMMENT: {
            int r = inf_gz_string(s, 3);
            if (r != -1000)
                return r;
            break;
        }

        case ST_GZHCRC: {
            const unsigned char *p = inf_bytep(s);
            unsigned want;

            if (inf_bytes_avail(s) < 2)
                return INFLATE_MORE;
            want = (unsigned)p[0] | ((unsigned)p[1] << 8);
            if ((s->gz_hcrc & 0xFFFFU) != want)
                return INFLATE_ERR_CHECKSUM;
            inf_skip_bytes(s, 2);
            s->state = ST_BLOCK;
            break;
        }

        case ST_BLOCK: {
            unsigned hdr;

            if (inf_avail(s) < 3)
                return INFLATE_MORE;
            hdr = inf_bits(s, 3);
            inf_drop(s, 3);
            s->last_block = (int)(hdr & 1);
            switch (hdr >> 1) {
            case 0:
                s->state = ST_STOREDLEN;
                break;
            case 1: {
                int r = inf_build_fixed(s);
                if (r)
                    return r;
                s->state = ST_CODES;
                break;
            }
            case 2:
                s->state = ST_TABLE;
                break;
            default:
                return INFLATE_ERR_BLOCK;
            }
            break;
        }

        case ST_STOREDLEN: {
            const unsigned char *p;
            unsigned len, nlen;

            inf_align(s);
            if (inf_bytes_avail(s) < 4)
                return INFLATE_MORE;
            p = inf_bytep(s);
            len = (unsigned)p[0] | ((unsigned)p[1] << 8);
            nlen = (unsigned)p[2] | ((unsigned)p[3] << 8);
            if ((len ^ 0xFFFFU) != nlen)
                return INFLATE_ERR_BLOCK;
            inf_skip_bytes(s, 4);
            s->stored_left = len;
            s->state = ST_STORED;
            break;
        }

        case ST_STORED: {
            unsigned long av, n;
            int r;

            if (s->stored_left == 0) {
                s->state = inf_block_end(s);
                break;
            }
            av = inf_bytes_avail(s);
            n = (av < s->stored_left) ? av : s->stored_left;
            if (n == 0)
                return INFLATE_MORE;
            r = inf_emit(s, inf_bytep(s), n);
            if (r)
                return r;
            inf_skip_bytes(s, n);
            s->stored_left -= n;
            break;
        }

        case ST_TABLE: {
            /* The whole dynamic header is one rollback unit: at worst
             * about 560 bytes, so retrying it costs nothing measurable
             * and there is no half-parsed state to keep. */
            unsigned long save = s->bitpos;
            unsigned char clens[19];
            unsigned hlit, hdist, hclen, i, n, used;
            int r;

            if (inf_avail(s) < 14)
                return INFLATE_MORE;
            hlit = inf_bits(s, 5) + 257;  inf_drop(s, 5);
            hdist = inf_bits(s, 5) + 1;   inf_drop(s, 5);
            hclen = inf_bits(s, 4) + 4;   inf_drop(s, 4);
            /* Symbols 286/287 and distance codes 30/31 do not exist. */
            if (hlit > 286 || hdist > 30)
                return INFLATE_ERR_HUFFMAN;

            for (i = 0; i < 19; i++)
                clens[i] = 0;
            for (i = 0; i < hclen; i++) {
                if (inf_avail(s) < 3) {
                    s->bitpos = save;
                    return INFLATE_MORE;
                }
                clens[clen_order[i]] = (unsigned char)inf_bits(s, 3);
                inf_drop(s, 3);
            }
            r = inf_build(clens, 19, INF_CROOT_BITS, s->ct, INF_POOL_CLENS,
                          0, &used);
            if (r)
                return r;

            n = 0;
            while (n < hlit + hdist) {
                unsigned sym, rep, extra;
                unsigned char val;

                r = inf_decode(s, s->ct, INF_CROOT_BITS, &sym);
                if (r > 0) {
                    s->bitpos = save;
                    return INFLATE_MORE;
                }
                if (r < 0)
                    return r;
                if (sym < 16) {
                    s->lens[n++] = (unsigned char)sym;
                    continue;
                }
                if (sym == 16) {
                    if (n == 0)
                        return INFLATE_ERR_HUFFMAN;
                    extra = 2;
                    rep = 3;
                    val = s->lens[n - 1];
                } else if (sym == 17) {
                    extra = 3;
                    rep = 3;
                    val = 0;
                } else {
                    extra = 7;
                    rep = 11;
                    val = 0;
                }
                if (inf_avail(s) < extra) {
                    s->bitpos = save;
                    return INFLATE_MORE;
                }
                rep += inf_bits(s, extra);
                inf_drop(s, extra);
                if (n + rep > hlit + hdist)
                    return INFLATE_ERR_HUFFMAN;
                while (rep--)
                    s->lens[n++] = val;
            }

            if (s->lens[256] == 0)     /* no end-of-block code */
                return INFLATE_ERR_HUFFMAN;

            r = inf_build(s->lens, hlit, INF_ROOT_BITS, s->lt,
                          INF_POOL_LENS, 0, &used);
            if (r)
                return r;
            if (used > inflate_dbg_lens_used)
                inflate_dbg_lens_used = used;
            r = inf_build(s->lens + hlit, hdist, INF_ROOT_BITS, s->dt,
                          INF_POOL_DISTS, 1, &used);
            if (r)
                return r;
            if (used > inflate_dbg_dist_used)
                inflate_dbg_dist_used = used;

            s->fixed_tables = 0;
            s->state = ST_CODES;
            break;
        }

        case ST_CODES: {
            for (;;) {
                unsigned long save = s->bitpos;
                unsigned sym, dsym, eb;
                unsigned long len, dist;
                int r;

                r = inf_decode(s, s->lt, INF_ROOT_BITS, &sym);
                if (r > 0)
                    return INFLATE_MORE;
                if (r < 0)
                    return r;

                if (sym < 256) {
                    r = inf_emit_byte(s, (unsigned char)sym);
                    if (r)
                        return r;
                    continue;
                }
                if (sym == 256) {
                    s->state = inf_block_end(s);
                    break;
                }
                sym -= 257;
                if (sym >= 29)               /* codes 286 and 287 */
                    return INFLATE_ERR_CODE;
                eb = len_extra[sym];
                if (inf_avail(s) < eb) {
                    s->bitpos = save;
                    return INFLATE_MORE;
                }
                len = (unsigned long)len_base[sym] + inf_bits(s, eb);
                inf_drop(s, eb);

                r = inf_decode(s, s->dt, INF_ROOT_BITS, &dsym);
                if (r > 0) {
                    s->bitpos = save;
                    return INFLATE_MORE;
                }
                if (r < 0)
                    return r;
                if (dsym >= 30)              /* codes 30 and 31 */
                    return INFLATE_ERR_CODE;
                eb = dist_extra[dsym];
                if (inf_avail(s) < eb) {
                    s->bitpos = save;
                    return INFLATE_MORE;
                }
                dist = (unsigned long)dist_base[dsym] + inf_bits(s, eb);
                inf_drop(s, eb);

                if (dist > s->out_dropped + s->out_len)
                    return INFLATE_ERR_DISTANCE;
                r = inf_emit_match(s, dist, len);
                if (r)
                    return r;
            }
            break;
        }

        case ST_TRAILER: {
            const unsigned char *p;

            inf_align(s);
            if (s->wrapper == INFLATE_ZLIB) {
                uint32_t want;

                if (inf_bytes_avail(s) < 4)
                    return INFLATE_MORE;
                p = inf_bytep(s);
                want = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
                if (want != s->adler)
                    return INFLATE_ERR_CHECKSUM;
                inf_skip_bytes(s, 4);
                s->state = ST_DONE;
            } else {
                uint32_t want, isize;

                if (inf_bytes_avail(s) < 8)
                    return INFLATE_MORE;
                p = inf_bytep(s);
                want = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
                isize = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                        ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
                if (want != s->crc)
                    return INFLATE_ERR_CHECKSUM;
                if (isize != (uint32_t)(s->member_out & 0xFFFFFFFFUL))
                    return INFLATE_ERR_CHECKSUM;
                inf_skip_bytes(s, 8);
                s->state = ST_AFTER;
            }
            break;
        }

        case ST_AFTER: {
            /* gzip members may be concatenated; anything else that
             * follows is not ours and is left alone. */
            const unsigned char *p = inf_bytep(s);

            if (inf_bytes_avail(s) >= 2) {
                if (p[0] == 0x1F && p[1] == 0x8B) {
                    s->state = ST_GZMAGIC;
                    break;
                }
                s->state = ST_DONE;
                break;
            }
            if (!s->eof)
                return INFLATE_MORE;
            s->state = ST_DONE;
            break;
        }

        case ST_DONE:
            return INFLATE_DONE;

        default:
            return INFLATE_ERR_STATE;
        }
    }
}

/* ------------------------------------------------------------ public API */

const char *inflate_strerror(int err)
{
    switch (err) {
    case INFLATE_OK:            return "ok";
    case INFLATE_DONE:          return "stream complete";
    case INFLATE_ERR_ARG:       return "invalid argument";
    case INFLATE_ERR_MEMORY:    return "out of memory";
    case INFLATE_ERR_HEADER:    return "bad compression header";
    case INFLATE_ERR_BLOCK:     return "bad deflate block";
    case INFLATE_ERR_HUFFMAN:   return "bad huffman table";
    case INFLATE_ERR_CODE:      return "invalid code";
    case INFLATE_ERR_DISTANCE:  return "distance too far back";
    case INFLATE_ERR_TRUNCATED: return "truncated stream";
    case INFLATE_ERR_CHECKSUM:  return "checksum mismatch";
    case INFLATE_ERR_LIMIT:     return "output limit exceeded";
    case INFLATE_ERR_STATE:     return "stream unusable after error";
    default:                    return "unknown error";
    }
}

struct inflate_stream *inflate_begin(int wrapper, unsigned long max_out)
{
    struct inflate_stream *s;

    if (wrapper < INFLATE_RAW || wrapper > INFLATE_AUTO)
        return 0;
    s = (struct inflate_stream *)malloc(sizeof(*s));
    if (!s)
        return 0;
    memset(s, 0, sizeof(*s));

    /* Halving the ceiling keeps every size sum below overflow. */
    if (max_out == 0 || max_out > ((unsigned long)-1) / 2)
        max_out = INFLATE_DEFAULT_MAX;
    s->max_out = max_out;
    s->req_wrapper = wrapper;
    s->wrapper = (wrapper == INFLATE_AUTO) ? INFLATE_RAW : wrapper;
    s->adler = 1;

    switch (wrapper) {
    case INFLATE_AUTO: s->state = ST_AUTO;    break;
    case INFLATE_ZLIB: s->state = ST_ZHEAD;   break;
    case INFLATE_GZIP: s->state = ST_GZMAGIC; break;
    default:           s->state = ST_BLOCK;   break;
    }
    return s;
}

void inflate_end(struct inflate_stream *s)
{
    if (!s)
        return;
    free(s->in);
    free(s->out);
    free(s);
}

/* Append to the pending input, growing it if needed. */
static int inf_take(struct inflate_stream *s, const unsigned char *p,
                    unsigned long n)
{
    if (s->in_len + n > s->in_cap) {
        unsigned long cap = s->in_cap ? s->in_cap : 512;
        unsigned char *q;

        while (cap < s->in_len + n)
            cap *= 2;
        q = (unsigned char *)realloc(s->in, cap);
        if (!q)
            return INFLATE_ERR_MEMORY;
        s->in = q;
        s->in_cap = cap;
    }
    memcpy(s->in + s->in_len, p, n);
    s->in_len += n;
    return 0;
}

/* Drop whole consumed bytes off the front. What is left is at most the
 * rollback history of the unit in progress, a few hundred bytes. */
static void inf_compact(struct inflate_stream *s)
{
    unsigned long drop = s->bitpos >> 3;

    if (!drop)
        return;
    memmove(s->in, s->in + drop, s->in_len - drop);
    s->in_len -= drop;
    s->bitpos -= drop * 8;
    s->in_base += drop;
}

int inflate_push(struct inflate_stream *s, const void *src,
                 unsigned long slen)
{
    int r;

    if (!s)
        return INFLATE_ERR_ARG;
    if (s->err)
        return s->err;
    if (slen && !src)
        return (s->err = INFLATE_ERR_ARG);
    if (s->state == ST_DONE)
        return INFLATE_DONE;     /* trailing bytes are not ours */

    if (slen) {
        r = inf_take(s, (const unsigned char *)src, slen);
        if (r)
            return (s->err = r);
    }
    r = inf_run(s);
    if (r < 0) {
        s->err = r;
        return r;
    }
    inf_compact(s);
    return r;
}

int inflate_finish(struct inflate_stream *s, void **out, unsigned long *olen)
{
    int r;

    if (out)
        *out = 0;
    if (olen)
        *olen = 0;
    if (!s)
        return INFLATE_ERR_ARG;
    if (s->err)
        return s->err;

    s->eof = 1;
    r = inf_run(s);
    if (r < 0)
        return (s->err = r);
    inf_compact(s);
    if (r != INFLATE_DONE)
        return (s->err = INFLATE_ERR_TRUNCATED);

    if (out) {
        if (!s->out) {                    /* nothing was ever produced */
            s->out = (unsigned char *)malloc(1);
            if (!s->out)
                return (s->err = INFLATE_ERR_MEMORY);
            s->out_cap = 1;
        }
        if (s->out_read) {
            memmove(s->out, s->out + s->out_read, s->out_len - s->out_read);
            s->out_len -= s->out_read;
            s->out_read = 0;
        }
        s->out[s->out_len] = 0;           /* inf_reserve() kept room */
        *out = s->out;
        if (olen)
            *olen = s->out_len;
        s->out = 0;
        s->out_cap = 0;
        s->out_len = 0;
    }
    return INFLATE_OK;
}

const unsigned char *inflate_peek(const struct inflate_stream *s,
                                  unsigned long *len)
{
    if (!s) {
        if (len)
            *len = 0;
        return 0;
    }
    if (len)
        *len = s->out_len - s->out_read;
    return s->out ? s->out + s->out_read : 0;
}

void inflate_drain(struct inflate_stream *s, unsigned long n)
{
    unsigned long have, keep, drop;

    if (!s)
        return;
    have = s->out_len - s->out_read;
    if (n > have)
        n = have;
    s->out_read += n;

    /* Release what the caller has read, but never the last 32 KiB: that
     * is the history a back reference may still point into. */
    keep = (s->out_len < INFLATE_WINDOW) ? s->out_len : INFLATE_WINDOW;
    drop = s->out_len - keep;
    if (drop > s->out_read)
        drop = s->out_read;
    if (!drop)
        return;
    memmove(s->out, s->out + drop, s->out_len - drop);
    s->out_len -= drop;
    s->out_read -= drop;
    s->out_dropped += drop;
}

unsigned long inflate_total_in(const struct inflate_stream *s)
{
    return s ? s->in_base + (s->bitpos >> 3) : 0;
}

unsigned long inflate_total_out(const struct inflate_stream *s)
{
    return s ? s->out_dropped + s->out_len : 0;
}

int inflate_error(const struct inflate_stream *s)
{
    return s ? s->err : INFLATE_ERR_ARG;
}

int inflate_wrapper(const struct inflate_stream *s)
{
    return s ? s->wrapper : INFLATE_ERR_ARG;
}

int inflate_buf_limit(const void *src, unsigned long slen, void **out,
                      unsigned long *olen, int wrapper,
                      unsigned long max_out)
{
    struct inflate_stream *s;
    int r;

    if (!out || !olen)
        return INFLATE_ERR_ARG;
    *out = 0;
    *olen = 0;
    if (slen && !src)
        return INFLATE_ERR_ARG;

    s = inflate_begin(wrapper, max_out);
    if (!s)
        return INFLATE_ERR_MEMORY;
    r = inflate_push(s, src, slen);
    if (r >= 0)
        r = inflate_finish(s, out, olen);
    inflate_end(s);
    if (r < 0) {
        free(*out);
        *out = 0;
        *olen = 0;
        return r;
    }
    return INFLATE_OK;
}

int inflate_buf(const void *src, unsigned long slen, void **out,
                unsigned long *olen, int wrapper)
{
    return inflate_buf_limit(src, slen, out, olen, wrapper,
                             INFLATE_DEFAULT_MAX);
}
