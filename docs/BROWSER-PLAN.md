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
APIs) is a multi-year project for a team. The current tree-walking interpreter
has an ES5 core, bounded arrow functions, `let`/`const` syntax with `var`
semantics, `Uint8Array`, UTF-8 text codecs, live `URL`/`URLSearchParams`,
bounded `Map`/`Set`, object-keyed `WeakMap`/`WeakSet`, common modern
Object/Array/Number static helpers, bounded Array/String search, flattening,
replacement, and padding helpers, explicit Array iterators, and bounded
`Object.fromEntries()`,
bounded static module graphs, a
Promise/microtask subset (`all`, `race`, and `finally` included),
asynchronous `fetch`/`Response` with
`AbortController` cancellation, and a bounded i32-only WebAssembly MVP core.
Those features run controlled applications and tests; they do not turn it into
a full modern-JavaScript engine. `import.meta.url` and literal-string
`import()` are supported, but literal dynamic imports are eagerly resolved and
evaluated before their Promise is returned. Inline `import.meta.url` is the
document URL. External modules follow fetch-style CORS/cookie rules and require
a JavaScript MIME type; supported import statements preserve source after
their terminating semicolon. Module bindings are not live, computed imports
and most ES2015+ syntax are absent, and large framework applications remain
outside the compatibility boundary.

**Crypto.** The TLS implementation is written from the specifications and is
**not audited**. It is constant-time where that is cheap to arrange and not
where it is not. It is good enough to fetch a public web page; it should not be
trusted with anything that matters.

**Rendering.** The renderer has alpha compositing, conventional block/inline/
table/floating/positioned layout, and bounded single-line flex-row and
flex-column subsets.
Inline SVG supports basic rectangles, lines, circles, ellipses, text,
polygons, polylines, presentation fill/stroke, and `viewBox`. Its bounded path
subset accepts move, line, horizontal/vertical line, quadratic/cubic curve,
arc, and close commands, but arcs use an endpoint fallback and this is not a
complete path or SVG implementation. Only one bounded subpath is consumed.
Default `viewBox` painting is centered and aspect-preserving with meet
behavior, and one missing intrinsic dimension can be derived from its ratio.
Coordinate, point, shape, path-flattening, pixel, and total raster-work caps
bound processing. Transforms, gradients, masks, and complete
`preserveAspectRatio` behavior remain absent. Video posters and audio/video
fallback boxes render, but codecs and playback do not exist. Fonts are built-in
bitmaps and downloadable webfonts are not implemented. Pages should be
legible, not pixel-identical to Chrome.

**Transport.** HTTPS is HTTP/1.1 protected by verified TLS 1.3. HTTP/2 and its
HPACK/framing layer are not implemented. HTTP/3 would additionally require a
QUIC stack, QPACK, and UDP transport work and is also not implemented.

**What should work well:** documentation sites, wikis, news articles, blogs,
forums, plain-HTML applications, and anything designed to degrade gracefully.
That is a large and genuinely useful part of the web.

## Implementation status

- **Wave A — foundations (complete):** DEFLATE; hashes, HMAC/HKDF and AEADs; bignum, RSA,
  X25519, P-256, X.509; multi-size and multi-weight fonts; the HTML5 tokenizer
  and DOM tree.
- **Wave B — the engine (complete):** TLS 1.3 on the crypto; the CSS parser and cascade;
  the layout engine; PNG, GIF and JPEG.
- **Wave C — usable web product (in progress):** HTTP, cache, cookies,
  JavaScript/DOM bindings, external resources, history, forms, and controlled
  web tests are implemented. Bounded static modules, Promise jobs and
  `all`/`race`/`finally`, asynchronous fetch with redirect/CORS controls,
  `Response.text()`/`json()`/`arrayBuffer()`, an i32-only WebAssembly MVP core,
  `import.meta.url`, eagerly resolved literal-string `import()`, arrow
  functions, `Uint8Array`, UTF-8 text codecs, live `URL`/`URLSearchParams`,
  `AbortController`/`AbortSignal` and queued fetch cancellation,
  constructible `Event`/`CustomEvent`/`EventTarget` with bounded bubbling,
  constructible normalized `Headers` collections shared with responses,
  constructible and independently cloneable text/byte/JSON `Response` objects,
  constructible `Request` objects with bounded real header transport,
  bounded `Blob`/`File` construction, asynchronous FileReader, body readers,
  Fetch-resolved object URLs, Request mode/credentials/redirect controls,
  response factories and redirect metadata, kernel-backed Web Crypto
  randomness and SHA-1/SHA-2 digests, SMP-aware Navigator metadata and queued
  `sendBeacon()`, CSS-engine-backed `matchMedia()` and viewport metrics,
  complete Location/document URL metadata and mutation, ordered
  string/Blob/File `FormData` with bounded multipart HTTP encoding and decoding,
  bounded same-document History routing, modern DOM traversal/mutation,
  multi-token class lists, general CSS declaration access, bounded
  `Map`/`Set`, object-keyed `WeakMap`/`WeakSet`, and common modern
  Object/Array/Number static and bounded Array/String prototype helpers,
  explicit Array iterators and bounded `Object.fromEntries()`,
  Node/Element/HTML/SVG identity, namespace-aware element construction and
  lookup, a live existing-key DOMStringMap dataset view, comment/child/root
  APIs, off-document DocumentFragment construction, and bounded Range/
  Selection editing state,
  bounded asynchronous MutationObserver records, forwarded timer arguments
  and cancellable animation/idle callbacks, bounded event capture/bubbling
  with listener option records, and constructible UI/mouse/keyboard/input/
  pointer event interfaces,
  origin-partitioned Web Storage, row/column flex layout subsets,
  MIME/CORS-checked external modules, bounded
  aspect-preserving inline SVG shapes/paths, and video-poster/media fallbacks
  are present.
- **Wave D — broader compatibility (future):** tabs, bookmarks, find, zoom,
  downloads, lexical ES2015+ semantics and broader syntax, live module
  bindings and computed/on-demand dynamic import, complete Fetch/DOM/Web APIs,
  full flex/grid and SVG, downloadable webfonts, media codecs/playback,
  HTTP/2, and HTTP/3.

This is not a plan to promise compatibility with every website. Each added
surface must remain bounded and testable, and the documentation must distinguish
a useful subset from standards conformance.
