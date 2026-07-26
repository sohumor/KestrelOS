#pragma once

/* KestrelOS libtls: RSA signature verification (RFC 8017).
 *
 * Verification only. The browser is a TLS client that authenticates
 * servers; it never holds an RSA private key, so there is no signing,
 * no decryption, and no CRT. That removes the entire class of
 * secret-dependent timing problems from this file: every value here is
 * public.
 *
 * Both signature schemes are checked by *construction*, not by parsing:
 * the expected encoded message is built from the digest and compared to
 * the recovered one with a single memcmp. Nothing branches on where a
 * malformed padding byte was found, which is what makes padding-oracle
 * and Bleichenbacher-style forgeries (BERserk, the "e=3 with slack at the
 * end" family) possible in the parse-and-accept style of implementation.
 */

#include <stdint.h>
#include <stddef.h>
#include "bignum.h"

/* ---- message digest registry ----
 *
 * libtls's hash implementations live in another translation unit; this is
 * the seam between them. Whoever owns the hashes registers them once at
 * startup and everything above -- RSA, X.509 -- looks them up by id. That
 * keeps the registry pattern the rest of the system uses and lets the
 * host test harness plug in its own reference implementations.
 */

#define CRYPTO_MD_SHA1    1
#define CRYPTO_MD_SHA224  2
#define CRYPTO_MD_SHA256  3
#define CRYPTO_MD_SHA384  4
#define CRYPTO_MD_SHA512  5

#define CRYPTO_MD_MAX_LEN 64
#define CRYPTO_MD_SLOTS   8

struct crypto_md {
    int         id;                    /* CRYPTO_MD_*                   */
    const char *name;                  /* "sha256", for error messages  */
    int         digest_len;            /* bytes                         */
    void      (*hash)(const void *data, size_t len, uint8_t *out);
};

/* -1 if the table is full, the descriptor is malformed, or the id is
 * already taken by a different descriptor. Re-registering the identical
 * pointer is a no-op success. */
int crypto_md_register(const struct crypto_md *md);
const struct crypto_md *crypto_md_get(int id);
void crypto_md_clear(void);            /* drops every registration */

/* The DER-encoded OID *contents* (no tag, no length) for a digest, as
 * used inside an AlgorithmIdentifier. Returns NULL for unknown ids. */
const uint8_t *crypto_md_oid(int id, int *len);
/* Reverse lookup: OID contents -> CRYPTO_MD_*, or 0. */
int crypto_md_from_oid(const uint8_t *oid, int len);

/* ---- keys ---- */

#define RSA_MIN_BITS 1024              /* refuse anything smaller outright */

struct rsa_pubkey {
    struct bn      n;
    struct bn      e;
    struct bn_mont mont;
    int            bits;               /* bit length of n   */
    int            k;                  /* byte length of n  */
};

/* Parse RSAPublicKey ::= SEQUENCE { modulus INTEGER, publicExponent
 * INTEGER } -- the contents of an RSA SubjectPublicKeyInfo bit string.
 * Rejects: trailing garbage, negative or non-minimal INTEGERs, an even or
 * < 3 exponent, an exponent wider than 64 bits, an even modulus, and any
 * modulus below RSA_MIN_BITS or above BN_MAX_BITS. Returns 0 or -1. */
int rsa_pubkey_from_der(struct rsa_pubkey *k, const uint8_t *der, size_t len);

/* ---- verification ----
 *
 * Both take the *digest*, not the message, because X.509 hashes a
 * to-be-signed region that the caller already has. Return 1 for a good
 * signature and 0 for anything else -- there is deliberately no
 * distinction between "bad padding" and "wrong hash".
 */

int rsa_verify_pkcs1_v15(const struct rsa_pubkey *key, int md_id,
                         const uint8_t *digest, size_t digest_len,
                         const uint8_t *sig, size_t sig_len);

/* salt_len < 0 means "recover the salt length from the encoded message",
 * which is what RFC 8017's auto variant and most X.509 profiles want. */
int rsa_verify_pss(const struct rsa_pubkey *key, int md_id, int mgf_md_id,
                   int salt_len, const uint8_t *digest, size_t digest_len,
                   const uint8_t *sig, size_t sig_len);

/* Raw public operation, exposed for tests: out = sig^e mod n, written as
 * exactly key->k big-endian bytes. Returns 0 or -1. */
int rsa_public(const struct rsa_pubkey *key, const uint8_t *sig,
               size_t sig_len, uint8_t *out);
