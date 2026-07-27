# Wave 2 certificate-negative crash ruling

Status: **Wave 2 blocker; Wave 3 remains suspended.** This ruling is the
implementation gate for the controlled self-signed TLS 1.3 crash. It assigns
one durable fix and disjoint proof ownership. It does not authorize browser,
HTTP, TLS, X.509, layout, or paint changes.

## Observed failure and exact cause

The controlled TLS 1.3 fixture presents a parseable certificate whose first
DNS SAN is `kestrel-negative.invalid`, 24 bytes long. Verification correctly
rejects that SAN for the requested host and
`libtls/x509.c:1622` constructs the bounded diagnostic with:

```c
"the certificate is for '%.*s'%s, not '%s'"
```

The SAN is a length-delimited DER slice, not a C string, so `%.*s` is the
correct operation. On the target, however, `libc/stdio.c` parses flags and a
numeric field width and then treats `.` as an unknown conversion. It does not
parse precision and therefore does not consume the precision `int`. The later
`%s` consumes that integer as its pointer. Here the integer is 24, or `0x18`,
so target `strlen()` receives `0x18`.

This exactly accounts for the captured evidence:

- exception 14, read fault at address `0x18`;
- RIP `0x44daf4`, the first dereference in target `strlen()`;
- the fixture SAN length is exactly `0x18`;
- host TLS tests do not reproduce it because they use the host C library's
  conforming formatter.

The gap is systemic rather than X.509-specific. Read-only inventory also finds
`apps/init.c` using `%.*s%d` for bounded service names and nine `%.Ns`
diagnostics. Those sites presently format incorrectly and the star form can
misalign later arguments in exactly the same way. They are evidence for the
libc ruling, not authorization to rewrite each caller.

The fault happens while X.509 is building its rejection reason, before
`tls_transport_open()` can return a failure to `libweb` and before
`apps/browser.c` can build or present an error page. Rendering and response
body ownership are not on the failing path.

## Ruling

Fix target `%s` precision in the shared formatter in `libc/stdio.c`.
Do **not** replace the bounded X.509 format with a temporary NUL-terminated
copy. That narrow workaround would mask a general target-libc varargs defect,
leave every other `%.*s` caller exposed, and weaken the correct
length-delimited X.509 contract.

The implementation scope is deliberately bounded:

1. Parse an optional precision after field width: `.` followed by decimal
   digits, `*`, or nothing. A bare `.` means precision zero.
2. A `*` precision consumes exactly one `int`. A negative value means
   “precision omitted,” matching normal `printf` behavior.
3. For `%s`, a nonnegative precision is the maximum number of bytes read and
   emitted. The formatter must stop earlier at NUL, but must not call
   unbounded `strlen()` or read byte `precision + 1` from a non-NUL slice.
4. Field width is computed from that bounded string length; existing left and
   right space padding remains intact.
5. Precision syntax must not desynchronize later arguments. If precision is
   parsed for another currently supported conversion but is not implemented
   for it in this patch, its `*` is still consumed once and the unsupported
   semantic must be documented and tested. This ruling does not require
   adding new conversions or floating-point formatting.
6. Preserve current `snprintf` truncation semantics: return the full
   would-have-written byte count and NUL-terminate whenever `size > 0`.
   `snprintf(NULL, 0, ...)` must perform no write, consume arguments normally,
   and return the full count.
7. Preserve the target library's existing `%s` NULL policy of rendering
   `"(null)"`; precision applies to that replacement string. A NULL buffer
   with nonzero size remains outside the contract.
8. Decimal precision parsing must not overflow its accumulator. Saturating at
   the implementation's maximum usable `int` is sufficient.

These are byte-string semantics, not Unicode character precision. No claim is
made that this small formatter becomes a complete ISO C `printf`
implementation.

## Disjoint file ownership

| Lane | Owned files | Required work |
|---|---|---|
| Backend — libc | `libc/stdio.c`, `libc/include/stdio.h` | Implement and document `%s` literal/star precision in the shared `format_core()` path used by `vsnprintf` and `vprintf`. |
| Architect — docs | `docs/ABI.md` and this ruling | Update the public formatter contract after the implementation and proof agree with this ruling; no production-code or QA ownership. |
| QA — focused host | New `tools/test_stdio.c` (or one equivalently focused new formatter harness) and optional `tools/run-stdio-tests.sh` | Compile the actual target `libc/stdio.c` formatting core on the host, exercise the contract below under ASan/UBSan, and publish the command/counts. |
| QA — TLS/E2E | `tools/test_tls.c` and `tools/e2e.py` only | Preserve host bad-certificate coverage and prove controlled target rejection plus public HTTPS success. |
| Lead/integration | `Makefile`, final image build, final serialized E2E run, acceptance record | Lead alone wires the focused test target; integrate only after the focused host gate is green. |

The following files are frozen for this correction:

- Frontend: `apps/browser.c`;
- TLS/X.509 and its transport adapter: `libtls/tls.c`, `libtls/x509.c`, and
  their headers;
- Web transport/client: `libweb/http.c`, `libweb/http.h`;
- DOM, CSS, layout, and paint code.

Read-only inspection shows their existing contracts are appropriate:

- X.509 rejects the hostname and supplies a bounded, certificate-specific
  diagnostic.
- `tls_transport_open()` retains that diagnostic and maps handshake failure
  to a negative HTTP transport-open result.
- `http_fetch()` zero-initializes the response, stops when transport open
  fails, frees its staging buffers, and cannot expose a response body.
- `page_load()` turns the returned transport error into a failed page;
  text mode consequently returns status 1.

Only new post-fix evidence of a separate violation may reopen one of those
files, under a new ruling. The libc implementer must not make a compensating
X.509 edit, and Frontend must not catch or hide a process fault.

## Host done gates

All of these gates are mandatory:

1. The focused harness links the real target formatter implementation and
   records at least 24 named assertions with zero failures.
2. Literal precision covers `%.0s`, `%.3s`, bare `%.s`, precision longer than
   the source, and precision combined with left/right width.
3. Star precision covers zero, 24, a value longer than the source, and a
   negative value.
4. A 24-byte non-NUL-terminated slice ending at a mapped page boundary, with
   the following page inaccessible, formats through `"%.*s"` with no read
   past byte 24.
5. An X.509-shaped call,
   `"'%.*s'%s, not '%s'"`, produces the complete expected SAN and hostname
   text, proving the following pointer arguments remain aligned.
6. A mixed call such as `"%.*s|%s|%d"` proves the precision integer, bounded
   string pointer, later string pointer, and later integer are each consumed
   exactly once.
7. Output capacities 0, 1, smaller than the result, exactly the result plus
   NUL, and larger than the result all return the same full length and preserve
   canaries. Capacity 0 is also run with a NULL destination.
8. NULL `%s` with omitted, zero, and short precision follows the documented
   `"(null)"` policy without a fault.
9. At least 10,000 reproducible seeded-random, defined-input format cases are
   compared with the host formatter for literal/star `%s` precision, width,
   truncation, and return count. The seed is published. Host-undefined NULL
   `%s` cases are checked only against the Kestrel policy.
10. ASan and UBSan report zero findings; the guard-page or poisoned-tail
    non-NUL test reports zero overreads.
11. Existing host TLS/X.509 negatives for wrong hostname and self-signed/no
    root still reject with certificate-specific text, and the full existing
    host TLS suite has zero failures.

Passing only the system-libc TLS test is not proof of this fix.

## Target done gates

Run these serially from the same clean image used for final Wave 2 acceptance:

1. The fixture self-test proves the controlled endpoint is TLS 1.3, strict
   verification rejects it, and an insecure host control can retrieve its
   unique body marker.
2. `/bin/browser -t` against the controlled plain-HTTP fixture still renders
   its unique marker and exits exactly 0.
3. `/bin/browser -t` against the controlled self-signed endpoint exits
   exactly 1, prints a certificate/hostname-specific diagnostic containing
   the presented SAN or equivalent trust reason, and never prints the
   endpoint's body marker.
4. The negative endpoint records zero HTTP GET requests. Reaching its handler
   would mean application bytes crossed a handshake that had to fail.
5. The negative case is repeated 10 times. Every run exits 1; there is no
   user-process exception, page fault, kernel panic, hang, generic
   `"no error"` text, or target-memory decline across runs.
6. A public `https://example.com/` browser fetch immediately after the
   negative sequence still renders `Example Domain`, exits exactly 0, and
   reports no load error. This remains verified TLS 1.3; disabling
   verification is forbidden.
7. The complete E2E suite finishes with zero failures (and no new skip), with
   controlled HTTP, certificate-negative TLS, and public HTTPS reported as
   three separate results.

The acceptance record must include the focused host command and assertion
count, sanitizer summaries, the target negative diagnostic, all explicit
shell status markers, proof that the forbidden body marker and process-fault
text were absent, the endpoint GET count, and the public HTTPS marker.

## Integration order and release gate

1. QA lands or stages the focused formatter regression first.
2. Backend changes only the libc-owned formatter files until that regression,
   its sanitizer run, and existing host TLS tests are green.
3. Lead performs the clean target build and the controlled negative/public
   positive sequence with no concurrent image or QEMU writers.
4. Lead runs the full E2E suite and publishes the evidence above.
5. Wave 2 may be accepted, and Wave 3 planning may resume, only after every
   host and target gate in this ruling passes.

Until then, the production certificate-negative crash is a P0 blocker. Local,
home-page, controlled HTTP, and public HTTPS successes do not waive it.
