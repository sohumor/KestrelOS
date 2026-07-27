# TLS 1.3

`libtls/tls.c` is a TLS 1.3 client written from RFC 8446. It is what makes
`https://` reachable, which is roughly 95% of the web; without it the
browser is a museum piece. It sits on the crypto that was already in
`libtls` — SHA-2, HMAC and HKDF from `hash.h`, ChaCha20-Poly1305 and
AES-GCM from `aead.h`, X25519 and P-256 from `ecc.h`, RSA verification
from `rsa.h`, and certificates from `x509.h` — and adds the handshake, the
key schedule and the record layer.

Client only, TLS 1.3 only. There is no TLS 1.2: that would mean RSA key
transport, CBC with MAC-then-encrypt, the PRF and renegotiation, which is
more machinery than everything in this file put together, and every server
worth reaching has spoken 1.3 for years. A server that will not do 1.3 is
**detected and named**, not silently worked around:

```
the server negotiated TLS 1.2, which this client does not implement
(it sent no supported_versions extension)
```

## What is implemented

**Handshake.** ClientHello with `server_name`, `supported_versions`,
`supported_groups`, `key_share`, `signature_algorithms` and ALPN;
HelloRetryRequest including the cookie echo and the synthetic
`message_hash` transcript of RFC 8446 4.4.1; ServerHello,
EncryptedExtensions, CertificateRequest, Certificate, CertificateVerify
and Finished; then the client's Certificate (empty, if one was asked for)
and Finished. Middlebox compatibility mode: a 32-byte legacy session id
that the server must echo, and the dummy `change_cipher_spec` record.

**Key schedule**, exactly as in section 7.1 — early secret, handshake
secret, master secret, the four traffic secrets, and `traffic upd` for key
updates — built on the `HKDF-Expand-Label` and `Derive-Secret` helpers in
`hash.h`. The transcript hash is the part implementations get subtly
wrong, so it is not trusted on inspection: every secret is compared
against the ones OpenSSL logs for the same connection (see Testing).

**Record layer.** TLSCiphertext framing, the nonce as sequence-number-XOR-
write-IV, the real content type hidden at the end of the plaintext,
padding removal, a record split across several TCP reads, several records
in one read, several handshake messages in one record, one handshake
message across many records, and post-handshake `KeyUpdate` in both
directions.

**Cipher suites.** `TLS_AES_128_GCM_SHA256`, `TLS_AES_256_GCM_SHA384` and
`TLS_CHACHA20_POLY1305_SHA256`, over X25519 or P-256.

**Certificates.** The chain is verified with `x509_verify_chain` against a
trust store, with the host name checked. If verification fails the API
says exactly why, in a sentence meant for a person, so the browser can
offer to proceed rather than either lying or giving up silently.

**Alerts** are sent and received, and a received alert is translated:
`protocol_version` becomes "it does not support TLS 1.3 (it most likely
requires TLS 1.2, which this client does not implement)", not "alert 70".

**Close.** `close_notify` on shutdown, and a connection that dies without
one is reported as `TLS_E_TRUNCATED` rather than passed off as a clean end
of file — that distinction is the whole point of the truncation attack.

## Using it

```c
struct tls_options o;
struct tls_error err;
struct tls_conn *c;

tls_options_default(&o);            /* verify required, ALPN http/1.1 */
c = tls_connect("example.com", 443, &o, &err);
if (!c) {
    printf("%s\n", err.msg);        /* one sentence, already readable */
    return;
}
tls_write(c, req, len);
n = tls_read(c, buf, sizeof buf);   /* >0, 0 at a clean end, <0 on error */
tls_close(c);                       /* sends close_notify */
```

`tls_info()` reports what was negotiated: cipher, group, ALPN, whether the
chain verified and why not if it did not, the leaf's subject, issuer and
validity, and whether a HelloRetryRequest happened.

### Plugging into the HTTP client

`libweb/http.h` defines the contract:

```c
struct http_transport {
    void *ctx;
    int (*read)(void*, void*, int);
    int (*write)(void*, const void*, int);
    void (*close)(void*);
    int (*set_timeout)(void*, int);
};
```

`tls_transport_open()` is a factory with exactly that shape, so:

```c
#include "http.h"
#include "tls.h"

static struct tls_options https_opts;    /* must outlive the connections */

tls_options_default(&https_opts);
TLS_REGISTER_HTTPS(&https_opts);         /* binds the "https" scheme */
```

`TLS_REGISTER_HTTPS` is a macro on purpose. `libtls` never references
`http_register_scheme` itself, so a program that links `libtls` does not
drag in `libweb`; the reference appears only in the translation unit that
uses the macro. `struct tls_transport` is declared in `tls.h` with the
same layout as `struct http_transport` for the same reason, and
`TLS_ASSERT_TRANSPORT_LAYOUT()` checks at compile time that the two have
not drifted apart. `http_fetch()` can only hand a numeric code back, so
the sentence explaining a failed connection is kept in
`tls_last_transport_error()`.

## The trust store

`libtls/roots.c` carries 18 root certificates as PEM. They are public
certificates published by the CAs — data, not code — and a TLS client
without them can verify nothing at all. Provenance, because it is the
entire basis of trust for every HTTPS connection this system makes:

- **Source:** the Mozilla NSS root store as shipped in Debian/Ubuntu's
  `ca-certificates` package version `20260601~26.04.1`, which installs
  the individual PEM files under `/usr/share/ca-certificates/mozilla/`.
- **Taken:** 2026-07-26, copied byte for byte; nothing was re-encoded.
- **Chosen:** the widely used roots this build can actually verify
  against, which means RSA roots only — see the P-384 limitation below.

ISRG Root X1 · DigiCert Global Root G2 · DigiCert Trusted Root G4 ·
DigiCert TLS RSA4096 Root G5 · GTS Root R1 · GlobalSign (R3) · GlobalSign
Root R46 · Amazon Root CA 1 · USERTrust RSA · Sectigo Public Server
Authentication Root R46 · COMODO RSA · Go Daddy Root G2 · Starfield Root
G2 · Microsoft RSA Root 2017 · IdenTrust Commercial Root CA 1 · Certainly
Root R1 · SSL.com Root RSA · SSL.com TLS RSA Root CA 2022

This is deliberately a small set, not a replacement for Mozilla's ~150. A
site anchored anywhere else fails with `no trusted root certificate issued
'...'`, which is the honest answer. Extra roots are read from
`/etc/ssl/roots.pem` at startup if that file exists, and are simply added
to the list.

**Verification never fails open.** With the default
`TLS_VERIFY_REQUIRED`, a chain that does not verify aborts the handshake
with `TLS_E_CERT`, `err.cert_failure = 1` and the specific reason. A
caller that wants to connect anyway must ask for `TLS_VERIFY_NONE`
explicitly, and even then `tls_info().verified` is 0 and
`tls_info().cert_error` still holds the reason.

### Two things the chain builder does that RFC 5280 does not spell out

*Prefix search.* `x509_verify_chain()` anchors the top of the chain it is
given. Real servers routinely append a **cross-signed** copy of their root
— GTS Root R1 signed by GlobalSign, SSL.com's roots signed by Comodo —
so the last certificate names an issuer nobody has while the one below it
is issued by a root that is right there in the store. `tls.c` therefore
tries the full chain and then each shorter prefix of it, taking the first
that verifies completely. Trimming a trailing certificate can only remove
a link the store already covers; every attempt is a full check, including
expiry, host name, basic constraints, key usage and every signature.
Without this, `example.com`, `www.iana.org` and `www.rfc-editor.org` all
fail.

*The RSA fallback.* `ecc.h` implements P-256 but not P-384, and a server
chooses which of its certificates to send from the client's
`signature_algorithms`. Several large CAs put ECDSA leaves under P-384
intermediates. So when verification fails **and** the chain contained a
key on a curve this build cannot check, `tls_connect()` reconnects once
with the ECDSA algorithms removed; a server that also holds an RSA
certificate then sends that chain instead, and it verifies. Nothing is
weakened — the second attempt is a full handshake with full verification —
and `tls_info().rsa_fallback` says it happened. Set
`options.no_rsa_fallback` to switch it off. This is what makes everything
behind Cloudflare reachable.

## The honest ceiling

- **Not audited.** Written from the specifications by one author. It is
  constant-time where that was cheap to arrange and not where it was not:
  `aead.h` says its AES T-tables and GHASH table are cache-timing
  observable, and that is not fixed here. Good enough to fetch a public
  web page; do not trust it with anything that matters.
- **No P-384 or P-521 ECDSA, no Ed25519**, because `ecc.h` has X25519 and
  P-256 only. A chain that needs one fails with
  `'GTS Root R4' uses curve P-384, which this build does not implement`.
  The RSA fallback above rescues most but not all of these; sites on an
  ECDSA-only hierarchy (Let's Encrypt's E-series, for instance) stay
  unreachable.
- **No SHA-1.** `hash.h` does not implement it, so a certificate still
  signed with SHA-1 in 2026 is refused. That is the right answer.
- **No session resumption, PSK, 0-RTT or session tickets.** Tickets are
  parsed and dropped. Every connection is a full handshake, so HTTPS
  costs one extra round trip and one signature verification per
  connection; `libweb`'s connection pool is what keeps that off the
  critical path.
- **No client certificates.** A `CertificateRequest` is answered with an
  empty `Certificate`, which is what the RFC requires and which servers
  that merely *ask* accept.
- **No OCSP, no CRLs, no certificate transparency, no name constraints** —
  those are `x509.c`'s stated gaps, and they are inherited. A revoked
  certificate will be accepted.
- **Entropy is the weakest link on the target.** The DRBG is HMAC-DRBG
  over SHA-256, seeded from RDSEED/RDRAND when the CPU has them, the time
  stamp counter sampled repeatedly, the wall clock, the process id and a
  few addresses. `/dev/random` is stirred in but the kernel says plainly
  it is not cryptographic. **If the CPU has no RDRAND, the only real
  entropy is timer jitter, and the keys are only as unpredictable as
  that.** `tls_entropy_is_weak()` and `tls_info().weak_entropy` report it
  so the UI can say so out loud, and `tls_add_entropy()` lets the browser
  stir in mouse and key timings. Do not paper over this.
- **The `signature_algorithms` list advertises exactly what can be
  verified** and nothing else, so servers do not pick chains that then
  have to be rejected.

## Memory and limits

Measured, not guessed, because the user stack is 64 KiB:

| | |
|---|---|
| one connection | 113,800 bytes of heap, one block |
| trust store | 49,672 bytes, heap or static |
| deepest stack | ~14 KiB, inside x509 chain verification |
| `tls.o` | 41,064 bytes of text |
| `roots.o` | 33,129 bytes, almost all PEM |
| `apps/tlstest` | 148,211 bytes of text, whole program |
| largest handshake message | 32 KiB (`TLS_HS_MAX`) |
| largest record | 2^14 plaintext, 2^14+256 ciphertext, per the RFC |
| HelloRetryRequest cookie | 1 KiB |
| certificates in a chain | 10 (`X509_MAX_CHAIN`) |

Nothing recurses. Every length that arrives from the network is checked
against what is actually left in the buffer before it is used, and every
list is bounded.

## Testing

`tools/test_tls.c` builds `tls.c` for the host with `-DTLS_HOST`, which
swaps the `SYS_TCP_*` calls for BSD sockets and changes nothing else — the
record layer, the handshake and every parser are the same code the target
runs. `libweb`'s HTTP client was validated the same way.

```sh
gcc -Wall -Wextra -O2 -fsanitize=address,undefined -Ilibtls -DTLS_HOST \
    -o /tmp/test_tls \
    tools/test_tls.c libtls/tls.c libtls/roots.c libtls/hash.c \
    libtls/aead.c libtls/ecc.c libtls/rsa.c libtls/bignum.c libtls/x509.c
/tmp/test_tls              # 122 checks, 0 failures
/tmp/test_tls fetch example.com 443 /
```

Four kinds of test, which check each other:

1. **Interop with `openssl s_server`** — a real, independent TLS 1.3
   server: each cipher suite, each group, both HelloRetryRequest
   directions, an RSA leaf and an ECDSA P-256 leaf, a leaf+CA chain, a
   `CertificateRequest` answered with an empty certificate, ALPN accepted
   and ALPN refused.
2. **The key schedule against `s_server -keylogfile`.** All four traffic
   secrets are compared byte for byte with the ones OpenSSL logged for the
   same connection, for all three suites — twelve comparisons. This is the
   only check that can catch both sides of a home-grown test making the
   *same* mistake.
3. **A mock server** written in the harness on `hash.h`/`aead.h`/`ecc.h`
   directly, with RSA-PSS signing implemented on `bignum.h` so it can
   complete a real handshake in-process. That turns every negative case
   into a deterministic unit test.
4. **Fuzzing** under `-fsanitize=address,undefined`: 500 server flights
   with a byte flipped somewhere inside the transcript, and 200 random
   byte streams in place of a ServerHello.

What the fuzz run asserts is worth stating precisely: the whole server
flight is covered by the transcript, so **a mutation anywhere in it must
be rejected** — by a parser, by the chain, by the signature, or finally by
the Finished MAC. One accepted flight would mean some byte of the
server's handshake is not actually authenticated. 500 mutations, 0
accepted, no sanitizer reports.

### Every failure, and what it says

| case | message |
|---|---|
| expired certificate | `'localhost' expired on 2024-03-01 (today is 2026-07-27)` |
| wrong host name | `the certificate is for 'not-localhost.example', not 'localhost'` |
| self-signed, no root | `no trusted root certificate issued 'localhost' (it names its issuer as 'localhost')` |
| empty trust store | `no trusted root certificates are loaded` |
| corrupted Finished | `the server's Finished message is wrong, so the handshake was tampered with or the two sides computed different keys` |
| corrupted record MAC | `a record from the server failed authentication (record 0 of this key)` |
| corrupted CertificateVerify | `the server's handshake signature does not verify against its certificate -- this connection is not authentic` |
| Finished with no Certificate | `the server sent Finished without a Certificate` |
| CertificateVerify first | `the server sent a CertificateVerify before its Certificate` |
| connection cut mid-handshake | `the connection was cut in the middle of the handshake (after the ServerHello)` |
| server that never answers | `the server stopped responding during the handshake (nothing arrived for 700 ms after the ClientHello)` |
| TLS 1.2 ServerHello | `the server negotiated TLS 1.2, which this client does not implement (it sent no supported_versions extension)` |
| `protocol_version` alert | `the server rejected the connection: it does not support TLS 1.3 (it most likely requires TLS 1.2, which this client does not implement) (TLS alert 70)` |
| `handshake_failure` alert | `the server rejected the connection: it could not agree on a cipher suite or key exchange group (TLS alert 40)` |
| plain HTTP on the port | `the server did not answer with TLS; it sent "HTTP/1.1 400 Bad Request", so it is probably speaking plain HTTP on this port` |
| oversized record | `the server sent a 20000 byte record, over the 16384 byte limit` |

### Real servers

The acceptance test is a real page off the real internet. `example.com`,
`www.rfc-editor.org`, `www.iana.org`, `www.kernel.org`, `www.python.org`,
`curl.se`, `www.debian.org` and `www.openbsd.org` all complete a verified
handshake and return their pages. For example:

```
$ /tmp/test_tls fetch example.com 443 /
cipher   TLS_CHACHA20_POLY1305_SHA256
group    x25519
alpn     http/1.1
verified 1
subject  example.com
issuer   Cloudflare TLS Issuing RSA CA 3
chain    4
----
HTTP/1.1 200 OK
Date: Mon, 27 Jul 2026 00:29:02 GMT
Content-Type: text/html
Transfer-Encoding: chunked
Connection: close
Server: cloudflare
...
<!doctype html><html lang="en"><head><title>Example Domain</title>...
---- 868 bytes
```

## On the target

`apps/tlstest.c` is the same thing over the kernel's own TCP stack:

```
tlstest example.com               fetch https://example.com/
tlstest example.com 443 /index.html
tlstest --roots                   list the trust store
tlstest --insecure ...            connect anyway, and print why it was bad
tlstest --head ...                headers only
tlstest --group p256              force a key exchange group
tlstest --suite chacha20          force a cipher suite
tlstest --retry                   force a HelloRetryRequest
```

It prints the negotiated cipher, group, ALPN, the certificate and the
verification result, and on failure the exact reason and whether the alert
came from the server or from us.
