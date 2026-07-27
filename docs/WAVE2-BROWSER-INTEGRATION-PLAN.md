# Wave 2 browser integration plan

Status: implementation gate for the minimum integrated Kestrel browser. No
Wave 2 code lands until this plan is present. This wave replaces the old flat
renderer with the built browser libraries; it does not expand the product.

## Outcome

`browser` keeps its two existing front ends and one navigation path:

- `browser -t [options] <url|file>` remains serial-friendly and testable.
- `browser <url|file>` keeps Back, Forward, Reload, address entry, scrolling, and clickable links.
- Local files, `http://`, and verified `https://` feed the same pipeline.
- GUI content is painted directly into `gui_window->px` by `lay_paint()`.

The required document pipeline is:

```text
bytes
  -> html_parse_document
  -> css_parse(UA) + css_parse(document style_text)
  -> style_engine_new + style_compute_tree(css_style_dom_sink)
  -> lay_layout
  -> lay_paint / lay_link_at
```

Only `css_ua_stylesheet()` and the DOM's collected `style_text` participate.
The UA sheet is `CSS_ORIGIN_UA`, precedes the `CSS_ORIGIN_AUTHOR` document sheet.

## Explicit non-scope

Do not fetch external CSS or images, execute JavaScript, or expose a JS DOM.
Leave image callbacks empty/placeholders. Add no tabs, persisted history,
bookmarks, find, zoom, or download UI; only the existing Back, Forward, Reload,
address, scroll, and link interactions belong in this wave.

## Strict ownership

These lanes are disjoint. Do not make opportunistic edits outside a lane.

| Owner | May change | Must not change |
|---|---|---|
| Architect | `docs/**` only | code, tests, build rules |
| Frontend | `apps/browser.c` only | libraries, tests, Makefile, old HTML files |
| Backend | `libweb/**` only | app, tests, build rules |
| QA | `tools/test_*.c`, `tools/e2e.py` only | app, libraries, Makefile |
| Lead | `Makefile`; later `apps/html.c` and `apps/html.h` deletion | feature work in other lanes |

`libweb/paint.*` and `libtls/**` are exception-only: Backend may touch one only
after Frontend reduces a failure to an isolated public-API defect and records
the defect plus a focused regression. No speculative refactor belongs here.

The existing `.gitignore` modification is user-owned and remains untouched.

## Application state and startup

The application owns one long-lived HTTP client, `struct tls_options`, and UA
sheet. TLS options outlive every pooled HTTPS transport registered with them.

At startup, before the first network navigation:

1. Initialize default TLS options and retain `TLS_VERIFY_REQUIRED`.
2. Call `TLS_REGISTER_HTTPS(&tls_options)` exactly once.
3. Call `http_set_inflate(inflate_buf)` exactly once.
4. Create the HTTP client; its built-in transport supplies plain HTTP.
5. Parse the UA stylesheet once and retain it until application shutdown.

Never use `TLS_VERIFY_NONE`, fall back to HTTP, or claim verification after
registration/trust failure. Show a more specific `tls_last_transport_error()`.

## Navigation transaction

A load is built off to the side and committed only when usable. Failure becomes
a local error document without half-replaced state or dangling history.

1. Convert address input/local paths to a canonical load target.
2. Read local/file targets into a bounded heap buffer; use one `http_client` request for HTTP(S), checking return code and status.
3. Copy `http_response.final_url` before freeing it; redirects, address, history, and relative links use this final base.
4. Parse bytes with `html_parse_document()`; its copies allow the body/response to be released.
5. Parse `doc->style_text`/`style_len` as the author sheet, even at length zero. Ignore external sheets.
6. Build the `[UA, author]` style engine, set its viewport, and call `style_compute_tree(..., doc->root, ..., css_style_dom_sink, ...)`.
7. Initialize `lay_opts` with the content viewport and call `lay_layout()`.
8. Commit the complete page state, reset/clamp scroll, then update history.

Non-2xx is still a response: show a usable body and status. Transport,
allocation, parse, style, or layout failure is readable and exits nonzero in `-t`.

## Lifetime and destruction

The engine borrows sheets, layout borrows the styled DOM, and styles may refer
to sheet strings. Replacement and shutdown therefore use this order:

```text
lay_free(layout)
css_style_dom_free(document->root)
style_engine_free(engine)
css_free(author stylesheet)
dom_document_free(document)
free/copy-owned response or local body as soon as parsing finishes
```

Free the UA sheet after all per-page state. Free the HTTP client before TLS
options leave scope, and release the default store only after pooled connections
close. Every successful `http_fetch()`, including non-2xx, gets one response free.

Relayout frees only layout, updates viewport/media, then rebuilds layout while
the DOM, attached styles, engine, and sheets remain alive.

## Paint, scroll, hit test, and links

The page surface is the window itself:

```text
paint_target = { win->px, win->w, win->h, win->w }
paint origin = content area's top-left in window coordinates
paint scroll = current document scroll
dirty rect   = content viewport in window coordinates
```

Chrome and scrollbar remain libgui-drawn. `lay_paint()` gets a content-clipped
dirty rect and `lay_canvas_color()`; one `gui_flush()` publishes the frame.

Paint and click conversion are inverses for a point inside content:

```text
document_x = window_x - origin_x + scroll_x
document_y = window_y - origin_y + scroll_y
```

Only these coordinates reach `lay_link_at()`; chrome/status/scrollbar points do
not. Thus paint and hit test retain their shared ordering/clipping contract.

Keep `scroll_x = 0`; clamp vertical scroll to
`0..max(0, lay_height(layout)-viewport_h)`. Resolve every `href` against the
final base via `url_resolve_str()`—including root/dot/query/fragment cases.
Unsupported schemes produce status, not a load.

## Text mode

`-t`, `--text`, `-w`, `-l`, and `-v` retain their meanings/exit behavior.
Text mode shares fetch, DOM, cascade, and layout, then writes laid-out text in
document order without a window. `-l` resolves anchors against the final URL.

## Resource envelope

- The user stack is 16 pages = 64 KiB. Bodies, DOM/CSS/layout, TLS, and DEFLATE
  state are heap objects; never stack page-sized buffers or URL arrays.
- Preserve 16 KiB headroom: deepest integrated use is at most 48 KiB. Keep
  parser/layout recursion caps and add no integration recursion.
- Cap HTTP `max_body` and local input at 1 MiB; oversize input is readable.
- Honor library ceilings rather than duplicating unbounded buffers:
  DOM 8 MiB input/64 MiB arena, CSS 2 MiB source/8 MiB arena per sheet, layout
  400,000 boxes/192 MiB arena, HTTP headers 32 KiB, and DEFLATE's explicit cap.
- One TLS connection is about 114 KiB heap and the trust store about 50 KiB;
  the shared client/pool owns them and must release them.
- Allocation failure is recoverable. Page replacement releases all old
  per-page ownership before the replacement becomes the sole live page.

## Integration and commit order

1. **Architect:** land this plan; implementation gate opens.
2. **QA:** land styled local/link fixtures, text assertions, and three-transport/GUI E2E steps.
3. **Frontend:** replace only `apps/browser.c`, preserving CLI/chrome; file-compile it.
4. **Backend (conditional):** land only a reduced API defect plus focused regression.
5. **QA:** run focused host/target tests, capture screenshot/click proof, report the gate.
6. **Lead:** change linking only after Frontend proves zero old-HTML references.
7. **Lead:** verify `rg -n "html_(parse|free|layout)|#include \"html.h\""
   apps Makefile` has zero integration references, then delete
   `apps/html.c`/`apps/html.h` and their build wiring in the same commit.
8. **Lead:** clean WSL build/E2E; merge plan, QA, Frontend, optional Backend, proof, cleanup.

## Numeric acceptance gate

All items are required:

1. WSL compile/link of `build/apps/browser.o` and `build/apps/browser`: 0 errors, 0 new warnings.
2. `browser -t`: 3 loads (local/HTTP/HTTPS), each exit 0 with expected markers.
3. `/bin/browser` fetches 1 real TLS 1.3 endpoint with default verification; `tlstest` or verify-disabled output is not proof.
4. At least 2 screenshots at 900x620 or larger (content at least 800 px) show UA+author CSS; 1 laid-out link click reaches its resolved target.
5. 25 target cycles of Reload/Back/Forward/link: 0 crashes/UAF symptoms or monotonic heap loss; focused host checks: 0 leaks/memory errors.
6. Deepest integrated stack use is at most 48 of 64 KiB; oversize/OOM fixtures fail readably.
7. Old renderer refs are 0 before cleanup; afterward both old files/object prerequisites and `APP_LIBS := html` are absent.
8. Clean WSL `make`, then E2E: at least **46 passed, 0 failed, no more than 1 skipped**.

Anything below this gate remains Wave 2 work; library-only tests cannot mask it.
