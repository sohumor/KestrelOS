#pragma once

/* KestrelOS libtls: the SHA-2 family, HMAC, HKDF and the TLS 1.3 key
 * schedule helpers.
 *
 * Everything here is written from the specifications: FIPS 180-4 for the
 * hashes, RFC 2104 for HMAC, RFC 5869 for HKDF, and RFC 8446 section 7.1
 * for HKDF-Expand-Label and Derive-Secret.
 *
 * One generic context covers all three digests so the TLS handshake can
 * carry a single `struct hash_ctx` for the transcript regardless of which
 * cipher suite was negotiated. The context is a plain value type with no
 * pointers into itself, so it can be copied by assignment; hash_copy() and
 * hash_peek() exist to make that guarantee explicit, because TLS needs the
 * transcript hash at several intermediate points without ending the
 * transcript.
 *
 * Note on libc/sha256.c: that file is the package manager's SHA-256 and
 * stays where it is. This file deliberately carries its own core so that
 * libtls builds and tests standalone and so all three digests share one
 * shape; tools/test_cryptosym.c links both and asserts they agree
 * bit-for-bit, so the two cannot silently diverge.
 */

#include <stdint.h>

#define HASH_MAX_DIGEST 64            /* SHA-512 */
#define HASH_MAX_BLOCK  128           /* SHA-384/512 */

enum {
    HASH_SHA256 = 0,
    HASH_SHA384 = 1,
    HASH_SHA512 = 2,
    HASH_ALG_COUNT = 3
};

struct hash_ctx {
    int alg;
    union {
        uint32_t w[8];                /* SHA-256 state */
        uint64_t d[8];                /* SHA-384/512 state */
    } h;
    uint64_t nlo;                     /* message bytes so far, low 64 */
    uint64_t nhi;                     /* and high 64, for SHA-512's 128-bit count */
    uint8_t buf[HASH_MAX_BLOCK];      /* partial block */
    unsigned long buflen;
};

/* Algorithm properties. Return 0 for an unknown algorithm. */
unsigned long hash_digest_len(int alg);
unsigned long hash_block_len(int alg);
const char *hash_name(int alg);       /* "SHA-256" etc, "?" if unknown */

/* Streaming interface. hash_init() returns -1 on an unknown algorithm, 0
 * otherwise; the others assume a context that was initialised. `out` must
 * have room for hash_digest_len(alg) bytes. */
int  hash_init(struct hash_ctx *c, int alg);
void hash_update(struct hash_ctx *c, const void *data, unsigned long len);
void hash_final(struct hash_ctx *c, uint8_t *out);

/* Fork a running hash. `dst` becomes an independent copy of `src`. */
void hash_copy(struct hash_ctx *dst, const struct hash_ctx *src);

/* Digest of everything absorbed so far, leaving `c` usable. This is
 * Transcript-Hash() over a prefix of the handshake. */
void hash_peek(const struct hash_ctx *c, uint8_t *out);

/* One shot. Returns -1 on an unknown algorithm, 0 otherwise. */
int  hash_oneshot(int alg, const void *data, unsigned long len, uint8_t *out);

/* ---------------- HMAC (RFC 2104) ---------------- */

struct hmac_ctx {
    int alg;
    struct hash_ctx inner;
    struct hash_ctx outer;
};

int  hmac_init(struct hmac_ctx *c, int alg, const uint8_t *key,
               unsigned long keylen);
void hmac_update(struct hmac_ctx *c, const void *data, unsigned long len);
void hmac_final(struct hmac_ctx *c, uint8_t *out);

int  hmac(int alg, const uint8_t *key, unsigned long keylen,
          const void *data, unsigned long len, uint8_t *out);

/* ---------------- HKDF (RFC 5869) ---------------- */

/* prk receives hash_digest_len(alg) bytes. A NULL/empty salt is treated as
 * hash_digest_len(alg) zero bytes, as the RFC specifies. */
int hkdf_extract(int alg, const uint8_t *salt, unsigned long saltlen,
                 const uint8_t *ikm, unsigned long ikmlen, uint8_t *prk);

/* outlen is capped at 255 * hash_digest_len(alg); past that, and for a prk
 * shorter than one digest, this returns -1 without writing anything. */
int hkdf_expand(int alg, const uint8_t *prk, unsigned long prklen,
                const uint8_t *info, unsigned long infolen,
                uint8_t *out, unsigned long outlen);

/* ---------------- TLS 1.3 key schedule (RFC 8446 7.1) ---------------- */

/* The HkdfLabel structure caps both strings at 255 bytes; "tls13 " eats 6
 * of the label's budget. */
#define TLS_LABEL_MAX   249           /* label bytes, excluding "tls13 " */
#define TLS_CONTEXT_MAX 255

/* HKDF-Expand-Label(secret, label, context, outlen). `secret` is
 * hash_digest_len(alg) bytes. `label` is the bare label, NUL-terminated
 * and without the "tls13 " prefix, which this adds. Returns -1 if the
 * label or context is over budget or outlen is unreachable. */
int hkdf_expand_label(int alg, const uint8_t *secret,
                      const char *label,
                      const uint8_t *context, unsigned long contextlen,
                      uint8_t *out, unsigned long outlen);

/* Derive-Secret(secret, label, Messages): expands with the transcript hash
 * of `msgs` as context, producing one digest of output. */
int hkdf_derive_secret(int alg, const uint8_t *secret, const char *label,
                       const void *msgs, unsigned long msglen, uint8_t *out);

/* Derive-Secret where the caller already has Transcript-Hash(Messages) --
 * which is the normal case, since the handshake keeps a running transcript.
 * `thash` is hash_digest_len(alg) bytes. */
int hkdf_derive_secret_hash(int alg, const uint8_t *secret, const char *label,
                            const uint8_t *thash, uint8_t *out);
