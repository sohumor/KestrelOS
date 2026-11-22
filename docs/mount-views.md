# Per-task mount views

The normal host view follows the live mount table. Kernel code can
instead select a task's mount view by listing exact attachment paths
that are already visible to the caller. A view is immutable, can be
empty, and is inherited by new tasks. Lookup still uses the longest
matching path; listing mounts uses the same view. Existing credentials,
unveil and mission checks still apply.

A view pins specific attachments, not their path strings. Detaching
from the host stops new host lookups immediately. A task with the old
view keeps seeing the old instance even if the host attaches a
different filesystem at the same path. Open file descriptions and
in-flight operations also pin the instance. The last reference runs
filesystem teardown and releases its block provider; until then the
instance counts against the fixed mount table limit.

This slice exposes no userspace selector and is not a filesystem
sandbox by itself. Inherited file descriptors remain usable, and
selecting a mounted root can still expose files in the underlying
root filesystem. A later spawn capability can install a curated
view, define descriptor inheritance policy, and decide how views
persist across Hangar snapshots.
