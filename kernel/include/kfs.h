#pragma once

#include <stdint.h>

/* KFS: the native KestrelOS filesystem. On-disk format (see docs/kfs.md):
 *
 *   Block size 4096 bytes = 8 disk sectors. The FS partition starts at
 *   disk LBA FS_START_LBA (ata.h); all block numbers are FS-relative.
 *
 *   block 0                superblock
 *   bitmap_start ..        block bitmap, 1 bit per FS block, set = used
 *   inode_start ..         inode table, 64 inodes per block, ino is 1-based
 *   data_start ..          data blocks
 */

#define KFS_MAGIC             0x3153464B  /* "KFS1" little-endian */
#define KFS_BLOCK_SIZE        4096
#define KFS_SECTORS_PER_BLOCK 8
#define KFS_NDIRECT           12
#define KFS_NINDIRECT         (KFS_BLOCK_SIZE / 4)          /* 1024 */
#define KFS_MAX_FILE_BLOCKS   (KFS_NDIRECT + KFS_NINDIRECT) /* 1036 */
#define KFS_MAX_FILE_SIZE     ((uint32_t)KFS_MAX_FILE_BLOCKS * KFS_BLOCK_SIZE)
#define KFS_NAME_MAX          59          /* + NUL fits in dirent name[60] */
#define KFS_ROOT_INO          1
#define KFS_INODES_PER_BLOCK  (KFS_BLOCK_SIZE / 64)         /* 64 */
#define KFS_DIRENTS_PER_BLOCK (KFS_BLOCK_SIZE / 64)         /* 64 */

/* inode types */
#define KFS_TYPE_FREE 0
#define KFS_TYPE_FILE 1
#define KFS_TYPE_DIR  2

/* Block 0, first 40 bytes; the rest of the block is zero. */
struct kfs_superblock {
    uint32_t magic;         /* KFS_MAGIC */
    uint32_t total_blocks;  /* size of the whole FS in blocks */
    uint32_t bitmap_start;  /* first bitmap block (= 1) */
    uint32_t bitmap_blocks;
    uint32_t inode_start;
    uint32_t inode_blocks;
    uint32_t inode_count;   /* number of inode slots (default 1024) */
    uint32_t data_start;    /* first data block */
    uint32_t root_ino;      /* = 1 */
    uint32_t free_blocks;
};

/* 64 bytes on disk. Inode numbers are 1-based indices into the table. */
struct kfs_inode {
    uint16_t type;          /* KFS_TYPE_* */
    uint16_t nlink;
    uint32_t size;          /* bytes */
    uint32_t direct[KFS_NDIRECT];
    uint32_t indirect;      /* block full of u32 block numbers, 0 = none */
    uint32_t pad;
};

/* 64 bytes; directory data is an array of these. ino 0 = free slot. */
struct kfs_dirent {
    uint32_t ino;
    char name[60];          /* NUL-terminated */
};

/* Mount / verify the superblock. Returns 0, or -1 if no valid KFS. */
int kfs_mount(void);

/* Resolve an absolute path to an inode number, or -1. */
int kfs_lookup(const char *path);

/* Copy inode `ino` into *out. Returns 0 or -1. */
int kfs_iget(uint32_t ino, struct kfs_inode *out);

/* File (or raw directory) data I/O at a byte offset.
 * Read returns bytes read (0 at/after EOF); write grows the file,
 * allocating blocks as needed. Both return -1 on error. */
long kfs_read(uint32_t ino, uint32_t off, void *buf, uint32_t n);
long kfs_write(uint32_t ino, uint32_t off, const void *buf, uint32_t n);

/* Create a regular file at `path` (parent must exist). Returns ino or -1.
 * Fails if the name already exists. */
int kfs_create(const char *path);

/* Create a directory (with "." and "..") at `path`. Returns 0 or -1. */
int kfs_mkdir(const char *path);

/* Remove a file, or an empty directory. Returns 0 or -1. */
int kfs_unlink(const char *path);

/* Fetch the index'th valid entry (free slots skipped) of directory `ino`
 * into *out. Returns 0, or -1 past the end / on error. */
int kfs_readdir(uint32_t ino, int index, struct kfs_dirent *out);

/* Free all data blocks of `ino` and set size to 0. Returns 0 or -1. */
int kfs_truncate(uint32_t ino);
