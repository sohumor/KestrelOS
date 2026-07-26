/* KestrelOS libtls: DER, X.509 and chain verification.
 *
 * Everything in this file reads attacker-controlled bytes. Three rules
 * hold throughout:
 *
 *   1. No pointer moves without first checking it against the end of the
 *      buffer it came from. der_read() is the only place lengths are
 *      decoded, and it refuses indefinite lengths, non-minimal lengths,
 *      non-minimal tags, and anything that claims to be longer than what
 *      is left.
 *   2. No recursion. The certificate grammar is walked with explicit
 *      loops, and the one place a general descent is needed -- the
 *      up-front structural check -- uses an explicit stack capped at
 *      DER_MAX_DEPTH. A malformed certificate cannot put the 64 KiB user
 *      stack anywhere near its limit.
 *   3. Nothing is allocated. A parsed certificate points into the
 *      caller's buffer. Only the trust store copies, and it bounds every
 *      copy at X509_MAX_DER.
 */

#include "x509.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define X509_MAX_DER 16384      /* bound on a certificate the store copies */

/* ================================================================== */
/* DER                                                                */
/* ================================================================== */

void der_init(struct der *d, const uint8_t *buf, size_t len)
{
    d->p = buf;
    d->end = buf + len;
}

int der_eof(const struct der *d)
{
    return d->p >= d->end;
}

int der_read(struct der *d, struct der_tlv *t)
{
    const uint8_t *p = d->p;
    size_t avail, len, used;
    uint8_t id;

    if (p >= d->end)
        return -1;
    avail = (size_t)(d->end - p);
    id = *p++;
    t->hdr = d->p;
    t->cls = id >> 6;
    t->constructed = (id >> 5) & 1;

    if ((id & 0x1f) != 0x1f) {
        t->tag = id & 0x1f;
    } else {
        uint32_t tag = 0;
        int i = 0;
        for (;;) {
            uint8_t b;
            if ((size_t)(p - d->p) >= avail)
                return -1;
            b = *p++;
            if (i == 0 && (b & 0x7f) == 0)
                return -1;                 /* leading zero: not minimal */
            if (tag > (0xffffffffu >> 7))
                return -1;
            tag = (tag << 7) | (b & 0x7f);
            i++;
            if (!(b & 0x80))
                break;
            if (i >= 5)
                return -1;
        }
        if (tag < 0x1f)
            return -1;                     /* should have used short form */
        t->tag = tag;
    }

    if ((size_t)(p - d->p) >= avail)
        return -1;
    len = *p++;
    if (len & 0x80) {
        int nb = (int)(len & 0x7f), i;
        if (nb == 0)
            return -1;                     /* indefinite length is not DER */
        if (nb > 4)
            return -1;
        if ((size_t)(p - d->p) + (size_t)nb > avail)
            return -1;
        len = 0;
        for (i = 0; i < nb; i++)
            len = (len << 8) | *p++;
        if (len < 0x80)
            return -1;                     /* should have used short form */
        if (nb > 1 && len < ((size_t)1 << (8 * (nb - 1))))
            return -1;                     /* leading zero length octet */
    }
    used = (size_t)(p - d->p);
    if (len > avail - used)
        return -1;
    t->hdr_len = used;
    t->val = p;
    t->len = len;
    d->p = p + len;
    return 0;
}

int der_peek(const struct der *d, struct der_tlv *t)
{
    struct der tmp = *d;
    return der_read(&tmp, t);
}

int der_read_expect(struct der *d, struct der_tlv *t, uint8_t want)
{
    struct der save = *d;

    if (der_read(d, t) < 0)
        return -1;
    if (t->cls != DER_CLASS_UNIVERSAL || t->tag != (uint32_t)(want & 0x1f) ||
        t->constructed != ((want >> 5) & 1)) {
        *d = save;
        return -1;
    }
    return 0;
}

int der_read_context(struct der *d, struct der_tlv *t, uint32_t tag,
                     int constructed)
{
    struct der save = *d;

    if (der_read(d, t) < 0)
        return -1;
    if (t->cls != DER_CLASS_CONTEXT || t->tag != tag ||
        t->constructed != constructed) {
        *d = save;
        return -1;
    }
    return 0;
}

void der_enter(const struct der_tlv *t, struct der *inner)
{
    inner->p = t->val;
    inner->end = t->val + t->len;
}

int der_read_uint(struct der *d, const uint8_t **val, size_t *len)
{
    struct der_tlv t;
    struct der save = *d;

    if (der_read_expect(d, &t, DER_INTEGER) < 0)
        return -1;
    if (t.len == 0)
        goto bad;
    if (t.val[0] & 0x80)
        goto bad;                          /* negative */
    if (t.len > 1 && t.val[0] == 0x00 && !(t.val[1] & 0x80))
        goto bad;                          /* non-minimal */
    *val = t.val;
    *len = t.len;
    if (*len > 1 && (*val)[0] == 0x00) {
        (*val)++;
        (*len)--;
    }
    return 0;
bad:
    *d = save;
    return -1;
}

int der_read_u32(struct der *d, uint32_t *out)
{
    const uint8_t *v;
    size_t l, i;
    uint32_t acc = 0;

    if (der_read_uint(d, &v, &l) < 0)
        return -1;
    if (l > 4)
        return -1;
    for (i = 0; i < l; i++)
        acc = (acc << 8) | v[i];
    *out = acc;
    return 0;
}

int der_read_bool(struct der *d, int *out)
{
    struct der_tlv t;
    struct der save = *d;

    if (der_read_expect(d, &t, DER_BOOLEAN) < 0)
        return -1;
    if (t.len != 1 || (t.val[0] != 0x00 && t.val[0] != 0xff)) {
        *d = save;
        return -1;
    }
    *out = t.val[0] ? 1 : 0;
    return 0;
}

/* An OID's contents are a run of base-128 subidentifiers; each must end
 * before the buffer does and must not start with a 0x80 continuation
 * byte, which would be a non-minimal encoding. */
static int oid_wellformed(const uint8_t *v, size_t len)
{
    size_t i;
    int start = 1;

    if (len == 0)
        return 0;
    for (i = 0; i < len; i++) {
        if (start && v[i] == 0x80)
            return 0;
        start = (v[i] & 0x80) ? 0 : 1;
    }
    return start;                          /* last byte must end a subid */
}

int der_read_oid(struct der *d, const uint8_t **oid, size_t *len)
{
    struct der_tlv t;
    struct der save = *d;

    if (der_read_expect(d, &t, DER_OID) < 0)
        return -1;
    if (!oid_wellformed(t.val, t.len)) {
        *d = save;
        return -1;
    }
    *oid = t.val;
    *len = t.len;
    return 0;
}

int der_read_bitstring(struct der *d, const uint8_t **val, size_t *len)
{
    struct der_tlv t;
    struct der save = *d;

    if (der_read_expect(d, &t, DER_BIT_STRING) < 0)
        return -1;
    if (t.len < 1 || t.val[0] != 0x00) {
        *d = save;
        return -1;                         /* must be a whole number of octets */
    }
    *val = t.val + 1;
    *len = t.len - 1;
    return 0;
}

int der_read_bitflags(struct der *d, uint32_t *out)
{
    struct der_tlv t;
    struct der save = *d;
    uint32_t bits = 0;
    size_t i, nbits;
    int unused;

    if (der_read_expect(d, &t, DER_BIT_STRING) < 0)
        return -1;
    if (t.len < 1)
        goto bad;
    unused = t.val[0];
    if (unused > 7)
        goto bad;
    if (t.len == 1 && unused != 0)
        goto bad;
    if (t.len > 1 && (t.val[t.len - 1] & ((1 << unused) - 1)) != 0)
        goto bad;                          /* DER: trailing bits must be zero */
    nbits = (t.len - 1) * 8 - (size_t)unused;
    if (nbits > 32)
        nbits = 32;
    for (i = 0; i < nbits; i++) {
        if (t.val[1 + i / 8] & (0x80 >> (i % 8)))
            bits |= (uint32_t)1 << i;
    }
    *out = bits;
    return 0;
bad:
    *d = save;
    return -1;
}

/* ---- structural pre-check ----
 *
 * Walk every constructed TLV and require that its children exactly fill
 * it. Primitive contents are left alone: a BIT STRING holding a public
 * key is not DER inside. The stack is explicit and capped, so a
 * certificate nested a million deep is rejected, not fatal.
 */
static int der_structure_ok(const uint8_t *buf, size_t len)
{
    const uint8_t *stack[DER_MAX_DEPTH];
    struct der d;
    int depth = 0;

    der_init(&d, buf, len);
    for (;;) {
        struct der_tlv t;
        while (depth > 0 && d.p == stack[depth - 1]) {
            depth--;
            d.end = (depth > 0) ? stack[depth - 1] : buf + len;
        }
        if (d.p >= d.end) {
            if (depth == 0)
                return 1;
            return 0;                      /* should have been caught above */
        }
        if (der_read(&d, &t) < 0)
            return 0;
        if (t.constructed && t.len > 0) {
            if (depth >= DER_MAX_DEPTH)
                return 0;
            stack[depth++] = t.val + t.len;
            d.p = t.val;
            d.end = t.val + t.len;
        }
    }
}

/* ================================================================== */
/* Time                                                               */
/* ================================================================== */

static const int mdays[12] = { 31, 28, 31, 30, 31, 30,
                               31, 31, 30, 31, 30, 31 };

static int is_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int64_t civil_to_unix(int y, int mo, int d, int h, int mi, int s)
{
    static const int cum[12] = { 0, 31, 59, 90, 120, 151,
                                 181, 212, 243, 273, 304, 334 };
    int64_t days, leaps;

    leaps = (int64_t)((y - 1) / 4 - (y - 1) / 100 + (y - 1) / 400) -
            (1969 / 4 - 1969 / 100 + 1969 / 400);
    days = (int64_t)(y - 1970) * 365 + leaps + cum[mo - 1] + (d - 1);
    if (mo > 2 && is_leap(y))
        days += 1;
    return days * 86400 + (int64_t)h * 3600 + (int64_t)mi * 60 + s;
}

void x509_format_date(uint32_t t, char *buf, size_t cap)
{
    int y = 1970, mo = 0;
    int64_t days = (int64_t)t / 86400;

    while (days >= (is_leap(y) ? 366 : 365)) {
        days -= is_leap(y) ? 366 : 365;
        y++;
    }
    for (mo = 0; mo < 12; mo++) {
        int dm = mdays[mo] + ((mo == 1 && is_leap(y)) ? 1 : 0);
        if (days < dm)
            break;
        days -= dm;
    }
    if (mo > 11)
        mo = 11;
    snprintf(buf, cap, "%04d-%02d-%02d", y, mo + 1, (int)days + 1);
}

static int dig2(const uint8_t *p, int *out)
{
    if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9')
        return -1;
    *out = (p[0] - '0') * 10 + (p[1] - '0');
    return 0;
}

int der_read_time(struct der *d, uint32_t *unix_out)
{
    struct der_tlv t;
    struct der save = *d;
    const uint8_t *v;
    int y = 0, mo, dd, h, mi, s, off, dm;
    int64_t u;

    if (der_read(d, &t) < 0)
        return -1;
    if (t.cls != DER_CLASS_UNIVERSAL || t.constructed)
        goto bad;
    v = t.val;
    if (t.tag == DER_UTCTIME) {
        int yy;
        if (t.len != 13 || v[12] != 'Z')
            goto bad;
        if (dig2(v, &yy) < 0)
            goto bad;
        y = yy < 50 ? 2000 + yy : 1900 + yy;
        off = 2;
    } else if (t.tag == DER_GENTIME) {
        int c, yy;
        if (t.len != 15 || v[14] != 'Z')
            goto bad;
        if (dig2(v, &c) < 0 || dig2(v + 2, &yy) < 0)
            goto bad;
        y = c * 100 + yy;
        off = 4;
    } else {
        goto bad;
    }
    if (dig2(v + off, &mo) < 0 || dig2(v + off + 2, &dd) < 0 ||
        dig2(v + off + 4, &h) < 0 || dig2(v + off + 6, &mi) < 0 ||
        dig2(v + off + 8, &s) < 0)
        goto bad;
    if (mo < 1 || mo > 12 || h > 23 || mi > 59 || s > 60)
        goto bad;
    dm = mdays[mo - 1] + ((mo == 2 && is_leap(y)) ? 1 : 0);
    if (dd < 1 || dd > dm)
        goto bad;
    if (s == 60)
        s = 59;                            /* a leap second, close enough */
    if (y < 1970)
        goto bad;
    u = civil_to_unix(y, mo, dd, h, mi, s);
    if (u < 0)
        goto bad;
    if (u > 0xffffffffLL)
        u = 0xffffffffLL;                  /* saturate rather than wrap */
    *unix_out = (uint32_t)u;
    return 0;
bad:
    *d = save;
    return -1;
}

/* ================================================================== */
/* PEM                                                                */
/* ================================================================== */

void pem_iter_init(struct pem_iter *it, const char *text, size_t len)
{
    it->p = text;
    it->end = text + len;
}

static int b64_val(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

static const char *find_str(const char *p, const char *end, const char *needle)
{
    size_t nl = strlen(needle);

    if (nl == 0 || (size_t)(end - p) < nl)
        return 0;
    for (; p <= end - (long)nl; p++) {
        if (memcmp(p, needle, nl) == 0)
            return p;
    }
    return 0;
}

int pem_next(struct pem_iter *it, char *label, size_t label_cap,
             uint8_t *out, size_t out_cap, size_t *out_len)
{
    const char *begin, *label_start, *label_end, *body, *end_marker;
    char endtag[80];
    size_t llen;
    uint32_t acc = 0;
    int nbits = 0, pad = 0;
    size_t n = 0;
    const char *q;

    if (!it || it->p >= it->end)
        return 0;
    begin = find_str(it->p, it->end, "-----BEGIN ");
    if (!begin)
        return 0;
    label_start = begin + 11;
    label_end = find_str(label_start, it->end, "-----");
    if (!label_end)
        return -1;
    llen = (size_t)(label_end - label_start);
    if (llen == 0 || llen > 64)
        return -1;
    if (label) {
        size_t copy;
        if (label_cap == 0)
            return -1;
        copy = llen < label_cap - 1 ? llen : label_cap - 1;
        memcpy(label, label_start, copy);
        label[copy] = 0;
    }
    body = label_end + 5;

    if (llen + 20 > sizeof(endtag))
        return -1;
    memcpy(endtag, "-----END ", 9);
    memcpy(endtag + 9, label_start, llen);
    memcpy(endtag + 9 + llen, "-----", 5);
    endtag[9 + llen + 5] = 0;

    end_marker = find_str(body, it->end, endtag);
    if (!end_marker)
        return -1;

    for (q = body; q < end_marker; q++) {
        int c = (unsigned char)*q, v;
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t')
            continue;
        if (c == '=') {
            pad++;
            if (pad > 2)
                return -1;
            continue;
        }
        if (pad)
            return -1;                     /* data after padding */
        v = b64_val(c);
        if (v < 0)
            return -1;
        acc = (acc << 6) | (uint32_t)v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            if (n >= out_cap)
                return -1;
            out[n++] = (uint8_t)(acc >> nbits);
        }
    }
    if ((nbits != 0 && (acc & ((1u << nbits) - 1)) != 0))
        return -1;                         /* stray non-zero bits */
    if (pad == 1 && nbits != 2)
        return -1;
    if (pad == 2 && nbits != 4)
        return -1;
    if (pad == 0 && nbits != 0)
        return -1;

    *out_len = n;
    it->p = end_marker + strlen(endtag);
    return 1;
}

/* ================================================================== */
/* Object identifiers                                                 */
/* ================================================================== */

#define OID_DEF(name, ...) static const uint8_t name[] = { __VA_ARGS__ }

OID_DEF(oid_cn,           0x55, 0x04, 0x03);
OID_DEF(oid_rsa_enc,      0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01);
OID_DEF(oid_rsa_pss,      0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0a);
OID_DEF(oid_mgf1,         0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x08);
OID_DEF(oid_sha1_rsa,     0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x05);
OID_DEF(oid_sha224_rsa,   0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0e);
OID_DEF(oid_sha256_rsa,   0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b);
OID_DEF(oid_sha384_rsa,   0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0c);
OID_DEF(oid_sha512_rsa,   0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0d);
OID_DEF(oid_ec_pubkey,    0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01);
OID_DEF(oid_ecdsa_sha1,   0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x01);
OID_DEF(oid_ecdsa_sha224, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x01);
OID_DEF(oid_ecdsa_sha256, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02);
OID_DEF(oid_ecdsa_sha384, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x03);
OID_DEF(oid_ecdsa_sha512, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x04);
OID_DEF(oid_p256,         0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07);
OID_DEF(oid_p384,         0x2b, 0x81, 0x04, 0x00, 0x22);
OID_DEF(oid_p521,         0x2b, 0x81, 0x04, 0x00, 0x23);
OID_DEF(oid_ext_san,      0x55, 0x1d, 0x11);
OID_DEF(oid_ext_bc,       0x55, 0x1d, 0x13);
OID_DEF(oid_ext_ku,       0x55, 0x1d, 0x0f);
OID_DEF(oid_ext_eku,      0x55, 0x1d, 0x25);
OID_DEF(oid_ext_skid,     0x55, 0x1d, 0x0e);
OID_DEF(oid_ext_akid,     0x55, 0x1d, 0x23);
OID_DEF(oid_ext_cp,       0x55, 0x1d, 0x20);
OID_DEF(oid_eku_server,   0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01);
OID_DEF(oid_eku_client,   0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x02);
OID_DEF(oid_eku_code,     0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x03);
OID_DEF(oid_eku_email,    0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x04);
OID_DEF(oid_eku_stamp,    0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x08);
OID_DEF(oid_eku_ocsp,     0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x09);
OID_DEF(oid_eku_any,      0x55, 0x1d, 0x25, 0x00);

#define OID_IS(v, l, k) ((l) == sizeof(k) && memcmp((v), (k), sizeof(k)) == 0)

/* Turn an OID into dotted decimal for an error message. Bounded output;
 * anything that does not fit is truncated. */
static void oid_to_text(const uint8_t *v, size_t len, char *out, size_t cap)
{
    size_t i, n = 0;
    uint64_t acc = 0;
    int first = 1;

    if (cap == 0)
        return;
    out[0] = 0;
    for (i = 0; i < len; i++) {
        char part[32];
        int plen;

        if (acc > ((uint64_t)1 << 56))
            break;                         /* a subidentifier this large is
                                            * not going to be meaningful */
        acc = (acc << 7) | (v[i] & 0x7f);
        if (v[i] & 0x80)
            continue;
        if (first) {
            unsigned a = (unsigned)(acc / 40), b = (unsigned)(acc % 40);
            if (a > 2) {
                a = 2;
                b = (unsigned)(acc - 80);
            }
            plen = snprintf(part, sizeof(part), "%u.%u", a, b);
            first = 0;
        } else {
            plen = snprintf(part, sizeof(part), ".%llu",
                            (unsigned long long)acc);
        }
        acc = 0;
        /* snprintf reports what it *would* have written, so the running
         * offset must never be advanced by more than the room left. */
        if (plen < 0 || (size_t)plen >= cap - n)
            break;
        memcpy(out + n, part, (size_t)plen);
        n += (size_t)plen;
        out[n] = 0;
    }
}

/* ================================================================== */
/* Names                                                              */
/* ================================================================== */

/* Copy an X.500 DirectoryString into a NUL-terminated ASCII buffer.
 * A value containing an embedded NUL is dropped entirely -- that is the
 * "CN=evil.com\0good.com" trick, and there is no legitimate use for it. */
static void copy_dirstring(const struct der_tlv *t, char *out, size_t cap)
{
    size_t i, n = 0;

    if (cap == 0)
        return;
    if (t->tag == DER_BMPSTRING) {
        if (t->len % 2)
            return;
        for (i = 0; i + 1 < t->len; i += 2) {
            uint16_t ch = (uint16_t)((t->val[i] << 8) | t->val[i + 1]);
            if (ch == 0)
                return;
            if (n + 1 >= cap)
                break;
            out[n++] = (ch < 0x80) ? (char)ch : '?';
        }
        out[n] = 0;
        return;
    }
    if (t->tag != DER_UTF8STRING && t->tag != DER_PRINTABLE &&
        t->tag != DER_IA5STRING && t->tag != DER_T61STRING)
        return;
    for (i = 0; i < t->len; i++) {
        if (t->val[i] == 0)
            return;
        if (n + 1 >= cap)
            break;
        out[n++] = (char)t->val[i];
    }
    out[n] = 0;
}

/* The most specific commonName in a Name, which is the last one. */
static void name_get_cn(const uint8_t *name, size_t len, char *out, size_t cap)
{
    struct der seq, rdn, atv;
    struct der_tlv t;

    if (cap)
        out[0] = 0;
    der_init(&seq, name, len);
    /* the Name TLV itself */
    if (der_read_expect(&seq, &t, DER_SEQUENCE) < 0)
        return;
    der_enter(&t, &seq);
    while (!der_eof(&seq)) {
        struct der_tlv set;
        if (der_read_expect(&seq, &set, DER_SET) < 0)
            return;
        der_enter(&set, &rdn);
        while (!der_eof(&rdn)) {
            struct der_tlv pair, val;
            const uint8_t *oid;
            size_t oidlen;
            if (der_read_expect(&rdn, &pair, DER_SEQUENCE) < 0)
                return;
            der_enter(&pair, &atv);
            if (der_read_oid(&atv, &oid, &oidlen) < 0)
                continue;
            if (der_read(&atv, &val) < 0)
                continue;
            if (val.cls == DER_CLASS_UNIVERSAL && OID_IS(oid, oidlen, oid_cn))
                copy_dirstring(&val, out, cap);
        }
    }
}

/* ================================================================== */
/* Algorithm identifiers                                              */
/* ================================================================== */

struct alg_info {
    int sig_alg;
    int md;
    int mgf_md;
    int salt_len;
};

/* RSASSA-PSS-params ::= SEQUENCE {
 *     hashAlgorithm    [0] AlgorithmIdentifier DEFAULT sha1,
 *     maskGenAlgorithm [1] AlgorithmIdentifier DEFAULT mgf1SHA1,
 *     saltLength       [2] INTEGER DEFAULT 20,
 *     trailerField     [3] INTEGER DEFAULT 1 } */
static int parse_pss_params(const struct der_tlv *params, struct alg_info *out)
{
    struct der d;
    struct der_tlv t;

    out->md = CRYPTO_MD_SHA1;
    out->mgf_md = CRYPTO_MD_SHA1;
    out->salt_len = 20;
    if (!params)
        return 0;
    if (params->cls != DER_CLASS_UNIVERSAL || params->tag != 0x10)
        return -1;
    der_enter(params, &d);
    if (der_read_context(&d, &t, 0, 1) == 0) {
        struct der inner, alg;
        struct der_tlv a;
        const uint8_t *oid;
        size_t oidlen;
        der_enter(&t, &inner);
        if (der_read_expect(&inner, &a, DER_SEQUENCE) < 0)
            return -1;
        der_enter(&a, &alg);
        if (der_read_oid(&alg, &oid, &oidlen) < 0)
            return -1;
        out->md = crypto_md_from_oid(oid, (int)oidlen);
        if (!out->md)
            return -1;
    }
    if (der_read_context(&d, &t, 1, 1) == 0) {
        struct der inner, alg;
        struct der_tlv a;
        const uint8_t *oid;
        size_t oidlen;
        der_enter(&t, &inner);
        if (der_read_expect(&inner, &a, DER_SEQUENCE) < 0)
            return -1;
        der_enter(&a, &alg);
        if (der_read_oid(&alg, &oid, &oidlen) < 0)
            return -1;
        if (!OID_IS(oid, oidlen, oid_mgf1))
            return -1;
        /* MGF1's own parameter is an AlgorithmIdentifier naming the hash,
         * not a bare OID. */
        if (!der_eof(&alg)) {
            struct der_tlv h;
            struct der hd;
            if (der_read_expect(&alg, &h, DER_SEQUENCE) < 0)
                return -1;
            der_enter(&h, &hd);
            if (der_read_oid(&hd, &oid, &oidlen) < 0)
                return -1;
            out->mgf_md = crypto_md_from_oid(oid, (int)oidlen);
            if (!out->mgf_md)
                return -1;
        }
    }
    if (der_read_context(&d, &t, 2, 1) == 0) {
        struct der inner;
        uint32_t sl;
        der_enter(&t, &inner);
        if (der_read_u32(&inner, &sl) < 0 || sl > 512)
            return -1;
        out->salt_len = (int)sl;
    }
    if (der_read_context(&d, &t, 3, 1) == 0) {
        struct der inner;
        uint32_t tf;
        der_enter(&t, &inner);
        if (der_read_u32(&inner, &tf) < 0 || tf != 1)
            return -1;
    }
    return der_eof(&d) ? 0 : -1;
}

/* AlgorithmIdentifier ::= SEQUENCE { algorithm OID, parameters ANY OPT } */
static int parse_sig_alg(const struct der_tlv *seq, struct alg_info *out)
{
    struct der d;
    const uint8_t *oid;
    size_t oidlen;
    struct der_tlv params;
    int have_params = 0;

    out->sig_alg = 0;
    out->md = 0;
    out->mgf_md = 0;
    out->salt_len = -1;
    if (seq->cls != DER_CLASS_UNIVERSAL || seq->tag != 0x10)
        return -1;
    der_enter(seq, &d);
    if (der_read_oid(&d, &oid, &oidlen) < 0)
        return -1;
    if (!der_eof(&d)) {
        if (der_read(&d, &params) < 0)
            return -1;
        have_params = 1;
    }
    if (!der_eof(&d))
        return -1;

    if (OID_IS(oid, oidlen, oid_sha256_rsa)) {
        out->sig_alg = X509_SIG_RSA_PKCS1; out->md = CRYPTO_MD_SHA256;
    } else if (OID_IS(oid, oidlen, oid_sha384_rsa)) {
        out->sig_alg = X509_SIG_RSA_PKCS1; out->md = CRYPTO_MD_SHA384;
    } else if (OID_IS(oid, oidlen, oid_sha512_rsa)) {
        out->sig_alg = X509_SIG_RSA_PKCS1; out->md = CRYPTO_MD_SHA512;
    } else if (OID_IS(oid, oidlen, oid_sha224_rsa)) {
        out->sig_alg = X509_SIG_RSA_PKCS1; out->md = CRYPTO_MD_SHA224;
    } else if (OID_IS(oid, oidlen, oid_sha1_rsa)) {
        out->sig_alg = X509_SIG_RSA_PKCS1; out->md = CRYPTO_MD_SHA1;
    } else if (OID_IS(oid, oidlen, oid_ecdsa_sha256)) {
        out->sig_alg = X509_SIG_ECDSA; out->md = CRYPTO_MD_SHA256;
    } else if (OID_IS(oid, oidlen, oid_ecdsa_sha384)) {
        out->sig_alg = X509_SIG_ECDSA; out->md = CRYPTO_MD_SHA384;
    } else if (OID_IS(oid, oidlen, oid_ecdsa_sha512)) {
        out->sig_alg = X509_SIG_ECDSA; out->md = CRYPTO_MD_SHA512;
    } else if (OID_IS(oid, oidlen, oid_ecdsa_sha224)) {
        out->sig_alg = X509_SIG_ECDSA; out->md = CRYPTO_MD_SHA224;
    } else if (OID_IS(oid, oidlen, oid_ecdsa_sha1)) {
        out->sig_alg = X509_SIG_ECDSA; out->md = CRYPTO_MD_SHA1;
    } else if (OID_IS(oid, oidlen, oid_rsa_pss)) {
        out->sig_alg = X509_SIG_RSA_PSS;
        if (parse_pss_params(have_params ? &params : 0, out) < 0)
            return -1;
        return 0;
    } else {
        return -1;
    }
    /* RFC 8017: the parameters of an RSA PKCS#1 signature algorithm are
     * NULL. ECDSA omits them entirely. */
    if (out->sig_alg == X509_SIG_RSA_PKCS1) {
        if (have_params && !(params.cls == DER_CLASS_UNIVERSAL &&
                             params.tag == 0x05 && params.len == 0))
            return -1;
    } else if (have_params) {
        return -1;
    }
    return 0;
}

/* ================================================================== */
/* Extensions                                                         */
/* ================================================================== */

static int parse_san(struct x509_cert *c, const uint8_t *v, size_t len)
{
    struct der d, seq;
    struct der_tlv t;

    der_init(&d, v, len);
    if (der_read_expect(&d, &t, DER_SEQUENCE) < 0)
        return -1;
    if (!der_eof(&d))
        return -1;
    der_enter(&t, &seq);
    while (!der_eof(&seq)) {
        struct der_tlv gn;
        if (der_read(&seq, &gn) < 0)
            return -1;
        if (gn.cls != DER_CLASS_CONTEXT)
            continue;
        if (gn.tag == 2 && !gn.constructed) {      /* dNSName          */
            size_t i;
            for (i = 0; i < gn.len; i++) {
                if (gn.val[i] == 0)
                    return -1;                     /* embedded NUL     */
            }
            if (gn.len == 0 || gn.len > 255)
                continue;
            if (c->n_san < X509_MAX_SAN) {
                c->san[c->n_san].p = (const char *)gn.val;
                c->san[c->n_san].len = (int)gn.len;
                c->san[c->n_san].is_ip = 0;
                c->n_san++;
            }
        } else if (gn.tag == 7 && !gn.constructed) { /* iPAddress      */
            if (gn.len != 4 && gn.len != 16)
                continue;
            if (c->n_san < X509_MAX_SAN) {
                c->san[c->n_san].p = (const char *)gn.val;
                c->san[c->n_san].len = (int)gn.len;
                c->san[c->n_san].is_ip = 1;
                c->n_san++;
            }
        }
    }
    return 0;
}

static int parse_basic_constraints(struct x509_cert *c, const uint8_t *v,
                                   size_t len)
{
    struct der d, seq;
    struct der_tlv t;
    int ca = 0;
    uint32_t pl;

    der_init(&d, v, len);
    if (der_read_expect(&d, &t, DER_SEQUENCE) < 0)
        return -1;
    if (!der_eof(&d))
        return -1;
    der_enter(&t, &seq);
    c->has_basic_constraints = 1;
    c->path_len = -1;
    if (!der_eof(&seq) && der_read_bool(&seq, &ca) == 0)
        c->is_ca = ca;
    if (!der_eof(&seq)) {
        if (der_read_u32(&seq, &pl) < 0)
            return -1;
        if (pl > 64)
            pl = 64;
        c->path_len = (int)pl;
    }
    return der_eof(&seq) ? 0 : -1;
}

static int parse_eku(struct x509_cert *c, const uint8_t *v, size_t len)
{
    struct der d, seq;
    struct der_tlv t;

    der_init(&d, v, len);
    if (der_read_expect(&d, &t, DER_SEQUENCE) < 0)
        return -1;
    if (!der_eof(&d))
        return -1;
    der_enter(&t, &seq);
    c->has_eku = 1;
    while (!der_eof(&seq)) {
        const uint8_t *oid;
        size_t oidlen;
        if (der_read_oid(&seq, &oid, &oidlen) < 0)
            return -1;
        if (OID_IS(oid, oidlen, oid_eku_server))
            c->eku |= X509_EKU_SERVER_AUTH;
        else if (OID_IS(oid, oidlen, oid_eku_client))
            c->eku |= X509_EKU_CLIENT_AUTH;
        else if (OID_IS(oid, oidlen, oid_eku_code))
            c->eku |= X509_EKU_CODE_SIGN;
        else if (OID_IS(oid, oidlen, oid_eku_email))
            c->eku |= X509_EKU_EMAIL;
        else if (OID_IS(oid, oidlen, oid_eku_stamp))
            c->eku |= X509_EKU_TIME_STAMP;
        else if (OID_IS(oid, oidlen, oid_eku_ocsp))
            c->eku |= X509_EKU_OCSP_SIGN;
        else if (OID_IS(oid, oidlen, oid_eku_any))
            c->eku |= X509_EKU_ANY;
        else
            c->eku |= X509_EKU_UNKNOWN;
    }
    return 0;
}

static int parse_extensions(struct x509_cert *c, const struct der_tlv *exts)
{
    struct der outer, list;
    struct der_tlv t;

    der_enter(exts, &outer);
    if (der_read_expect(&outer, &t, DER_SEQUENCE) < 0)
        return -1;
    if (!der_eof(&outer))
        return -1;
    der_enter(&t, &list);
    while (!der_eof(&list)) {
        struct der ext;
        struct der_tlv e, val;
        const uint8_t *oid;
        size_t oidlen;
        int critical = 0, known = 1;

        if (der_read_expect(&list, &e, DER_SEQUENCE) < 0)
            return -1;
        der_enter(&e, &ext);
        if (der_read_oid(&ext, &oid, &oidlen) < 0)
            return -1;
        if (der_read_bool(&ext, &critical) < 0)
            critical = 0;
        if (der_read_expect(&ext, &val, DER_OCTET_STRING) < 0)
            return -1;
        if (!der_eof(&ext))
            return -1;

        if (OID_IS(oid, oidlen, oid_ext_san)) {
            if (parse_san(c, val.val, val.len) < 0)
                return -1;
        } else if (OID_IS(oid, oidlen, oid_ext_bc)) {
            if (parse_basic_constraints(c, val.val, val.len) < 0)
                return -1;
        } else if (OID_IS(oid, oidlen, oid_ext_ku)) {
            struct der kd;
            uint32_t bits;
            der_init(&kd, val.val, val.len);
            if (der_read_bitflags(&kd, &bits) < 0 || !der_eof(&kd))
                return -1;
            c->has_key_usage = 1;
            c->key_usage = bits;
        } else if (OID_IS(oid, oidlen, oid_ext_eku)) {
            if (parse_eku(c, val.val, val.len) < 0)
                return -1;
        } else if (OID_IS(oid, oidlen, oid_ext_skid) ||
                   OID_IS(oid, oidlen, oid_ext_akid) ||
                   OID_IS(oid, oidlen, oid_ext_cp)) {
            /* recognised and deliberately not acted on */
        } else {
            known = 0;
        }
        if (!known && critical && !c->unknown_critical) {
            c->unknown_critical = 1;
            oid_to_text(oid, oidlen, c->unknown_critical_oid,
                        sizeof(c->unknown_critical_oid));
        }
    }
    return 0;
}

/* ================================================================== */
/* Certificate parsing                                                */
/* ================================================================== */

#define PARSE_FAIL(msg)                                                    \
    do {                                                                   \
        if (err && err_len)                                                \
            snprintf(err, err_len, "%s", (msg));                           \
        return -1;                                                         \
    } while (0)

int x509_parse(struct x509_cert *c, const uint8_t *der, size_t len,
               char *err, size_t err_len)
{
    struct der top, cert, tbs;
    struct der_tlv t, tbs_tlv, sigalg_outer, sigalg_inner, spki, name;
    struct alg_info outer_alg, inner_alg;
    const uint8_t *sigbits;
    size_t sigbits_len;

    if (err && err_len)
        err[0] = 0;
    if (!c || !der || len < 8)
        PARSE_FAIL("certificate is too short to be DER");
    memset(c, 0, sizeof(*c));
    c->path_len = -1;
    c->der = der;
    c->der_len = len;

    if (!der_structure_ok(der, len))
        PARSE_FAIL("certificate is not well-formed DER");

    der_init(&top, der, len);
    if (der_read_expect(&top, &t, DER_SEQUENCE) < 0)
        PARSE_FAIL("certificate is not a SEQUENCE");
    if (!der_eof(&top))
        PARSE_FAIL("trailing bytes after the certificate");
    der_enter(&t, &cert);

    if (der_read_expect(&cert, &tbs_tlv, DER_SEQUENCE) < 0)
        PARSE_FAIL("tbsCertificate is not a SEQUENCE");
    c->tbs = tbs_tlv.hdr;
    c->tbs_len = tbs_tlv.hdr_len + tbs_tlv.len;

    if (der_read_expect(&cert, &sigalg_outer, DER_SEQUENCE) < 0)
        PARSE_FAIL("signatureAlgorithm is not a SEQUENCE");
    if (der_read_bitstring(&cert, &sigbits, &sigbits_len) < 0)
        PARSE_FAIL("signatureValue is not a whole-octet BIT STRING");
    if (!der_eof(&cert))
        PARSE_FAIL("trailing bytes inside the certificate");
    if (sigbits_len == 0 || sigbits_len > 2048)
        PARSE_FAIL("signature value has an implausible length");
    c->sig = sigbits;
    c->sig_len = sigbits_len;

    /* ---- the to-be-signed body ---- */
    der_enter(&tbs_tlv, &tbs);

    c->version = 1;
    if (der_read_context(&tbs, &t, 0, 1) == 0) {
        struct der v;
        uint32_t ver;
        der_enter(&t, &v);
        if (der_read_u32(&v, &ver) < 0 || !der_eof(&v))
            PARSE_FAIL("malformed version");
        if (ver > 2)
            PARSE_FAIL("unknown certificate version");
        c->version = (int)ver + 1;
        if (ver == 0)
            PARSE_FAIL("version 1 must not be encoded explicitly");
    }

    if (der_read_uint(&tbs, &c->serial, &c->serial_len) < 0) {
        /* Some real certificates carry a negative serial. Accept the
         * INTEGER as raw bytes rather than rejecting the chain. */
        struct der_tlv s;
        if (der_read_expect(&tbs, &s, DER_INTEGER) < 0 || s.len == 0)
            PARSE_FAIL("malformed serial number");
        c->serial = s.val;
        c->serial_len = s.len;
    }
    if (c->serial_len > 24)
        PARSE_FAIL("serial number is longer than 20 octets");

    if (der_read_expect(&tbs, &sigalg_inner, DER_SEQUENCE) < 0)
        PARSE_FAIL("tbsCertificate signature field is not a SEQUENCE");
    if (parse_sig_alg(&sigalg_outer, &outer_alg) < 0)
        PARSE_FAIL("unsupported or malformed signature algorithm");
    if (parse_sig_alg(&sigalg_inner, &inner_alg) < 0)
        PARSE_FAIL("unsupported or malformed inner signature algorithm");
    /* RFC 5280 4.1.1.2: the two must be identical, or a signer could be
     * tricked about what it signed. */
    if (sigalg_outer.hdr_len + sigalg_outer.len !=
            sigalg_inner.hdr_len + sigalg_inner.len ||
        memcmp(sigalg_outer.hdr, sigalg_inner.hdr,
               sigalg_outer.hdr_len + sigalg_outer.len) != 0)
        PARSE_FAIL("signature algorithm differs between tbsCertificate "
                   "and the outer signature");
    c->sig_alg = outer_alg.sig_alg;
    c->sig_md = outer_alg.md;
    c->sig_mgf_md = outer_alg.mgf_md;
    c->sig_salt_len = outer_alg.salt_len;

    if (der_read_expect(&tbs, &name, DER_SEQUENCE) < 0)
        PARSE_FAIL("issuer is not a Name");
    c->issuer_raw = name.hdr;
    c->issuer_raw_len = name.hdr_len + name.len;
    name_get_cn(c->issuer_raw, c->issuer_raw_len, c->issuer_cn,
                sizeof(c->issuer_cn));

    if (der_read_expect(&tbs, &t, DER_SEQUENCE) < 0)
        PARSE_FAIL("validity is not a SEQUENCE");
    {
        struct der v;
        der_enter(&t, &v);
        if (der_read_time(&v, &c->not_before) < 0)
            PARSE_FAIL("malformed notBefore time");
        if (der_read_time(&v, &c->not_after) < 0)
            PARSE_FAIL("malformed notAfter time");
        if (!der_eof(&v))
            PARSE_FAIL("trailing bytes in validity");
    }
    if (c->not_after < c->not_before)
        PARSE_FAIL("validity period ends before it starts");

    if (der_read_expect(&tbs, &name, DER_SEQUENCE) < 0)
        PARSE_FAIL("subject is not a Name");
    c->subject_raw = name.hdr;
    c->subject_raw_len = name.hdr_len + name.len;
    name_get_cn(c->subject_raw, c->subject_raw_len, c->subject_cn,
                sizeof(c->subject_cn));
    c->self_signed = (c->issuer_raw_len == c->subject_raw_len) &&
                     memcmp(c->issuer_raw, c->subject_raw,
                            c->issuer_raw_len) == 0;

    if (der_read_expect(&tbs, &spki, DER_SEQUENCE) < 0)
        PARSE_FAIL("subjectPublicKeyInfo is not a SEQUENCE");
    {
        struct der s;
        struct der_tlv alg;
        struct der a;
        const uint8_t *oid;
        size_t oidlen;

        der_enter(&spki, &s);
        if (der_read_expect(&s, &alg, DER_SEQUENCE) < 0)
            PARSE_FAIL("public key algorithm is not a SEQUENCE");
        der_enter(&alg, &a);
        if (der_read_oid(&a, &oid, &oidlen) < 0)
            PARSE_FAIL("malformed public key algorithm OID");
        if (OID_IS(oid, oidlen, oid_rsa_enc) ||
            OID_IS(oid, oidlen, oid_rsa_pss)) {
            c->pk_alg = X509_PK_RSA;
        } else if (OID_IS(oid, oidlen, oid_ec_pubkey)) {
            const uint8_t *curve;
            size_t curvelen;
            c->pk_alg = X509_PK_EC;
            if (der_read_oid(&a, &curve, &curvelen) < 0)
                PARSE_FAIL("EC public key has no named curve");
            if (OID_IS(curve, curvelen, oid_p256))
                c->pk_curve = X509_CURVE_P256;
            else if (OID_IS(curve, curvelen, oid_p384))
                c->pk_curve = X509_CURVE_P384;
            else if (OID_IS(curve, curvelen, oid_p521))
                c->pk_curve = X509_CURVE_P521;
            else
                PARSE_FAIL("unsupported elliptic curve");
        } else {
            PARSE_FAIL("unsupported public key algorithm");
        }
        if (der_read_bitstring(&s, &c->pk_bits, &c->pk_bits_len) < 0)
            PARSE_FAIL("subjectPublicKey is not a whole-octet BIT STRING");
        if (!der_eof(&s))
            PARSE_FAIL("trailing bytes in subjectPublicKeyInfo");
        if (c->pk_bits_len == 0 || c->pk_bits_len > 2048)
            PARSE_FAIL("public key has an implausible length");
    }

    /* issuerUniqueID / subjectUniqueID, both obsolete but legal */
    if (der_read_context(&tbs, &t, 1, 0) == 0) { }
    if (der_read_context(&tbs, &t, 2, 0) == 0) { }

    if (der_read_context(&tbs, &t, 3, 1) == 0) {
        if (c->version != 3)
            PARSE_FAIL("extensions present in a non-v3 certificate");
        if (parse_extensions(c, &t) < 0)
            PARSE_FAIL("malformed certificate extension");
    }
    if (!der_eof(&tbs))
        PARSE_FAIL("trailing bytes in tbsCertificate");
    return 0;
}

/* ================================================================== */
/* Trust store                                                        */
/* ================================================================== */

void x509_store_init(struct x509_store *s)
{
    memset(s, 0, sizeof(*s));
}

int x509_store_add_der(struct x509_store *s, const uint8_t *der, size_t len)
{
    uint8_t *copy;

    if (!s || s->n >= X509_STORE_MAX)
        return -1;
    if (len == 0 || len > X509_MAX_DER)
        return -1;
    copy = (uint8_t *)malloc(len);
    if (!copy)
        return -1;
    memcpy(copy, der, len);
    if (x509_parse(&s->certs[s->n], copy, len, 0, 0) < 0) {
        free(copy);
        return -1;
    }
    s->owned[s->n] = copy;
    s->n++;
    return 0;
}

int x509_store_add_pem(struct x509_store *s, const char *text, size_t len)
{
    struct pem_iter it;
    uint8_t *buf;
    int added = 0, rc;

    if (!s || !text)
        return -1;
    buf = (uint8_t *)malloc(X509_MAX_DER);
    if (!buf)
        return -1;
    pem_iter_init(&it, text, len);
    for (;;) {
        char label[80];
        size_t dlen = 0;
        rc = pem_next(&it, label, sizeof(label), buf, X509_MAX_DER, &dlen);
        if (rc == 0)
            break;
        if (rc < 0) {
            free(buf);
            return -1;
        }
        if (strcmp(label, "CERTIFICATE") != 0 &&
            strcmp(label, "TRUSTED CERTIFICATE") != 0)
            continue;
        if (x509_store_add_der(s, buf, dlen) < 0) {
            free(buf);
            return -1;
        }
        added++;
    }
    free(buf);
    return added;
}

void x509_store_free(struct x509_store *s)
{
    int i;

    for (i = 0; i < s->n; i++) {
        free(s->owned[i]);
        s->owned[i] = 0;
    }
    s->n = 0;
}

/* ================================================================== */
/* Signature verification                                             */
/* ================================================================== */

#define VFAIL(...)                                                         \
    do {                                                                   \
        if (err && err_len)                                                \
            snprintf(err, err_len, __VA_ARGS__);                           \
        return 0;                                                          \
    } while (0)

int x509_verify_signature(const struct x509_cert *cert,
                          const struct x509_cert *issuer,
                          char *err, size_t err_len)
{
    const struct crypto_md *md;
    uint8_t digest[CRYPTO_MD_MAX_LEN];

    if (err && err_len)
        err[0] = 0;
    if (!cert || !issuer)
        VFAIL("internal error: missing certificate");
    md = crypto_md_get(cert->sig_md);
    if (!md)
        VFAIL("no implementation registered for the digest '%s' used to "
              "sign '%s'", cert->sig_md ? "(known id)" : "(unknown)",
              cert->subject_cn);
    md->hash(cert->tbs, cert->tbs_len, digest);

    if (cert->sig_alg == X509_SIG_RSA_PKCS1 ||
        cert->sig_alg == X509_SIG_RSA_PSS) {
        struct rsa_pubkey key;
        int ok;
        if (issuer->pk_alg != X509_PK_RSA)
            VFAIL("'%s' has an RSA signature but '%s' holds a non-RSA key",
                  cert->subject_cn, issuer->subject_cn);
        if (rsa_pubkey_from_der(&key, issuer->pk_bits, issuer->pk_bits_len) < 0)
            VFAIL("the RSA public key in '%s' is malformed or too small",
                  issuer->subject_cn);
        if (key.bits < X509_MIN_RSA_BITS)
            VFAIL("the RSA key in '%s' is %d bits, below the %d bit minimum",
                  issuer->subject_cn, key.bits, X509_MIN_RSA_BITS);
        if (cert->sig_alg == X509_SIG_RSA_PKCS1)
            ok = rsa_verify_pkcs1_v15(&key, cert->sig_md, digest,
                                      (size_t)md->digest_len,
                                      cert->sig, cert->sig_len);
        else
            ok = rsa_verify_pss(&key, cert->sig_md, cert->sig_mgf_md,
                                cert->sig_salt_len, digest,
                                (size_t)md->digest_len,
                                cert->sig, cert->sig_len);
        if (!ok)
            VFAIL("the signature on '%s' does not verify against '%s'",
                  cert->subject_cn, issuer->subject_cn);
        return 1;
    }

    if (cert->sig_alg == X509_SIG_ECDSA) {
        struct p256_point pub;
        struct der d, seq;
        struct der_tlv t;
        const uint8_t *r, *s;
        size_t rlen, slen;

        if (issuer->pk_alg != X509_PK_EC)
            VFAIL("'%s' has an ECDSA signature but '%s' holds a non-EC key",
                  cert->subject_cn, issuer->subject_cn);
        if (issuer->pk_curve != X509_CURVE_P256)
            VFAIL("'%s' uses curve %s, which this build does not implement",
                  issuer->subject_cn,
                  issuer->pk_curve == X509_CURVE_P384 ? "P-384" : "P-521");
        if (p256_point_from_bytes(&pub, issuer->pk_bits,
                                  issuer->pk_bits_len) < 0)
            VFAIL("the P-256 public key in '%s' is not a valid curve point",
                  issuer->subject_cn);
        der_init(&d, cert->sig, cert->sig_len);
        if (der_read_expect(&d, &t, DER_SEQUENCE) < 0 || !der_eof(&d))
            VFAIL("the ECDSA signature on '%s' is not a DER SEQUENCE",
                  cert->subject_cn);
        der_enter(&t, &seq);
        if (der_read_uint(&seq, &r, &rlen) < 0 ||
            der_read_uint(&seq, &s, &slen) < 0 || !der_eof(&seq))
            VFAIL("the ECDSA signature on '%s' is malformed",
                  cert->subject_cn);
        if (!p256_ecdsa_verify(&pub, digest, (size_t)md->digest_len,
                               r, rlen, s, slen))
            VFAIL("the signature on '%s' does not verify against '%s'",
                  cert->subject_cn, issuer->subject_cn);
        return 1;
    }

    VFAIL("'%s' is signed with an algorithm this build does not support",
          cert->subject_cn);
}

/* ================================================================== */
/* Hostname matching                                                  */
/* ================================================================== */

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int ci_eq(const char *a, size_t alen, const char *b, size_t blen)
{
    size_t i;

    if (alen != blen)
        return 0;
    for (i = 0; i < alen; i++) {
        if (lower((unsigned char)a[i]) != lower((unsigned char)b[i]))
            return 0;
    }
    return 1;
}

/* Parse a dotted quad. Returns 1 and fills ip when the whole string is
 * one, which is how an IP literal is told apart from a hostname. */
static int parse_ipv4(const char *s, size_t len, uint8_t ip[4])
{
    size_t i = 0;
    int part;

    for (part = 0; part < 4; part++) {
        int v = 0, digits = 0;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            v = v * 10 + (s[i] - '0');
            if (v > 255 || ++digits > 3)
                return 0;
            i++;
        }
        if (digits == 0)
            return 0;
        ip[part] = (uint8_t)v;
        if (part < 3) {
            if (i >= len || s[i] != '.')
                return 0;
            i++;
        }
    }
    return i == len;
}

/* A wildcard is only ever the whole leftmost label, it never matches a
 * dot, and it must leave at least two labels behind it -- so *.co.uk is
 * usable but *.uk is not, and a.b.example.com is not matched by
 * *.example.com. */
static int wildcard_match(const char *pat, size_t plen, const char *host,
                          size_t hlen)
{
    const char *prest, *hrest;
    size_t prest_len, hrest_len;
    size_t i, dots = 0;

    if (plen < 3 || pat[0] != '*' || pat[1] != '.')
        return 0;
    prest = pat + 2;
    prest_len = plen - 2;
    for (i = 0; i < prest_len; i++) {
        if (prest[i] == '*')
            return 0;                      /* only one wildcard, at the front */
        if (prest[i] == '.')
            dots++;
    }
    if (dots < 1)
        return 0;                          /* *.com and friends */
    hrest = 0;
    hrest_len = 0;
    for (i = 0; i < hlen; i++) {
        if (host[i] == '.') {
            hrest = host + i + 1;
            hrest_len = hlen - i - 1;
            break;
        }
    }
    if (!hrest || i == 0)
        return 0;                          /* no leftmost label to consume */
    return ci_eq(prest, prest_len, hrest, hrest_len);
}

int x509_check_hostname(const struct x509_cert *cert, const char *host)
{
    size_t hlen;
    uint8_t ip[4];
    int i, is_ip;

    if (!cert || !host)
        return 0;
    hlen = strlen(host);
    if (hlen == 0 || hlen > 255)
        return 0;
    if (host[hlen - 1] == '.')
        hlen--;                            /* an absolute name is the same name */
    if (hlen == 0)
        return 0;
    /* The name being looked up is never a pattern. Without this, a request
     * for "*.example.com" would match the wildcard SAN of the same shape. */
    for (i = 0; i < (int)hlen; i++) {
        unsigned char ch = (unsigned char)host[i];
        if (ch <= 0x20 || ch >= 0x7f || ch == '*')
            return 0;
    }
    is_ip = parse_ipv4(host, hlen, ip);

    for (i = 0; i < cert->n_san; i++) {
        const struct x509_san *s = &cert->san[i];
        if (is_ip) {
            if (s->is_ip && s->len == 4 && memcmp(s->p, ip, 4) == 0)
                return 1;
            continue;
        }
        if (s->is_ip)
            continue;
        if (s->len > 2 && s->p[0] == '*') {
            if (wildcard_match(s->p, (size_t)s->len, host, hlen))
                return 1;
            continue;
        }
        if (ci_eq(s->p, (size_t)s->len, host, hlen))
            return 1;
    }
    /* Only fall back to the subject CN when there is no SAN at all;
     * RFC 6125 deprecated the CN and a certificate that has a SAN has
     * already said everything it means to say. */
    if (cert->n_san == 0 && cert->subject_cn[0]) {
        size_t clen = strlen(cert->subject_cn);
        if (is_ip)
            return 0;
        if (clen > 2 && cert->subject_cn[0] == '*')
            return wildcard_match(cert->subject_cn, clen, host, hlen);
        return ci_eq(cert->subject_cn, clen, host, hlen);
    }
    return 0;
}

/* ================================================================== */
/* Chain verification                                                 */
/* ================================================================== */

static int names_equal(const struct x509_cert *a, const struct x509_cert *b)
{
    /* Byte comparison of the encoded Name. RFC 5280 describes a
     * normalising comparison; refusing to match anything that is not
     * byte-identical is strictly the safer direction, and is what
     * certificates issued in the last twenty years assume. */
    return a->issuer_raw_len == b->subject_raw_len &&
           memcmp(a->issuer_raw, b->subject_raw, a->issuer_raw_len) == 0;
}

static const char *cn_or(const struct x509_cert *c)
{
    return c->subject_cn[0] ? c->subject_cn : "(no common name)";
}

#define CFAIL(...)                                                         \
    do {                                                                   \
        if (err && errlen > 0)                                             \
            snprintf(err, (size_t)errlen, __VA_ARGS__);                    \
        return 0;                                                          \
    } while (0)

int x509_verify_chain(const struct x509_cert *chain, int n,
                      const struct x509_store *roots, const char *hostname,
                      uint32_t now_unix, char *err, int errlen)
{
    char d1[16], d2[16];
    int i, j;

    if (err && errlen > 0)
        err[0] = 0;
    if (!chain || n <= 0)
        CFAIL("the server sent an empty certificate chain");
    if (n > X509_MAX_CHAIN)
        CFAIL("the certificate chain is %d long, more than the %d this "
              "build accepts", n, X509_MAX_CHAIN);
    if (!roots || roots->n == 0)
        CFAIL("no trusted root certificates are loaded");

    for (i = 0; i < n; i++) {
        if (chain[i].unknown_critical)
            CFAIL("'%s' has a critical extension this build does not "
                  "understand (%s), so it cannot be trusted",
                  cn_or(&chain[i]), chain[i].unknown_critical_oid);
        if (now_unix < chain[i].not_before) {
            x509_format_date(chain[i].not_before, d1, sizeof(d1));
            x509_format_date(now_unix, d2, sizeof(d2));
            CFAIL("'%s' is not valid until %s (today is %s)",
                  cn_or(&chain[i]), d1, d2);
        }
        if (now_unix > chain[i].not_after) {
            x509_format_date(chain[i].not_after, d1, sizeof(d1));
            x509_format_date(now_unix, d2, sizeof(d2));
            CFAIL("'%s' expired on %s (today is %s)",
                  cn_or(&chain[i]), d1, d2);
        }
    }

    if (hostname && !x509_check_hostname(&chain[0], hostname)) {
        if (chain[0].n_san > 0)
            CFAIL("the certificate is for '%.*s'%s, not '%s'",
                  chain[0].san[0].len, chain[0].san[0].p,
                  chain[0].n_san > 1 ? " (among others)" : "", hostname);
        CFAIL("the certificate is for '%s', not '%s'",
              cn_or(&chain[0]), hostname);
    }
    if (chain[0].has_eku &&
        !(chain[0].eku & (X509_EKU_SERVER_AUTH | X509_EKU_ANY)))
        CFAIL("'%s' is not permitted to authenticate a TLS server "
              "(extKeyUsage)", cn_or(&chain[0]));
    if (chain[0].has_key_usage &&
        !(chain[0].key_usage & (X509_KU_DIGITAL_SIGNATURE |
                                X509_KU_KEY_ENCIPHERMENT |
                                X509_KU_KEY_AGREEMENT)))
        CFAIL("'%s' has a keyUsage that permits neither signing nor key "
              "exchange", cn_or(&chain[0]));

    for (i = 1; i < n; i++) {
        const struct x509_cert *ca = &chain[i];
        if (!ca->has_basic_constraints || !ca->is_ca)
            CFAIL("'%s' is used as an intermediate but is not marked as a "
                  "CA in basicConstraints", cn_or(ca));
        if (ca->has_key_usage && !(ca->key_usage & X509_KU_KEY_CERT_SIGN))
            CFAIL("'%s' is a CA but its keyUsage does not allow signing "
                  "certificates", cn_or(ca));
        if (ca->path_len >= 0 && (i - 1) > ca->path_len)
            CFAIL("'%s' allows at most %d intermediate%s below it, but the "
                  "chain has %d", cn_or(ca), ca->path_len,
                  ca->path_len == 1 ? "" : "s", i - 1);
        if (!names_equal(&chain[i - 1], ca))
            CFAIL("'%s' names its issuer as '%s', which is not the subject "
                  "of the next certificate ('%s')", cn_or(&chain[i - 1]),
                  chain[i - 1].issuer_cn[0] ? chain[i - 1].issuer_cn
                                            : "(no common name)",
                  cn_or(ca));
        if (!x509_verify_signature(&chain[i - 1], ca, err, (size_t)errlen))
            return 0;
    }

    /* Anchor the top of the chain in the trust store. */
    for (j = 0; j < roots->n; j++) {
        const struct x509_cert *root = &roots->certs[j];
        char scratch[X509_ERR_LEN];

        if (!names_equal(&chain[n - 1], root))
            continue;
        if (root->has_basic_constraints && !root->is_ca)
            continue;
        if (now_unix < root->not_before || now_unix > root->not_after)
            continue;
        if (root->has_key_usage && !(root->key_usage & X509_KU_KEY_CERT_SIGN))
            continue;
        if (x509_verify_signature(&chain[n - 1], root, scratch,
                                  sizeof(scratch)))
            return 1;
    }

    CFAIL("no trusted root certificate issued '%s' (it names its issuer as "
          "'%s')", cn_or(&chain[n - 1]),
          chain[n - 1].issuer_cn[0] ? chain[n - 1].issuer_cn
                                    : "(no common name)");
}
