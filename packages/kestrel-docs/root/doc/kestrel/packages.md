# Using kpkg

`kpkg` installs, removes and verifies KestrelOS packages. Everything
that changes the system needs uid 0.

    kpkg list                    installed packages and their versions
    kpkg info <name>             metadata and the file list
    kpkg search <text>           search the repository index
    kpkg install <name>          install from the repository
    kpkg install ./thing.kpkg    install a package file directly
    kpkg remove <name>           remove a package
    kpkg verify [name]           re-hash installed files
    kpkg update                  refresh the repository index

The repository is configured in `/etc/kpkg.conf`:

    repo = /var/pkg/repo
    cache = /var/pkg/cache

`repo` may also be an `http://` URL, in which case `kpkg update` fetches
`<repo>/index.kpi` and `kpkg install` downloads packages over HTTP.
There is no TLS in KestrelOS, so `https://` is refused outright.

The installed-package database lives in `/var/pkg/db/<name>/`, one
directory per package holding `meta` (the package metadata) and `files`
(one line per installed path with its mode, owner, size and SHA-256).
The database entry is written only after extraction succeeds, so an
interrupted install never leaves a package half-registered.

Two packages may not own the same path. `kpkg install --force` overrides
that, and `kpkg remove` leaves any file that a second package also owns.
