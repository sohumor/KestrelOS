/* KestrelOS libtls: RSA signature verification, from RFC 8017.
 *
 * The public key operation is s^e mod n over the bignum layer. What
 * matters for security is everything after it: how the recovered encoded
 * message is checked. Both schemes here build the message they expect and
 * compare it whole, so a signature is accepted only if it reproduces the
 * one encoding this file would have produced. There is no "skip the
 * padding, find the digest, compare that" path, which is the shape of
 * every practical RSA forgery against a low public exponent.
 */

#include "rsa.h"
#include <string.h>

/* ---------------- digest registry ---------------- */

static const struct crypto_md *md_slots[CRYPTO_MD_SLOTS];

int crypto_md_register(const struct crypto_md *md)
{
    int i, free_slot = -1;

    if (!md || !md->hash || md->id <= 0 || md->digest_len <= 0 ||
        md->digest_len > CRYPTO_MD_MAX_LEN)
        return -1;
    for (i = 0; i < CRYPTO_MD_SLOTS; i++) {
        if (md_slots[i] == md)
            return 0;
        if (md_slots[i] && md_slots[i]->id == md->id)
            return -1;
        if (!md_slots[i] && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0)
        return -1;
    md_slots[free_slot] = md;
    return 0;
}

const struct crypto_md *crypto_md_get(int id)
{
    int i;

    for (i = 0; i < CRYPTO_MD_SLOTS; i++) {
        if (md_slots[i] && md_slots[i]->id == id)
            return md_slots[i];
    }
    return 0;
}

void crypto_md_clear(void)
{
    int i;

    for (i = 0; i < CRYPTO_MD_SLOTS; i++)
        md_slots[i] = 0;
}

/* Digest OIDs. These are published identifiers, not an implementation:
 * SHA-1 is the OIW value 1.3.14.3.2.26, the SHA-2 family lives under
 * joint-iso-itu-t nistAlgorithms 2.16.840.1.101.3.4.2.x. */
static const uint8_t oid_sha1[]   = { 0x2b, 0x0e, 0x03, 0x02, 0x1a };
static const uint8_t oid_sha224[] = { 0x60, 0x86, 0x48, 0x01, 0x65,
                                      0x03, 0x04, 0x02, 0x04 };
static const uint8_t oid_sha256[] = { 0x60, 0x86, 0x48, 0x01, 0x65,
                                      0x03, 0x04, 0x02, 0x01 };
static const uint8_t oid_sha384[] = { 0x60, 0x86, 0x48, 0x01, 0x65,
                                      0x03, 0x04, 0x02, 0x02 };
static const uint8_t oid_sha512[] = { 0x60, 0x86, 0x48, 0x01, 0x65,
                                      0x03, 0x04, 0x02, 0x03 };

struct md_oid {
    int            id;
    const uint8_t *oid;
    int            len;
};

static const struct md_oid md_oids[] = {
    { CRYPTO_MD_SHA1,   oid_sha1,   (int)sizeof(oid_sha1)   },
    { CRYPTO_MD_SHA224, oid_sha224, (int)sizeof(oid_sha224) },
    { CRYPTO_MD_SHA256, oid_sha256, (int)sizeof(oid_sha256) },
    { CRYPTO_MD_SHA384, oid_sha384, (int)sizeof(oid_sha384) },
    { CRYPTO_MD_SHA512, oid_sha512, (int)sizeof(oid_sha512) }
};
#define MD_OID_COUNT ((int)(sizeof(md_oids) / sizeof(md_oids[0])))

const uint8_t *crypto_md_oid(int id, int *len)
{
    int i;

    for (i = 0; i < MD_OID_COUNT; i++) {
        if (md_oids[i].id == id) {
            if (len)
                *len = md_oids[i].len;
            return md_oids[i].oid;
        }
    }
    return 0;
}

int crypto_md_from_oid(const uint8_t *oid, int len)
{
    int i;

    for (i = 0; i < MD_OID_COUNT; i++) {
        if (md_oids[i].len == len && memcmp(md_oids[i].oid, oid, (size_t)len) == 0)
            return md_oids[i].id;
    }
    return 0;
}

/* ---------------- a very small DER reader ----------------
 *
 * rsa.c parses exactly one shape -- SEQUENCE { INTEGER, INTEGER } -- and
 * x509.c owns the general parser. Duplicating forty lines here keeps the
 * dependency pointing one way (x509 -> rsa) instead of in a circle.
 */

struct der_cursor {
    const uint8_t *p;
    const uint8_t *end;
};

static int der_tag(struct der_cursor *c, uint8_t want, const uint8_t **val,
                   size_t *len)
{
    size_t l;
    size_t avail;

    if (c->p >= c->end || *c->p != want)
        return -1;
    c->p++;
    if (c->p >= c->end)
        return -1;
    avail = (size_t)(c->end - c->p);
    if (*c->p < 0x80) {
        l = *c->p++;
    } else {
        int nb = *c->p++ & 0x7f;
        int i;
        if (nb == 0 || nb > 4 || (size_t)nb + 1 > avail)
            return -1;
        l = 0;
        for (i = 0; i < nb; i++)
            l = (l << 8) | *c->p++;
        if (l < 0x80)
            return -1;                     /* non-minimal length */
        if (nb > 1 && l < ((size_t)1 << (8 * (nb - 1))))
            return -1;
    }
    if (l > (size_t)(c->end - c->p))
        return -1;
    *val = c->p;
    *len = l;
    c->p += l;
    return 0;
}

/* A DER INTEGER that must be a non-negative, minimally encoded number. */
static int der_uint(struct der_cursor *c, const uint8_t **val, size_t *len)
{
    if (der_tag(c, 0x02, val, len) < 0)
        return -1;
    if (*len == 0)
        return -1;
    if ((*val)[0] & 0x80)
        return -1;                         /* negative */
    if (*len > 1 && (*val)[0] == 0 && !((*val)[1] & 0x80))
        return -1;                         /* non-minimal padding */
    if (*len > 1 && (*val)[0] == 0) {
        (*val)++;
        (*len)--;
    }
    return 0;
}

/* ---------------- key parsing ---------------- */

int rsa_pubkey_from_der(struct rsa_pubkey *key, const uint8_t *der, size_t len)
{
    struct der_cursor top, seq;
    const uint8_t *body, *nv, *ev;
    size_t blen, nlen, elen;

    if (!key || !der)
        return -1;
    top.p = der;
    top.end = der + len;
    if (der_tag(&top, 0x30, &body, &blen) < 0)
        return -1;
    if (top.p != top.end)
        return -1;                         /* trailing garbage */
    seq.p = body;
    seq.end = body + blen;
    if (der_uint(&seq, &nv, &nlen) < 0)
        return -1;
    if (der_uint(&seq, &ev, &elen) < 0)
        return -1;
    if (seq.p != seq.end)
        return -1;

    if (elen == 0 || elen > 8)
        return -1;                         /* absurd exponent */
    if (bn_from_bytes(&key->n, nv, nlen) < 0)
        return -1;
    if (bn_from_bytes(&key->e, ev, elen) < 0)
        return -1;

    key->bits = bn_bits(&key->n);
    key->k = (key->bits + 7) / 8;
    if (key->bits < RSA_MIN_BITS || key->bits > BN_MAX_BITS)
        return -1;
    if (!bn_is_odd(&key->n))
        return -1;
    if (!bn_is_odd(&key->e))
        return -1;
    if (bn_bits(&key->e) < 2)
        return -1;                         /* e >= 3 */
    if (bn_mont_init(&key->mont, &key->n) < 0)
        return -1;
    return 0;
}

/* ---------------- the public operation ---------------- */

int rsa_public(const struct rsa_pubkey *key, const uint8_t *sig,
               size_t sig_len, uint8_t *out)
{
    struct bn s, m;

    if (!key || !sig || !out || key->k <= 0)
        return -1;
    if (sig_len != (size_t)key->k)
        return -1;
    if (bn_from_bytes(&s, sig, sig_len) < 0)
        return -1;
    if (bn_cmp(&s, &key->n) >= 0)
        return -1;                         /* signature out of range */
    if (bn_modexp(&m, &s, &key->e, &key->mont) < 0)
        return -1;
    if (bn_to_bytes(&m, out, (size_t)key->k) < 0)
        return -1;
    return 0;
}

/* ---------------- PKCS#1 v1.5 ---------------- */

/* DigestInfo ::= SEQUENCE { AlgorithmIdentifier, OCTET STRING }.
 *
 * Two encodings are in the wild: with an explicit NULL parameter (what
 * RFC 8017 mandates) and with the parameter absent (what some older
 * signers emit). Both are built here and the recovered block must equal
 * one of them exactly. */
static int build_digestinfo(uint8_t *out, size_t cap, int md_id,
                            const uint8_t *digest, size_t dlen, int with_null)
{
    const uint8_t *oid;
    int oidlen = 0;
    size_t alg_content, di_content, total;
    size_t i = 0;

    oid = crypto_md_oid(md_id, &oidlen);
    if (!oid)
        return -1;
    /* AlgorithmIdentifier contents: the OID, plus an explicit NULL when
     * the caller wants that variant. */
    alg_content = 2 + (size_t)oidlen + (with_null ? 2u : 0u);
    /* DigestInfo contents: the AlgorithmIdentifier and the OCTET STRING,
     * each with its own two-byte header. */
    di_content = (2 + alg_content) + (2 + dlen);
    total = 2 + di_content;
    if (total > cap || di_content > 0x7f || dlen > 0x7f)
        return -1;

    out[i++] = 0x30;
    out[i++] = (uint8_t)di_content;
    out[i++] = 0x30;
    out[i++] = (uint8_t)alg_content;
    out[i++] = 0x06;
    out[i++] = (uint8_t)oidlen;
    memcpy(out + i, oid, (size_t)oidlen);
    i += (size_t)oidlen;
    if (with_null) {
        out[i++] = 0x05;
        out[i++] = 0x00;
    }
    out[i++] = 0x04;
    out[i++] = (uint8_t)dlen;
    memcpy(out + i, digest, dlen);
    i += dlen;
    return (int)i;
}

int rsa_verify_pkcs1_v15(const struct rsa_pubkey *key, int md_id,
                         const uint8_t *digest, size_t digest_len,
                         const uint8_t *sig, size_t sig_len)
{
    uint8_t em[BN_MAX_BYTES];
    uint8_t want[BN_MAX_BYTES];
    uint8_t di[128];
    int variant;

    if (!key || !digest || !sig)
        return 0;
    if (digest_len == 0 || digest_len > CRYPTO_MD_MAX_LEN)
        return 0;
    if (key->k <= 0 || (size_t)key->k > sizeof(em))
        return 0;
    if (sig_len != (size_t)key->k)
        return 0;
    if (rsa_public(key, sig, sig_len, em) < 0)
        return 0;

    for (variant = 0; variant < 2; variant++) {
        int dilen = build_digestinfo(di, sizeof(di), md_id, digest,
                                     digest_len, variant == 0);
        size_t pslen;
        if (dilen < 0)
            continue;
        if ((size_t)key->k < (size_t)dilen + 11)
            continue;                      /* no room for 8 bytes of padding */
        pslen = (size_t)key->k - (size_t)dilen - 3;
        want[0] = 0x00;
        want[1] = 0x01;
        memset(want + 2, 0xff, pslen);
        want[2 + pslen] = 0x00;
        memcpy(want + 3 + pslen, di, (size_t)dilen);
        if (memcmp(want, em, (size_t)key->k) == 0)
            return 1;
    }
    return 0;
}

/* ---------------- PSS ---------------- */

/* MGF1 from RFC 8017 appendix B.2.1: the mask is Hash(seed || counter)
 * for counter = 0, 1, 2, ... truncated to the requested length. */
static int mgf1(const struct crypto_md *md, const uint8_t *seed, size_t seedlen,
                uint8_t *mask, size_t masklen)
{
    uint8_t buf[CRYPTO_MD_MAX_LEN + 8];
    uint8_t dig[CRYPTO_MD_MAX_LEN];
    uint32_t counter = 0;
    size_t done = 0;

    if (seedlen > CRYPTO_MD_MAX_LEN)
        return -1;
    memcpy(buf, seed, seedlen);
    while (done < masklen) {
        size_t take = masklen - done;
        buf[seedlen + 0] = (uint8_t)(counter >> 24);
        buf[seedlen + 1] = (uint8_t)(counter >> 16);
        buf[seedlen + 2] = (uint8_t)(counter >> 8);
        buf[seedlen + 3] = (uint8_t)counter;
        md->hash(buf, seedlen + 4, dig);
        if (take > (size_t)md->digest_len)
            take = (size_t)md->digest_len;
        memcpy(mask + done, dig, take);
        done += take;
        counter++;
        if (counter == 0)
            return -1;
    }
    return 0;
}

int rsa_verify_pss(const struct rsa_pubkey *key, int md_id, int mgf_md_id,
                   int salt_len, const uint8_t *digest, size_t digest_len,
                   const uint8_t *sig, size_t sig_len)
{
    const struct crypto_md *md = crypto_md_get(md_id);
    const struct crypto_md *mgfmd = crypto_md_get(mgf_md_id);
    uint8_t em[BN_MAX_BYTES];
    uint8_t db[BN_MAX_BYTES];
    uint8_t mask[BN_MAX_BYTES];
    uint8_t mprime[8 + CRYPTO_MD_MAX_LEN + BN_MAX_BYTES];
    uint8_t hprime[CRYPTO_MD_MAX_LEN];
    const uint8_t *h;
    size_t emlen, dblen, i, sep, slen;
    int embits, drop;

    if (!key || !md || !mgfmd || !digest || !sig)
        return 0;
    if (digest_len != (size_t)md->digest_len)
        return 0;
    if (key->k <= 0 || (size_t)key->k > sizeof(em))
        return 0;
    if (sig_len != (size_t)key->k)
        return 0;

    embits = key->bits - 1;
    emlen = (size_t)((embits + 7) / 8);
    if (emlen < digest_len + 2)
        return 0;
    if (rsa_public(key, sig, sig_len, em) < 0)
        return 0;
    /* The encoded message is emLen bytes; when the modulus bit length is
     * one more than a whole number of bytes, EM is one byte shorter than
     * the modulus and that leading byte must be zero. */
    if (emlen < (size_t)key->k) {
        size_t pad = (size_t)key->k - emlen;
        for (i = 0; i < pad; i++) {
            if (em[i] != 0)
                return 0;
        }
        memmove(em, em + pad, emlen);
    }

    if (em[emlen - 1] != 0xbc)
        return 0;
    dblen = emlen - digest_len - 1;
    h = em + dblen;

    drop = 8 * (int)emlen - embits;
    if (drop < 0 || drop > 7)
        return 0;
    if (drop && (em[0] >> (8 - drop)) != 0)
        return 0;

    if (mgf1(mgfmd, h, digest_len, mask, dblen) < 0)
        return 0;
    for (i = 0; i < dblen; i++)
        db[i] = em[i] ^ mask[i];
    if (drop)
        db[0] &= (uint8_t)(0xff >> drop);

    /* DB = PS || 0x01 || salt, PS all zero. */
    sep = 0;
    while (sep < dblen && db[sep] == 0)
        sep++;
    if (sep >= dblen || db[sep] != 0x01)
        return 0;
    slen = dblen - sep - 1;
    if (salt_len >= 0 && slen != (size_t)salt_len)
        return 0;
    if (slen > BN_MAX_BYTES)
        return 0;

    /* M' = eight zero bytes || mHash || salt; H' must equal H. */
    memset(mprime, 0, 8);
    memcpy(mprime + 8, digest, digest_len);
    memcpy(mprime + 8 + digest_len, db + sep + 1, slen);
    md->hash(mprime, 8 + digest_len + slen, hprime);
    if (memcmp(hprime, h, digest_len) != 0)
        return 0;
    return 1;
}
