#pragma once

/* libz - DEFLATE decompression for KestrelOS.
 *
 * Implements RFC 1951 (raw DEFLATE), RFC 1950 (the zlib wrapper) and
 * RFC 1952 (the gzip wrapper) from the specifications. This unlocks PNG
 * images and HTTP Content-Encoding: gzip/deflate, so it sits on the hot
 * path of every page load.
 *
 * Two ways in:
 *
 *   inflate_buf()      one shot: a whole compressed buffer in, a freshly
 *                      allocated output buffer out.
 *   inflate_begin()    streaming: feed network-sized chunks with
 *   inflate_push()     inflate_push() and read what has been produced so
 *   inflate_finish()   far with inflate_peek()/inflate_drain(). The
 *                      trailing checksum is only verified by
 *                      inflate_finish(), so a caller that wants integrity
 *                      must call it.
 *
 * Both forms take a maximum output size. Decompression bombs are the
 * cheapest way for a hostile page to kill the browser, so the cap is not
 * optional: exceeding it is INFLATE_ERR_LIMIT and the stream stops
 * cleanly with everything produced so far still intact.
 *
 * Memory: the decoder keeps history in the output buffer itself rather
 * than in a separate sliding window, and inflate_drain() never discards
 * the last 32 KiB, so a streaming caller that drains as it goes holds
 * about 32 KiB plus one chunk regardless of how much data flows through.
 * The stream object is ~14 KiB and is always heap allocated, because the
 * userspace stack is only 64 KiB.
 *
 * Nothing here uses floating point and every allocation is bounded.
 */

#include <stdint.h>

/* ------------------------------------------------------------- wrappers */

#define INFLATE_RAW    0   /* RFC 1951, no header or trailer            */
#define INFLATE_ZLIB   1   /* RFC 1950, CMF/FLG header + Adler-32       */
#define INFLATE_GZIP   2   /* RFC 1952, gzip header + CRC-32 and ISIZE  */
#define INFLATE_AUTO   3   /* sniff gzip, then zlib, else raw           */

/* ---------------------------------------------------- results and errors */

#define INFLATE_OK             0
#define INFLATE_MORE           0   /* push(): needs more input           */
#define INFLATE_DONE           1   /* push(): the stream ended           */

#define INFLATE_ERR_ARG       (-1)   /* nonsense arguments               */
#define INFLATE_ERR_MEMORY    (-2)   /* out of memory                    */
#define INFLATE_ERR_HEADER    (-3)   /* bad zlib/gzip header             */
#define INFLATE_ERR_BLOCK     (-4)   /* bad block type or stored length  */
#define INFLATE_ERR_HUFFMAN   (-5)   /* unbuildable code length set      */
#define INFLATE_ERR_CODE      (-6)   /* symbol that the code cannot emit */
#define INFLATE_ERR_DISTANCE  (-7)   /* back reference before the output */
#define INFLATE_ERR_TRUNCATED (-8)   /* input ended mid stream           */
#define INFLATE_ERR_CHECKSUM  (-9)   /* Adler-32, CRC-32 or ISIZE wrong  */
#define INFLATE_ERR_LIMIT    (-10)   /* output would exceed the maximum  */
#define INFLATE_ERR_STATE    (-11)   /* used after an error              */

/* Human readable form of any of the above. Never returns NULL. */
const char *inflate_strerror(int err);

/* ---------------------------------------------------------- one shot API */

#define INFLATE_WINDOW      32768UL              /* DEFLATE history window */
#define INFLATE_DEFAULT_MAX (64UL * 1024 * 1024) /* inflate_buf() cap      */

/* Decompress src entirely. On success *out is a malloc'd buffer of *olen
 * bytes that the caller frees, with a NUL byte written just past the end
 * (not counted in *olen) so text can be used as a C string directly.
 * On failure *out is NULL and the return value says why.
 *
 * inflate_buf() applies INFLATE_DEFAULT_MAX; inflate_buf_limit() takes an
 * explicit cap (0 means INFLATE_DEFAULT_MAX). */
int inflate_buf(const void *src, unsigned long slen, void **out,
                unsigned long *olen, int wrapper);
int inflate_buf_limit(const void *src, unsigned long slen, void **out,
                      unsigned long *olen, int wrapper,
                      unsigned long max_out);

/* --------------------------------------------------------- streaming API */

struct inflate_stream;   /* opaque */

/* max_out caps the total number of bytes the stream will ever produce,
 * counting bytes already drained. 0 means INFLATE_DEFAULT_MAX. Returns
 * NULL on bad arguments or out of memory. */
struct inflate_stream *inflate_begin(int wrapper, unsigned long max_out);

/* Feed slen bytes. Returns INFLATE_MORE if more input is needed,
 * INFLATE_DONE if the stream ended, or a negative error. Input past the
 * end of a finished stream is ignored. Errors are sticky. */
int inflate_push(struct inflate_stream *s, const void *src,
                 unsigned long slen);

/* Declare end of input, verify the wrapper trailer, and hand the output
 * over. If out is non-NULL, *out receives the buffer (the caller frees
 * it) and *olen its length, covering everything not already drained; a
 * NUL byte is written just past the end. If out is NULL the output stays
 * with the stream. A stream that has not reached its end is
 * INFLATE_ERR_TRUNCATED. */
int inflate_finish(struct inflate_stream *s, void **out,
                   unsigned long *olen);

/* Release the stream and anything inflate_finish() did not hand over.
 * Safe on NULL. */
void inflate_end(struct inflate_stream *s);

/* Output produced but not yet drained. The pointer is invalidated by the
 * next push/drain/finish/end. *len may be NULL. */
const unsigned char *inflate_peek(const struct inflate_stream *s,
                                  unsigned long *len);

/* Declare the first n peekable bytes consumed so their memory can be
 * reused. n is clamped to what is available. */
void inflate_drain(struct inflate_stream *s, unsigned long n);

unsigned long inflate_total_in(const struct inflate_stream *s);   /* consumed */
unsigned long inflate_total_out(const struct inflate_stream *s);  /* produced */
int inflate_error(const struct inflate_stream *s);
int inflate_wrapper(const struct inflate_stream *s);  /* resolved INFLATE_AUTO */

/* --------------------------------------------------------------- hashes */

/* Both take the running value and return the updated one; start CRC-32
 * at 0 and Adler-32 at 1, which is what crc32_buf()/adler32_buf() do.
 * The CRC table is built from the polynomial on first use. */
uint32_t crc32_update(uint32_t crc, const void *buf, unsigned long len);
uint32_t crc32_buf(const void *buf, unsigned long len);
uint32_t adler32_update(uint32_t adler, const void *buf, unsigned long len);
uint32_t adler32_buf(const void *buf, unsigned long len);

/* ---------------------------------------------------------- diagnostics */

/* High water marks of the Huffman decode tables, in entries, over every
 * table this program has built. Used by the test harness to show that the
 * fixed pools are not close to full; of no interest otherwise. */
extern unsigned inflate_dbg_lens_used;
extern unsigned inflate_dbg_dist_used;
