#include "kernel.h"
#include "proc.h"
#include "uproc.h"
#include "interrupts.h"
#include "console.h"
#include "serial.h"
#include "input.h"
#include "vfs.h"
#include "vmm.h"
#include "pmm.h"
#include "timer.h"
#include "string.h"
#include "kestrel_abi.h"

/* Hooks exported by idt.c. */
extern void (*syscall_entry_hook)(struct regs *r);
extern void (*user_fault_hook)(struct regs *r);

#include "net.h"

#define USER_VA_LIMIT 0x0000800000000000ULL
#define COPY_CHUNK    256
#define FILE_CHUNK    512
#define UDP_MAX       1400
#define BRK_CEILING   (USER_STACK_TOP - (64ULL << 20))

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

/* We execute syscalls on the process page tables, so user memory is
 * directly addressable — but only touch it after proving every page is
 * mapped, so a bad pointer becomes -1 instead of a kernel page fault. */
static int user_pages_mapped(uint64_t a, size_t len)
{
    uint64_t end = a + len;
    for (uint64_t p = a & ~(PAGE_SIZE - 1); p < end; p += PAGE_SIZE)
        if (!vmm_virt_to_phys(current->pml4, p))
            return 0;
    return 1;
}

int copy_from_user(void *dst, const void *usrc, size_t len)
{
    if (len == 0)
        return 0;
    if (!user_range_ok(usrc, len) || !user_pages_mapped((uint64_t)usrc, len))
        return -1;
    memcpy(dst, usrc, len);
    return 0;
}

int copy_to_user(void *udst, const void *src, size_t len)
{
    if (len == 0)
        return 0;
    if (!user_range_ok(udst, len) || !user_pages_mapped((uint64_t)udst, len))
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
            !vmm_virt_to_phys(current->pml4, a))
            return -1;
        dst[i] = *(const char *)a;
        if (dst[i] == '\0')
            return (long)i;
    }
    return -1;                       /* unterminated within max */
}

/* --- per-call helpers ------------------------------------------------- */

static struct file *fd_file(uint64_t fd)
{
    if (fd < 3 || fd >= MAX_OPEN_FILES)
        return NULL;
    return current->files[fd];
}

static long sys_write(uint64_t fd, uint64_t ubuf, uint64_t len)
{
    char chunk[FILE_CHUNK];
    uint64_t done = 0;

    if (fd == 1 || fd == 2) {
        while (done < len) {
            uint64_t n = len - done;
            if (n > COPY_CHUNK)
                n = COPY_CHUNK;
            if (copy_from_user(chunk, (const void *)(ubuf + done), n) < 0)
                return done ? (long)done : -1;
            for (uint64_t i = 0; i < n; i++) {
                console_putc(chunk[i]);
                serial_putc(chunk[i]);
            }
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

    if (fd == 0) {
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

static long sys_close(uint64_t fd)
{
    struct file *f = fd_file(fd);
    if (!f)
        return -1;
    vfs_close(f);
    current->files[fd] = NULL;
    return 0;
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

    uint64_t lo = old & ~(PAGE_SIZE - 1);
    uint64_t hi = (new_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (uint64_t p = lo; p < hi; p += PAGE_SIZE)
        if (!vmm_virt_to_phys(current->pml4, p))
            vmm_map_page(current->pml4, p, pmm_alloc(), PTE_U | PTE_W);

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

static long sys_psinfo(uint64_t index, uint64_t upi)
{
    struct task *t = task_all_list();
    for (uint64_t i = 0; t && i < index; i++)
        t = t->allnext;
    if (!t)
        return -1;

    struct k_psinfo pi;
    memset(&pi, 0, sizeof(pi));
    pi.pid = t->pid;
    strncpy(pi.name, t->name, sizeof(pi.name) - 1);
    switch (t->state) {
    case TASK_RUNNABLE: pi.state = K_STATE_RUNNABLE; break;
    case TASK_RUNNING:  pi.state = K_STATE_RUNNING;  break;
    case TASK_SLEEPING: pi.state = K_STATE_SLEEPING; break;
    case TASK_ZOMBIE:   pi.state = K_STATE_ZOMBIE;   break;
    default:            pi.state = K_STATE_ZOMBIE;   break;
    }
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
    default:
        kprintf("syscall: unknown %lu from %s (pid %d)\n",
                (unsigned long)r->rax, current->name, current->pid);
        ret = -1;
        break;
    }

    r->rax = (uint64_t)ret;
}

/* Ring-3 CPU exception: kill the process, never the kernel. */
static void user_fault(struct regs *r)
{
    kprintf("uproc: %s (pid %d) killed: exception %lu err=%lx rip=%016lx\n",
            current->name, current->pid, (unsigned long)r->vector,
            (unsigned long)r->err, (unsigned long)r->rip);
    if (r->vector == 14)
        kprintf("uproc: fault address %016lx\n",
                (unsigned long)read_cr2());
    uproc_record_exit(current->pid, -1);
    task_exit(-1);
}

void syscall_init(void)
{
    syscall_entry_hook = syscall_dispatch;
    user_fault_hook = user_fault;
    kprintf("syscall: int 0x80 dispatcher + user fault handler installed\n");
}
