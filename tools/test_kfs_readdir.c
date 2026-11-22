#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <assert.h>
#include "driver.c"

static unsigned char disk[128][KFS_BLOCK_SIZE];
static int fail_reads;
bool sched_active;
void flight_record(uint8_t cat, const char *event, int16_t pid,
                   uint64_t arg0, uint64_t arg1, const char *detail)
{
    (void)cat; (void)event; (void)pid;
    (void)arg0; (void)arg1; (void)detail;
}
struct task *smp_current_task(void) { return NULL; }
void task_sleep_ticks(uint64_t ticks) { (void)ticks; abort(); }
int blockdev_read(struct blockdev *bd, uint64_t lba, uint32_t n, void *buf)
{
    (void)bd;
    if (fail_reads || lba + n > 128) return -1;
    memcpy(buf, disk[lba], n * KFS_BLOCK_SIZE);
    return 0;
}
int blockdev_write(struct blockdev *bd, uint64_t lba, uint32_t n, const void *buf)
{
    (void)bd;
    if (lba + n > 128) return -1;
    memcpy(disk[lba], buf, n * KFS_BLOCK_SIZE);
    return 0;
}

/* Stubs for kernel deps kfs.c grew after this harness was written (VFS
 * access policy, fs registration, heap).  The readdir path under test only
 * touches the RAM disk; PE linkers still need every symbol resolved. */
int fs_register(struct fs_type *t) { (void)t; return 0; }
void kprintf(const char *fmt, ...) { (void)fmt; }
uint32_t vfs_uid(void) { return 0; }          /* root: everything allowed */
uint32_t vfs_gid(void) { return 0; }
int vfs_perm_ok(uint32_t mode, uint32_t uid, uint32_t gid, int want)
{
    (void)mode; (void)uid; (void)gid; (void)want; return 1;
}
int vfs_flags_allow_read(int flags) { (void)flags; return 1; }
int vfs_flags_allow_write(int flags) { (void)flags; return 1; }
uint32_t vfs_now(void) { return 1700000000; }
void *kzalloc(size_t size) { return calloc(1, size); }
void kfree(void *ptr) { free(ptr); }

static void setup(struct kfs_fs *fs, int entries)
{
    bcache_invalidate(NULL);
    memset(disk, 0, sizeof(disk));
    memset(fs, 0, sizeof(*fs));
    fs->mounted = 1;
    fs->spb = 1;
    fs->sb.inode_start = 1;
    fs->sb.inode_count = 4096;
    fs->sb.journal_start = 90;
    fs->sb.journal_blocks = KFS_JOURNAL_BLOCKS;
    struct kfs_inode *ip = (void *)disk[1];
    ip->type = KFS_TYPE_DIR;
    /* A free slot before each entry exercises ordinal vs physical offsets. */
    ip->size = entries * 2 * sizeof(struct kfs_dirent);
    for (unsigned i = 0; i < (ip->size + 4095) / 4096; i++) {
        if (i < KFS_NDIRECT) ip->direct[i] = 3 + i;
        else {
            ip->indirect = 2;
            ((uint32_t *)disk[2])[i - KFS_NDIRECT] = 3 + i;
        }
    }
    for (int i = 0; i < entries; i++) {
        unsigned off = (2 * i + 1) * sizeof(struct kfs_dirent);
        struct kfs_dirent *de = (void *)(disk[3 + off / 4096] + off % 4096);
        de->ino = i + 2;
        snprintf(de->name, sizeof(de->name), "entry%d", i);
    }
}

static void test_dir_scan(void)
{
    struct kfs_fs fs;
    uint32_t slot = 123;
    setup(&fs, 2000);
    assert(dir_lookup(&fs, 1, "entry1999", &slot) == 2001);
    assert(slot == 3999 * sizeof(struct kfs_dirent));
    assert(dir_lookup(&fs, 1, "missing", NULL) == 0);
    assert(journal_begin(&fs) == 0);
    assert(dir_add(&fs, 1, "new", 42, 1) == 0);
    assert(dir_lookup(&fs, 1, "new", &slot) == 42 && slot == 0);
    assert(dir_add(&fs, 1, "next", 43, 1) == 0);
    assert(dir_lookup(&fs, 1, "next", &slot) == 43 && slot == 128);
    journal_abort();
    assert(dir_lookup(&fs, 1, "new", NULL) == 0);
    setup(&fs, 2000);
    for (unsigned i = 0; i < 3998; i += 2) {
        unsigned off = i * sizeof(struct kfs_dirent);
        struct kfs_dirent *de = (void *)(disk[3 + off / 4096] + off % 4096);
        de->ino = 2;
        strcpy(de->name, "occupied");
    }
    assert(journal_begin(&fs) == 0);
    assert(dir_add(&fs, 1, "late", 44, 1) == 0);
    assert(dir_lookup(&fs, 1, "late", &slot) == 44 && slot == 3998 * 64);
    journal_abort();
    setup(&fs, 0);
    assert(dir_lookup(&fs, 1, "missing", NULL) == 0);
    ((struct kfs_inode *)disk[1])->direct[0] = 3;
    bcache_invalidate(&fs);
    assert(journal_begin(&fs) == 0);
    assert(dir_add(&fs, 1, "first", 42, 1) == 0);
    assert(dir_add(&fs, 1, "second", 43, 1) == 0);
    assert(journal_commit(&fs) == 0);
    assert(dir_lookup(&fs, 1, "second", &slot) == 43 && slot == 64);
    setup(&fs, 3);
    fail_reads = 1;
    assert(dir_lookup(&fs, 1, "entry0", NULL) == 0);
    assert(dir_add(&fs, 1, "new", 42, 1) == -1);
    fail_reads = 0;
    struct kfs_inode *ip = (void *)disk[1];
    ip->direct[0] = 0;
    assert(dir_lookup(&fs, 1, "entry0", NULL) == 0);
    assert(dir_add(&fs, 1, "new", 42, 1) == -1);
    puts("PASS KFS directory scan: indirect lookup, miss, holes, pending writes, abort, append, empty, errors");
}

int main(int argc, char **argv)
{
    int baseline = argc == 2 && strcmp(argv[1], "--baseline") == 0;
    struct kfs_fs fs;
    struct kfs_dirent de;
    const int sizes[] = {50, 500, 2000};
    for (unsigned s = 0; s < 3; s++) {
        int n = sizes[s];
        setup(&fs, n);
        dirent_reads = 0;
        clock_t start = clock();
        for (int i = 0; i < n; i++) {
            assert(kfs_readdir(&fs, 1, i, &de) == 0);
            assert(de.ino == (unsigned)i + 2);
            char name[60];
            snprintf(name, sizeof(name), "entry%d", i);
            assert(strcmp(de.name, name) == 0);
        }
        assert(kfs_readdir(&fs, 1, n, &de) == -1);
        double ms = 1000.0 * (clock() - start) / CLOCKS_PER_SEC;
        printf("entries=%d ms=%.3f dirent_scans=%lu\n", n, ms, dirent_reads);
        fflush(stdout);
        if (!baseline) assert(dirent_reads == (unsigned)n * 2);
        assert(kfs_readdir(&fs, 1, 0, &de) == 0 && de.ino == 2);
        assert(kfs_readdir(&fs, 1, n - 1, &de) == 0 && de.ino == (unsigned)n + 1);
        assert(kfs_readdir(&fs, 1, -1, &de) == -1);
        assert(kfs_readdir(&fs, 1, 0, NULL) == -1);
    }
    setup(&fs, 3);
    assert(kfs_readdir(&fs, 1, 0, &de) == 0);
    /* Same-size deletion with unchanged mtime must invalidate the cursor. */
    assert(journal_begin(&fs) == 0);
    struct kfs_dirent empty = {0};
    assert(writei(&fs, 1, 64, &empty, sizeof(empty), 0) == sizeof(empty));
    assert(kfs_readdir(&fs, 1, 1, &de) == 0 && de.ino == 4);
    journal_abort();
    assert(kfs_readdir(&fs, 1, 1, &de) == 0 && de.ino == 3);
    assert(kfs_readdir(&fs, 1, 0, &de) == 0);
    assert(journal_begin(&fs) == 0);
    assert(writei(&fs, 1, 64, &empty, sizeof(empty), 0) == sizeof(empty));
    assert(journal_commit(&fs) == 0);
    assert(kfs_readdir(&fs, 1, 1, &de) == 0 && de.ino == 4);
    de.ino = 2;
    strcpy(de.name, "replacement");
    assert(journal_begin(&fs) == 0);
    assert(writei(&fs, 1, 64, &de, sizeof(de), 0) == sizeof(de));
    assert(journal_commit(&fs) == 0);
    assert(kfs_readdir(&fs, 1, 1, &de) == 0 && de.ino == 3);
    assert(kfs_readdir(&fs, 1, 0, &de) == 0);
    assert(writei(&fs, 1, 64, &empty, sizeof(empty), 0) == sizeof(empty));
    assert(kfs_readdir(&fs, 1, 1, &de) == 0 && de.ino == 4);
    setup(&fs, 0);
    assert(kfs_readdir(&fs, 1, 0, &de) == -1);
    setup(&fs, 3);
    fail_reads = 1;
    assert(kfs_readdir(&fs, 1, 0, &de) == -1);
    fail_reads = 0;
    assert(kfs_readdir(&fs, 1, 0, &de) == 0 && de.ino == 2);
    puts("PASS KFS readdir: counts, names, holes, rewind, mutation/abort, empty, errors");
    test_dir_scan();
    return 0;
}
