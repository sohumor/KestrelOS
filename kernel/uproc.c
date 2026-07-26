#include "kernel.h"
#include "proc.h"
#include "uproc.h"
#include "elf.h"
#include "vfs.h"
#include "vmm.h"
#include "pmm.h"
#include "kheap.h"
#include "string.h"
#include "kestrel_abi.h"

extern void enter_usermode(uint64_t entry, uint64_t user_rsp,
                           uint64_t argc, uint64_t argv)
    __attribute__((noreturn));

/* Everything the new task needs, copied so the spawner keeps ownership
 * of whatever it passed in. Freed by the thunk. */
struct uproc_pkg {
    char path[UPROC_PATH_MAX];
    int argc;
    char argv[UPROC_MAX_ARGS][UPROC_ARG_MAX];
};

/* --- exit-code ring for waitpid ------------------------------------- */

#define EXIT_RING 64

static struct {
    int pid;
    long code;
} exit_ring[EXIT_RING];
static int exit_head;

void uproc_record_exit(int pid, long code)
{
    uint64_t f = irq_save();
    exit_ring[exit_head].pid = pid;
    exit_ring[exit_head].code = code;
    exit_head = (exit_head + 1) % EXIT_RING;
    irq_restore(f);
}

static int exit_lookup(int pid, long *code)
{
    uint64_t f = irq_save();
    for (int i = 0; i < EXIT_RING; i++) {
        if (exit_ring[i].pid == pid && pid > 0) {
            *code = exit_ring[i].code;
            irq_restore(f);
            return 1;
        }
    }
    irq_restore(f);
    return 0;
}

long uproc_waitpid(int pid)
{
    if (pid <= 0)
        return -1;
    for (;;) {
        long code;
        if (exit_lookup(pid, &code))
            return code;
        if (!task_find(pid))
            return -1;             /* never existed, or record overwritten */
        task_sleep_ticks(1);
    }
}

/* --- spawning -------------------------------------------------------- */

/* Read the whole file at `path` into a kmalloc buffer. Returns NULL on
 * any failure; *size_out gets the file size. */
static uint8_t *slurp_file(const char *path, uint32_t *size_out)
{
    struct k_stat st;
    struct file *f;
    uint8_t *buf;
    uint32_t got = 0;

    if (vfs_stat(path, &st) < 0 || st.is_dir || st.size == 0)
        return NULL;
    buf = kmalloc(st.size);
    if (!buf)
        return NULL;
    f = vfs_open(path, O_RDONLY);
    if (!f) {
        kfree(buf);
        return NULL;
    }
    while (got < st.size) {
        long n = vfs_read(f, buf + got, st.size - got);
        if (n <= 0)
            break;
        got += (uint32_t)n;
    }
    vfs_close(f);
    if (got != st.size) {
        kfree(buf);
        return NULL;
    }
    *size_out = st.size;
    return buf;
}

/* Copy argv strings and the pointer array onto the freshly mapped user
 * stack. Returns the final 16-byte-aligned rsp; *argv_out points at the
 * user-side pointer array. Runs after vmm_switch to the process pml4. */
static uint64_t build_user_stack(const struct uproc_pkg *pkg,
                                 uint64_t *argv_out)
{
    uint64_t sp = USER_STACK_TOP;
    uint64_t uptr[UPROC_MAX_ARGS];
    int argc = pkg->argc;

    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(pkg->argv[i]) + 1;
        sp -= len;
        memcpy((void *)sp, pkg->argv[i], len);
        uptr[i] = sp;
    }
    sp &= ~0xFULL;
    sp -= (uint64_t)(argc + 1) * 8;
    sp &= ~0xFULL;                       /* keep rsp 16-byte aligned */

    uint64_t *uargv = (uint64_t *)sp;
    for (int i = 0; i < argc; i++)
        uargv[i] = uptr[i];
    uargv[argc] = 0;

    *argv_out = sp;
    return sp;
}

/* Runs as the new task, still in kernel mode on its kernel stack. */
static void user_task_thunk(void *arg)
{
    struct uproc_pkg *pkg = arg;
    uint64_t *pml4 = NULL;
    uint8_t *buf;
    uint32_t fsize = 0;
    uint64_t entry, brk, argv_user, rsp, f;
    int argc;

    buf = slurp_file(pkg->path, &fsize);
    if (!buf) {
        kprintf("uproc: cannot read %s\n", pkg->path);
        goto fail;
    }

    pml4 = vmm_new_pml4();
    if (elf_load(pml4, buf, fsize, &entry, &brk) < 0) {
        kprintf("uproc: %s is not a valid ELF64 executable\n", pkg->path);
        goto fail;
    }

    for (int i = 0; i < USER_STACK_PAGES; i++)
        vmm_map_page(pml4, USER_STACK_TOP - (uint64_t)(i + 1) * PAGE_SIZE,
                     pmm_alloc(), PTE_U | PTE_W);

    /* Adopt the new address space. From here on the process page tables
     * are live, so the user stack is directly addressable. */
    f = irq_save();
    current->pml4 = pml4;
    current->user = true;
    current->user_brk = brk;
    vmm_switch(pml4);
    irq_restore(f);

    rsp = build_user_stack(pkg, &argv_user);
    argc = pkg->argc;

    kfree(pkg);
    kfree(buf);
    enter_usermode(entry, rsp, (uint64_t)argc, argv_user);

fail:
    /* Nothing was installed on `current` yet: still on the kernel pml4. */
    if (pml4)
        vmm_destroy_user(pml4);
    if (buf)
        kfree(buf);
    kfree(pkg);
    uproc_record_exit(current->pid, -1);
    task_exit(-1);
}

int uproc_spawn(const char *path, char *const argv[], int argc)
{
    struct uproc_pkg *pkg;
    struct task *t;
    const char *name;

    if (!path || !path[0] || argc < 0)
        return -1;
    if (argc > UPROC_MAX_ARGS)
        argc = UPROC_MAX_ARGS;

    pkg = kzalloc(sizeof(*pkg));
    if (!pkg)
        return -1;
    strncpy(pkg->path, path, UPROC_PATH_MAX - 1);
    pkg->argc = argc;
    for (int i = 0; i < argc; i++)
        if (argv && argv[i])
            strncpy(pkg->argv[i], argv[i], UPROC_ARG_MAX - 1);

    name = strrchr(path, '/');
    name = name ? name + 1 : path;

    t = kthread_create(user_task_thunk, pkg, name);
    if (!t) {
        kfree(pkg);
        return -1;
    }
    return t->pid;
}

int uproc_spawn_from_user(const char *upath, char *const *uargv)
{
    char path[UPROC_PATH_MAX];
    char args[UPROC_MAX_ARGS][UPROC_ARG_MAX];
    char *kargv[UPROC_MAX_ARGS];
    int argc = 0;

    if (copy_str_from_user(path, upath, sizeof(path)) < 0)
        return -1;

    if (uargv) {
        while (argc < UPROC_MAX_ARGS) {
            uint64_t p;
            if (copy_from_user(&p, (const uint8_t *)uargv + argc * 8, 8) < 0)
                return -1;
            if (!p)
                break;
            if (copy_str_from_user(args[argc], (const void *)p,
                                   UPROC_ARG_MAX) < 0)
                return -1;
            kargv[argc] = args[argc];
            argc++;
        }
    }
    return uproc_spawn(path, argc ? kargv : NULL, argc);
}
