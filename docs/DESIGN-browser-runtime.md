# Kestrel Browser runtime architecture

Kestrel Browser is a clean-room engine. Firefox and Chromium documentation is
used to understand durable architectural boundaries, not as an implementation
source. Kestrel keeps its own bounded C data structures, algorithms, APIs, and
tests.

## Design rules

The runtime follows five rules:

1. Procedural code implements parsing and processing pipelines. HTML parsing,
   CSS matching, CSS custom property/`calc()` resolution, Grid/Flexbox/block
   layout, paint-list generation (gradients, shadows, rounded rects, SVG,
   Canvas 2D bitmaps), image decoding, and AST/async JS interpretation are
   explicit operations over inputs and outputs.
2. Performance-sensitive work is data-oriented. DOM arenas, layout boxes,
   paint items, grid/flex items, storage entries, and task queues use bounded
   flat or contiguous collections so hot loops do not chase object hierarchies.
3. Modules own resources. A browser runtime owns HTTP/TLS services, the
   persistent Keep-Alive connection pool, the UA stylesheet, and local/session
   stores. A tab/page owns its DOM, author CSS, computed style engine, layout
   document, `<canvas>` bitmaps, script realm, and decoded images.
4. Input, page events, timers, Promise jobs, async continuations, observers, and
   scripted fetches are delivered through event checkpoints. Interrupt handlers
   never execute page script.
5. State machines make lifecycle changes visible. Composition connects these
   components; no browser subsystem inherits behavior from another.

## Ownership by composition

```text
browser_runtime
  +-- HTTP client (Keep-Alive pool up to HTTP_POOL_MAX) + TLS 1.3 options
  +-- UA stylesheet
  +-- persistent localStorage service
  `-- process-lifetime sessionStorage service

browser_tab
  +-- history stack, address buffer, scroll/focus/hover/active/reader state
  +-- per-tab zoom_pct (90/100/125/150%), favicon_url, 16-entry LRU favicon cache on bstate
  +-- console log ring (CONSOLE_LOG_MAX) + network log ring (NET_LOG_MAX)
  `-- active browser_page
        +-- URL/base URL (incl. view-source: wrapper) and response metadata
        +-- DOM document (including <canvas> pixel buffers & <details>/<dialog>)
        +-- author stylesheet + computed style engine
        +-- layout document + flat paint order
        +-- JavaScript/DOM realm (ES6+ libjs + jsdom)
        `-- decoded image resources (libimg: PNG/GIF/BMP/JPEG/ICO/CUR/SVG/QOI/Netpbm)
```

The page receives references to runtime and host services through
`struct jsdom_config` and explicit callbacks (`print`, `cookie_get`/`cookie_set`,
`fetch`, `get_box_rect`, `scroll_cb`, `clipboard_write`/`clipboard_read`,
`stylesheet`). The DOM bridge does not subclass the network, layout, clipboard,
storage, or renderer. This makes ownership and teardown explicit and lets host
tests substitute deterministic fetch, cookie, geometry, clipboard, storage, and
print services.

## Page lifecycle

Each page has a validated lifecycle:

```text
created -> navigating -> fetching -> response -> parsing
        -> styling -> layout -> scripting -> complete
```

A DOM mutation, canvas draw, or viewport resize (`jsdom_notify_resize()`)
triggers style recomputation and re-layout:

```text
complete -> styling -> layout -> complete
```

Any stage can enter `failed`. A transport or parser failure then transitions
from `failed` into `parsing` to build Kestrel's internal error document through
the same style, layout, and paint pipeline. The `failed` result bit remains
separate from lifecycle state, so a fully rendered HTTP error page is both
complete and correctly reported as an unsuccessful navigation.

Navigation is currently synchronous at the transport boundary: the application
yields while `http_fetch()` resolves and reads a bounded response. The state
machine makes that boundary explicit so a later nonblocking transport can
split `fetching` into DNS, connect, TLS, request, headers, and body events
without changing page ownership.

## Event tasks and async execution

Timer and fetch records live in fixed-capacity arrays and move through:

```text
free -> queued -> running -> settling -> free
```

Promise reactions, `queueMicrotask()`, and `async`/`await` continuations share
the JavaScript realm's bounded FIFO job queue (`js_run_jobs()`). At each
checkpoint (`jsdom_pump()`), the runtime drains Promise jobs, delivers
`MutationObserver`, `ResizeObserver`, and `IntersectionObserver` batches, then
advances due timer, animation-frame, idle-callback, Fetch, and `FileReader`
tasks. `async`/`await` suspends function frames in explicit `js_async_task`
continuations rather than C stack switching, keeping the 64 KiB user stack
safe across multi-step awaits.

Repeating timers schedule their next event turn only after the current callback
returns, and cancellation remains valid from inside the callback. GUI idle
events also run checkpoints, so work progresses without keyboard or mouse
input. Each timer record owns up to 16 forwarded callback values. Animation and
idle requests compose over the same lifecycle with an explicit callback kind:
animation receives the event-turn timestamp, while idle receives a freshly
owned deadline record. Cancellation therefore uses one ID namespace and one
state transition path instead of parallel schedulers.

An `AbortController` and its `AbortSignal` compose over one reference-counted
state record. Fetch tasks keep a non-owning pointer to that state while queued:
an abort moves matching tasks directly from `queued` to `settling`, rejects
their Promises with the signal's exact reason, and frees the task before the
network host is called. Timed signals reuse the timer queue and enter the same
transition with a `TimeoutError`. Composed signals form a bounded acyclic
dependency graph: `AbortSignal.any()` validates the complete source array
transactionally, deduplicates sources, and caps both fan-out and graph depth
before registering edges.

Constructed events carry explicit dispatch, stopped, and immediately-stopped
state rather than encoding propagation in the cancellation flag. Dispatch
snapshots the existing DOM parent links into a bounded path, walks it in
capture/target/bubble order, updates target/current-target and phase at each
step, and consults the same listener arrays used by trusted input events.
Each listener is a compact record holding callback, capture, once, passive,
and optional AbortSignal state. `once` clears its listener slot before invocation
so re-entrant dispatch cannot call it twice, while abort-linked records are
skipped once triggered. `composedPath()` exposes a copy of the active traversal
path while dispatch is active. Specialized UI, mouse, keyboard, focus, input,
and pointer events compose prototype layers over the same `Event` record.

## Data-oriented storage, layout, and rendering

Web Storage is an origin-partitioned contiguous table with procedural lookup
and explicit quotas (`1 MiB` per origin). The storage module owns copied
origin/key/value strings, and its versioned percent-escaped persistence format
is imported transactionally.

The separate `libjs/webapi.c` module operates directly on owned byte spans.
Text codecs consume `ArrayBuffer`/TypedArray views without intermediate number
arrays, and `libjs/builtin.c` backs `Int8Array`, `Uint8Array`,
`Uint8ClampedArray`, `Int16Array`, `Uint16Array`, `Int32Array`, `Uint32Array`,
`Float32Array`, `Float64Array`, and `DataView` over shared `ArrayBuffer` byte
slices, along with `structuredClone()` cycle tracking (`sc_map`). `URLSearchParams`
uses one dense JavaScript array with alternating key/value entries, preserving
order while keeping lookup, mutation, sorting, and serialization as bounded
procedural passes. `URL` owns a parsed URL record and composes it with one live
parameter object; snapshot comparison synchronizes changes without introducing
inheritance or a second query representation.

Fetch header sets use a compact fixed-capacity table of unique normalized
names and owned values. Script-created sets and response wrappers share the
same reference-counted state; cloning performs bounded procedural copies.
Request records use the same owned header and byte-body components, enforcing
same-origin, cookie, credentialed-CORS, and redirect decisions at transport time.
`Blob` construction appends heterogeneous parts into one capped owned byte span;
`FormData` emits and parses bounded `multipart/form-data` and URL-encoded
bodies; `FileReader` and Blob object URLs tie directly into the per-realm task
and registry lifecycle.

Web `crypto` (`libjs/crypto.c`) is completely self-contained so `libjs` builds
standalone without `libtls`: it implements circular 16-word-schedule FIPS 180-4
SHA-1/256/384/512 for `crypto.subtle.digest()`, RFC 4122 v4 `randomUUID()`, and
`getRandomValues()`, delegating entropy generation through an optional
`js_config.random_bytes` hook wired in `jsdom.c` to OS `getrandom()` (backed by
the SHA-256 entropy pool and ChaCha20 CSPRNG). `navigator.hardwareConcurrency` reads
from `SYS_CPUINFO`. `navigator.clipboard` delegates `writeText()` / `readText()`
through the `clipboard_write` / `clipboard_read` seams in `struct jsdom_config`
(falling back to an internal realm clipboard buffer in headless host tests).

`matchMedia()` and stylesheet `@media` evaluation share the CSS parser's bounded
media-query evaluator (`width`/`height` ranges, `prefers-color-scheme`,
`scripting`). Custom property (`--*`) inheritance, recursive-descent
`calc()`/`min()`/`max()`/`clamp()`, `light-dark()`, `box-shadow`, `outline`,
`aspect-ratio`, `object-fit`, `text-overflow: ellipsis`, 2D `transform`
(`translate`/`scale`), and `linear-gradient()` resolve procedurally during style
computation and layout without floating-point dependencies.

Flex and Grid layout operate over flat item arrays rather than polymorphic layout
objects: Flex computes row/column wrapping, `flex-grow`, `flex-shrink`,
`flex-basis`, `align-items: baseline`, `align-self`, `order`, and gaps; Grid
computes explicit/fraction (`fr`) / `repeat()` / `minmax()` column and
`grid-template-rows` tracks and item placements. `position: sticky`
offsets are evaluated against scrolled viewport coordinates using the shared
box tree, and `lay_paint_order()` feeds both `libweb/paint.c` (anti-aliased
rounded corners, gradients, inset/outer shadows, `outline`, `object-fit`
replaced image crops, multi-subpath SVG, `<canvas>` bitmaps) and
`lay_hit_test_view()`.

Software `<canvas>` 2D backing stores (`struct dom_canvas` attached directly to
`HTAG_CANVAS` DOM nodes) stay decoupled from layout: `canvas_2d_state` tracks
affine transform stacks, polygon/arc path arrays, `CanvasGradient`
(`createLinearGradient`/`createRadialGradient`), styles, and font state,
rasterizes into bounded RGBA buffers (`JSDOM_CANVAS_MAX_DIM = 2048`), supports
`ImageData` (`new ImageData(...)`, `createImageData`, `getImageData`,
`putImageData` with dirty-rect compositing), marks the DOM dirty on draw, and
serializes uncompressed RGBA PNGs for `toDataURL('image/png')`. Layout and
`paint.c` blit the resulting bitmap whenever present.

GUI session state (`/home/.browser_session`) records open tab URLs, per-tab
zoom (`90%`/`100%`/`125%`/`150%`), and the active tab index when launched
without an explicit CLI URL, while clicking `<details> > summary` toggles `[open]`
in the DOM and triggers restyle/relayout.

`DOMParser` parses HTML, SVG, or XML strings directly via `html_parse_document()`
into standalone documents; `<template>` elements retain inert parsed contents in
a `DocumentFragment` exposed as `.content`. Geometry queries
(`getBoundingClientRect()`) consult `jsdom_config.get_box_rect`, `scrollTo()` /
`scrollBy()` synchronize with `jsdom_config.scroll_cb`, and `ResizeObserver` /
`IntersectionObserver` queue bounded observation batches drained in `jsdom_pump()`.

Two-way `dataset` synchronization keeps a per-element binding object over `data-*`
attributes, syncing reads, new-key creation, writes, and `delete` operations
directly with the underlying DOM attribute array.

`libjs` implements real block-scoped `let`/`const` lexical environments with
temporal dead zone (TDZ) checks and per-iteration loop closures, ES6 `class` /
`extends` / `super`, destructuring, default/rest parameters, spread syntax,
template and tagged literals, `Symbol` (`[Symbol.iterator]` + `for..of`),
optional chaining (`?.`), nullish coalescing (`??`), logical assignments, and
exponentiation (`**`). `Map`, `Set`, `WeakMap`, and `WeakSet` store dense
bounded key/value arrays with SameValueZero lookup over the realm lifetime.

## Clean-room architectural references

- Chromium's Life of a Navigation describes the separation between navigation,
  response handling, and rendering:
  <https://chromium.googlesource.com/chromium/src/+/master/docs/navigation.md>
- Chromium's multi-process resource-loading design documents centralized
  resource ownership and asynchronous request messaging:
  <https://www.chromium.org/developers/design-documents/multi-process-resource-loading/>
- Chromium's process architecture documents component isolation:
  <https://www.chromium.org/developers/design-documents/multi-process-architecture/>
- Firefox's Gecko overview separates content, layout, and platform work:
  <https://firefox-source-docs.mozilla.org/overview/gecko.html>
- Firefox's GeckoView architecture documents event-driven embedding boundaries:
  <https://firefox-source-docs.mozilla.org/mobile/android/geckoview/contributor/geckoview-architecture.html>
- Firefox's dynamic-change documentation describes restyle and reflow work
  after DOM changes:
  <https://firefox-source-docs.mozilla.org/layout/DynamicChangeHandling.html>

These references influence boundaries and terminology only. Kestrel does not
copy source code, internal data structures, or algorithms from either engine.
