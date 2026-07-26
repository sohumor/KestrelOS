# Building a real browser

The browser that shipped with the desktop could fetch plain HTTP and render a
flat run of styled text. That is a demo. This document is the plan for making it
a genuinely capable browser, what that requires, and — just as important — where
the honest ceiling is.

## What actually blocks the web

In order of how much of the web each one unlocks:

1. **TLS.** Without HTTPS roughly 95% of the web is simply unreachable, and no
   amount of rendering quality compensates. This is the single most valuable
   thing to build and also the hardest: X25519, ChaCha20-Poly1305, SHA-2, HKDF,
   the TLS 1.3 handshake, X.509 parsing, and RSA/ECDSA signature verification —
   all from scratch.
2. **CSS.** Every real page is laid out by its stylesheet. Without a cascade,
   pages render as an undifferentiated column of text no matter how good the
   HTML parser is.
3. **A real layout engine.** Block and inline formatting, the box model, tables
   with measured columns, floats. A flat box list cannot express any of it.
4. **Images.** Which means DEFLATE (for PNG), and DEFLATE also unlocks
   `Content-Encoding: gzip`, which a large fraction of servers use.
5. **A real DOM.** A tree, not a token stream — required by CSS selectors, by
   JavaScript, and by correct handling of malformed markup.
6. **HTTP that a modern server likes.** Keep-alive, compression, cookies,
   caching, redirects, POST.
7. **JavaScript.** Last, deliberately — see below.

## Architecture

The old browser was two files. This is a library stack, so each piece is
testable on its own and reusable:

```
apps/browser.c      the application: chrome, tabs, history, input
libweb/             dom.c  css.c  layout.c  paint.c  http.c  cookies.c
libimg/             png.c  gif.c  jpeg.c
libtls/             tls.c  x509.c  bignum.c  rsa.c  ecc.c  aead.c  hash.c
libz/               inflate.c
libgui/             font rendering at multiple sizes and weights
```

Everything below `apps/` is a static archive, so a program that does not use
TLS does not link it. "Lightweight" is measured, not asserted: each library
reports its own size and the browser's total is tracked.

## The honest ceiling

**JavaScript.** A production engine (JIT, full ES2023, the complete DOM and Web
APIs) is a multi-year project for a team. What is achievable here is a
tree-walking interpreter for an ES5 subset with DOM bindings: enough to run
simple scripts that manipulate the page, handle a click, or validate a form. It
will not run Gmail, React, or anything that expects `fetch`, promises,
modules, or a real event loop — and pages built entirely in JavaScript will
render as whatever their `<noscript>` fallback is, which is usually nothing.

**Crypto.** The TLS implementation is written from the specifications and is
**not audited**. It is constant-time where that is cheap to arrange and not
where it is not. It is good enough to fetch a public web page; it should not be
trusted with anything that matters.

**Rendering.** The framebuffer has no alpha compositing and the fonts are
bitmaps, so gradients, shadows, rounded corners, transforms and web fonts are
out of reach. Pages will be legible and correctly laid out, not pixel-identical
to Chrome.

**What should work well:** documentation sites, wikis, news articles, blogs,
forums, plain-HTML applications, and anything designed to degrade gracefully.
That is a large and genuinely useful part of the web.

## Order of work

- **Wave A — foundations:** DEFLATE; hashes, HMAC/HKDF and AEADs; bignum, RSA,
  X25519, P-256, X.509; multi-size and multi-weight fonts; the HTML5 tokenizer
  and DOM tree.
- **Wave B — the engine:** TLS 1.3 on the crypto; the CSS parser and cascade;
  the layout engine; PNG, GIF and JPEG.
- **Wave C — the product:** the HTTP stack, the JavaScript interpreter, the
  browser UI (tabs, history, bookmarks, find, zoom, downloads), and the tests.
