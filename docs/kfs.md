# KFS — the KestrelOS filesystem

KFS is a deliberately small fixed-layout filesystem: one superblock, one
block bitmap, a fixed redo journal, a fixed inode table, then data blocks.
Regular file data is write-through; metadata updates commit atomically through
the journal and are replayed at mount after an interrupted write.

All multi-byte fields are little-endian.

**This document describes on-disk format version 3** (magic `"KFS3"`).
v3 reserves a checksummed metadata redo journal; the inode layout is unchanged
from v2. v2 added `mode`, `uid`, `gid` and `mtime` to the inode so KestrelOS
could grow multi-user support, and dropped `nlink` (KFS has never supported
hard links, so the field was never anything but decoration). The inode stayed
64 bytes by giving up two direct block pointers. The kernel recognises v1
(`"KFS1"`) and v2 (`"KFS2"`) by magic but refuses both: their geometry has no
journal, and a v1 inode's `nlink`/`direct[10..11]` sit exactly where newer
formats keep `mode` and `indirect`. Rebuild older images with `tools/mkfs.py`;
there is no in-place format upgrader.

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
| `journal_start .. +journal_blocks-1` | journal header + 32 payload blocks |
| `inode_start .. +inode_blocks-1`   | inode table  |
| `data_start .. total_blocks-1`     | data blocks  |

With the default geometry (`mkfs.py`, 1024 inodes) `bitmap_start` is 1,
`journal_start` follows the bitmap, `inode_start` is
`journal_start + 33`, and `data_start` is `inode_start + 16`.

## Superblock (block 0)

First 52 bytes of block 0; the rest of the block is zero.

| offset | type | field         | meaning                                  |
|--------|------|---------------|------------------------------------------|
| 0      | u32  | magic         | `0x3353464B` ("KFS3")                    |
| 4      | u32  | total_blocks  | size of the FS in blocks                 |
| 8      | u32  | bitmap_start  | first bitmap block (= 1)                 |
| 12     | u32  | bitmap_blocks | bitmap length in blocks                  |
| 16     | u32  | journal_start | journal header block                     |
| 20     | u32  | journal_blocks| journal length (= 33)                    |
| 24     | u32  | inode_start   | first inode-table block                  |
| 28     | u32  | inode_blocks  | inode-table length in blocks (16)        |
| 32     | u32  | inode_count   | number of inode slots (1024)             |
| 36     | u32  | data_start    | first data block                         |
| 40     | u32  | root_ino      | root directory inode (= 1)               |
| 44     | u32  | free_blocks   | count of free blocks                     |
| 48     | u32  | features      | `1` = metadata redo journal              |

## Block bitmap

One bit per block of the whole FS, LSB-first within each byte: block `B`
is bit `B % 8` of byte `B / 8`. Set = used. Blocks `0 .. data_start-1`
(superblock, bitmap, journal, inode table) are pre-marked used at mkfs time.
Padding bits at or past `total_blocks` are always zero.

## Metadata journal

The journal is fixed at 33 blocks: one header and 32 full-block redo payloads.
A transaction coalesces repeated writes to the same metadata block, so the
default geometry needs only a handful of entries even when truncating the
largest possible file.

The committed header contains:

| field | meaning |
|---|---|
| magic | `0x314C4E4A` (`"JNL1"`) |
| state | `1` = committed |
| sequence | diagnostic transaction sequence |
| count | payload/target count, `1..32` |
| checksum | IEEE CRC32 over sequence, count, targets, then payload blocks |
| targets[32] | home block for each payload |

Commit ordering is:

1. write regular file data and every journal payload;
2. write the checksummed committed header;
3. copy every payload to its home block;
4. zero the journal header.

A reset before step 2 leaves no committed transaction. A reset during steps
3–4 leaves a valid header, so mount verifies the CRC and replays every home
block idempotently before exposing the filesystem. A torn header or payload is
cleared without replay because home writes never begin before a valid commit
record. Unsafe targets (outside the filesystem, duplicated, or inside the
journal itself) refuse the mount.

The journal covers structural metadata: the superblock, bitmap, inode table,
indirect pointer blocks, and directory contents. Regular file contents use
ordered-data semantics rather than data journaling: new data reaches disk
before the inode size which exposes it, but an in-place data overwrite can be
partially visible after a power loss.

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

`kernel/kfs.c` keeps its transaction buffer, scratch blocks, in-memory
superblock and the
singleton ATA controller as shared mutable state, and syscalls are
preemptible. Every public `kfs_*` entry point therefore takes a
whole-filesystem mutex (a flag plus an owner, so it is recursive, with
waiters backing off via `task_sleep_ticks(1)`); everything below the entry
points assumes the lock is held. The lock may only be held across polled
disk I/O — never across the clock, the keyboard or the network — which is
why the mutating entry points take an `mtime` argument instead of reading
the RTC themselves. Each public mutator opens one journal transaction while
holding that lock; failed operations discard staged metadata and restore the
in-memory superblock snapshot.

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

- magic is `"KFS3"` (v1/v2 images are reported and refused);
- geometry, journal placement, feature bits, and `root_ino == 1` are
  consistent;
- a committed journal has a valid CRC, unique safe targets, and at most 32
  entries;
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

Journal transactions are checksummed, but home metadata and file contents are
not. Bit-rot in an already committed home block or inside file data remains
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

- `tools/kfsck.py [--recover] [-l] <image>` — validate an image; `--recover`
  replays a committed journal or clears a torn/uncommitted record before
  checking, and `-l` lists the tree
  with mode, owner, size and mtime. Exits nonzero on corruption.
