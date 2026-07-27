#pragma once

/* KestrelOS libtls: a TLS 1.3 client (RFC 8446).
 *
 * Client only, TLS 1.3 only. There is no TLS 1.2 fallback -- that would
 * mean RSA key transport, CBC-with-MAC-then-encrypt, the PRF and
 * renegotiation, which is more machinery than the rest of this file and
 * buys almost nothing in 2026. A server that will not do 1.3 is detected
 * and reported precisely rather than being quietly worked around.
 *
 * What is implemented:
 *   - the full 1-RTT handshake: ClientHello (server_name, supported_versions,
 *     supported_groups, key_share, signature_algorithms, ALPN),
 *     HelloRetryRequest with cookie echo, ServerHello, EncryptedExtensions,
 *     CertificateRequest (answered with an empty Certificate),
 *     Certificate, CertificateVerify, Finished, client Finished;
 *   - the key schedule of section 7.1, verified against a real server;
 *   - the record layer of section 5, including the content-type-inside-the-
 *     plaintext trick, padding removal, records split across TCP reads,
 *     several records in one read, and post-handshake KeyUpdate;
 *   - TLS_AES_128_GCM_SHA256, TLS_AES_256_GCM_SHA384 and
 *     TLS_CHACHA20_POLY1305_SHA256 over X25519 and P-256;
 *   - certificate chain verification through x509.h against a trust store,
 *     with the hostname checked, and a *specific* reason on failure.
 *
 * What is not:
 *   - session resumption, PSK, 0-RTT, session tickets (they are parsed and
 *     discarded), client certificates, OCSP stapling, CRLs;
 *   - Ed25519 and P-384/P-521 signatures, because ecc.h implements X25519
 *     and P-256 only. A chain anchored in a P-384 root therefore fails
 *     verification with a message that says exactly that.
 *
 * Not audited. See the honest-ceiling section of docs/BROWSER-PLAN.md and
 * docs/tls.md.
 *
 * Memory, measured rather than guessed: one connection is a single heap
 * block of 113,776 bytes -- three 16 KiB record buffers, a 48 KiB
 * handshake reassembly buffer and a ten-certificate chain -- plus a
 * short-lived copy of the server's certificate message that is freed when
 * the handshake ends. The trust store is another 49,672 bytes and is also
 * on the heap. Nothing here recurses, and the deepest stack frame is
 * x509's chain verification at about 14 KiB, against a 64 KiB user stack.
 */

#include <stdint.h>
#include "x509.h"

/* ---- result codes ---------------------------------------------------- */

#define TLS_OK            0
#define TLS_E_INVAL      -1   /* bad argument */
#define TLS_E_DNS        -2   /* the host name did not resolve */
#define TLS_E_CONNECT    -3   /* TCP connect failed or timed out */
#define TLS_E_IO         -4   /* the transport failed */
#define TLS_E_TIMEOUT    -5   /* a deadline expired */
#define TLS_E_PROTO      -6   /* the peer sent something malformed */
#define TLS_E_ALERT      -7   /* the peer sent a fatal alert */
#define TLS_E_VERSION    -8   /* the peer will not speak TLS 1.3 */
#define TLS_E_CERT       -9   /* the certificate chain is not trusted */
#define TLS_E_CRYPTO    -10   /* a key exchange or signature check failed */
#define TLS_E_NOMEM     -11
#define TLS_E_CLOSED    -12   /* the peer closed cleanly (close_notify) */
#define TLS_E_TRUNCATED -13   /* the connection died without close_notify */
#define TLS_E_UNSUPPORTED -14 /* a feature this build does not implement */

#define TLS_ERR_LEN 256

struct tls_error {
    int  code;                /* TLS_E_* */
    int  alert;               /* alert description sent or received, else -1 */
    int  alert_received;      /* 1 = the alert came from the peer */
    int  cert_failure;        /* 1 = chain verification is what failed, so a
                               * browser may offer "proceed anyway" */
    char msg[TLS_ERR_LEN];    /* one sentence, for a human */
};

/* ---- options --------------------------------------------------------- */

#define TLS_VERIFY_REQUIRED 0   /* the default: a bad chain aborts */
#define TLS_VERIFY_NONE     1   /* connect anyway, but record why it failed */

/* Named groups, as a bitmask of what to offer. */
#define TLS_G_X25519 0x01
#define TLS_G_P256   0x02
#define TLS_G_ALL    (TLS_G_X25519 | TLS_G_P256)

/* Cipher suites, likewise. */
#define TLS_S_CHACHA20  0x01
#define TLS_S_AES128GCM 0x02
#define TLS_S_AES256GCM 0x04
#define TLS_S_ALL       (TLS_S_CHACHA20 | TLS_S_AES128GCM | TLS_S_AES256GCM)

struct tls_options {
    const struct x509_store *roots; /* NULL = tls_default_store()          */
    int          verify;            /* TLS_VERIFY_*                        */
    const char  *alpn;              /* "http/1.1", or several separated by
                                     * commas; NULL sends no ALPN          */
    const char  *sni;               /* NULL = the host argument            */
    int          timeout_ms;        /* per I/O operation; 0 = 15000        */
    unsigned     groups;            /* supported_groups; 0 = TLS_G_ALL     */
    unsigned     key_shares;        /* groups to send a share for; 0 = all
                                     * of `groups`. Offering a group with
                                     * no share is what triggers a
                                     * HelloRetryRequest.                  */
    unsigned     suites;            /* 0 = TLS_S_ALL                       */
    uint32_t     now;               /* unix time for validity checks;
                                     * 0 = ask the operating system        */
    int          rsa_only;          /* offer only RSA signature algorithms,
                                     * so a server with both an ECDSA and
                                     * an RSA certificate serves the RSA
                                     * one -- see no_rsa_fallback          */
    int          no_rsa_fallback;   /* 1 disables the automatic retry
                                     * described below                     */
};

/* The automatic RSA fallback, and why it exists.
 *
 * ecc.h implements P-256 but not P-384, so a chain with a P-384 key
 * anywhere in it cannot be verified. That is not a rare corner: several
 * large CAs -- SSL.com, Google Trust Services, Let's Encrypt's ECDSA
 * hierarchy -- issue ECDSA leaves under P-384 intermediates, and a server
 * picks which of its certificates to send based on the client's
 * signature_algorithms.
 *
 * So when tls_connect() fails verification and the chain contained a curve
 * this build cannot verify, it reconnects once with the ECDSA algorithms
 * removed from signature_algorithms. Servers that hold both certificate
 * types then send the RSA chain, which does verify. Nothing is weakened:
 * the second attempt is a full handshake with full verification, and if it
 * also fails the error reported is the second one. Set no_rsa_fallback to
 * turn it off; tls_info().rsa_fallback says whether it happened. */

void tls_options_default(struct tls_options *o);

/* ---- transport ------------------------------------------------------- */

/* Structurally identical to libweb's `struct http_transport`, declared
 * here so that libtls does not have to include libweb and invert the
 * layering. tools/test_tls.c asserts the two layouts agree, and
 * TLS_ASSERT_TRANSPORT_LAYOUT below lets any translation unit that
 * includes both headers check it at compile time.
 *
 *   read()  > 0 bytes, 0 for a clean end of stream, < 0 on error.
 *   write() the number of bytes accepted (> 0), or < 0; short writes are
 *           fine, the caller loops.
 *   close() releases everything including ctx.
 *   set_timeout() may be NULL. */
struct tls_transport {
    void *ctx;
    int (*read)(void *ctx, void *buf, int len);
    int (*write)(void *ctx, const void *buf, int len);
    void (*close)(void *ctx);
    int (*set_timeout)(void *ctx, int ms);
};

#define TLS_ASSERT_TRANSPORT_LAYOUT()                                        \
    typedef char tls_transport_layout_check[                                 \
        (sizeof(struct tls_transport) == sizeof(struct http_transport) &&    \
         __builtin_offsetof(struct tls_transport, read) ==                   \
             __builtin_offsetof(struct http_transport, read) &&              \
         __builtin_offsetof(struct tls_transport, write) ==                  \
             __builtin_offsetof(struct http_transport, write) &&             \
         __builtin_offsetof(struct tls_transport, close) ==                  \
             __builtin_offsetof(struct http_transport, close) &&             \
         __builtin_offsetof(struct tls_transport, set_timeout) ==            \
             __builtin_offsetof(struct http_transport, set_timeout))         \
        ? 1 : -1]

/* ---- connections ----------------------------------------------------- */

struct tls_conn;

/* Resolve `host`, open a TCP connection to `port`, and run the handshake.
 * Returns NULL on failure with *err filled in (err may be NULL). */
struct tls_conn *tls_connect(const char *host, int port,
                             const struct tls_options *opt,
                             struct tls_error *err);

/* The same, over a transport the caller already opened -- any byte pipe
 * will do, which is what the test harness and a future proxy use. On
 * success the connection owns `lower` and will close it; on failure
 * `lower` is left open and untouched, so the caller still owns it. */
struct tls_conn *tls_client(const struct tls_transport *lower,
                            const char *host,
                            const struct tls_options *opt,
                            struct tls_error *err);

/* > 0 bytes, 0 at a clean end of stream (the peer sent close_notify), or a
 * negative TLS_E_*. TLS_E_TRUNCATED means the connection was cut without
 * a close_notify, which the caller should treat as suspicious rather than
 * as an ordinary end of file. */
int tls_read(struct tls_conn *c, void *buf, int len);

/* Writes all `len` bytes. Returns len, or a negative TLS_E_*. */
int tls_write(struct tls_conn *c, const void *buf, int len);

/* Sends close_notify (best effort), closes the transport, frees c. */
void tls_close(struct tls_conn *c);

int tls_set_timeout(struct tls_conn *c, int ms);

/* The message for the most recent failure on this connection. */
const char *tls_conn_error(const struct tls_conn *c);

/* Ask for a key update. `request_peer` also asks the server to rekey its
 * own sending direction. Returns TLS_OK or a negative code. */
int tls_key_update(struct tls_conn *c, int request_peer);

/* ---- what was negotiated --------------------------------------------- */

struct tls_info {
    const char *cipher;             /* "TLS_AES_128_GCM_SHA256"      */
    const char *group;              /* "x25519" / "secp256r1"        */
    char        alpn[32];           /* "" when none was selected     */
    int         verified;           /* 1 = the chain is trusted      */
    int         hello_retry;        /* 1 = a HelloRetryRequest happened */
    int         rsa_fallback;       /* 1 = connected on the RSA retry */
    int         weak_entropy;       /* 1 = no hardware RNG was found */
    int         chain_len;
    char        subject[X509_MAX_CN];
    char        issuer[X509_MAX_CN];
    uint32_t    not_before;
    uint32_t    not_after;
    char        cert_error[X509_ERR_LEN];  /* why verification failed, when
                                            * it did and verify was NONE  */
};

int tls_info(const struct tls_conn *c, struct tls_info *out);

/* ---- trust store ----------------------------------------------------- */

/* Extra roots are read from here when the file exists. */
#define TLS_ROOTS_FILE "/etc/ssl/roots.pem"

/* The process-wide store: the roots compiled into roots.c, plus every
 * certificate in /etc/ssl/roots.pem when that file exists. Built on first
 * use; NULL if it could not be built (out of memory). */
struct x509_store *tls_default_store(void);

/* How many roots the default store holds, and how many came from the
 * file rather than the binary. Either pointer may be NULL. */
void tls_default_store_stat(int *total, int *from_file);

void tls_default_store_free(void);

/* roots.c: the compiled-in PEM bundle, and its inventory. */
const char *tls_builtin_roots_pem(unsigned long *len);
int         tls_builtin_root_count(void);
const char *tls_builtin_root_name(int i);   /* NULL when i is out of range */

/* ---- entropy --------------------------------------------------------- */

/* Stir extra entropy into the DRBG. The browser should feed it mouse and
 * key timings; it is never harmful and the on-target sources are thin. */
void tls_add_entropy(const void *data, unsigned long len);

/* 1 when no hardware random source was found and the DRBG is seeded only
 * from timers and addresses. Callers that care should say so out loud. */
int tls_entropy_is_weak(void);

/* ---- plugging into libweb's HTTP client ------------------------------ *
 *
 * A transport factory with exactly the shape http_transport_fn wants.
 * `user` is a `const struct tls_options *` (NULL for the defaults) and
 * must outlive every connection made through it.
 *
 * libtls deliberately does not call http_register_scheme() itself -- that
 * would make every program linking libtls also need libweb. The
 * registration is a macro, so the reference to http_register_scheme
 * appears only in the translation unit that uses it:
 *
 *     #include "http.h"
 *     #include "tls.h"
 *     TLS_REGISTER_HTTPS(&my_tls_options);
 */
int tls_transport_open(const char *host, int port, int timeout_ms,
                       void *user, struct tls_transport *out);

/* http_fetch() can only pass a numeric code back, so the sentence
 * explaining the most recent tls_transport_open() failure is kept here for
 * the browser to show. */
const char *tls_last_transport_error(void);

#define TLS_REGISTER_HTTPS(opts)                                             \
    http_register_scheme("https",                                            \
        (http_transport_fn)(void *)tls_transport_open, (void *)(opts))

/* ---- diagnostics ----------------------------------------------------- */

const char *tls_error_text(int code);       /* TLS_E_* -> a short phrase */
const char *tls_alert_text(int desc);       /* alert description -> text */
const char *tls_suite_name(int id);         /* 0x1301 -> "TLS_AES_..."   */
const char *tls_group_name(int id);         /* 0x001d -> "x25519"        */
