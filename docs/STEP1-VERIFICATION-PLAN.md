# Browser Step 1: verification gate

Status: published before implementation. Baseline commit:
`2322387bf38a44d08fd4b9af8cf9db8bf1806125`.

This wave proves or rejects the newly landed TLS 1.3 client and the
layout/painter. They are untrusted until QA reports the measurements below.
It does not rewrite the browser, add DOM bindings, or add browser chrome.
The pre-existing `.gitignore` modification is user-owned and outside this
wave.

## Ownership

Only one owner may write a file during this wave.

| Owner | Writable files | Responsibility |
|---|---|---|
| Architect | `docs/STEP1-VERIFICATION-PLAN.md` | Contracts, gates, triage, and the ship/rework decision. Existing documentation stays untouched until the claims are proven. |
| Backend | `libtls/**`, `libweb/layout.c`, `libweb/layout.h`, `libweb/layout_arena.h` | Fix TLS, transport, layout, bounds, lifetime, or freestanding-build defects demonstrated by QA. |
| Frontend | `libweb/paint.c`, `libweb/paint.h` | Fix painter defects and visually review both generated layouts. No `apps/browser.c` rewrite in this wave. |
| QA | `tools/test_tls.c`, `tools/test_layout.c` | Run and, when needed, strengthen the host harnesses. Preserve raw logs, sanitizer output, PPM/PNG artifacts, counts, timings, memory, and stack figures. QA may reject the wave. |
| Lead | `Makefile`, `apps/tlstest.c` only if integration proves either is defective | Serialize integration, apply build-wiring changes, arbitrate any scope expansion, run the final WSL build/VM gate, and make commits. |

All other files are read-only. In particular, `libgui/**`, `libimg/**`,
`libweb/dom.*`, `libweb/css.*`, `libweb/style.c`, and the crypto primitives
under TLS are treated as frozen dependencies unless QA isolates a failure
there. If that happens, work stops long enough for the architect to publish a
new non-overlapping assignment.

No production source changes are allowed before QA records the initial
executable baseline. A minimal QA-owned harness fix needed to make that
baseline executable is the sole exception, and its revision must be recorded
separately from the unchanged production baseline. No agent runs a full
`make` while another agent is editing.

## Triage decision 1: make the baseline executable

GCC 15.2 rejects `tools/test_tls.c` under the required `-Werror` build with
`-Wstringop-overflow` in `m_read_record`: it cannot prove that the
`rbuf + rlen` write and remaining length stay within the 32,768-byte mock
server buffer. No TLS test has executed yet, so this is a harness compile
blocker, not a libtls result.

The order is fixed:

1. **QA alone** may edit `tools/test_tls.c`. It must make the mock record
   reader prove, before every pointer/length calculation and `read`, that
   `rlen` is nonnegative and no greater than the buffer size, and that the
   complete `5 + record_length` fits. The change must preserve the same
   accepted inputs, rejection limits, reads, mutations, and test counts.
2. QA rebuilds with the exact GCC 15.2 `-Wall -Wextra -Werror`, ASan, and
   UBSan command in this plan. Done means zero warnings and an executable
   binary. Warning suppression, removing `-Werror`, merely enlarging the
   buffer, or weakening a check is not a fix.
3. QA records the harness diff/hash and confirms all production files still
   match the pre-test baseline. It then runs the complete TLS harness and
   publishes the first executable baseline with actual check, failure, skip,
   mutation, sanitizer, and leak counts.
4. Only after that baseline is captured, QA adds a counted-close regression
   for the confirmed `tls_client` contract: failure leaves the lower
   transport open for the caller; success transfers ownership and
   `tls_close` closes it exactly once. The regression must fail against the
   current production implementation.
5. **Backend alone** then edits `libtls/tls.c` to make the public ownership
   contract true. QA reruns the focused regression and the complete TLS
   harness. The fix is done only with one close on success, zero closes on
   handshake failure, no leak, and no other regression.

The confirmed production ownership defect therefore does **not** get changed
ahead of the executable baseline. It gets a failing regression immediately
after that baseline, then a backend fix.

Backend's four layout observations remain candidates until reproduced. After
the initial layout harness baseline, QA alone adds focused cases to
`tools/test_layout.c` for percentage heights with definite versus indefinite
containing blocks, ordered-list continuation after an `li value`, row indices
at the `uint16_t` boundary versus `LAY_MAX_ROWS`, and fixed descendants under
translated/relatively positioned ancestors. Backend may edit only
`libweb/layout.c`, `libweb/layout.h`, or `libweb/layout_arena.h`, and only for
a failing focused case. Frontend has no ownership in this triage unless QA
isolates an error in `libweb/paint.c` or `libweb/paint.h`.

## Triage decision 2: layout fixtures and target stack

The first layout/paint build passed GCC 15.2 `-Werror`. Its ASan/UBSan run
completed 3,000 randomized documents with zero sanitizer and leak findings,
but exited 1 with 255 checks and five assertion failures. Those failures are
not permission for production edits.

QA's focused reduction classifies four assertions as harness/spec errors:

- The left-float fixture supplies only 47 body-font characters
  (376 measured pixels). With a 100-pixel float in a 400-pixel block, the
  remaining 300-pixel line width produces two 16-pixel lines, not the three
  needed to reach `y = 48`. A null “line below the float” is therefore
  expected for that input.
- An ordinary following block may begin at `y = 32` while the float still
  exists; the block box is laid out normally and its line content avoids the
  float. Only `clear: left` requires the block itself to begin at `y = 48`.
- In the right-float case the parent itself establishes the inline formatting
  context, so an extra anonymous block wrapper is not required.
- The paint fixture's first child's top margin collapses through its
  borderless, padding-free parent. Pixel `(5,5)` therefore remains the
  explicitly white canvas; it is not evidence that paint dropped the parent's
  background.

The rendered-article fixture has a related content-box error: a body with
`width: 100%` plus horizontal padding is wider than the viewport, so its own
right-side clipping/overflow cannot be used to reject layout or paint. QA must
use an auto content width (or otherwise account for the padding in the content
box) before visual acceptance.

Ownership and order are exact:

1. **QA alone edits `tools/test_layout.c`.** It corrects the four
   spec-invalid fixtures/assertions without deleting their coverage:
   float tests must still prove shortened line widths, content avoidance,
   full width below the float, right-float placement, and explicit `clear`;
   paint tests must isolate background painting from margin collapse, while a
   separate assertion retains the collapse behavior. QA also corrects the
   article body width-plus-padding fixture and regenerates both wide and
   narrow artifacts.
2. QA reruns the 3,000-document ASan/UBSan suite. Done means zero functional
   failures, zero ASan findings, zero UBSan findings, zero leaks, and the
   original geometry/paint coverage replaced with equivalent spec-grounded
   checks rather than weakened or removed.
3. Stack safety uses a **separate target-like non-ASan run**. Addresses of
   local `char here` probes are not contiguous under ASan fake-stack/redzone
   instrumentation, so the reported 936,976-byte pathological and
   1,095,696-byte large-page figures are invalid as literal target stack use.
   The non-ASan exact `-O2` measurement is 49,193 bytes for the capped
   pathological case and 1,577 bytes for the large page. QA owns the separate
   mode and reports both its command and result. The sanitizer run remains the
   memory-safety gate but does not assert its address-difference stack figure.
   Compiler `-fstack-usage` evidence is retained alongside the dynamic result;
   its observed largest static frame is 2,080 bytes.
4. The 48 KiB target gate remains strict. If QA's corrected, repeatable
   non-ASan mode still reports 49,193 bytes, **Backend alone may edit
   `libweb/layout.c`** to reduce at least the 41-byte overage with reasonable
   headroom, or reduce the capped recursion depth with documented degradation.
   Backend does not edit the harness, headers, arena, or paint for this fix.
   Done means the focused pathological case stays safely truncated
   (`LAY_TRUNC_DEPTH`, 97 or fewer generated boxes), measures below 49,152
   bytes, and the complete sanitizer and non-sanitizer suites both pass.
5. **Frontend makes no production edit in this triage.** `libweb/paint.c` and
   `libweb/paint.h` stay frozen. Frontend visually reviews QA's regenerated
   wide and narrow PPMs. Only a product defect that remains after the
   corrected non-collapsing background fixture and is reproduced by QA can
   open a later frontend-owned paint change.

The accepted baseline numbers remain recorded for comparison: 11,002 nodes to
26,562 boxes, 8,991 KiB layout memory, 58.2 ms layout, 8.5 ms full paint,
3,000 fuzz documents with a worst case of 526 boxes in 0.99 seconds, and a
4,000-deep input degrading with truncation `0x02` to 97 boxes. These are
evidence, not acceptance while corrected assertions or the strict stack gate
remain outstanding.

## Triage decision 3: four proven layout defects

QA's focused candidate suite now produces nine checks and seven failures
against current production, with zero sanitizer diagnostics. The four
observations are therefore backend defects rather than inspection candidates.

File ownership for this fix is disjoint:

- **QA alone owns `tools/test_layout.c`** and the focused/full result logs.
  It freezes the failing reproductions before backend integration.
- **Backend alone owns `libweb/layout.c`**, plus the single
  `LAY_MAX_ROWS` constant in `libweb/layout.h`. No other header/API change is
  authorized.
- `libweb/layout_arena.h`, `libweb/paint.c`, and `libweb/paint.h` are frozen.
  Frontend has no implementation work in this fix.

The backend contracts and done criteria are:

1. A percentage height resolves against a containing block only when that
   height is definite. In QA's cases, a 50% child in a definite 200-pixel
   parent is 100 pixels; the same declaration in an auto-height parent behaves
   as `auto` and takes its 16-pixel content height. Backend must carry
   definiteness separately from a convenient numeric fallback.
2. An ordered-list item's explicit `value` resets that list's current counter
   for following siblings. The frozen sequence must render `3`, explicit
   `10`, then `11`, then `12`; markers may not repeat the pre-reset counter or
   the explicit value.
3. `struct lay_box.row` is a zero-based `uint16_t`, so it represents rows
   0 through 65,535: at most 65,536 rows. The lightweight fix is confirmed as
   changing `LAY_MAX_ROWS` in `libweb/layout.h` from 100,000 to **65,536**,
   not widening the public box field. At and beyond the boundary, layout must
   set `LAY_TRUNC_TABLE`, avoid row-index wrap/aliasing, and remain within
   allocation bounds.
4. A fixed-position descendant is positioned against the initial containing
   block and must not inherit translation from a relatively positioned
   ancestor. QA's viewport-relative result is exactly `(560,360)`, not the
   translated `(585,390)`. Relative translation must continue to apply to
   non-fixed descendants.

Backend may begin after QA records the focused failing test revision and raw
baseline; the files are disjoint, so QA may run read-only deterministic
reductions in parallel. No full build runs during edits. Acceptance requires
the focused suite at 9/9 with zero sanitizer findings, the corrected
273-check/3,000-document suite at zero failures and zero sanitizer/leak
findings, the separate non-ASan stack gate below 48 KiB, and no regression in
the generated wide/narrow layouts. QA, not backend, declares the fix done.

## Contracts

### TLS and HTTP transport

- `tls_connect(host, port, options, error)` performs a verified TLS 1.3
  handshake by default. Verification failure is fatal; an
  `--insecure`/`TLS_VERIFY_NONE` result cannot satisfy acceptance.
- `tls_read` returns positive bytes, zero only after a clean
  `close_notify`, and a negative `TLS_E_*` otherwise. An EOF without
  `close_notify` is `TLS_E_TRUNCATED`. `tls_write` writes the full requested
  length or returns a negative error.
- `struct tls_transport` and `struct http_transport` remain layout-compatible.
  Read, write, close, and timeout callback semantics are those documented in
  `libtls/tls.h` and `libweb/http.h`.
- `tls_client` borrows the caller's lower transport while the handshake is in
  progress. On failure, the caller still owns it and it must not have been
  closed. On success, the TLS connection owns it and `tls_close` closes it
  exactly once. The current header states this contract; QA must add/retain a
  counted-close test because the implementation's `own_lower` path is a known
  inspection risk.
- `tls_transport_open` maps failures to the HTTP error domain while preserving
  the human-readable TLS diagnosis in `tls_last_transport_error`.
- `TLS_REGISTER_HTTPS` is the only dependency direction: the application
  registers libtls with libweb. LibTLS must not acquire a link-time dependency
  on libweb.
- The host build may replace only the bottom TCP transport with BSD sockets.
  The record layer, handshake, transcript, certificate checks, and crypto used
  by the target must be the same source.

### DOM, style, layout, and paint

The later browser integration must use this order:

```
HTML bytes
  -> html_parse_document
  -> css_parse(UA) + css_parse(document style text)
  -> style_engine_new + style_compute_tree + css_style_dom_sink
  -> lay_layout
  -> lay_paint
```

- The DOM owns nodes and source text. Computed styles are attached to DOM
  elements and must outlive layout. Layout borrows the DOM, computed styles,
  and callback results; it owns its box tree and paint-order index.
- Destruction order is `lay_free`, then `css_style_dom_free`/style objects,
  then stylesheet/style-engine cleanup, then `dom_document_free`.
- Layout boxes use absolute document coordinates. Screen-to-document hit
  testing reverses paint's translation:
  `document = screen - origin + scroll`.
- `lay_opts` supplies viewport dimensions and optional borrowed image-size
  callbacks. `lay_layout` must return a usable, explicitly truncated document
  at a resource cap and may return null only when its first allocation cannot
  be made.
- `paint_target` is a caller-owned 32-bit RGB surface. `lay_paint` may write
  only within the target, dirty rectangle, and effective box clip. Image,
  selection, and highlight values supplied through `paint_opts` are borrowed
  for the call.
- Layout's measured font and paint's drawn font are the same
  `struct font`. The no-alpha framebuffer contract is explicit compositing
  into RGB, never storage of an alpha channel.
- Every document-driven traversal respects the existing caps. The measured
  deepest stack use must remain below 48 KiB, leaving headroom on the 64 KiB
  userspace stack.

Public API changes require an architect review before either implementer edits
a header.

## Ordered verification

Every command is run inside Ubuntu WSL from
`/mnt/c/Users/aiari/OperatingSystem`. Raw command lines, exit codes, and output
go into the QA report. A summary without numbers is not evidence.

### 1. Record the environment and baseline

```sh
git rev-parse HEAD
git status --short
gcc --version
openssl version
```

QA records whether WSL has outbound DNS/TCP access separately from product
failures.

### 2. TLS host harness under ASan and UBSan

```sh
gcc -Wall -Wextra -Werror -O2 -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Ilibtls -DTLS_HOST -o /tmp/kestrel-test-tls \
  tools/test_tls.c libtls/tls.c libtls/roots.c libtls/hash.c \
  libtls/aead.c libtls/ecc.c libtls/rsa.c libtls/bignum.c libtls/x509.c
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  /tmp/kestrel-test-tls
```

This harness must actually start `openssl s_server`; skips do not count.
QA reports:

- OpenSSL version;
- negotiated results for all three TLS 1.3 suites, X25519, P-256, both
  HelloRetryRequest directions, RSA and P-256 certificates, ALPN, and
  CertificateRequest;
- number of traffic-secret comparisons with OpenSSL's key log;
- positive, negative-certificate, and negative-protocol check counts;
- mutated-flight and random-record counts, accepted mutation count, leak
  count, ASan findings, and UBSan findings;
- heap size of one connection and the trust-store size;
- the counted-close ownership result for `tls_client`.

Any unexpected skip, accepted mutation, sanitizer finding, leak, or nonzero
failure count rejects TLS.

### 3. A real verified HTTPS fetch

After the local `s_server` suite passes:

```sh
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  /tmp/kestrel-test-tls fetch example.com 443 / \
  > /tmp/kestrel-real-https.log
```

Acceptance requires all of the following in the captured result:

- a completed TLS 1.3 handshake using one of the implemented suites/groups;
- `verified 1`, never insecure mode;
- ALPN `http/1.1` or no ALPN followed by an HTTP/1.x response;
- a real HTTP status line and non-empty response bytes;
- no sanitizer report.

If outbound networking or the remote service is unavailable, QA records that
as an environmental block and retries one independently operated documented
site such as `www.iana.org`. A local server, certificate verification disabled,
or `curl`/OpenSSL fetching on behalf of libtls does not satisfy this gate.

### 4. Layout and paint host harness under ASan and UBSan

```sh
gcc -Wall -Wextra -Werror -O2 -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -DPAINT_WITH_STDIO -Ilibweb -Ilibgui -Ilibimg \
  -o /tmp/kestrel-test-layout \
  tools/test_layout.c libweb/layout.c libweb/paint.c libweb/dom.c \
  libweb/html.c libweb/entities.c libgui/font.c libgui/font_data.c
rm -rf /tmp/layout-shots
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  /tmp/kestrel-test-layout 3000
```

QA reports:

- total checks and failures;
- exactly 3,000 randomized styled documents completed and the worst box
  count;
- ASan, UBSan, and leak counts;
- 4,000-level nesting truncation flag, box count, and measured stack bytes;
- large-page DOM-node, box, text, line, table, memory, depth, layout-time,
  full-paint, and dirty-strip figures;
- hashes and dimensions of `article.ppm` and `article-narrow.ppm`.

Frontend converts or opens both PPM artifacts and reports visual results at
760 px and 360 px: readable glyphs, visible hierarchy, correct wrapping,
float flow, list markers, table columns/borders, link styling, no clipping,
and genuine narrow-width reflow. A numeric-only report cannot accept paint.

The default-style stub build above must pass first. QA then performs a second
integration build using the real `css_style_initial` from `libweb/style.c`
(with the required CSS objects and `LAYOUT_TEST_REAL_CSS`) and compares the
check/failure result. A stub/real mismatch rejects the layout integration.

### 5. Freestanding archive checks

With implementers idle, the lead runs only scoped targets:

```sh
make build/libtls.a build/libweb.a
tools/build-libs.sh
```

QA records archive sizes and every warning/error. No host-only symbol or
standard-library dependency may enter the target archives.

### 6. Serialized target and regression gate

Only after the host gates pass and all edits stop:

```sh
make
tools/try-cmds.sh 30 \
  'tlstest --roots' \
  'tlstest --head example.com 443 /'
make test
```

The target smoke must show a nonempty root store, a verified handshake, an
implemented cipher/group, and a real HTTP response over KestrelOS's TCP stack.
QA reports the end-to-end total as passed/skipped/failed, including the
pre-existing shell-pipe skip. This wave does not fix that skip.

## Acceptance and triage

Wave 1 is done only when:

1. both sanitizer harnesses exit zero with zero sanitizer/leak findings;
2. no required OpenSSL interop case is skipped;
3. every OpenSSL traffic-secret comparison matches;
4. all 500 mutated flights are rejected and all 200 random streams complete
   without a crash, or QA reports larger actual counts;
5. a real public HTTPS response verifies and returns nonempty bytes;
6. layout completes 3,000 random documents, its pathological cases degrade
   through truncation flags, and measured stack use is below 48 KiB;
7. both wide and narrow renders pass frontend visual review;
8. libtls/libweb build freestanding, the target `tlstest` fetch succeeds, and
   the existing end-to-end suite has no new failure.

When a gate fails, QA sends the smallest reproduction and raw result to the
lead. The architect assigns it to exactly one owner. The owner runs only the
focused reproducer; QA reruns the failed gate and then the complete relevant
harness. Authors do not declare their own fixes accepted.

## Integration and commit boundaries

Commits remain small and ordered:

1. `docs: define the browser verification gate` — this document only.
2. `test(tls): cover <specific contract or failure>` and/or
   `test(layout): cover <specific contract or failure>` — QA reproductions,
   separate by subsystem.
3. `tls: <reason for verified fix>` — backend TLS files only.
4. `layout: <reason for verified fix>` — backend layout files only.
5. `paint: <reason for verified fix>` — frontend paint files only.
6. `browser verification: record measured Wave 1 evidence` — this document,
   after QA acceptance.

The lead stages explicit paths and verifies each diff before committing so
the user's unrelated `.gitignore` edit and another agent's work cannot be
captured. Rebase, reset, broad checkout, and cleanup commands are forbidden.

Passing this gate opens the next wave: rewrite `apps/browser.c` over
HTML -> CSS -> style -> layout -> paint and remove `apps/html.c` plus its
Makefile wiring. It does not itself prove the finished browser; the later
browser wave must repeat a real verified HTTPS fetch through `http_fetch` and
the GUI pipeline.
