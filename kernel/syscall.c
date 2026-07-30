#include "kernel.h"
#include "proc.h"
#include "uproc.h"
#include "interrupts.h"
#include "console.h"
#include "serial.h"
#include "output.h"
#include "input.h"
#include "vfs.h"
#include "vmm.h"
#include "pmm.h"
#include "timer.h"
#include "rtc.h"
#include "power.h"
#include "pipe.h"
#include "klog.h"
#include "fb.h"
#include "mouse.h"
#include "tcp.h"
#include "wm.h"
#include "module.h"
#include "mount.h"
#include "blockdev.h"
#include "device.h"
#include "random.h"
#include "vm.h"
#include "signal.h"
#include "string.h"
#include "kestrel_abi.h"
#include "smp.h"

/* Hooks exported by idt.c. */
extern void (*syscall_entry_hook)(struct regs *r);
extern void (*user_fault_hook)(struct regs *r);

#include "net.h"

#define USER_VA_LIMIT 0x0000800000000000ULL
#define COPY_CHUNK    256
#define FILE_CHUNK    512
#define UDP_MAX       1400
#define BRK_CEILING   (USER_STACK_TOP - (64ULL << 20))
#define BRK_MAX_GROW  16384          /* pages per call: 64 MiB */
#define PMM_RESERVE   512            /* frames the kernel keeps for itself */

/* copy_str_from_user: no NUL within `max`. dst holds a truncated but
 * NUL-terminated prefix; still negative, so plain `< 0` callers reject. */
#define COPY_STR_TRUNC (-2)

/* --- user-memory access ---------------------------------------------- */

int user_range_ok(const void *uptr, size_t len)
{
    uint64_t a = (uint64_t)uptr;
    if (!a || len > 0x10000000ULL)
        return 0;
    if (a >= USER_VA_LIMIT || len > USER_VA_LIMIT - a)
        return 0;
    return 1;
}

int copy_from_user(void *dst, const void *usrc, size_t len)
{
    if (len == 0)
        return 0;
    if (!user_range_ok(usrc, len) ||
        vm_fault_in_range(current, (uint64_t)usrc, len, 0) < 0)
        return -1;
    memcpy(dst, usrc, len);
    return 0;
}

int copy_to_user(void *udst, const void *src, size_t len)
{
    if (len == 0)
        return 0;
    if (!user_range_ok(udst, len) ||
        vm_fault_in_range(current, (uint64_t)udst, len, 1) < 0)
        return -1;
    memcpy(udst, src, len);
    return 0;
}

long copy_str_from_user(char *dst, const void *usrc, size_t max)
{
    uint64_t a = (uint64_t)usrc;
    if (!a || max == 0)
        return -1;
    for (size_t i = 0; i < max; i++, a++) {
        if (a >= USER_VA_LIMIT)
            return -1;
        if ((i == 0 || (a & (PAGE_SIZE - 1)) == 0) &&
            vm_fault_in_range(current, a, 1, 0) < 0)
            return -1;
        dst[i] = *(const char *)a;
        if (dst[i] == '\0')
            return (long)i;
    }
    dst[max - 1] = '\0';
    return COPY_STR_TRUNC;           /* unterminated within max */
}

/* --- per-call helpers ------------------------------------------------- */

/* fd -> open file. A NULL entry for fd 0/1/2 means "the console"; a
 * non-NULL one (installed by open() or by SYS_SPAWN_IO) means the fd is
 * backed by a file, so redirected stdio flows through the VFS. */
static struct file *fd_file(uint64_t fd)
{
    if (fd >= MAX_OPEN_FILES)
        return NULL;
    return current->files[fd];
}

static long sys_write(uint64_t fd, uint64_t ubuf, uint64_t len)
{
    char chunk[FILE_CHUNK];
    uint64_t done = 0;

    if ((fd == 1 || fd == 2) && !current->files[fd]) {
        while (done < len) {
            uint64_t n = len - done;
            if (n > COPY_CHUNK)
                n = COPY_CHUNK;
            if (copy_from_user(chunk, (const void *)(ubuf + done), n) < 0)
                return done ? (long)done : -1;
            output_write(chunk, (unsigned long)n);
            done += n;
        }
        return (long)done;
    }

    struct file *f = fd_file(fd);
    if (!f)
        return -1;
    while (done < len) {
        uint64_t n = len - done;
        if (n > FILE_CHUNK)
            n = FILE_CHUNK;
        if (copy_from_user(chunk, (const void *)(ubuf + done), n) < 0)
            return done ? (long)done : -1;
        long w = vfs_write(f, chunk, n);
        if (w < 0)
            return done ? (long)done : -1;
        done += (uint64_t)w;
        if ((uint64_t)w < n)
            break;
    }
    return (long)done;
}

static long sys_read(uint64_t fd, uint64_t ubuf, uint64_t len, int blocking)
{
    uint8_t chunk[FILE_CHUNK];

    if (len == 0)
        return 0;

    if (fd == 0 && !current->files[0]) {
        /* Raw console bytes, special keys arrive as values >= 0x80. */
        uint64_t want = len > COPY_CHUNK ? COPY_CHUNK : len;
        uint64_t got = 0;
        if (blocking)
            chunk[got++] = (uint8_t)input_getc();
        while (got < want) {
            int c = input_trygetc();
            if (c < 0)
                break;
            chunk[got++] = (uint8_t)c;
        }
        if (got && copy_to_user((void *)ubuf, chunk, got) < 0)
            return -1;
        return (long)got;
    }

    struct file *f = fd_file(fd);
    if (!f)
        return -1;
    uint64_t done = 0;
    while (done < len) {
        uint64_t n = len - done;
        if (n > FILE_CHUNK)
            n = FILE_CHUNK;
        long g = vfs_read(f, chunk, n);
        if (g < 0)
            return done ? (long)done : -1;
        if (g == 0)
            break;
        if (copy_to_user((void *)(ubuf + done), chunk, (uint64_t)g) < 0)
            return done ? (long)done : -1;
        done += (uint64_t)g;
        if ((uint64_t)g < n)
            break;
    }
    return (long)done;
}

static long sys_open(uint64_t upath, uint64_t flags)
{
    char path[UPROC_PATH_MAX];
    if (copy_str_from_user(path, (const void *)upath, sizeof(path)) < 0)
        return -1;
    struct file *f = vfs_open(path, (int)flags);
    if (!f)
        return -1;
    for (int fd = 3; fd < MAX_OPEN_FILES; fd++) {
        if (!current->files[fd]) {
            current->files[fd] = f;
            return fd;
        }
    }
    vfs_close(f);
    return -1;
}

/* Closing a file-backed fd 0/1/2 reverts it to the console (the entry
 * goes back to NULL); a console fd has nothing to close and fails. */
static long sys_close(uint64_t fd)
{
    struct file *f = fd_file(fd);
    if (!f)
        return -1;
    vfs_close(f);
    current->files[fd] = NULL;
    return 0;
}

/* True if s carries a NUL somewhere within its first `max` bytes. */
static int str_terminated(const char *s, size_t max)
{
    for (size_t i = 0; i < max; i++)
        if (s[i] == '\0')
            return 1;
    return 0;
}

static long sys_spawn_io(uint64_t upath, uint64_t uargv, uint64_t uio)
{
    struct k_spawn_io io;

    if (copy_from_user(&io, (const void *)uio, sizeof(io)) < 0)
        return -1;
    /* Each path must carry its NUL inside its own field: an unterminated
     * one would let the child's vfs_open read past the copied struct. */
    if (!str_terminated(io.in_path, sizeof(io.in_path)) ||
        !str_terminated(io.out_path, sizeof(io.out_path)))
        return -1;
    return uproc_spawn_io_from_user((const char *)upath,
                                    (char *const *)uargv,
                                    io.in_path, io.out_path,
                                    io.out_append ? 1 : 0);
}

static long sys_stat(uint64_t upath, uint64_t ust)
{
    char path[UPROC_PATH_MAX];
    struct k_stat st;
    if (copy_str_from_user(path, (const void *)upath, sizeof(path)) < 0)
        return -1;
    if (vfs_stat(path, &st) < 0)
        return -1;
    return copy_to_user((void *)ust, &st, sizeof(st));
}

static long sys_readdir(uint64_t upath, uint64_t index, uint64_t ude)
{
    char path[UPROC_PATH_MAX];
    struct k_dirent de;
    if (copy_str_from_user(path, (const void *)upath, sizeof(path)) < 0)
        return -1;
    if (vfs_readdir(path, (int)index, &de) < 0)
        return -1;
    return copy_to_user((void *)ude, &de, sizeof(de));
}

static long sys_path_op(uint64_t upath, int (*op)(const char *))
{
    char path[UPROC_PATH_MAX];
    if (copy_str_from_user(path, (const void *)upath, sizeof(path)) < 0)
        return -1;
    return op(path);
}

static long sys_brk(uint64_t new_brk)
{
    if (!current->user)
        return -1;
    uint64_t old = current->user_brk;
    if (new_brk == 0)
        return (long)old;
    if (new_brk < old || new_brk > BRK_CEILING)
        return (long)old;            /* rejected: break unchanged */

    /* Reserve address space only. Pages are committed on first touch and
     * may later move transparently to swap. Keep a per-call virtual-growth
     * cap so a corrupt length cannot reserve most of the user half. */
    if ((new_brk - old + PAGE_SIZE - 1) / PAGE_SIZE > BRK_MAX_GROW)
        return (long)old;

    current->user_brk = new_brk;
    return (long)new_brk;
}

static long sys_meminfo(uint64_t utotal, uint64_t ufree)
{
    uint64_t total_kb = pmm_total_pages() * (PAGE_SIZE / 1024);
    uint64_t free_kb = pmm_free_pages() * (PAGE_SIZE / 1024);
    if (copy_to_user((void *)utotal, &total_kb, sizeof(total_kb)) < 0)
        return -1;
    if (copy_to_user((void *)ufree, &free_kb, sizeof(free_kb)) < 0)
        return -1;
    return 0;
}

static long sys_cpuinfo(uint64_t uinfo)
{
    struct k_cpuinfo info;
    info.online = smp_cpu_count();
    info.discovered = smp_cpu_discovered();
    info.current_cpu = smp_cpu_index();
    info.apic_id = smp_cpu_apic_id(info.current_cpu);
    return copy_to_user((void *)uinfo, &info, sizeof(info));
}

static long sys_psinfo(uint64_t index, uint64_t upi)
{
    struct k_psinfo pi;
    if (task_psinfo(index, &pi) < 0)
        return -1;
    return copy_to_user((void *)upi, &pi, sizeof(pi));
}

static long sys_dns(uint64_t uname, uint64_t uip)
{
    char name[UPROC_PATH_MAX];
    uint32_t ip;
    if (copy_str_from_user(name, (const void *)uname, sizeof(name)) < 0)
        return -1;
    if (dns_resolve(name, &ip) < 0)
        return -1;
    return copy_to_user((void *)uip, &ip, sizeof(ip));
}

static long sys_udp_send(uint64_t ip_be, uint64_t ports, uint64_t ubuf,
                         uint64_t len)
{
    uint8_t kb[UDP_MAX];
    if (len > UDP_MAX)
        return -1;
    if (copy_from_user(kb, (const void *)ubuf, len) < 0)
        return -1;
    /* ABI packs arg2 as (dport << 16) | sport; net.h wants (sport, dport) */
    return udp_send((uint32_t)ip_be, (uint16_t)(ports & 0xFFFF),
                    (uint16_t)(ports >> 16), kb, (int)len);
}

static long sys_udp_recv(uint64_t port, uint64_t ubuf, uint64_t maxlen,
                         uint64_t timeout_ms)
{
    uint8_t kb[UDP_MAX];
    if (maxlen > UDP_MAX)
        maxlen = UDP_MAX;
    int n = udp_recv((uint16_t)port, kb, (int)maxlen, (int)timeout_ms);
    if (n < 0)
        return -1;
    if (n > 0 && copy_to_user((void *)ubuf, kb, (uint64_t)n) < 0)
        return -1;
    return n;
}

static long sys_netinfo(uint64_t uinfo)
{
    struct k_netinfo ni;
    memset(&ni, 0, sizeof(ni));
    if (!net_ready())
        return -1;
    net_get_info(&ni);
    return copy_to_user((void *)uinfo, &ni, sizeof(ni));
}

static long sys_rtc(uint64_t urtc)
{
    struct k_rtc rt;
    /* rtc_read already rejects a chip that never settled or reported an
     * impossible date, so a bad clock surfaces as -1 and never as garbage. */
    if (rtc_read(&rt) < 0)
        return -1;
    return copy_to_user((void *)urtc, &rt, sizeof(rt));
}

static long sys_power(uint64_t action)
{
    switch (action) {
    case K_POWER_REBOOT:
        kprintf("power: reboot requested by %s (pid %d)\n",
                current->name, current->pid);
        power_reboot();              /* noreturn */
    case K_POWER_HALT:
        kprintf("power: halt requested by %s (pid %d)\n",
                current->name, current->pid);
        power_halt();                /* noreturn */
    default:
        return -1;
    }
}

/* --- descriptors ------------------------------------------------------ */

static int fd_alloc_from(int start, struct file *f)
{
    for (int fd = start; fd < MAX_OPEN_FILES; fd++) {
        if (!current->files[fd]) {
            current->files[fd] = f;
            return fd;
        }
    }
    return -1;
}

static long sys_pipe(uint64_t ufds)
{
    struct file *rd, *wr;
    int fds[2];

    if (pipe_create(&rd, &wr) < 0)
        return -1;

    fds[0] = fd_alloc_from(0, rd);
    if (fds[0] < 0) {
        vfs_close(rd);
        vfs_close(wr);
        return -1;
    }
    fds[1] = fd_alloc_from(0, wr);
    if (fds[1] < 0) {
        current->files[fds[0]] = NULL;
        vfs_close(rd);
        vfs_close(wr);
        return -1;
    }
    if (copy_to_user((void *)ufds, fds, sizeof(fds)) < 0) {
        current->files[fds[0]] = NULL;
        current->files[fds[1]] = NULL;
        vfs_close(rd);
        vfs_close(wr);
        return -1;
    }
    return 0;
}

static long sys_dup2(uint64_t oldfd, uint64_t newfd)
{
    if (oldfd >= MAX_OPEN_FILES || newfd >= MAX_OPEN_FILES)
        return -1;
    struct file *f = current->files[oldfd];
    if (!f)
        return -1;                   /* fd 0/1/2 on the console cannot dup */
    if (oldfd == newfd)
        return (long)newfd;

    __atomic_add_fetch(&f->refs, 1, __ATOMIC_ACQ_REL);

    struct file *old = current->files[newfd];
    current->files[newfd] = f;
    if (old)
        vfs_close(old);
    return (long)newfd;
}

/* --- users and permissions -------------------------------------------- */

static long sys_setuid(uint64_t uid)
{
    if (current->uid != 0)
        return -1;                   /* only root may change identity */
    current->uid = (uint32_t)uid;
    current->gid = (uint32_t)uid;
    return 0;
}

static long sys_chmod(uint64_t upath, uint64_t mode)
{
    char path[UPROC_PATH_MAX];
    if (copy_str_from_user(path, (const void *)upath, sizeof(path)) < 0)
        return -1;
    return vfs_chmod(path, (uint32_t)mode & 0777);
}

static long sys_chown(uint64_t upath, uint64_t uid, uint64_t gid)
{
    char path[UPROC_PATH_MAX];
    if (copy_str_from_user(path, (const void *)upath, sizeof(path)) < 0)
        return -1;
    return vfs_chown(path, (uint32_t)uid, (uint32_t)gid);
}

/* --- logging ---------------------------------------------------------- */

static long sys_log(uint64_t level, uint64_t umsg)
{
    char msg[112];
    if (copy_str_from_user(msg, (const void *)umsg, sizeof(msg)) == -1)
        return -1;                   /* truncation (-2) is tolerated */
    klog_write((int)level, current->name, msg);
    return 0;
}

static long sys_logread(uint64_t index, uint64_t uent)
{
    struct k_logent e;
    if (klog_read((uint32_t)index, &e) < 0)
        return -1;
    return copy_to_user((void *)uent, &e, sizeof(e));
}

/* --- loadable kernel modules -------------------------------------------
 * Loading code into the kernel is the most privileged thing a process can
 * ask for, so all three are root-only. The loader reports the reason for a
 * failure to the kernel log; the syscall only says yes or no. */

static long sys_insmod(uint64_t upath)
{
    char path[UPROC_PATH_MAX];

    if (current->uid != 0)
        return -1;
    if (copy_str_from_user(path, (const void *)upath, sizeof(path)) < 0)
        return -1;
    return module_load_path(path) == 0 ? 0 : -1;
}

static long sys_rmmod(uint64_t uname)
{
    char name[MODULE_NAME_MAX];

    if (current->uid != 0)
        return -1;
    if (copy_str_from_user(name, (const void *)uname, sizeof(name)) < 0)
        return -1;
    return module_unload(name) == 0 ? 0 : -1;
}

static long sys_modlist(uint64_t index, uint64_t uinfo)
{
    struct module_info mi;
    struct k_modinfo ki;

    if (module_list((int)index, &mi) < 0)
        return -1;
    memset(&ki, 0, sizeof(ki));
    strncpy(ki.name, mi.name, sizeof(ki.name) - 1);
    strncpy(ki.desc, mi.desc, sizeof(ki.desc) - 1);
    ki.size = (uint32_t)mi.size;
    ki.refs = (uint32_t)mi.refs;
    ki.state = (uint32_t)mi.state;
    return copy_to_user((void *)uinfo, &ki, sizeof(ki));
}

/* --- mounts, block devices, device tree -------------------------------- */

static long sys_mount(uint64_t upath, uint64_t utype, uint64_t udev)
{
    char path[UPROC_PATH_MAX], type[32], dev[32];

    if (current->uid != 0)
        return -1;
    if (copy_str_from_user(path, (const void *)upath, sizeof(path)) < 0)
        return -1;
    if (copy_str_from_user(type, (const void *)utype, sizeof(type)) < 0)
        return -1;
    if (udev) {
        if (copy_str_from_user(dev, (const void *)udev, sizeof(dev)) < 0)
            return -1;
    } else {
        dev[0] = '\0';
    }
    return mount_add(path, type, dev[0] ? dev : NULL);
}

static long sys_umount(uint64_t upath)
{
    char path[UPROC_PATH_MAX];

    if (current->uid != 0)
        return -1;
    if (copy_str_from_user(path, (const void *)upath, sizeof(path)) < 0)
        return -1;
    return mount_remove(path);
}

static long sys_mountlist(uint64_t index, uint64_t uinfo)
{
    struct mount *m;
    struct k_mountinfo ki;
    struct fs_statfs sf;

    if (mount_list((int)index, &m) < 0 || !m)
        return -1;
    memset(&ki, 0, sizeof(ki));
    strncpy(ki.path, m->path, sizeof(ki.path) - 1);
    if (m->type)
        strncpy(ki.fstype, m->type->name, sizeof(ki.fstype) - 1);
    if (m->bd)
        strncpy(ki.device, m->bd->name, sizeof(ki.device) - 1);
    if (m->type && m->type->ops && m->type->ops->statfs &&
        m->type->ops->statfs(m, &sf) == 0) {
        ki.blocks = sf.blocks;
        ki.free_blocks = sf.free_blocks;
        ki.block_size = sf.block_size;
    }
    return copy_to_user((void *)uinfo, &ki, sizeof(ki));
}

static long sys_blklist(uint64_t index, uint64_t uinfo)
{
    struct blockdev *bd;
    struct k_blkinfo ki;

    if (blockdev_list((int)index, &bd) < 0 || !bd)
        return -1;
    memset(&ki, 0, sizeof(ki));
    strncpy(ki.name, bd->name, sizeof(ki.name) - 1);
    ki.block_size = bd->block_size;
    ki.blocks = bd->blocks;
    return copy_to_user((void *)uinfo, &ki, sizeof(ki));
}

static long sys_devlist(uint64_t index, uint64_t uinfo)
{
    struct device_info di;
    struct k_devinfo ki;

    if (device_list((int)index, &di) < 0)
        return -1;
    memset(&ki, 0, sizeof(ki));
    strncpy(ki.bus, di.bus, sizeof(ki.bus) - 1);
    strncpy(ki.name, di.name, sizeof(ki.name) - 1);
    strncpy(ki.driver, di.driver, sizeof(ki.driver) - 1);
    ki.bound = di.bound;
    ki.vendor = di.vendor;
    ki.device = di.device;
    ki.class_id = di.class_id;
    return copy_to_user((void *)uinfo, &ki, sizeof(ki));
}

/* --- graphics and input ------------------------------------------------ */

static long sys_fbinfo(uint64_t uinfo)
{
    struct k_fbinfo fi;
    fb_get(&fi);
    return copy_to_user((void *)uinfo, &fi, sizeof(fi));
}

static long sys_mouse(uint64_t uinfo)
{
    struct k_mouse m;
    memset(&m, 0, sizeof(m));
    mouse_get(&m);
    return copy_to_user((void *)uinfo, &m, sizeof(m));
}

/* --- TCP --------------------------------------------------------------- */

static long sys_tcp_send(uint64_t h, uint64_t ubuf, uint64_t len)
{
    uint8_t kb[1400];
    if (len > sizeof(kb))
        len = sizeof(kb);
    if (copy_from_user(kb, (const void *)ubuf, len) < 0)
        return -1;
    return tcp_send((int)h, kb, (int)len);
}

static long sys_tcp_recv(uint64_t h, uint64_t ubuf, uint64_t max,
                         uint64_t timeout_ms)
{
    uint8_t kb[1400];
    if (max > sizeof(kb))
        max = sizeof(kb);
    int n = tcp_recv((int)h, kb, (int)max, (int)timeout_ms);
    if (n <= 0)
        return n;
    if (copy_to_user((void *)ubuf, kb, (uint64_t)n) < 0)
        return -1;
    return n;
}

/* --- process control --------------------------------------------------- */

static long sys_kill(uint64_t pid, uint64_t sig)
{
    if ((int)pid <= 1)
        return -1;                   /* never the kernel task or init */
    if (sig == 0)
        return task_signal_pid((int)pid, 0, current->uid, true) == -1
                   ? -1 : 0;         /* permission/existence probe */
    if (sig >= K_NSIG)
        return -1;
    return task_signal_pid((int)pid, (int)sig, current->uid, true);
}

static long sys_waitany(uint64_t upid)
{
    int pid = 0;
    long code = uproc_waitany(&pid);
    if (code == -1 && pid == 0)
        return -1;
    if (copy_to_user((void *)upid, &pid, sizeof(pid)) < 0)
        return -1;
    return code;
}

static long sys_getrandom(uint64_t ubuf, uint64_t len, uint64_t flags)
{
    uint8_t chunk[FILE_CHUNK];
    uint64_t done = 0;
    unsigned rflags = 0;

    if (flags & ~(uint64_t)(GRND_NONBLOCK | GRND_RANDOM))
        return -1;
    if (flags & GRND_NONBLOCK)
        rflags |= RANDOM_NONBLOCK;
    if (flags & GRND_RANDOM)
        rflags |= RANDOM_STRONG;
    while (done < len) {
        size_t take = len - done > sizeof(chunk)
                          ? sizeof(chunk) : (size_t)(len - done);
        if (random_get_bytes(chunk, take, rflags) < 0)
            return done ? (long)done : -1;
        if (copy_to_user((void *)(ubuf + done), chunk, take) < 0)
            return done ? (long)done : -1;
        done += take;
    }
    memset(chunk, 0, sizeof(chunk));
    return (long)done;
}

static long sys_swapinfo(uint64_t utotal, uint64_t uused)
{
    uint64_t total_kb = swap_total_pages() * (PAGE_SIZE / 1024);
    uint64_t used_kb = swap_used_pages() * (PAGE_SIZE / 1024);
    if (copy_to_user((void *)utotal, &total_kb, sizeof(total_kb)) < 0 ||
        copy_to_user((void *)uused, &used_kb, sizeof(used_kb)) < 0)
        return -1;
    return 0;
}

/* --- dispatcher ------------------------------------------------------- */

static void syscall_dispatch(struct regs *r)
{
    uint64_t a1 = r->rdi, a2 = r->rsi, a3 = r->rdx, a4 = r->r10;
    long ret = -1;

    /* Vector 0x80 is an interrupt gate: IF is off on entry. Syscalls may
     * block (input, sleep, waitpid), so run with interrupts enabled. */
    sti();

    switch (r->rax) {
    case SYS_EXIT:
        uproc_record_exit(current->pid, (long)(int64_t)a1);
        task_exit((int)a1);          /* noreturn */
    case SYS_WRITE:
        ret = sys_write(a1, a2, a3);
        break;
    case SYS_READ:
        ret = sys_read(a1, a2, a3, 1);
        break;
    case SYS_READ_NB:
        ret = sys_read(a1, a2, a3, 0);
        break;
    case SYS_OPEN:
        ret = sys_open(a1, a2);
        break;
    case SYS_CLOSE:
        ret = sys_close(a1);
        break;
    case SYS_SEEK: {
        struct file *f = fd_file(a1);
        ret = f ? vfs_seek(f, (long)a2, (int)a3) : -1;
        break;
    }
    case SYS_SPAWN:
        ret = uproc_spawn_from_user((const char *)a1, (char *const *)a2);
        break;
    case SYS_SPAWN_IO:
        ret = sys_spawn_io(a1, a2, a3);
        break;
    case SYS_WAITPID:
        ret = uproc_waitpid((int)a1);
        break;
    case SYS_GETPID:
        ret = current->pid;
        break;
    case SYS_SLEEP_MS: {
        uint64_t ticks = a1 / (1000 / TIMER_HZ);
        task_sleep_ticks(ticks ? ticks : 1);
        ret = 0;
        break;
    }
    case SYS_YIELD:
        yield();
        ret = 0;
        break;
    case SYS_READDIR:
        ret = sys_readdir(a1, a2, a3);
        break;
    case SYS_UNLINK:
        ret = sys_path_op(a1, vfs_unlink);
        break;
    case SYS_MKDIR:
        ret = sys_path_op(a1, vfs_mkdir);
        break;
    case SYS_STAT:
        ret = sys_stat(a1, a2);
        break;
    case SYS_BRK:
        ret = sys_brk(a1);
        break;
    case SYS_UPTIME_MS:
        ret = (long)(timer_ticks() * (1000 / TIMER_HZ));
        break;
    case SYS_MEMINFO:
        ret = sys_meminfo(a1, a2);
        break;
    case SYS_PSINFO:
        ret = sys_psinfo(a1, a2);
        break;
    case SYS_DNS:
        ret = sys_dns(a1, a2);
        break;
    case SYS_PING:
        ret = icmp_ping((uint32_t)a1, (uint32_t)a2);
        break;
    case SYS_UDP_SEND:
        ret = sys_udp_send(a1, a2, a3, a4);
        break;
    case SYS_UDP_RECV:
        ret = sys_udp_recv(a1, a2, a3, a4);
        break;
    case SYS_NETINFO:
        ret = sys_netinfo(a1);
        break;
    case SYS_RTC:
        ret = sys_rtc(a1);
        break;
    case SYS_POWER:
        ret = sys_power(a1);
        break;

    /* --- descriptors and processes --- */
    case SYS_PIPE:
        ret = sys_pipe(a1);
        break;
    case SYS_DUP2:
        ret = sys_dup2(a1, a2);
        break;
    case SYS_KILL:
        ret = sys_kill(a1, a2);
        break;
    case SYS_WAITANY:
        ret = sys_waitany(a1);
        break;
    case SYS_EXEC:
        ret = uproc_exec_from_user((const char *)a1, (char *const *)a2);
        break;

    /* --- users and permissions --- */
    case SYS_GETUID:
        ret = (long)current->uid;
        break;
    case SYS_GETGID:
        ret = (long)current->gid;
        break;
    case SYS_SETUID:
        ret = sys_setuid(a1);
        break;
    case SYS_CHMOD:
        ret = sys_chmod(a1, a2);
        break;
    case SYS_CHOWN:
        ret = sys_chown(a1, a2, a3);
        break;

    /* --- time and logging --- */
    case SYS_TIME:
        ret = (long)rtc_unix_time();
        break;
    case SYS_LOG:
        ret = sys_log(a1, a2);
        break;
    case SYS_LOGREAD:
        ret = sys_logread(a1, a2);
        break;
    case SYS_SYNC:
        ret = 0;                     /* KFS is write-through */
        break;

    /* --- loadable kernel modules --- */
    case SYS_INSMOD:
        ret = sys_insmod(a1);
        break;
    case SYS_RMMOD:
        ret = sys_rmmod(a1);
        break;
    case SYS_MODLIST:
        ret = sys_modlist(a1, a2);
        break;

    /* --- mounts, block devices, device tree --- */
    case SYS_MOUNT:
        ret = sys_mount(a1, a2, a3);
        break;
    case SYS_UMOUNT:
        ret = sys_umount(a1);
        break;
    case SYS_MOUNTLIST:
        ret = sys_mountlist(a1, a2);
        break;
    case SYS_GETRANDOM:
        ret = sys_getrandom(a1, a2, a3);
        break;
    case SYS_SWAPINFO:
        ret = sys_swapinfo(a1, a2);
        break;
    case SYS_SIGACTION:
        ret = signal_sys_sigaction(a1, a2, a3);
        break;
    case SYS_SIGPROCMASK:
        ret = signal_sys_sigprocmask(a1, a2, a3);
        break;
    case SYS_SIGRETURN:
        if (signal_sigreturn(current, r) < 0) {
            uproc_record_exit(current->pid, 128 + SIGSEGV);
            task_exit(128 + SIGSEGV);
        }
        return;
    case SYS_CPUINFO:
        ret = sys_cpuinfo(a1);
        break;
    case SYS_BLKLIST:
        ret = sys_blklist(a1, a2);
        break;
    case SYS_DEVLIST:
        ret = sys_devlist(a1, a2);
        break;

    /* --- graphics, input, windows --- */
    case SYS_FBINFO:
        ret = sys_fbinfo(a1);
        break;
    case SYS_MOUSE:
        ret = sys_mouse(a1);
        break;
    case SYS_WIN_CREATE:
        ret = wm_sys_create(a1, a2);
        break;
    case SYS_WIN_DESTROY:
        ret = wm_sys_destroy(a1);
        break;
    case SYS_WIN_FLUSH:
        ret = wm_sys_flush(a1);
        break;
    case SYS_WIN_EVENT:
        ret = wm_sys_event(a1, a2, a3);
        break;
    case SYS_WIN_MOVE:
        ret = wm_sys_move(a1, (int)(int64_t)a2, (int)(int64_t)a3);
        break;
    case SYS_WIN_LIST:
        ret = wm_sys_list(a1, a2);
        break;
    case SYS_WIN_CTL:
        ret = wm_sys_ctl(a1, a2);
        break;

    /* --- TCP --- */
    case SYS_TCP_CONNECT:
        ret = tcp_connect((uint32_t)a1, (uint16_t)a2, (int)a3);
        break;
    case SYS_TCP_SEND:
        ret = sys_tcp_send(a1, a2, a3);
        break;
    case SYS_TCP_RECV:
        ret = sys_tcp_recv(a1, a2, a3, a4);
        break;
    case SYS_TCP_CLOSE:
        ret = tcp_close((int)a1);
        break;

    default:
        kprintf("syscall: unknown %lu from %s (pid %d)\n",
                (unsigned long)r->rax, current->name, current->pid);
        ret = -1;
        break;
    }

    r->rax = (uint64_t)ret;

    /* SYS_KILL never tears a task down while it may hold a kernel lock.
     * A sleeping victim is made runnable and resumes inside its interrupted
     * syscall, so this return checkpoint is what makes the pending kill
     * take effect before any more ring-3 code can run. */
    task_check_kill();
    signal_deliver_pending(current, r);
}

/* Ring-3 CPU exception: kill the process, never the kernel. */
static void user_fault(struct regs *r)
{
    if (r->vector == 14 &&
        vm_handle_page_fault(current, read_cr2(), r->err) == 0)
        return;
    kprintf("uproc: %s (pid %d): exception %lu err=%lx rip=%016lx\n",
            current->name, current->pid, (unsigned long)r->vector,
            (unsigned long)r->err, (unsigned long)r->rip);
    if (r->vector == 14)
        kprintf("uproc: fault address %016lx\n",
                (unsigned long)read_cr2());
    signal_user_exception(current, r);
}

void syscall_init(void)
{
    syscall_entry_hook = syscall_dispatch;
    user_fault_hook = user_fault;
    kprintf("syscall: int 0x80 dispatcher + user fault handler installed\n");
}
