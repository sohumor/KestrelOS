#include "kernel.h"
#include "initcall.h"
#include "kheap.h"
#include "mount.h"
#include "proc.h"
#include "string.h"
#include "timer.h"
#include "vfs.h"

static unsigned generations[MOUNT_MAX];
static unsigned mounted;
static unsigned retired;
static int start_child, child_result;

static int probe_mount(struct blockdev *bd, void **out)
{
    (void)bd;
    if (mounted >= MOUNT_MAX)
        return -1;
    generations[mounted] = mounted + 1;
    *out = &generations[mounted++];
    return 0;
}

static void probe_unmount(void *priv)
{
    if (priv == &generations[0])
        __atomic_add_fetch(&retired, 1, __ATOMIC_RELEASE);
}

static int probe_stat(struct mount *m, const char *path, struct k_stat *st)
{
    if (strcmp(path, "/value") != 0)
        return -1;
    st->size = *(unsigned *)m->priv;
    return 0;
}

static struct file *probe_open(struct mount *m, const char *path, int flags,
                               struct open_guard *guard)
{
    (void)guard;
    struct k_stat st;
    if (probe_stat(m, path, &st) < 0)
        return NULL;
    struct file *f = kzalloc(sizeof(*f));
    if (f) {
        f->inum = st.size;
        f->flags = flags;
        f->refs = 1;
    }
    return f;
}

static long probe_read(struct file *f, void *buf, unsigned long len, int blocking)
{
    (void)blocking;
    if (!len)
        return 0;
    *(char *)buf = '0' + f->inum;
    return 1;
}

static struct fs_ops probe_ops = {
    .open = probe_open, .read = probe_read, .stat = probe_stat,
};

static struct fs_type probe_type = {
    .name = "viewprobe", .mount = probe_mount, .unmount = probe_unmount,
    .ops = &probe_ops,
};

static bool lists_only_scope(void)
{
    for (int i = 0; i <= 2; i++) {
        struct mount *m = mount_list_acquire(i);
        bool ok = i == 2 ? !m :
                  m && strcmp(m->path, i == 0 ? "/" : "/scope") == 0;
        mount_release(m);
        if (!ok)
            return false;
    }
    return true;
}

static void scoped_child(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&start_child, __ATOMIC_ACQUIRE))
        task_sleep_ticks(1);

    struct k_stat st;
    const char *hidden[] = {"/other"};
    struct mount_view *wider = mount_view_create(hidden, 1);
    bool ok = !wider && vfs_stat("/scope/value", &st) == 0 && st.size == 1 &&
              vfs_stat("/other/value", &st) < 0 && lists_only_scope();
    mount_view_release(wider);
    __atomic_store_n(&child_result, ok ? 1 : -1, __ATOMIC_RELEASE);
}

static bool check_views(void)
{
    const char *paths[] = {"/", "/scope"};
    const char *duplicate[] = {"/scope", "/scope/"};
    if (fs_register(&probe_type) < 0 ||
        mount_add("/scope", "viewprobe", NULL) < 0 ||
        mount_add("/other", "viewprobe", NULL) < 0)
        return false;

    struct mount_view *invalid = mount_view_create(duplicate, 2);
    if (invalid) {
        mount_view_release(invalid);
        return false;
    }
    struct mount_view *view = mount_view_create(paths, 2);
    if (!view)
        return false;
    mount_view_set_current(view);
    mount_view_release(view);
    struct file *held = vfs_open("/scope/value", O_RDONLY);
    bool spawned = held && kthread_create(scoped_child, NULL, "view-child");
    mount_view_set_current(NULL);

    struct k_stat st;
    bool ok = spawned && mount_remove("/scope") == 0 &&
              vfs_stat("/scope/value", &st) < 0 &&
              mount_add("/scope", "viewprobe", NULL) == 0 &&
              vfs_stat("/scope/value", &st) == 0 && st.size == 3 &&
              vfs_stat("/other/value", &st) == 0 && st.size == 2;
    __atomic_store_n(&start_child, 1, __ATOMIC_RELEASE);

    uint64_t deadline = timer_ticks() + 500;
    while (spawned && !__atomic_load_n(&child_result, __ATOMIC_ACQUIRE) &&
           timer_ticks() < deadline)
        task_sleep_ticks(1);

    char ch = 0;
    ok = ok && __atomic_load_n(&child_result, __ATOMIC_ACQUIRE) == 1 &&
         __atomic_load_n(&retired, __ATOMIC_ACQUIRE) == 0 &&
         vfs_read(held, &ch, 1) == 1 && ch == '1';
    vfs_close(held);
    while (ok && !__atomic_load_n(&retired, __ATOMIC_ACQUIRE) &&
           timer_ticks() < deadline)
        task_sleep_ticks(1);

    return ok && __atomic_load_n(&retired, __ATOMIC_ACQUIRE) == 1 &&
           mount_remove("/scope") == 0 && mount_remove("/other") == 0;
}

static int mount_views_probe(void)
{
    if (check_views())
        kprintf("PASS mount views: inherited selection, detached backing, replacement isolation\n");
    else
        kprintf("FAIL mount views: selection or lifetime check failed\n");
    return 0;
}

late_initcall(mount_views_probe);
