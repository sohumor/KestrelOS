# KFS — the KestrelOS filesystem

KFS is a deliberately small fixed-layout filesystem: one superblock, one
block bitmap, a fixed inode table, then data blocks. All metadata updates
are write-through; there is no journal and no cache to flush.

All multi-byte fields are little-endian.

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
| 0      | u32  | magic         | `0x3153464B` ("KFS1")                    |
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
| 2      | u16     | nlink    | link count (informational: files 1, dirs 2) |
| 4      | u32     | size     | length in bytes                           |
| 8      | u32[12] | direct   | direct data block numbers (0 = none)      |
| 56     | u32     | indirect | block holding 1024 u32 block numbers      |
| 60     | u32     | pad      | zero                                      |

File block `n` maps to `direct[n]` for `n < 12`, else to entry `n - 12`
of the indirect block. Maximum file size is `(12 + 1024) * 4096` =
4,243,456 bytes. Sparse files are **not** supported: every block below
`size` is allocated, and all pointers past `size` are zero.

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

## Invariants (checked by `tools/kfsck.py`)

- magic, geometry fields, and `root_ino == 1` are consistent;
- the bitmap marks exactly the metadata blocks plus every block
  referenced by a live inode; `free_blocks` matches;
- no block is referenced twice; all references are in
  `[data_start, total_blocks)`;
- block pointers agree with `size` (non-sparse);
- every directory has exactly one `.` and `..`, pointing at itself and
  its parent; names are NUL-terminated and unique;
- every allocated inode is reachable from the root exactly once
  (hard links are not supported).

## Tools

- `tools/mkfs.py <rootdir> <out.img> <size_mb>` — build an image from a
  host directory tree. Deterministic: names sorted, inodes/blocks
  assigned in preorder.
- `tools/kfsck.py [-l] <image>` — validate an image; `-l` lists the tree
  with file sizes. Exits nonzero on corruption.
