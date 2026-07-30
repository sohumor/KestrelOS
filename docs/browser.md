# The Kestrel Browser

Kestrel Browser is a native browser built from the operating system's own
libraries. It does not embed Chromium, WebKit, Gecko, OpenSSL, or the host
operating system's network stack.

## Architecture

The navigation pipeline is:

```text
URL -> HTTP/1.1 (plain or over verified TLS 1.3) -> HTML DOM
    -> classic scripts, arrow functions, and bounded static ES modules
    -> DOM events, Promise jobs, and asynchronous fetch
    -> UA, inline, linked, and imported CSS
    -> computed styles -> layout -> paint
```

The major components are:

| Component | Responsibility |
|---|---|
| `apps/browser.c` | browser chrome, navigation, history, resources, forms, text mode |
| `libweb/html.c`, `dom.c` | HTML tokenizer, tree construction, DOM |
| `libweb/css.c`, `style.c` | selectors, cascade, media queries, computed values |
| `libweb/layout.c`, `paint.c` | box layout, tables, floats, positioning, controls, painting |
| `libweb/http.c`, `cache.c`, `cookie.c` | HTTP/1.1, redirects, compression, cache, cookies |
| `libweb/jsdom.c`, `libweb/storage.c`, `libjs/` | JavaScript, modern text/URL helpers, event tasks, origin storage, bounded WebAssembly, and live DOM bindings |
| `libimg/` | PNG, GIF, baseline JPEG, and BMP decoding |
| `libtls/` | verified TLS 1.3 and X.509 |

All response bodies and subresources have hard memory, nesting, execution,
and count limits. A malformed or hostile page should fail a resource or show a
truncation diagnostic rather than exhaust the kernel.

The implementation architecture is described in
[DESIGN-browser-runtime.md](DESIGN-browser-runtime.md). The short version is:
procedural parsing/layout/painting pipelines operate on compact data, major
resource owners are composed rather than inherited, and navigation and
asynchronous work use explicit bounded state machines.

## Navigation and networking

The browser accepts `file:`, `http:`, and `https:` URLs. A bare hostname is
treated as HTTP and an existing path is treated as a local file.

Network navigation supports:

- HTTP/1.1 keep-alive and connection reuse;
- chunked and content-length response bodies;
- gzip and deflate content coding;
- redirects with method rewriting;
- conditional cache revalidation;
- persistent RFC 6265-style cookies;
- verified TLS 1.3, SNI, hostname validation, and trusted roots;
- linked stylesheets, recursive bounded `@import`, scripts, and images;
- relative resource URLs, `<base href>`, and CSS URLs relative to their
  stylesheet response URL;
- mixed-content blocking on HTTPS pages.

Scripted `fetch()` is asynchronous at browser yield points. It has bounded
redirect handling, same-origin/CORS checks, and Promise-backed `Response`
objects with status metadata, `Headers.get()`, and `text()`, `json()`, and
`arrayBuffer()` body readers. This is a useful Fetch subset, not the complete
standard: there are no streaming bodies, service workers, request cloning,
abort signals, or full credentials/cache/mode semantics.

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
UTF-8, and applies explicit depth, node, attribute, text, and arena caps.

The CSS engine implements the cascade, specificity, `!important`, inline
styles, attribute and structural selectors, state selectors, media queries,
colours, lengths, the box model, typography, backgrounds, borders, lists,
tables, floats, overflow, relative/absolute/fixed positioning, and z-order.
It also has bounded, single-line flex-row and flex-column subsets: `row`,
`row-reverse`, `column`, and `column-reverse`. Both axes support `flex-grow`,
`gap`, `justify-content`, and basic cross-axis `align-items`. Wrapping, the
complete flex-basis/shrink algorithm, ordering, and all browser-compatible
flex sizing edge cases are not implemented.

Layout supports block and inline formatting, measured wrapping, whitespace
modes, margin collapsing, inline blocks, intrinsic images, lists, automatic
and fixed tables with spans, floats, clipping, and positioned elements.
Painting supports alpha compositing against the framebuffer, image scaling,
form controls, selection/caret hooks, and dirty rectangles.

Inline SVG has a deliberately small vector painter for nested groups,
`rect`, `line`, `circle`, `ellipse`, `polygon`, `polyline`, and text, with
basic `viewBox` scaling and presentation `fill`, `stroke`, and `stroke-width`
attributes. Bounded paths accept `M/m`, `L/l`, `H/h`, `V/v`, `Q/q`, `C/c`,
`A/a`, and `Z/z`; curves are flattened, and arc commands use an endpoint
fallback rather than true elliptical-arc geometry. Only one bounded path
subpath is consumed; a second move command stops path processing.

By default a `viewBox` is scaled with centered meet behavior, preserving its
aspect ratio rather than stretching it. If exactly one intrinsic `width` or
`height` is supplied, the other can be derived from the `viewBox` ratio.
Coordinates, point counts, shapes, recursion, path flattening, raster pixels,
and total raster work are capped. It does not implement the rest of the path
language, full arcs or `preserveAspectRatio` modes, transforms, gradients,
masks, filters, animation, external SVG resources, or the full SVG DOM/CSS
model.

`<video poster>` uses the ordinary bounded image pipeline, and video without a
usable poster and `<audio>` receive readable fallback boxes. There are no
audio/video codecs, demuxers, streaming buffers, controls, or playback.

The framebuffer uses built-in bitmap fonts. Complex shaping and downloadable
webfonts (including WOFF/WOFF2) are not provided. CSS transforms, filters,
canvas drawing, and pixel-identical browser-engine rendering also remain
outside the current engine.

## JavaScript and DOM

Classic inline and external scripts run in document order. The interpreter
implements the ES5 core language, closures, prototypes, exceptions, regular
expressions, JSON, common built-ins, and guarded execution limits. `let` and
`const` declarations are accepted for source compatibility but currently have
`var`-style function/global semantics rather than lexical scope, temporal dead
zones, or immutable bindings. Arrow functions support identifier parameters,
expression and block bodies, lexical `this` and `arguments`, and ordinary
Promise callbacks. Default, rest, destructured, and async arrow parameters are
not yet supported.

Static `<script type="module">` graphs are loaded and evaluated with strict
resource, graph-size, and recursion limits. The loader supports bounded
relative imports, side-effect/default/namespace/named imports, and common
default, declaration, list, and re-export forms. Module URLs are cached and
evaluated once. This is a source transformer rather than a complete ECMAScript
module implementation: bindings are snapshots instead of live bindings,
circular graphs are rejected, and general multiline/complex module grammar is
not supported. Within the supported single-line grammar, source following an
import declaration's terminating semicolon is preserved and executed.

External module resources use the fetch-style origin and cookie policy:
cross-origin requests omit cookies and require an allowing CORS response.
They must also return a recognized JavaScript MIME type. `import.meta.url` is
the fetched module URL for an external module and the document URL for an
inline module. A dynamic `import()` whose specifier is a literal string is
eagerly resolved and evaluated with the bounded graph, then returns a Promise
for that namespace; computed specifiers and truly on-demand module loading are
not supported. There is no top-level `await`.

Promise jobs run through a bounded microtask queue at browser checkpoints.
The available subset includes the constructor, `resolve`, `reject`, `then`,
`catch`, `finally`, `all`, and `race`; it is not a complete implementation of
every ECMAScript Promise edge case.

`Map` and `Set` provide bounded SameValueZero-keyed storage, insertion-order
size/mutation/lookup operations, `forEach`, and callable
`keys()`/`values()`/`entries()` iterators. `WeakMap` and `WeakSet` enforce
object-only keys and expose only their non-enumerating lookup/mutation
surfaces. The interpreter currently allocates an entire page realm from one
region and releases it together, so weak entries share the realm lifetime
rather than participating in independently observable garbage collection.
`Symbol.iterator`, weak-reference observation, and arbitrary-sized collections
are not implemented.

Common static compatibility helpers include `Object.assign()`, `Object.is()`,
`Object.values()`, `Object.entries()`, `Object.fromEntries()`, `Array.from()`,
`Array.of()`, strict
`Number.isFinite()`/`isNaN()`/`isInteger()`/`isSafeInteger()`, Number parsing
aliases, and safe-integer/EPSILON constants. `Array.from()` currently consumes
the bounded array-like path because general JavaScript symbol iterators and
constructor species are absent.

Common prototype helpers include SameValueZero `Array.includes()`, callback-
based `find()`/`findIndex()`, and String `includes()`, `startsWith()`,
`endsWith()`, `repeat()`, `padStart()`, and `padEnd()`. Repetition and padding
are capped at 1 MiB results. These operations follow the interpreter's
byte-string indexing model rather than browser-standard UTF-16 code units.
Bounded `Array.flat()`/`flatMap()`, String `replaceAll()`, and start/end trim
aliases are also present. Flattening is capped at depth 32 and one million
result elements; replacement uses the existing bounded string builder and
requires the global flag when its pattern is a regular expression.
Arrays also expose callable `keys()`/`values()`/`entries()` iterator objects.
`Object.fromEntries()` accepts bounded array-like entries, direct Map input,
or an explicit iterator object with `next()`. Automatic iterable discovery
still awaits the Symbol-keyed iterator protocol.

`queueMicrotask()` feeds the same bounded job queue. `ArrayBuffer` and
`Uint8Array` views support indexed byte access, `set`, `subarray`, `slice`, and
`fill`; subarrays share their backing buffer. `ArrayBuffer.isView()` and
WebAssembly byte input accept these views. Other typed arrays, `DataView`,
shared memory, atomics, and resizable buffers are not implemented.

`TextEncoder` produces UTF-8 `Uint8Array` data and supports bounded
`encodeInto()` writes without splitting a valid multibyte sequence.
`TextDecoder` accepts `ArrayBuffer` or `Uint8Array`, strips a leading UTF-8 BOM
by default, replaces malformed input, and supports fatal validation and
`ignoreBOM`. Only UTF-8 is implemented, decoder streaming state is not
preserved between calls, and the interpreter's documented byte-string model
means read counts differ from UTF-16 JavaScript for non-ASCII text.

`URLSearchParams` stores ordered key/value pairs in one bounded dense array.
It accepts query strings, another parameter object, arrays of pairs, and plain
records. `append`, `delete`, `get`, `getAll`, `has`, `set`, `sort`, `forEach`,
`size`, and form-style serialization are available. Symbol-based iterators are
absent, and non-ASCII sorting follows UTF-8 byte order rather than UTF-16 code
units.

`URL` composes the browser's bounded parser/resolver with `URLSearchParams`.
It resolves relative references, exposes `href`, `origin`, protocol,
credentials, host, port, path, query, fragment, `toString()`, `toJSON()`, and
`URL.canParse()`. Its `searchParams` object is live in both directions and
keeps the same identity when `search` or `href` is replaced.

`AbortController` and `AbortSignal` expose `signal`, `abort(reason)`,
`aborted`, `reason`, `onabort`, abort event listeners, `throwIfAborted()`, and
the static `abort()`, `timeout()`, and `any()` factories. `any()` accepts a
bounded array of signals and propagates the first exact abort reason; its
dependency depth and fan-out are capped. Passing a signal to `fetch()` prevents
an already aborted request from being queued and removes a queued request
before the host network callback runs. Since the present transport boundary is
synchronous, an abort cannot interrupt a host callback after that task has
entered its running state.

`EventTarget`, `Event`, and `CustomEvent` are constructible. Elements,
documents, standalone event targets, and abort signals participate in the
prototype relationships expected by feature detection. Listeners are
deduplicated by type/callback/capture and removable; function and
`handleEvent()` object listeners support boolean capture or bounded
`capture`/`once`/`passive`/`signal` option records. `dispatchEvent()` tracks
target/current target and all three standard phases, removes `once` listeners
before re-entrant invocation, prevents passive cancellation, rejects
re-entrant dispatch of the same event object, and follows a bounded
window/document/DOM-parent capture and bubbling path. It also supports
`composedPath()` while dispatch is active, the standard phase constants,
`srcElement`, `cancelBubble`, `returnValue`, `CustomEvent.detail`,
cancellation, and immediate or ordinary propagation stops. Shadow/composed-
tree retargeting and default-passive event heuristics are not implemented.

`UIEvent`, `MouseEvent`, `KeyboardEvent`, `FocusEvent`, `InputEvent`, and
`PointerEvent` are constructible with their common coordinates, buttons,
modifiers, key/code/location, composition/input, related-target, and pointer
geometry/pressure fields. Their prototypes compose over `Event`, modifier
state is queryable, and trusted native click/input/focus/pointer dispatches use
the corresponding interface identity. Kestrel does not yet synthesize full
platform scan-code, pointer-coordinate, click-count, or IME payloads at the
host boundary.

`Headers` is a constructible, reference-counted collection shared by
script-created sets and fetched responses. It accepts records, arrays of
two-item pairs, or another `Headers`; validates and lowercases field names;
trims values; combines appends; and implements `get`, `has`, `append`, `set`,
`delete`, and `forEach`. Collections are capped at 64 unique fields with a
4 KiB combined value per field. Callable `entries`, `keys`, and `values`
iterators are present. JavaScript `Symbol.iterator`, header guards, and the
complete Fetch forbidden-header matrix are not yet implemented.

`Request` resolves relative URLs and owns a normalized method, independent
`Headers`, a text or byte body, and an `AbortSignal`. It supports construction
from URLs or another request, init overrides, `clone()`, `text()`, `json()`,
`arrayBuffer()`, `blob()`, `bytes()`, `formData()`, and `bodyUsed`. It exposes
validated cache, credentials, destination, integrity, mode, redirect,
referrer, referrer-policy, keepalive, and navigation metadata. Fetch accepts
these objects directly and
transports their normalized `Accept`, `Content-Type`, and bounded extra
headers to the HTTP client. Forbidden transport-controlled headers are
rejected. Same-origin mode, cookie credential policy, credentialed CORS, and
follow/error/manual redirects are enforced. Cross-origin non-simple headers
or content types are rejected until CORS preflight is implemented. Streaming
bodies, opaque no-CORS responses, cache-mode behavior, integrity verification,
and automatic referrer-header generation remain absent.

`Blob` accepts bounded mixtures of strings, `ArrayBuffer`, `Uint8Array`, and
other blobs. It exposes normalized `size`/`type`, negative-index-aware
`slice()`, and Promise-based `text()`, `arrayBuffer()`, and `bytes()` readers.
Blob bytes and MIME types flow directly into `Request` and `Response`. `File`
adds name, modification time, and relative-path metadata. Per-realm
`URL.createObjectURL()` values retain bounded byte copies, resolve directly
through Fetch, and stop resolving after `URL.revokeObjectURL()`. Streaming
Blob bodies and object-URL navigation/subresource loading remain absent.

`FileReader` is an EventTarget with EMPTY/LOADING/DONE state, result/error
accessors, abort handling, progress metadata, and asynchronous text,
ArrayBuffer, binary-string, and data-URL reads. Its jobs run through the same
bounded browser event loop as timers and Fetch.

`FormData` stores up to 64 ordered string, Blob, or File fields and implements
`append`, `set`, `delete`, `get`, `getAll`, `has`, `forEach`, and callable
`entries`/`keys`/`values` iterators. Request and Response bodies encode it as
bounded `multipart/form-data` with a generated boundary, quoted field names,
filenames, and per-file content types. Body `formData()` readers decode both
URL-encoded and multipart data and reconstruct file parts. Construction from
an HTML form and automatic `Symbol.iterator` remain absent.

`Response` is constructible from text, byte arrays, Blob, URLSearchParams, or
FormData bodies with validated status, status text, and header options.
Script-created and network responses share text, JSON, ArrayBuffer, Blob,
bytes, and FormData readers. `clone()` creates an independently consumable
body/header copy, and `Response.json()` serializes a JavaScript value with an
`application/json` default content type. `Response.redirect()` and
`Response.error()` provide validated factories; network responses retain final
URL, basic/default/error type, and redirect metadata. Streaming bodies and
trailers are not yet implemented.

The browser binding provides:

- `window`, `self`, `document`, `location`, `navigator`, `screen`, and
  `performance`, including browser identity, language, online state, and the
  kernel-reported SMP processor count;
- viewport-derived window/screen dimensions and `matchMedia()` results that
  reuse the CSS engine's bounded media-query evaluator;
- `getElementById`, `querySelector`, `querySelectorAll`,
  `getElementsByTagName`, `createElement`, and `createTextNode`;
- element attributes, `textContent`, `innerText`, `innerHTML`, `outerHTML`,
  `className`, `classList`, common `style` properties, form values, and tree
  mutation;
- selector matching/closest lookup, containment and connectivity, element-only
  sibling traversal, cloning, mixed Node/text append/prepend/replace,
  sibling/adjacent insertion, and parsed adjacent/outer HTML replacement;
- snapshot tag/class/name collections, namespace/local-name filtered tag
  collections, plus attribute-name enumeration and `hasAttributes()`;
- `Attr` and bounded `NamedNodeMap` identity with indexed snapshots, live
  length/value access, named lookup, namespace-null lookup, and owner metadata;
- document-aware `contains()`, `isSameNode()`, recursive `isEqualNode()`, and
  standard `compareDocumentPosition()` bitmasks/constants;
- bounded recursive `normalize()` merging adjacent text and removing empty
  text nodes through observable native tree edits;
- `NodeFilter` masks plus bounded `NodeIterator` and `TreeWalker` traversal,
  callback/object filters, current/reference state, and root confinement;
- constructible bounded `Range` boundary state with relative setters,
  node/content selection, point/boundary comparison, intersection, text
  projection, clone/extract/delete/insert/surround operations and contextual
  HTML fragments, plus singleton `Selection` add/remove/collapse/select/
  contains/delete state;
- Node/Element/HTMLElement/SVGElement prototype identity, standard Node type
  constants, element/text/document names and types, ownership/local-name/
  prefix/namespace metadata, `createElementNS()`, parsed SVG-descendant
  namespace propagation, `Document`/`HTMLDocument` and
  `CharacterData`/`Text`/`Comment` identity, constructible text/comment nodes,
  live data/length, and bounded substring/append/insert/delete/replace
  operations;
- constructible `DOMException` values with standard names, legacy codes,
  constants, string rendering, and `IndexSizeError` from CharacterData bounds;
- snapshot `childNodes`/`children`, child/root inspection, comment creation,
  and clonable `DocumentFragment` trees that splice their children into a
  destination on insertion;
- a stable `dataset` DOMStringMap view that camel-cases existing `data-*`
  attributes, reflects writes to known keys, and discovers attributes added
  through `setAttribute()` on the next access;
- multi-token DOMTokenList operations and general CSSStyleDeclaration
  property/value/priority mutation, removal, and indexed inspection;
- asynchronous `MutationObserver` delivery with `MutationRecord` identity,
  attribute/child-list/character-data records, subtree and old-value options,
  bounded Array `attributeFilter`, `takeRecords()`, and `disconnect()`;
- constructible `EventTarget`, `Event`, and `CustomEvent`,
  `addEventListener`/`removeEventListener`/`dispatchEvent`, inline event
  attributes, standard phase constants, bounded capture/bubbling, listener
  objects and capture/once/passive/abort-signal options, cancellable
  click/change/input/submit events, active composed paths and legacy
  cancellation/target aliases, DOMContentLoaded, and load;
- constructible UI, mouse, keyboard, focus, input, and pointer event
  interfaces with composed prototype identity and common initializer fields;
- `setTimeout`/repeating `setInterval` with bounded forwarded arguments,
  cancellation, `queueMicrotask`, timestamped/cancellable
  `requestAnimationFrame`, and cancellable `requestIdleCallback` with a
  bounded IdleDeadline;
- asynchronous `fetch()` and the bounded `Response`/`Headers` subset described
  above, including queued request cancellation through `AbortSignal`;
- constructible, cloneable, normalized `Headers` collections and constructible
  text/byte/JSON `Response` objects;
- constructible and cloneable `Request` objects whose validated headers and
  bodies reach the HTTP transport;
- bounded binary `Blob` construction, slicing, readers, and Fetch body
  integration;
- ordered multipart `FormData` storage and real HTTP body encoding;
- `File`, asynchronous `FileReader`, Blob object URLs, and URL-encoded or
  multipart Body `formData()` readers;
- kernel-CSPRNG-backed `crypto.getRandomValues()` and `crypto.randomUUID()`,
  plus Promise-based SHA-1, SHA-256, SHA-384, and SHA-512
  `crypto.subtle.digest()`;
- bounded, queued `navigator.sendBeacon()` POST delivery through the same
  credential and redirect policy pipeline as Fetch;
- origin-partitioned `localStorage` and `sessionStorage`; local data is
  persisted per OS user using a versioned bounded file, while session data
  lasts for the browser process;
- `TextEncoder`, UTF-8 `TextDecoder`, live `URL`/`URLSearchParams`, and
  `AbortController`/`AbortSignal`;
- bounded `Map`/`Set` collections and object-keyed, non-enumerable
  `WeakMap`/`WeakSet`;
- common modern Object, Array, and Number static compatibility helpers;
- common modern Array and String prototype lookup, search, flattening,
  replacement, trimming, repeat, and padding helpers;
- explicit Array iterators and bounded `Object.fromEntries()` consumption;
- `document.cookie` without exposing or allowing creation of HttpOnly
  cookies;
- complete Location URL component inspection/mutation, document URL/base/
  encoding/visibility metadata, and queued assign/replace/reload navigation.
- bounded same-document `history.pushState()`/`replaceState()` and
  back/forward/go traversal with `popstate`, same-origin enforcement, state,
  and address-bar synchronization.

The current DOMStringMap does not yet have JavaScript exotic-property hooks:
assigning a previously absent `dataset` key or deleting a key does not create
or remove its attribute. Existing keys are live in both directions and newly
added `data-*` attributes are discovered when `dataset` is read again.
The current `children` and `childNodes` values are fresh bounded Array
snapshots, not live HTMLCollection/NodeList objects.
Element namespaces preserve standard and custom namespace URIs and distinguish
HTML from SVG wrappers. The HTML parser still lowercases foreign-element names,
and attribute namespaces, XML namespace fixups, and the broader SVG-specific
DOM interface family are not implemented.
Range extraction/deletion preserves markup for same-container child spans and
CharacterData slices. Cross-container `cloneContents()` currently projects
selected text, while complex cross-container deletion and CharacterData-point
insertion raise `NotSupportedError`. Boundary points are not yet automatically
retargeted after unrelated external tree edits, Selection stores one forward
range, and geometry APIs are absent.
Mutation observation is capped at 16 observers and 64 pending records per
observer, with up to 16 target registrations per observer. Re-observing the
same target replaces that target's options while retaining its other targets.
Transient removed-subtree registrations, record coalescing, iterable non-Array
attribute filters, and live NodeList record fields are not yet implemented.

DOM changes trigger style recomputation and re-layout. Timer callbacks run at
safe browser yield points rather than from an interrupt. Timer and fetch work
move through explicit free, queued, running, and settling states. Delays are
quantized to browser event turns rather than real-time millisecond precision.
Animation timestamps and `performance.now()` use that same event-turn clock;
the idle deadline reports a fixed bounded compatibility budget rather than
measuring wall-clock frame slack.
Web Storage has a 1 MiB per-origin quota and bounded entry counts; import is
transactional so malformed persistence data cannot partially replace a store.

`WebAssembly` provides bounded `validate`, `Module`, `Instance`, `compile`, and
`instantiate` entry points for an i32-only MVP execution core. It can execute
small exported functions using i32 locals, calls, constants, comparisons,
integer arithmetic, and bit operations. It has no imports, memory, tables,
globals, structured control flow, floating point, SIMD, threads, WASI, or
streaming compilation.

The interpreter is intentionally not a production modern-JavaScript engine.
It does not have a JIT, a tracing garbage collector, general ES2015+ syntax,
computed dynamic module loading, XMLHttpRequest, workers, most typed arrays,
or the hundreds of specialised Web APIs used by Chromium-scale applications.
React-heavy single-page applications, streaming media sites, and browser games
therefore remain outside the honest compatibility boundary. Kestrel Browser
does not claim full compatibility with arbitrary websites.

## Forms and interaction

Text, search, password, email, URL, telephone, number, checkbox, radio, range,
submit, select, and textarea controls receive native sizes and painting.
Mouse focus and keyboard editing update their DOM values and dispatch events.
Checkboxes, radio groups, and selections update their checked/selected state.
Forms submit successful controls using
`application/x-www-form-urlencoded`, with GET and POST supported. Script
handlers can cancel submission with `preventDefault()` or `return false`.

The graphical controls are:

- click links and controls;
- Back, Fwd, and Reload;
- Ctrl-L to focus the address bar;
- Ctrl-R to reload;
- Ctrl-B and Ctrl-F for history;
- arrows, Page Up, Page Down, Home, End, and Space to scroll;
- Ctrl-Q to close.

## Text mode

```text
browser [-t] [-w columns] [-l] [-v] <url-or-file>
```

`-t` runs the same fetch, HTML, CSS, script, DOM, and layout pipeline and
projects the resulting layout to the terminal. `-l` lists resolved links and
`-v` prints the final URL, status, size, lifecycle state, and title. This is
the deterministic interface used by the end-to-end suite.

## Testing

The browser has four complementary validation layers:

- `tools/run-jsdom-tests.sh` tests live DOM mutation and observation, selectors, events,
  timers, Promise jobs, arrow functions, typed byte views, Web Storage, fetch
  body readers and cancellation, text codecs, URL parsing and parameters,
  WebAssembly, navigation, and `innerHTML`;
- `tools/run-browser-stack.sh` runs text and graphical paths on a guarded
  64 KiB stack;
- the DOM, CSS, layout, HTTP, image, JavaScript, inflate, and TLS host suites
  test their engines independently;
- `tools/e2e.py` boots KestrelOS and uses controlled HTTP/TLS fixtures to test
  linked and imported CSS, external and inline JavaScript, a static module
  graph, Promise/fetch behavior, DOM mutation, images, HTTPS, certificate
  rejection, and ordinary navigation.

## Security boundary

TLS certificate verification is mandatory, mixed active content is blocked,
cross-origin fetches require an allowed CORS response, resource and interpreter
limits are enforced, and HttpOnly cookies stay hidden from scripts. The
from-scratch TLS and image/parser code has not received a professional
security audit. Do not use Kestrel Browser for sensitive credentials or
high-value transactions.
