/* Host test harness for libweb's URL, HTTP, cookie and cache code.
 *
 * Build and run (from the repo root, on Linux/WSL):
 *
 *   gcc -Wall -Wextra -O2 -fsanitize=address,undefined -Ilibweb \
 *       -DHTTP_HOST -o /tmp/test_http \
 *       tools/test_http.c libweb/http.c libweb/url.c libweb/cookie.c \
 *       libweb/cache.c && /tmp/test_http
 *
 * The library never opens a socket itself: it asks a transport factory
 * registered for the URL scheme. That is what makes this possible — the
 * harness registers a POSIX-socket factory for "http", writes a raw
 * HTTP/1.1 server in python3 to /tmp, runs it as a subprocess, and drives
 * the real client against it over a real TCP connection. Nothing is
 * mocked except the socket layer's origin resolution: every host name
 * resolves to 127.0.0.1 so cookie domain rules can be exercised.
 *
 * The inflate hook is filled with a stored-block-only DEFLATE decoder
 * written here (the python server compresses at level 0, which produces
 * real gzip/zlib streams whose blocks are all stored). That validates
 * every part of the HTTP side — coding detection, staging, the hook
 * contract, CRC/length verification — without duplicating the real
 * inflate implementation another agent is writing.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "http.h"
#include "url.h"
#include "cookie.h"

static int g_checks, g_fails;
static int g_port;

#define T(cond, msg)                                                          \
    do {                                                                      \
        g_checks++;                                                           \
        if (!(cond)) {                                                        \
            g_fails++;                                                        \
            printf("  FAIL line %d: %s\n", __LINE__, (msg));                  \
        }                                                                     \
    } while (0)

static void ts(const char *got, const char *want, const char *msg, int line)
{
    g_checks++;
    if (got == 0 || strcmp(got, want) != 0) {
        g_fails++;
        printf("  FAIL line %d: %s\n     got  \"%s\"\n     want \"%s\"\n",
               line, msg, got ? got : "(null)", want);
    }
}
#define TS(got, want, msg) ts((got), (want), (msg), __LINE__)

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

/* ===================================================================== *
 * A stored-block DEFLATE decoder, enough for level-0 gzip/zlib streams.
 * ===================================================================== */

static unsigned long crc32_of(const unsigned char *d, unsigned long n)
{
    static unsigned long tab[256];
    static int built;
    unsigned long c = 0xffffffffUL;
    unsigned long i;

    if (!built) {
        unsigned long k, j;
        for (k = 0; k < 256; k++) {
            unsigned long v = k;
            for (j = 0; j < 8; j++)
                v = (v & 1) ? (0xedb88320UL ^ (v >> 1)) : (v >> 1);
            tab[k] = v;
        }
        built = 1;
    }
    for (i = 0; i < n; i++)
        c = tab[(c ^ d[i]) & 0xff] ^ (c >> 8);
    return (c ^ 0xffffffffUL) & 0xffffffffUL;
}

static unsigned long adler32_of(const unsigned char *d, unsigned long n)
{
    unsigned long a = 1, b = 0, i;

    for (i = 0; i < n; i++) {
        a = (a + d[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

static int test_inflate(const void *src, unsigned long slen, void **out,
                        unsigned long *olen, int wrapper)
{
    const unsigned char *p = src;
    unsigned long off = 0, end = slen;
    unsigned char *o = 0;
    unsigned long ocap = 0, olen2 = 0;

    *out = 0;
    *olen = 0;
    if (slen < 4)
        return -1;

    if (wrapper == HTTP_INFLATE_GZIP) {
        int flg;
        if (slen < 18 || p[0] != 0x1f || p[1] != 0x8b || p[2] != 8)
            return -1;
        flg = p[3];
        off = 10;
        if (flg & 4) {
            if (off + 2 > slen)
                return -1;
            off += 2 + (unsigned long)(p[off] | (p[off + 1] << 8));
        }
        if (flg & 8) {
            while (off < slen && p[off])
                off++;
            off++;
        }
        if (flg & 16) {
            while (off < slen && p[off])
                off++;
            off++;
        }
        if (flg & 2)
            off += 2;
        if (off + 8 > slen)
            return -1;
        end = slen - 8;
    } else if (wrapper == HTTP_INFLATE_ZLIB) {
        if ((p[0] & 0x0f) != 8)
            return -1;
        off = 2;
        end = slen - 4;
    }

    for (;;) {
        int bfinal, btype;
        unsigned long len;

        if (off >= end)
            goto bad;
        bfinal = p[off] & 1;
        btype = (p[off] >> 1) & 3;
        if (btype != 0)
            goto bad;               /* only stored blocks here */
        off++;
        if (off + 4 > end)
            goto bad;
        len = (unsigned long)(p[off] | (p[off + 1] << 8));
        if ((unsigned long)(p[off + 2] | (p[off + 3] << 8)) != (~len & 0xffff))
            goto bad;
        off += 4;
        if (off + len > end)
            goto bad;
        if (olen2 + len + 1 > ocap) {
            unsigned char *n;
            ocap = (olen2 + len + 1) * 2;
            n = realloc(o, ocap);
            if (n == 0)
                goto bad;
            o = n;
        }
        memcpy(o + olen2, p + off, len);
        olen2 += len;
        off += len;
        if (bfinal)
            break;
    }
    if (o == 0) {
        o = malloc(1);
        if (o == 0)
            return -1;
    }
    o[olen2] = 0;

    if (wrapper == HTTP_INFLATE_GZIP) {
        unsigned long crc = (unsigned long)p[end] | ((unsigned long)p[end + 1] << 8) |
                            ((unsigned long)p[end + 2] << 16) |
                            ((unsigned long)p[end + 3] << 24);
        unsigned long isize = (unsigned long)p[end + 4] |
                              ((unsigned long)p[end + 5] << 8) |
                              ((unsigned long)p[end + 6] << 16) |
                              ((unsigned long)p[end + 7] << 24);
        if (crc != crc32_of(o, olen2) || isize != (olen2 & 0xffffffffUL))
            goto bad;
    } else if (wrapper == HTTP_INFLATE_ZLIB) {
        unsigned long ad = ((unsigned long)p[end] << 24) |
                           ((unsigned long)p[end + 1] << 16) |
                           ((unsigned long)p[end + 2] << 8) |
                           (unsigned long)p[end + 3];
        if (ad != adler32_of(o, olen2))
            goto bad;
    }
    *out = o;
    *olen = olen2;
    return 0;

bad:
    free(o);
    return -1;
}

/* ===================================================================== *
 * POSIX socket transport
 * ===================================================================== */

struct sock_ctx {
    int fd;
};

static int sock_read(void *ctx, void *buf, int len)
{
    struct sock_ctx *s = ctx;
    ssize_t n = recv(s->fd, buf, (size_t)len, 0);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return HTTP_E_TIMEOUT;
        return HTTP_E_RECV;
    }
    return (int)n;
}

static int sock_write(void *ctx, const void *buf, int len)
{
    struct sock_ctx *s = ctx;
    ssize_t n = send(s->fd, buf, (size_t)len, MSG_NOSIGNAL);

    if (n <= 0)
        return HTTP_E_SEND;
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

    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(s->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return 0;
}

/* Every host resolves to loopback; the port comes from the URL. */
static int sock_factory(const char *host, int port, int timeout_ms, void *user,
                        struct http_transport *out)
{
    struct sockaddr_in sa;
    struct sock_ctx *s;
    struct timeval tv;
    int fd;

    (void)host;
    (void)user;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return HTTP_E_CONNECT;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = htonl(0x7f000001);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return HTTP_E_CONNECT;
    }
    s = malloc(sizeof(*s));
    if (s == 0) {
        close(fd);
        return HTTP_E_NOMEM;
    }
    s->fd = fd;
    out->ctx = s;
    out->read = sock_read;
    out->write = sock_write;
    out->close = sock_close;
    out->set_timeout = sock_set_timeout;
    return HTTP_OK;
}

/* ===================================================================== *
 * The python3 server, written out at run time
 * ===================================================================== */

static const char *const PYSRC[] = {
"import socket, threading, sys, zlib, time, os\n",
"conns = 0\n",
"hostconns = {}\n",
"fresh_hits = 0\n",
"nostore_hits = 0\n",
"cond200 = 0\n",
"cond304 = 0\n",
"lock = threading.Lock()\n",
"\n",
"def resp(status, reason, headers, body):\n",
"    h = 'HTTP/1.1 ' + str(status) + ' ' + reason + '\\r\\n'\n",
"    for k, v in headers:\n",
"        h += k + ': ' + v + '\\r\\n'\n",
"    h += '\\r\\n'\n",
"    return h.encode('latin-1') + body\n",
"\n",
"def plain(body, extra=None):\n",
"    hs = [('Content-Type', 'text/plain'),\n",
"          ('Content-Length', str(len(body)))]\n",
"    if extra:\n",
"        hs = extra + hs\n",
"    return resp(200, 'OK', hs, body)\n",
"\n",
"def hget(hdrs, name):\n",
"    n = name.lower() + ':'\n",
"    for s in hdrs:\n",
"        if s.lower().startswith(n):\n",
"            return s.split(':', 1)[1].strip()\n",
"    return None\n",
"\n",
"def handle(conn):\n",
"    global conns, fresh_hits, nostore_hits, cond200, cond304\n",
"    with lock:\n",
"        conns += 1\n",
"    first = True\n",
"    f = conn.makefile('rb')\n",
"    while True:\n",
"        try:\n",
"            line = f.readline()\n",
"        except Exception:\n",
"            break\n",
"        if not line:\n",
"            break\n",
"        parts = line.split()\n",
"        if len(parts) < 2:\n",
"            break\n",
"        method = parts[0].decode('latin-1')\n",
"        path = parts[1].decode('latin-1')\n",
"        hdrs = []\n",
"        clen = 0\n",
"        while True:\n",
"            h = f.readline()\n",
"            if not h or h == b'\\r\\n' or h == b'\\n':\n",
"                break\n",
"            s = h.decode('latin-1').rstrip('\\r\\n')\n",
"            hdrs.append(s)\n",
"            if s.lower().startswith('content-length:'):\n",
"                clen = int(s.split(':', 1)[1].strip())\n",
"        body = f.read(clen) if clen > 0 else b''\n",
"        if first:\n",
"            first = False\n",
"            hn = hget(hdrs, 'host') or '?'\n",
"            with lock:\n",
"                hostconns[hn] = hostconns.get(hn, 0) + 1\n",
"        keep = True\n",
"        out = None\n",
"\n",
"        if path == '/len':\n",
"            out = plain(b'hello world')\n",
"        elif path == '/empty':\n",
"            out = plain(b'')\n",
"        elif path == '/chunked':\n",
"            b = (b'5;ext=one\\r\\nHello\\r\\n' + b'6\\r\\n World\\r\\n'\n",
"                 + b'0\\r\\nX-Trailer: yes\\r\\n\\r\\n')\n",
"            out = resp(200, 'OK', [('Content-Type', 'text/plain'),\n",
"                                   ('Transfer-Encoding', 'chunked')], b)\n",
"        elif path == '/close':\n",
"            out = resp(200, 'OK', [('Content-Type', 'text/plain'),\n",
"                                   ('Connection', 'close')], b'closed-body')\n",
"            keep = False\n",
"        elif path == '/gzip':\n",
"            payload = b'gzip-payload.' * 500\n",
"            co = zlib.compressobj(0, zlib.DEFLATED, 16 + zlib.MAX_WBITS)\n",
"            z = co.compress(payload) + co.flush()\n",
"            out = resp(200, 'OK', [('Content-Type', 'text/plain'),\n",
"                                   ('Content-Encoding', 'gzip'),\n",
"                                   ('Content-Length', str(len(z)))], z)\n",
"        elif path == '/deflate':\n",
"            payload = b'deflate-payload.' * 500\n",
"            co = zlib.compressobj(0)\n",
"            z = co.compress(payload) + co.flush()\n",
"            out = resp(200, 'OK', [('Content-Type', 'text/plain'),\n",
"                                   ('Content-Encoding', 'deflate'),\n",
"                                   ('Content-Length', str(len(z)))], z)\n",
"        elif path == '/gzipchunked':\n",
"            payload = b'gzip-chunked.' * 500\n",
"            co = zlib.compressobj(0, zlib.DEFLATED, 16 + zlib.MAX_WBITS)\n",
"            z = co.compress(payload) + co.flush()\n",
"            b = b''\n",
"            i = 0\n",
"            while i < len(z):\n",
"                part = z[i:i + 700]\n",
"                b += ('%x' % len(part)).encode() + b'\\r\\n' + part + b'\\r\\n'\n",
"                i += 700\n",
"            b += b'0\\r\\n\\r\\n'\n",
"            out = resp(200, 'OK', [('Content-Encoding', 'gzip'),\n",
"                                   ('Transfer-Encoding', 'chunked')], b)\n",
"        elif path == '/badgzip':\n",
"            z = bytes([31, 139, 8, 0, 0, 0, 0, 0, 0, 3]) + b'A' * 40\n",
"            out = resp(200, 'OK', [('Content-Encoding', 'gzip'),\n",
"                                   ('Content-Length', str(len(z)))], z)\n",
"        elif path == '/method':\n",
"            m = (method + '|' + str(len(body))).encode()\n",
"            out = plain(m)\n",
"        elif path == '/post':\n",
"            ct = hget(hdrs, 'content-type') or '-'\n",
"            m = (method + '|' + ct + '|'\n",
"                 + body.decode('latin-1')).encode('latin-1')\n",
"            out = plain(m)\n",
"        elif path.startswith('/redir/'):\n",
"            code = int(path[7:])\n",
"            out = resp(code, 'Redirect', [('Location', '/method'),\n",
"                                          ('Content-Length', '0')], b'')\n",
"        elif path == '/relredir':\n",
"            out = resp(302, 'Found', [('Location', 'method'),\n",
"                                      ('Content-Length', '0')], b'')\n",
"        elif path == '/loop1':\n",
"            out = resp(302, 'Found', [('Location', '/loop2'),\n",
"                                      ('Content-Length', '0')], b'')\n",
"        elif path == '/loop2':\n",
"            out = resp(302, 'Found', [('Location', '/loop1'),\n",
"                                      ('Content-Length', '0')], b'')\n",
"        elif path.startswith('/chain/'):\n",
"            n = int(path[7:])\n",
"            out = resp(302, 'Found', [('Location', '/chain/' + str(n + 1)),\n",
"                                      ('Content-Length', '0')], b'')\n",
"        elif path == '/fresh':\n",
"            with lock:\n",
"                fresh_hits += 1\n",
"                n = fresh_hits\n",
"            b = ('fresh-' + str(n)).encode()\n",
"            out = resp(200, 'OK', [('Cache-Control', 'max-age=60'),\n",
"                                   ('Content-Type', 'text/plain'),\n",
"                                   ('Content-Length', str(len(b)))], b)\n",
"        elif path == '/nostore':\n",
"            with lock:\n",
"                nostore_hits += 1\n",
"                n = nostore_hits\n",
"            b = ('nostore-' + str(n)).encode()\n",
"            out = resp(200, 'OK', [('Cache-Control', 'no-store'),\n",
"                                   ('Content-Length', str(len(b)))], b)\n",
"        elif path == '/hits':\n",
"            b = str(fresh_hits).encode()\n",
"            out = resp(200, 'OK', [('Cache-Control', 'no-store'),\n",
"                                   ('Content-Length', str(len(b)))], b)\n",
"        elif path == '/condstats':\n",
"            b = (str(cond200) + '/' + str(cond304)).encode()\n",
"            out = resp(200, 'OK', [('Cache-Control', 'no-store'),\n",
"                                   ('Content-Length', str(len(b)))], b)\n",
"        elif path == '/cond':\n",
"            inm = hget(hdrs, 'if-none-match')\n",
"            ims = hget(hdrs, 'if-modified-since')\n",
"            if inm == '\"v1\"' or ims is not None:\n",
"                with lock:\n",
"                    cond304 += 1\n",
"                out = resp(304, 'Not Modified',\n",
"                           [('ETag', '\"v1\"'),\n",
"                            ('Cache-Control', 'max-age=0'),\n",
"                            ('X-Revalidated', 'yes')], b'')\n",
"            else:\n",
"                with lock:\n",
"                    cond200 += 1\n",
"                b = b'cond-body'\n",
"                out = resp(200, 'OK',\n",
"                           [('ETag', '\"v1\"'),\n",
"                            ('Last-Modified',\n",
"                             'Sun, 06 Nov 1994 08:49:37 GMT'),\n",
"                            ('Cache-Control', 'max-age=0'),\n",
"                            ('Content-Type', 'text/plain'),\n",
"                            ('Content-Length', str(len(b)))], b)\n",
"        elif path == '/cookie/set':\n",
"            hs = [('Set-Cookie', 'a=1; Path=/'),\n",
"                  ('Set-Cookie', 'b=2; Path=/cookie'),\n",
"                  ('Set-Cookie', 'sec=3; Path=/; Secure'),\n",
"                  ('Set-Cookie', 'gone=4; Path=/; Max-Age=0'),\n",
"                  ('Set-Cookie', 'keep=5; Path=/; Max-Age=3600'),\n",
"                  ('Set-Cookie', 'evil=6; Domain=test; Path=/'),\n",
"                  ('Set-Cookie', 'ho=7; Path=/; HttpOnly'),\n",
"                  ('Cache-Control', 'no-store'),\n",
"                  ('Content-Length', '2')]\n",
"            out = resp(200, 'OK', hs, b'ok')\n",
"        elif path.endswith('/echo'):\n",
"            c = hget(hdrs, 'cookie') or ''\n",
"            b = c.encode('latin-1')\n",
"            out = resp(200, 'OK', [('Cache-Control', 'no-store'),\n",
"                                   ('Content-Length', str(len(b)))], b)\n",
"        elif path == '/hdrecho':\n",
"            b = ('|'.join(hdrs)).encode('latin-1')\n",
"            out = resp(200, 'OK', [('Cache-Control', 'no-store'),\n",
"                                   ('Content-Length', str(len(b)))], b)\n",
"        elif path == '/fold':\n",
"            raw = ('HTTP/1.1 200 OK\\r\\nX-Folded: one\\r\\n\\ttwo\\r\\n'\n",
"                   'Content-Length: 2\\r\\n\\r\\nok')\n",
"            out = raw.encode()\n",
"        elif path == '/dup':\n",
"            out = resp(200, 'OK', [('X-Dup', 'one'), ('X-Dup', 'two'),\n",
"                                   ('Content-Length', '2')], b'ok')\n",
"        elif path == '/case':\n",
"            out = resp(200, 'OK', [('cOnTeNt-TyPe', 'text/html'),\n",
"                                   ('CONTENT-LENGTH', '2')], b'ok')\n",
"        elif path == '/bighdr':\n",
"            raw = ('HTTP/1.1 200 OK\\r\\nX-Big: ' + 'a' * 70000\n",
"                   + '\\r\\nContent-Length: 2\\r\\n\\r\\nok')\n",
"            out = raw.encode()\n",
"        elif path == '/manyhdr':\n",
"            hs = [('X-Pad-' + str(i), 'v' * 300) for i in range(200)]\n",
"            hs.append(('Content-Length', '2'))\n",
"            out = resp(200, 'OK', hs, b'ok')\n",
"        elif path == '/smuggle':\n",
"            raw = ('HTTP/1.1 200 OK\\r\\nContent-Length: 2\\r\\n'\n",
"                   'Content-Length: 9\\r\\n\\r\\nok')\n",
"            out = raw.encode()\n",
"        elif path == '/continue':\n",
"            conn.sendall(b'HTTP/1.1 100 Continue\\r\\n\\r\\n')\n",
"            out = plain(b'after-continue')\n",
"        elif path == '/cutbody':\n",
"            conn.sendall(b'HTTP/1.1 200 OK\\r\\nContent-Length: 100\\r\\n'\n",
"                         b'\\r\\n' + b'x' * 10)\n",
"            break\n",
"        elif path == '/cutchunk':\n",
"            conn.sendall(b'HTTP/1.1 200 OK\\r\\n'\n",
"                         b'Transfer-Encoding: chunked\\r\\n\\r\\n'\n",
"                         b'10\\r\\n01234')\n",
"            break\n",
"        elif path == '/stall':\n",
"            conn.sendall(b'HTTP/1.1 200 OK\\r\\nContent-Length: 100\\r\\n\\r\\n')\n",
"            time.sleep(25)\n",
"            break\n",
"        elif path == '/stallhdr':\n",
"            time.sleep(25)\n",
"            break\n",
"        elif path == '/big10':\n",
"            n = 10 * 1024 * 1024\n",
"            conn.sendall(('HTTP/1.1 200 OK\\r\\n'\n",
"                          'Content-Type: application/octet-stream\\r\\n'\n",
"                          'Content-Length: ' + str(n)\n",
"                          + '\\r\\n\\r\\n').encode())\n",
"            chunk = b'K' * 65536\n",
"            try:\n",
"                for i in range(n // 65536):\n",
"                    conn.sendall(chunk)\n",
"            except Exception:\n",
"                break\n",
"            continue\n",
"        elif path == '/oneshot':\n",
"            conn.sendall(plain(b'one-shot'))\n",
"            break\n",
"        elif path == '/conns':\n",
"            hn = hget(hdrs, 'host') or '?'\n",
"            b = str(hostconns.get(hn, 0)).encode()\n",
"            out = resp(200, 'OK', [('Cache-Control', 'no-store'),\n",
"                                   ('Content-Length', str(len(b)))], b)\n",
"        elif path == '/status/500':\n",
"            out = resp(500, 'Internal Server Error',\n",
"                       [('Content-Length', '3')], b'err')\n",
"        elif path == '/204':\n",
"            out = resp(204, 'No Content', [('Content-Length', '17')], b'')\n",
"        elif path == '/teunknown':\n",
"            out = resp(200, 'OK', [('Transfer-Encoding', 'bogus')],\n",
"                       b'unknown-coding')\n",
"            keep = False\n",
"        else:\n",
"            out = resp(404, 'Not Found', [('Content-Length', '9')],\n",
"                       b'not-found')\n",
"        try:\n",
"            conn.sendall(out)\n",
"        except Exception:\n",
"            break\n",
"        if not keep:\n",
"            break\n",
"    try:\n",
"        conn.shutdown(socket.SHUT_RDWR)\n",
"    except Exception:\n",
"        pass\n",
"    conn.close()\n",
"\n",
"srv = socket.socket()\n",
"srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)\n",
"srv.bind(('127.0.0.1', 0))\n",
"srv.listen(64)\n",
"print(srv.getsockname()[1])\n",
"sys.stdout.flush()\n",
"threading.Timer(180, lambda: os._exit(0)).start()\n",
"while True:\n",
"    c, a = srv.accept()\n",
"    t = threading.Thread(target=handle, args=(c,))\n",
"    t.daemon = True\n",
"    t.start()\n",
0
};

static pid_t g_server_pid = -1;

static int start_server(void)
{
    const char *path = "/tmp/kestrel_test_http_server.py";
    FILE *f = fopen(path, "w");
    int pipefd[2];
    char line[64];
    int i, n = 0;

    if (f == 0) {
        printf("cannot write %s\n", path);
        return -1;
    }
    for (i = 0; PYSRC[i]; i++)
        fputs(PYSRC[i], f);
    fclose(f);

    if (pipe(pipefd) < 0)
        return -1;
    g_server_pid = fork();
    if (g_server_pid < 0)
        return -1;
    if (g_server_pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], 1);
        close(pipefd[1]);
        execlp("python3", "python3", "-u", path, (char *)0);
        _exit(127);
    }
    close(pipefd[1]);
    while (n < (int)sizeof(line) - 1) {
        char ch;
        ssize_t r = read(pipefd[0], &ch, 1);
        if (r <= 0)
            break;
        if (ch == '\n')
            break;
        line[n++] = ch;
    }
    line[n] = '\0';
    close(pipefd[0]);
    g_port = atoi(line);
    if (g_port <= 0) {
        printf("server did not report a port (got \"%s\")\n", line);
        return -1;
    }
    return 0;
}

static void stop_server(void)
{
    if (g_server_pid > 0) {
        kill(g_server_pid, SIGKILL);
        waitpid(g_server_pid, 0, 0);
        g_server_pid = -1;
    }
}

static const char *U(const char *host, const char *path)
{
    static char buf[8][512];
    static int k;
    char *b = buf[k++ & 7];

    snprintf(b, 512, "http://%s:%d%s", host, g_port, path);
    return b;
}

/* ===================================================================== *
 * URL tests
 * ===================================================================== */

static void test_url_basics(void)
{
    struct url u;
    char out[URL_MAX];

    section("URL parsing and serialisation");

    T(url_parse("http://user:pw@Example.COM:8080/a/b?x=1&y=2#frag", &u) ==
      URL_OK, "parse full URL");
    TS(u.scheme, "http", "scheme lower-cased");
    TS(u.userinfo, "user:pw", "userinfo");
    TS(u.host, "example.com", "host lower-cased");
    TI(u.port, 8080, "port");
    TS(u.path, "/a/b", "path");
    TS(u.query, "x=1&y=2", "query");
    TS(u.fragment, "frag", "fragment");
    T(url_serialize(&u, out, sizeof(out)) == URL_OK, "serialize");
    TS(out, "http://user:pw@example.com:8080/a/b?x=1&y=2#frag", "round trip");

    T(url_parse("http://[2001:db8::1]:8080/x", &u) == URL_OK, "parse IPv6");
    TS(u.host, "2001:db8::1", "IPv6 host without brackets");
    TI(u.is_ipv6, 1, "is_ipv6");
    TI(u.port, 8080, "IPv6 port");
    url_serialize(&u, out, sizeof(out));
    TS(out, "http://[2001:db8::1]:8080/x", "IPv6 round trip");
    url_host_header(&u, out, sizeof(out));
    TS(out, "[2001:db8::1]:8080", "IPv6 Host header");

    T(url_parse("http://[::ffff:192.0.2.1]/", &u) == URL_OK, "IPv4-mapped");
    TS(u.host, "::ffff:192.0.2.1", "IPv4-mapped host");

    T(url_parse("http://host]/x", &u) == URL_EPARSE, "reject stray bracket");
    T(url_parse("http://host:99999/x", &u) == URL_EPARSE, "reject huge port");
    T(url_parse("http://host:8a/x", &u) == URL_EPARSE, "reject non-numeric port");
    T(url_parse("http://[2001:db8::1/x", &u) == URL_EPARSE, "unterminated v6");

    T(url_parse("http://host/", &u) == URL_OK, "no port");
    TI(u.port, -1, "port absent");
    TI(url_effective_port(&u), 80, "default port 80");
    T(url_parse("https://host/", &u) == URL_OK, "https");
    TI(url_effective_port(&u), 443, "default port 443");

    T(url_parse("http://host:80/a", &u) == URL_OK, "explicit default port");
    url_normalize(&u, URL_N_PORT);
    url_serialize(&u, out, sizeof(out));
    TS(out, "http://host/a", "default port dropped");

    T(url_parse("http://host", &u) == URL_OK, "empty path");
    url_normalize(&u, URL_N_EMPTY_PATH);
    TS(u.path, "/", "empty path becomes /");

    /* Bytes that are illegal in a component get escaped, valid triplets
     * are preserved with upper-case hex. */
    T(url_parse("http://h/a b\"c?q=a b#f g", &u) == URL_OK, "escape spaces");
    TS(u.path, "/a%20b%22c", "path escaped");
    TS(u.query, "q=a%20b", "query escaped");
    TS(u.fragment, "f%20g", "fragment escaped");
    T(url_parse("http://h/%7ename%2f%zz", &u) == URL_OK, "triplets");
    TS(u.path, "/%7Ename%2F%25zz", "hex upper-cased, stray %% escaped");
    url_normalize(&u, URL_N_PCT);
    TS(u.path, "/~name%2F%25zz", "unreserved triplet decoded");

    T(url_parse("/just/a/path?q", &u) == URL_OK, "relative reference");
    TI(u.has_scheme, 0, "no scheme");
    TI(u.has_authority, 0, "no authority");
    TS(u.path, "/just/a/path", "relative path");

    T(url_parse("//example.org/p", &u) == URL_OK, "scheme-relative");
    TI(u.has_authority, 1, "authority present");
    TS(u.host, "example.org", "scheme-relative host");

    url_parse("http://h/a/b/c?q=1", &u);
    url_request_target(&u, out, sizeof(out));
    TS(out, "/a/b/c?q=1", "request target");
    url_parse("http://h", &u);
    url_request_target(&u, out, sizeof(out));
    TS(out, "/", "request target for empty path");
    url_parse("http://h:8080/x", &u);
    url_origin(&u, out, sizeof(out));
    TS(out, "http://h:8080", "origin with port");
    url_parse("http://h:80/x", &u);
    url_origin(&u, out, sizeof(out));
    TS(out, "http://h", "origin drops default port");
}

static void test_url_encode(void)
{
    char out[256];

    section("percent encoding and decoding");

    T(url_pct_encode("a b/c?d", ~0UL, URL_COMP_PATH, out, sizeof(out)) > 0,
      "encode path");
    TS(out, "a%20b/c%3Fd", "path keeps '/', escapes '?'");
    url_pct_encode("a b/c?d", ~0UL, URL_COMP_SEGMENT, out, sizeof(out));
    TS(out, "a%20b%2Fc%3Fd", "segment escapes '/'");
    url_pct_encode("a b&c=d", ~0UL, URL_COMP_QUERY, out, sizeof(out));
    TS(out, "a%20b&c=d", "query keeps sub-delims");
    url_pct_encode("a b&c=d~e", ~0UL, URL_COMP_FORM, out, sizeof(out));
    TS(out, "a+b%26c%3Dd~e", "form encoding");
    url_pct_encode("user:pw@host", ~0UL, URL_COMP_USERINFO, out, sizeof(out));
    TS(out, "user:pw%40host", "userinfo escapes '@'");

    T(url_pct_decode("a%20b%2Fc", ~0UL, 0, out, sizeof(out)) == 5, "decode");
    TS(out, "a b/c", "decoded");
    url_pct_decode("a+b%2Bc", ~0UL, 1, out, sizeof(out));
    TS(out, "a b+c", "plus decoding");
    url_pct_decode("bad%zz%", ~0UL, 0, out, sizeof(out));
    TS(out, "bad%zz%", "invalid triplets pass through");
    T(url_pct_encode("aaaaaaaaaa", ~0UL, URL_COMP_PATH, out, 5) == -1,
      "encode reports overflow");
    T(url_pct_decode("aaaaaaaaaa", ~0UL, 0, out, 5) == -1,
      "decode reports overflow");
}

/* Every example from RFC 3986 section 5.4. */
static void test_url_rfc3986(void)
{
    static const char *const base = "http://a/b/c/d;p?q";
    static const char *const cases[][2] = {
        /* 5.4.1 normal examples */
        { "g:h",           "g:h" },
        { "g",             "http://a/b/c/g" },
        { "./g",           "http://a/b/c/g" },
        { "g/",            "http://a/b/c/g/" },
        { "/g",            "http://a/g" },
        { "//g",           "http://g" },
        { "?y",            "http://a/b/c/d;p?y" },
        { "g?y",           "http://a/b/c/g?y" },
        { "#s",            "http://a/b/c/d;p?q#s" },
        { "g#s",           "http://a/b/c/g#s" },
        { "g?y#s",         "http://a/b/c/g?y#s" },
        { ";x",            "http://a/b/c/;x" },
        { "g;x",           "http://a/b/c/g;x" },
        { "g;x?y#s",       "http://a/b/c/g;x?y#s" },
        { "",              "http://a/b/c/d;p?q" },
        { ".",             "http://a/b/c/" },
        { "./",            "http://a/b/c/" },
        { "..",            "http://a/b/" },
        { "../",           "http://a/b/" },
        { "../g",          "http://a/b/g" },
        { "../..",         "http://a/" },
        { "../../",        "http://a/" },
        { "../../g",       "http://a/g" },
        /* 5.4.2 abnormal examples */
        { "../../../g",    "http://a/g" },
        { "../../../../g", "http://a/g" },
        { "/./g",          "http://a/g" },
        { "/../g",         "http://a/g" },
        { "g.",            "http://a/b/c/g." },
        { ".g",            "http://a/b/c/.g" },
        { "g..",           "http://a/b/c/g.." },
        { "..g",           "http://a/b/c/..g" },
        { "./../g",        "http://a/b/g" },
        { "./g/.",         "http://a/b/c/g/" },
        { "g/./h",         "http://a/b/c/g/h" },
        { "g/../h",        "http://a/b/c/h" },
        { "g;x=1/./y",     "http://a/b/c/g;x=1/y" },
        { "g;x=1/../y",    "http://a/b/c/y" },
        { "g?y/./x",       "http://a/b/c/g?y/./x" },
        { "g?y/../x",      "http://a/b/c/g?y/../x" },
        { "g#s/./x",       "http://a/b/c/g#s/./x" },
        { "g#s/../x",      "http://a/b/c/g#s/../x" },
        { "http:g",        "http:g" },      /* strict */
        { 0, 0 }
    };
    char out[URL_MAX];
    char msg[160];
    int i;

    section("RFC 3986 section 5.4 reference resolution");

    for (i = 0; cases[i][0]; i++) {
        int rc = url_resolve_str(base, cases[i][0], out, sizeof(out));
        snprintf(msg, sizeof(msg), "resolve \"%s\"", cases[i][0]);
        if (rc != URL_OK) {
            g_checks++;
            g_fails++;
            printf("  FAIL %s: rc=%d (%s)\n", msg, rc, url_strerror(rc));
            continue;
        }
        ts(out, cases[i][1], msg, __LINE__);
    }
    printf("  %d RFC 3986 5.4 examples checked\n", i);

    /* The one case where strict and non-strict differ. */
    {
        struct url b, r, t;
        url_parse(base, &b);
        url_parse("http:g", &r);
        T(url_resolve(&b, &r, &t, 0) == URL_OK, "non-strict resolve");
        url_serialize(&t, out, sizeof(out));
        TS(out, "http://a/b/c/g", "non-strict http:g");
    }
}

/* ===================================================================== *
 * Cookie unit tests
 * ===================================================================== */

static void test_cookies_unit(void)
{
    char buf[512];
    struct cookie_jar *j;
    long now = 1700000000L;

    section("cookie rules");

    T(cookie_domain_match("example.com", "example.com"), "exact domain");
    T(cookie_domain_match("a.example.com", "example.com"), "subdomain");
    T(!cookie_domain_match("badexample.com", "example.com"), "suffix is not enough");
    T(!cookie_domain_match("example.com", "a.example.com"), "no upward match");
    T(!cookie_domain_match("192.168.1.1", "168.1.1"), "IP host never matches");

    T(cookie_path_match("/a/b", "/a/b"), "identical path");
    T(cookie_path_match("/a/b/c", "/a/b"), "prefix at a boundary");
    T(cookie_path_match("/a/b", "/a/"), "prefix ending in slash");
    T(!cookie_path_match("/a/bc", "/a/b"), "prefix mid-segment");
    T(cookie_path_match("/anything", "/"), "root matches everything");

    cookie_default_path("/a/b/c", buf, sizeof(buf));
    TS(buf, "/a/b", "default path strips the last segment");
    cookie_default_path("/a", buf, sizeof(buf));
    TS(buf, "/", "default path of a top-level resource");
    cookie_default_path("", buf, sizeof(buf));
    TS(buf, "/", "default path of an empty request path");

    T(cookie_public_suffix("com"), "bare TLD rejected");
    T(cookie_public_suffix("co.uk"), "co.uk rejected");
    T(cookie_public_suffix("com.au"), "com.au rejected");
    T(cookie_public_suffix("ac.nz"), "generic second level rejected");
    T(cookie_public_suffix("1.2.3.4"), "IP literal rejected as a domain");
    T(!cookie_public_suffix("example.com"), "ordinary domain allowed");
    T(!cookie_public_suffix("a.example.co.uk"), "deep domain allowed");

    TI(cookie_parse_date("Sun, 06 Nov 1994 08:49:37 GMT"), 784111777L,
       "IMF-fixdate");
    TI(cookie_parse_date("Sunday, 06-Nov-94 08:49:37 GMT"), 784111777L,
       "RFC 850 date");
    TI(cookie_parse_date("Sun Nov  6 08:49:37 1994"), 784111777L,
       "asctime date");
    TI(cookie_parse_date("Wed, 21 Oct 2015 07:28:00 GMT"), 1445412480L,
       "2015 date");
    TI(cookie_parse_date("not a date"), -1, "garbage rejected");
    TI(http_parse_date("Sun, 06 Nov 1994 08:49:37 GMT"), 784111777L,
       "http_parse_date IMF");
    TI(http_parse_date("Sun Nov  6 08:49:37 1994"), 784111777L,
       "http_parse_date asctime");

    j = cookie_jar_new();
    T(j != 0, "jar allocated");

    T(cookie_set(j, "sid=abc; Path=/", "example.com", "/x", 0, now) == 1,
      "store a session cookie");
    TI(cookie_jar_count(j), 1, "one cookie");
    cookie_header(j, "example.com", "/x", 0, 1, COOKIE_CTX_SAME_SITE, now,
                  buf, sizeof(buf));
    TS(buf, "sid=abc", "cookie sent back");

    cookie_header(j, "other.com", "/x", 0, 1, COOKIE_CTX_SAME_SITE, now,
                  buf, sizeof(buf));
    TS(buf, "", "not sent to another host");

    T(cookie_set(j, "sub=1; Domain=example.com", "www.example.com", "/", 0,
                 now) == 1, "domain cookie from a subdomain");
    cookie_header(j, "deep.www.example.com", "/", 0, 1, COOKIE_CTX_SAME_SITE,
                  now, buf, sizeof(buf));
    TS(buf, "sub=1", "domain cookie reaches deeper subdomains");

    T(cookie_set(j, "evil=1; Domain=com", "example.com", "/", 0, now) == 0,
      "public suffix domain rejected");
    T(cookie_set(j, "evil=1; Domain=other.com", "example.com", "/", 0,
                 now) == 0, "unrelated domain rejected");
    T(cookie_set(j, "novalue", "example.com", "/", 0, now) == 0,
      "Set-Cookie without '=' rejected");

    /* Ordering: longer paths first, then oldest first. */
    cookie_set(j, "p1=1; Path=/", "ord.com", "/", 0, now);
    cookie_set(j, "p2=2; Path=/a/b", "ord.com", "/a/b", 0, now + 1);
    cookie_set(j, "p3=3; Path=/a", "ord.com", "/a", 0, now + 2);
    cookie_header(j, "ord.com", "/a/b/c", 0, 1, COOKIE_CTX_SAME_SITE, now + 3,
                  buf, sizeof(buf));
    TS(buf, "p2=2; p3=3; p1=1", "RFC 6265 5.4 ordering");

    /* Secure and HttpOnly filtering. */
    cookie_set(j, "s=1; Path=/; Secure", "sec.com", "/", 1, now);
    cookie_set(j, "h=1; Path=/; HttpOnly", "sec.com", "/", 1, now + 1);
    cookie_header(j, "sec.com", "/", 0, 1, COOKIE_CTX_SAME_SITE, now, buf,
                  sizeof(buf));
    TS(buf, "h=1", "Secure cookie withheld over plain http");
    cookie_header(j, "sec.com", "/", 1, 1, COOKIE_CTX_SAME_SITE, now, buf,
                  sizeof(buf));
    TS(buf, "s=1; h=1", "Secure cookie sent over https");
    cookie_header(j, "sec.com", "/", 1, 0, COOKIE_CTX_SAME_SITE, now, buf,
                  sizeof(buf));
    TS(buf, "s=1", "HttpOnly hidden from the script view");

    /* Expiry. */
    cookie_set(j, "e=1; Path=/; Max-Age=100", "exp.com", "/", 0, now);
    cookie_header(j, "exp.com", "/", 0, 1, COOKIE_CTX_SAME_SITE, now + 50, buf,
                  sizeof(buf));
    TS(buf, "e=1", "cookie alive before Max-Age");
    cookie_header(j, "exp.com", "/", 0, 1, COOKIE_CTX_SAME_SITE, now + 150, buf,
                  sizeof(buf));
    TS(buf, "", "cookie dead after Max-Age");
    TI(cookie_expire(j, now + 150), 1, "one cookie reaped");

    cookie_set(j, "d=1; Path=/; Max-Age=100", "del.com", "/", 0, now);
    T(cookie_set(j, "d=1; Path=/; Max-Age=0", "del.com", "/", 0, now) == 1,
      "Max-Age=0 deletes");
    cookie_header(j, "del.com", "/", 0, 1, COOKIE_CTX_SAME_SITE, now, buf,
                  sizeof(buf));
    TS(buf, "", "deleted cookie is gone");

    /* Max-Age beats Expires. */
    cookie_set(j, "m=1; Path=/; Expires=Sun, 06 Nov 1994 08:49:37 GMT; "
               "Max-Age=100", "prec.com", "/", 0, now);
    cookie_header(j, "prec.com", "/", 0, 1, COOKIE_CTX_SAME_SITE, now, buf,
                  sizeof(buf));
    TS(buf, "m=1", "Max-Age overrides an expired Expires");

    /* SameSite. */
    cookie_set(j, "ss=strict; Path=/; SameSite=Strict", "ss.com", "/", 0, now);
    cookie_set(j, "sl=lax; Path=/; SameSite=Lax", "ss.com", "/", 0, now + 1);
    cookie_set(j, "sn=none; Path=/; SameSite=None", "ss.com", "/", 0, now + 2);
    cookie_header(j, "ss.com", "/", 0, 1, COOKIE_CTX_CROSS_TOP, now + 3, buf,
                  sizeof(buf));
    TS(buf, "sl=lax; sn=none", "Strict withheld on a cross-site navigation");
    cookie_header(j, "ss.com", "/", 0, 1, COOKIE_CTX_CROSS_SUB, now + 3, buf,
                  sizeof(buf));
    TS(buf, "sn=none", "only SameSite=None on a cross-site subresource");

    /* Per-domain cap. */
    {
        int i, before = cookie_jar_count(j);
        char sc[64];
        for (i = 0; i < COOKIE_PER_DOMAIN_MAX + 20; i++) {
            snprintf(sc, sizeof(sc), "c%d=v; Path=/; Max-Age=1000", i);
            cookie_set(j, sc, "flood.com", "/", 0, now + i);
        }
        (void)before;
        {
            int n = 0;
            const struct cookie *c;
            for (c = cookie_jar_first(j); c; c = c->next)
                if (strcmp(c->domain, "flood.com") == 0)
                    n++;
            TI(n, COOKIE_PER_DOMAIN_MAX, "per-domain cap enforced");
        }
    }

    /* Persistence round trip. */
    {
        struct cookie_jar *j2;
        cookie_set(j, "persist=yes; Path=/deep; Max-Age=3600; HttpOnly",
                   "save.com", "/deep", 0, now);
        cookie_set(j, "session=no; Path=/", "save.com", "/", 0, now);
        T(cookie_jar_save(j, "/tmp/kestrel_cookies_test.txt", now) == 0,
          "jar saved");
        j2 = cookie_jar_new();
        T(cookie_jar_load(j2, "/tmp/kestrel_cookies_test.txt", now) == 0,
          "jar loaded");
        cookie_header(j2, "save.com", "/deep", 0, 1, COOKIE_CTX_SAME_SITE, now,
                      buf, sizeof(buf));
        TS(buf, "persist=yes", "persistent cookie survived");
        cookie_header(j2, "save.com", "/", 0, 1, COOKIE_CTX_SAME_SITE, now,
                      buf, sizeof(buf));
        TS(buf, "", "session cookie was not persisted");
        cookie_jar_free(j2);
    }

    /* Hostile input. */
    {
        char big[9000];
        memset(big, 'a', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        memcpy(big, "huge=", 5);
        T(cookie_set(j, big, "big.com", "/", 0, now) == 0,
          "oversized Set-Cookie rejected");
    }

    cookie_jar_free(j);
}

/* ===================================================================== *
 * HTTP tests
 * ===================================================================== */

struct sink_state {
    unsigned long bytes;
    int calls;
    unsigned long checksum;
    int abort_after;
};

static int counting_sink(void *u, const void *d, unsigned long n)
{
    struct sink_state *s = u;
    const unsigned char *p = d;
    unsigned long i;

    s->bytes += n;
    s->calls++;
    for (i = 0; i < n; i++)
        s->checksum += p[i];
    if (s->abort_after && s->calls >= s->abort_after)
        return 1;
    return 0;
}

static void test_framing(void)
{
    struct http_client *c = http_client_new();
    struct http_response r;

    section("response framing");

    TI(http_get_url(c, U("h.test", "/len"), &r), HTTP_OK, "GET /len");
    TI(r.status, 200, "status 200");
    TI(r.body_len, 11, "Content-Length body length");
    TS(r.body, "hello world", "Content-Length body");
    TS(http_header_get(&r, "content-type"), "text/plain",
       "case-insensitive header lookup");
    http_response_free(&r);

    TI(http_get_url(c, U("h.test", "/empty"), &r), HTTP_OK, "GET /empty");
    TI(r.body_len, 0, "zero-length body");
    http_response_free(&r);

    TI(http_get_url(c, U("h.test", "/chunked"), &r), HTTP_OK, "GET /chunked");
    TS(r.body, "Hello World", "chunked body with extensions");
    TS(http_header_get(&r, "X-Trailer"), "yes", "trailer merged into headers");
    http_response_free(&r);

    TI(http_get_url(c, U("h.test", "/close"), &r), HTTP_OK, "GET /close");
    TS(r.body, "closed-body", "close-delimited body");
    http_response_free(&r);

    TI(http_get_url(c, U("h.test", "/teunknown"), &r), HTTP_OK,
       "unknown Transfer-Encoding");
    TS(r.body, "unknown-coding", "falls back to close framing");
    http_response_free(&r);

    TI(http_get_url(c, U("h.test", "/continue"), &r), HTTP_OK, "100 Continue");
    TI(r.status, 200, "informational response skipped");
    TS(r.body, "after-continue", "real response read");
    http_response_free(&r);

    TI(http_get_url(c, U("h.test", "/204"), &r), HTTP_OK, "204");
    TI(r.status, 204, "204 status");
    TI(r.body_len, 0, "204 has no body whatever Content-Length says");
    http_response_free(&r);

    TI(http_get_url(c, U("h.test", "/fold"), &r), HTTP_OK, "folded header");
    TS(http_header_get(&r, "X-Folded"), "one two", "obs-fold unfolded");
    http_response_free(&r);

    TI(http_get_url(c, U("h.test", "/dup"), &r), HTTP_OK, "duplicate headers");
    TS(http_header_nth(&r, "X-Dup", 0), "one", "first duplicate");
    TS(http_header_nth(&r, "X-Dup", 1), "two", "second duplicate");
    T(http_header_nth(&r, "X-Dup", 2) == 0, "no third duplicate");
    http_response_free(&r);

    TI(http_get_url(c, U("h.test", "/case"), &r), HTTP_OK, "odd header case");
    TS(http_header_get(&r, "Content-Type"), "text/html", "case folding");
    http_response_free(&r);

    TI(http_get_url(c, U("h.test", "/status/500"), &r), HTTP_OK,
       "500 is a complete response");
    TI(r.status, 500, "500 status reported");
    TS(r.body, "err", "500 body delivered");
    http_response_free(&r);

    {
        struct http_request rq;
        memset(&rq, 0, sizeof(rq));
        rq.method = "HEAD";
        rq.url = U("h.test", "/len");
        TI(http_fetch(c, &rq, &r), HTTP_OK, "HEAD");
        TI(r.status, 200, "HEAD status");
        TI(r.body_len, 0, "HEAD has no body");
        TS(http_header_get(&r, "Content-Length"), "11",
           "HEAD keeps Content-Length");
        http_response_free(&r);
    }

    http_client_free(c);
}

static void test_errors(void)
{
    struct http_client *c = http_client_new();
    struct http_response r;
    struct http_request rq;

    section("hostile and broken servers");

    TI(http_get_url(c, U("h.test", "/cutbody"), &r),
       HTTP_E_RECV, "server closes mid-body");
    TI(http_get_url(c, U("h.test", "/cutchunk"), &r),
       HTTP_E_RECV, "server closes mid-chunk");
    TI(http_get_url(c, U("h.test", "/bighdr"), &r),
       HTTP_E_TOOBIG, "over-long header line");
    TI(http_get_url(c, U("h.test", "/manyhdr"), &r),
       HTTP_E_TOOBIG, "over-large header block");
    TI(http_get_url(c, U("h.test", "/smuggle"), &r),
       HTTP_E_PROTO, "conflicting Content-Length");

    memset(&rq, 0, sizeof(rq));
    rq.url = U("h.test", "/stall");
    rq.header_timeout_ms = 800;
    rq.body_timeout_ms = 800;
    {
        unsigned long t0 = 0, t1 = 0;
        struct timeval tv;
        gettimeofday(&tv, 0);
        t0 = (unsigned long)tv.tv_sec * 1000 + (unsigned long)tv.tv_usec / 1000;
        TI(http_fetch(c, &rq, &r), HTTP_E_TIMEOUT, "body timeout fires");
        gettimeofday(&tv, 0);
        t1 = (unsigned long)tv.tv_sec * 1000 + (unsigned long)tv.tv_usec / 1000;
        T(t1 - t0 < 4000, "body timeout fired promptly");
        printf("  body timeout took %lu ms\n", t1 - t0);
    }

    rq.url = U("h.test", "/stallhdr");
    TI(http_fetch(c, &rq, &r), HTTP_E_TIMEOUT, "header timeout fires");

    TI(http_get_url(c, "ftp://h.test/x", &r), HTTP_E_SCHEME,
       "unregistered scheme");
    TI(http_get_url(c, "not a url", &r), HTTP_E_URL, "malformed URL");
    TI(http_get_url(c, "/relative", &r), HTTP_E_URL, "relative URL rejected");

    /* Body cap. */
    memset(&rq, 0, sizeof(rq));
    rq.url = U("h.test", "/big10");
    rq.max_body = 1024UL * 1024UL;
    TI(http_fetch(c, &rq, &r), HTTP_E_TOOBIG, "max_body enforced");

    /* Sink abort. */
    {
        struct sink_state s;
        memset(&s, 0, sizeof(s));
        s.abort_after = 2;
        memset(&rq, 0, sizeof(rq));
        rq.url = U("h.test", "/big10");
        rq.sink = counting_sink;
        rq.sink_user = &s;
        TI(http_fetch(c, &rq, &r), HTTP_E_ABORT, "sink can abort a transfer");
    }

    http_client_free(c);
}

static void test_redirects(void)
{
    struct http_client *c = http_client_new();
    struct http_response r;
    struct http_request rq;
    int codes[] = { 301, 302, 303, 307, 308 };
    const char *want_get[] = { "GET|0", "GET|0", "GET|0", "GET|0", "GET|0" };
    /* POST: 301/302/303 rewrite to GET, 307/308 keep POST and the body. */
    const char *want_post[] = { "GET|0", "GET|0", "GET|0", "POST|9",
                                "POST|9" };
    char path[64];
    int i;

    section("redirects");

    for (i = 0; i < 5; i++) {
        snprintf(path, sizeof(path), "/redir/%d", codes[i]);
        TI(http_get_url(c, U("h.test", path), &r), HTTP_OK, "GET redirect");
        TI(r.status, 200, "landed on 200");
        TI(r.redirects, 1, "one hop");
        TS(r.body, want_get[i], "GET method after redirect");
        T(strstr(r.final_url, "/method") != 0, "final URL updated");
        http_response_free(&r);
    }
    for (i = 0; i < 5; i++) {
        snprintf(path, sizeof(path), "/redir/%d", codes[i]);
        memset(&rq, 0, sizeof(rq));
        rq.method = "POST";
        rq.url = U("h.test", path);
        rq.body = "some-body";
        rq.body_len = 9;
        rq.content_type = "text/plain";
        TI(http_fetch(c, &rq, &r), HTTP_OK, "POST redirect");
        TS(r.body, want_post[i], "POST method rewriting");
        http_response_free(&r);
    }

    TI(http_get_url(c, U("h.test", "/relredir"), &r), HTTP_OK,
       "relative Location");
    TS(r.body, "GET|0", "relative Location resolved");
    http_response_free(&r);

    TI(http_get_url(c, U("h.test", "/loop1"), &r), HTTP_E_LOOP,
       "redirect loop detected");
    TI(http_get_url(c, U("h.test", "/chain/1"), &r), HTTP_E_REDIR,
       "hop limit enforced");

    memset(&rq, 0, sizeof(rq));
    rq.url = U("h.test", "/redir/302");
    rq.flags = HTTP_F_NO_REDIRECT;
    TI(http_fetch(c, &rq, &r), HTTP_OK, "redirect suppressed");
    TI(r.status, 302, "3xx returned as-is");
    TS(http_header_get(&r, "Location"), "/method", "Location visible");
    http_response_free(&r);

    memset(&rq, 0, sizeof(rq));
    rq.url = U("h.test", "/chain/1");
    rq.max_redirects = 3;
    TI(http_fetch(c, &rq, &r), HTTP_E_REDIR, "custom hop limit");

    http_client_free(c);
}

static void test_post(void)
{
    struct http_client *c = http_client_new();
    struct http_response r;
    char form[256];
    char enc[128];

    section("POST");

    url_pct_encode("hello world & more", ~0UL, URL_COMP_FORM, enc,
                   sizeof(enc));
    snprintf(form, sizeof(form), "q=%s&n=2", enc);
    TI(http_post_url(c, U("h.test", "/post"), form, strlen(form),
                     "application/x-www-form-urlencoded", &r),
       HTTP_OK, "form POST");
    {
        char want[320];
        snprintf(want, sizeof(want),
                 "POST|application/x-www-form-urlencoded|%s", form);
        TS(r.body, want, "server saw the form body and content type");
    }
    http_response_free(&r);

    TI(http_post_url(c, U("h.test", "/post"), "", 0, "text/plain", &r),
       HTTP_OK, "empty POST");
    TS(r.body, "POST|-|", "empty POST sends Content-Length: 0");
    http_response_free(&r);

    http_client_free(c);
}

static void test_encoding(void)
{
    struct http_client *c = http_client_new();
    struct http_response r;
    struct http_request rq;
    char want[8192];
    int i;

    section("content encoding");

    want[0] = '\0';
    for (i = 0; i < 500; i++)
        strcat(want, "gzip-payload.");
    TI(http_get_url(c, U("h.test", "/gzip"), &r), HTTP_OK, "gzip GET");
    TI(r.body_len, strlen(want), "gzip decoded length");
    TS(r.body, want, "gzip decoded body");
    http_response_free(&r);

    want[0] = '\0';
    for (i = 0; i < 500; i++)
        strcat(want, "deflate-payload.");
    TI(http_get_url(c, U("h.test", "/deflate"), &r), HTTP_OK, "deflate GET");
    TI(r.body_len, strlen(want), "deflate decoded length");
    TS(r.body, want, "deflate decoded body");
    http_response_free(&r);

    want[0] = '\0';
    for (i = 0; i < 500; i++)
        strcat(want, "gzip-chunked.");
    TI(http_get_url(c, U("h.test", "/gzipchunked"), &r), HTTP_OK,
       "gzip over chunked");
    TS(r.body, want, "gzip+chunked decoded body");
    http_response_free(&r);

    TI(http_get_url(c, U("h.test", "/badgzip"), &r), HTTP_E_DECODE,
       "corrupt gzip rejected");

    /* Raw bytes on request. */
    memset(&rq, 0, sizeof(rq));
    rq.url = U("h.test", "/gzip");
    rq.flags = HTTP_F_NO_DECODE;
    TI(http_fetch(c, &rq, &r), HTTP_OK, "NO_DECODE fetch");
    T(r.body_len > 0 && (unsigned char)r.body[0] == 0x1f,
      "coded bytes handed back untouched");
    http_response_free(&r);

    /* Advertising: with a decompressor we ask for gzip. */
    TI(http_get_url(c, U("h.test", "/hdrecho"), &r), HTTP_OK, "header echo");
    T(strstr(r.body, "Accept-Encoding: gzip, deflate") != 0,
      "Accept-Encoding advertised");
    T(strstr(r.body, "Host: h.test:") != 0, "Host header sent");
    T(strstr(r.body, "User-Agent: KestrelOS") != 0, "User-Agent sent");
    T(strstr(r.body, "Connection: keep-alive") != 0, "keep-alive requested");
    http_response_free(&r);

    /* Without one, we must not claim to accept encodings. */
    http_set_inflate(0);
    TI(http_get_url(c, U("h.test", "/hdrecho"), &r), HTTP_OK, "header echo 2");
    T(strstr(r.body, "Accept-Encoding: identity") != 0,
      "identity advertised without a decompressor");
    http_response_free(&r);
    TI(http_get_url(c, U("h.test", "/gzip"), &r), HTTP_E_DECODE,
       "encoded response refused without a decompressor");
    http_set_inflate(test_inflate);

    http_client_free(c);
}

static void test_keepalive(void)
{
    struct http_client *c = http_client_new();
    struct http_response r;
    struct http_stats st;
    int i;

    section("persistent connections");

    for (i = 0; i < 5; i++) {
        TI(http_get_url(c, U("ka.test", "/len"), &r), HTTP_OK, "pooled GET");
        TS(r.body, "hello world", "pooled body");
        http_response_free(&r);
    }
    TI(http_get_url(c, U("ka.test", "/conns"), &r), HTTP_OK, "connection count");
    TS(r.body, "1", "the server accepted exactly ONE connection for 6 requests");
    http_response_free(&r);

    http_client_stats(c, &st);
    TI(st.connections, 1, "client opened one transport");
    TI(st.reused, 5, "five requests reused it");
    printf("  requests=%lu connections=%lu reused=%lu bytes_in=%lu "
           "bytes_out=%lu\n", st.requests, st.connections, st.reused,
           st.bytes_in, st.bytes_out);

    /* A close-framed response must not be pooled. */
    TI(http_get_url(c, U("ka.test", "/close"), &r), HTTP_OK, "close response");
    http_response_free(&r);
    TI(http_get_url(c, U("ka.test", "/len"), &r), HTTP_OK, "after close");
    http_response_free(&r);
    http_client_stats(c, &st);
    T(st.connections >= 2, "a new connection was opened after Connection: close");

    /* A different origin gets its own connection. */
    TI(http_get_url(c, U("other.test", "/len"), &r), HTTP_OK, "other origin");
    http_response_free(&r);
    http_client_stats(c, &st);
    printf("  after mixed traffic: connections=%lu reused=%lu\n",
           st.connections, st.reused);

    http_client_drop_connections(c);
    http_client_free(c);
}

/* The server may close an idle pooled connection at any moment. A request
 * that dies on a reused socket has to be retried on a fresh one. */
static void test_stale_pool(void)
{
    struct http_client *c = http_client_new();
    struct http_response r;
    struct http_stats st;

    section("a pooled connection the server closed behind our back");

    TI(http_get_url(c, U("race.test", "/oneshot"), &r), HTTP_OK, "first GET");
    TS(r.body, "one-shot", "first body");
    http_response_free(&r);
    /* The server closed without a Connection: close, so the socket sitting
     * in the pool is dead and the client does not know it yet. */
    TI(http_get_url(c, U("race.test", "/len"), &r), HTTP_OK,
       "second GET retried transparently");
    TS(r.body, "hello world", "second body");
    http_response_free(&r);
    http_client_stats(c, &st);
    TI(st.connections, 2, "exactly one extra connection was opened");
    TI(st.requests, 2, "two requests, no user-visible failure");
    http_client_free(c);
}

static int hdr_calls;
static int hdr_status;
static unsigned long hdr_bytes_at_call;
static struct sink_state *hdr_sink;

static int on_headers_cb(void *u, const struct http_response *r)
{
    (void)u;
    hdr_calls++;
    hdr_status = r->status;
    hdr_bytes_at_call = hdr_sink ? hdr_sink->bytes : 0;
    return 0;
}

static int on_headers_abort(void *u, const struct http_response *r)
{
    (void)u;
    (void)r;
    return 1;
}

static void test_request_options(void)
{
    struct http_client *c = http_client_new();
    struct http_response r;
    struct http_request rq;
    struct sink_state s;

    section("request options and the headers callback");

    memset(&rq, 0, sizeof(rq));
    rq.url = U("opt.test", "/hdrecho");
    rq.accept = "text/html";
    rq.extra_headers = "X-Custom: value\r\nX-Second: 2\r\n";
    TI(http_fetch(c, &rq, &r), HTTP_OK, "custom headers");
    T(strstr(r.body, "Accept: text/html") != 0, "Accept overridden");
    T(strstr(r.body, "X-Custom: value") != 0, "extra header sent");
    T(strstr(r.body, "X-Second: 2") != 0, "second extra header sent");
    http_response_free(&r);

    memset(&rq, 0, sizeof(rq));
    rq.url = U("opt.test", "/hdrecho");
    rq.flags = HTTP_F_NO_KEEPALIVE;
    TI(http_fetch(c, &rq, &r), HTTP_OK, "no-keepalive");
    T(strstr(r.body, "Connection: close") != 0, "Connection: close requested");
    http_response_free(&r);

    /* The callback must fire once, before any body byte reaches the sink. */
    memset(&s, 0, sizeof(s));
    hdr_calls = 0;
    hdr_status = 0;
    hdr_sink = &s;
    memset(&rq, 0, sizeof(rq));
    rq.url = U("opt.test", "/big10");
    rq.sink = counting_sink;
    rq.sink_user = &s;
    rq.on_headers = on_headers_cb;
    TI(http_fetch(c, &rq, &r), HTTP_OK, "streamed with a headers callback");
    TI(hdr_calls, 1, "on_headers called exactly once");
    TI(hdr_status, 200, "on_headers saw the status");
    TI(hdr_bytes_at_call, 0, "on_headers ran before the first body byte");
    TI(s.bytes, 10UL * 1024 * 1024, "whole body still delivered");
    http_response_free(&r);
    hdr_sink = 0;

    /* And it must fire only for the final response of a redirect chain. */
    hdr_calls = 0;
    memset(&rq, 0, sizeof(rq));
    rq.url = U("opt.test", "/redir/302");
    rq.on_headers = on_headers_cb;
    TI(http_fetch(c, &rq, &r), HTTP_OK, "redirect with a headers callback");
    TI(hdr_calls, 1, "callback skipped the 3xx hop");
    TI(hdr_status, 200, "callback saw the final status");
    http_response_free(&r);

    memset(&rq, 0, sizeof(rq));
    rq.url = U("opt.test", "/len");
    rq.on_headers = on_headers_abort;
    TI(http_fetch(c, &rq, &r), HTTP_E_ABORT, "on_headers can abort");

    http_client_free(c);
}

static void test_cookies_wire(void)
{
    struct http_client *c = http_client_new();
    struct http_response r;
    struct cookie_jar *j;

    section("cookies over the wire");

    TI(http_get_url(c, U("cook.test", "/cookie/set"), &r), HTTP_OK,
       "Set-Cookie response");
    http_response_free(&r);

    j = http_client_jar(c);
    T(j != 0, "client has a jar");

    TI(http_get_url(c, U("cook.test", "/echo"), &r), HTTP_OK, "root echo");
    T(strstr(r.body, "a=1") != 0, "path=/ cookie sent at the root");
    T(strstr(r.body, "keep=5") != 0, "Max-Age cookie sent");
    T(strstr(r.body, "ho=7") != 0, "HttpOnly cookie sent over HTTP");
    T(strstr(r.body, "b=2") == 0, "path=/cookie cookie not sent at the root");
    T(strstr(r.body, "gone=4") == 0, "Max-Age=0 cookie never stored");
    T(strstr(r.body, "sec=3") == 0, "Secure cookie not sent over http");
    T(strstr(r.body, "evil=6") == 0, "public-suffix cookie rejected");
    printf("  Cookie at /: %s\n", r.body);
    http_response_free(&r);

    TI(http_get_url(c, U("cook.test", "/cookie/echo"), &r), HTTP_OK,
       "deep echo");
    T(strstr(r.body, "b=2") != 0, "path cookie sent under its path");
    T(strstr(r.body, "a=1") != 0, "root cookie also sent");
    printf("  Cookie at /cookie: %s\n", r.body);
    http_response_free(&r);

    /* A different host must not see them. */
    TI(http_get_url(c, U("elsewhere.test", "/echo"), &r), HTTP_OK,
       "other host echo");
    TS(r.body, "", "no cookies leak to another host");
    http_response_free(&r);

    /* Persist and reload into a second client. */
    T(cookie_jar_save(j, "/tmp/kestrel_cookies_wire.txt", 0) == 0, "jar saved");
    {
        struct http_client *c2 = http_client_new();
        cookie_jar_load(http_client_jar(c2), "/tmp/kestrel_cookies_wire.txt",
                        0);
        TI(http_get_url(c2, U("cook.test", "/echo"), &r), HTTP_OK,
           "reloaded jar echo");
        T(strstr(r.body, "keep=5") != 0,
          "persistent cookie survived a restart");
        T(strstr(r.body, "a=1") == 0, "session cookie did not");
        http_response_free(&r);
        http_client_free(c2);
    }

    http_client_free(c);
}

static void test_cache(void)
{
    struct http_client *c = http_client_new();
    struct http_response r;
    struct http_stats st;
    unsigned long bytes;
    int entries;
    char first[64];

    section("response cache");

    TI(http_get_url(c, U("cache.test", "/fresh"), &r), HTTP_OK, "first fetch");
    snprintf(first, sizeof(first), "%s", r.body);
    TI(r.from_cache, 0, "first fetch is not from cache");
    http_response_free(&r);

    TI(http_get_url(c, U("cache.test", "/fresh"), &r), HTTP_OK, "second fetch");
    TS(r.body, first, "same body served");
    TI(r.from_cache, 1, "second fetch came from the cache");
    http_response_free(&r);

    TI(http_get_url(c, U("cache.test", "/hits"), &r), HTTP_OK, "hit count");
    TS(r.body, "1", "the server was only asked once");
    http_response_free(&r);

    http_cache_stat(http_client_cache(c), &bytes, &entries);
    T(entries >= 1, "cache holds the entry");
    printf("  cache: %d entries, %lu bytes\n", entries, bytes);

    /* no-store must never be cached. */
    TI(http_get_url(c, U("cache.test", "/nostore"), &r), HTTP_OK, "no-store 1");
    snprintf(first, sizeof(first), "%s", r.body);
    http_response_free(&r);
    TI(http_get_url(c, U("cache.test", "/nostore"), &r), HTTP_OK, "no-store 2");
    T(strcmp(r.body, first) != 0, "no-store response refetched");
    TI(r.from_cache, 0, "no-store never served from cache");
    http_response_free(&r);

    /* Conditional revalidation and 304. */
    TI(http_get_url(c, U("cache.test", "/cond"), &r), HTTP_OK, "cond first");
    TS(r.body, "cond-body", "conditional body");
    TI(r.from_cache, 0, "first conditional fetch is live");
    http_response_free(&r);

    TI(http_get_url(c, U("cache.test", "/cond"), &r), HTTP_OK, "cond second");
    TS(r.body, "cond-body", "body reused after 304");
    TI(r.status, 200, "304 turned back into the stored 200");
    TI(r.from_cache, 1, "served from cache after revalidation");
    TS(http_header_get(&r, "X-Revalidated"), "yes",
       "304 headers merged into the stored entry");
    http_response_free(&r);

    TI(http_get_url(c, U("cache.test", "/condstats"), &r), HTTP_OK,
       "conditional stats");
    TS(r.body, "1/1", "the server saw one 200 and one 304");
    http_response_free(&r);

    http_client_stats(c, &st);
    TI(st.cache_hits, 1, "one fresh cache hit");
    TI(st.revalidations, 1, "one revalidation");
    TI(st.not_modified, 1, "one 304");

    /* HTTP_F_REFRESH forces a revalidation even when fresh. */
    {
        struct http_request rq;
        memset(&rq, 0, sizeof(rq));
        rq.url = U("cache.test", "/fresh");
        rq.flags = HTTP_F_REFRESH;
        TI(http_fetch(c, &rq, &r), HTTP_OK, "forced refresh");
        http_response_free(&r);
        TI(http_get_url(c, U("cache.test", "/hits"), &r), HTTP_OK, "hits 2");
        TS(r.body, "2", "refresh went to the network");
        http_response_free(&r);
    }

    /* NO_CACHE bypasses in both directions. */
    {
        struct http_request rq;
        memset(&rq, 0, sizeof(rq));
        rq.url = U("cache.test", "/fresh");
        rq.flags = HTTP_F_NO_CACHE;
        TI(http_fetch(c, &rq, &r), HTTP_OK, "no-cache fetch");
        TI(r.from_cache, 0, "NO_CACHE skipped the cache");
        http_response_free(&r);
    }

    /* LRU eviction under a tiny budget. */
    {
        struct http_cache *small = http_cache_new(4096);
        char hdr[256];
        char key[64];
        int i;
        snprintf(hdr, sizeof(hdr),
                 "Cache-Control: max-age=600\nContent-Type: text/plain\n");
        for (i = 0; i < 40; i++) {
            char body[512];
            memset(body, 'x', sizeof(body));
            snprintf(key, sizeof(key), "GET http://x/%d", i);
            http_cache_store(small, key, 200, hdr, body, sizeof(body),
                             1700000000L, "GET");
        }
        http_cache_stat(small, &bytes, &entries);
        T(bytes <= 4096, "cache stayed under its byte budget");
        T(entries > 0 && entries < 40, "LRU evicted older entries");
        printf("  LRU cache after 40 stores: %d entries, %lu bytes\n",
               entries, bytes);
        /* The most recent key must still be there. */
        snprintf(key, sizeof(key), "GET http://x/39");
        TI(http_cache_lookup(small, key, 1700000000L, 0, 0),
           HTTP_CACHE_FRESH, "newest entry survived");
        snprintf(key, sizeof(key), "GET http://x/0");
        TI(http_cache_lookup(small, key, 1700000000L, 0, 0),
           HTTP_CACHE_MISS, "oldest entry evicted");
        http_cache_free(small);
    }

    http_client_free(c);
}

static void test_streaming(void)
{
    struct http_client *c = http_client_new();
    struct http_response r;
    struct http_request rq;
    struct sink_state s;

    section("streaming and large bodies");

    memset(&s, 0, sizeof(s));
    memset(&rq, 0, sizeof(rq));
    rq.url = U("big.test", "/big10");
    rq.sink = counting_sink;
    rq.sink_user = &s;
    TI(http_fetch(c, &rq, &r), HTTP_OK, "10 MiB streamed");
    TI(s.bytes, 10UL * 1024 * 1024, "sink saw every byte");
    T(s.calls > 100, "delivered incrementally, not in one lump");
    TI(s.checksum, (unsigned long)'K' * 10UL * 1024 * 1024, "content correct");
    T(r.body == 0, "no in-memory copy when streaming");
    printf("  10 MiB in %d sink calls (avg %lu bytes)\n", s.calls,
           s.bytes / (unsigned long)s.calls);
    http_response_free(&r);

    /* And the one-shot form of the same body. */
    memset(&rq, 0, sizeof(rq));
    rq.url = U("big.test", "/big10");
    TI(http_fetch(c, &rq, &r), HTTP_OK, "10 MiB buffered");
    TI(r.body_len, 10UL * 1024 * 1024, "buffered length");
    T(r.body[0] == 'K' && r.body[10UL * 1024 * 1024 - 1] == 'K',
      "buffered content correct");
    http_response_free(&r);

    http_client_free(c);
}

/* ===================================================================== */

int main(void)
{
    printf("libweb HTTP/URL/cookie/cache test harness\n");

    test_url_basics();
    test_url_encode();
    test_url_rfc3986();
    test_cookies_unit();

    if (start_server() != 0) {
        printf("\ncould not start the python3 test server; "
               "network tests skipped\n");
        printf("\n%d checks, %d failures\n", g_checks, g_fails);
        return g_fails ? 1 : 1;
    }
    printf("\npython3 test server listening on 127.0.0.1:%d\n", g_port);

    http_register_scheme("http", sock_factory, 0);
    http_set_inflate(test_inflate);

    test_framing();
    test_errors();
    test_redirects();
    test_post();
    test_encoding();
    test_keepalive();
    test_stale_pool();
    test_request_options();
    test_cookies_wire();
    test_cache();
    test_streaming();

    stop_server();

    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
