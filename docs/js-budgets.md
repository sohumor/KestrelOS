# Shared JavaScript budgets

A libjs host can group independent contexts into one workload without
sharing their globals, objects, or lifetimes. Create a
`js_heap_budget` with `js_heap_budget_init(&heap, bytes)`, then set
`cfg.heap_budget = &heap` on each participating context before calling
`js_new(&cfg)`. Contexts can also draw metered interpreter steps from a
separate `js_execution_budget` through `cfg.execution_budget`.

Each context still has its own `max_heap` and `max_steps`. The shared
heap pool charges the same backing memory as the local cap: whole arena
chunks (including unused capacity and chunk headers) and the live
capacity of temporary string buffers. It does not count host objects,
the context structure itself, or temporary copies made inside the host
allocator. `js_heap_budget_remaining()` reports the unreserved bytes.

An allocation that cannot draw enough shared credit produces an
uncatchable fatal heap error in the allocating context, not in its
siblings. If there is not enough credit to initialize a new context,
`js_new()` returns null. Freeing a string buffer returns its credit
immediately; destroying a context returns its arena credit. This makes
it possible to replace one script realm without invalidating objects in
another. Unlike heap credit, used execution credit is never returned.

The host must keep each pool alive until all attached contexts are
destroyed. Initialize it before first use and do not copy or reinitialize
it while attached. Independent contexts can draw from a pool
concurrently, but the pool does not make an individual context safe for
concurrent calls. Both budgets are opt-in; existing hosts keep their
current defaults.
