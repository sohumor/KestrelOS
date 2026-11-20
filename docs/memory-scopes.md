# Memory scopes

KestrelOS libc's `<memscope.h>` lets a host limit the backing memory
used by several related lifetimes. For example, a host can put a
document and a short-lived parsing stage on one byte budget, then
reset the parser's scope without invalidating the document's objects.
Neither lifetime needs its own private quota.

Initialize a `k_mem_budget` with the total number of bytes and a
`k_mem_scope` for each independent lifetime. Allocate objects with
`k_mem_scope_alloc()`. Use `k_mem_scope_reset()` to free all objects
in one scope and make their backing credit available to every scope
on that budget. Do not pass individual scope allocations to `free()`.

The charged amount is the size of each chunk requested from libc's
heap, including its scope header and spare capacity. It does not
include internal heap metadata or measure resident pages. A scope can
reuse space in its current chunk even when no unreserved budget is
left. A failed allocation neither evicts sibling data nor falls back
to unbudgeted allocation.

This is cooperative accounting, not a kernel isolation boundary.
Callers must serialize access to a shared budget and keep it alive
until its scopes have been reset.
