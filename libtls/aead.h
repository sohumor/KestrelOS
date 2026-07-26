#pragma once

/* KestrelOS libtls: ChaCha20, Poly1305, AES, GHASH, and the two AEADs
 * TLS 1.3 actually negotiates.
 *
 * Written from RFC 8439 (ChaCha20 and Poly1305), FIPS 197 (AES) and NIST
 * SP 800-38D / the McGrew-Viega GCM specification (GCM and GHASH).
 *
 * The AEAD layer is a small vtable-free dispatch on an algorithm id, so a
 * TLS record layer can hold one `struct aead_ctx` per direction and never
 * care which cipher suite was negotiated.
 *
 * Nothing in this file allocates. Every buffer is caller-provided or a
 * fixed-size automatic, and there is no recursion, so the 64 KiB user
 * stack is never at risk.
 *
 * TIMING: the tag comparison and all of Poly1305 are constant time. AES
 * uses 4 KiB of T-tables indexed by round state, and the GHASH multiply
 * uses a 256-byte per-key table indexed by accumulator nibbles; both are
 * cache-timing observable. That is a real weakness and it is not fixed
 * here -- see the honest-ceiling section of docs/BROWSER-PLAN.md.
 */

#include <stdint.h>

#define AEAD_MAX_KEY    32
#define AEAD_NONCE_LEN  12            /* all three suites use a 96-bit nonce */
#define AEAD_TAG_LEN    16

/* Documented input ceilings. Both AEADs are safe far past these; the caps
 * exist so a broken or hostile length can never be mistaken for a valid
 * one, and both are far above the 2^14 + 256 byte TLS record limit. */
#define AEAD_MAX_PLAINTEXT  (1UL << 30)
#define AEAD_MAX_AAD        (1UL << 24)

enum {
    AEAD_CHACHA20_POLY1305 = 0,
    AEAD_AES_128_GCM = 1,
    AEAD_AES_256_GCM = 2,
    AEAD_ALG_COUNT = 3
};

/* ---------------- AES (FIPS 197), encryption direction ---------------- */

struct aes_ctx {
    uint32_t rk[60];                  /* expanded key, 4*(Nr+1) words */
    int nr;                           /* 10 for AES-128, 14 for AES-256 */
};

/* keylen must be 16 or 32. Returns -1 otherwise. */
int  aes_init(struct aes_ctx *c, const uint8_t *key, unsigned long keylen);
void aes_encrypt_block(const struct aes_ctx *c, const uint8_t in[16],
                       uint8_t out[16]);

/* ---------------- GHASH (GF(2^128), GCM bit order) ---------------- */

/* z = z * h in GF(2^128) modulo x^128 + x^7 + x^2 + x + 1, with GCM's
 * reflected bit convention (bit 0 of byte 0 is the x^0 coefficient). This
 * is the straightforward shift-and-add form from the specification, kept
 * as the reference the table-driven path inside aead.c is checked against. */
void ghash_mul(uint8_t z[16], const uint8_t h[16]);

/* ---------------- ChaCha20 (RFC 8439) ---------------- */

/* One 64-byte keystream block for the given 256-bit key, 32-bit block
 * counter and 96-bit nonce. */
void chacha20_block(const uint8_t key[32], uint32_t counter,
                    const uint8_t nonce[12], uint8_t out[64]);

/* XOR `len` bytes of keystream, starting at block `counter`, into `in`,
 * writing to `out`. `out` may be exactly `in`. Returns -1 if the message
 * would run the 32-bit block counter past its maximum. */
int  chacha20_xor(const uint8_t key[32], uint32_t counter,
                  const uint8_t nonce[12], const uint8_t *in,
                  unsigned long len, uint8_t *out);

/* ---------------- Poly1305 (RFC 8439) ---------------- */

struct poly1305_ctx {
    uint32_t r[5];                    /* clamped key, 26-bit limbs */
    uint32_t h[5];                    /* accumulator, 26-bit limbs */
    uint32_t pad[4];                  /* the "s" half of the key */
    uint8_t buf[16];
    unsigned long buflen;
};

void poly1305_init(struct poly1305_ctx *c, const uint8_t key[32]);
void poly1305_update(struct poly1305_ctx *c, const void *data,
                     unsigned long len);
void poly1305_final(struct poly1305_ctx *c, uint8_t tag[16]);
void poly1305_mac(const uint8_t key[32], const void *data, unsigned long len,
                  uint8_t tag[16]);

/* ---------------- the AEAD interface ---------------- */

struct aead_ctx {
    int alg;
    struct aes_ctx aes;               /* GCM only */
    uint8_t key[AEAD_MAX_KEY];        /* ChaCha20 only */
    uint64_t htab[16][2];             /* GCM only: H * 0..15, hi/lo halves */
    uint16_t rtab[16];                /* GCM only: x^4 reduction table */
};

unsigned long aead_key_len(int alg);      /* 0 if the algorithm is unknown */
unsigned long aead_nonce_len(int alg);
unsigned long aead_tag_len(int alg);
const char *aead_name(int alg);

/* Set up a context for repeated use with one key -- the normal TLS case,
 * where the AES key schedule and the GHASH table are built once per
 * connection rather than once per record. Returns -1 on an unknown
 * algorithm. */
int aead_init(struct aead_ctx *c, int alg, const uint8_t *key);

/* out receives plainlen + AEAD_TAG_LEN bytes: the ciphertext followed by
 * the tag. `out` may be exactly `plain`, or fully disjoint from it;
 * partial overlap is not supported. Returns -1 on a bad length. */
int aead_ctx_seal(const struct aead_ctx *c, const uint8_t *nonce,
                  const uint8_t *aad, unsigned long aadlen,
                  const uint8_t *plain, unsigned long plainlen,
                  uint8_t *out);

/* `ctlen` counts the trailing tag, so it must be at least AEAD_TAG_LEN,
 * and `out` receives ctlen - AEAD_TAG_LEN bytes. The tag is verified
 * before any plaintext is produced, so a forgery never yields output.
 * Returns -1 on a bad length or a tag mismatch, 0 on success. */
int aead_ctx_open(const struct aead_ctx *c, const uint8_t *nonce,
                  const uint8_t *aad, unsigned long aadlen,
                  const uint8_t *ct, unsigned long ctlen,
                  uint8_t *out);

/* One-shot forms, for callers that are not reusing a key. */
int aead_seal(int alg, const uint8_t *key, const uint8_t *nonce,
              const uint8_t *aad, unsigned long aadlen,
              const uint8_t *plain, unsigned long plainlen, uint8_t *out);
int aead_open(int alg, const uint8_t *key, const uint8_t *nonce,
              const uint8_t *aad, unsigned long aadlen,
              const uint8_t *ct, unsigned long ctlen, uint8_t *out);

/* RFC 8446 section 5.3: the per-record nonce is the 64-bit sequence
 * number, left-padded with zeros to the IV length, XORed with the static
 * write IV. Provided here because it belongs with the AEAD rather than
 * being re-derived by every caller. */
void aead_nonce(const uint8_t *iv, unsigned long ivlen, uint64_t seq,
                uint8_t *out);
