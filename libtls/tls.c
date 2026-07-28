/* KestrelOS libtls: the TLS 1.3 client.
 *
 * Written from RFC 8446, with the record layer of section 5, the key
 * schedule of section 7.1 and the handshake of section 4. It sits on
 * hash.h (SHA-2, HMAC, HKDF and the TLS 1.3 label helpers), aead.h (the
 * three AEADs), ecc.h (X25519 and P-256), rsa.h and x509.h.
 *
 * Two builds come out of this one file:
 *   - the target build talks to the kernel's TCP stack through
 *     SYS_TCP_CONNECT / SEND / RECV / CLOSE;
 *   - -DTLS_HOST swaps those for BSD sockets so the whole client can be
 *     driven against openssl s_server and against real web servers from
 *     Linux, which is how it was actually tested. libweb's HTTP client
 *     was validated the same way; see the notes at the top of
 *     tools/test_http.c.
 * Nothing else differs between the two: the record layer, the handshake
 * and every parser are the same code in both.
 *
 * Hostile input: everything after the ClientHello is chosen by the peer.
 * Every length is checked against what is left in the buffer before it is
 * used, every list is bounded, no parser recurses, and a single reassembled
 * handshake message is capped at TLS_HS_MAX. The record and handshake
 * parsers are fuzzed with mutated server flights under
 * -fsanitize=address,undefined in tools/test_tls.c.
 */

#include "tls.h"
#include "hash.h"
#include "aead.h"
#include "ecc.h"
#include "rsa.h"
#include "x509.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#ifdef TLS_HOST
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
static uint32_t tls_now_unix(void) { return (uint32_t)time(0); }
#else
#include <kestrel.h>
static uint32_t tls_now_unix(void)
{
    return (uint32_t)syscall(SYS_TIME, 0, 0, 0, 0);
}
#endif

/* ===================================================================== *
 * Constants                                                             *
 * ===================================================================== */

#define REC_HDR         5
#define REC_MAX_PLAIN   16384              /* 2^14, the record size limit  */
#define REC_MAX_CIPHER  (REC_MAX_PLAIN + 256)
#define RX_CAP          (REC_HDR + REC_MAX_CIPHER)
#define TX_CAP          (REC_HDR + REC_MAX_CIPHER)
#define PT_CAP          REC_MAX_CIPHER     /* inner plaintext ceiling      */

/* The largest handshake message that will be reassembled. This is the
 * Certificate message in practice: ten certificates of 3 KiB each still
 * fit, and real chains are under 10 KiB. It is a cap on a peer-controlled
 * allocation, which is the reason it exists at all. */
#define TLS_HS_MAX      32768
/* ...plus room for the record that carried the last of it. */
#define HS_CAP          (TLS_HS_MAX + REC_MAX_PLAIN + 8)
#define SCRATCH_CAP     2048               /* messages we build ourselves  */
#define COOKIE_MAX      1024

#define MAX_CCS         8                  /* dummy CCS records tolerated  */
#define MAX_SKIPPED     32                 /* ignorable messages in a row  */

/* Record content types. */
#define CT_CCS          20
#define CT_ALERT        21
#define CT_HANDSHAKE    22
#define CT_APPDATA      23

/* Handshake types. */
#define HS_CLIENT_HELLO   1
#define HS_SERVER_HELLO   2
#define HS_NEW_TICKET     4
#define HS_END_EARLY_DATA 5
#define HS_ENCRYPTED_EXT  8
#define HS_CERTIFICATE    11
#define HS_CERT_REQUEST   13
#define HS_CERT_VERIFY    15
#define HS_FINISHED       20
#define HS_KEY_UPDATE     24
#define HS_MESSAGE_HASH   254

/* Extensions. */
#define EXT_SERVER_NAME        0
#define EXT_STATUS_REQUEST     5
#define EXT_SUPPORTED_GROUPS   10
#define EXT_SIG_ALGS           13
#define EXT_ALPN               16
#define EXT_SCT                18
#define EXT_PRE_SHARED_KEY     41
#define EXT_EARLY_DATA         42
#define EXT_SUPPORTED_VERSIONS 43
#define EXT_COOKIE             44
#define EXT_PSK_KEX_MODES      45
#define EXT_KEY_SHARE          51

/* Alerts. */
#define AL_WARNING 1
#define AL_FATAL   2
#define A_CLOSE_NOTIFY        0
#define A_UNEXPECTED_MESSAGE  10
#define A_BAD_RECORD_MAC      20
#define A_RECORD_OVERFLOW     22
#define A_HANDSHAKE_FAILURE   40
#define A_BAD_CERTIFICATE     42
#define A_UNSUPPORTED_CERT    43
#define A_CERTIFICATE_REVOKED 44
#define A_CERTIFICATE_EXPIRED 45
#define A_CERTIFICATE_UNKNOWN 46
#define A_ILLEGAL_PARAMETER   47
#define A_UNKNOWN_CA          48
#define A_ACCESS_DENIED       49
#define A_DECODE_ERROR        50
#define A_DECRYPT_ERROR       51
#define A_PROTOCOL_VERSION    70
#define A_INSUFFICIENT_SEC    71
#define A_INTERNAL_ERROR      80
#define A_INAPPROPRIATE_FB    86
#define A_USER_CANCELED       90
#define A_MISSING_EXTENSION   109
#define A_UNSUPPORTED_EXT     110
#define A_UNRECOGNIZED_NAME   112
#define A_BAD_CERT_STATUS     113
#define A_UNKNOWN_PSK         115
#define A_CERTIFICATE_REQUIRED 116
#define A_NO_APPLICATION_PROTOCOL 120

/* Named groups. */
#define GRP_X25519 0x001d
#define GRP_P256   0x0017

/* Cipher suites. */
#define CS_AES128GCM_SHA256   0x1301
#define CS_AES256GCM_SHA384   0x1302
#define CS_CHACHA20_SHA256    0x1303

/* Signature schemes. */
#define SIG_RSA_PKCS1_SHA256 0x0401
#define SIG_RSA_PKCS1_SHA384 0x0501
#define SIG_RSA_PKCS1_SHA512 0x0601
#define SIG_ECDSA_P256_SHA256 0x0403
#define SIG_ECDSA_P384_SHA384 0x0503
#define SIG_ECDSA_P521_SHA512 0x0603
#define SIG_RSA_PSS_RSAE_SHA256 0x0804
#define SIG_RSA_PSS_RSAE_SHA384 0x0805
#define SIG_RSA_PSS_RSAE_SHA512 0x0806
#define SIG_ED25519 0x0807
#define SIG_ED448   0x0808
#define SIG_RSA_PSS_PSS_SHA256 0x0809
#define SIG_RSA_PSS_PSS_SHA384 0x080a
#define SIG_RSA_PSS_PSS_SHA512 0x080b

/* RFC 8446 4.1.3: the ServerHello.random value that means "this is really
 * a HelloRetryRequest" -- SHA-256 of "HelloRetryRequest". */
static const uint8_t hrr_magic[32] = {
    0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11,
    0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
    0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E,
    0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C
};

/* ===================================================================== *
 * Small utilities                                                       *
 * ===================================================================== */

static void wipe(void *p, unsigned long n)
{
    volatile uint8_t *v = p;

    while (n--)
        *v++ = 0;
}

static int ct_eq(const uint8_t *a, const uint8_t *b, unsigned long n)
{
    unsigned d = 0;
    unsigned long i;

    for (i = 0; i < n; i++)
        d |= (unsigned)(a[i] ^ b[i]);
    return d == 0;
}

/* --- bounded writer --- */

struct wbuf {
    uint8_t *p;
    uint8_t *end;
    int      ovf;
};

static void wb_init(struct wbuf *w, uint8_t *buf, unsigned long cap)
{
    w->p = buf;
    w->end = buf + cap;
    w->ovf = 0;
}

static void w8(struct wbuf *w, unsigned v)
{
    if (w->p < w->end)
        *w->p++ = (uint8_t)v;
    else
        w->ovf = 1;
}

static void w16(struct wbuf *w, unsigned v)
{
    w8(w, v >> 8);
    w8(w, v);
}

static void w24(struct wbuf *w, unsigned long v)
{
    w8(w, (unsigned)(v >> 16));
    w8(w, (unsigned)(v >> 8));
    w8(w, (unsigned)v);
}

static void wbytes(struct wbuf *w, const void *d, unsigned long n)
{
    const uint8_t *s = d;
    unsigned long i;

    for (i = 0; i < n; i++)
        w8(w, s[i]);
}

/* Reserve a 16-bit length that is filled in later. */
static uint8_t *w_open16(struct wbuf *w)
{
    uint8_t *at = w->p;

    w16(w, 0);
    return at;
}

static void w_close16(struct wbuf *w, uint8_t *at)
{
    unsigned long n;

    if (w->ovf || at + 2 > w->end)
        return;
    n = (unsigned long)(w->p - (at + 2));
    at[0] = (uint8_t)(n >> 8);
    at[1] = (uint8_t)n;
}

static uint8_t *w_open8(struct wbuf *w)
{
    uint8_t *at = w->p;

    w8(w, 0);
    return at;
}

static void w_close8(struct wbuf *w, uint8_t *at)
{
    unsigned long n;

    if (w->ovf || at + 1 > w->end)
        return;
    n = (unsigned long)(w->p - (at + 1));
    at[0] = (uint8_t)n;
}

/* --- bounded reader --- */

struct rbuf {
    const uint8_t *p;
    const uint8_t *end;
    int            err;
};

static void rb_init(struct rbuf *r, const uint8_t *buf, unsigned long len)
{
    r->p = buf;
    r->end = buf + len;
    r->err = 0;
}

static unsigned long rb_left(const struct rbuf *r)
{
    return (unsigned long)(r->end - r->p);
}

static unsigned r8(struct rbuf *r)
{
    if (r->p >= r->end) {
        r->err = 1;
        return 0;
    }
    return *r->p++;
}

static unsigned r16(struct rbuf *r)
{
    unsigned hi = r8(r);

    return (hi << 8) | r8(r);
}

static unsigned long r24(struct rbuf *r)
{
    unsigned long v = r8(r);

    v = (v << 8) | r8(r);
    return (v << 8) | r8(r);
}

/* n bytes, or NULL (and err set) if they are not there. */
static const uint8_t *rbytes(struct rbuf *r, unsigned long n)
{
    const uint8_t *at = r->p;

    if (r->err || n > rb_left(r)) {
        r->err = 1;
        return 0;
    }
    r->p += n;
    return at;
}

/* A vector with a 1- or 2-byte length prefix, into out and outlen. */
static int rvec8(struct rbuf *r, const uint8_t **out, unsigned long *outlen)
{
    unsigned long n = r8(r);
    const uint8_t *p = rbytes(r, n);

    if (r->err)
        return -1;
    *out = p;
    *outlen = n;
    return 0;
}

static int rvec16(struct rbuf *r, const uint8_t **out, unsigned long *outlen)
{
    unsigned long n = r16(r);
    const uint8_t *p = rbytes(r, n);

    if (r->err)
        return -1;
    *out = p;
    *outlen = n;
    return 0;
}

/* ===================================================================== *
 * Entropy and the DRBG                                                  *
 *                                                                       *
 * HMAC-DRBG (NIST SP 800-90A) over SHA-256. On KestrelOS it is seeded    *
 * through getrandom(), whose kernel side is a SHA-256 mixing pool fed by  *
 * RDSEED/RDRAND and device timing, followed by a ChaCha20 CSPRNG. The     *
 * host build uses /dev/urandom plus the local timing inputs below.        *
 * ===================================================================== */

static uint8_t g_drbg_k[32];
static uint8_t g_drbg_v[32];
static int g_drbg_seeded;
static int g_weak_entropy = 1;

static void drbg_update(const uint8_t *provided, unsigned long len)
{
    struct hmac_ctx h;
    uint8_t sep;

    sep = 0x00;
    hmac_init(&h, HASH_SHA256, g_drbg_k, 32);
    hmac_update(&h, g_drbg_v, 32);
    hmac_update(&h, &sep, 1);
    if (len)
        hmac_update(&h, provided, len);
    hmac_final(&h, g_drbg_k);
    hmac(HASH_SHA256, g_drbg_k, 32, g_drbg_v, 32, g_drbg_v);
    if (len == 0)
        return;
    sep = 0x01;
    hmac_init(&h, HASH_SHA256, g_drbg_k, 32);
    hmac_update(&h, g_drbg_v, 32);
    hmac_update(&h, &sep, 1);
    hmac_update(&h, provided, len);
    hmac_final(&h, g_drbg_k);
    hmac(HASH_SHA256, g_drbg_k, 32, g_drbg_v, 32, g_drbg_v);
}

#if defined(__x86_64__)
static uint64_t cpu_tsc(void)
{
    uint32_t lo, hi;

    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static int cpu_has_rdrand(void)
{
    uint32_t a, b, c, d;

    __asm__ volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(1U), "c"(0U));
    (void)a; (void)b; (void)d;
    return (int)((c >> 30) & 1U);
}

static int cpu_rdrand64(uint64_t *out)
{
    unsigned char ok = 0;
    uint64_t r = 0;
    int try_;

    for (try_ = 0; try_ < 16; try_++) {
        __asm__ volatile("rdrand %0; setc %1" : "=r"(r), "=qm"(ok) : : "cc");
        if (ok) {
            *out = r;
            return 1;
        }
    }
    return 0;
}
#else
static uint64_t cpu_tsc(void) { return 0; }
static int cpu_has_rdrand(void) { return 0; }
static int cpu_rdrand64(uint64_t *out) { (void)out; return 0; }
#endif

/* Collect what we can into buf; returns how many bytes were written. */
static unsigned long entropy_gather(uint8_t *buf, unsigned long cap)
{
    struct wbuf w;
    uint64_t t;
    int i, hw = 0;

    wb_init(&w, buf, cap);
    if (cpu_has_rdrand()) {
        for (i = 0; i < 8; i++) {
            uint64_t r;
            if (!cpu_rdrand64(&r))
                break;
            wbytes(&w, &r, sizeof(r));
            hw = 1;
        }
    }
    /* Timer jitter: consecutive TSC reads across a loop of work. */
    for (i = 0; i < 16; i++) {
        t = cpu_tsc();
        wbytes(&w, &t, sizeof(t));
    }
    t = tls_now_unix();
    wbytes(&w, &t, sizeof(t));
    t = (uint64_t)(uintptr_t)buf;
    wbytes(&w, &t, sizeof(t));
    t = (uint64_t)(uintptr_t)&g_drbg_k;
    wbytes(&w, &t, sizeof(t));

#ifdef TLS_HOST
    {
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) {
            uint8_t b[32];
            ssize_t n = read(fd, b, sizeof(b));
            close(fd);
            if (n == (ssize_t)sizeof(b)) {
                wbytes(&w, b, sizeof(b));
                hw = 1;
            }
        }
        t = (uint64_t)getpid();
        wbytes(&w, &t, sizeof(t));
    }
#else
    {
        uint8_t b[64];
        long n = getrandom(b, sizeof(b), GRND_RANDOM);

        if (n == (long)sizeof(b)) {
            wbytes(&w, b, sizeof(b));
            hw = 1;  /* kernel pool crossed its initialization threshold */
        }
        wipe(b, sizeof(b));
        t = (uint64_t)getpid();
        wbytes(&w, &t, sizeof(t));
        t = uptime_ms();
        wbytes(&w, &t, sizeof(t));
    }
#endif
    if (hw)
        g_weak_entropy = 0;
    return (unsigned long)(w.p - buf);
}

static void drbg_reseed(void)
{
    uint8_t seed[256];
    unsigned long n;

    n = entropy_gather(seed, sizeof(seed));
    if (!g_drbg_seeded) {
        memset(g_drbg_k, 0x00, sizeof(g_drbg_k));
        memset(g_drbg_v, 0x01, sizeof(g_drbg_v));
        g_drbg_seeded = 1;
    }
    drbg_update(seed, n);
    wipe(seed, sizeof(seed));
}

void tls_add_entropy(const void *data, unsigned long len)
{
    if (!data || !len)
        return;
    if (!g_drbg_seeded)
        drbg_reseed();
    drbg_update(data, len);
}

int tls_entropy_is_weak(void)
{
    if (!g_drbg_seeded)
        drbg_reseed();
    return g_weak_entropy;
}

static void drbg_bytes(void *out, unsigned long n)
{
    uint8_t *p = out;

    if (!g_drbg_seeded)
        drbg_reseed();
    while (n) {
        unsigned long take = n < 32 ? n : 32;
        hmac(HASH_SHA256, g_drbg_k, 32, g_drbg_v, 32, g_drbg_v);
        memcpy(p, g_drbg_v, take);
        p += take;
        n -= take;
    }
    drbg_update(0, 0);
}

/* ===================================================================== *
 * Digest registry                                                       *
 *                                                                       *
 * rsa.c and x509.c look digests up by id; hash.c provides them. Wiring   *
 * the two together is the TLS client's job because it is the thing that  *
 * uses both. Registration is idempotent.                                 *
 * ===================================================================== */

static void md_sha256_fn(const void *d, size_t n, uint8_t *out)
{
    hash_oneshot(HASH_SHA256, d, (unsigned long)n, out);
}
static void md_sha384_fn(const void *d, size_t n, uint8_t *out)
{
    hash_oneshot(HASH_SHA384, d, (unsigned long)n, out);
}
static void md_sha512_fn(const void *d, size_t n, uint8_t *out)
{
    hash_oneshot(HASH_SHA512, d, (unsigned long)n, out);
}

static const struct crypto_md md_sha256 = {
    CRYPTO_MD_SHA256, "sha256", 32, md_sha256_fn
};
static const struct crypto_md md_sha384 = {
    CRYPTO_MD_SHA384, "sha384", 48, md_sha384_fn
};
static const struct crypto_md md_sha512 = {
    CRYPTO_MD_SHA512, "sha512", 64, md_sha512_fn
};

/* SHA-1 is deliberately absent: hash.h does not implement it, and a
 * certificate still signed with SHA-1 in 2026 should fail. */
static void tls_register_digests(void)
{
    crypto_md_register(&md_sha256);
    crypto_md_register(&md_sha384);
    crypto_md_register(&md_sha512);
}

/* ===================================================================== *
 * Cipher suites                                                         *
 * ===================================================================== */

struct suite_def {
    int id;
    int hash;                 /* HASH_*  */
    int aead;                 /* AEAD_*  */
    unsigned flag;            /* TLS_S_* */
    const char *name;
};

static const struct suite_def g_suites[] = {
    { CS_CHACHA20_SHA256,  HASH_SHA256, AEAD_CHACHA20_POLY1305,
      TLS_S_CHACHA20,  "TLS_CHACHA20_POLY1305_SHA256" },
    { CS_AES128GCM_SHA256, HASH_SHA256, AEAD_AES_128_GCM,
      TLS_S_AES128GCM, "TLS_AES_128_GCM_SHA256" },
    { CS_AES256GCM_SHA384, HASH_SHA384, AEAD_AES_256_GCM,
      TLS_S_AES256GCM, "TLS_AES_256_GCM_SHA384" }
};
#define N_SUITES ((int)(sizeof(g_suites) / sizeof(g_suites[0])))

static const struct suite_def *suite_by_id(int id)
{
    int i;

    for (i = 0; i < N_SUITES; i++)
        if (g_suites[i].id == id)
            return &g_suites[i];
    return 0;
}

const char *tls_suite_name(int id)
{
    const struct suite_def *s = suite_by_id(id);

    return s ? s->name : "(unknown cipher suite)";
}

const char *tls_group_name(int id)
{
    switch (id) {
    case GRP_X25519: return "x25519";
    case GRP_P256:   return "secp256r1";
    case 0x0018:     return "secp384r1";
    case 0x0019:     return "secp521r1";
    case 0x001e:     return "x448";
    case 0x0100:     return "ffdhe2048";
    case 0x0101:     return "ffdhe3072";
    case 0x11ec:     return "X25519MLKEM768";
    default:         return "an unknown group";
    }
}

/* ===================================================================== *
 * The connection                                                        *
 * ===================================================================== */

#define ST_NEW        0
#define ST_HANDSHAKE  1
#define ST_READY      2
#define ST_CLOSED     3
#define ST_FAILED     4

struct tls_conn {
    struct tls_transport lower;
    int    own_lower;
    int    state;
    struct tls_error err;

    char   host[256];
    struct tls_options opt;
    const struct x509_store *roots;
    uint32_t now;

    /* negotiated */
    const struct suite_def *suite;
    int    group;
    int    hrr_done;
    int    rsa_fallback;
    char   alpn[32];

    /* transcript: both candidate hashes run until the suite is known */
    struct hash_ctx tr256;
    struct hash_ctx tr384;
    struct hash_ctx tr;
    int    tr_selected;

    /* key schedule */
    uint8_t secret[HASH_MAX_DIGEST];       /* rolling: early -> hs -> master */
    uint8_t c_hs[HASH_MAX_DIGEST];
    uint8_t s_hs[HASH_MAX_DIGEST];
    uint8_t c_ap[HASH_MAX_DIGEST];
    uint8_t s_ap[HASH_MAX_DIGEST];

    /* record layer */
    struct aead_ctx rx_aead, tx_aead;
    uint8_t rx_iv[AEAD_NONCE_LEN], tx_iv[AEAD_NONCE_LEN];
    uint64_t rx_seq, tx_seq;
    int    rx_ready, tx_ready;
    int    first_record_sent;
    int    ccs_sent, ccs_seen;
    int    got_close_notify, sent_close_notify;
    int    handshake_done;

    /* our key shares */
    uint8_t client_random[32];
    uint8_t session_id[32];
    uint8_t x_priv[32], x_pub[32];
    uint8_t p_priv[32];
    struct p256_point p_pub;
    unsigned shares_sent;                  /* TLS_G_* mask */
    uint8_t cookie[COOKIE_MAX];
    unsigned long cookie_len;

    /* certificate request context, when the server asked for a cert */
    int    cert_requested;
    uint8_t cr_context[255];
    unsigned long cr_context_len;

    /* peer certificates */
    uint8_t *certbuf;
    unsigned long certbuf_len;
    struct x509_cert chain[X509_MAX_CHAIN];
    int    chain_len;
    int    verified;
    int    want_rsa_retry;             /* the chain used a curve we lack */
    char   cert_error[X509_ERR_LEN];
    /* Copied out of the leaf before certbuf is released, because every
     * pointer in struct x509_cert points into that buffer. */
    char   peer_subject[X509_MAX_CN];
    char   peer_issuer[X509_MAX_CN];
    uint32_t peer_not_before, peer_not_after;

    /* buffers */
    unsigned char rx[RX_CAP];
    int    rxlen;
    unsigned char pt[PT_CAP];
    int    ptlen, ptoff, pttype;
    unsigned char tx[TX_CAP];
    unsigned char hs[HS_CAP];
    unsigned long hslen;
    unsigned long hs_msglen;               /* current message, header included */
    unsigned char scratch[SCRATCH_CAP];
};

/* ---- errors ---------------------------------------------------------- */

static int rec_write(struct tls_conn *c, int type, const uint8_t *data,
                     unsigned long len, int force_plain);

static void send_alert(struct tls_conn *c, int level, int desc)
{
    uint8_t b[2];

    if (!c->lower.write || c->state == ST_CLOSED)
        return;
    b[0] = (uint8_t)level;
    b[1] = (uint8_t)desc;
    /* Best effort: we are already failing, a write error changes nothing. */
    rec_write(c, CT_ALERT, b, 2, 0);
}

static int vfail(struct tls_conn *c, int code, int alert, const char *fmt,
                 va_list ap)
{
    if (c->err.code == TLS_OK || c->state != ST_FAILED) {
        c->err.code = code;
        c->err.alert = alert;
        c->err.alert_received = 0;
        vsnprintf(c->err.msg, sizeof(c->err.msg), fmt, ap);
    }
    if (alert >= 0)
        send_alert(c, AL_FATAL, alert);
    c->state = ST_FAILED;
    return code;
}

static int fail(struct tls_conn *c, int code, int alert, const char *fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = vfail(c, code, alert, fmt, ap);
    va_end(ap);
    return r;
}

const char *tls_conn_error(const struct tls_conn *c)
{
    if (!c)
        return "no connection";
    return c->err.msg[0] ? c->err.msg : "no error";
}

const char *tls_error_text(int code)
{
    switch (code) {
    case TLS_OK:            return "no error";
    case TLS_E_INVAL:       return "invalid argument";
    case TLS_E_DNS:         return "the host name did not resolve";
    case TLS_E_CONNECT:     return "the connection could not be opened";
    case TLS_E_IO:          return "the connection failed";
    case TLS_E_TIMEOUT:     return "the connection timed out";
    case TLS_E_PROTO:       return "the server sent a malformed message";
    case TLS_E_ALERT:       return "the server aborted the connection";
    case TLS_E_VERSION:     return "the server will not speak TLS 1.3";
    case TLS_E_CERT:        return "the certificate is not trusted";
    case TLS_E_CRYPTO:      return "a cryptographic check failed";
    case TLS_E_NOMEM:       return "out of memory";
    case TLS_E_CLOSED:      return "the connection is closed";
    case TLS_E_TRUNCATED:   return "the connection was cut short";
    case TLS_E_UNSUPPORTED: return "unsupported by this client";
    default:                return "unknown error";
    }
}

const char *tls_alert_text(int desc)
{
    switch (desc) {
    case A_CLOSE_NOTIFY:       return "the connection was closed";
    case A_UNEXPECTED_MESSAGE: return "it received a message out of order";
    case A_BAD_RECORD_MAC:     return "a record failed authentication";
    case A_RECORD_OVERFLOW:    return "a record was too large";
    case A_HANDSHAKE_FAILURE:
        return "it could not agree on a cipher suite or key exchange group";
    case A_BAD_CERTIFICATE:    return "it rejected a certificate";
    case A_UNSUPPORTED_CERT:   return "it does not support this certificate";
    case A_CERTIFICATE_REVOKED: return "a certificate has been revoked";
    case A_CERTIFICATE_EXPIRED: return "a certificate has expired";
    case A_CERTIFICATE_UNKNOWN: return "a certificate was not acceptable";
    case A_ILLEGAL_PARAMETER:
        return "it rejected a value in the handshake as illegal";
    case A_UNKNOWN_CA:         return "it does not trust the certificate issuer";
    case A_ACCESS_DENIED:      return "access was denied";
    case A_DECODE_ERROR:       return "it could not decode a message";
    case A_DECRYPT_ERROR:      return "a signature or key exchange failed";
    case A_PROTOCOL_VERSION:
        return "it does not support TLS 1.3 (it most likely requires "
               "TLS 1.2, which this client does not implement)";
    case A_INSUFFICIENT_SEC:
        return "it requires stronger parameters than this client offered";
    case A_INTERNAL_ERROR:     return "it hit an internal error";
    case A_INAPPROPRIATE_FB:   return "it detected a downgrade attempt";
    case A_USER_CANCELED:      return "the handshake was cancelled";
    case A_MISSING_EXTENSION:  return "a required extension was missing";
    case A_UNSUPPORTED_EXT:    return "an extension was not acceptable";
    case A_UNRECOGNIZED_NAME:
        return "it does not serve the requested host name";
    case A_BAD_CERT_STATUS:    return "the certificate status response was bad";
    case A_UNKNOWN_PSK:        return "the pre-shared key identity was unknown";
    case A_CERTIFICATE_REQUIRED: return "it requires a client certificate";
    case A_NO_APPLICATION_PROTOCOL:
        return "it does not support any of the offered application protocols";
    default:                   return "an unspecified problem occurred";
    }
}

/* ===================================================================== *
 * Transcript                                                            *
 * ===================================================================== */

static void tr_update(struct tls_conn *c, const void *d, unsigned long n)
{
    if (c->tr_selected) {
        hash_update(&c->tr, d, n);
    } else {
        hash_update(&c->tr256, d, n);
        hash_update(&c->tr384, d, n);
    }
}

static void tr_select(struct tls_conn *c, int alg)
{
    hash_copy(&c->tr, alg == HASH_SHA384 ? &c->tr384 : &c->tr256);
    c->tr_selected = 1;
}

static void tr_hash(const struct tls_conn *c, uint8_t *out)
{
    hash_peek(&c->tr, out);
}

static unsigned long hlen(const struct tls_conn *c)
{
    return hash_digest_len(c->suite->hash);
}

/* ===================================================================== *
 * Key schedule (RFC 8446 section 7.1)                                   *
 * ===================================================================== */

static int ks_early(struct tls_conn *c)
{
    uint8_t zeros[HASH_MAX_DIGEST];
    int alg = c->suite->hash;
    unsigned long n = hash_digest_len(alg);

    memset(zeros, 0, sizeof(zeros));
    /* Early Secret = HKDF-Extract(salt = 0, IKM = PSK = 0^Hash.length) */
    return hkdf_extract(alg, 0, 0, zeros, n, c->secret);
}

static int ks_handshake(struct tls_conn *c, const uint8_t *shared,
                        unsigned long sharedlen)
{
    uint8_t derived[HASH_MAX_DIGEST];
    uint8_t th[HASH_MAX_DIGEST];
    int alg = c->suite->hash;

    if (hkdf_derive_secret(alg, c->secret, "derived", "", 0, derived) < 0)
        return -1;
    if (hkdf_extract(alg, derived, hash_digest_len(alg), shared, sharedlen,
                     c->secret) < 0)
        return -1;
    tr_hash(c, th);                        /* ClientHello..ServerHello */
    if (hkdf_derive_secret_hash(alg, c->secret, "c hs traffic", th,
                                c->c_hs) < 0 ||
        hkdf_derive_secret_hash(alg, c->secret, "s hs traffic", th,
                                c->s_hs) < 0)
        return -1;
    wipe(derived, sizeof(derived));
    return 0;
}

static int ks_master(struct tls_conn *c)
{
    uint8_t derived[HASH_MAX_DIGEST];
    uint8_t zeros[HASH_MAX_DIGEST];
    uint8_t th[HASH_MAX_DIGEST];
    int alg = c->suite->hash;
    unsigned long n = hash_digest_len(alg);

    memset(zeros, 0, sizeof(zeros));
    if (hkdf_derive_secret(alg, c->secret, "derived", "", 0, derived) < 0)
        return -1;
    if (hkdf_extract(alg, derived, n, zeros, n, c->secret) < 0)
        return -1;
    /* ClientHello..server Finished -- the client Finished is NOT included. */
    tr_hash(c, th);
    if (hkdf_derive_secret_hash(alg, c->secret, "c ap traffic", th,
                                c->c_ap) < 0 ||
        hkdf_derive_secret_hash(alg, c->secret, "s ap traffic", th,
                                c->s_ap) < 0)
        return -1;
    wipe(derived, sizeof(derived));
    return 0;
}

/* Install traffic keys derived from `secret` into one direction. */
static int install_keys(struct tls_conn *c, const uint8_t *secret, int send)
{
    uint8_t key[AEAD_MAX_KEY];
    int alg = c->suite->hash;
    unsigned long klen = aead_key_len(c->suite->aead);
    int rc = 0;

    if (hkdf_expand_label(alg, secret, "key", 0, 0, key, klen) < 0 ||
        hkdf_expand_label(alg, secret, "iv", 0, 0,
                          send ? c->tx_iv : c->rx_iv, AEAD_NONCE_LEN) < 0)
        rc = -1;
    if (rc == 0)
        rc = aead_init(send ? &c->tx_aead : &c->rx_aead, c->suite->aead, key);
    wipe(key, sizeof(key));
    if (rc < 0)
        return -1;
    if (send) {
        c->tx_seq = 0;
        c->tx_ready = 1;
    } else {
        c->rx_seq = 0;
        c->rx_ready = 1;
    }
    return 0;
}

/* verify_data = HMAC(finished_key, Transcript-Hash(...)) */
static int finished_mac(struct tls_conn *c, const uint8_t *base_secret,
                        const uint8_t *thash, uint8_t *out)
{
    uint8_t fk[HASH_MAX_DIGEST];
    int alg = c->suite->hash;
    unsigned long n = hash_digest_len(alg);

    if (hkdf_expand_label(alg, base_secret, "finished", 0, 0, fk, n) < 0)
        return -1;
    if (hmac(alg, fk, n, thash, n, out) < 0) {
        wipe(fk, sizeof(fk));
        return -1;
    }
    wipe(fk, sizeof(fk));
    return 0;
}

/* ===================================================================== *
 * Record layer                                                          *
 * ===================================================================== */

static int lower_write_all(struct tls_conn *c, const uint8_t *p,
                           unsigned long n)
{
    while (n) {
        int chunk = n > 16384UL ? 16384 : (int)n;
        int w = c->lower.write(c->lower.ctx, p, chunk);
        if (w <= 0)
            return -1;
        p += w;
        n -= (unsigned long)w;
    }
    return 0;
}

/* One record out. `force_plain` writes it unencrypted even when send keys
 * are installed, which is what the dummy change_cipher_spec needs. */
static int rec_write(struct tls_conn *c, int type, const uint8_t *data,
                     unsigned long len, int force_plain)
{
    unsigned long off = 0;

    if (!c->lower.write)
        return TLS_E_IO;
    do {
        unsigned long n = len - off;
        unsigned long body;
        int encrypt = c->tx_ready && !force_plain;

        if (n > REC_MAX_PLAIN)
            n = REC_MAX_PLAIN;
        if (encrypt) {
            uint8_t nonce[AEAD_NONCE_LEN];
            /* TLSInnerPlaintext = content || content_type || zeros.
             * No padding: it costs bytes and hides nothing an observer
             * cannot get from the record boundaries anyway. */
            body = n + 1 + AEAD_TAG_LEN;
            c->tx[0] = CT_APPDATA;
            c->tx[1] = 0x03;
            c->tx[2] = 0x03;
            c->tx[3] = (uint8_t)(body >> 8);
            c->tx[4] = (uint8_t)body;
            memcpy(c->tx + REC_HDR, data + off, n);
            c->tx[REC_HDR + n] = (uint8_t)type;
            aead_nonce(c->tx_iv, AEAD_NONCE_LEN, c->tx_seq, nonce);
            if (aead_ctx_seal(&c->tx_aead, nonce, c->tx, REC_HDR,
                              c->tx + REC_HDR, n + 1, c->tx + REC_HDR) < 0)
                return TLS_E_CRYPTO;
            c->tx_seq++;
        } else {
            body = n;
            c->tx[0] = (uint8_t)type;
            /* RFC 8446 5.1: 0x0301 is allowed only on an initial
             * ClientHello, 0x0303 everywhere else. */
            c->tx[1] = 0x03;
            c->tx[2] = c->first_record_sent ? 0x03 : 0x01;
            c->tx[3] = (uint8_t)(body >> 8);
            c->tx[4] = (uint8_t)body;
            memcpy(c->tx + REC_HDR, data + off, n);
        }
        c->first_record_sent = 1;
        if (lower_write_all(c, c->tx, REC_HDR + body) < 0)
            return TLS_E_IO;
        off += n;
    } while (off < len);
    return TLS_OK;
}

/* Pull at least `want` raw bytes into c->rx. Returns TLS_OK, or an error;
 * TLS_E_TRUNCATED when the peer went away mid-record. */
static int rx_fill(struct tls_conn *c, int want)
{
    while (c->rxlen < want) {
        int n = c->lower.read(c->lower.ctx, c->rx + c->rxlen,
                              (int)(RX_CAP - (unsigned)c->rxlen));
        if (n == 0)
            return c->rxlen == 0 && want == REC_HDR
                   ? TLS_E_CLOSED : TLS_E_TRUNCATED;
        if (n < 0) {
            /* -13 is libweb's HTTP_E_TIMEOUT, in case an http transport is
             * handed to tls_client() as the lower layer. */
            if (n == TLS_E_TIMEOUT || n == -13)
                return TLS_E_TIMEOUT;
            return TLS_E_IO;
        }
        c->rxlen += n;
    }
    return TLS_OK;
}

static void rx_consume(struct tls_conn *c, int n)
{
    if (n >= c->rxlen) {
        c->rxlen = 0;
        return;
    }
    memmove(c->rx, c->rx + n, (unsigned long)(c->rxlen - n));
    c->rxlen -= n;
}

/* Describe a first response that is not TLS at all, e.g. an HTTP error
 * page from a server that is not listening for TLS on this port. */
static int not_tls(struct tls_conn *c)
{
    char snip[41];
    int i, n = c->rxlen < 40 ? c->rxlen : 40;
    int printable = 1;

    for (i = 0; i < n; i++) {
        unsigned ch = c->rx[i];
        if (ch == '\r' || ch == '\n') {
            n = i;
            break;
        }
        if (ch < 0x20 || ch > 0x7e)
            printable = 0;
        snip[i] = (char)ch;
    }
    snip[n < 0 ? 0 : n] = 0;
    if (printable && n > 3)
        return fail(c, TLS_E_PROTO, -1,
                    "the server did not answer with TLS; it sent \"%s\", so "
                    "it is probably speaking plain HTTP on this port", snip);
    return fail(c, TLS_E_PROTO, -1,
                "the server did not answer with TLS (first byte 0x%02x)",
                c->rxlen ? c->rx[0] : 0);
}

/* Read exactly one record into c->pt / c->ptlen / c->pttype. Dummy
 * change_cipher_spec records are swallowed here. */
static int rec_read(struct tls_conn *c)
{
    for (;;) {
        int type, len, rc;
        unsigned ver;

        rc = rx_fill(c, REC_HDR);
        if (rc != TLS_OK)
            return rc;                     /* TLS_E_CLOSED = clean EOF */
        type = c->rx[0];
        ver = ((unsigned)c->rx[1] << 8) | c->rx[2];
        len = ((int)c->rx[3] << 8) | c->rx[4];

        if (type < CT_CCS || type > CT_APPDATA)
            return not_tls(c);
        if (ver < 0x0300 || ver > 0x0304)
            return not_tls(c);
        if (len > (c->rx_ready ? REC_MAX_CIPHER : REC_MAX_PLAIN))
            return fail(c, TLS_E_PROTO, A_RECORD_OVERFLOW,
                        "the server sent a %d byte record, over the %d byte "
                        "limit", len, c->rx_ready ? REC_MAX_CIPHER
                                                  : REC_MAX_PLAIN);
        rc = rx_fill(c, REC_HDR + len);
        if (rc != TLS_OK)
            return rc == TLS_E_CLOSED ? TLS_E_TRUNCATED : rc;

        if (type == CT_CCS) {
            /* RFC 8446 appendix D.4: a dummy record for middleboxes. It is
             * legal only before the peer's Finished, and only ever 0x01. */
            if (c->handshake_done)
                return fail(c, TLS_E_PROTO, A_UNEXPECTED_MESSAGE,
                            "the server sent a change_cipher_spec record "
                            "after the handshake finished");
            if (len != 1 || c->rx[REC_HDR] != 0x01)
                return fail(c, TLS_E_PROTO, A_UNEXPECTED_MESSAGE,
                            "the server sent a malformed change_cipher_spec "
                            "record");
            if (++c->ccs_seen > MAX_CCS)
                return fail(c, TLS_E_PROTO, A_UNEXPECTED_MESSAGE,
                            "the server sent more than %d change_cipher_spec "
                            "records", MAX_CCS);
            rx_consume(c, REC_HDR + len);
            continue;
        }

        if (!c->rx_ready) {
            memcpy(c->pt, c->rx + REC_HDR, (unsigned long)len);
            c->ptlen = len;
            c->ptoff = 0;
            c->pttype = type;
            rx_consume(c, REC_HDR + len);
            return TLS_OK;
        }

        /* An unencrypted alert once keys are up: some servers and rather
         * more middleboxes still do this on a fatal error. Accepting it
         * during the handshake turns a mystery hang into a real message;
         * after the handshake it is refused, because there it could only
         * be an injected disconnect. */
        if (type == CT_ALERT && !c->handshake_done && len == 2) {
            memcpy(c->pt, c->rx + REC_HDR, 2);
            c->ptlen = 2;
            c->ptoff = 0;
            c->pttype = CT_ALERT;
            rx_consume(c, REC_HDR + len);
            return TLS_OK;
        }
        if (type != CT_APPDATA)
            return fail(c, TLS_E_PROTO, A_UNEXPECTED_MESSAGE,
                        "the server sent an unprotected record (type %d) "
                        "after the keys were established", type);
        if (len < AEAD_TAG_LEN + 1)
            return fail(c, TLS_E_PROTO, A_DECODE_ERROR,
                        "the server sent a %d byte encrypted record, which "
                        "is too short to hold a tag", len);
        {
            uint8_t nonce[AEAD_NONCE_LEN];
            int inner;

            aead_nonce(c->rx_iv, AEAD_NONCE_LEN, c->rx_seq, nonce);
            if (aead_ctx_open(&c->rx_aead, nonce, c->rx, REC_HDR,
                              c->rx + REC_HDR, (unsigned long)len,
                              c->pt) < 0)
                return fail(c, TLS_E_CRYPTO, A_BAD_RECORD_MAC,
                            "a record from the server failed authentication "
                            "(record %llu of this key)",
                            (unsigned long long)c->rx_seq);
            if (c->rx_seq == 0xffffffffffffffffULL)
                return fail(c, TLS_E_PROTO, A_INTERNAL_ERROR,
                            "the record sequence number wrapped");
            c->rx_seq++;
            inner = len - AEAD_TAG_LEN;
            while (inner > 0 && c->pt[inner - 1] == 0)
                inner--;
            if (inner == 0)
                return fail(c, TLS_E_PROTO, A_UNEXPECTED_MESSAGE,
                            "the server sent a record with no content type");
            c->pttype = c->pt[inner - 1];
            c->ptlen = inner - 1;
            c->ptoff = 0;
            rx_consume(c, REC_HDR + len);
            if (c->pttype == CT_CCS)
                return fail(c, TLS_E_PROTO, A_UNEXPECTED_MESSAGE,
                            "the server put a change_cipher_spec inside an "
                            "encrypted record");
            if (c->pttype != CT_ALERT && c->pttype != CT_HANDSHAKE &&
                c->pttype != CT_APPDATA)
                return fail(c, TLS_E_PROTO, A_UNEXPECTED_MESSAGE,
                            "the server sent an unknown content type (%d)",
                            c->pttype);
            return TLS_OK;
        }
    }
}

/* Turn a received alert record into an error (or into "keep going" for a
 * warning we are allowed to ignore). */
static int handle_alert(struct tls_conn *c)
{
    int level, desc;

    if (c->ptlen != 2)
        return fail(c, TLS_E_PROTO, A_DECODE_ERROR,
                    "the server sent a malformed alert (%d bytes)", c->ptlen);
    level = c->pt[0];
    desc = c->pt[1];
    if (desc == A_CLOSE_NOTIFY) {
        c->got_close_notify = 1;
        c->state = ST_CLOSED;
        return TLS_E_CLOSED;
    }
    if (level == AL_WARNING && desc == A_USER_CANCELED)
        return TLS_OK;                     /* caller loops */
    c->err.code = desc == A_PROTOCOL_VERSION ? TLS_E_VERSION : TLS_E_ALERT;
    c->err.alert = desc;
    c->err.alert_received = 1;
    snprintf(c->err.msg, sizeof(c->err.msg),
             "the server rejected the connection: %s (TLS alert %d%s)",
             tls_alert_text(desc), desc,
             level == AL_WARNING ? ", warning" : "");
    c->state = ST_FAILED;
    return c->err.code;
}

/* ===================================================================== *
 * Handshake message reassembly                                          *
 * ===================================================================== */

/* Append the current plaintext record to the handshake buffer. */
static int hs_absorb(struct tls_conn *c)
{
    unsigned long n = (unsigned long)(c->ptlen - c->ptoff);

    if (n == 0)
        return fail(c, TLS_E_PROTO, A_DECODE_ERROR,
                    "the server sent an empty handshake record");
    if (c->hslen + n > HS_CAP)
        return fail(c, TLS_E_PROTO, A_DECODE_ERROR,
                    "the server's handshake messages exceeded the %d byte "
                    "reassembly buffer", (int)HS_CAP);
    memcpy(c->hs + c->hslen, c->pt + c->ptoff, n);
    c->hslen += n;
    c->ptoff = c->ptlen;
    return TLS_OK;
}

/* Get the next complete handshake message. On success *msg points at the
 * 4-byte header, mlen is header + body, and type, body and blen describe
 * the message. The bytes stay valid until hs_consume(). */
static int hs_next(struct tls_conn *c, int *type, const uint8_t **msg,
                   unsigned long *mlen, const uint8_t **body,
                   unsigned long *blen)
{
    unsigned long need = 4;

    for (;;) {
        if (c->hslen >= 4) {
            unsigned long l = ((unsigned long)c->hs[1] << 16) |
                              ((unsigned long)c->hs[2] << 8) | c->hs[3];
            if (l > TLS_HS_MAX)
                return fail(c, TLS_E_PROTO, A_DECODE_ERROR,
                            "the server sent a %lu byte handshake message, "
                            "over the %d byte limit this client accepts",
                            l, (int)TLS_HS_MAX);
            need = 4 + l;
            if (c->hslen >= need) {
                *type = c->hs[0];
                *msg = c->hs;
                *mlen = need;
                *body = c->hs + 4;
                *blen = l;
                c->hs_msglen = need;
                return TLS_OK;
            }
        }
        /* Need more bytes: pull another record. */
        if (c->ptoff >= c->ptlen) {
            int rc = rec_read(c);
            if (rc != TLS_OK)
                return rc;
            if (c->pttype == CT_ALERT) {
                rc = handle_alert(c);
                if (rc != TLS_OK)
                    return rc;
                continue;
            }
            if (c->pttype != CT_HANDSHAKE)
                return fail(c, TLS_E_PROTO, A_UNEXPECTED_MESSAGE,
                            "the server sent %s where a handshake message "
                            "was expected",
                            c->pttype == CT_APPDATA ? "application data"
                                                    : "another record type");
        }
        {
            int rc = hs_absorb(c);
            if (rc != TLS_OK)
                return rc;
        }
    }
}

static void hs_consume(struct tls_conn *c)
{
    unsigned long n = c->hs_msglen;

    if (n == 0 || n > c->hslen) {
        c->hslen = 0;
        return;
    }
    memmove(c->hs, c->hs + n, c->hslen - n);
    c->hslen -= n;
    c->hs_msglen = 0;
}

/* Write one handshake message we built in c->scratch. */
static int hs_send(struct tls_conn *c, int type, const uint8_t *body,
                   unsigned long len)
{
    uint8_t hdr[4];
    int rc;

    hdr[0] = (uint8_t)type;
    hdr[1] = (uint8_t)(len >> 16);
    hdr[2] = (uint8_t)(len >> 8);
    hdr[3] = (uint8_t)len;
    /* The transcript covers header + body; the record framing is not part
     * of it, which is why this is done here and not in rec_write(). */
    tr_update(c, hdr, 4);
    tr_update(c, body, len);
    /* One record: every message we send is far under 2^14 bytes. */
    if (len + 4 > SCRATCH_CAP)
        return fail(c, TLS_E_INVAL, A_INTERNAL_ERROR,
                    "internal error: handshake message too large to send");
    {
        uint8_t buf[SCRATCH_CAP + 4];
        memcpy(buf, hdr, 4);
        memcpy(buf + 4, body, len);
        rc = rec_write(c, CT_HANDSHAKE, buf, len + 4, 0);
    }
    if (rc != TLS_OK)
        return fail(c, rc, -1, "the connection failed while sending the "
                    "handshake");
    return TLS_OK;
}

/* ===================================================================== *
 * ClientHello                                                           *
 * ===================================================================== */

static int host_is_ip_literal(const char *h)
{
    int dots = 0;
    const char *p;

    if (strchr(h, ':'))
        return 1;                          /* IPv6 literal */
    for (p = h; *p; p++) {
        if (*p == '.') {
            dots++;
            continue;
        }
        if (*p < '0' || *p > '9')
            return 0;
    }
    return dots == 3;
}

static int gen_share(struct tls_conn *c, int group)
{
    if (group == GRP_X25519) {
        drbg_bytes(c->x_priv, sizeof(c->x_priv));
        if (x25519_base(c->x_pub, c->x_priv) < 0)
            return -1;
        return 0;
    }
    if (group == GRP_P256) {
        int tries;
        for (tries = 0; tries < 64; tries++) {
            drbg_bytes(c->p_priv, sizeof(c->p_priv));
            if (p256_base_mul(&c->p_pub, c->p_priv) == 0)
                return 0;
        }
        return -1;
    }
    return -1;
}

static void put_alpn(struct wbuf *w, const char *alpn)
{
    uint8_t *ext, *list;
    const char *p = alpn;

    w16(w, EXT_ALPN);
    ext = w_open16(w);
    list = w_open16(w);
    while (*p) {
        const char *q = p;
        uint8_t *nl;
        while (*q && *q != ',')
            q++;
        nl = w_open8(w);
        wbytes(w, p, (unsigned long)(q - p));
        w_close8(w, nl);
        p = *q ? q + 1 : q;
    }
    w_close16(w, list);
    w_close16(w, ext);
}

/* Is `name` one of the protocols we offered? */
static int alpn_offered(const char *alpn, const char *name, unsigned long n)
{
    const char *p = alpn;

    while (p && *p) {
        const char *q = p;
        while (*q && *q != ',')
            q++;
        if ((unsigned long)(q - p) == n && memcmp(p, name, n) == 0)
            return 1;
        p = *q ? q + 1 : q;
    }
    return 0;
}

static int build_client_hello(struct tls_conn *c, int second)
{
    struct wbuf w;
    uint8_t *ext_at, *at;
    unsigned groups = c->opt.groups;
    unsigned shares = c->opt.key_shares;
    int i;

    wb_init(&w, c->scratch, SCRATCH_CAP);

    w16(&w, 0x0303);                       /* legacy_version */
    wbytes(&w, c->client_random, 32);
    at = w_open8(&w);                      /* legacy_session_id */
    wbytes(&w, c->session_id, 32);
    w_close8(&w, at);

    at = w_open16(&w);                     /* cipher_suites */
    for (i = 0; i < N_SUITES; i++)
        if (c->opt.suites & g_suites[i].flag)
            w16(&w, g_suites[i].id);
    w_close16(&w, at);

    w8(&w, 1);                             /* legacy_compression_methods */
    w8(&w, 0);

    ext_at = w_open16(&w);

    if (c->opt.sni || !host_is_ip_literal(c->host)) {
        const char *name = c->opt.sni ? c->opt.sni : c->host;
        unsigned long n = strlen(name);
        uint8_t *e, *l;
        if (n && n < 256) {
            w16(&w, EXT_SERVER_NAME);
            e = w_open16(&w);
            l = w_open16(&w);              /* ServerNameList */
            w8(&w, 0);                     /* host_name */
            at = w_open16(&w);
            wbytes(&w, name, n);
            w_close16(&w, at);
            w_close16(&w, l);
            w_close16(&w, e);
        }
    }

    w16(&w, EXT_SUPPORTED_VERSIONS);       /* the extension that makes it 1.3 */
    at = w_open16(&w);
    w8(&w, 2);
    w16(&w, 0x0304);
    w_close16(&w, at);

    w16(&w, EXT_SUPPORTED_GROUPS);
    at = w_open16(&w);
    {
        uint8_t *l = w_open16(&w);
        if (groups & TLS_G_X25519)
            w16(&w, GRP_X25519);
        if (groups & TLS_G_P256)
            w16(&w, GRP_P256);
        w_close16(&w, l);
    }
    w_close16(&w, at);

    w16(&w, EXT_SIG_ALGS);
    at = w_open16(&w);
    {
        uint8_t *l = w_open16(&w);
        /* Exactly what rsa.h and ecc.h can actually verify, and nothing
         * else: advertising more would make servers pick chains we then
         * have to reject. P-384 and P-521 ECDSA and Ed25519 are absent
         * for that reason, and on the RSA fallback pass P-256 goes too. */
        if (!c->opt.rsa_only)
            w16(&w, SIG_ECDSA_P256_SHA256);
        w16(&w, SIG_RSA_PSS_RSAE_SHA256);
        w16(&w, SIG_RSA_PSS_RSAE_SHA384);
        w16(&w, SIG_RSA_PSS_RSAE_SHA512);
        w16(&w, SIG_RSA_PSS_PSS_SHA256);
        w16(&w, SIG_RSA_PSS_PSS_SHA384);
        w16(&w, SIG_RSA_PSS_PSS_SHA512);
        w16(&w, SIG_RSA_PKCS1_SHA256);
        w16(&w, SIG_RSA_PKCS1_SHA384);
        w16(&w, SIG_RSA_PKCS1_SHA512);
        w_close16(&w, l);
    }
    w_close16(&w, at);

    w16(&w, EXT_KEY_SHARE);
    at = w_open16(&w);
    {
        uint8_t *l = w_open16(&w);
        if (shares & TLS_G_X25519) {
            uint8_t *k;
            if (!second && gen_share(c, GRP_X25519) < 0)
                return fail(c, TLS_E_CRYPTO, -1,
                            "could not generate an X25519 key share");
            w16(&w, GRP_X25519);
            k = w_open16(&w);
            wbytes(&w, c->x_pub, 32);
            w_close16(&w, k);
        }
        if (shares & TLS_G_P256) {
            uint8_t enc[65];
            uint8_t *k;
            if (!second && gen_share(c, GRP_P256) < 0)
                return fail(c, TLS_E_CRYPTO, -1,
                            "could not generate a P-256 key share");
            p256_point_to_bytes(&c->p_pub, enc);
            w16(&w, GRP_P256);
            k = w_open16(&w);
            wbytes(&w, enc, sizeof(enc));
            w_close16(&w, k);
        }
        w_close16(&w, l);
    }
    w_close16(&w, at);
    c->shares_sent = shares;

    if (c->opt.alpn && c->opt.alpn[0])
        put_alpn(&w, c->opt.alpn);

    if (second && c->cookie_len) {
        w16(&w, EXT_COOKIE);
        at = w_open16(&w);
        {
            uint8_t *l = w_open16(&w);
            wbytes(&w, c->cookie, c->cookie_len);
            w_close16(&w, l);
        }
        w_close16(&w, at);
    }

    w_close16(&w, ext_at);
    if (w.ovf)
        return fail(c, TLS_E_INVAL, -1,
                    "the ClientHello did not fit in %d bytes (a host name or "
                    "cookie is too long)", (int)SCRATCH_CAP);
    return hs_send(c, HS_CLIENT_HELLO, c->scratch,
                   (unsigned long)(w.p - c->scratch));
}

/* ===================================================================== *
 * ServerHello / HelloRetryRequest                                       *
 * ===================================================================== */

struct sh_parsed {
    int      is_hrr;
    int      suite;
    int      version_ok;
    int      group;                        /* HRR: selected_group          */
    const uint8_t *ks;                     /* SH: server key_exchange      */
    unsigned long kslen;
    const uint8_t *cookie;
    unsigned long cookie_len;
    int      have_key_share;
};

static int parse_server_hello(struct tls_conn *c, const uint8_t *body,
                              unsigned long blen, struct sh_parsed *out)
{
    struct rbuf r;
    const uint8_t *rnd, *sid, *exts;
    unsigned long sidlen, extlen;
    unsigned legacy;
    struct rbuf e;

    memset(out, 0, sizeof(*out));
    rb_init(&r, body, blen);
    legacy = r16(&r);
    rnd = rbytes(&r, 32);
    if (rvec8(&r, &sid, &sidlen) < 0 || r.err)
        return fail(c, TLS_E_PROTO, A_DECODE_ERROR,
                    "the ServerHello is truncated");
    out->suite = (int)r16(&r);
    if (r8(&r) != 0)
        return fail(c, TLS_E_PROTO, A_ILLEGAL_PARAMETER,
                    "the server selected a compression method, which TLS 1.3 "
                    "does not allow");
    if (rvec16(&r, &exts, &extlen) < 0 || rb_left(&r) != 0)
        return fail(c, TLS_E_PROTO, A_DECODE_ERROR,
                    "the ServerHello has trailing bytes or a bad extension "
                    "block");

    out->is_hrr = memcmp(rnd, hrr_magic, 32) == 0;

    if (legacy < 0x0303)
        return fail(c, TLS_E_VERSION, A_PROTOCOL_VERSION,
                    "the server answered with TLS %s, and this client "
                    "implements only TLS 1.3",
                    legacy == 0x0301 ? "1.0" :
                    legacy == 0x0302 ? "1.1" : "an older version");

    /* The extensions are parsed -- and the version taken from them --
     * before anything else is judged, because a TLS 1.2 server gets every
     * other field slightly wrong too (it invents its own session id rather
     * than echoing ours, and picks a suite from a different registry), and
     * "the session id does not match" would be a useless thing to tell
     * someone whose real problem is that the server does not do TLS 1.3. */
    rb_init(&e, exts, extlen);
    while (rb_left(&e) >= 4) {
        unsigned id = r16(&e);
        const uint8_t *data;
        unsigned long n;
        struct rbuf x;

        if (rvec16(&e, &data, &n) < 0)
            return fail(c, TLS_E_PROTO, A_DECODE_ERROR,
                        "a ServerHello extension is truncated");
        rb_init(&x, data, n);
        switch (id) {
        case EXT_SUPPORTED_VERSIONS:
            if (n != 2)
                return fail(c, TLS_E_PROTO, A_DECODE_ERROR,
                            "the supported_versions extension is %lu bytes, "
                            "not 2", n);
            if (r16(&x) != 0x0304)
                return fail(c, TLS_E_VERSION, A_PROTOCOL_VERSION,
                            "the server selected a version other than "
                            "TLS 1.3");
            out->version_ok = 1;
            break;
        case EXT_KEY_SHARE:
            out->have_key_share = 1;
            out->group = (int)r16(&x);
            if (!out->is_hrr) {
                if (rvec16(&x, &out->ks, &out->kslen) < 0)
                    return fail(c, TLS_E_PROTO, A_DECODE_ERROR,
                                "the server's key_share is truncated");
            } else if (rb_left(&x) != 0) {
                return fail(c, TLS_E_PROTO, A_DECODE_ERROR,
                            "the HelloRetryRequest key_share is malformed");
            }
            break;
        case EXT_COOKIE:
            if (rvec16(&x, &out->cookie, &out->cookie_len) < 0)
                return fail(c, TLS_E_PROTO, A_DECODE_ERROR,
                            "the cookie extension is malformed");
            break;
        case EXT_PRE_SHARED_KEY:
            return fail(c, TLS_E_PROTO, A_ILLEGAL_PARAMETER,
                        "the server selected a pre-shared key, which this "
                        "client never offered");
        default:
            /* RFC 8446 4.1.3 allows only the three above in a ServerHello.
             * Anything else is a broken server rather than an attack, and
             * refusing to talk to it would help nobody, so it is ignored. */
            break;
        }
        if (x.err)
            return fail(c, TLS_E_PROTO, A_DECODE_ERROR,
                        "extension %u in the ServerHello is malformed", id);
    }
    if (rb_left(&e) != 0)
        return fail(c, TLS_E_PROTO, A_DECODE_ERROR,
                    "the ServerHello extension block is malformed");

    if (!out->version_ok)
        return fail(c, TLS_E_VERSION, A_PROTOCOL_VERSION,
                    "the server negotiated TLS 1.2, which this client does "
                    "not implement (it sent no supported_versions extension)");
    if (sidlen != 32 || memcmp(sid, c->session_id, 32) != 0)
        return fail(c, TLS_E_PROTO, A_ILLEGAL_PARAMETER,
                    "the server did not echo the session id, which a TLS 1.3 "
                    "server must");
    if (!suite_by_id(out->suite))
        return fail(c, TLS_E_PROTO, A_ILLEGAL_PARAMETER,
                    "the server selected cipher suite 0x%04x, which this "
                    "client did not offer", (unsigned)out->suite);
    if (!(suite_by_id(out->suite)->flag & c->opt.suites))
        return fail(c, TLS_E_PROTO, A_ILLEGAL_PARAMETER,
                    "the server selected %s, which this client did not offer",
                    tls_suite_name(out->suite));
    return TLS_OK;
}

/* Replace the transcript with the synthetic message_hash of RFC 8446
 * 4.4.1, which is what a HelloRetryRequest does to it. */
static int transcript_hrr(struct tls_conn *c)
{
    uint8_t th[HASH_MAX_DIGEST];
    uint8_t hdr[4];
    unsigned long n = hash_digest_len(c->suite->hash);

    tr_hash(c, th);                        /* Hash(ClientHello1) */
    hash_init(&c->tr, c->suite->hash);
    hdr[0] = HS_MESSAGE_HASH;
    hdr[1] = 0;
    hdr[2] = 0;
    hdr[3] = (uint8_t)n;
    hash_update(&c->tr, hdr, 4);
    hash_update(&c->tr, th, n);
    return TLS_OK;
}

static int do_key_exchange(struct tls_conn *c, int group, const uint8_t *peer,
                           unsigned long peerlen, uint8_t *shared,
                           unsigned long *sharedlen)
{
    if (group == GRP_X25519) {
        if (peerlen != 32)
            return fail(c, TLS_E_PROTO, A_ILLEGAL_PARAMETER,
                        "the server's X25519 key share is %lu bytes, not 32",
                        peerlen);
        if (x25519(shared, c->x_priv, peer) < 0)
            return fail(c, TLS_E_CRYPTO, A_ILLEGAL_PARAMETER,
                        "the server's X25519 key share is a small-order "
                        "point, so the shared secret would be zero");
        *sharedlen = 32;
        return TLS_OK;
    }
    if (group == GRP_P256) {
        struct p256_point pt;
        if (p256_point_from_bytes(&pt, peer, peerlen) < 0)
            return fail(c, TLS_E_CRYPTO, A_ILLEGAL_PARAMETER,
                        "the server's P-256 key share is not a point on the "
                        "curve");
        if (p256_ecdh(shared, c->p_priv, &pt) < 0)
            return fail(c, TLS_E_CRYPTO, A_ILLEGAL_PARAMETER,
                        "the P-256 key agreement failed");
        *sharedlen = 32;
        return TLS_OK;
    }
    return fail(c, TLS_E_PROTO, A_ILLEGAL_PARAMETER,
                "the server chose %s, a group this client did not offer",
                tls_group_name(group));
}

/* ===================================================================== *
 * EncryptedExtensions, Certificate, CertificateVerify, Finished         *
 * ===================================================================== */

static int parse_encrypted_extensions(struct tls_conn *c, const uint8_t *body,
                                      unsigned long blen)
{
    struct rbuf r, e;
    const uint8_t *exts;
    unsigned long extlen;

    rb_init(&r, body, blen);
    if (rvec16(&r, &exts, &extlen) < 0 || rb_left(&r) != 0)
        return fail(c, TLS_E_PROTO, A_DECODE_ERROR,
                    "the EncryptedExtensions message is malformed");
    rb_init(&e, exts, extlen);
    while (rb_left(&e) >= 4) {
        unsigned id = r16(&e);
        const uint8_t *data;
        unsigned long n;

        if (rvec16(&e, &data, &n) < 0)
            return fail(c, TLS_E_PROTO, A_DECODE_ERROR,
                        "an EncryptedExtensions entry is truncated");
        if (id == EXT_ALPN) {
            struct rbuf x;
            const uint8_t *list, *name;
            unsigned long listlen, namelen;
            rb_init(&x, data, n);
            if (rvec16(&x, &list, &listlen) < 0 || rb_left(&x) != 0)
                return fail(c, TLS_E_PROTO, A_DECODE_ERROR,
                            "the ALPN extension is malformed");
            rb_init(&x, list, listlen);
            if (rvec8(&x, &name, &namelen) < 0)
                return fail(c, TLS_E_PROTO, A_DECODE_ERROR,
                            "the ALPN protocol name the server selected is "
                            "truncated");
            if (rb_left(&x) != 0)
                return fail(c, TLS_E_PROTO, A_ILLEGAL_PARAMETER,
                            "the server selected more than one ALPN "
                            "protocol");
            if (namelen == 0 || namelen >= sizeof(c->alpn))
                return fail(c, TLS_E_PROTO, A_ILLEGAL_PARAMETER,
                            "the server selected an ALPN protocol name of "
                            "%lu bytes", namelen);
            if (!alpn_offered(c->opt.alpn, (const char *)name, namelen))
                return fail(c, TLS_E_PROTO, A_ILLEGAL_PARAMETER,
                            "the server selected an application protocol "
                            "this client did not offer");
            memcpy(c->alpn, name, namelen);
            c->alpn[namelen] = 0;
        } else if (id == EXT_EARLY_DATA) {
            return fail(c, TLS_E_PROTO, A_UNSUPPORTED_EXT,
                        "the server accepted early data, which this client "
                        "never offered");
        }
        /* Anything else -- server_name acknowledgement, max_fragment_length,
         * record_size_limit, supported_groups as a hint -- is either
         * informational or something we did not ask for. TLS 1.3 already
         * forbids a server from sending an extension the client did not
         * offer, and a server that breaks that rule here is not worth
         * killing the connection over. */
    }
    if (rb_left(&e) != 0)
        return fail(c, TLS_E_PROTO, A_DECODE_ERROR,
                    "the EncryptedExtensions block is malformed");
    return TLS_OK;
}

static int parse_cert_request(struct tls_conn *c, const uint8_t *body,
                              unsigned long blen)
{
    struct rbuf r;
    const uint8_t *ctx, *exts;
    unsigned long ctxlen, extlen;

    rb_init(&r, body, blen);
    if (rvec8(&r, &ctx, &ctxlen) < 0 ||
        rvec16(&r, &exts, &extlen) < 0 || rb_left(&r) != 0)
        return fail(c, TLS_E_PROTO, A_DECODE_ERROR,
                    "the CertificateRequest message is malformed");
    if (ctxlen > sizeof(c->cr_context))
        return fail(c, TLS_E_PROTO, A_DECODE_ERROR,
                    "the CertificateRequest context is %lu bytes", ctxlen);
    memcpy(c->cr_context, ctx, ctxlen);
    c->cr_context_len = ctxlen;
    c->cert_requested = 1;
    return TLS_OK;
}

static int parse_certificate(struct tls_conn *c, const uint8_t *body,
                             unsigned long blen)
{
    struct rbuf r, list;
    const uint8_t *ctx, *certs;
    unsigned long ctxlen, certslen;
    int n = 0;

    rb_init(&r, body, blen);
    if (rvec8(&r, &ctx, &ctxlen) < 0)
        return fail(c, TLS_E_PROTO, A_DECODE_ERROR,
                    "the Certificate message is truncated");
    if (ctxlen != 0)
        return fail(c, TLS_E_PROTO, A_ILLEGAL_PARAMETER,
                    "the server's Certificate carries a request context, "
                    "which is only legal in a reply to a "
                    "CertificateRequest");
    certslen = r24(&r);
    certs = rbytes(&r, certslen);
    if (r.err || rb_left(&r) != 0)
        return fail(c, TLS_E_PROTO, A_DECODE_ERROR,
                    "the certificate list length does not match the message");

    /* The certificates must outlive the handshake buffer, which is reused
     * by the next message, so take a copy. */
    if (certslen == 0)
        return fail(c, TLS_E_PROTO, A_CERTIFICATE_REQUIRED,
                    "the server sent an empty certificate chain");
    c->certbuf = malloc(certslen);
    if (!c->certbuf)
        return fail(c, TLS_E_NOMEM, A_INTERNAL_ERROR,
                    "out of memory copying the %lu byte certificate chain",
                    certslen);
    memcpy(c->certbuf, certs, certslen);
    c->certbuf_len = certslen;

    rb_init(&list, c->certbuf, certslen);
    while (rb_left(&list) > 0) {
        unsigned long dlen, extlen;
        const uint8_t *der, *exts;
        char e[X509_ERR_LEN];

        dlen = r24(&list);
        der = rbytes(&list, dlen);
        if (list.err)
            return fail(c, TLS_E_PROTO, A_DECODE_ERROR,
                        "certificate %d in the chain is truncated", n + 1);
        if (rvec16(&list, &exts, &extlen) < 0)
            return fail(c, TLS_E_PROTO, A_DECODE_ERROR,
                        "the extensions of certificate %d are truncated",
                        n + 1);
        if (n >= X509_MAX_CHAIN)
            return fail(c, TLS_E_CERT, A_UNSUPPORTED_CERT,
                        "the server sent more than %d certificates",
                        X509_MAX_CHAIN);
        if (x509_parse(&c->chain[n], der, dlen, e, sizeof(e)) < 0)
            return fail(c, TLS_E_CERT, A_BAD_CERTIFICATE,
                        "certificate %d in the chain could not be parsed: %s",
                        n + 1, e);
        n++;
    }
    c->chain_len = n;

    memcpy(c->peer_subject, c->chain[0].subject_cn, X509_MAX_CN);
    memcpy(c->peer_issuer, c->chain[0].issuer_cn, X509_MAX_CN);
    c->peer_not_before = c->chain[0].not_before;
    c->peer_not_after = c->chain[0].not_after;

    /* Path building, such as it is. x509_verify_chain() anchors the *top*
     * of what it is given in the trust store, so a server that appends a
     * cross-signed copy of its root -- which is extremely common, because
     * it is how a CA stays valid for clients with the older anchor -- would
     * fail against a store that holds the self-signed copy instead. So try
     * the chain the server sent, then each shorter prefix of it. Every
     * attempt is a full verification: the same expiry, host name, basic
     * constraints, key usage and signature checks, ending at a root that
     * is actually in the store. Trimming a trailing certificate can only
     * *remove* a link the store already covers, never skip one. The
     * message kept for the user is the one from the full chain, since
     * that is what the server actually presented. */
    {
        char first[X509_ERR_LEN];
        int k;

        first[0] = 0;
        c->verified = 0;
        for (k = n; k >= 1; k--) {
            c->verified = x509_verify_chain(c->chain, k, c->roots, c->host,
                                            c->now, c->cert_error,
                                            sizeof(c->cert_error));
            if (k == n)
                memcpy(first, c->cert_error, sizeof(first));
            if (c->verified)
                break;
        }
        if (!c->verified)
            memcpy(c->cert_error, first, sizeof(c->cert_error));
    }
    if (!c->verified) {
        /* Did the chain contain a key on a curve this build cannot check?
         * If so the caller may be able to get an RSA chain instead by
         * asking again without the ECDSA signature algorithms. This is a
         * structural test, not a guess at the error string. */
        int i;
        for (i = 0; i < n; i++)
            if (c->chain[i].pk_alg == X509_PK_EC &&
                c->chain[i].pk_curve != X509_CURVE_P256)
                c->want_rsa_retry = 1;
    }
    if (!c->verified && c->opt.verify == TLS_VERIFY_REQUIRED) {
        c->err.cert_failure = 1;
        return fail(c, TLS_E_CERT, A_CERTIFICATE_UNKNOWN, "%s",
                    c->cert_error[0] ? c->cert_error
                                     : "the certificate chain is not trusted");
    }
    if (c->verified)
        c->cert_error[0] = 0;
    return TLS_OK;
}

/* The signature scheme -> (digest, key type) mapping of RFC 8446 4.2.3. */
static int sig_hash_alg(int scheme)
{
    switch (scheme) {
    case SIG_ECDSA_P256_SHA256:
    case SIG_RSA_PSS_RSAE_SHA256:
    case SIG_RSA_PSS_PSS_SHA256:
    case SIG_RSA_PKCS1_SHA256:  return HASH_SHA256;
    case SIG_RSA_PSS_RSAE_SHA384:
    case SIG_RSA_PSS_PSS_SHA384:
    case SIG_RSA_PKCS1_SHA384:  return HASH_SHA384;
    case SIG_RSA_PSS_RSAE_SHA512:
    case SIG_RSA_PSS_PSS_SHA512:
    case SIG_RSA_PKCS1_SHA512:  return HASH_SHA512;
    default:                    return -1;
    }
}

static int sig_md_id(int alg)
{
    return alg == HASH_SHA256 ? CRYPTO_MD_SHA256 :
           alg == HASH_SHA384 ? CRYPTO_MD_SHA384 : CRYPTO_MD_SHA512;
}

static const char *sig_scheme_name(int scheme)
{
    switch (scheme) {
    case SIG_RSA_PKCS1_SHA256: return "rsa_pkcs1_sha256";
    case SIG_RSA_PKCS1_SHA384: return "rsa_pkcs1_sha384";
    case SIG_RSA_PKCS1_SHA512: return "rsa_pkcs1_sha512";
    case SIG_ECDSA_P256_SHA256: return "ecdsa_secp256r1_sha256";
    case SIG_ECDSA_P384_SHA384: return "ecdsa_secp384r1_sha384";
    case SIG_ECDSA_P521_SHA512: return "ecdsa_secp521r1_sha512";
    case SIG_RSA_PSS_RSAE_SHA256: return "rsa_pss_rsae_sha256";
    case SIG_RSA_PSS_RSAE_SHA384: return "rsa_pss_rsae_sha384";
    case SIG_RSA_PSS_RSAE_SHA512: return "rsa_pss_rsae_sha512";
    case SIG_RSA_PSS_PSS_SHA256: return "rsa_pss_pss_sha256";
    case SIG_RSA_PSS_PSS_SHA384: return "rsa_pss_pss_sha384";
    case SIG_RSA_PSS_PSS_SHA512: return "rsa_pss_pss_sha512";
    case SIG_ED25519: return "ed25519";
    case SIG_ED448: return "ed448";
    default: return "an unknown signature scheme";
    }
}

static int parse_certificate_verify(struct tls_conn *c, const uint8_t *body,
                                    unsigned long blen,
                                    const uint8_t *thash_before)
{
    struct rbuf r;
    const uint8_t *sig;
    unsigned long siglen;
    int scheme, alg;
    uint8_t content[64 + 33 + 1 + HASH_MAX_DIGEST];
    uint8_t digest[HASH_MAX_DIGEST];
    unsigned long clen;
    static const char ctx[] = "TLS 1.3, server CertificateVerify";

    rb_init(&r, body, blen);
    scheme = (int)r16(&r);
    if (rvec16(&r, &sig, &siglen) < 0 || rb_left(&r) != 0)
        return fail(c, TLS_E_PROTO, A_DECODE_ERROR,
                    "the CertificateVerify message is malformed");
    if (c->chain_len == 0)
        return fail(c, TLS_E_PROTO, A_UNEXPECTED_MESSAGE,
                    "the server sent a CertificateVerify without a "
                    "Certificate");

    alg = sig_hash_alg(scheme);
    if (alg < 0)
        return fail(c, TLS_E_UNSUPPORTED, A_ILLEGAL_PARAMETER,
                    "the server signed the handshake with %s, which this "
                    "client cannot verify", sig_scheme_name(scheme));
    if (scheme == SIG_RSA_PKCS1_SHA256 || scheme == SIG_RSA_PKCS1_SHA384 ||
        scheme == SIG_RSA_PKCS1_SHA512)
        return fail(c, TLS_E_PROTO, A_ILLEGAL_PARAMETER,
                    "the server signed the handshake with %s; RFC 8446 "
                    "forbids PKCS#1 v1.5 signatures in CertificateVerify",
                    sig_scheme_name(scheme));

    /* RFC 8446 4.4.3: 64 octets of 0x20, the context string, a single
     * zero byte, then the transcript hash. */
    memset(content, 0x20, 64);
    memcpy(content + 64, ctx, sizeof(ctx) - 1);
    content[64 + sizeof(ctx) - 1] = 0x00;
    memcpy(content + 64 + sizeof(ctx), thash_before, hlen(c));
    clen = 64 + sizeof(ctx) + hlen(c);

    hash_oneshot(alg, content, clen, digest);

    if (scheme == SIG_ECDSA_P256_SHA256) {
        struct p256_point pub;
        struct der d, seq;
        struct der_tlv t;
        const uint8_t *rr, *ss;
        size_t rlen, slen;

        if (c->chain[0].pk_alg != X509_PK_EC ||
            c->chain[0].pk_curve != X509_CURVE_P256)
            return fail(c, TLS_E_PROTO, A_ILLEGAL_PARAMETER,
                        "the server signed with ECDSA P-256 but its "
                        "certificate does not hold a P-256 key");
        if (p256_point_from_bytes(&pub, c->chain[0].pk_bits,
                                  c->chain[0].pk_bits_len) < 0)
            return fail(c, TLS_E_CERT, A_BAD_CERTIFICATE,
                        "the P-256 public key in the server certificate is "
                        "not a valid curve point");
        der_init(&d, sig, siglen);
        if (der_read_expect(&d, &t, DER_SEQUENCE) < 0 || !der_eof(&d))
            return fail(c, TLS_E_PROTO, A_DECODE_ERROR,
                        "the ECDSA signature is not a DER SEQUENCE");
        der_enter(&t, &seq);
        if (der_read_uint(&seq, &rr, &rlen) < 0 ||
            der_read_uint(&seq, &ss, &slen) < 0 || !der_eof(&seq))
            return fail(c, TLS_E_PROTO, A_DECODE_ERROR,
                        "the ECDSA signature is malformed");
        if (!p256_ecdsa_verify(&pub, digest, hash_digest_len(alg),
                               rr, rlen, ss, slen))
            return fail(c, TLS_E_CRYPTO, A_DECRYPT_ERROR,
                        "the server's handshake signature does not verify "
                        "against its certificate -- this connection is not "
                        "authentic");
        return TLS_OK;
    }

    {
        struct rsa_pubkey key;
        if (c->chain[0].pk_alg != X509_PK_RSA)
            return fail(c, TLS_E_PROTO, A_ILLEGAL_PARAMETER,
                        "the server signed with RSA but its certificate does "
                        "not hold an RSA key");
        if (rsa_pubkey_from_der(&key, c->chain[0].pk_bits,
                                c->chain[0].pk_bits_len) < 0)
            return fail(c, TLS_E_CERT, A_BAD_CERTIFICATE,
                        "the RSA public key in the server certificate is "
                        "malformed");
        if (key.bits < X509_MIN_RSA_BITS)
            return fail(c, TLS_E_CERT, A_INSUFFICIENT_SEC,
                        "the server's RSA key is %d bits, below the %d bit "
                        "minimum", key.bits, X509_MIN_RSA_BITS);
        /* RFC 8446 4.2.3: the salt length equals the digest length. */
        if (!rsa_verify_pss(&key, sig_md_id(alg), sig_md_id(alg),
                            (int)hash_digest_len(alg), digest,
                            hash_digest_len(alg), sig, siglen))
            return fail(c, TLS_E_CRYPTO, A_DECRYPT_ERROR,
                        "the server's handshake signature does not verify "
                        "against its certificate -- this connection is not "
                        "authentic");
        return TLS_OK;
    }
}

/* ===================================================================== *
 * The handshake                                                         *
 * ===================================================================== */

static int send_ccs(struct tls_conn *c)
{
    uint8_t one = 1;

    if (c->ccs_sent)
        return TLS_OK;
    c->ccs_sent = 1;
    /* Always plaintext, and never part of the transcript. */
    return rec_write(c, CT_CCS, &one, 1, 1);
}

static int expect_hs(struct tls_conn *c, int *type, const uint8_t **msg,
                     unsigned long *mlen, const uint8_t **body,
                     unsigned long *blen)
{
    int rc = hs_next(c, type, msg, mlen, body, blen);

    if (rc == TLS_E_CLOSED)
        return fail(c, TLS_E_TRUNCATED, -1,
                    "the server closed the connection in the middle of the "
                    "handshake");
    if (rc == TLS_E_TRUNCATED)
        return fail(c, TLS_E_TRUNCATED, -1,
                    "the connection was cut in the middle of the handshake "
                    "(after %s)",
                    c->rx_ready ? "the ServerHello" : "the ClientHello");
    if (rc == TLS_E_TIMEOUT)
        return fail(c, TLS_E_TIMEOUT, -1,
                    "the server stopped responding during the handshake "
                    "(nothing arrived for %d ms after %s)", c->opt.timeout_ms,
                    c->rx_ready ? "the ServerHello" : "the ClientHello");
    return rc;
}

static int do_handshake(struct tls_conn *c)
{
    int type, rc;
    const uint8_t *msg, *body;
    unsigned long mlen, blen;
    uint8_t shared[64];
    unsigned long sharedlen = 0;
    uint8_t th[HASH_MAX_DIGEST];
    uint8_t th_cert[HASH_MAX_DIGEST];
    uint8_t mac[HASH_MAX_DIGEST];
    struct sh_parsed sh;
    int saw_cert = 0, saw_cv = 0;
    int skipped = 0;

    drbg_bytes(c->client_random, sizeof(c->client_random));
    drbg_bytes(c->session_id, sizeof(c->session_id));

    hash_init(&c->tr256, HASH_SHA256);
    hash_init(&c->tr384, HASH_SHA384);

    rc = build_client_hello(c, 0);
    if (rc != TLS_OK)
        return rc;

    /* --- ServerHello, possibly preceded by a HelloRetryRequest --- */
    for (;;) {
        rc = expect_hs(c, &type, &msg, &mlen, &body, &blen);
        if (rc != TLS_OK)
            return rc;
        if (type != HS_SERVER_HELLO)
            return fail(c, TLS_E_PROTO, A_UNEXPECTED_MESSAGE,
                        "the server sent handshake message type %d where a "
                        "ServerHello was expected", type);
        rc = parse_server_hello(c, body, blen, &sh);
        if (rc != TLS_OK)
            return rc;

        c->suite = suite_by_id(sh.suite);
        if (!sh.is_hrr) {
            if (!c->tr_selected)
                tr_select(c, c->suite->hash);
            else if (c->suite->hash != c->tr.alg)
                return fail(c, TLS_E_PROTO, A_ILLEGAL_PARAMETER,
                            "the server changed cipher suite between its "
                            "HelloRetryRequest and its ServerHello");
            tr_update(c, msg, mlen);
            hs_consume(c);
            break;
        }

        /* HelloRetryRequest. */
        if (c->hrr_done)
            return fail(c, TLS_E_PROTO, A_UNEXPECTED_MESSAGE,
                        "the server sent a second HelloRetryRequest, which "
                        "RFC 8446 forbids");
        if (!sh.have_key_share)
            return fail(c, TLS_E_PROTO, A_MISSING_EXTENSION,
                        "the HelloRetryRequest does not say which key "
                        "exchange group it wants");
        {
            unsigned want = sh.group == GRP_X25519 ? TLS_G_X25519 :
                            sh.group == GRP_P256 ? TLS_G_P256 : 0;
            if (!want || !(c->opt.groups & want))
                return fail(c, TLS_E_PROTO, A_ILLEGAL_PARAMETER,
                            "the server asked for %s, which this client did "
                            "not offer", tls_group_name(sh.group));
            if (c->shares_sent & want)
                return fail(c, TLS_E_PROTO, A_ILLEGAL_PARAMETER,
                            "the server asked for %s in a HelloRetryRequest "
                            "although this client already sent a share for "
                            "it", tls_group_name(sh.group));
            if (sh.cookie_len) {
                if (sh.cookie_len > COOKIE_MAX)
                    return fail(c, TLS_E_PROTO, A_DECODE_ERROR,
                                "the server's cookie is %lu bytes, over the "
                                "%d byte limit", sh.cookie_len,
                                (int)COOKIE_MAX);
                memcpy(c->cookie, sh.cookie, sh.cookie_len);
                c->cookie_len = sh.cookie_len;
            }
            /* The transcript becomes message_hash || HRR || ClientHello2. */
            tr_select(c, c->suite->hash);
            transcript_hrr(c);
            tr_update(c, msg, mlen);
            hs_consume(c);
            c->hrr_done = 1;
            c->opt.key_shares = want;
            if (gen_share(c, sh.group) < 0)
                return fail(c, TLS_E_CRYPTO, -1,
                            "could not generate a %s key share",
                            tls_group_name(sh.group));
            send_ccs(c);
            rc = build_client_hello(c, 1);
            if (rc != TLS_OK)
                return rc;
        }
    }

    if (!sh.have_key_share)
        return fail(c, TLS_E_PROTO, A_MISSING_EXTENSION,
                    "the ServerHello has no key_share extension");
    c->group = sh.group;
    rc = do_key_exchange(c, sh.group, sh.ks, sh.kslen, shared, &sharedlen);
    if (rc != TLS_OK)
        return rc;

    if (ks_early(c) < 0 || ks_handshake(c, shared, sharedlen) < 0)
        return fail(c, TLS_E_CRYPTO, A_INTERNAL_ERROR,
                    "the key schedule failed");
    wipe(shared, sizeof(shared));
    if (install_keys(c, c->s_hs, 0) < 0 || install_keys(c, c->c_hs, 1) < 0)
        return fail(c, TLS_E_CRYPTO, A_INTERNAL_ERROR,
                    "the handshake traffic keys could not be installed");

    /* --- the server's encrypted flight --- */
    for (;;) {
        rc = expect_hs(c, &type, &msg, &mlen, &body, &blen);
        if (rc != TLS_OK)
            return rc;

        if (type == HS_ENCRYPTED_EXT) {
            rc = parse_encrypted_extensions(c, body, blen);
        } else if (type == HS_CERT_REQUEST) {
            rc = parse_cert_request(c, body, blen);
        } else if (type == HS_CERTIFICATE) {
            if (saw_cert)
                return fail(c, TLS_E_PROTO, A_UNEXPECTED_MESSAGE,
                            "the server sent two Certificate messages");
            rc = parse_certificate(c, body, blen);
            saw_cert = 1;
        } else if (type == HS_CERT_VERIFY) {
            if (!saw_cert)
                return fail(c, TLS_E_PROTO, A_UNEXPECTED_MESSAGE,
                            "the server sent a CertificateVerify before its "
                            "Certificate");
            tr_hash(c, th_cert);           /* ...through Certificate */
            rc = parse_certificate_verify(c, body, blen, th_cert);
            saw_cv = 1;
        } else if (type == HS_FINISHED) {
            if (!saw_cert || !saw_cv)
                return fail(c, TLS_E_PROTO, A_UNEXPECTED_MESSAGE,
                            "the server sent Finished without %s",
                            saw_cert ? "a CertificateVerify"
                                     : "a Certificate");
            tr_hash(c, th);                /* ...through CertificateVerify */
            if (finished_mac(c, c->s_hs, th, mac) < 0)
                return fail(c, TLS_E_CRYPTO, A_INTERNAL_ERROR,
                            "could not compute the Finished MAC");
            if (blen != hlen(c) || !ct_eq(mac, body, hlen(c)))
                return fail(c, TLS_E_CRYPTO, A_DECRYPT_ERROR,
                            "the server's Finished message is wrong, so the "
                            "handshake was tampered with or the two sides "
                            "computed different keys");
            tr_update(c, msg, mlen);
            hs_consume(c);
            break;
        } else if (type == HS_NEW_TICKET) {
            rc = TLS_OK;                   /* not resuming; ignore */
            if (++skipped > MAX_SKIPPED)
                return fail(c, TLS_E_PROTO, A_UNEXPECTED_MESSAGE,
                            "the server sent too many messages this client "
                            "does not use");
        } else {
            return fail(c, TLS_E_PROTO, A_UNEXPECTED_MESSAGE,
                        "the server sent handshake message type %d during "
                        "its flight, which does not belong there", type);
        }
        if (rc != TLS_OK)
            return rc;
        tr_update(c, msg, mlen);
        hs_consume(c);
    }

    /* Nothing may follow the server's Finished inside the same record:
     * what comes next is encrypted under a different key, so it has to be
     * a different record. Leaving stray bytes in the reassembly buffer
     * would also mean they were never accounted for. */
    if (c->hslen != 0)
        return fail(c, TLS_E_PROTO, A_UNEXPECTED_MESSAGE,
                    "the server put %lu more bytes after its Finished in "
                    "the same record", c->hslen);

    /* --- our flight --- */
    if (ks_master(c) < 0)
        return fail(c, TLS_E_CRYPTO, A_INTERNAL_ERROR,
                    "the application key schedule failed");

    send_ccs(c);

    if (c->cert_requested) {
        /* We have no client certificate. RFC 8446 4.4.2: answer with an
         * empty certificate_list rather than skipping the message. */
        struct wbuf w;
        wb_init(&w, c->scratch, SCRATCH_CAP);
        w8(&w, (unsigned)c->cr_context_len);
        wbytes(&w, c->cr_context, c->cr_context_len);
        w24(&w, 0);
        if (w.ovf)
            return fail(c, TLS_E_INVAL, A_INTERNAL_ERROR,
                        "internal error building the client Certificate");
        rc = hs_send(c, HS_CERTIFICATE, c->scratch,
                     (unsigned long)(w.p - c->scratch));
        if (rc != TLS_OK)
            return rc;
    }

    tr_hash(c, th);
    if (finished_mac(c, c->c_hs, th, mac) < 0)
        return fail(c, TLS_E_CRYPTO, A_INTERNAL_ERROR,
                    "could not compute the client Finished MAC");
    rc = hs_send(c, HS_FINISHED, mac, hlen(c));
    if (rc != TLS_OK)
        return rc;

    if (install_keys(c, c->s_ap, 0) < 0 || install_keys(c, c->c_ap, 1) < 0)
        return fail(c, TLS_E_CRYPTO, A_INTERNAL_ERROR,
                    "the application traffic keys could not be installed");
    c->handshake_done = 1;
    c->state = ST_READY;

    free(c->certbuf);
    c->certbuf = 0;
    return TLS_OK;
}

/* ===================================================================== *
 * Post-handshake messages                                               *
 * ===================================================================== */

static int handle_key_update(struct tls_conn *c, const uint8_t *body,
                             unsigned long blen)
{
    uint8_t next[HASH_MAX_DIGEST];
    int alg = c->suite->hash;
    unsigned long n = hash_digest_len(alg);
    int request;

    if (blen != 1)
        return fail(c, TLS_E_PROTO, A_DECODE_ERROR,
                    "the server sent a %lu byte KeyUpdate, not 1", blen);
    request = body[0];
    if (request > 1)
        return fail(c, TLS_E_PROTO, A_ILLEGAL_PARAMETER,
                    "the server sent KeyUpdate request %d, which is not 0 "
                    "or 1", request);

    /* application_traffic_secret_N+1 =
     *     HKDF-Expand-Label(secret_N, "traffic upd", "", Hash.length) */
    if (hkdf_expand_label(alg, c->s_ap, "traffic upd", 0, 0, next, n) < 0)
        return fail(c, TLS_E_CRYPTO, A_INTERNAL_ERROR,
                    "the receive key update failed");
    memcpy(c->s_ap, next, n);
    if (install_keys(c, c->s_ap, 0) < 0)
        return fail(c, TLS_E_CRYPTO, A_INTERNAL_ERROR,
                    "the new receive keys could not be installed");

    if (request == 1) {
        uint8_t zero = 0;
        int rc = hs_send(c, HS_KEY_UPDATE, &zero, 1);
        if (rc != TLS_OK)
            return rc;
        if (hkdf_expand_label(alg, c->c_ap, "traffic upd", 0, 0, next, n) < 0)
            return fail(c, TLS_E_CRYPTO, A_INTERNAL_ERROR,
                        "the send key update failed");
        memcpy(c->c_ap, next, n);
        if (install_keys(c, c->c_ap, 1) < 0)
            return fail(c, TLS_E_CRYPTO, A_INTERNAL_ERROR,
                        "the new send keys could not be installed");
    }
    wipe(next, sizeof(next));
    return TLS_OK;
}

/* Absorb the handshake record just read and act on every complete message
 * it completed. Several messages can share one record -- two session
 * tickets usually do -- and a message can also span records, so this
 * never blocks waiting for the rest of one: it stops and lets the caller
 * read another record. */
static int handle_post_handshake(struct tls_conn *c)
{
    int rc = hs_absorb(c);

    if (rc != TLS_OK)
        return rc;
    while (c->hslen >= 4) {
        unsigned long blen = ((unsigned long)c->hs[1] << 16) |
                             ((unsigned long)c->hs[2] << 8) | c->hs[3];
        const uint8_t *body = c->hs + 4;
        int type = c->hs[0];

        if (blen > TLS_HS_MAX)
            return fail(c, TLS_E_PROTO, A_DECODE_ERROR,
                        "the server sent a %lu byte post-handshake message, "
                        "over the %d byte limit", blen, (int)TLS_HS_MAX);
        if (c->hslen < 4 + blen)
            return TLS_OK;                 /* wait for the next record */
        c->hs_msglen = 4 + blen;

        switch (type) {
        case HS_NEW_TICKET:
            /* We do not resume, so a ticket is just bytes to drop. */
            rc = TLS_OK;
            break;
        case HS_KEY_UPDATE:
            rc = handle_key_update(c, body, blen);
            break;
        case HS_CERT_REQUEST:
            rc = fail(c, TLS_E_PROTO, A_UNEXPECTED_MESSAGE,
                      "the server asked for post-handshake client "
                      "authentication, which this client did not offer");
            break;
        default:
            rc = fail(c, TLS_E_PROTO, A_UNEXPECTED_MESSAGE,
                      "the server sent handshake message type %d after the "
                      "handshake finished", type);
            break;
        }
        hs_consume(c);
        if (rc != TLS_OK)
            return rc;
    }
    return TLS_OK;
}

int tls_key_update(struct tls_conn *c, int request_peer)
{
    uint8_t next[HASH_MAX_DIGEST];
    uint8_t b;
    int alg, rc;
    unsigned long n;

    if (!c || c->state != ST_READY)
        return TLS_E_INVAL;
    alg = c->suite->hash;
    n = hash_digest_len(alg);
    b = request_peer ? 1 : 0;
    rc = hs_send(c, HS_KEY_UPDATE, &b, 1);
    if (rc != TLS_OK)
        return rc;
    if (hkdf_expand_label(alg, c->c_ap, "traffic upd", 0, 0, next, n) < 0)
        return fail(c, TLS_E_CRYPTO, A_INTERNAL_ERROR,
                    "the send key update failed");
    memcpy(c->c_ap, next, n);
    wipe(next, sizeof(next));
    if (install_keys(c, c->c_ap, 1) < 0)
        return fail(c, TLS_E_CRYPTO, A_INTERNAL_ERROR,
                    "the new send keys could not be installed");
    return TLS_OK;
}

/* ===================================================================== *
 * Application data                                                      *
 * ===================================================================== */

int tls_read(struct tls_conn *c, void *buf, int len)
{
    if (!c || !buf || len <= 0)
        return TLS_E_INVAL;
    if (c->state == ST_FAILED)
        return c->err.code;

    for (;;) {
        int rc;

        if (c->pttype == CT_APPDATA && c->ptoff < c->ptlen) {
            int n = c->ptlen - c->ptoff;
            if (n > len)
                n = len;
            memcpy(buf, c->pt + c->ptoff, (unsigned long)n);
            c->ptoff += n;
            return n;
        }
        if (c->got_close_notify)
            return 0;
        if (c->state == ST_CLOSED)
            return 0;

        rc = rec_read(c);
        if (rc == TLS_E_CLOSED || rc == TLS_E_TRUNCATED) {
            c->state = ST_CLOSED;
            return fail(c, TLS_E_TRUNCATED, -1,
                        "the server closed the connection without sending "
                        "close_notify, so the data may be incomplete");
        }
        if (rc == TLS_E_TIMEOUT)
            return fail(c, TLS_E_TIMEOUT, -1,
                        "the server stopped sending (nothing arrived for "
                        "%d ms)", c->opt.timeout_ms);
        if (rc != TLS_OK)
            return rc;

        switch (c->pttype) {
        case CT_ALERT:
            rc = handle_alert(c);
            if (rc == TLS_E_CLOSED)
                return 0;
            if (rc != TLS_OK)
                return rc;
            break;
        case CT_HANDSHAKE:
            rc = handle_post_handshake(c);
            if (rc != TLS_OK)
                return rc;
            break;
        case CT_APPDATA:
            break;                          /* may be a zero-length record */
        default:
            return fail(c, TLS_E_PROTO, A_UNEXPECTED_MESSAGE,
                        "the server sent content type %d after the "
                        "handshake", c->pttype);
        }
    }
}

int tls_write(struct tls_conn *c, const void *buf, int len)
{
    int rc;

    if (!c || !buf || len < 0)
        return TLS_E_INVAL;
    if (c->state == ST_FAILED)
        return c->err.code;
    if (c->state != ST_READY)
        return fail(c, TLS_E_INVAL, -1,
                    "the connection is not open for writing");
    if (len == 0)
        return 0;
    rc = rec_write(c, CT_APPDATA, buf, (unsigned long)len, 0);
    if (rc != TLS_OK)
        return fail(c, rc, -1, "the connection failed while sending data");
    return len;
}

int tls_set_timeout(struct tls_conn *c, int ms)
{
    if (!c)
        return TLS_E_INVAL;
    if (!c->lower.set_timeout)
        return TLS_OK;
    return c->lower.set_timeout(c->lower.ctx, ms) < 0 ? TLS_E_IO : TLS_OK;
}

void tls_close(struct tls_conn *c)
{
    if (!c)
        return;
    if (c->state == ST_READY && !c->sent_close_notify) {
        uint8_t b[2];
        b[0] = AL_WARNING;
        b[1] = A_CLOSE_NOTIFY;
        c->sent_close_notify = 1;
        rec_write(c, CT_ALERT, b, 2, 0);
    }
    if (c->own_lower && c->lower.close)
        c->lower.close(c->lower.ctx);
    free(c->certbuf);
    /* Every secret in the connection dies here. */
    wipe(c->secret, sizeof(c->secret));
    wipe(c->c_hs, sizeof(c->c_hs));
    wipe(c->s_hs, sizeof(c->s_hs));
    wipe(c->c_ap, sizeof(c->c_ap));
    wipe(c->s_ap, sizeof(c->s_ap));
    wipe(c->x_priv, sizeof(c->x_priv));
    wipe(c->p_priv, sizeof(c->p_priv));
    wipe(&c->rx_aead, sizeof(c->rx_aead));
    wipe(&c->tx_aead, sizeof(c->tx_aead));
    free(c);
}

int tls_info(const struct tls_conn *c, struct tls_info *out)
{
    if (!c || !out)
        return TLS_E_INVAL;
    memset(out, 0, sizeof(*out));
    out->cipher = c->suite ? c->suite->name : "(none)";
    out->group = tls_group_name(c->group);
    memcpy(out->alpn, c->alpn, sizeof(out->alpn) < sizeof(c->alpn)
                               ? sizeof(out->alpn) : sizeof(c->alpn));
    out->alpn[sizeof(out->alpn) - 1] = 0;
    out->verified = c->verified;
    out->hello_retry = c->hrr_done;
    out->rsa_fallback = c->rsa_fallback;
    out->weak_entropy = g_weak_entropy;
    out->chain_len = c->chain_len;
    memcpy(out->subject, c->peer_subject, X509_MAX_CN);
    memcpy(out->issuer, c->peer_issuer, X509_MAX_CN);
    out->subject[X509_MAX_CN - 1] = 0;
    out->issuer[X509_MAX_CN - 1] = 0;
    out->not_before = c->peer_not_before;
    out->not_after = c->peer_not_after;
    snprintf(out->cert_error, sizeof(out->cert_error), "%s", c->cert_error);
    return TLS_OK;
}

/* ===================================================================== *
 * Options                                                               *
 * ===================================================================== */

void tls_options_default(struct tls_options *o)
{
    if (!o)
        return;
    memset(o, 0, sizeof(*o));
    o->verify = TLS_VERIFY_REQUIRED;
    o->alpn = "http/1.1";
    o->timeout_ms = 15000;
    o->groups = TLS_G_ALL;
    o->key_shares = TLS_G_ALL;
    o->suites = TLS_S_ALL;
}

static void options_fill(struct tls_options *o)
{
    if (o->timeout_ms <= 0)
        o->timeout_ms = 15000;
    if (o->groups == 0)
        o->groups = TLS_G_ALL;
    o->groups &= TLS_G_ALL;
    if (o->key_shares == 0)
        o->key_shares = o->groups;
    o->key_shares &= o->groups;
    if (o->suites == 0)
        o->suites = TLS_S_ALL;
    o->suites &= TLS_S_ALL;
}

/* ===================================================================== *
 * The trust store                                                       *
 * ===================================================================== */

#define ROOTS_FILE TLS_ROOTS_FILE
#define ROOTS_FILE_MAX (2UL * 1024UL * 1024UL)

static struct x509_store *g_store;
static int g_store_total, g_store_from_file;

static char *read_whole_file(const char *path, unsigned long *outlen)
{
    char *buf;
    unsigned long cap = 0, len = 0;

#ifdef TLS_HOST
    {
        FILE *f = fopen(path, "rb");
        if (!f)
            return 0;
        if (fseek(f, 0, SEEK_END) != 0) {
            fclose(f);
            return 0;
        }
        {
            long sz = ftell(f);
            if (sz < 0 || (unsigned long)sz > ROOTS_FILE_MAX) {
                fclose(f);
                return 0;
            }
            cap = (unsigned long)sz;
        }
        rewind(f);
        buf = malloc(cap + 1);
        if (!buf) {
            fclose(f);
            return 0;
        }
        len = (unsigned long)fread(buf, 1, cap, f);
        fclose(f);
    }
#else
    {
        int fd = open(path, O_RDONLY);
        long n;
        if (fd < 0)
            return 0;
        cap = 65536;
        buf = malloc(cap + 1);
        if (!buf) {
            close(fd);
            return 0;
        }
        for (;;) {
            if (len == cap) {
                char *nb;
                if (cap >= ROOTS_FILE_MAX)
                    break;
                cap *= 2;
                nb = realloc(buf, cap + 1);
                if (!nb) {
                    free(buf);
                    close(fd);
                    return 0;
                }
                buf = nb;
            }
            n = read(fd, buf + len, cap - len);
            if (n <= 0)
                break;
            len += (unsigned long)n;
        }
        close(fd);
    }
#endif
    buf[len] = 0;
    *outlen = len;
    return buf;
}

struct x509_store *tls_default_store(void)
{
    const char *pem;
    unsigned long pemlen;
    char *file;
    unsigned long filelen = 0;
    int n;

    if (g_store)
        return g_store;
    tls_register_digests();
    g_store = malloc(sizeof(*g_store));
    if (!g_store)
        return 0;
    x509_store_init(g_store);

    pem = tls_builtin_roots_pem(&pemlen);
    n = x509_store_add_pem(g_store, pem, pemlen);
    g_store_total = n > 0 ? n : 0;

    file = read_whole_file(ROOTS_FILE, &filelen);
    if (file) {
        int m = x509_store_add_pem(g_store, file, filelen);
        if (m > 0) {
            g_store_from_file = m;
            g_store_total += m;
        }
        free(file);
    }
    return g_store;
}

void tls_default_store_stat(int *total, int *from_file)
{
    if (!g_store)
        tls_default_store();
    if (total)
        *total = g_store ? g_store->n : 0;
    if (from_file)
        *from_file = g_store_from_file;
}

void tls_default_store_free(void)
{
    if (!g_store)
        return;
    x509_store_free(g_store);
    free(g_store);
    g_store = 0;
    g_store_total = 0;
    g_store_from_file = 0;
}

/* ===================================================================== *
 * The plain TCP layer under us                                          *
 * ===================================================================== */

struct sock_ctx {
    int fd;
    int timeout_ms;
};

#ifdef TLS_HOST

static int sock_read(void *ctx, void *buf, int len)
{
    struct sock_ctx *s = ctx;
    ssize_t n = recv(s->fd, buf, (size_t)len, 0);

    if (n == 0)
        return 0;
    if (n < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? TLS_E_TIMEOUT
                                                         : TLS_E_IO;
    return (int)n;
}

static int sock_write(void *ctx, const void *buf, int len)
{
    struct sock_ctx *s = ctx;
    ssize_t n = send(s->fd, buf, (size_t)len, 0);

    if (n <= 0)
        return TLS_E_IO;
    return (int)n;
}

static void sock_close(void *ctx)
{
    struct sock_ctx *s = ctx;

    close(s->fd);
    free(s);
}

static int sock_set_timeout(void *ctx, int ms)
{
    struct sock_ctx *s = ctx;
    struct timeval tv;

    if (ms <= 0)
        ms = 1;
    s->timeout_ms = ms;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(s->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return 0;
}

static int tcp_open(const char *host, int port, int timeout_ms,
                    struct tls_transport *out)
{
    struct addrinfo hints, *res = 0, *ai;
    char portstr[16];
    int fd = -1;
    struct sock_ctx *s;

    snprintf(portstr, sizeof(portstr), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
        return TLS_E_DNS;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        return TLS_E_CONNECT;
    s = malloc(sizeof(*s));
    if (!s) {
        close(fd);
        return TLS_E_NOMEM;
    }
    s->fd = fd;
    s->timeout_ms = timeout_ms;
    out->ctx = s;
    out->read = sock_read;
    out->write = sock_write;
    out->close = sock_close;
    out->set_timeout = sock_set_timeout;
    sock_set_timeout(s, timeout_ms);
    {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    return TLS_OK;
}

#else

static int sock_read(void *ctx, void *buf, int len)
{
    struct sock_ctx *s = ctx;
    long n = syscall(SYS_TCP_RECV, s->fd, (long)buf, (long)len,
                     s->timeout_ms);

    if (n < 0)
        return TLS_E_TIMEOUT;
    return (int)n;                          /* 0 means the peer closed */
}

static int sock_write(void *ctx, const void *buf, int len)
{
    struct sock_ctx *s = ctx;
    long n;

    /* The kernel stages TCP payloads through a 1400-byte buffer. */
    if (len > 1400)
        len = 1400;
    n = syscall(SYS_TCP_SEND, s->fd, (long)buf, (long)len, 0);
    if (n <= 0)
        return TLS_E_IO;
    return (int)n;
}

static void sock_close(void *ctx)
{
    struct sock_ctx *s = ctx;

    syscall(SYS_TCP_CLOSE, s->fd, 0, 0, 0);
    free(s);
}

static int sock_set_timeout(void *ctx, int ms)
{
    struct sock_ctx *s = ctx;

    s->timeout_ms = ms > 0 ? ms : 1;
    return 0;
}

static int tcp_open(const char *host, int port, int timeout_ms,
                    struct tls_transport *out)
{
    struct sock_ctx *s;
    uint32_t ip;
    int handle;

    ip = ip_aton(host);
    if (ip == 0 && dns_resolve(host, &ip) < 0)
        return TLS_E_DNS;
    if (ip == 0)
        return TLS_E_DNS;
    handle = (int)syscall(SYS_TCP_CONNECT, (long)ip, port, timeout_ms, 0);
    if (handle < 0)
        return TLS_E_CONNECT;
    s = malloc(sizeof(*s));
    if (!s) {
        syscall(SYS_TCP_CLOSE, handle, 0, 0, 0);
        return TLS_E_NOMEM;
    }
    s->fd = handle;
    s->timeout_ms = timeout_ms;
    out->ctx = s;
    out->read = sock_read;
    out->write = sock_write;
    out->close = sock_close;
    out->set_timeout = sock_set_timeout;
    return TLS_OK;
}

#endif /* TLS_HOST */

/* ===================================================================== *
 * Opening a connection                                                  *
 * ===================================================================== */

static void copy_error(const struct tls_conn *c, struct tls_error *err)
{
    if (!err)
        return;
    *err = c->err;
}

static struct tls_conn *client_run(const struct tls_transport *lower,
                                   const char *host,
                                   const struct tls_options *opt,
                                   struct tls_error *err, int *want_rsa);

struct tls_conn *tls_client(const struct tls_transport *lower,
                            const char *host,
                            const struct tls_options *opt,
                            struct tls_error *err)
{
    int want_rsa = 0;

    return client_run(lower, host, opt, err, &want_rsa);
}

static struct tls_conn *client_run(const struct tls_transport *lower,
                                   const char *host,
                                   const struct tls_options *opt,
                                   struct tls_error *err, int *want_rsa)
{
    struct tls_conn *c;
    int rc;

    if (err) {
        memset(err, 0, sizeof(*err));
        err->alert = -1;
    }
    if (!lower || !lower->read || !lower->write || !host || !host[0]) {
        if (err) {
            err->code = TLS_E_INVAL;
            snprintf(err->msg, sizeof(err->msg), "invalid arguments to "
                     "tls_client()");
        }
        return 0;
    }
    if (strlen(host) >= sizeof(c->host)) {
        if (err) {
            err->code = TLS_E_INVAL;
            snprintf(err->msg, sizeof(err->msg),
                     "the host name is longer than %d bytes",
                     (int)sizeof(c->host) - 1);
        }
        return 0;
    }

    tls_register_digests();
    drbg_reseed();

    c = calloc(1, sizeof(*c));
    if (!c) {
        if (err) {
            err->code = TLS_E_NOMEM;
            snprintf(err->msg, sizeof(err->msg),
                     "out of memory for a %lu byte TLS connection",
                     (unsigned long)sizeof(*c));
        }
        return 0;
    }
    c->lower = *lower;
    c->own_lower = 0;
    c->err.alert = -1;
    strcpy(c->host, host);
    if (opt)
        c->opt = *opt;
    else
        tls_options_default(&c->opt);
    options_fill(&c->opt);
    c->now = c->opt.now ? c->opt.now : tls_now_unix();
    c->roots = c->opt.roots ? c->opt.roots : tls_default_store();
    if (!c->roots && c->opt.verify == TLS_VERIFY_REQUIRED) {
        if (err) {
            err->code = TLS_E_CERT;
            err->cert_failure = 1;
            snprintf(err->msg, sizeof(err->msg),
                     "no trust store could be built, so no certificate can "
                     "be verified");
        }
        free(c);
        return 0;
    }
    if (c->lower.set_timeout)
        c->lower.set_timeout(c->lower.ctx, c->opt.timeout_ms);

    c->state = ST_HANDSHAKE;
    rc = do_handshake(c);
    if (rc != TLS_OK) {
        copy_error(c, err);
        *want_rsa = c->want_rsa_retry;
        free(c->certbuf);
        free(c);
        return 0;
    }
    c->own_lower = 1;
    return c;
}

/* One TCP connection plus one handshake. */
static struct tls_conn *connect_once(const char *host, int port,
                                     const struct tls_options *opt,
                                     struct tls_error *err, int *want_rsa)
{
    struct tls_transport lower;
    struct tls_conn *c;
    int timeout = opt->timeout_ms > 0 ? opt->timeout_ms : 15000;
    int rc;

    memset(&lower, 0, sizeof(lower));
    rc = tcp_open(host, port, timeout, &lower);
    if (rc != TLS_OK) {
        if (err) {
            err->code = rc;
            err->alert = -1;
            snprintf(err->msg, sizeof(err->msg),
                     rc == TLS_E_DNS
                     ? "'%s' did not resolve to an address"
                     : "could not open a connection to '%s' port %d", host,
                     port);
        }
        return 0;
    }
    c = client_run(&lower, host, opt, err, want_rsa);
    if (!c) {
        lower.close(lower.ctx);
        return 0;
    }
    return c;
}

struct tls_conn *tls_connect(const char *host, int port,
                             const struct tls_options *opt,
                             struct tls_error *err)
{
    struct tls_options o;
    struct tls_conn *c;
    int want_rsa = 0;

    if (err) {
        memset(err, 0, sizeof(*err));
        err->alert = -1;
    }
    if (!host || port <= 0 || port > 65535) {
        if (err) {
            err->code = TLS_E_INVAL;
            snprintf(err->msg, sizeof(err->msg), "invalid host or port");
        }
        return 0;
    }
    if (opt)
        o = *opt;
    else
        tls_options_default(&o);
    options_fill(&o);

    c = connect_once(host, port, &o, err, &want_rsa);
    if (c)
        return c;

    /* The chain held a key on a curve this build cannot verify. Ask again
     * without offering ECDSA at all: a server that has an RSA certificate
     * as well will send that chain instead. See the note in tls.h. */
    if (want_rsa && !o.rsa_only && !o.no_rsa_fallback &&
        o.verify == TLS_VERIFY_REQUIRED) {
        struct tls_error err2;
        int again = 0;
        o.rsa_only = 1;
        c = connect_once(host, port, &o, &err2, &again);
        if (c) {
            c->rsa_fallback = 1;
            return c;
        }
        /* The retry's own failure is usually noise -- a server with only
         * an ECDSA certificate answers an RSA-only ClientHello with
         * handshake_failure, which explains nothing. The first diagnosis
         * is the useful one, so *err keeps it. */
        (void)err2;
    }
    return 0;
}

/* ===================================================================== *
 * The libweb transport factory                                          *
 * ===================================================================== */

static char g_transport_err[TLS_ERR_LEN];

const char *tls_last_transport_error(void)
{
    return g_transport_err[0] ? g_transport_err : "no error";
}

static int tp_read(void *ctx, void *buf, int len)
{
    return tls_read(ctx, buf, len);
}

static int tp_write(void *ctx, const void *buf, int len)
{
    return tls_write(ctx, buf, len);
}

static void tp_close(void *ctx)
{
    tls_close(ctx);
}

static int tp_set_timeout(void *ctx, int ms)
{
    return tls_set_timeout(ctx, ms);
}

#ifdef TLS_HOST
/* Test hook, host build only: hand the traffic secrets to
 * tools/test_tls.c so it can compare them against the ones openssl
 * s_server wrote to its -keylogfile. That comparison is the only check in
 * the suite that can catch the client and a mock server agreeing on the
 * *same wrong* key schedule, so it is worth the hole -- and the hole does
 * not exist in the target build, where this whole block is compiled out. */
unsigned long tls_test_conn_size(void)
{
    return sizeof(struct tls_conn);
}

void tls_test_secrets(const struct tls_conn *c, uint8_t *client_random,
                      uint8_t *chs, uint8_t *shs, uint8_t *cap,
                      uint8_t *sap, int *len)
{
    unsigned long n = hash_digest_len(c->suite->hash);

    memcpy(client_random, c->client_random, 32);
    memcpy(chs, c->c_hs, n);
    memcpy(shs, c->s_hs, n);
    memcpy(cap, c->c_ap, n);
    memcpy(sap, c->s_ap, n);
    *len = (int)n;
}
#endif

int tls_transport_open(const char *host, int port, int timeout_ms,
                       void *user, struct tls_transport *out)
{
    const struct tls_options *opt = user;
    struct tls_options local;
    struct tls_error err;
    struct tls_conn *c;

    if (!host || !out)
        return -1;                          /* HTTP_E_INVAL */
    if (opt)
        local = *opt;
    else
        tls_options_default(&local);
    if (timeout_ms > 0)
        local.timeout_ms = timeout_ms;

    c = tls_connect(host, port, &local, &err);
    if (!c) {
        snprintf(g_transport_err, sizeof(g_transport_err), "%s", err.msg);
        /* Mapped onto libweb's codes: HTTP_E_DNS is -4 and HTTP_E_CONNECT
         * is -5. The precise reason survives in
         * tls_last_transport_error(), because http_fetch() only passes the
         * code back. */
        return err.code == TLS_E_DNS ? -4 : -5;
    }
    g_transport_err[0] = 0;
    out->ctx = c;
    out->read = tp_read;
    out->write = tp_write;
    out->close = tp_close;
    out->set_timeout = tp_set_timeout;
    return 0;                               /* HTTP_OK */
}
