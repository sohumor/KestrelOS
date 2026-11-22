# The Kestrel Browser

Kestrel Browser is a native browser built from the operating system's own
libraries. It does not embed Chromium, WebKit, Gecko, OpenSSL, or the host
operating system's network stack.

## Architecture

The navigation pipeline is:

```text
URL -> HTTP/1.1 (plain or over verified TLS 1.3) -> HTML DOM
    -> classic ES6+ scripts and bounded static/dynamic ES modules
    -> DOM events, Promise jobs, async/await, observers, and asynchronous fetch
    -> UA, inline, linked, and imported CSS (custom props, @media, Grid/Flex)
    -> computed styles -> layout -> paint (HTML, SVG, software Canvas 2D)
```

The major components are:

| Component | Responsibility |
|---|---|
| `apps/browser.c` | browser chrome, navigation, history, resources, forms, text mode, tab/reader/bookmark/download/inspector structures |
| `libweb/html.c`, `dom.c` | HTML5 tokenizer, tree construction, mutable DOM, `<canvas>` backing buffers |
| `libweb/css.c`, `style.c` | selectors, cascade, `@media` queries, `--*` custom properties, `calc()`/`light-dark()`, computed values |
| `libweb/layout.c`, `paint.c` | Grid, Flexbox, tables, floats, sticky/fixed positioning, controls, gradients/shadows/AA corners, SVG & Canvas painting |
| `libweb/http.c`, `cache.c`, `cookie.c` | HTTP/1.1 Keep-Alive connection pool, redirects, compression, cache, cookies |
| `libweb/jsdom.c`, `libweb/storage.c`, `libjs/` | ES6+ JavaScript (`libjs/crypto.c` SHA-1/256/384/512, TypedArrays + `DataView`, `URL`, `structuredClone`, `AbortController`, `Blob`/`FormData`), Canvas 2D (`CanvasGradient`, `ImageData`), `DOMParser`, observers, geometry, clipboard, origin storage, bounded WebAssembly, and live DOM bindings |
| `libimg/` | PNG, GIF, baseline and progressive (`SOF2`) JPEG, BMP, ICO/CUR (PNG + 1/4/8/24/32-bpp DIB), standalone fixed-point SVG rasterizer, QOI, and Netpbm (`P1`–`P6`) decoding |
| `libtls/` | verified TLS 1.3 and X.509 (up to 128 SANs) |

All response bodies and subresources have hard memory, nesting, execution,
and count limits. A malformed or hostile page should fail a resource or show a
truncation diagnostic rather than exhaust the kernel.

The implementation architecture is described in
[DESIGN-browser-runtime.md](DESIGN-browser-runtime.md). The short version is:
procedural parsing/layout/painting pipelines operate on compact data, major
resource owners are composed rather than inherited, and navigation and
asynchronous work use explicit bounded state machines.

## Navigation and networking

The browser accepts `file:`, `http:`, `https:`, and `view-source:` URLs (where
`view-source:<url>` fetches the inner `file:`, `http:`, or `https:` resource
and renders HTML-escaped, line-numbered source). A bare hostname is treated as
HTTP and an existing path is treated as a local file.

Network navigation supports:

- HTTP/1.1 Keep-Alive connection reuse (`HTTP_POOL_MAX = 6`, `30 s` idle window);
- chunked and content-length response bodies;
- gzip and deflate content coding;
- redirects with method rewriting;
- conditional cache revalidation;
- persistent RFC 6265-style cookies;
- verified TLS 1.3, SNI, hostname validation (up to `X509_MAX_SAN` = 128 SANs),
  and trusted roots;
- linked stylesheets, recursive bounded `@import`, scripts, and images
  (`Accept: image/png,image/jpeg,image/gif,image/bmp,image/svg+xml,image/x-icon,*/*;q=0.5`,
  covering PNG, GIF, BMP, baseline/progressive `SOF2` JPEG, ICO/CUR, SVG, QOI,
  and Netpbm `P1`–`P6`);
- relative resource URLs, `<base href>`, and CSS URLs relative to their
  stylesheet response URL;
- mixed-content blocking on HTTPS pages.

Scripted `fetch()` is asynchronous at browser yield points. It has bounded
redirect handling, `AbortController`/`AbortSignal` queued cancellation,
same-origin/CORS checks, and Promise-backed `Response` objects with status
metadata, `Headers`, and `text()`, `json()`, `bytes()`, `blob()`, `formData()`,
and `arrayBuffer()` body readers. There are no streaming bodies, service workers,
or full cache-mode/referrer-generation semantics.

The wire protocol is **HTTP/1.1 only**, including for HTTPS. TLS 1.3 provides
encryption and authentication; it does not add HTTP/2. HTTP/2 framing and HPACK
and the QUIC, QPACK, and UDP transport required by HTTP/3 are not implemented.

Pages are limited to 8 MiB of HTML. One page may load up to 192 subresources
and 48 MiB of decoded response bytes. Individual CSS and script resources have
smaller limits. Images are independently bounded by the decoder's dimensions,
pixel count, and input caps. `data:image/...` URLs are supported.

## HTML, CSS, and layout

The HTML engine builds a real mutable DOM, repairs common malformed table and
formatting markup, decodes named and numeric character references, preserves
UTF-8, supports `<template>` inert fragment content, and applies explicit depth,
node, attribute, text, and arena caps.

The CSS engine implements:

- selectors, specificity, cascade, `!important`, inline styles, attribute and
  structural selectors, and state selectors (`:hover`, `:focus`, `:active`, etc.);
- `--*` custom property inheritance and `var()` expansion with cycle detection;
- recursive-descent `calc()`, `min()`, `max()`, and `clamp()` expressions;
- `color-scheme` and `light-dark()` color selection;
- `@media` width/height range queries (`min-width`, `max-width`, comparisons),
  `prefers-color-scheme`, and `scripting` queries;
- **CSS Grid** (`display: grid` / `inline-grid`, `grid-template-columns` and
  `grid-template-rows` with `fr`, fixed/percent lengths, `repeat()`, `minmax()`,
  explicit column/row lines and spans, `order`, and `gap`/`row-gap`/`column-gap`);
- **Flexbox** (`row`, `row-reverse`, `column`, `column-reverse`, `flex-wrap`,
  `flex-grow`, `flex-shrink`, `flex-basis`, `justify-content`, `align-items`
  including `baseline`, `align-self`, `order`, and gaps);
- the box model, `aspect-ratio`, `object-fit` (`fill`, `contain`, `cover`,
  `none`, `scale-down`), `outline` (`outline-width`, `outline-style`,
  `outline-color`, `outline-offset`), typography (`letter-spacing`, `word-break`,
  `overflow-wrap` / `word-wrap`, `text-overflow: ellipsis`), 2D `transform`
  (`translate`, `translateX`, `translateY`, `translate3d`, `scale`, `scaleX`,
  `scaleY`), `opacity`, anti-aliased `border-radius` with rounded clipping,
  outer and inset `box-shadow`, `linear-gradient()`, backgrounds, borders, lists,
  automatic and fixed tables with spans, floats, overflow, `<details>`/`<summary>`
  and `<dialog[open]>` UA rules, and `relative`/`absolute`/`fixed`/`sticky`
  positioning with z-order.

Painting supports alpha compositing against the framebuffer, image scaling,
form controls, selection/caret hooks, hover/active updates, dirty rectangles,
and compositing software `<canvas>` bitmaps.

Inline `<svg>` has a bounded vector painter for nested groups, `rect`, `line`,
`circle`, `ellipse`, `polygon`, `polyline`, and text, with `viewBox` scaling,
`preserveAspectRatio` modes (`none`, alignments, `meet`/`slice`), `currentColor`,
opacity, and presentation `fill`, `stroke`, and `stroke-width` attributes.
Multi-subpath paths accept `M/m`, `L/l`, `H/h`, `V/v`, `Q/q`, `T/t`, `C/c`,
`S/s`, `A/a`, and `Z/z`; quadratic/cubic/smooth curves are flattened cleanly,
while inline `<svg>` `A/a` elliptical-arc commands currently use an endpoint
straight-segment fallback. Standalone SVG images (`<img>` or `data:image/svg+xml`)
are rasterized by `libimg/svg_img.c`, which supports `transform` (`translate`,
`scale`, `rotate`, `matrix`) and elliptical arcs. Coordinates, point counts,
shapes, recursion, path flattening, raster pixels, and total raster work are
capped. Inline `<svg>` vector gradients, CSS-style inline SVG transforms,
filter effects, masks, and animation remain unimplemented.

`<canvas>` provides a software 2D rendering context (`getContext('2d')`) capped
at `2048` px per dimension (`4 MiB` total pixels per canvas): `save`/`restore`,
affine transforms (`translate`, `scale`, `rotate`, `transform`, `setTransform`,
`resetTransform`), `fillStyle`/`strokeStyle`/`lineWidth`/`globalAlpha`/`font`,
`CanvasGradient` (`createLinearGradient`, `createRadialGradient`, and
`addColorStop`), `fillRect`/`strokeRect`/`clearRect`, path drawing
(`beginPath`, `closePath`, `moveTo`, `lineTo`, `rect`, `arc`,
`quadraticCurveTo`, `bezierCurveTo`, `fill`, `stroke`),
`fillText`/`strokeText`/`measureText`, `ImageData` pixel manipulation
(`new ImageData(...)` overloads, `createImageData`, `getImageData`,
`putImageData` with dirty-rect compositing), and uncompressed RGBA PNG
serialization via `toDataURL('image/png')`.

`<video poster>` uses the ordinary bounded image pipeline, and video without a
usable poster and `<audio>` receive readable fallback boxes. There are no
audio/video codecs, demuxers, streaming buffers, controls, or playback.

The framebuffer uses built-in bitmap fonts. Complex text shaping,
downloadable webfonts (including WOFF/WOFF2), rotation/skew/3D CSS transforms,
CSS transitions/animations, and CSS filters are not provided.

## JavaScript and DOM

Each page runtime gives the detached documents created by its
`DOMParser` instances one backing-memory budget. HTML, XML, XHTML, and
SVG share it: creating another parser does not provide more credit.
The default is 64 MiB, and embedders can set a different
`max_parsed_document_bytes`. Backing includes DOM arena capacity,
indexes, and collected stylesheet and script buffers; it does not
include parser scratch space, JS wrappers, or the main document.
Existing per-document limits still apply. If a parse runs out of shared
credit, it throws instead of exposing a partial document and returns its
reservation. Successful detached documents stay funded until page
teardown, including any later DOM growth.

Classic inline and external scripts run in document order through `libjs`. The
interpreter provides ES6+ execution within explicit memory and step budgets:

- real block-scoped `let` and `const` declarations with temporal dead zone
  (TDZ) errors and per-iteration loop closure bindings;
- `class`, `extends`, `super()`, `super.method()`, and `static` members;
- `async` functions / async arrows and `await` backed by Promise microtask jobs;
- template literals, tagged templates, and `String.raw`;
- array and object destructuring (including function parameters), default
  parameters, rest parameters, and spread syntax in arrays, objects, and calls;
- arrow functions (sync and `async`, expression/block bodies, lexical `this`);
- `for..of` iteration and `[Symbol.iterator]` custom iterables;
- `Symbol`, `Symbol.for()`, `Symbol.keyFor()`, and `Symbol.toStringTag`;
- optional chaining (`?.`), nullish coalescing (`??`), logical assignments
  (`??=`, `&&=`, `||=`), exponentiation (`**`, `**=`), and optional `catch {}`.

Static `<script type="module">` graphs are loaded and evaluated with strict
resource, graph-size, and recursion limits. The loader supports bounded
relative imports, side-effect/default/namespace/named imports, and common
default, declaration, list, and re-export forms. Module URLs are cached and
evaluated once. Bindings are evaluated snapshots rather than live ES module
bindings, circular graphs are rejected, and general complex module grammar is
bounded. Within the supported grammar, source following an import declaration's
terminating semicolon is preserved and executed.

External module resources use the fetch-style origin and cookie policy:
cross-origin requests omit cookies and require an allowing CORS response.
They must also return a recognized JavaScript MIME type. `import.meta.url` is
the fetched module URL for an external module and the document URL for an
inline module. A dynamic `import()` whose specifier is a literal string is
eagerly resolved and evaluated with the bounded graph, then returns a Promise
for that namespace; computed specifiers, truly on-demand lazy chunk loading,
and top-level `await` are not supported.

Promise jobs run through a bounded microtask queue at browser checkpoints.
The available subset includes the constructor, `resolve`, `reject`, `then`,
`catch`, `finally`, `all`, and `race`.

`Map` and `Set` provide bounded SameValueZero-keyed storage, insertion-order
size/mutation/lookup operations, `forEach`, `[Symbol.iterator]`, and
`keys()`/`values()`/`entries()` iterators. `WeakMap` and `WeakSet` enforce
object-only keys and expose non-enumerating lookup/mutation surfaces. Because a
page realm is region-allocated and freed as a unit, weak entries share the realm
lifetime rather than participating in tracing-GC ephemeron collection.

Common static compatibility helpers include `Object.assign()`, `Object.is()`,
`Object.hasOwn()`, `Object.values()`, `Object.entries()`, `Object.fromEntries()`,
`Object.freeze()`, `Object.seal()`, `Object.isFrozen()`, `Object.isSealed()`,
`Array.from()`, `Array.of()`, `structuredClone()` (preserving cyclic object
references and `ArrayBuffer`s), strict `Number.isFinite()`/`isNaN()`/`isInteger()`/
`isSafeInteger()`, Number parsing aliases, and safe-integer/`EPSILON` constants.

Common prototype helpers include SameValueZero `Array.includes()`, `.at()`
(on `Array` and `String`), callback-based `find()`/`findIndex()`/`findLast()`/
`findLastIndex()`, non-mutating `toReversed()`/`toSorted()`/`toSpliced()`/`with()`,
`flat()`/`flatMap()`, `RegExp.prototype.flags`, and String `includes()`,
`startsWith()`, `endsWith()`, `repeat()`, `padStart()`, `padEnd()`,
`replaceAll()`, and start/end trim aliases. Repetition and padding are capped at
1 MiB results, and flattening is capped at depth 32 and one million result
elements. String indexing follows the interpreter's byte-string model.

`queueMicrotask()` feeds the bounded Promise job queue. `ArrayBuffer`
(`isView`, `prototype.slice`), all 9 TypedArray types (`Int8Array`, `Uint8Array`,
`Uint8ClampedArray`, `Int16Array`, `Uint16Array`, `Int32Array`, `Uint32Array`,
`Float32Array`, `Float64Array`), and `DataView` (16 big/little-endian getters
and setters with `RangeError` bounds checks) support shared backing buffers.
`BigInt64Array`/`BigUint64Array`, shared memory (`SharedArrayBuffer`), atomics,
and resizable buffers are not implemented.

`TextEncoder` produces UTF-8 `Uint8Array` data with bounded `encodeInto()`.
`TextDecoder` accepts `ArrayBuffer` or `Uint8Array`, strips a leading UTF-8 BOM
by default, replaces malformed input, and supports `fatal` and `ignoreBOM`. Only
UTF-8 is implemented without cross-call streaming state.

`URLSearchParams` stores ordered key/value pairs (`append`, `delete`, `get`,
`getAll`, `has`, `set`, `sort`, `forEach`, `size`, form-style serialization),
and `URL` composes a live `searchParams` object with `href`, `origin`,
`protocol`, `credentials`, `host`, `port`, path/query/fragment fields,
`toString()`, `toJSON()`, and `URL.canParse()`.

`AbortController` and `AbortSignal` expose `signal`, `abort(reason)`, `aborted`,
`reason`, `onabort`, listeners, `throwIfAborted()`, and `abort()`, `timeout()`,
and `any()` factories. Signals passed to `fetch()` prevent already-aborted
requests from queuing and drop queued tasks before transport execution.

`EventTarget`, `Event`, and `CustomEvent` are constructible, along with
`UIEvent`, `MouseEvent`, `KeyboardEvent`, `FocusEvent`, `InputEvent`, and
`PointerEvent`. Listeners support boolean capture or `capture`/`once`/`passive`/
`signal` options, `handleEvent()`, capture/target/bubble propagation, and
`composedPath()`. Shadow-tree retargeting is not implemented.

`Headers`, `Request`, `Response`, `Blob`, `File`, `FileReader`, and `FormData`
are constructible and integrate with `fetch()` and `URL.createObjectURL()`.
Collections are capped at 64 unique fields (`4 KiB` combined value per header).
Streaming bodies, CORS preflight, cache-mode controls, and constructing
`FormData` directly from an `<form>` element remain absent.

The browser binding provides:

- `window`, `self`, `document`, `location`, `navigator`, `screen`, and
  `performance`, including browser identity, language, online state, and the
  kernel-reported SMP processor count (`SYS_CPUINFO`);
- viewport-derived window/screen dimensions and `matchMedia()` backed by the CSS
  engine's media-query evaluator (`width`/`height`, `prefers-color-scheme`,
  `scripting`);
- `getElementById`, `querySelector`, `querySelectorAll`,
  `getElementsByTagName`, `getElementsByClassName`, `getElementsByName`,
  `createElement`, `createElementNS`, and `createTextNode`;
- `DOMParser` supporting `text/html`, `image/svg+xml`, and `text/xml`;
- `<template>` elements whose parsed subtree is accessible via `.content`;
- `getComputedStyle()`, `getBoundingClientRect()` (wired to layout geometry via
  `get_box_rect`), `window.scrollTo()` / `window.scrollBy()` (`scroll_cb`), and
  `window.scrollX` / `window.scrollY`;
- element attributes, `textContent`, `innerText`, `innerHTML`, `outerHTML`,
  `className`, `classList`, `style` declarations, form values, two-way `dataset`
  (reading, creating, updating, and deleting `data-*` attributes), cloning,
  `DocumentFragment`, `Attr`/`NamedNodeMap`, `NodeIterator`/`TreeWalker`,
  bounded `Range` and singleton `Selection`, `normalize()`, and tree checks
  (`contains`, `isSameNode`, `isEqualNode`, `compareDocumentPosition`);
- asynchronous `MutationObserver`, `ResizeObserver` (`jsdom_notify_resize()`),
  and `IntersectionObserver` notification queues;
- `navigator.clipboard.writeText()` and `navigator.clipboard.readText()` wired
  through `clipboard_write` / `clipboard_read` seams;
- `setTimeout`/`setInterval`, `queueMicrotask`, `requestAnimationFrame`, and
  `requestIdleCallback`;
- asynchronous `fetch()` and `navigator.sendBeacon()`;
- kernel-CSPRNG-backed `crypto.getRandomValues()`, `crypto.randomUUID()`, and
  Promise-based SHA-1/256/384/512 `crypto.subtle.digest()`;
- origin-partitioned `localStorage` and `sessionStorage` (`1 MiB` per-origin
  quota, atomic persistence format);
- `document.cookie` hiding `HttpOnly` cookies, complete `location` navigation,
  and same-document `history.pushState()` / `replaceState()` + `popstate`.

The current `children` and `childNodes` accessors return bounded Array
snapshots rather than live `HTMLCollection`/`NodeList` proxies. The HTML
parser still lowercases foreign-element tag names. Cross-container `Range`
deletion and CharacterData-point insertion raise `NotSupportedError`. Mutation
observation is capped at 16 observers and 64 pending records per observer.

`WebAssembly` provides bounded `validate`, `Module`, `Instance`, `compile`, and
`instantiate` entry points for an i32-only MVP execution core (i32 locals,
calls, constants, comparisons, integer arithmetic, and bit operations; no
imports, memory, tables, globals, floats, SIMD, threads, WASI, or streaming).

The interpreter is intentionally not a JIT engine: it omits tracing garbage
collection, generators, `Proxy`, `eval()`, `Function()`, `XMLHttpRequest`,
Web Workers, and full-scale framework compatibility. React-heavy SPAs,
streaming media sites, and browser games remain outside the compatibility
boundary.

## Forms and interaction

Text, search, password, email, URL, telephone, number, checkbox, radio, range,
submit, select, and textarea controls receive native sizes and painting.
Mouse focus and keyboard editing update their DOM values and dispatch events.
Checkboxes, radio groups, and selections update their checked/selected state.
Forms submit successful controls using `application/x-www-form-urlencoded`, with
GET and POST supported. Script handlers can cancel submission with
`preventDefault()` or `return false`.

The graphical controls in `apps/browser.c` support up to 8 tabs and live
Avionics desktop integration:

- clickable tab bar with 16x16 tab favicons (16-entry LRU cache using `<link rel="icon|shortcut icon">` or HTTP(S) `/favicon.ico` fallback via `img_decode`/`img_scale`), tab titles, `[x]` close, and `[+]` new tab (`Ctrl-T` new tab, `Ctrl-W` close tab, `Ctrl-N` / `Ctrl-P` next/previous tab);
- click links, buttons, `<details>`/`<summary>` disclosures (toggling `[open]` and reflowing), and form controls, with clickable `<select>` dropdown menus and `<input>` / `<textarea>` Talon clipboard (`Ctrl-C` / `Ctrl-V`) + caret editing;
- hovered link URL preview in the bottom status bar and dynamic `:hover` / `:active` / `:focus` styling;
- Back (`Ctrl-B`), Fwd (`Ctrl-E`), Reload (`Ctrl-R`), Star (`Ctrl-D` persistent `/home/.browser_bookmarks` bar), `Pin` (copy URL to Talon shelf), Reader (`Ctrl-O` distraction-free Reader Mode and local `.md` Markdown rendering), Zoom toolbar pill (`90%` / `100%` / `125%` / `150%`, or `0` / `+`/`=` / `-`/`_` keys outside inputs), Find (`Ctrl-F` / `Ctrl-G` match search), and Dev (`Ctrl-K` bottom Web Inspector drawer with `Elements` tree, interactive `Console` REPL, and `Network` request waterfall);
- `Ctrl-L` to focus the address bar; `Ctrl-U` clears the active line input when editing the address bar, Console REPL, or Find bar, or opens `view-source:<current_url>` in a new tab (or current tab when all 8 tabs are open) when browsing; `Esc` closes dropdowns/inputs/Find; arrows / Page Up / Page Down / Home / End / Space / mouse wheel scroll; live `KEV_RESIZE` and `KEV_THEME` updates; GUI session persistence in `/home/.browser_session` (restored when launched without an explicit URL); binary/`<a download>` saves to `/home/Downloads/`; and `Ctrl-Q` to quit.

## Text mode

```text
browser [-t] [-w columns] [-l] [-v] <url|file>
```

`-t` runs the same fetch, HTML, CSS, script, DOM, and layout pipeline and
projects the resulting layout to the terminal. `-l` lists resolved links and
`-v` prints the final URL, status, size, lifecycle state, and title. Local start
pages include `/doc/home.html`, `/doc/playground.html`, and the offline
`/doc/flightdeck.html` SPA diagnostics showcase.

## Testing

The browser has four complementary validation layers:

- `python3 tools/build.py test-host` builds and runs the 13 host C suites
  (`test_random_core`, `test_tcp_reassembly`, `test_net_checksum`,
  `test_gui_primitives`, `test_cryptosym`, `test_jpeg_prog`,
  `test_img_formats`, `test_js`, `test_dom`, `test_css`, `test_font`,
  `test_layout`, `test_jsdom`) plus `kfsck.py` and `test_kfs_journal.py`;
- `tools/run-jsdom-tests.sh` tests live DOM mutation, Canvas 2D, `DOMParser`,
  observers, geometry, selectors, events, timers, Promise/async jobs, Web
  Storage, fetch, WebAssembly, and navigation;
- `tools/run-browser-stack.sh` runs text and graphical paths on a guarded
  64 KiB stack;
- `python3 tools/build.py e2e` (`tools/e2e.py`) boots KestrelOS in headless QEMU
  and tests `/doc/home.html`, linked and imported CSS, external and inline ES6+
  JavaScript, static and multi-line ES modules, HTTP/1.1 Keep-Alive, `<canvas>`
  2D, Promise/fetch behavior, DOM mutation, images, verified HTTPS, certificate
  rejection, and navigation (`browser-web-compat` and `browser-modern-web`).

## Security boundary

TLS certificate verification is mandatory, mixed active content is blocked,
cross-origin fetches require an allowed CORS response, resource and interpreter
limits are enforced, and `HttpOnly` cookies stay hidden from scripts. The
from-scratch TLS and image/parser code has not received a professional
security audit. Do not use Kestrel Browser for sensitive credentials or
high-value transactions.
