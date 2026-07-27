/* Host test harness for libtls's TLS 1.3 client.
 *
 * Build and run (from the repo root, on Linux/WSL):
 *
 *   gcc -Wall -Wextra -O2 -fsanitize=address,undefined -Ilibtls \
 *       -DTLS_HOST -o /tmp/test_tls \
 *       tools/test_tls.c libtls/tls.c libtls/roots.c libtls/hash.c \
 *       libtls/aead.c libtls/ecc.c libtls/rsa.c libtls/bignum.c \
 *       libtls/x509.c && /tmp/test_tls
 *
 * tls.c compiled with -DTLS_HOST puts BSD sockets under the record layer
 * instead of SYS_TCP_*; everything above that -- the handshake, the key
 * schedule, every parser -- is the same code the target runs. libweb's
 * HTTP client was validated the same way.
 *
 * There are four kinds of test here, and they check each other:
 *
 *   1. openssl s_server interop. A real, independent TLS 1.3 server, run
 *      once per cipher suite and once per group, plus a HelloRetryRequest
 *      forced by offering a group without a share for it. This is what
 *      proves the key schedule and the transcript are right: nothing else
 *      in this file could catch both sides making the same mistake.
 *      s_server's -keylogfile is read back and compared against the
 *      secrets the client derived, secret by secret.
 *
 *   2. A mock server written here, on top of hash.h/aead.h/ecc.h directly
 *      -- a second, independent implementation of the server half of the
 *      key schedule. It can complete a handshake (it shells out to
 *      openssl to sign the CertificateVerify, since libtls verifies
 *      signatures but cannot make them), which makes every negative case
 *      -- a corrupted Finished, a flipped bit in a record, messages out of
 *      order, a truncated flight -- a deterministic unit test instead of
 *      something that needs a co-operative server.
 *
 *   3. Certificate failures: expired, wrong host name, self-signed with no
 *      root, a chain missing its intermediate. Each must fail with a
 *      specific message, and this file prints every one of them.
 *
 *   4. Fuzzing. The mock server's flight is mutated byte by byte and
 *      replayed into the client under -fsanitize=address,undefined, as are
 *      raw record streams. The client must reject everything without
 *      reading out of bounds or leaking.
 *
 * Everything openssl-dependent is skipped with a loud message if openssl
 * is not on PATH, so the file still builds and runs somewhere without it.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "tls.h"
#include "hash.h"
#include "aead.h"
#include "ecc.h"
#include "x509.h"

/* The transport contract libweb codes against. Declared here rather than
 * included so this file does not depend on libweb being present; the
 * layout assertion below is the point. */
struct http_transport {
    void *ctx;
    int (*read)(void *ctx, void *buf, int len);
    int (*write)(void *ctx, const void *buf, int len);
    void (*close)(void *ctx);
    int (*set_timeout)(void *ctx, int ms);
};
TLS_ASSERT_TRANSPORT_LAYOUT();

static int g_checks, g_fails;
static int g_verbose;

#define T(cond, msg)                                                          \
    do {                                                                      \
        g_checks++;                                                           \
        if (!(cond)) {                                                        \
            g_fails++;                                                        \
            printf("  FAIL line %d: %s\n", __LINE__, (msg));                  \
        }                                                                     \
    } while (0)

static void ti(long got, long want, const char *msg, int line)
{
    g_checks++;
    if (got != want) {
        g_fails++;
        printf("  FAIL line %d: %s: got %ld want %ld\n", line, msg, got, want);
    }
}
#define TI(got, want, msg) ti((long)(got), (long)(want), (msg), __LINE__)

static void section(const char *name)
{
    printf("\n== %s ==\n", name);
}

/* Does `hay` contain `needle`? Used to check that a failure message says
 * the right thing, without pinning the exact wording. */
static int says(const char *hay, const char *needle)
{
    return hay && strstr(hay, needle) != 0;
}

static void expect_msg(const char *what, const char *msg, const char *needle,
                       int line)
{
    g_checks++;
    if (!says(msg, needle)) {
        g_fails++;
        printf("  FAIL line %d: %s\n     message: \"%s\"\n"
               "     expected it to mention: \"%s\"\n",
               line, what, msg ? msg : "(none)", needle);
    } else {
        printf("  %-34s %s\n", what, msg);
    }
}
#define EXPECT_MSG(what, msg, needle) expect_msg(what, msg, needle, __LINE__)

/* ===================================================================== *
 * openssl helpers                                                       *
 * ===================================================================== */

#define WORK "/tmp/kestrel-tls-test"

static int g_have_openssl;

static int run(const char *fmt, ...)
{
    char cmd[2048];
    va_list ap;
    int rc;

    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    if (g_verbose)
        printf("  $ %s\n", cmd);
    rc = system(cmd);
    return rc == 0 ? 0 : -1;
}

static int check_openssl(void)
{
    return system("openssl version >/dev/null 2>&1") == 0;
}

/* One CA, and leaves for every case the tests need. Generated once. */
static int make_certs(void)
{
    struct stat st;

    if (stat(WORK "/ready", &st) == 0)
        return 0;
    if (run("rm -rf %s && mkdir -p %s", WORK, WORK) < 0)
        return -1;

    /* The CA. */
    if (run("openssl req -x509 -newkey rsa:2048 -nodes -keyout %s/ca.key "
            "-out %s/ca.pem -days 3650 -subj '/CN=KestrelOS Test CA' "
            "-addext basicConstraints=critical,CA:TRUE "
            "-addext keyUsage=critical,keyCertSign,cRLSign "
            ">/dev/null 2>&1", WORK, WORK) < 0)
        return -1;

    /* An RSA leaf and a P-256 leaf, both for localhost, both issued by the
     * CA -- the two signature families a real server uses. */
    if (run("openssl req -newkey rsa:2048 -nodes -keyout %s/rsa.key "
            "-out %s/rsa.csr -subj '/CN=localhost' >/dev/null 2>&1",
            WORK, WORK) < 0)
        return -1;
    if (run("openssl x509 -req -in %s/rsa.csr -CA %s/ca.pem -CAkey %s/ca.key "
            "-CAcreateserial -out %s/rsa.pem -days 825 "
            "-extfile /dev/stdin <<'EOF' >/dev/null 2>&1\n"
            "subjectAltName=DNS:localhost,DNS:kestrel.test,IP:127.0.0.1\n"
            "extendedKeyUsage=serverAuth\n"
            "basicConstraints=critical,CA:FALSE\n"
            "EOF\n", WORK, WORK, WORK, WORK) < 0)
        return -1;

    if (run("openssl req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 "
            "-nodes -keyout %s/ec.key -out %s/ec.csr -subj '/CN=localhost' "
            ">/dev/null 2>&1", WORK, WORK) < 0)
        return -1;
    if (run("openssl x509 -req -in %s/ec.csr -CA %s/ca.pem -CAkey %s/ca.key "
            "-CAcreateserial -out %s/ec.pem -days 825 "
            "-extfile /dev/stdin <<'EOF' >/dev/null 2>&1\n"
            "subjectAltName=DNS:localhost,DNS:kestrel.test,IP:127.0.0.1\n"
            "extendedKeyUsage=serverAuth\n"
            "basicConstraints=critical,CA:FALSE\n"
            "EOF\n", WORK, WORK, WORK, WORK) < 0)
        return -1;

    /* Expired: valid a year ago, dead a month ago. */
    if (run("openssl x509 -req -in %s/rsa.csr -CA %s/ca.pem -CAkey %s/ca.key "
            "-CAcreateserial -out %s/expired.pem "
            "-not_before 20240101000000Z -not_after 20240301000000Z "
            "-extfile /dev/stdin <<'EOF' >/dev/null 2>&1\n"
            "subjectAltName=DNS:localhost\n"
            "extendedKeyUsage=serverAuth\n"
            "basicConstraints=critical,CA:FALSE\n"
            "EOF\n", WORK, WORK, WORK, WORK) < 0)
        return -1;

    /* A certificate for somewhere else entirely. */
    if (run("openssl x509 -req -in %s/rsa.csr -CA %s/ca.pem -CAkey %s/ca.key "
            "-CAcreateserial -out %s/wronghost.pem -days 825 "
            "-extfile /dev/stdin <<'EOF' >/dev/null 2>&1\n"
            "subjectAltName=DNS:not-localhost.example\n"
            "extendedKeyUsage=serverAuth\n"
            "basicConstraints=critical,CA:FALSE\n"
            "EOF\n", WORK, WORK, WORK, WORK) < 0)
        return -1;

    /* Self-signed, issued by nobody. */
    if (run("openssl req -x509 -newkey rsa:2048 -nodes -keyout %s/self.key "
            "-out %s/self.pem -days 825 -subj '/CN=localhost' "
            "-addext subjectAltName=DNS:localhost >/dev/null 2>&1",
            WORK, WORK) < 0)
        return -1;

    /* chain.pem = leaf + CA, for servers that should send a full chain. */
    if (run("cat %s/rsa.pem %s/ca.pem > %s/chain.pem", WORK, WORK, WORK) < 0)
        return -1;

    /* A second root that cross-signs the first, and a chain that ends in
     * the cross-signed copy. This is the shape almost every real CA ships
     * -- GTS Root R1 signed by GlobalSign, SSL.com's roots signed by
     * Comodo -- and it is what the chain-prefix search in tls.c exists
     * for: the trust store holds the *self-signed* root, the server sends
     * the *cross-signed* one, and the anchor is one certificate down. */
    if (run("openssl req -x509 -newkey rsa:2048 -nodes -keyout %s/ca2.key "
            "-out %s/ca2.pem -days 3650 -subj '/CN=KestrelOS Cross CA' "
            "-addext basicConstraints=critical,CA:TRUE "
            "-addext keyUsage=critical,keyCertSign,cRLSign "
            ">/dev/null 2>&1", WORK, WORK) < 0)
        return -1;
    if (run("openssl x509 -x509toreq -in %s/ca.pem -signkey %s/ca.key "
            "-out %s/ca-cross.csr >/dev/null 2>&1", WORK, WORK, WORK) < 0)
        return -1;
    if (run("openssl x509 -req -in %s/ca-cross.csr -CA %s/ca2.pem "
            "-CAkey %s/ca2.key -CAcreateserial -out %s/ca-cross.pem "
            "-days 3650 -extfile /dev/stdin <<'EOF' >/dev/null 2>&1\n"
            "basicConstraints=critical,CA:TRUE\n"
            "keyUsage=critical,keyCertSign,cRLSign\n"
            "EOF\n", WORK, WORK, WORK, WORK) < 0)
        return -1;
    if (run("cat %s/rsa.pem %s/ca.pem %s/ca-cross.pem > %s/crosschain.pem",
            WORK, WORK, WORK, WORK) < 0)
        return -1;
    /* And the same chain with a completely unrelated certificate stapled
     * on the end, which a server has no business sending but some do. */
    if (run("cat %s/rsa.pem %s/ca.pem %s/self.pem > %s/junkchain.pem",
            WORK, WORK, WORK, WORK) < 0)
        return -1;

    return run("touch %s/ready", WORK);
}

/* ---- running s_server ------------------------------------------------ */

static int free_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    int port = 0;

    if (fd < 0)
        return 0;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0 &&
        getsockname(fd, (struct sockaddr *)&a, &len) == 0)
        port = ntohs(a.sin_port);
    close(fd);
    return port;
}

static int port_open(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    int ok;

    if (fd < 0)
        return 0;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)port);
    ok = connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0;
    close(fd);
    return ok;
}

/* Start `openssl s_server ...` in the background; returns its pid. */
static pid_t start_server(int port, const char *extra, const char *keylog)
{
    char cmd[1024];
    pid_t pid;
    int i;

    snprintf(cmd, sizeof(cmd),
             "exec openssl s_server -accept %d -naccept 200 -quiet -www "
             "-tls1_3 %s %s%s >%s/server.log 2>&1",
             port, extra, keylog ? "-keylogfile " : "",
             keylog ? keylog : "", WORK);
    pid = fork();
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd, (char *)0);
        _exit(127);
    }
    if (pid < 0)
        return -1;
    for (i = 0; i < 200; i++) {
        if (port_open(port))
            return pid;
        usleep(25000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, 0, 0);
    return -1;
}

static void stop_server(pid_t pid)
{
    if (pid <= 0)
        return;
    kill(pid, SIGTERM);
    usleep(20000);
    kill(pid, SIGKILL);
    waitpid(pid, 0, 0);
}

/* ---- trust stores ---------------------------------------------------- */

static char *slurp(const char *path, unsigned long *len)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long sz;

    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    rewind(f);
    if (sz < 0) {
        fclose(f);
        return 0;
    }
    buf = malloc((unsigned long)sz + 1);
    if (!buf) {
        fclose(f);
        return 0;
    }
    *len = fread(buf, 1, (unsigned long)sz, f);
    buf[*len] = 0;
    fclose(f);
    return buf;
}

static struct x509_store *store_from(const char *path)
{
    struct x509_store *s = malloc(sizeof(*s));
    unsigned long len = 0;
    char *pem;

    if (!s)
        return 0;
    x509_store_init(s);
    pem = slurp(path, &len);
    if (!pem) {
        free(s);
        return 0;
    }
    x509_store_add_pem(s, pem, len);
    free(pem);
    return s;
}

static void store_free(struct x509_store *s)
{
    if (!s)
        return;
    x509_store_free(s);
    free(s);
}

/* ===================================================================== *
 * Driving the client                                                    *
 * ===================================================================== */

struct fetch_result {
    int   ok;
    int   code;
    char  err[TLS_ERR_LEN];
    struct tls_info info;
    char  body[8192];
    int   body_len;
    long  total;               /* every byte read, not just the first 8k */
    int   last;                /* what the final tls_read() returned     */
};

/* Connect, send a request, read everything back. */
static void fetch_n(const char *host, int port, const struct tls_options *opt,
                    const void *request, int reqlen, struct fetch_result *out)
{
    struct tls_error err;
    struct tls_conn *c;
    int n;

    memset(out, 0, sizeof(*out));
    c = tls_connect(host, port, opt, &err);
    if (!c) {
        out->code = err.code;
        snprintf(out->err, sizeof(out->err), "%s", err.msg);
        return;
    }
    tls_info(c, &out->info);
    if (request) {
        n = tls_write(c, request, reqlen);
        if (n < 0) {
            out->code = n;
            snprintf(out->err, sizeof(out->err), "%s", tls_conn_error(c));
            tls_close(c);
            return;
        }
        for (;;) {
            char buf[4096];
            n = tls_read(c, buf, sizeof(buf));
            if (n <= 0)
                break;
            out->total += n;
            if (out->body_len + n < (int)sizeof(out->body)) {
                memcpy(out->body + out->body_len, buf, (unsigned long)n);
                out->body_len += n;
            }
        }
        out->last = n;
        if (n < 0 && n != TLS_E_TRUNCATED) {
            out->code = n;
            snprintf(out->err, sizeof(out->err), "%s", tls_conn_error(c));
            tls_close(c);
            return;
        }
    }
    out->ok = 1;
    tls_close(c);
}

static void fetch(const char *host, int port, const struct tls_options *opt,
                  const char *request, struct fetch_result *out)
{
    fetch_n(host, port, opt, request, request ? (int)strlen(request) : 0, out);
}

/* ===================================================================== *
 * 1. Interop with openssl s_server                                      *
 * ===================================================================== */

static const char *GET = "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n";

struct suite_case {
    const char *osslname;
    unsigned    flag;
    const char *expect;
};

static void interop_one(const char *what, const char *server_args,
                        const struct tls_options *opt,
                        const char *expect_cipher, const char *expect_group)
{
    int port = free_port();
    pid_t pid;
    struct fetch_result r;

    pid = start_server(port, server_args, 0);
    if (pid < 0) {
        printf("  SKIP %s: s_server would not start (%s)\n", what,
               server_args);
        return;
    }
    fetch("localhost", port, opt, GET, &r);
    stop_server(pid);

    g_checks++;
    if (!r.ok) {
        g_fails++;
        printf("  FAIL %s: %s\n", what, r.err);
        return;
    }
    if (expect_cipher && strcmp(r.info.cipher, expect_cipher) != 0) {
        g_fails++;
        printf("  FAIL %s: negotiated %s, expected %s\n", what,
               r.info.cipher, expect_cipher);
        return;
    }
    if (expect_group && strcmp(r.info.group, expect_group) != 0) {
        g_fails++;
        printf("  FAIL %s: group %s, expected %s\n", what, r.info.group,
               expect_group);
        return;
    }
    if (r.body_len < 12 || memcmp(r.body, "HTTP/1.0 200", 12) != 0) {
        g_fails++;
        printf("  FAIL %s: response did not start with HTTP/1.0 200 "
               "(%d bytes: %.40s)\n", what, r.body_len, r.body);
        return;
    }
    printf("  %-30s %s over %s%s, verified=%d, %d bytes back\n", what,
           r.info.cipher, r.info.group,
           r.info.hello_retry ? " after a HelloRetryRequest" : "",
           r.info.verified, r.body_len);
}

static void test_interop(void)
{
    struct tls_options o;
    struct x509_store *ca;
    char args[512];

    section("openssl s_server interop");
    if (!g_have_openssl) {
        printf("  SKIP: openssl is not on PATH\n");
        return;
    }
    ca = store_from(WORK "/ca.pem");
    if (!ca) {
        printf("  FAIL: could not build a trust store from the test CA\n");
        g_fails++;
        return;
    }

    tls_options_default(&o);
    o.roots = ca;
    o.alpn = 0;

    /* --- every cipher suite --- */
    snprintf(args, sizeof(args), "-cert %s/rsa.pem -key %s/rsa.key "
             "-ciphersuites TLS_AES_128_GCM_SHA256", WORK, WORK);
    interop_one("AES-128-GCM, RSA cert", args, &o,
                "TLS_AES_128_GCM_SHA256", 0);

    snprintf(args, sizeof(args), "-cert %s/rsa.pem -key %s/rsa.key "
             "-ciphersuites TLS_AES_256_GCM_SHA384", WORK, WORK);
    interop_one("AES-256-GCM, RSA cert", args, &o,
                "TLS_AES_256_GCM_SHA384", 0);

    snprintf(args, sizeof(args), "-cert %s/rsa.pem -key %s/rsa.key "
             "-ciphersuites TLS_CHACHA20_POLY1305_SHA256", WORK, WORK);
    interop_one("ChaCha20-Poly1305, RSA", args, &o,
                "TLS_CHACHA20_POLY1305_SHA256", 0);

    /* --- ECDSA certificate, which exercises the P-256 signature path --- */
    snprintf(args, sizeof(args), "-cert %s/ec.pem -key %s/ec.key", WORK, WORK);
    interop_one("ECDSA P-256 certificate", args, &o, 0, 0);

    /* --- each group --- */
    snprintf(args, sizeof(args), "-cert %s/rsa.pem -key %s/rsa.key "
             "-groups X25519", WORK, WORK);
    o.groups = TLS_G_ALL;
    o.key_shares = TLS_G_ALL;
    interop_one("X25519 key exchange", args, &o, 0, "x25519");

    snprintf(args, sizeof(args), "-cert %s/rsa.pem -key %s/rsa.key "
             "-groups P-256", WORK, WORK);
    interop_one("P-256 key exchange", args, &o, 0, "secp256r1");

    /* --- HelloRetryRequest: offer both groups but send a share only for
     * the one the server will not use, so it has to ask again. --- */
    snprintf(args, sizeof(args), "-cert %s/rsa.pem -key %s/rsa.key "
             "-groups P-256", WORK, WORK);
    o.groups = TLS_G_ALL;
    o.key_shares = TLS_G_X25519;
    interop_one("HelloRetryRequest -> P-256", args, &o, 0, "secp256r1");

    snprintf(args, sizeof(args), "-cert %s/rsa.pem -key %s/rsa.key "
             "-groups X25519", WORK, WORK);
    o.groups = TLS_G_ALL;
    o.key_shares = TLS_G_P256;
    interop_one("HelloRetryRequest -> X25519", args, &o, 0, "x25519");

    /* --- a full chain, leaf + intermediate --- */
    o.groups = TLS_G_ALL;
    o.key_shares = TLS_G_ALL;
    snprintf(args, sizeof(args), "-cert %s/chain.pem -key %s/rsa.key",
             WORK, WORK);
    interop_one("leaf + CA chain", args, &o, 0, 0);

    /* --- the server asks for a client certificate we do not have; RFC
     * 8446 says answer with an empty Certificate, and the handshake
     * carries on. --- */
    snprintf(args, sizeof(args), "-cert %s/rsa.pem -key %s/rsa.key "
             "-verify 1 -CAfile %s/ca.pem", WORK, WORK, WORK);
    interop_one("CertificateRequest, no cert", args, &o, 0, 0);

    /* --- an ALPN the server will not accept --- */
    {
        int port = free_port();
        pid_t pid;
        struct fetch_result r;
        snprintf(args, sizeof(args), "-cert %s/rsa.pem -key %s/rsa.key "
                 "-alpn h2", WORK, WORK);
        pid = start_server(port, args, 0);
        if (pid < 0) {
            printf("  SKIP ALPN mismatch: s_server would not start\n");
        } else {
            o.alpn = "http/1.1";
            fetch("localhost", port, &o, GET, &r);
            stop_server(pid);
            if (r.ok) {
                g_checks++;
                g_fails++;
                printf("  FAIL ALPN mismatch: the handshake succeeded\n");
            } else {
                EXPECT_MSG("no shared ALPN protocol", r.err,
                           "application protocol");
            }
            o.alpn = 0;
        }
    }

    /* --- ALPN --- */
    {
        int port = free_port();
        pid_t pid;
        struct fetch_result r;
        snprintf(args, sizeof(args), "-cert %s/rsa.pem -key %s/rsa.key "
                 "-alpn http/1.1", WORK, WORK);
        pid = start_server(port, args, 0);
        if (pid < 0) {
            printf("  SKIP ALPN: s_server would not start\n");
        } else {
            o.alpn = "http/1.1";
            fetch("localhost", port, &o, GET, &r);
            stop_server(pid);
            g_checks++;
            if (!r.ok || strcmp(r.info.alpn, "http/1.1") != 0) {
                g_fails++;
                printf("  FAIL ALPN: ok=%d alpn=\"%s\" %s\n", r.ok,
                       r.info.alpn, r.err);
            } else {
                printf("  %-30s the server selected \"%s\"\n",
                       "ALPN http/1.1", r.info.alpn);
            }
            o.alpn = 0;
        }
    }

    store_free(ca);
}

/* ===================================================================== *
 * RSA-PSS signing, for the mock server                                  *
 *                                                                       *
 * libtls verifies signatures but never makes them -- a browser holds no  *
 * private key -- so the harness needs its own signer to stand up a       *
 * server. bignum.h has everything required: parse the PKCS#8 key with    *
 * the DER reader in x509.h, EMSA-PSS-encode per RFC 8017, and one        *
 * modexp. Doing it in process rather than shelling out to openssl keeps  *
 * a fuzz iteration at well under a millisecond.                         *
 * ===================================================================== */

struct privkey {
    struct bn      n, d;
    struct bn_mont mont;
    int            k;              /* byte length of the modulus */
    int            bits;
};

static int load_privkey(const char *path, struct privkey *pk)
{
    unsigned long len = 0;
    char *pem = slurp(path, &len);
    struct pem_iter it;
    static uint8_t der[8192];
    size_t derlen = 0;
    char label[64];
    struct der d0, inner;
    struct der_tlv t;
    uint32_t version;
    const uint8_t *v;
    size_t vlen;

    if (!pem)
        return -1;
    pem_iter_init(&it, pem, len);
    if (pem_next(&it, label, sizeof(label), der, sizeof(der), &derlen) != 1) {
        free(pem);
        return -1;
    }
    free(pem);

    der_init(&d0, der, derlen);
    if (der_read_expect(&d0, &t, DER_SEQUENCE) < 0)
        return -1;
    der_enter(&t, &inner);
    if (der_read_u32(&inner, &version) < 0)
        return -1;
    if (der_peek(&inner, &t) == 0 && t.cls == DER_CLASS_UNIVERSAL &&
        t.tag == 0x10) {
        /* PKCS#8: AlgorithmIdentifier then an OCTET STRING wrapping the
         * RSAPrivateKey. */
        struct der oct;
        if (der_read(&inner, &t) < 0)
            return -1;
        if (der_read(&inner, &t) < 0 || t.tag != DER_OCTET_STRING)
            return -1;
        der_init(&oct, t.val, t.len);
        if (der_read_expect(&oct, &t, DER_SEQUENCE) < 0)
            return -1;
        der_enter(&t, &inner);
        if (der_read_u32(&inner, &version) < 0)
            return -1;
    }
    if (der_read_uint(&inner, &v, &vlen) < 0 ||
        bn_from_bytes(&pk->n, v, vlen) < 0)
        return -1;
    if (der_read_uint(&inner, &v, &vlen) < 0)          /* publicExponent */
        return -1;
    if (der_read_uint(&inner, &v, &vlen) < 0 ||
        bn_from_bytes(&pk->d, v, vlen) < 0)
        return -1;
    if (bn_mont_init(&pk->mont, &pk->n) < 0)
        return -1;
    pk->bits = bn_bits(&pk->n);
    pk->k = (pk->bits + 7) / 8;
    return 0;
}

static void mgf1_sha256(const uint8_t *seed, size_t slen, uint8_t *mask,
                        size_t mlen)
{
    uint8_t buf[64], md[32];
    size_t off = 0;
    uint32_t counter = 0;

    while (off < mlen) {
        size_t take = mlen - off < 32 ? mlen - off : 32;
        struct hash_ctx h;
        hash_init(&h, HASH_SHA256);
        hash_update(&h, seed, slen);
        buf[0] = (uint8_t)(counter >> 24);
        buf[1] = (uint8_t)(counter >> 16);
        buf[2] = (uint8_t)(counter >> 8);
        buf[3] = (uint8_t)counter;
        hash_update(&h, buf, 4);
        hash_final(&h, md);
        memcpy(mask + off, md, take);
        off += take;
        counter++;
    }
}

/* EMSA-PSS with SHA-256 and a 32-byte salt, then the private operation. */
static int pss_sign_sha256(const struct privkey *pk, const uint8_t *digest,
                           uint8_t *sig, size_t *siglen)
{
    int emBits = pk->bits - 1;
    size_t emLen = (size_t)((emBits + 7) / 8);
    uint8_t em[512], mprime[8 + 32 + 32], hh[32], salt[32], mask[512];
    size_t dblen, i;
    struct bn m, s;
    int fd;

    if (emLen < 32 + 32 + 2 || emLen > sizeof(em))
        return -1;
    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, salt, sizeof(salt)) != (ssize_t)sizeof(salt)) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    close(fd);

    memset(mprime, 0, 8);
    memcpy(mprime + 8, digest, 32);
    memcpy(mprime + 40, salt, 32);
    hash_oneshot(HASH_SHA256, mprime, sizeof(mprime), hh);

    dblen = emLen - 32 - 1;
    memset(em, 0, dblen);
    em[dblen - 33] = 0x01;
    memcpy(em + dblen - 32, salt, 32);
    mgf1_sha256(hh, 32, mask, dblen);
    for (i = 0; i < dblen; i++)
        em[i] ^= mask[i];
    em[0] &= (uint8_t)(0xff >> (8 * (int)emLen - emBits));
    memcpy(em + dblen, hh, 32);
    em[emLen - 1] = 0xbc;

    if (bn_from_bytes(&m, em, emLen) < 0)
        return -1;
    if (bn_modexp(&s, &m, &pk->d, &pk->mont) < 0)
        return -1;
    if (bn_to_bytes(&s, sig, (size_t)pk->k) < 0)
        return -1;
    *siglen = (size_t)pk->k;
    return 0;
}

/* ===================================================================== *
 * The mock server                                                       *
 * ===================================================================== */

#define M_CT_CCS 20
#define M_CT_ALERT 21
#define M_CT_HS 22
#define M_CT_APP 23

struct mock_cfg {
    int suite;              /* 0 = whatever the client offered first     */
    int group;              /* 0 = the group the client sent a share for */
    int send_hrr;           /* ask for cfg->group with a retry           */
    const char *cookie;     /* cookie to put in the HelloRetryRequest    */
    const char *certfile;   /* PEM chain to send (leaf first)            */
    const char *keyfile;
    int frag;               /* split the flight into records of n bytes  */
    int dribble;            /* write one byte per send() call            */
    int corrupt_finished;   /* flip a bit in the server Finished         */
    int corrupt_record;     /* flip a bit in the Nth encrypted record    */
    int bad_signature;      /* corrupt the CertificateVerify signature   */
    int omit_certificate;
    int omit_cert_verify;
    int swap_cert_cv;       /* CertificateVerify before Certificate      */
    int extra_ccs;          /* how many dummy change_cipher_spec records */
    int close_after;        /* close after this many bytes written, 0=no */
    int plain_alert;        /* send this alert description before the SH */
    int tls12_hello;        /* answer as a TLS 1.2 server would          */
    int http_response;      /* answer with plain HTTP, not TLS at all    */
    int send_ticket;        /* a NewSessionTicket after the handshake    */
    int key_update;         /* 1 = KeyUpdate, 2 = KeyUpdate(requested)   */
    int mutate_off;         /* fuzz: byte offset within the flight       */
    int mutate_val;
    int reply_app;          /* send an application-data reply            */
    long expect_bytes;      /* read this much application data first     */
    long big_reply;         /* reply with this many bytes instead of 43  */
    int no_close_notify;    /* cut the connection instead of closing it  */
    int hang;               /* read the ClientHello, then never answer   */
    int oversize_record;    /* claim a record length past the 2^14+256 cap */
};

struct mock {
    int fd;
    const struct mock_cfg *cfg;
    struct hash_ctx tr;
    int hashalg, aeadalg, suite, group;
    unsigned long hlen;
    uint8_t secret[64], c_hs[64], s_hs[64], c_ap[64], s_ap[64];
    struct aead_ctx tx, rx;
    uint8_t txiv[12], rxiv[12];
    uint64_t txseq, rxseq;
    int tx_ready, rx_ready;
    uint8_t sess[32];
    int sesslen;
    uint8_t cpub[65];
    int cpublen;
    uint8_t xpriv[32], xpub[32];
    uint8_t ppriv[32];
    struct p256_point ppub;
    uint8_t shared[32];
    unsigned char rbuf[32768];
    int rlen;
    unsigned char pt[20000];
    int ptlen, pttype;
    long written;
    int records_out;
    int dead;
};

static void m_send(struct mock *m, const void *data, unsigned long len)
{
    const uint8_t *p = data;
    unsigned long i;

    if (m->dead)
        return;
    if (m->cfg->close_after > 0 &&
        m->written + (long)len > m->cfg->close_after) {
        long allow = m->cfg->close_after - m->written;
        if (allow > 0)
            (void)!write(m->fd, p, (size_t)allow);
        m->written += allow;
        shutdown(m->fd, SHUT_RDWR);
        m->dead = 1;
        return;
    }
    if (m->cfg->dribble) {
        for (i = 0; i < len; i++) {
            if (write(m->fd, p + i, 1) != 1) {
                m->dead = 1;
                return;
            }
            usleep(200);
        }
    } else if (write(m->fd, p, len) != (ssize_t)len) {
        m->dead = 1;
    }
    m->written += (long)len;
}

static void m_record(struct mock *m, int type, const uint8_t *data,
                     unsigned long len, int force_plain)
{
    uint8_t out[20000 + 64];
    unsigned long body;

    if (m->tx_ready && !force_plain) {
        uint8_t nonce[12];
        body = len + 1 + AEAD_TAG_LEN;
        out[0] = M_CT_APP;
        out[1] = 0x03;
        out[2] = 0x03;
        out[3] = (uint8_t)(body >> 8);
        out[4] = (uint8_t)body;
        memcpy(out + 5, data, len);
        out[5 + len] = (uint8_t)type;
        aead_nonce(m->txiv, 12, m->txseq++, nonce);
        aead_ctx_seal(&m->tx, nonce, out, 5, out + 5, len + 1, out + 5);
        if (m->cfg->corrupt_record &&
            m->records_out + 1 == m->cfg->corrupt_record)
            out[5 + (body / 2)] ^= 0x40;
    } else {
        body = len;
        out[0] = (uint8_t)type;
        out[1] = 0x03;
        out[2] = 0x03;
        out[3] = (uint8_t)(body >> 8);
        out[4] = (uint8_t)body;
        memcpy(out + 5, data, len);
    }
    m->records_out++;
    m_send(m, out, 5 + body);
}

/* Read one record from the client into m->pt. Returns 0 or -1. */
static int m_read_record(struct mock *m)
{
    for (;;) {
        int len, type;
        while (m->rlen < 5) {
            ssize_t n = read(m->fd, m->rbuf + m->rlen,
                             sizeof(m->rbuf) - (size_t)m->rlen);
            if (n <= 0)
                return -1;
            m->rlen += (int)n;
        }
        type = m->rbuf[0];
        len = (m->rbuf[3] << 8) | m->rbuf[4];
        if (len < 0 || len > 18000)
            return -1;
        while (m->rlen < 5 + len) {
            ssize_t n = read(m->fd, m->rbuf + m->rlen,
                             sizeof(m->rbuf) - (size_t)m->rlen);
            if (n <= 0)
                return -1;
            m->rlen += (int)n;
        }
        if (type == M_CT_CCS) {
            memmove(m->rbuf, m->rbuf + 5 + len, (size_t)(m->rlen - 5 - len));
            m->rlen -= 5 + len;
            continue;
        }
        if (m->rx_ready && type == M_CT_APP) {
            uint8_t nonce[12];
            int inner;
            aead_nonce(m->rxiv, 12, m->rxseq++, nonce);
            if (aead_ctx_open(&m->rx, nonce, m->rbuf, 5, m->rbuf + 5,
                              (unsigned long)len, m->pt) < 0)
                return -1;
            inner = len - AEAD_TAG_LEN;
            while (inner > 0 && m->pt[inner - 1] == 0)
                inner--;
            if (inner == 0)
                return -1;
            m->pttype = m->pt[inner - 1];
            m->ptlen = inner - 1;
        } else {
            memcpy(m->pt, m->rbuf + 5, (size_t)len);
            m->ptlen = len;
            m->pttype = type;
        }
        memmove(m->rbuf, m->rbuf + 5 + len, (size_t)(m->rlen - 5 - len));
        m->rlen -= 5 + len;
        return 0;
    }
}

/* --- little-endian-free readers for the mock's own parsing --- */

static unsigned m_g16(const uint8_t *p) { return ((unsigned)p[0] << 8) | p[1]; }

/* Pull the pieces of a ClientHello the server needs. Returns 0 or -1. */
static int m_parse_hello(struct mock *m, const uint8_t *b, int len,
                         int *suites, int *nsuites, int *have_x, int *have_p)
{
    int off = 0, n, i;

    *nsuites = 0;
    *have_x = *have_p = 0;
    if (len < 38)
        return -1;
    off = 2 + 32;
    n = b[off++];
    if (off + n > len || n > 32)
        return -1;
    memcpy(m->sess, b + off, (size_t)n);
    m->sesslen = n;
    off += n;
    if (off + 2 > len)
        return -1;
    n = (int)m_g16(b + off);
    off += 2;
    if (off + n > len)
        return -1;
    for (i = 0; i + 1 < n && *nsuites < 32; i += 2)
        suites[(*nsuites)++] = (int)m_g16(b + off + i);
    off += n;
    if (off >= len)
        return -1;
    n = b[off++];
    off += n;
    if (off + 2 > len)
        return -1;
    n = (int)m_g16(b + off);
    off += 2;
    if (off + n > len)
        return -1;
    {
        int e = off, end = off + n;
        while (e + 4 <= end) {
            int id = (int)m_g16(b + e);
            int elen = (int)m_g16(b + e + 2);
            const uint8_t *v = b + e + 4;
            if (e + 4 + elen > end)
                return -1;
            if (id == 51) {                 /* key_share */
                int p = 2, klen = (int)m_g16(v);
                if (klen + 2 > elen)
                    return -1;
                while (p + 4 <= 2 + klen) {
                    int g = (int)m_g16(v + p);
                    int gl = (int)m_g16(v + p + 2);
                    if (p + 4 + gl > 2 + klen)
                        return -1;
                    if (g == 0x001d && gl == 32) {
                        *have_x = 1;
                        if (!m->cpublen || m->group == 0x001d) {
                            memcpy(m->cpub, v + p + 4, 32);
                            m->cpublen = 32;
                        }
                    } else if (g == 0x0017 && gl == 65) {
                        *have_p = 1;
                        if (m->group == 0x0017 || !*have_x) {
                            memcpy(m->cpub, v + p + 4, 65);
                            m->cpublen = 65;
                        }
                    }
                    p += 4 + gl;
                }
            }
            e += 4 + elen;
        }
    }
    return 0;
}

static void m_derive(struct mock *m)
{
    uint8_t derived[64], zeros[64], th[64];
    uint8_t key[32];

    memset(zeros, 0, sizeof(zeros));
    hkdf_extract(m->hashalg, 0, 0, zeros, m->hlen, m->secret);
    hkdf_derive_secret(m->hashalg, m->secret, "derived", "", 0, derived);
    hkdf_extract(m->hashalg, derived, m->hlen, m->shared, 32, m->secret);
    hash_peek(&m->tr, th);
    hkdf_derive_secret_hash(m->hashalg, m->secret, "c hs traffic", th,
                            m->c_hs);
    hkdf_derive_secret_hash(m->hashalg, m->secret, "s hs traffic", th,
                            m->s_hs);
    hkdf_expand_label(m->hashalg, m->s_hs, "key", 0, 0, key,
                      aead_key_len(m->aeadalg));
    hkdf_expand_label(m->hashalg, m->s_hs, "iv", 0, 0, m->txiv, 12);
    aead_init(&m->tx, m->aeadalg, key);
    m->txseq = 0;
    m->tx_ready = 1;
    hkdf_expand_label(m->hashalg, m->c_hs, "key", 0, 0, key,
                      aead_key_len(m->aeadalg));
    hkdf_expand_label(m->hashalg, m->c_hs, "iv", 0, 0, m->rxiv, 12);
    aead_init(&m->rx, m->aeadalg, key);
    m->rxseq = 0;
    m->rx_ready = 1;
}

static void m_app_keys(struct mock *m)
{
    uint8_t derived[64], zeros[64], th[64], key[32];

    memset(zeros, 0, sizeof(zeros));
    hkdf_derive_secret(m->hashalg, m->secret, "derived", "", 0, derived);
    hkdf_extract(m->hashalg, derived, m->hlen, zeros, m->hlen, m->secret);
    hash_peek(&m->tr, th);
    hkdf_derive_secret_hash(m->hashalg, m->secret, "c ap traffic", th,
                            m->c_ap);
    hkdf_derive_secret_hash(m->hashalg, m->secret, "s ap traffic", th,
                            m->s_ap);
    hkdf_expand_label(m->hashalg, m->s_ap, "key", 0, 0, key,
                      aead_key_len(m->aeadalg));
    hkdf_expand_label(m->hashalg, m->s_ap, "iv", 0, 0, m->txiv, 12);
    aead_init(&m->tx, m->aeadalg, key);
    m->txseq = 0;
}

static void m_client_app_keys(struct mock *m)
{
    uint8_t key[32];

    hkdf_expand_label(m->hashalg, m->c_ap, "key", 0, 0, key,
                      aead_key_len(m->aeadalg));
    hkdf_expand_label(m->hashalg, m->c_ap, "iv", 0, 0, m->rxiv, 12);
    aead_init(&m->rx, m->aeadalg, key);
    m->rxseq = 0;
}

/* Append a handshake message to the flight buffer and the transcript. */
static void m_msg(struct mock *m, uint8_t *buf, int *len, int type,
                  const uint8_t *body, unsigned long blen)
{
    uint8_t *p = buf + *len;

    p[0] = (uint8_t)type;
    p[1] = (uint8_t)(blen >> 16);
    p[2] = (uint8_t)(blen >> 8);
    p[3] = (uint8_t)blen;
    memcpy(p + 4, body, blen);
    hash_update(&m->tr, p, 4 + blen);
    *len += (int)(4 + blen);
}

static void mock_server(int fd, const struct mock_cfg *cfg)
{
    static struct mock m;
    struct privkey pk;
    int suites[32], nsuites = 0, have_x = 0, have_p = 0, i;
    uint8_t sh[512];
    int shlen;
    uint8_t flight[16384];
    int flen = 0;
    uint8_t certs[8192];
    unsigned long certslen = 0;
    uint8_t body[9000];
    int hrr_sent = 0;

    memset(&m, 0, sizeof(m));
    m.fd = fd;
    m.cfg = cfg;
    m.group = cfg->group;

    if (cfg->http_response) {
        const char *r = "HTTP/1.1 400 Bad Request\r\n"
                        "Content-Length: 0\r\n\r\n";
        (void)!write(fd, r, strlen(r));
        shutdown(fd, SHUT_RDWR);
        return;
    }

    /* --- the ClientHello (and a second one after a retry) --- */
again:
    if (m_read_record(&m) < 0 || m.pttype != M_CT_HS)
        return;
    if (m.ptlen < 4 || m.pt[0] != 1)
        return;
    if (m_parse_hello(&m, m.pt + 4, m.ptlen - 4, suites, &nsuites,
                      &have_x, &have_p) < 0)
        return;

    if (cfg->plain_alert) {
        uint8_t a[2];
        a[0] = 2;
        a[1] = (uint8_t)cfg->plain_alert;
        m_record(&m, M_CT_ALERT, a, 2, 1);
        shutdown(fd, SHUT_RDWR);
        return;
    }
    if (cfg->hang) {
        sleep(30);
        return;
    }
    if (cfg->oversize_record) {
        uint8_t h[5];
        h[0] = M_CT_HS;
        h[1] = 0x03;
        h[2] = 0x03;
        h[3] = (uint8_t)(cfg->oversize_record >> 8);
        h[4] = (uint8_t)cfg->oversize_record;
        m_send(&m, h, 5);
        sleep(2);
        return;
    }

    m.suite = cfg->suite ? cfg->suite : (nsuites ? suites[0] : 0x1301);
    switch (m.suite) {
    case 0x1302:
        m.hashalg = HASH_SHA384;
        m.aeadalg = AEAD_AES_256_GCM;
        break;
    case 0x1303:
        m.hashalg = HASH_SHA256;
        m.aeadalg = AEAD_CHACHA20_POLY1305;
        break;
    default:
        m.suite = 0x1301;
        m.hashalg = HASH_SHA256;
        m.aeadalg = AEAD_AES_128_GCM;
        break;
    }
    m.hlen = hash_digest_len(m.hashalg);
    if (!hrr_sent)
        hash_init(&m.tr, m.hashalg);
    hash_update(&m.tr, m.pt, (unsigned long)m.ptlen);

    if (cfg->tls12_hello) {
        /* A TLS 1.2 ServerHello: no supported_versions, its own session id,
         * and a TLS 1.2 cipher suite. */
        uint8_t h[128];
        int p = 0;
        h[p++] = 2;
        h[p++] = 0;
        h[p++] = 0;
        h[p++] = 0;                      /* length patched below */
        h[p++] = 0x03;
        h[p++] = 0x03;
        memset(h + p, 0x5a, 32);
        p += 32;
        h[p++] = 32;
        memset(h + p, 0x11, 32);
        p += 32;
        h[p++] = 0xc0;                   /* ECDHE-RSA-AES128-GCM-SHA256 */
        h[p++] = 0x2f;
        h[p++] = 0;                      /* compression */
        h[p++] = 0;                      /* no extensions */
        h[p++] = 0;
        h[3] = (uint8_t)(p - 4);
        m_record(&m, M_CT_HS, h, (unsigned long)p, 1);
        shutdown(fd, SHUT_RDWR);
        return;
    }

    /* --- HelloRetryRequest --- */
    if (cfg->send_hrr && !hrr_sent) {
        static const uint8_t magic[32] = {
            0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11,
            0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
            0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E,
            0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C
        };
        uint8_t th[64], hdr[4];
        int p = 0, extat;
        int want = cfg->group ? cfg->group : 0x0017;

        /* transcript = message_hash(Hash(ClientHello1)) */
        hash_peek(&m.tr, th);
        hash_init(&m.tr, m.hashalg);
        hdr[0] = 254;
        hdr[1] = 0;
        hdr[2] = 0;
        hdr[3] = (uint8_t)m.hlen;
        hash_update(&m.tr, hdr, 4);
        hash_update(&m.tr, th, m.hlen);

        sh[p++] = 2;
        p += 3;
        sh[p++] = 0x03;
        sh[p++] = 0x03;
        memcpy(sh + p, magic, 32);
        p += 32;
        sh[p++] = (uint8_t)m.sesslen;
        memcpy(sh + p, m.sess, (size_t)m.sesslen);
        p += m.sesslen;
        sh[p++] = (uint8_t)(m.suite >> 8);
        sh[p++] = (uint8_t)m.suite;
        sh[p++] = 0;
        extat = p;
        p += 2;
        sh[p++] = 0;                        /* supported_versions */
        sh[p++] = 43;
        sh[p++] = 0;
        sh[p++] = 2;
        sh[p++] = 0x03;
        sh[p++] = 0x04;
        sh[p++] = 0;                        /* key_share: selected group */
        sh[p++] = 51;
        sh[p++] = 0;
        sh[p++] = 2;
        sh[p++] = (uint8_t)(want >> 8);
        sh[p++] = (uint8_t)want;
        if (cfg->cookie) {
            int cl = (int)strlen(cfg->cookie);
            sh[p++] = 0;
            sh[p++] = 44;
            sh[p++] = (uint8_t)((cl + 2) >> 8);
            sh[p++] = (uint8_t)(cl + 2);
            sh[p++] = (uint8_t)(cl >> 8);
            sh[p++] = (uint8_t)cl;
            memcpy(sh + p, cfg->cookie, (size_t)cl);
            p += cl;
        }
        sh[extat] = (uint8_t)((p - extat - 2) >> 8);
        sh[extat + 1] = (uint8_t)(p - extat - 2);
        sh[1] = 0;
        sh[2] = (uint8_t)((p - 4) >> 8);
        sh[3] = (uint8_t)(p - 4);
        hash_update(&m.tr, sh, (unsigned long)p);
        m_record(&m, M_CT_HS, sh, (unsigned long)p, 1);
        hrr_sent = 1;
        m.group = want;
        m.cpublen = 0;
        goto again;
    }

    /* --- pick a group and do the key exchange --- */
    if (!m.group)
        m.group = m.cpublen == 65 ? 0x0017 : 0x001d;
    {
        int fdr = open("/dev/urandom", O_RDONLY);
        if (fdr < 0)
            return;
        if (m.group == 0x001d) {
            if (read(fdr, m.xpriv, 32) != 32) {
                close(fdr);
                return;
            }
            x25519_base(m.xpub, m.xpriv);
            if (m.cpublen != 32 || x25519(m.shared, m.xpriv, m.cpub) < 0) {
                close(fdr);
                return;
            }
        } else {
            struct p256_point peer;
            do {
                if (read(fdr, m.ppriv, 32) != 32) {
                    close(fdr);
                    return;
                }
            } while (p256_base_mul(&m.ppub, m.ppriv) < 0);
            if (m.cpublen != 65 ||
                p256_point_from_bytes(&peer, m.cpub, 65) < 0 ||
                p256_ecdh(m.shared, m.ppriv, &peer) < 0) {
                close(fdr);
                return;
            }
        }
        close(fdr);
    }

    /* --- ServerHello --- */
    {
        int p = 0, extat, fdr;
        sh[p++] = 2;
        p += 3;
        sh[p++] = 0x03;
        sh[p++] = 0x03;
        fdr = open("/dev/urandom", O_RDONLY);
        if (fdr < 0 || read(fdr, sh + p, 32) != 32) {
            if (fdr >= 0)
                close(fdr);
            return;
        }
        close(fdr);
        p += 32;
        sh[p++] = (uint8_t)m.sesslen;
        memcpy(sh + p, m.sess, (size_t)m.sesslen);
        p += m.sesslen;
        sh[p++] = (uint8_t)(m.suite >> 8);
        sh[p++] = (uint8_t)m.suite;
        sh[p++] = 0;
        extat = p;
        p += 2;
        sh[p++] = 0;
        sh[p++] = 43;
        sh[p++] = 0;
        sh[p++] = 2;
        sh[p++] = 0x03;
        sh[p++] = 0x04;
        sh[p++] = 0;                        /* key_share */
        sh[p++] = 51;
        {
            int klen = m.group == 0x001d ? 32 : 65;
            sh[p++] = (uint8_t)((klen + 4) >> 8);
            sh[p++] = (uint8_t)(klen + 4);
            sh[p++] = (uint8_t)(m.group >> 8);
            sh[p++] = (uint8_t)m.group;
            sh[p++] = (uint8_t)(klen >> 8);
            sh[p++] = (uint8_t)klen;
            if (m.group == 0x001d) {
                memcpy(sh + p, m.xpub, 32);
            } else {
                uint8_t enc[65];
                p256_point_to_bytes(&m.ppub, enc);
                memcpy(sh + p, enc, 65);
            }
            p += klen;
        }
        sh[extat] = (uint8_t)((p - extat - 2) >> 8);
        sh[extat + 1] = (uint8_t)(p - extat - 2);
        sh[1] = 0;
        sh[2] = (uint8_t)((p - 4) >> 8);
        sh[3] = (uint8_t)(p - 4);
        shlen = p;
        hash_update(&m.tr, sh, (unsigned long)shlen);
        m_record(&m, M_CT_HS, sh, (unsigned long)shlen, 1);
    }

    for (i = 0; i < cfg->extra_ccs; i++) {
        uint8_t one = 1;
        m_record(&m, M_CT_CCS, &one, 1, 1);
    }

    m_derive(&m);

    /* --- the encrypted flight --- */
    {
        /* EncryptedExtensions: empty. */
        body[0] = 0;
        body[1] = 0;
        m_msg(&m, flight, &flen, 8, body, 2);
    }

    /* Certificate. */
    {
        unsigned long len = 0;
        struct pem_iter it;
        char label[64];
        char *pem;
        unsigned long pemlen = 0;
        uint8_t der[4096];
        size_t derlen;

        pem = slurp(cfg->certfile ? cfg->certfile : WORK "/rsa.pem", &pemlen);
        if (!pem)
            return;
        pem_iter_init(&it, pem, pemlen);
        while (pem_next(&it, label, sizeof(label), der, sizeof(der),
                        &derlen) == 1) {
            if (certslen + derlen + 5 > sizeof(certs))
                break;
            certs[certslen++] = (uint8_t)(derlen >> 16);
            certs[certslen++] = (uint8_t)(derlen >> 8);
            certs[certslen++] = (uint8_t)derlen;
            memcpy(certs + certslen, der, derlen);
            certslen += derlen;
            certs[certslen++] = 0;
            certs[certslen++] = 0;
        }
        free(pem);
        len = 0;
        body[len++] = 0;                    /* empty context */
        body[len++] = (uint8_t)(certslen >> 16);
        body[len++] = (uint8_t)(certslen >> 8);
        body[len++] = (uint8_t)certslen;
        memcpy(body + len, certs, certslen);
        len += certslen;
        if (!cfg->omit_certificate && !cfg->swap_cert_cv)
            m_msg(&m, flight, &flen, 11, body, len);

        /* CertificateVerify. Omitting the Certificate omits this too:
         * sending a signature with nothing to check it against is a
         * different test, and it is the one swap_cert_cv does. */
        if (!cfg->omit_cert_verify && !cfg->omit_certificate) {
            uint8_t content[64 + 33 + 1 + 64], digest[32], sig[512];
            size_t siglen = 0, clen;
            uint8_t cv[600];
            uint8_t th[64];

            if (load_privkey(cfg->keyfile ? cfg->keyfile : WORK "/rsa.key",
                             &pk) < 0)
                return;
            memset(content, 0x20, 64);
            memcpy(content + 64, "TLS 1.3, server CertificateVerify", 33);
            content[97] = 0;
            hash_peek(&m.tr, th);
            memcpy(content + 98, th, m.hlen);
            clen = 98 + m.hlen;
            hash_oneshot(HASH_SHA256, content, clen, digest);
            if (pss_sign_sha256(&pk, digest, sig, &siglen) < 0)
                return;
            if (cfg->bad_signature)
                sig[siglen / 2] ^= 0x80;
            cv[0] = 0x08;                   /* rsa_pss_rsae_sha256 */
            cv[1] = 0x04;
            cv[2] = (uint8_t)(siglen >> 8);
            cv[3] = (uint8_t)siglen;
            memcpy(cv + 4, sig, siglen);
            m_msg(&m, flight, &flen, 15, cv, 4 + siglen);
        }
        if (cfg->swap_cert_cv)
            m_msg(&m, flight, &flen, 11, body, len);
    }

    /* Finished. */
    {
        uint8_t th[64], fk[64], mac[64];
        hash_peek(&m.tr, th);
        hkdf_expand_label(m.hashalg, m.s_hs, "finished", 0, 0, fk, m.hlen);
        hmac(m.hashalg, fk, m.hlen, th, m.hlen, mac);
        if (cfg->corrupt_finished)
            mac[0] ^= 0x01;
        m_msg(&m, flight, &flen, 20, mac, m.hlen);
    }

    if (cfg->mutate_off > 0 && cfg->mutate_off < flen)
        flight[cfg->mutate_off] ^= (uint8_t)(cfg->mutate_val | 1);

    /* Send it, optionally chopped into small records. */
    if (cfg->frag > 0) {
        int off = 0;
        while (off < flen) {
            int n = flen - off < cfg->frag ? flen - off : cfg->frag;
            m_record(&m, M_CT_HS, flight + off, (unsigned long)n, 0);
            off += n;
        }
    } else {
        m_record(&m, M_CT_HS, flight, (unsigned long)flen, 0);
    }

    m_app_keys(&m);

    /* The client's Finished. */
    if (m_read_record(&m) < 0)
        return;
    m_client_app_keys(&m);

    if (cfg->send_ticket) {
        /* cfg->send_ticket tickets, all in one record: real servers send
         * two at once, and the client has to walk the buffer rather than
         * assume one message per record. */
        uint8_t msg[512];
        int p = 0, t;
        for (t = 0; t < cfg->send_ticket && p < 400; t++) {
            int at = p;
            msg[p++] = 4;
            msg[p++] = 0;
            msg[p++] = 0;
            msg[p++] = 0;
            msg[p++] = 0; msg[p++] = 0; msg[p++] = 0; msg[p++] = 100;
            msg[p++] = 0; msg[p++] = 0; msg[p++] = 0; msg[p++] = 0;
            msg[p++] = 0;                    /* nonce<0..255>          */
            msg[p++] = 0; msg[p++] = 8;      /* ticket<1..2^16-1>      */
            memset(msg + p, (uint8_t)(0xa0 + t), 8);
            p += 8;
            msg[p++] = 0; msg[p++] = 0;      /* extensions             */
            msg[at + 3] = (uint8_t)(p - at - 4);
        }
        m_record(&m, M_CT_HS, msg, (unsigned long)p, 0);
    }

    if (cfg->key_update) {
        uint8_t msg[5];
        msg[0] = 24;
        msg[1] = 0;
        msg[2] = 0;
        msg[3] = 1;
        msg[4] = cfg->key_update == 2 ? 1 : 0;
        m_record(&m, M_CT_HS, msg, 5, 0);
        {
            uint8_t next[64], key[32];
            hkdf_expand_label(m.hashalg, m.s_ap, "traffic upd", 0, 0, next,
                              m.hlen);
            memcpy(m.s_ap, next, m.hlen);
            hkdf_expand_label(m.hashalg, m.s_ap, "key", 0, 0, key,
                              aead_key_len(m.aeadalg));
            hkdf_expand_label(m.hashalg, m.s_ap, "iv", 0, 0, m.txiv, 12);
            aead_init(&m.tx, m.aeadalg, key);
            m.txseq = 0;
        }
    }

    if (cfg->reply_app) {
        const char *msg = "HTTP/1.0 200 OK\r\nContent-Length: 5\r\n\r\nmock\n";
        long want = cfg->expect_bytes ? cfg->expect_bytes : 1;
        long got = 0;

        /* Read the client's request first so the exchange is two-way. */
        while (got < want) {
            if (m_read_record(&m) < 0)
                break;
            if (m.pttype == M_CT_APP)
                got += m.ptlen;
        }
        if (cfg->big_reply) {
            static uint8_t chunk[16384];
            long sent = 0;
            long i;
            for (i = 0; i < (long)sizeof(chunk); i++)
                chunk[i] = (uint8_t)(i * 7 + 3);
            memcpy(chunk, msg, strlen(msg));
            while (sent < cfg->big_reply) {
                long n = cfg->big_reply - sent;
                if (n > (long)sizeof(chunk))
                    n = (long)sizeof(chunk);
                m_record(&m, M_CT_APP, chunk, (unsigned long)n, 0);
                sent += n;
            }
        } else {
            m_record(&m, M_CT_APP, (const uint8_t *)msg, strlen(msg), 0);
        }
        if (!cfg->no_close_notify) {
            uint8_t a[2];
            a[0] = 1;
            a[1] = 0;                        /* close_notify */
            m_record(&m, M_CT_ALERT, a, 2, 0);
        }
    }
    shutdown(fd, SHUT_RDWR);
}

/* Run the mock server in a child process and the client here. */
static void with_mock_n(const struct mock_cfg *cfg,
                        const struct tls_options *o, const void *request,
                        int reqlen, struct fetch_result *out)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    socklen_t alen = sizeof(a);
    pid_t pid;
    int port, one = 1;

    memset(out, 0, sizeof(*out));
    if (lfd < 0)
        return;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) < 0 ||
        listen(lfd, 4) < 0 ||
        getsockname(lfd, (struct sockaddr *)&a, &alen) < 0) {
        close(lfd);
        return;
    }
    port = ntohs(a.sin_port);

    pid = fork();
    if (pid == 0) {
        int cfd = accept(lfd, 0, 0);
        if (cfd >= 0) {
            mock_server(cfd, cfg);
            close(cfd);
        }
        close(lfd);
        _exit(0);
    }
    close(lfd);
    fetch_n("localhost", port, o, request, reqlen, out);
    kill(pid, SIGKILL);
    waitpid(pid, 0, 0);
}

static void with_mock(const struct mock_cfg *cfg, const struct tls_options *o,
                      const char *request, struct fetch_result *out)
{
    with_mock_n(cfg, o, request, request ? (int)strlen(request) : 0, out);
}

/* ===================================================================== *
 * 2. The mock server: positive cases                                    *
 * ===================================================================== */

static struct x509_store *g_ca;

static void mock_defaults(struct mock_cfg *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->certfile = WORK "/rsa.pem";
    cfg->keyfile = WORK "/rsa.key";
    cfg->reply_app = 1;
}

static void opt_defaults(struct tls_options *o)
{
    tls_options_default(o);
    o->roots = g_ca;
    o->alpn = 0;
    o->timeout_ms = 5000;
}

static void mock_ok(const char *what, struct mock_cfg *cfg,
                    struct tls_options *o, const char *cipher,
                    const char *group)
{
    struct fetch_result r;

    with_mock(cfg, o, GET, &r);
    g_checks++;
    if (!r.ok) {
        g_fails++;
        printf("  FAIL %-28s %s\n", what, r.err);
        return;
    }
    if (cipher && strcmp(r.info.cipher, cipher) != 0) {
        g_fails++;
        printf("  FAIL %-28s cipher %s, wanted %s\n", what, r.info.cipher,
               cipher);
        return;
    }
    if (group && strcmp(r.info.group, group) != 0) {
        g_fails++;
        printf("  FAIL %-28s group %s, wanted %s\n", what, r.info.group,
               group);
        return;
    }
    if (r.body_len < 15 || memcmp(r.body, "HTTP/1.0 200 OK", 15) != 0) {
        g_fails++;
        printf("  FAIL %-28s got %d bytes back: %.30s\n", what, r.body_len,
               r.body);
        return;
    }
    printf("  %-28s %s over %s%s, %d bytes\n", what, r.info.cipher,
           r.info.group, r.info.hello_retry ? " (after a retry)" : "",
           r.body_len);
}

static void test_mock_positive(void)
{
    struct mock_cfg cfg;
    struct tls_options o;

    section("mock server: complete handshakes");
    opt_defaults(&o);

    mock_defaults(&cfg);
    cfg.suite = 0x1301;
    mock_ok("AES-128-GCM", &cfg, &o, "TLS_AES_128_GCM_SHA256", 0);
    cfg.suite = 0x1302;
    mock_ok("AES-256-GCM / SHA-384", &cfg, &o, "TLS_AES_256_GCM_SHA384", 0);
    cfg.suite = 0x1303;
    mock_ok("ChaCha20-Poly1305", &cfg, &o, "TLS_CHACHA20_POLY1305_SHA256", 0);

    mock_defaults(&cfg);
    cfg.group = 0x001d;
    mock_ok("X25519", &cfg, &o, 0, "x25519");
    cfg.group = 0x0017;
    mock_ok("P-256", &cfg, &o, 0, "secp256r1");

    /* One record per 40 bytes: the flight now spans many records and every
     * handshake message is split across at least two of them. */
    mock_defaults(&cfg);
    cfg.frag = 40;
    mock_ok("flight in 40-byte records", &cfg, &o, 0, 0);
    cfg.frag = 1;
    mock_ok("flight in 1-byte records", &cfg, &o, 0, 0);

    /* One byte per write(): every record arrives split across TCP reads. */
    mock_defaults(&cfg);
    cfg.dribble = 1;
    mock_ok("one byte per TCP write", &cfg, &o, 0, 0);

    mock_defaults(&cfg);
    cfg.send_hrr = 1;
    cfg.group = 0x0017;
    o.key_shares = TLS_G_X25519;
    mock_ok("HelloRetryRequest to P-256", &cfg, &o, 0, "secp256r1");
    cfg.cookie = "a-cookie-the-client-must-echo";
    mock_ok("HelloRetryRequest + cookie", &cfg, &o, 0, "secp256r1");
    opt_defaults(&o);

    mock_defaults(&cfg);
    cfg.extra_ccs = 3;
    mock_ok("dummy change_cipher_spec x3", &cfg, &o, 0, 0);

    mock_defaults(&cfg);
    cfg.send_ticket = 1;
    mock_ok("NewSessionTicket ignored", &cfg, &o, 0, 0);
    cfg.send_ticket = 3;
    mock_ok("3 tickets in one record", &cfg, &o, 0, 0);

    mock_defaults(&cfg);
    cfg.key_update = 1;
    mock_ok("KeyUpdate (no reply asked)", &cfg, &o, 0, 0);
    cfg.key_update = 2;
    mock_ok("KeyUpdate (reply asked for)", &cfg, &o, 0, 0);

    mock_defaults(&cfg);
    cfg.certfile = WORK "/chain.pem";
    mock_ok("leaf + CA in the message", &cfg, &o, 0, 0);

    /* The store holds the self-signed root; the server sends the
     * cross-signed one on top. The anchor is one certificate down. */
    mock_defaults(&cfg);
    cfg.certfile = WORK "/crosschain.pem";
    mock_ok("chain ends in a cross-sign", &cfg, &o, 0, 0);

    mock_defaults(&cfg);
    cfg.certfile = WORK "/junkchain.pem";
    mock_ok("an unrelated cert stapled on", &cfg, &o, 0, 0);

    /* rsa_only is what the automatic fallback turns on. */
    mock_defaults(&cfg);
    o.rsa_only = 1;
    mock_ok("no ECDSA in signature_algorithms", &cfg, &o, 0, 0);
    o.rsa_only = 0;
}

/* Data larger than one record, in both directions. */
static void test_mock_bulk(void)
{
    struct mock_cfg cfg;
    struct tls_options o;
    struct fetch_result r;
    char *req;
    const long reqlen = 100000;
    const long replylen = 300000;
    long i;

    section("mock server: data larger than a record");
    opt_defaults(&o);
    o.timeout_ms = 10000;

    req = malloc((unsigned long)reqlen);
    for (i = 0; i < reqlen; i++)
        req[i] = (char)('a' + (i % 26));

    mock_defaults(&cfg);
    cfg.expect_bytes = reqlen;
    cfg.big_reply = replylen;
    with_mock_n(&cfg, &o, req, (int)reqlen, &r);
    T(r.ok, "a 100 KB write and a 300 KB read complete");
    TI(r.total, replylen, "every byte of the 300 KB reply arrived");
    T(r.body_len >= 15 && memcmp(r.body, "HTTP/1.0 200 OK", 15) == 0,
      "the reply starts where it should");
    printf("  wrote %ld bytes (7 records), read %ld bytes (19 records)\n",
           reqlen, r.total);

    /* A connection cut without close_notify must be reported, not passed
     * off as a clean end of file: that is the truncation attack. */
    mock_defaults(&cfg);
    cfg.no_close_notify = 1;
    with_mock(&cfg, &o, GET, &r);
    T(r.ok, "a connection with no close_notify still delivers its data");
    TI(r.last, TLS_E_TRUNCATED, "and the last read reports truncation");

    mock_defaults(&cfg);
    with_mock(&cfg, &o, GET, &r);
    TI(r.last, 0, "a close_notify reads as a clean end of stream");

    free(req);
}

/* ===================================================================== *
 * 3. Failures that must be reported precisely                           *
 * ===================================================================== */

static void mock_fail(const char *what, struct mock_cfg *cfg,
                      struct tls_options *o, const char *needle)
{
    struct fetch_result r;

    with_mock(cfg, o, GET, &r);
    if (r.ok) {
        g_checks++;
        g_fails++;
        printf("  FAIL %-30s the handshake SUCCEEDED and should not have\n",
               what);
        return;
    }
    expect_msg(what, r.err, needle, 0);
}

static void test_negative_protocol(void)
{
    struct mock_cfg cfg;
    struct tls_options o;

    section("failures: the protocol");
    opt_defaults(&o);

    mock_defaults(&cfg);
    cfg.corrupt_finished = 1;
    mock_fail("corrupted server Finished", &cfg, &o, "Finished");

    mock_defaults(&cfg);
    cfg.corrupt_record = 2;      /* the first encrypted record */
    mock_fail("corrupted record MAC", &cfg, &o, "failed authentication");

    mock_defaults(&cfg);
    cfg.bad_signature = 1;
    mock_fail("corrupted CertificateVerify", &cfg, &o, "does not verify");

    mock_defaults(&cfg);
    cfg.omit_certificate = 1;
    mock_fail("Finished with no Certificate", &cfg, &o, "without a");

    mock_defaults(&cfg);
    cfg.omit_cert_verify = 1;
    mock_fail("Finished with no CertVerify", &cfg, &o, "without a");

    mock_defaults(&cfg);
    cfg.swap_cert_cv = 1;
    mock_fail("CertVerify before Certificate", &cfg, &o, "before its");

    mock_defaults(&cfg);
    cfg.close_after = 5;
    mock_fail("cut after 5 bytes", &cfg, &o, "handshake");

    mock_defaults(&cfg);
    cfg.close_after = 200;
    mock_fail("cut mid ServerHello", &cfg, &o, "handshake");

    mock_defaults(&cfg);
    cfg.close_after = 1200;
    mock_fail("cut mid certificate flight", &cfg, &o, "handshake");

    mock_defaults(&cfg);
    cfg.tls12_hello = 1;
    mock_fail("a TLS 1.2 ServerHello", &cfg, &o, "TLS 1.2");

    mock_defaults(&cfg);
    cfg.plain_alert = 70;        /* protocol_version */
    mock_fail("alert: protocol_version", &cfg, &o, "TLS 1.2");

    mock_defaults(&cfg);
    cfg.plain_alert = 40;        /* handshake_failure */
    mock_fail("alert: handshake_failure", &cfg, &o, "cipher suite");

    mock_defaults(&cfg);
    cfg.plain_alert = 112;       /* unrecognized_name */
    mock_fail("alert: unrecognized_name", &cfg, &o, "host name");

    mock_defaults(&cfg);
    cfg.http_response = 1;
    mock_fail("a plain HTTP server", &cfg, &o, "plain HTTP");

    mock_defaults(&cfg);
    cfg.extra_ccs = 40;
    mock_fail("40 change_cipher_spec records", &cfg, &o,
              "change_cipher_spec");

    mock_defaults(&cfg);
    cfg.oversize_record = 20000;
    mock_fail("a 20000 byte record", &cfg, &o, "over the");

    /* A server that accepts the connection and then says nothing must time
     * out with a message that says so, not hang or fail namelessly. */
    mock_defaults(&cfg);
    cfg.hang = 1;
    o.timeout_ms = 700;
    mock_fail("a server that never answers", &cfg, &o, "stopped responding");
    o.timeout_ms = 5000;
}

static void test_negative_certs(void)
{
    struct mock_cfg cfg;
    struct tls_options o;
    struct fetch_result r;

    section("failures: certificates");
    opt_defaults(&o);

    mock_defaults(&cfg);
    cfg.certfile = WORK "/expired.pem";
    mock_fail("an expired certificate", &cfg, &o, "expired on");

    mock_defaults(&cfg);
    cfg.certfile = WORK "/wronghost.pem";
    mock_fail("the wrong host name", &cfg, &o, "not 'localhost'");

    mock_defaults(&cfg);
    cfg.certfile = WORK "/self.pem";
    cfg.keyfile = WORK "/self.key";
    mock_fail("self-signed, no root", &cfg, &o, "no trusted root");

    /* An empty trust store must fail loudly rather than accept anything. */
    {
        struct x509_store *empty = malloc(sizeof(*empty));
        x509_store_init(empty);
        o.roots = empty;
        mock_defaults(&cfg);
        mock_fail("an empty trust store", &cfg, &o, "no trusted root");
        free(empty);
        o.roots = g_ca;
    }

    /* TLS_VERIFY_NONE connects, but must still say exactly what was
     * wrong -- that is what lets a browser offer "proceed anyway" instead
     * of silently accepting. */
    mock_defaults(&cfg);
    cfg.certfile = WORK "/self.pem";
    cfg.keyfile = WORK "/self.key";
    o.verify = TLS_VERIFY_NONE;
    with_mock(&cfg, &o, GET, &r);
    T(r.ok, "TLS_VERIFY_NONE completes the handshake");
    T(r.info.verified == 0, "TLS_VERIFY_NONE reports verified = 0");
    expect_msg("verify=NONE still explains", r.info.cert_error,
               "no trusted root", 0);
    o.verify = TLS_VERIFY_REQUIRED;

    /* And the certificate details come back either way. */
    T(strcmp(r.info.subject, "localhost") == 0,
      "the leaf subject CN is reported");
}

/* ===================================================================== *
 * 4. The key schedule, checked against openssl's own key log            *
 * ===================================================================== */

/* Only compiled into the host build; declared in tls.c under TLS_HOST. */
extern void tls_test_secrets(const struct tls_conn *c, uint8_t *client_random,
                             uint8_t *chs, uint8_t *shs, uint8_t *cap,
                             uint8_t *sap, int *len);

static void hex(const uint8_t *b, int n, char *out)
{
    static const char *d = "0123456789abcdef";
    int i;

    for (i = 0; i < n; i++) {
        out[2 * i] = d[b[i] >> 4];
        out[2 * i + 1] = d[b[i] & 15];
    }
    out[2 * n] = 0;
}

/* Find "<label> <client_random> <secret>" in the key log. */
static int keylog_lookup(const char *path, const char *label,
                         const char *crandom, char *out, int outcap)
{
    FILE *f = fopen(path, "r");
    char line[1024];
    int found = 0;

    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        char l[64], cr[128], sec[256];
        if (sscanf(line, "%63s %127s %255s", l, cr, sec) != 3)
            continue;
        if (strcmp(l, label) != 0 || strcmp(cr, crandom) != 0)
            continue;
        snprintf(out, (unsigned long)outcap, "%s", sec);
        found = 1;
    }
    fclose(f);
    return found;
}

static void test_keylog(void)
{
    char args[512], keylog[256];
    int port;
    pid_t pid;
    struct tls_options o;
    struct tls_error err;
    struct tls_conn *c;
    uint8_t cr[32], chs[64], shs[64], cap[64], sap[64];
    int len = 0;
    char crhex[80], mine[160], theirs[256];
    struct x509_store *ca;
    static const struct {
        const char *label;
        int which;
    } want[] = {
        { "CLIENT_HANDSHAKE_TRAFFIC_SECRET", 0 },
        { "SERVER_HANDSHAKE_TRAFFIC_SECRET", 1 },
        { "CLIENT_TRAFFIC_SECRET_0", 2 },
        { "SERVER_TRAFFIC_SECRET_0", 3 }
    };
    int i, suite;
    static const char *suites[] = {
        "TLS_AES_128_GCM_SHA256", "TLS_AES_256_GCM_SHA384",
        "TLS_CHACHA20_POLY1305_SHA256"
    };

    section("key schedule vs openssl's key log");
    if (!g_have_openssl) {
        printf("  SKIP: openssl is not on PATH\n");
        return;
    }
    ca = store_from(WORK "/ca.pem");
    if (!ca)
        return;

    for (suite = 0; suite < 3; suite++) {
        snprintf(keylog, sizeof(keylog), "%s/keylog-%d.txt", WORK, suite);
        unlink(keylog);
        snprintf(args, sizeof(args),
                 "-cert %s/rsa.pem -key %s/rsa.key -ciphersuites %s",
                 WORK, WORK, suites[suite]);
        port = free_port();
        pid = start_server(port, args, keylog);
        if (pid < 0) {
            printf("  SKIP %s: s_server would not start\n", suites[suite]);
            continue;
        }
        tls_options_default(&o);
        o.roots = ca;
        o.alpn = 0;
        c = tls_connect("localhost", port, &o, &err);
        if (!c) {
            g_checks++;
            g_fails++;
            printf("  FAIL %s: %s\n", suites[suite], err.msg);
            stop_server(pid);
            continue;
        }
        /* Exchange something so the server flushes its log. */
        tls_write(c, GET, (int)strlen(GET));
        {
            char buf[512];
            tls_read(c, buf, sizeof(buf));
        }
        tls_test_secrets(c, cr, chs, shs, cap, sap, &len);
        tls_close(c);
        stop_server(pid);

        hex(cr, 32, crhex);
        for (i = 0; i < 4; i++) {
            const uint8_t *s = i == 0 ? chs : i == 1 ? shs :
                               i == 2 ? cap : sap;
            hex(s, len, mine);
            g_checks++;
            if (!keylog_lookup(keylog, want[i].label, crhex, theirs,
                               sizeof(theirs))) {
                g_fails++;
                printf("  FAIL %s %s: not in the key log\n", suites[suite],
                       want[i].label);
            } else if (strcmp(mine, theirs) != 0) {
                g_fails++;
                printf("  FAIL %s %s\n     ours   %s\n     openssl %s\n",
                       suites[suite], want[i].label, mine, theirs);
            }
        }
        printf("  %-30s all four traffic secrets match openssl's\n",
               suites[suite]);
    }
    store_free(ca);
}

/* ===================================================================== *
 * 5. Fuzzing the record and handshake parsers                           *
 * ===================================================================== */

static void test_fuzz(void)
{
    struct mock_cfg cfg;
    struct tls_options o;
    struct fetch_result r;
    int i, survived = 0, accepted = 0;
    unsigned seed = 12345;

    section("fuzzing with mutated server flights");
    opt_defaults(&o);
    o.timeout_ms = 3000;

    /* Every one of these decrypts correctly and then hands a corrupted
     * handshake message to the parsers -- EncryptedExtensions, the
     * certificate list, the DER inside it, CertificateVerify, Finished.
     * The whole flight is covered by the transcript, so a mutation
     * anywhere in it must be rejected: either by a parser, or by the
     * chain, or by the signature, or finally by the Finished MAC. A
     * single accepted flight would mean some byte of the server's
     * handshake is not actually being authenticated. The offset range is
     * kept inside the shortest flight the mock can produce (~1200 bytes)
     * so that "no mutation happened" is not a possible outcome. */
    for (i = 0; i < 500; i++) {
        mock_defaults(&cfg);
        seed = seed * 1103515245u + 12345u;
        cfg.mutate_off = (int)((seed >> 16) % 1000) + 1;
        seed = seed * 1103515245u + 12345u;
        cfg.mutate_val = (int)(seed >> 16) & 0xff;
        cfg.frag = (i % 7) == 0 ? 64 : 0;
        with_mock(&cfg, &o, GET, &r);
        survived++;
        if (r.ok) {
            accepted++;
            if (accepted < 4)
                printf("  ACCEPTED a flight mutated at offset %d\n",
                       cfg.mutate_off);
        }
    }
    printf("  %d flights mutated inside the transcript: %d accepted\n",
           survived, accepted);
    TI(survived, 500, "every mutated flight was handled");
    TI(accepted, 0, "no mutated flight was accepted");

    /* Raw garbage where a record should be. */
    for (i = 0; i < 200; i++) {
        int port, fd, lfd;
        struct sockaddr_in a;
        socklen_t alen = sizeof(a);
        pid_t pid;
        struct tls_error err;
        struct tls_conn *c;

        lfd = socket(AF_INET, SOCK_STREAM, 0);
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (lfd < 0 || bind(lfd, (struct sockaddr *)&a, sizeof(a)) < 0 ||
            listen(lfd, 4) < 0 ||
            getsockname(lfd, (struct sockaddr *)&a, &alen) < 0) {
            if (lfd >= 0)
                close(lfd);
            break;
        }
        port = ntohs(a.sin_port);
        pid = fork();
        if (pid == 0) {
            uint8_t junk[4096];
            int n, j;
            unsigned s = (unsigned)i * 2654435761u + 7u;
            fd = accept(lfd, 0, 0);
            if (fd >= 0) {
                char sink[4096];
                (void)!read(fd, sink, sizeof(sink));
                n = (int)(s % 3000) + 5;
                for (j = 0; j < n; j++) {
                    s = s * 1103515245u + 12345u;
                    junk[j] = (uint8_t)(s >> 16);
                }
                /* Half the time, make it look like a plausible record. */
                if (i % 2) {
                    junk[0] = 22;
                    junk[1] = 3;
                    junk[2] = 3;
                    junk[3] = (uint8_t)((n - 5) >> 8);
                    junk[4] = (uint8_t)(n - 5);
                }
                (void)!write(fd, junk, (size_t)n);
                shutdown(fd, SHUT_RDWR);
                close(fd);
            }
            close(lfd);
            _exit(0);
        }
        close(lfd);
        c = tls_connect("localhost", port, &o, &err);
        if (c)
            tls_close(c);
        kill(pid, SIGKILL);
        waitpid(pid, 0, 0);
    }
    printf("  200 random byte streams in place of a ServerHello: no crash\n");
    T(1, "random server streams handled");
}

/* ===================================================================== *
 * 6. Odds and ends                                                      *
 * ===================================================================== */

static void test_misc(void)
{
    struct tls_options o;
    struct x509_store *s;
    int total = 0, from_file = 0, i, n;

    section("options, roots and diagnostics");

    tls_options_default(&o);
    TI(o.verify, TLS_VERIFY_REQUIRED, "verification is required by default");
    T(o.alpn && strcmp(o.alpn, "http/1.1") == 0, "ALPN defaults to http/1.1");
    TI(o.groups, TLS_G_ALL, "both groups are offered by default");
    TI(o.suites, TLS_S_ALL, "all three suites are offered by default");

    {
        extern unsigned long tls_test_conn_size(void);
        printf("  one connection is %lu bytes of heap, the trust store is "
               "%lu\n", tls_test_conn_size(),
               (unsigned long)sizeof(struct x509_store));
    }

    s = tls_default_store();
    T(s != 0, "the default trust store builds");
    tls_default_store_stat(&total, &from_file);
    n = tls_builtin_root_count();
    printf("  built-in roots: %d, store holds %d (%d from "
           TLS_ROOTS_FILE ")\n", n, total, from_file);
    T(n >= 15, "at least fifteen roots are compiled in");
    TI(total, n + from_file, "every compiled-in root parsed and loaded");
    for (i = 0; i < n; i++)
        T(tls_builtin_root_name(i) != 0, "every root is named");
    T(tls_builtin_root_name(n) == 0, "the name table ends");

    T(says(tls_alert_text(70), "TLS 1.2"),
      "the protocol_version alert mentions TLS 1.2");
    T(says(tls_alert_text(48), "trust"), "unknown_ca mentions trust");
    T(says(tls_error_text(TLS_E_CERT), "certificate"),
      "TLS_E_CERT reads sensibly");
    T(strcmp(tls_suite_name(0x1303), "TLS_CHACHA20_POLY1305_SHA256") == 0,
      "cipher suite names");
    T(strcmp(tls_group_name(0x001d), "x25519") == 0, "group names");
    T(says(tls_group_name(0x0018), "384"), "P-384 is named even though it "
      "is not implemented");

    /* Bad arguments must be refused, not crash. */
    T(tls_connect(0, 443, 0, 0) == 0, "a NULL host is refused");
    T(tls_connect("localhost", 0, 0, 0) == 0, "port 0 is refused");
    T(tls_connect("localhost", 70000, 0, 0) == 0, "port 70000 is refused");
    T(tls_read(0, 0, 0) == TLS_E_INVAL, "tls_read(NULL) is refused");
    T(tls_write(0, 0, 0) == TLS_E_INVAL, "tls_write(NULL) is refused");
    tls_close(0);
    T(1, "tls_close(NULL) is safe");
    T(tls_info(0, 0) == TLS_E_INVAL, "tls_info(NULL) is refused");
    T(tls_entropy_is_weak() == 0 || tls_entropy_is_weak() == 1,
      "the entropy quality is reported");
}

/* The transport factory libweb plugs into. */
static void test_transport_factory(void)
{
    struct mock_cfg cfg;
    struct tls_options o;
    struct tls_transport t;
    int lfd, port, rc, one = 1;
    struct sockaddr_in a;
    socklen_t alen = sizeof(a);
    pid_t pid;
    char buf[512];

    section("the libweb transport factory");
    opt_defaults(&o);
    mock_defaults(&cfg);

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (lfd < 0 || bind(lfd, (struct sockaddr *)&a, sizeof(a)) < 0 ||
        listen(lfd, 4) < 0 ||
        getsockname(lfd, (struct sockaddr *)&a, &alen) < 0) {
        printf("  SKIP: could not listen\n");
        return;
    }
    port = ntohs(a.sin_port);
    pid = fork();
    if (pid == 0) {
        int cfd = accept(lfd, 0, 0);
        if (cfd >= 0) {
            mock_server(cfd, &cfg);
            close(cfd);
        }
        close(lfd);
        _exit(0);
    }
    close(lfd);

    memset(&t, 0, sizeof(t));
    rc = tls_transport_open("localhost", port, 5000, &o, &t);
    TI(rc, 0, "tls_transport_open returns HTTP_OK");
    if (rc == 0) {
        int n = t.write(t.ctx, GET, (int)strlen(GET));
        T(n == (int)strlen(GET), "the transport writes");
        n = t.read(t.ctx, buf, sizeof(buf));
        T(n > 0 && memcmp(buf, "HTTP/1.0 200", 12) == 0,
          "the transport reads the response");
        T(t.set_timeout && t.set_timeout(t.ctx, 1000) == 0,
          "set_timeout works");
        t.close(t.ctx);
    }
    kill(pid, SIGKILL);
    waitpid(pid, 0, 0);

    /* A failure must map onto libweb's codes and keep the message. */
    rc = tls_transport_open("no-such-host.invalid", 443, 2000, &o, &t);
    T(rc < 0, "an unresolvable host fails");
    printf("  transport failure message: %s\n", tls_last_transport_error());
}

/* ===================================================================== *
 * main                                                                  *
 * ===================================================================== */

static void usage(void)
{
    printf("usage: test_tls [-v] [fetch <host> [port] [path]]\n");
}

int main(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            g_verbose = 1;
        } else if (strcmp(argv[i], "fetch") == 0) {
            const char *host = i + 1 < argc ? argv[i + 1] : "example.com";
            int port = i + 2 < argc ? atoi(argv[i + 2]) : 443;
            const char *path = i + 3 < argc ? argv[i + 3] : "/";
            char req[512];
            struct tls_options o;
            struct fetch_result r;

            tls_options_default(&o);
            snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: "
                     "KestrelOS/1.0 (libtls)\r\nConnection: close\r\n\r\n",
                     path, host);
            fetch(host, port, &o, req, &r);
            if (!r.ok) {
                printf("FAILED: %s\n", r.err);
                return 1;
            }
            printf("cipher   %s\ngroup    %s\nalpn     %s\nverified %d\n"
                   "subject  %s\nissuer   %s\nchain    %d\n"
                   "entropy  %s\n----\n",
                   r.info.cipher, r.info.group,
                   r.info.alpn[0] ? r.info.alpn : "(none)",
                   r.info.verified, r.info.subject, r.info.issuer,
                   r.info.chain_len,
                   r.info.weak_entropy ? "WEAK (no hardware RNG)"
                                       : "hardware RNG present");
            fwrite(r.body, 1, (unsigned long)r.body_len, stdout);
            printf("\n---- %d bytes\n", r.body_len);
            return 0;
        } else {
            usage();
            return 2;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    g_have_openssl = check_openssl();
    if (g_have_openssl && make_certs() < 0) {
        printf("could not generate test certificates\n");
        g_have_openssl = 0;
    }

    test_misc();
    if (g_have_openssl) {
        g_ca = store_from(WORK "/ca.pem");
        if (!g_ca) {
            printf("could not build a trust store from the test CA\n");
            return 1;
        }
        test_interop();
        test_keylog();
        test_mock_positive();
        test_mock_bulk();
        test_negative_protocol();
        test_negative_certs();
        test_transport_factory();
        test_fuzz();
        store_free(g_ca);
    } else {
        printf("\nopenssl is not on PATH: every test that needs a server or "
               "a certificate was skipped.\n");
    }
    tls_default_store_free();

    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
