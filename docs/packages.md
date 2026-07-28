# KestrelOS packages

Three things are specified here and nowhere else:

* the `.kpkg` container,
* the manifest `tools/mkpkg.py` builds one from,
* the repository index `tools/mkrepo.py` writes and `kpkg` reads.

`apps/kpkg.c` (on-target) and `tools/mkpkg.py` (host) are the two
implementations. If they ever disagree, this document is the referee.

Everything is little-endian, which is the only byte order KestrelOS
runs on, and every offset is measured from the start of the file.

---

## 1. The `.kpkg` container

```
+--------------------------------------------------+  0
| header, 64 bytes                                 |
+--------------------------------------------------+  meta_off
| metadata block, meta_len bytes of "key: value"   |
+--------------------------------------------------+  ftab_off (16-aligned)
| file table, ftab_count entries of 192 bytes      |
+--------------------------------------------------+  data_off (16-aligned)
| payload: file contents, each padded to 16 bytes  |
+--------------------------------------------------+  total_size - 32
| SHA-256 of every byte before this point          |
+--------------------------------------------------+  total_size
```

### 1.1 Header (64 bytes)

| offset | type       | field         | meaning |
|--------|------------|---------------|---------|
| 0      | `char[4]`  | `magic`       | `"KPKG"` |
| 4      | `uint32`   | `version`     | format version, currently 1 |
| 8      | `uint32`   | `header_size` | 64 |
| 12     | `uint32`   | `meta_off`    | offset of the metadata block |
| 16     | `uint32`   | `meta_len`    | its length in bytes |
| 20     | `uint32`   | `ftab_off`    | offset of the file table |
| 24     | `uint32`   | `ftab_count`  | number of file-table entries |
| 28     | `uint32`   | `data_off`    | offset of the payload |
| 32     | `uint32`   | `data_len`    | payload length in bytes |
| 36     | `uint32`   | `total_size`  | size of the whole file, digest included |
| 40     | `uint32`   | `flags`       | 0; no flags are defined |
| 44     | `uint32[5]`| `reserved`    | zero |

A reader must reject the file unless `magic` is `"KPKG"`, `version` is 1,
`header_size` is 64, `total_size` equals the actual file size, and all
three blocks lie inside `[64, total_size - 32)`. `kpkg` checks all of
that before it looks at a single byte of content.

### 1.2 Metadata block

Plain ASCII, one `key: value` line per entry, `\n`-terminated, keys
lower-case, written in a fixed order so the output is deterministic:

```
format: 1
name: kestrel-extras
version: 1.0.0
arch: x86_64
description: Extra command line tools - cal and factor.
depends: foo,bar
maintainer: KestrelOS
files: 3
size: 210000
```

* `name` and `version` are required; a package with either missing is
  rejected.
* `depends` is a comma-separated list of package names, no spaces, empty
  when there are none.
* `files` is the file-table entry count, `size` the installed size in
  bytes (the sum of the file sizes, directories counted as zero).
* Unknown keys are ignored by readers, so the block can grow.

### 1.3 File table

`ftab_count` entries of exactly 192 bytes, sorted by `path`, no
duplicates. Because the sort is by full path a directory always precedes
everything inside it, so an extractor can simply walk the table in
order.

| offset | type          | field    | meaning |
|--------|---------------|----------|---------|
| 0      | `char[128]`   | `path`   | absolute install path, NUL-padded |
| 128    | `uint32`      | `mode`   | permission bits, e.g. `0755` |
| 132    | `uint32`      | `uid`    | owner |
| 136    | `uint32`      | `gid`    | group |
| 140    | `uint32`      | `size`   | content length (0 for a directory) |
| 144    | `uint32`      | `offset` | content offset **relative to `data_off`** |
| 148    | `uint32`      | `type`   | 0 = regular file, 1 = directory |
| 152    | `uint8[32]`   | `sha256` | SHA-256 of the content (zero for a directory) |
| 184    | `uint8[8]`    | `pad`    | zero |

`path` must be absolute, must not contain `..`, `//` or whitespace, and
must fit in 127 bytes plus the NUL. The whitespace rule exists because
the installed-file database (below) is one whitespace-separated record
per line. `kpkg` rejects the whole package if any entry breaks a rule -
partial installs from a malformed package are not a thing.

### 1.4 Payload and trailer

File contents are concatenated in file-table order, each padded with
zeroes to the next 16-byte boundary. Directories contribute nothing.

The last 32 bytes of the file are the SHA-256 of bytes
`[0, total_size - 32)`. `kpkg` verifies this before extracting anything,
and verifies each file's own `sha256` again as it writes it out, so a
truncated download or a flipped bit is caught twice.

---

## 2. Manifest format

`tools/mkpkg.py --manifest M --root DIR --out P.kpkg` builds a package
from the directory tree `DIR` (which becomes `/`) plus the manifest `M`.

`#` starts a comment. A line whose first word is `mode`, `own`, `dir` or
`exclude` is a rule; anything else must be `key: value`.

| key | meaning |
|-----|---------|
| `name` | package name (required; letters, digits and `-_.+`) |
| `version` | version string (required) |
| `description` | one line |
| `arch` | defaults to `x86_64` |
| `depends` | comma-separated package names |
| `maintainer` | free text |
| `default-file-mode` | octal, default `0644` |
| `default-dir-mode` | octal, default `0755` |

| rule | meaning |
|------|---------|
| `mode <path> <octal>` | set one entry's permission bits |
| `own <path> <uid>[:<gid>]` | set one entry's owner (`gid` defaults to `uid`) |
| `dir <path> [octal]` | include a directory that is not in `DIR` |
| `exclude <pattern>` | drop matching entries (glob on the install path or the base name) |

Files whose install path starts with `/bin/` or `/sbin/` default to
`0755`, because the shell has to exec them. Rules are applied last and
win over every default. A `mode` or `own` rule naming a path that is not
in the package is an error, not a no-op - a typo there would silently
ship the wrong permissions.

Parent directories of every file are inserted automatically, so a
manifest never has to list them.

Example (`packages/hello/manifest`):

```
name: hello
version: 1.0.0
description: A trivial demo program that greets you.
arch: x86_64
maintainer: KestrelOS
depends: kestrel-extras

mode /bin/hello 0755
own  /bin/hello 0:0

exclude *.o
```

### Determinism

Output bytes depend only on the manifest and the contents of `DIR`:
entries are emitted in sorted path order, payload offsets follow from
that order, and no field comes from the host clock or the host file
mode. Rebuilding a package produces a byte-identical file, so the index
digest is stable across builds.

Other modes of the same tool:

```
mkpkg.py --list PKG.kpkg              metadata and file table
mkpkg.py --check PKG.kpkg             re-hash everything
mkpkg.py --extract PKG.kpkg --into D  unpack (host-side)
```

---

## 3. Repository index

`tools/mkrepo.py --repo DIR` writes `DIR/index.kpi`: one line per
package, sorted by name.

```
# kpkg repository index v1
# name version size sha256 filename depends description
hello 1.0.0 95824 19a12949...73cc hello.kpkg kestrel-extras A trivial demo program.
kestrel-docs 1.0.0 4272 5271b819...ef0c kestrel-docs.kpkg - Handbook pages.
```

Fields are separated by single spaces. The first six are fixed; the
seventh, the description, is the rest of the line and may contain
spaces. `depends` is comma-separated with no spaces, or `-` when empty.
Lines starting with `#`, and blank lines, are comments.

`size` is the size of the `.kpkg` file and `sha256` is its trailer
digest - that is, the SHA-256 of the file minus its last 32 bytes, the
same value the package carries. `kpkg install <name>` compares the
downloaded file against this before it trusts the package's own
trailer, so a repository that serves a substituted file is caught.

`mkrepo.py` verifies every package it indexes and refuses to index a
corrupt one or two packages claiming the same name.

---

## 4. On-target layout

| path | contents |
|------|----------|
| `/etc/kpkg.conf` | configuration (optional) |
| `/var/pkg/repo/` | the local repository shipped in the image |
| `/var/pkg/repo/index.kpi` | its index |
| `/var/pkg/cache/` | packages downloaded over HTTP or HTTPS |
| `/var/pkg/index.kpi` | the index fetched by `kpkg update` |
| `/var/pkg/db/<name>/meta` | the package's metadata block plus `installed: <unix time>` |
| `/var/pkg/db/<name>/files` | one line per installed path |

`/etc/kpkg.conf` is `key = value` (a `:` works too), `#` comments:

```
# Where packages come from. A path, or an http:// or https:// URL.
repo = /var/pkg/repo
cache = /var/pkg/cache
```

Without the file the defaults above apply, so `kpkg` works on an image
with no network at all. HTTPS repositories use TLS 1.3 with certificate
and hostname verification.

### The installed-file database

`/var/pkg/db/<name>/files`, one record per line:

```
<mode> <uid> <gid> <size> <sha256|-> <f|d> <path>
0755 0 0 95160 2f60cbb0...0029 f /bin/hello
0755 0 0 0 - d /bin
```

Fields are whitespace-separated, which is why package paths may not
contain whitespace. The directory is created `0700 root:root`.

The database entry is written **only after extraction has fully
succeeded**. An install interrupted half-way therefore leaves files on
disk but no database entry: `kpkg install` again and it completes; the
files it wrote are simply overwritten. There is never a registered
package whose files are missing.

---

## 5. `kpkg`

```
kpkg install [--force] <name|file.kpkg>...
kpkg remove <name>
kpkg list
kpkg info <name>
kpkg search <text>
kpkg verify [name]
kpkg update
```

`install`, `remove` and `update` require uid 0 (`kpkg` checks
`SYS_GETUID` itself; the filesystem permissions would stop a non-root
user anyway, but the error message is much clearer this way).
`list`, `info`, `search` and `verify` are readable by anyone.

**install** takes either a path (anything ending in `.kpkg`, or
containing a `/`, resolved against the shell's `--cwd`) or a package
name looked up in the index. In order it:

1. loads the file, checks the structure and the trailer digest, and -
   when it came from the index - checks the digest the index promised;
2. refuses to proceed if the package is already installed, unless
   `--force`;
3. resolves `depends` recursively, installing missing ones first.
   The chain of packages currently being installed is kept on a stack,
   so a cycle (`a` needs `b` needs `a`) is reported rather than
   recursed into, and the chain is capped at 16 deep;
4. refuses to overwrite a file another installed package owns, listing
   each conflict, unless `--force`;
5. creates parent directories, writes each file, re-checks its SHA-256,
   then applies `uid`/`gid` and finally `mode`;
6. on a reinstall, deletes files the old version owned that the new one
   does not (unless another package owns them too);
7. writes the database entry.

**remove** walks the recorded file list in reverse, so directories are
tried after their contents. A path that another installed package also
lists is kept and reported. Directories only disappear if they end up
empty, because `SYS_UNLINK` refuses a non-empty one.

**verify** re-hashes every recorded file and reports `MISSING`,
`MODIFIED` (content or size), `UNREAD`, or `CHANGED` (mode or owner
drifted from what was installed).

**update** fetches `<repo>/index.kpi` over HTTP or verified HTTPS into
`/var/pkg/index.kpi`. For a local repository there is nothing to fetch,
so it just reports how many packages the index lists.

### Dependencies of the implementation

`apps/kpkg.c` uses SHA-256 from libc (`libc/sha256.c`, declared in
`libc/include/sha256.h`): `sha256_init`, `sha256_update`,
`sha256_final` over a `struct sha256_ctx`. It does not carry its own
copy. HTTP comes from `libc/http.c` via `<http.h>`.

---

## 6. The shipped packages

Sources live in `packages/<name>/`: a `manifest`, an optional `src/`
holding C programs built with the normal userspace flags, and an
optional `root/` holding static files that map onto `/`. The Makefile
stages `root/` plus the built programs into `build/pkg/<name>/root/` and
runs `mkpkg.py`, then `mkrepo.py` over `build/repo/`, then copies the
result into the image at `/var/pkg/repo/`.

| package | contents |
|---------|----------|
| `kestrel-extras` | `/bin/cal`, `/bin/factor` |
| `kestrel-docs` | `/doc/kestrel/{packages,networking,extras}.md` |
| `hello` | `/bin/hello`; depends on `kestrel-extras` |

`hello`'s dependency is there on purpose: it makes `kpkg install hello`
on a clean system exercise the resolver rather than the trivial path.

```
$ kpkg update
kpkg: local repository /var/pkg/repo, 3 packages
$ kpkg search cal
kestrel-extras     1.0.0      Extra command line tools - cal and factor.
$ kpkg install hello
kpkg: hello requires kestrel-extras
kpkg: installed kestrel-extras 1.0.0 (3 files)
kpkg: installed hello 1.0.0 (2 files)
$ kpkg list
hello              1.0.0      A trivial demo program that greets you.
kestrel-extras     1.0.0      Extra command line tools - cal and factor.
2 packages installed
$ kpkg verify
hello              ok (2 files)
kestrel-extras     ok (3 files)
kpkg: everything matches
```

---

## 7. The HTTP client

`libc/http.c` / `<http.h>` is a GET-only HTTP/1.1 client over
`SYS_TCP_CONNECT/SEND/RECV/CLOSE` and `SYS_DNS`. HTTPS connections use
`libtls` and the same verified TLS 1.3 policy as the browser.

```c
int http_get(const char *url, char **body, unsigned long *len, int *status);
const char *http_strerror(int err);
const char *http_status_text(int status);
```

`http_get` returns `HTTP_OK` whenever a response was received and
parsed - **including 404 and other error statuses** - so callers must
check `*status` too. `*body` is a `malloc`ed buffer of `*len` bytes with
a NUL past the end; the caller frees it. Negative returns are
`HTTP_EURL`, `HTTP_ESCHEME`, `HTTP_EDNS`,
`HTTP_ECONNECT`, `HTTP_ESEND`, `HTTP_ERECV`, `HTTP_EPROTO`,
`HTTP_ETOOBIG`, `HTTP_EREDIR`, `HTTP_ENOMEM` and `HTTP_ETLS`;
`http_strerror()` turns each into a sentence and preserves the direct
TLS verification error for `HTTP_ETLS`.

It handles `host`, `:port` and a path (query strings kept, fragments
dropped), sends `Host:`, `User-Agent:`, `Accept:` and
`Connection: close`, parses the status line and headers
case-insensitively, and reads the body by `Content-Length`, by chunked
transfer-encoding (trailers included) or up to connection close. It
follows up to 3 redirects, resolving an absolute, root-relative or
path-relative `Location:`. Bodies are capped at 8 MiB
(`HTTP_MAX_BODY`), past which it fails with `HTTP_ETOOBIG` rather than
eating the heap. Redirects may cross between HTTP and HTTPS, but an
HTTPS failure never downgrades or retries in plaintext.

`apps/wget.c` is the command-line front end:

```
wget <url>              save as the last path component of the URL
wget -O <file> <url>    save under a given name
wget -O - <url>         write the body to stdout
wget -q <url>           errors only
```

It prints the status, the byte count and the elapsed time.
