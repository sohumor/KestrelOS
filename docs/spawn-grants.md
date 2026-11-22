# Spawn grants

`spawn_grant(path, argv, &grant)` starts a child with one explicit
bundle of authority. Set `version` to `K_SPAWN_GRANT_VERSION`. The kernel
intersects `caps.cap_mask` with the caller's capabilities; zero grants no
bits. The child also keeps the caller's existing mission and unveil
restrictions. `caps.cwd` sets the initial directory metadata, and
`caps.fds` explicitly maps existing parent descriptors into child slots.
Unmapped descriptors are not copied. A granted descriptor remains
usable even when its path is outside the child's mount view.

Set `K_SPAWN_GRANT_MOUNT_VIEW` to select exact attachment paths from the
caller's [mount view](mount-views.md) or, for an unscoped caller, the
live host table. The selected attachments are pinned before launch.
The child uses that view to load its executable and libraries as well
as for later path operations. Descendants inherit the view and can
select fewer attachments, but cannot add ones their spawner cannot see.
Without the flag the view is inherited; with the flag and zero mounts,
the view is empty, so a path-launched executable cannot load.

Unknown flags, an unsupported version or request size, duplicate
descriptor destinations, and invalid mount selections are rejected
before publishing a child. Image load failures keep the existing spawn
behavior: the call returns a PID and the child exits with status -1.
The older `spawn_caps` ABI and its zero-mask inheritance rule are unchanged.

Hangar cannot park a task with a scoped mount view: its on-disk format
does not preserve attachment identity. Mount selection is not a rewrite
of the root filesystem, and choosing the root does not hide files in it.
