# Wave 2 browser integration addendum

This addendum refines
[WAVE2-BROWSER-INTEGRATION-PLAN.md](WAVE2-BROWSER-INTEGRATION-PLAN.md) after
the backend seam review. Its decisions are part of the Wave 2 merge gate.

## Shipped page and QA ownership

The current image is stale in two observable ways:

- `rootfs/doc/home.html` says HTTPS is refused, while Wave 2 requires verified
  HTTPS through `/bin/browser`; `t_browser_home` also asserts `"no TLS"`.
- `rootfs/doc/test.html` says the renderer ignores CSS, while the integrated
  pipeline intentionally applies its embedded author sheet.

Ownership remains disjoint:

| Surface | Owner | Required change |
|---|---|---|
| `rootfs/doc/home.html` | Lead | Remove the no-TLS/HTTP-only claim, use at least one `https://` example, and describe verified TLS 1.3 without hiding its unaudited/limited status. Preserve short stable start-page markers. |
| `rootfs/doc/test.html` | Lead | Replace the “CSS is ignored” comment. Keep the sheet embedded, make its supported author styling visually unambiguous, and add a visible stable `CSS-AUTHOR-OK` marker. Do not add external CSS or image fetching. |
| `tools/e2e.py::t_browser_home` | QA | Replace the stale `"no TLS"` assertion with a short marker from the revised verified-HTTPS copy; retain the page/title/local-page checks. |
| `tools/e2e.py::t_browser_text` and GUI proof | QA | Assert `CSS-AUTHOR-OK` plus the existing text/link markers. Text mode proves the shared pipeline remains readable; the wide GUI screenshot, not serial color text, proves author styling. |

The Lead updates both rootfs pages only after HTTPS and styled GUI rendering
work, then gives QA the exact stable phrases. QA owns corresponding assertions
and must not edit the pages. The final image/E2E gate includes both changes.

## Backend seam decisions

### `http_response.final_url`: Wave 2 blocker

`http.h` promises `final_url` is always set on `HTTP_OK`, but fresh-cache and
normal finish paths can currently ignore allocation failure and return
`HTTP_OK` with `final_url == 0`. Wave 2 relies on that value for redirect
history, the address field, and relative-link resolution. Falling back to the
requested URL after a redirect would silently produce the wrong base.

This contract defect blocks final Wave 2 acceptance:

1. Backend owns the focused `libweb` fix: allocation failure returns
   `HTTP_E_NOMEM` with response ownership left safe for documented cleanup.
2. QA owns a focused `tools/test_*.c` regression covering successful fresh
   cache and ordinary completion paths and the non-success allocation path.
3. Frontend still null-checks before copying as a defensive boundary and turns
   an impossible/legacy null into a readable OOM load failure; it does not use
   the requested URL as a redirect-base substitute.

Frontend work may continue in parallel, but old-renderer deletion and the final
acceptance run wait for the contract fix and regression.

### 64 MiB gzip transient: deferred bounded hardening

The browser's 1 MiB `http_request.max_body` is an accepted decoded-body limit,
not a peak-allocation guarantee. The current `http_inflate_fn` has no limit
argument, so `inflate_buf()` may transiently grow to
`INFLATE_DEFAULT_MAX` (64 MiB) before HTTP rejects decoded output over 1 MiB.

This is not a Wave 2 blocker because the allocation is bounded, decoder
allocation failure is recoverable, and propagating the per-request limit needs
a deliberate libweb/libz API change. Wave 2 must:

- retain the 1 MiB browser body limit and show compressed oversize/decode/OOM
  failure readably;
- state honestly that encoded responses may transiently allocate up to 64 MiB;
- promote this to a blocker if focused target testing produces a crash, leak,
  or unrecoverable OOM.

Post-Wave-2 hardening should add a cap-aware HTTP inflater seam backed by
`inflate_buf_limit()`, passing the request's remaining decoded budget rather
than the global 64 MiB default.

### Missing paint order after OOM: Wave 2 blocker

`lay_layout()` can currently return a nontrivial document after paint-order
allocation fails, while `lay_truncated()` does not report
`LAY_TRUNC_MEMORY`. `lay_paint_order()` then returns zero and the browser can
mistake a blank page for a successful load.

This is a Wave 2 blocker because allocation failure must be readable and a
styled nonempty document must never pass acceptance as blank:

1. Backend owns the focused `libweb` fix. A failed paint-order allocation must
   set `LAY_TRUNC_MEMORY`; retain the documented late-failure/partial-layout
   contract rather than silently returning an apparently complete document.
2. Frontend owns a defensive boundary in `apps/browser.c`: if
   `lay_box_count(layout) > 1` but `lay_paint_order(layout, ...) == 0`, reject
   the candidate page as OOM before history/state commit. This remains useful
   against an older library and is not a substitute for the backend signal.
3. QA owns a focused allocation-failure regression in `tools/test_*.c` plus an
   application assertion that the failure is visible and non-successful.

Acceptance requires paint order greater than zero for the nontrivial styled
fixture; an injected order-allocation failure must set `LAY_TRUNC_MEMORY`,
produce a readable load error, and leave the prior page/history transaction
intact. Old-renderer cleanup waits for this gate.

### Fixed-position paint/hit seam: later CSS hardening

Painting currently applies global scroll to `LAYF_FIXED` boxes. The public hit
APIs accept only document coordinates and have no viewport/scroll context, so
they cannot independently keep a fixed box at viewport coordinates. Fixed
elements therefore scroll with the document.

This is deferred, not a Wave 2 blocker: it is incorrect fixed-position CSS,
but it is bounded, does not create a memory-safety failure, and the current
shared document translation keeps ordinary painted and clicked content
aligned. A painter-only special case is forbidden because it would make a
visible fixed link disagree with hit testing.

The later fix belongs to Backend across the `libweb` paint/layout/hit contract,
with Frontend adopting the new viewport-aware hit seam and QA adding a test
that a fixed link remains at the same window point and clickable across scroll.
Wave 2 still requires the normal scrolled-link click acceptance case. Promote
fixed positioning to a blocker only if scoped testing exposes a crash or an
actual paint/hit mismatch, not merely that fixed content scrolls.

## Routed Frontend P1/P2 fixes

All items below belong exclusively to Frontend in `apps/browser.c`; QA owns
their focused assertions. Priority controls integration order, not whether
they are required before final acceptance.

- **P1 — text order:** project text with a nonrecursive layout-box DFS, not
  paint order or numeric `order`. Absolute/fixed DOM-first text may be painted
  after normal flow; `-t` must remain DOM/layout order and must respect the
  64 KiB stack limit.
- **P1 — hostile geometry arithmetic:** use checked 64-bit intermediates and
  clamp conversions for text `box->x * cols` and scrollbar products. Large CSS
  geometry must not trigger signed overflow or an out-of-range draw/hit value.
- **P2 — TLS diagnostics:** append `tls_last_transport_error()` only for the
  HTTPS transport-open failure it describes, and never display literal
  `"no error"` on unrelated HTTP failures.
- **P2 — generated HTML sizing:** plain-text and error-page wrapping must
  budget the full worst-case six-byte quote escape, rather than a five-times
  buffer that can silently truncate.
- **P2 — transactional navigation:** Back, Forward, new-history allocation,
  and fatal candidate-load failures must roll back index/history/page state;
  failed loads never consume or invent a navigation entry.
- **P2 — blank-order defense:** retain the nontrivial-layout/zero-paint-order
  check described above until and after the Backend truncation fix.

QA acceptance covers deterministic `-t` text order, hostile geometry without
overflow, transport-specific TLS wording, worst-case quote escaping without
truncation, transactional load/history rollback, and the blank-order OOM gate.

## Lead Makefile and cleanup duties

The Lead owns these changes after Frontend has zero old-renderer references:

1. Fix static archive order. The browser link must be equivalent to:

   ```text
   CRT0 browser.o libweb.a libjs.a libimg.a libtls.a libz.a libgui.a libc.a
   ```

   `libgui.a` must follow `libweb.a` because layout/paint import `font_*`.
   A linker group containing those archives is an acceptable equivalent.
2. Extend the final `-include` block to cover generated dependency files under
   `build/libgui/`, `build/libz/`, `build/libtls/`, `build/libimg/`,
   `build/libweb/`, and `build/libjs/`, in addition to existing app/libc paths.
3. Remove `build/apps/html.o` from the browser prerequisites/link line, remove
   `APP_LIBS := html` and its filtering role, then delete `apps/html.c` and
   `apps/html.h` in the same cleanup change.
4. Before deletion, require zero old-renderer references from the Frontend
   search gate and a successful file-scoped browser link. After deletion, run
   clean WSL `make` and the full **47 passed / 0 failed / at most 1 skipped**
   E2E gate.
