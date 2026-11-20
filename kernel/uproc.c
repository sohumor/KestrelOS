#include "kernel.h"
#include "proc.h"
#include "uproc.h"
#include "elf.h"
#include "vfs.h"
#include "vmm.h"
#include "pmm.h"
#include "vm.h"
#include "kheap.h"
#include "string.h"
#include "kestrel_abi.h"
#include "spinlock.h"
#include "pulse.h"
#include "mission.h"

extern void enter_usermode(uint64_t entry, uint64_t user_rsp,
                           uint64_t argc, uint64_t argv)
    __attribute__((noreturn));

/* Everything the new task needs, copied so the spawner keeps ownership
 * of whatever it passed in. Freed by the thunk. io carries the child's
 * redirected stdin/stdout paths; empty strings mean "the console". */
struct uproc_pkg {
    char path[UPROC_PATH_MAX];
    int argc;
    char argv[UPROC_MAX_ARGS][UPROC_ARG_MAX];
    struct k_spawn_io io;
    int n_fds;
    int32_t fd_nums[K_SPAWN_CAPS_MAX_FDS];
    struct file *fds[K_SPAWN_CAPS_MAX_FDS];
    int has_cap_mask;
    uint64_t cap_mask;
    int mission_id;
};

/* copy_str_from_user's "no NUL within max" result; the destination still
 * holds a NUL-terminated prefix. Mirrors COPY_STR_TRUNC in syscall.c. */
#define UPROC_COPY_TRUNC (-2)

/* --- exit-code ring for waitpid ------------------------------------- */

#define EXIT_RING 128

static struct {
    int pid;
    int ppid;
    long code;
    int collected;
} exit_ring[EXIT_RING];
static int exit_head;
static spinlock_t exit_lock = SPINLOCK_INIT;

void uproc_record_exit(int pid, long code)
{
    int ppid = current ? current->parent_pid : 0;
    const char *name = current ? current->name : "";
    uint64_t f = spin_lock_irqsave(&exit_lock);
    exit_ring[exit_head].pid = pid;
    exit_ring[exit_head].ppid = ppid;
    exit_ring[exit_head].code = code;
    exit_ring[exit_head].collected = 0;
    exit_head = (exit_head + 1) % EXIT_RING;
    spin_unlock_irqrestore(&exit_lock, f);

    pulse_emit(K_TOPIC_PROC, K_PULSE_PROC_EXIT,
               (uint32_t)pid, (uint32_t)code, name);
}

static int exit_collect_pid(int pid, long *code)
{
    if (pid <= 0)
        return 0;
    uint64_t f = spin_lock_irqsave(&exit_lock);
    for (int i = 0; i < EXIT_RING; i++) {
        int idx = (exit_head - 1 - i + EXIT_RING) % EXIT_RING;
        if (exit_ring[idx].pid == pid && !exit_ring[idx].collected) {
            *code = exit_ring[idx].code;
            exit_ring[idx].collected = 1;
            spin_unlock_irqrestore(&exit_lock, f);
            return 1;
        }
    }
    spin_unlock_irqrestore(&exit_lock, f);
    return 0;
}

static int exit_collect_any(int ppid, int *pid_out, long *code)
{
    uint64_t f = spin_lock_irqsave(&exit_lock);
    for (int i = 0; i < EXIT_RING; i++) {
        int idx = (exit_head + i) % EXIT_RING;
        if (exit_ring[idx].pid > 0 &&
            exit_ring[idx].ppid == ppid &&
            !exit_ring[idx].collected) {
            *pid_out = exit_ring[idx].pid;
            *code = exit_ring[idx].code;
            exit_ring[idx].collected = 1;
            spin_unlock_irqrestore(&exit_lock, f);
            return 1;
        }
    }
    spin_unlock_irqrestore(&exit_lock, f);
    return 0;
}

long uproc_waitpid(int pid)
{
    if (pid <= 0)
        return -1;
    for (;;) {
        long code;
        int alive;

        if (exit_collect_pid(pid, &code))
            return code;
        alive = task_exists(pid);
        if (!alive) {
            if (exit_collect_pid(pid, &code))
                return code;
            return -1;             /* never existed, or record overwritten */
        }
        if (current && current->kill_pending)
            return -1;
        task_sleep_ticks(1);
    }
}

/* Wait for any child to exit. Returns its exit code and stores the pid;
 * returns -1 with *pid_out = 0 when the caller has no children left, which
 * is how a supervisor knows to stop waiting. */
long uproc_waitany(int *pid_out)
{
    *pid_out = 0;
    for (;;) {
        int found_pid = 0;
        long code = -1;

        if (exit_collect_any(current->pid, &found_pid, &code)) {
            *pid_out = found_pid;
            return code;
        }

        int child_pids[64];
        int children = task_child_pids(current->pid, child_pids,
                                       (int)(sizeof(child_pids) /
                                             sizeof(child_pids[0])));
        if (!children) {
            if (exit_collect_any(current->pid, &found_pid, &code)) {
                *pid_out = found_pid;
                return code;
            }
            return -1;
        }
        if (current && current->kill_pending)
            return -1;
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
    if (pmm_free_pages() <= 256 + ((uint64_t)st.size + PAGE_SIZE - 1) / PAGE_SIZE)
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

static void free_pkg(struct uproc_pkg *pkg)
{
    if (!pkg)
        return;
    for (int i = 0; i < pkg->n_fds; i++) {
        if (pkg->fds[i]) {
            vfs_close(pkg->fds[i]);
            pkg->fds[i] = NULL;
        }
    }
    kfree(pkg);
}

/* Runs as the new task, still in kernel mode on its kernel stack. */
static void user_task_thunk(void *arg)
{
    struct uproc_pkg *pkg = arg;
    uint64_t *pml4 = NULL;
    uint8_t *buf = NULL;
    struct file *backing = NULL;
    uint32_t fsize = 0;
    uint64_t entry, brk, argv_user, rsp, f;
    int argc;

    if (pmm_free_pages() <= 256) {
        kprintf("uproc: out of memory spawning %s\n", pkg->path);
        goto fail;
    }
    buf = slurp_file(pkg->path, &fsize);
    if (!buf) {
        kprintf("uproc: cannot read %s\n", pkg->path);
        goto fail;
    }
    if (pmm_free_pages() <= 256) {
        kprintf("uproc: out of memory for address space of %s\n", pkg->path);
        goto fail;
    }

    pml4 = vmm_new_pml4();
    backing = vfs_open(pkg->path, O_RDONLY);
    if (!backing) {
        kprintf("uproc: cannot keep %s open for demand paging\n", pkg->path);
        goto fail;
    }

    /* Dynamic relocation targets are populated through the same demand
     * fault path as normal execution, so make the new address space live
     * before asking the ELF loader to finish the image. */
    f = irq_save();
    current->pml4 = pml4;
    current->user = true;
    current->vm_files[0] = backing;
    current->vm_file_count = 1;
    vmm_switch(pml4);
    irq_restore(f);
    backing = NULL;                    /* task now owns the handle */

    if (elf_load(current, current->vm_files[0], buf, fsize,
                 &entry, &brk) < 0) {
        kprintf("uproc: %s is not a valid ELF64 executable\n", pkg->path);
        goto live_fail;
    }

    /* Redirected stdio (SYS_SPAWN_IO). A NULL files[0]/files[1] means
     * the console, so only a named path installs a file; fd 2 always
     * stays on the console. Installed on `current` before the address-
     * space switch: if either open fails, the fail path's task_exit
     * closes whatever was already installed. */
    if (pkg->io.in_path[0]) {
        current->files[0] = vfs_open(pkg->io.in_path, O_RDONLY);
        if (!current->files[0]) {
            kprintf("uproc: %s: cannot open %s for stdin\n",
                    pkg->path, pkg->io.in_path);
            goto live_fail;
        }
    }
    if (pkg->io.out_path[0]) {
        current->files[1] = vfs_open(pkg->io.out_path, O_WRONLY | O_CREAT |
                                     (pkg->io.out_append ? O_APPEND
                                                         : O_TRUNC));
        if (!current->files[1]) {
            kprintf("uproc: %s: cannot open %s for stdout\n",
                    pkg->path, pkg->io.out_path);
            goto live_fail;
        }
    }

    for (int i = 0; i < pkg->n_fds; i++) {
        int cfd = pkg->fd_nums[i];
        if (current->files[cfd])
            vfs_close(current->files[cfd]);
        current->files[cfd] = pkg->fds[i];
        pkg->fds[i] = NULL;
    }

    current->user_heap_start = brk;
    current->user_brk = brk;

    /* argv can occupy just over one page at the ABI limits. Populate only
     * the stack pages it needs; the remainder of the 16-page reservation
     * remains lazy. */
    size_t stack_need = (size_t)pkg->argc * (UPROC_ARG_MAX + sizeof(uint64_t))
                        + 64;
    if (vm_fault_in_range(current, USER_STACK_TOP - stack_need,
                          stack_need, 1) < 0) {
        kprintf("uproc: cannot allocate initial stack for %s\n", pkg->path);
        goto live_fail;
    }
    rsp = build_user_stack(pkg, &argv_user);
    argc = pkg->argc;

    if (pkg->has_cap_mask) {
        if (current->mission_id)
            __atomic_fetch_and(&current->cap_mask, pkg->cap_mask, __ATOMIC_SEQ_CST);
        else
            current->cap_mask = pkg->cap_mask;
        mission_limit_current();
        if (current->uid != 0)
            current->cap_mask &= ~KCAP_ADMIN;
    }

    free_pkg(pkg);
    kfree(buf);
    pulse_emit(K_TOPIC_PROC, K_PULSE_PROC_SPAWN,
               (uint32_t)current->pid, (uint32_t)current->parent_pid,
               current->name);
    enter_usermode(entry, rsp, (uint64_t)argc, argv_user);

live_fail:
    free_pkg(pkg);
    kfree(buf);
    uproc_record_exit(current->pid, -1);
    task_exit(-1);

fail:
    /* Still on the kernel pml4: the address-space switch never happened.
     * Any files[] entry the redirection setup installed is closed by
     * task_exit's descriptor sweep. */
    if (pml4)
        vmm_destroy_user(pml4);
    if (backing)
        vfs_close(backing);
    if (buf)
        kfree(buf);
    free_pkg(pkg);
    uproc_record_exit(current->pid, -1);
    task_exit(-1);
}

static int spawn_pkg(struct uproc_pkg *pkg)
{
    const char *name;
    struct task *t;
    int pid = -1;

    name = strrchr(pkg->path, '/');
    name = name ? name + 1 : pkg->path;

    /* The scheduler publishes the PID through pid_out before it releases
     * its cross-CPU run-queue lock. The child may complete immediately
     * after publication without leaving us to dereference a freed task. */
    t = kthread_create_mission(user_task_thunk, pkg, name, &pid, pkg->mission_id);
    if (!t) {
        free_pkg(pkg);
        return -1;
    }
    return pid;
}

static int pkg_set_io(struct uproc_pkg *pkg, const char *in_path,
                      const char *out_path, int append)
{
    /* A truncated redirection target would open the wrong file, so an
     * over-long path fails the spawn instead. */
    if (in_path && in_path[0]) {
        if (strlen(in_path) >= sizeof(pkg->io.in_path))
            return -1;
        strncpy(pkg->io.in_path, in_path, sizeof(pkg->io.in_path) - 1);
    }
    if (out_path && out_path[0]) {
        if (strlen(out_path) >= sizeof(pkg->io.out_path))
            return -1;
        strncpy(pkg->io.out_path, out_path, sizeof(pkg->io.out_path) - 1);
    }
    pkg->io.out_append = append ? 1 : 0;
    return 0;
}

int uproc_spawn_io(const char *path, char *const argv[], int argc,
                   const char *in_path, const char *out_path, int append)
{
    struct uproc_pkg *pkg;

    if (!path || !path[0] || argc < 0)
        return -1;
    if (pmm_free_pages() <= 256)
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

    if (pkg_set_io(pkg, in_path, out_path, append) < 0) {
        free_pkg(pkg);
        return -1;
    }
    return spawn_pkg(pkg);
}

int uproc_spawn(const char *path, char *const argv[], int argc)
{
    return uproc_spawn_io(path, argv, argc, NULL, NULL, 0);
}

/* Heap-allocated bounded copy-in of a spawn's path + argv from the current
 * process so ~4.5 KiB of argument buffers never sit on the kernel stack. */
static struct uproc_pkg *alloc_pkg_from_user(const char *upath,
                                             char *const *uargv)
{
    struct uproc_pkg *pkg;

    if (pmm_free_pages() <= 256)
        return NULL;
    pkg = kzalloc(sizeof(*pkg));
    if (!pkg)
        return NULL;

    if (copy_str_from_user(pkg->path, upath, UPROC_PATH_MAX) < 0 ||
        !pkg->path[0])
        goto fail;

    if (uargv) {
        while (pkg->argc < UPROC_MAX_ARGS) {
            uint64_t p;
            if (copy_from_user(&p,
                               (const uint8_t *)uargv + pkg->argc * 8, 8) < 0)
                goto fail;
            if (!p)
                break;
            /* An over-long argument is truncated, matching what the
             * in-kernel uproc_spawn() path does; only an unreadable
             * pointer fails the whole spawn. */
            long n = copy_str_from_user(pkg->argv[pkg->argc],
                                        (const void *)p, UPROC_ARG_MAX);
            if (n < 0 && n != UPROC_COPY_TRUNC)
                goto fail;
            pkg->argc++;
        }
    }
    return pkg;

fail:
    kfree(pkg);
    return NULL;
}

static int spawn_user_allowed(void)
{
    if (current && current->user && !(current->cap_mask & KCAP_SPAWN))
        return 0;
    return 1;
}

int uproc_spawn_from_user(const char *upath, char *const *uargv)
{
    struct uproc_pkg *pkg;

    if (!spawn_user_allowed())
        return -1;
    pkg = alloc_pkg_from_user(upath, uargv);
    if (!pkg)
        return -1;
    return spawn_pkg(pkg);
}

/* Replace the caller with a new program. There is no fork here, so this is
 * implemented as "spawn the replacement, then exit" — the pid changes,
 * which is why the shell uses spawn+wait and only login-style handoffs use
 * exec. Returns -1 if the new program could not be started; on success the
 * caller does not return. */
long uproc_exec_from_user(const char *upath, char *const *uargv)
{
    struct uproc_pkg *pkg;

    if (!spawn_user_allowed())
        return -1;
    pkg = alloc_pkg_from_user(upath, uargv);
    if (!pkg)
        return -1;

    int pid = spawn_pkg(pkg);
    if (pid < 0)
        return -1;

    long code = uproc_waitpid(pid);
    uproc_record_exit(current->pid, (int)code);
    task_exit((int)code);            /* noreturn */
}

int uproc_spawn_io_from_user(const char *upath, char *const *uargv,
                             const char *in_path, const char *out_path,
                             int append)
{
    struct uproc_pkg *pkg;

    if (!spawn_user_allowed())
        return -1;
    pkg = alloc_pkg_from_user(upath, uargv);
    if (!pkg)
        return -1;
    if (pkg_set_io(pkg, in_path, out_path, append) < 0) {
        free_pkg(pkg);
        return -1;
    }
    return spawn_pkg(pkg);
}

static int str_terminated_in(const char *s, size_t max)
{
    for (size_t i = 0; i < max; i++)
        if (s[i] == '\0')
            return 1;
    return 0;
}

int uproc_spawn_caps_from_user(const char *upath, char *const *uargv,
                               const struct k_spawn_caps *kcaps)
{
    struct uproc_pkg *pkg;

    if (!spawn_user_allowed())
        return -1;
    if (!upath || !kcaps || kcaps->n_fds > K_SPAWN_CAPS_MAX_FDS)
        return -1;
    if (!str_terminated_in(kcaps->cwd, sizeof(kcaps->cwd)))
        return -1;

    pkg = alloc_pkg_from_user(upath, uargv);
    if (!pkg)
        return -1;

    for (uint32_t i = 0; i < kcaps->n_fds; i++) {
        int32_t cfd = kcaps->fds[i].child_fd;
        int32_t pfd = kcaps->fds[i].parent_fd;
        if (cfd < 0 || cfd >= MAX_OPEN_FILES ||
            pfd < 0 || pfd >= MAX_OPEN_FILES)
            goto fail;

        struct file *pf = current->files[pfd];
        if (!pf) {
            if (pfd >= 3)
                goto fail;
            pkg->fd_nums[pkg->n_fds] = cfd;
            pkg->fds[pkg->n_fds] = NULL;
            pkg->n_fds++;
        } else {
            struct file *dup = vfs_file_dup(pf);
            if (!dup)
                goto fail;
            pkg->fd_nums[pkg->n_fds] = cfd;
            pkg->fds[pkg->n_fds] = dup;
            pkg->n_fds++;
        }
    }

    if (kcaps->cwd[0]) {
        int slot = -1;
        if (pkg->argc > 0 &&
            strncmp(pkg->argv[pkg->argc - 1], "--cwd=", 6) == 0) {
            slot = pkg->argc - 1;
        } else if (pkg->argc < UPROC_MAX_ARGS) {
            slot = pkg->argc++;
        }
        if (slot >= 0) {
            memcpy(pkg->argv[slot], "--cwd=", 6);
            strncpy(pkg->argv[slot] + 6, kcaps->cwd, UPROC_ARG_MAX - 7);
            pkg->argv[slot][UPROC_ARG_MAX - 1] = '\0';
        }
    }

    pkg->has_cap_mask = 1;
    pkg->cap_mask = kcaps->cap_mask
                  ? (current->cap_mask & kcaps->cap_mask)
                  : current->cap_mask;

    return spawn_pkg(pkg);

fail:
    free_pkg(pkg);
    return -1;
}

int uproc_spawn_mission_from_user(const char *upath, char *const *uargv,
                                 const struct k_mission_grant *grant, uint32_t *mission_id)
{
    if (!spawn_user_allowed()) return -1;
    struct uproc_pkg *pkg = alloc_pkg_from_user(upath, uargv);
    if (!pkg) return -1;
    int id = mission_create(grant, pkg->path);
    if (id < 0) { free_pkg(pkg); return -1; }
    pkg->mission_id = id;
    int pid = spawn_pkg(pkg);
    if (pid >= 0 && mission_id) *mission_id = (uint32_t)id;
    mission_put(id);
    return pid;
}
