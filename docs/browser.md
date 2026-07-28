# The Kestrel Browser

Kestrel Browser is a native browser built from the operating system's own
libraries. It does not embed Chromium, WebKit, Gecko, OpenSSL, or the host
operating system's network stack.

## Architecture

The navigation pipeline is:

```text
URL -> HTTP/1.1 (plain or over verified TLS 1.3) -> HTML DOM
    -> classic scripts and bounded static ES modules
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
| `libweb/jsdom.c`, `libjs/` | ES5-derived JavaScript, Promise jobs, bounded WebAssembly, and live DOM bindings |
| `libimg/` | PNG, GIF, baseline JPEG, and BMP decoding |
| `libtls/` | verified TLS 1.3 and X.509 |

All response bodies and subresources have hard memory, nesting, execution,
and count limits. A malformed or hostile page should fail a resource or show a
truncation diagnostic rather than exhaust the kernel.

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
It also has a bounded, single-line flex-row subset: `row` and `row-reverse`,
`flex-grow`, `gap`, `justify-content`, and basic cross-axis `align-items`.
Flex columns use the readable block fallback, and wrapping, the complete
flex-basis/shrink algorithm, ordering, and all browser-compatible flex sizing
edge cases are not implemented.

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
zones, or immutable bindings.

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

The browser binding provides:

- `window`, `self`, `document`, `location`, `navigator`, `screen`, and
  `performance`;
- `getElementById`, `querySelector`, `querySelectorAll`,
  `getElementsByTagName`, `createElement`, and `createTextNode`;
- element attributes, `textContent`, `innerText`, `innerHTML`, `outerHTML`,
  `className`, `classList`, common `style` properties, form values, and tree
  mutation;
- `addEventListener`, inline event attributes, cancellable click/change/input/
  submit events, DOMContentLoaded, and load;
- one-shot `setTimeout`, `setInterval`, and `requestAnimationFrame`
  compatibility callbacks;
- asynchronous `fetch()` and the bounded `Response`/`Headers` subset described
  above;
- `document.cookie` without exposing or allowing creation of HttpOnly
  cookies;
- script navigation through `location`.

DOM changes trigger style recomputation and re-layout. Timer callbacks run at
safe browser yield points rather than from an interrupt.

`WebAssembly` provides bounded `validate`, `Module`, `Instance`, `compile`, and
`instantiate` entry points for an i32-only MVP execution core. It can execute
small exported functions using i32 locals, calls, constants, comparisons,
integer arithmetic, and bit operations. It has no imports, memory, tables,
globals, structured control flow, floating point, SIMD, threads, WASI, or
streaming compilation.

The interpreter is intentionally not a production modern-JavaScript engine.
It does not have a JIT, a tracing garbage collector, general ES2015+ syntax,
computed dynamic module loading, XMLHttpRequest, workers, typed arrays, or the
hundreds of specialised Web APIs used by Chromium-scale applications.
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
`-v` prints the final URL, status, size, and title. This is the deterministic
interface used by the end-to-end suite.

## Testing

The browser has four complementary validation layers:

- `tools/run-jsdom-tests.sh` tests live DOM mutation, selectors, events,
  timers, Promise jobs, fetch body readers, WebAssembly, navigation, and
  `innerHTML`;
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
