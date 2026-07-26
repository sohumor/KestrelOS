# KFS — the KestrelOS filesystem

KFS is a deliberately small fixed-layout filesystem: one superblock, one
block bitmap, a fixed inode table, then data blocks. All metadata updates
are write-through; there is no journal and no cache to flush.

All multi-byte fields are little-endian.

**This document describes on-disk format version 2** (magic `"KFS2"`).
v2 added `mode`, `uid`, `gid` and `mtime` to the inode so KestrelOS could
grow multi-user support, and dropped `nlink` (KFS has never supported hard
links, so the field was never anything but decoration). The inode stayed 64
bytes by giving up two direct block pointers. A v1 (`"KFS1"`) image is
**not** upgraded in place: the kernel recognises the old magic and refuses
the mount with a message telling you to rebuild, because a v1 inode's
`nlink`/`direct[10..11]` sit exactly where v2 keeps `mode` and `indirect`.
Rebuild with `tools/mkfs.py`.

## Placement on disk

The FS lives in a partition starting at disk sector `FS_START_LBA`
(2048, defined in `kernel/include/ata.h`). Everything below is expressed
in **FS blocks**: a block is 4096 bytes = 8 disk sectors, and block
numbers are relative to the start of the partition. FS block `B` therefore
occupies disk LBA `FS_START_LBA + 8*B` through `FS_START_LBA + 8*B + 7`.

## Layout

| blocks                        | contents      |
|-------------------------------|---------------|
| 0                             | superblock    |
| `bitmap_start .. +bitmap_blocks-1` | block bitmap |
| `inode_start .. +inode_blocks-1`   | inode table  |
| `data_start .. total_blocks-1`     | data blocks  |

With the default geometry (`mkfs.py`, 1024 inodes) `bitmap_start` is 1,
`inode_start` is `1 + bitmap_blocks`, and `data_start` is
`inode_start + 16`.

## Superblock (block 0)

First 40 bytes of block 0; the rest of the block is zero.

| offset | type | field         | meaning                                  |
|--------|------|---------------|------------------------------------------|
| 0      | u32  | magic         | `0x3253464B` ("KFS2")                    |
| 4      | u32  | total_blocks  | size of the FS in blocks                 |
| 8      | u32  | bitmap_start  | first bitmap block (= 1)                 |
| 12     | u32  | bitmap_blocks | bitmap length in blocks                  |
| 16     | u32  | inode_start   | first inode-table block                  |
| 20     | u32  | inode_blocks  | inode-table length in blocks (16)        |
| 24     | u32  | inode_count   | number of inode slots (1024)             |
| 28     | u32  | data_start    | first data block                         |
| 32     | u32  | root_ino      | root directory inode (= 1)               |
| 36     | u32  | free_blocks   | count of free blocks                     |

## Block bitmap

One bit per block of the whole FS, LSB-first within each byte: block `B`
is bit `B % 8` of byte `B / 8`. Set = used. Blocks `0 .. data_start-1`
(superblock, bitmap, inode table) are pre-marked used at mkfs time.
Padding bits at or past `total_blocks` are always zero.

## Inodes

64 bytes each, 64 per block. Inode numbers are **1-based** indices into
the table: inode `i` lives at byte `(i-1) * 64` of the table. Inode 1 is
the root directory. `inode_count` defaults to 1024 (16 blocks).

| offset | type    | field    | meaning                                   |
|--------|---------|----------|-------------------------------------------|
| 0      | u16     | type     | 0 = free, 1 = file, 2 = directory         |
| 2      | u16     | mode     | permission bits, `0..0777`                |
| 4      | u32     | size     | length in bytes                           |
| 8      | u32     | uid      | owning user id (0 = root)                 |
| 12     | u32     | gid      | owning group id                           |
| 16     | u32     | mtime    | last modification, Unix seconds, 0 = unknown |
| 20     | u32[10] | direct   | direct data block numbers (0 = none)      |
| 60     | u32     | indirect | block holding 1024 u32 block numbers      |

The size arithmetic: `2 + 2 + 4 + 4 + 4 + 4 + 40 + 4 = 64`.

`mode` holds **permission bits only** — the type has its own field and
KestrelOS has no setuid bit, so anything outside `0777` is corruption.
A free inode slot (`type == 0`) is entirely zero.

File block `n` maps to `direct[n]` for `n < 10`, else to entry `n - 10`
of the indirect block. Maximum file size is `(10 + 1024) * 4096` =
**4,235,264 bytes** (v1 allowed `(12 + 1024) * 4096` = 4,243,456, so v2
costs 8 KiB off the top end). Sparse files are **not** supported: every
block below `size` is allocated, and all pointers past `size` are zero.

## Directories

Directory data is an array of 64-byte entries:

| offset | type     | field | meaning                    |
|--------|----------|-------|-----------------------------|
| 0      | u32      | ino   | target inode, 0 = free slot |
| 4      | char[60] | name  | NUL-terminated, max 59 chars |

Every directory contains `.` (itself) and `..` (its parent; the root's
`..` points to the root). Unlinking clears an entry's 64 bytes to zero;
creation reuses the first free slot before growing the directory. A
directory's `size` is always a multiple of 64.

## Permissions

KFS itself enforces nothing; it only stores `mode`, `uid` and `gid`. All
access control lives in `kernel/vfs.c`, which compares the calling task's
`current->uid` / `current->gid` against the inode:

* **uid 0 is root** and bypasses every check. Kernel threads run as root.
* Otherwise the owner triad (`mode >> 6`) applies if `uid` matches, else
  the group triad (`mode >> 3`) if `gid` matches, else the other triad.
* **Search (x) permission is required on every directory component** of a
  path. `vfs.c` walks paths one name at a time (`kfs_lookup_in()`) rather
  than resolving the whole string in KFS, precisely so it can check this.
* `vfs_open()` requires read permission for `O_RDONLY`, and write
  permission for `O_WRONLY`, `O_RDWR`, `O_CREAT` and `O_TRUNC`. Creating a
  new name additionally requires write + search on the parent directory.
* `vfs_unlink()` and `vfs_mkdir()` require write + search on the parent.
* `vfs_readdir()` requires read permission on the directory.
* `vfs_chmod()` requires being the owner, or root. `vfs_chown()` is
  root-only.
* `vfs_exec_ok()` reports whether the caller may execute a path; the ELF
  loader consults it before reading an image.
* New files are created `0644`, new directories `0755`, both owned by
  `current->uid` / `current->gid`.

There is no errno: every denial is reported as the same `-1` / `NULL` as
"not found".

`mtime` is stamped by the VFS from `vfs_now()`, which converts the CMOS
RTC (assumed to be UTC) to Unix seconds and caches the result against the
timer tick, re-reading the chip at most once a minute. The same value is
available to the rest of the kernel as `rtc_unix_time()` (declared in
`kernel/include/vfs.h`, implemented in `kernel/vfs.c`).

## Locking

`kernel/kfs.c` keeps its scratch blocks, the in-memory superblock and the
singleton ATA controller as shared mutable state, and syscalls are
preemptible. Every public `kfs_*` entry point therefore takes a
whole-filesystem mutex (a flag plus an owner, so it is recursive, with
waiters backing off via `task_sleep_ticks(1)`); everything below the entry
points assumes the lock is held. The lock may only be held across polled
disk I/O — never across the clock, the keyboard or the network — which is
why the mutating entry points take an `mtime` argument instead of reading
the RTC themselves.

## Pipes

`kernel/pipe.c` implements anonymous pipes on top of the same
`struct file` the VFS hands out; `struct file` carries a `type` tag
(`FILE_KFS` / `FILE_PIPE`) and `vfs_read()` / `vfs_write()` dispatch on
it. A pipe is a 4 KiB ring buffer with a reader count and a writer count:

* reading blocks while the buffer is empty and a writer still exists, and
  returns 0 (EOF) once the last writer closes;
* writing blocks while the buffer is full and a reader exists, and fails
  with -1 once every reader is gone (KestrelOS has no signals, so there is
  no `SIGPIPE`);
* `vfs_close()` drops the right side and frees the buffer when both ends
  are gone.

Pipes are not seekable and have no inode.

## Invariants (checked by `tools/kfsck.py`)

- magic is `"KFS2"` (a `"KFS1"` image is reported as such), geometry
  fields and `root_ino == 1` are consistent;
- the bitmap marks exactly the metadata blocks plus every block
  referenced by a live inode; `free_blocks` matches;
- no block is referenced twice; all references are in
  `[data_start, total_blocks)`;
- block pointers agree with `size` (non-sparse);
- `mode` has no bits outside `0777`, `uid`/`gid` are <= 65535, `mtime` is
  a plausible Unix time, and free inode slots are fully zeroed;
- every directory has exactly one `.` and `..`, pointing at itself and
  its parent; names are NUL-terminated, within 59 bytes, and unique;
- every allocated inode is reachable from the root exactly once
  (hard links are not supported).

There are no checksums anywhere in KFS, so bit-rot *inside file data* is
undetectable by design; `kfsck` finds structural damage only.

## Tools

- `tools/mkfs.py [options] <rootdir> <out.img> <size_mb>` — build an image
  from a host directory tree. Deterministic: names sorted, inodes/blocks
  assigned in preorder, and no field taken from the host clock or the host
  file mode. Defaults: everything `root:root`, directories `0755`, files
  `0644`, and anything under the top-level `bin/` (at any depth) `0755`,
  since those are the programs the shell execs.

  | option | effect |
  |--------|--------|
  | `--mtime SECS` | timestamp stamped on every inode (default 0 = unknown). Pass a fixed value to keep builds reproducible. |
  | `--mode PATH:MODE` | override one image path's permission bits, octal, e.g. `--mode /etc/shadow:0600`. Repeatable. |
  | `--own PATH:UID[:GID]` | override one image path's owner, e.g. `--own /home/ana:1000:1000`. `GID` defaults to `UID`. Repeatable. |

  `PATH` is an image-absolute path (`/etc/shadow`, `/home/ana`). Overrides
  are applied after the defaults and win. An override that matches nothing
  is reported as a warning, so typos do not pass silently.

- `tools/kfsck.py [-l] <image>` — validate an image; `-l` lists the tree
  with mode, owner, size and mtime. Exits nonzero on corruption.
