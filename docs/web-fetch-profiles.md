# Libweb fetch profiles

`http_client_new_profile()` binds an explicit set of URL-scheme routes and
an optional content decoder to one HTTP client. For example, a KestrelOS
host can run a document preview through an in-memory HTTP transport while
another client uses a network transport for the same URL. The clients keep
separate connection pools, cookie jars and default response caches.

Each client copies its route names, factories and user pointers. The caller
can discard the route array after construction but must keep the factory
user data alive until it frees the client. To opt into ordinary TCP,
explicitly bind `"http"` to `http_tcp_transport`. Omitting the decoder
advertises identity encoding. Omitting a scheme rejects requests and
redirects to that scheme rather than falling back to the process defaults.
An empty profile has no routes.

A fetch profile scopes libweb's dispatch; it is not a process sandbox.
Factories choose how to resolve hosts and open connections. A host that
installs a replacement cache should give each profile its own cache.

`http_client_new()`, the process-wide scheme and decoder setters, and
the existing browser integration retain their behavior. A host must
choose the profile constructor to opt in; no kernel or syscall ABI changes
are required.
