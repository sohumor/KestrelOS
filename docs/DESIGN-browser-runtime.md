# Browser runtime architecture

Kestrel Browser is a clean-room engine. Firefox and Chromium documentation is
used to understand durable architectural boundaries, not as an implementation
source. Kestrel keeps its own bounded C data structures, algorithms, APIs, and
tests.

## Design rules

The runtime follows five rules:

1. Procedural code implements parsing and processing pipelines. HTML parsing,
   CSS matching, layout, paint-list generation, image decoding, and bytecode
   interpretation are explicit operations over inputs and outputs.
2. Performance-sensitive work is data-oriented. DOM arenas, layout boxes,
   paint items, flex items, storage entries, and task queues use bounded flat
   or contiguous collections so hot loops do not chase object hierarchies.
3. Modules own resources. A browser runtime owns HTTP/TLS services, the UA
   stylesheet, and local/session stores. A page owns its DOM, author CSS,
   computed style engine, layout document, script realm, and decoded images.
4. Input, page events, timers, Promise jobs, and scripted fetches are delivered
   through event checkpoints. Interrupt handlers never execute page script.
5. State machines make lifecycle changes visible. Composition connects these
   components; no browser subsystem inherits behavior from another.

## Ownership by composition

```text
browser_runtime
  +-- HTTP client + TLS options
  +-- UA stylesheet
  +-- persistent localStorage service
  `-- process-lifetime sessionStorage service

browser_page
  +-- URL/base URL and response metadata
  +-- DOM document
  +-- author stylesheet + computed style engine
  +-- layout document + flat paint order
  +-- JavaScript/DOM realm
  `-- decoded image resources
```

The page receives references to runtime services through configuration
structures and callbacks. The DOM bridge does not subclass the network,
storage, or renderer. This makes ownership and teardown explicit and lets host
tests substitute deterministic fetch, cookie, storage, and print services.

## Page lifecycle

Each page has a validated lifecycle:

```text
created -> navigating -> fetching -> response -> parsing
        -> styling -> layout -> scripting -> complete
```

A DOM mutation or viewport resize performs:

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

## Event tasks

Timer and fetch records live in fixed-capacity arrays and move through:

```text
free -> queued -> running -> settling -> free
```

Promise reactions and `queueMicrotask()` use the JavaScript realm's bounded
FIFO job queue. One browser checkpoint advances due tasks and drains jobs.
Repeating timers schedule their next event turn only after the current callback
returns, and cancellation remains valid from inside the callback. GUI idle
events also run checkpoints, so work progresses without keyboard or mouse
input.
Each timer record owns up to 16 forwarded callback values. Animation and idle
requests compose over the same lifecycle with an explicit callback kind:
animation receives the event-turn timestamp, while idle receives a freshly
owned deadline record. Cancellation therefore uses one ID namespace and one
state transition path instead of parallel schedulers.

An `AbortController` and its `AbortSignal` compose over one reference-counted
state record. Fetch tasks keep a non-owning pointer to that state while queued:
an abort moves matching tasks directly from `queued` to `settling`, rejects
their Promises with the signal's exact reason, and frees the task before the
network host is called. The single-threaded synchronous host boundary means a
task already in `running` completes normally; a future nonblocking transport
can add cancellation checks at each connection-state transition.
Timed signals reuse the timer queue and enter the same transition with a
`TimeoutError`. Composed signals form a bounded acyclic dependency graph:
`AbortSignal.any()` validates the complete source array transactionally,
deduplicates sources, and caps both fan-out and graph depth before registering
edges.

Constructed events carry explicit dispatch, stopped, and immediately-stopped
state rather than encoding propagation in the cancellation flag. Dispatch
snapshots the existing DOM parent links into a bounded path, walks it in
capture/target/bubble order, updates target/current-target and phase at each
step, and consults the same listener arrays used by trusted input events.
Each listener is a compact record holding callback, capture, once, passive,
and optional AbortSignal state. Listener removal leaves bounded holes that
dispatch skips; `once` creates that hole before invocation so re-entrant
dispatch cannot call it twice, while abort-linked records are skipped as soon
as their signal transitions.
The active path is also exposed as a copied `composedPath()` Array during
callbacks and cleared when dispatch completes, preventing page code from
mutating the internal traversal snapshot or retaining stale dispatch state.
Specialized UI, mouse, keyboard, focus, input, and pointer events compose
prototype layers over the same Event record. Their constructors only add
normalized initializer fields; cancellation, path traversal, listener
delivery, and lifecycle state remain owned by the one dispatch pipeline.

## Data-oriented storage and layout

Web Storage is an origin-partitioned contiguous table with procedural lookup
and explicit quotas. The storage module owns copied origin/key/value strings.
Its versioned percent-escaped persistence format is imported transactionally.

The separate `libjs/webapi.c` module operates directly on owned byte spans.
Text codecs consume `ArrayBuffer`/`Uint8Array` views without intermediate
number arrays. `URLSearchParams` uses one dense JavaScript array with
alternating key/value entries, preserving order while keeping lookup,
mutation, sorting, and serialization as bounded procedural passes.
`URL` owns a parsed URL record and composes it with one live parameter object;
snapshot comparison synchronizes changes without introducing inheritance or a
second query representation.

Fetch header sets use a compact fixed-capacity table of unique normalized
names and owned values. Script-created sets and response wrappers share the
same reference-counted state; cloning performs bounded procedural copies.
Append combines in place, while set/delete update the dense table without a
second object hierarchy.
Script-created and fetched responses own the same response record shape.
Cloning procedurally copies the bounded body bytes and header table so each
clone advances its own `bodyUsed` state without aliasing mutable header data.
Request records use the same owned header and byte-body components. Fetch
moves a validated snapshot into its queued task, separating transport-owned
`Accept`/`Content-Type` fields from a bounded raw extra-header block. The
browser application rejects cross-origin non-simple requests until a preflight
state machine is present, preventing the compatibility layer from silently
bypassing CORS. Mode, credential, and redirect strings live in the request
record and are copied into the queued transport task, where the connection
state machine enforces same-origin, cookie, credentialed-CORS, and redirect
decisions.
Blob construction appends heterogeneous parts into one capped owned byte
span. Slices copy only their selected range, and Request/Response copy from
that same span while inheriting its validated MIME type when no explicit
`Content-Type` exists.
FormData reuses Blob values and string values already owned by the realm, then
procedurally emits one capped multipart byte span per body snapshot. Field
names and filenames are percent-escaped in disposition headers, preventing
page-controlled CR/LF or quotes from creating injected HTTP headers.
The inverse body pipeline parses URL-encoded pairs or bounded multipart
delimiters back into FormData and reconstructs file parts through the same
File constructor. Callable collection iterators are small host records holding
only source identity, cursor, and iteration kind.

FileReader keeps result/error/readiness in an owning host record and queues a
generation-tagged event-loop task. Generation checks prevent an aborted or
superseded read from publishing stale results. Blob object URLs use a bounded
per-realm registry of copied bytes and are resolved inside the Fetch pump, so
revocation and realm teardown have explicit ownership.

Web `crypto` fills mutable typed-array views through the OS `getrandom()`
boundary. On Kestrel this reaches the SHA-256 mixing pool and ChaCha20 CSPRNG;
UUID formatting only occurs after the secure bytes have been obtained.
Promise-based Web Crypto digests share the TLS library's SHA-256/SHA-384/
SHA-512 core; the compatibility-only SHA-1 path is isolated from certificate
and transport decisions. Navigator processor capacity is injected from the
kernel `cpuinfo()` boundary. Beacon delivery composes a bounded keepalive POST
onto the existing Fetch task state machine, so it does not add a second
networking lifecycle.

`matchMedia()` composes directly over the stylesheet parser's standalone
media-query evaluator and receives the same viewport dimensions as layout.
Location component writes are normalized through the shared URL parser and
become queued navigation intents, preserving the no-recursive-navigation rule
inside script execution.

Same-document history is a bounded per-realm array of URL/state records.
Traversal changes the realm URL without entering the network lifecycle,
dispatches `popstate`, and is synchronized back into the embedding page and
address bar at browser checkpoints. Modern DOM mutation methods compose over
the same capped arena tree operations as parsing; string arguments become text
nodes before the mutation pass. General style-property mutation procedurally
rewrites one bounded declaration string, replacing duplicates and preserving
priority metadata.

ECMAScript collections use dense key and value arrays with bounded linear
SameValueZero lookup. Map and Set iteration is a cursor over those arrays,
avoiding an allocation-heavy node hierarchy. WeakMap and WeakSet use the same
object-keyed representation but deliberately omit size, clearing, enumeration,
and iterator APIs. Because a JavaScript document realm is region-allocated and
freed as a unit today, weak entries naturally have realm lifetime; this is
weaker than tracing-GC ephemeron semantics and is documented as a compatibility
surface rather than claimed garbage-collection observability.

DOM node wrappers now compose separate EventTarget, Node, Element,
HTMLElement/SVGElement, and Document prototype layers while retaining one
arena-owned DOM node record. Element records carry a compact known-namespace
identifier plus an exact arena-owned URI, allowing fast HTML/SVG dispatch
without losing custom namespace identity. Parser placement propagates SVG and
MathML namespaces, while scripted construction and namespace-filtered lookup
reuse the same record and iterative traversal.
The dataset view keeps a stable object per element and binds each discovered
camel-cased key to its owning `data-*` attribute; subsequent `setAttribute()`
keys are added during the next dataset refresh without a parallel attribute
store.
DocumentFragment is an arena-owned node type accepted by the same procedural
tree editor as elements. Inserting one moves its children in order and leaves
the fragment empty, so off-document composition does not introduce a second
tree representation or a special rendering path.

Range is a compact pair of `(node, offset)` boundary points over the same arena
tree. Boundary comparison walks existing parent/sibling links, content
operations reuse the normal clone and mutation pipelines, and contextual HTML
is parsed into an off-document fragment. Selection owns at most one Range
reference and projects anchor/focus state from it; it does not introduce a
second text or tree model.

Mutation observation composes a fixed observer table with the same tree and
attribute operations. Each mutation snapshots a small `MutationRecord` into a
64-entry per-observer queue; event-loop checkpoints drain that queue before
timers run. The callback therefore cannot run recursively inside a DOM edit,
and `takeRecords()` can transactionally transfer the pending snapshot first.

Flex layout gathers participating children into a compact temporary
`flex_item` array, measures them, distributes available main-axis space, and
writes results back in a bounded pass. Row and column directions share the
same record shape while using axis-specific procedural algorithms. Ordinary
block flow remains the allocation-failure fallback.

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
