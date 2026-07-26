#pragma once

/* KestrelOS libtls: DER decoding, X.509 certificates, chain verification.
 *
 * This is the part of TLS that parses bytes chosen by whoever is on the
 * other end of the socket, so the DER reader is strict on purpose:
 * definite lengths only, minimal length encoding, minimal tag encoding,
 * every length checked against what is actually left in the buffer, and
 * no recursion that is not bounded by DER_MAX_DEPTH. It never allocates
 * and never copies -- a parsed certificate is a set of pointers into the
 * caller's DER buffer, which must therefore outlive it.
 *
 * What is deliberately not here: CRLs, OCSP, name constraints, policy
 * constraints, and certificate transparency. A chain that would only be
 * rejected by one of those will be accepted. That is stated in
 * docs/BROWSER-PLAN.md as part of the honest ceiling.
 *
 * Space, measured rather than guessed, because the user stack is 64 KiB:
 * a struct x509_cert is 768 bytes, so a full ten-certificate chain is
 * 7.7 KiB; the deepest call inside verification (chain -> signature ->
 * PKCS#1 -> public op -> modexp -> Montgomery) uses about 14 KiB. A
 * struct x509_store is 49 KiB and must live on the heap or in static
 * storage.
 */

#include <stdint.h>
#include <stddef.h>
#include "rsa.h"
#include "ecc.h"

/* ================================================================== */
/* DER                                                                */
/* ================================================================== */

#define DER_MAX_DEPTH 24        /* every descent is explicit, but cap it */

#define DER_BOOLEAN      0x01
#define DER_INTEGER      0x02
#define DER_BIT_STRING   0x03
#define DER_OCTET_STRING 0x04
#define DER_NULL         0x05
#define DER_OID          0x06
#define DER_UTF8STRING   0x0c
#define DER_PRINTABLE    0x13
#define DER_T61STRING    0x14
#define DER_IA5STRING    0x16
#define DER_UTCTIME      0x17
#define DER_GENTIME      0x18
#define DER_BMPSTRING    0x1e
#define DER_SEQUENCE     0x30
#define DER_SET          0x31

#define DER_CLASS_UNIVERSAL 0
#define DER_CLASS_APP       1
#define DER_CLASS_CONTEXT   2
#define DER_CLASS_PRIVATE   3

struct der {
    const uint8_t *p;
    const uint8_t *end;
};

struct der_tlv {
    int            cls;         /* DER_CLASS_*                        */
    int            constructed;
    uint32_t       tag;         /* the tag number, class bits removed */
    const uint8_t *hdr;         /* first byte of the whole TLV        */
    size_t         hdr_len;     /* identifier + length octets         */
    const uint8_t *val;
    size_t         len;
};

void der_init(struct der *d, const uint8_t *buf, size_t len);
int  der_eof(const struct der *d);

/* Read the next TLV. Returns 0 or -1; on -1 the cursor is unchanged. */
int  der_read(struct der *d, struct der_tlv *t);

/* Read the next TLV and require a particular universal primitive tag
 * (DER_INTEGER, DER_OID, ...) or DER_SEQUENCE / DER_SET. */
int  der_read_expect(struct der *d, struct der_tlv *t, uint8_t want);

/* Read the next TLV and require a context-specific tag number. */
int  der_read_context(struct der *d, struct der_tlv *t, uint32_t tag,
                      int constructed);

/* Peek without consuming. */
int  der_peek(const struct der *d, struct der_tlv *t);

/* Make a cursor over the contents of a constructed TLV. */
void der_enter(const struct der_tlv *t, struct der *inner);

/* Typed readers, all strict. */
int  der_read_uint(struct der *d, const uint8_t **val, size_t *len);
int  der_read_u32(struct der *d, uint32_t *out);
int  der_read_bool(struct der *d, int *out);
int  der_read_oid(struct der *d, const uint8_t **oid, size_t *len);
/* A BIT STRING with a whole number of octets; *val excludes the leading
 * unused-bit count, which must be zero. */
int  der_read_bitstring(struct der *d, const uint8_t **val, size_t *len);
/* A BIT STRING used as a flag set: returns the bits with bit 0 being the
 * most significant bit of the first octet, mapped to (1 << 0). */
int  der_read_bitflags(struct der *d, uint32_t *out);

/* UTCTime / GeneralizedTime -> seconds since the epoch. Only the "Z"
 * forms with seconds present are accepted, which is all RFC 5280 allows. */
int  der_read_time(struct der *d, uint32_t *unix_out);

/* YYYY-MM-DD into buf, for error messages. */
void x509_format_date(uint32_t t, char *buf, size_t cap);

/* ================================================================== */
/* PEM                                                                */
/* ================================================================== */

struct pem_iter {
    const char *p;
    const char *end;
};

void pem_iter_init(struct pem_iter *it, const char *text, size_t len);

/* Decode the next "-----BEGIN <label>-----" block into out. Returns 1 on
 * success, 0 when there are no more blocks, -1 on malformed base64 or an
 * output buffer that is too small. */
int  pem_next(struct pem_iter *it, char *label, size_t label_cap,
              uint8_t *out, size_t out_cap, size_t *out_len);

/* ================================================================== */
/* Certificates                                                       */
/* ================================================================== */

#define X509_MAX_CN     128
#define X509_MAX_SAN    16
#define X509_MAX_CHAIN  10
#define X509_STORE_MAX  64
#define X509_ERR_LEN    192
#define X509_MIN_RSA_BITS 2048

#define X509_SIG_RSA_PKCS1 1
#define X509_SIG_RSA_PSS   2
#define X509_SIG_ECDSA     3

#define X509_PK_RSA 1
#define X509_PK_EC  2

#define X509_CURVE_P256 1
#define X509_CURVE_P384 2
#define X509_CURVE_P521 3

#define X509_KU_DIGITAL_SIGNATURE 0x0001
#define X509_KU_NON_REPUDIATION   0x0002
#define X509_KU_KEY_ENCIPHERMENT  0x0004
#define X509_KU_DATA_ENCIPHERMENT 0x0008
#define X509_KU_KEY_AGREEMENT     0x0010
#define X509_KU_KEY_CERT_SIGN     0x0020
#define X509_KU_CRL_SIGN          0x0040
#define X509_KU_ENCIPHER_ONLY     0x0080
#define X509_KU_DECIPHER_ONLY     0x0100

#define X509_EKU_SERVER_AUTH 0x0001
#define X509_EKU_CLIENT_AUTH 0x0002
#define X509_EKU_CODE_SIGN   0x0004
#define X509_EKU_EMAIL       0x0008
#define X509_EKU_TIME_STAMP  0x0010
#define X509_EKU_OCSP_SIGN   0x0020
#define X509_EKU_ANY         0x0040
#define X509_EKU_UNKNOWN     0x8000

struct x509_san {
    const char *p;              /* not NUL-terminated */
    int         len;
    int         is_ip;          /* 1 for an iPAddress, 0 for a dNSName */
};

struct x509_cert {
    const uint8_t *der;
    size_t         der_len;
    const uint8_t *tbs;         /* the signed region, header included  */
    size_t         tbs_len;

    int            version;     /* 1, 2 or 3 */
    const uint8_t *serial;
    size_t         serial_len;

    const uint8_t *issuer_raw;  /* the whole Name TLV, for chaining    */
    size_t         issuer_raw_len;
    const uint8_t *subject_raw;
    size_t         subject_raw_len;
    char           subject_cn[X509_MAX_CN];
    char           issuer_cn[X509_MAX_CN];

    uint32_t       not_before;
    uint32_t       not_after;

    int            sig_alg;     /* X509_SIG_*     */
    int            sig_md;      /* CRYPTO_MD_*    */
    int            sig_mgf_md;  /* PSS only       */
    int            sig_salt_len;/* PSS only, -1 for "recover it" */
    const uint8_t *sig;
    size_t         sig_len;

    int            pk_alg;      /* X509_PK_*      */
    int            pk_curve;    /* X509_CURVE_*   */
    const uint8_t *pk_bits;     /* subjectPublicKey contents */
    size_t         pk_bits_len;

    int            has_basic_constraints;
    int            is_ca;
    int            path_len;    /* -1 when absent */
    int            has_key_usage;
    uint32_t       key_usage;
    int            has_eku;
    uint32_t       eku;
    int            n_san;
    struct x509_san san[X509_MAX_SAN];

    int            unknown_critical;   /* a critical extension we do not know */
    char           unknown_critical_oid[64];
    int            self_signed;        /* issuer == subject                   */
};

/* Parse one DER certificate. The buffer must outlive `c`. Returns 0 or
 * -1; `err` (may be NULL) gets a reason. */
int x509_parse(struct x509_cert *c, const uint8_t *der, size_t len,
               char *err, size_t err_len);

/* ================================================================== */
/* Trust store                                                        */
/* ================================================================== */

struct x509_store {
    int              n;
    struct x509_cert certs[X509_STORE_MAX];
    uint8_t         *owned[X509_STORE_MAX];   /* malloc'ed copies of the DER */
};

/* A store is 49 KiB. Put it on the heap or in static storage, never on
 * the 64 KiB user stack. */
void x509_store_init(struct x509_store *s);
int  x509_store_add_der(struct x509_store *s, const uint8_t *der, size_t len);
/* Adds every CERTIFICATE block in a PEM file. Returns the number added,
 * or -1 if the store filled up or a block was malformed. */
int  x509_store_add_pem(struct x509_store *s, const char *text, size_t len);
void x509_store_free(struct x509_store *s);

/* ================================================================== */
/* Verification                                                       */
/* ================================================================== */

/* Does `cert`'s signature check out against `issuer`'s public key?
 * Returns 1 / 0, with a reason in err. */
int x509_verify_signature(const struct x509_cert *cert,
                          const struct x509_cert *issuer,
                          char *err, size_t err_len);

/* RFC 6125 hostname matching against subjectAltName, falling back to the
 * subject CN only when the certificate carries no SAN at all. Wildcards
 * are accepted only as a complete leftmost label, must not match across a
 * dot, and must leave at least two labels behind them. Returns 1 / 0. */
int x509_check_hostname(const struct x509_cert *cert, const char *host);

/* chain[0] is the leaf and chain[i+1] issued chain[i]. Returns 1 when the
 * chain is trusted for `hostname` at `now_unix`, otherwise 0 with a
 * specific reason written to err. Pass hostname == NULL to skip the name
 * check (useful when verifying a chain for something other than a web
 * origin). */
int x509_verify_chain(const struct x509_cert *chain, int n,
                      const struct x509_store *roots, const char *hostname,
                      uint32_t now_unix, char *err, int errlen);
