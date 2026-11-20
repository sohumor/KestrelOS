/* Hangar: park a running app to disk, resume it exactly where it was.
 *
 * Parking happens in the app's own context, at its syscall-exit
 * checkpoint, so its register file is the complete ring-3 state (the
 * syscall's return value already in rax) and no kernel lock is held. The
 * task faults in every file-backed page, then writes its resident pages,
 * window pixels, FPU state, open-file positions and signal state to a
 * bay under /hangar and exits. Resuming builds a fresh task from that
 * image and irets straight back into the saved instruction stream.
 *
 * Anything whose state lives outside the process — a TCP connection, a
 * bound UDP port, a pipe, a device, shared memory, child processes — is
 * refused with a sentence the desktop shows verbatim. */

#include "kernel.h"
#include "string.h"
#include "proc.h"
#include "uproc.h"
#include "interrupts.h"
#include "vfs.h"
#include "vmm.h"
#include "vm.h"
#include "pmm.h"
#include "kheap.h"
#include "timer.h"
#include "fpu.h"
#include "fb.h"
#include "wm.h"
#include "tcp.h"
#include "udp.h"
#include "mount.h"
#include "flight.h"
#include "spinlock.h"
#include "hangar.h"
#include "hangar_fmt.h"
#include "kestrel_abi.h"

extern void hangar_enter_user(const struct hangar_regs *r)
    __attribute__((noreturn));

_Static_assert(sizeof(((struct task *)0)->sig_actions) <= HANGAR_SIGACT_BYTES,
               "signal actions must fit the Hangar manifest");
_Static_assert(FPU_STATE_SIZE == HANGAR_FPU_BYTES,
               "FXSAVE area must match the Hangar manifest");
_Static_assert(K_HANGAR_BAYS <= 99, "bay names are two digits");

#define REASON_MAX   K_HANGAR_REASON_MAX
#define WBUF_BYTES   (64U * 1024U)
#define WIN_VA_END   (USER_WIN_BASE + (uint64_t)USER_WIN_MAX * USER_WIN_STRIDE)
#define THUMB_BG     0xFF0B1220U
#define PARK_START_TICKS (3 * TIMER_HZ)     /* to reach a safe point */
#define PARK_TOTAL_TICKS (180 * TIMER_HZ)   /* to finish writing */

/* ------------------------------------------------------------ helpers */

static void rs_cat(char *dst, const char *src)
{
    size_t n = strlen(dst);

    while (*src && n + 1 < REASON_MAX)
        dst[n++] = *src++;
    dst[n] = '\0';
}

static void reason3(char *dst, const char *a, const char *b, const char *c)
{
    dst[0] = '\0';
    if (a)
        rs_cat(dst, a);
    if (b)
        rs_cat(dst, b);
    if (c)
        rs_cat(dst, c);
}

static void bay_path(char *out, int bay, const char *leaf, int n)
{
    char *p = out;
    const char *pre = "/hangar/bay";

    while (*pre)
        *p++ = *pre++;
    *p++ = (char)('0' + bay / 10);
    *p++ = (char)('0' + bay % 10);
    if (leaf) {
        *p++ = '/';
        while (*leaf)
            *p++ = *leaf++;
        if (n >= 0) {
            *p++ = '.';
            if (n >= 10)
                *p++ = (char)('0' + n / 10);
            *p++ = (char)('0' + n % 10);
        }
    }
    *p = '\0';
}

static bool io_begin(void)
{
    bool was = current->hangar_io;
    current->hangar_io = true;
    return was;
}

static void io_end(bool was)
{
    current->hangar_io = was;
}

static int write_all(struct file *f, const void *data, size_t len)
{
    const uint8_t *p = data;

    while (len) {
        long n = vfs_write(f, p, len);
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int read_all(struct file *f, void *data, size_t len)
{
    uint8_t *p = data;

    while (len) {
        long n = vfs_read(f, p, len);
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int write_file(const char *path, const void *data, size_t len)
{
    struct file *f = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    int rc;

    if (!f)
        return -1;
    rc = write_all(f, data, len);
    vfs_close(f);
    return rc;
}

static int read_file(const char *path, void *data, size_t len)
{
    struct k_stat st;
    struct file *f;
    int rc;

    if (vfs_stat(path, &st) < 0 || st.is_dir || st.size != len)
        return -1;
    f = vfs_open(path, O_RDONLY);
    if (!f)
        return -1;
    rc = read_all(f, data, len);
    vfs_close(f);
    return rc;
}

static bool bay_exists(int bay)
{
    char path[40];
    struct k_stat st;

    bay_path(path, bay, NULL, -1);
    return vfs_stat(path, &st) == 0;
}

/* Manifest first, so a half-removed bay can never look resumable. */
static void bay_remove(int bay)
{
    char path[40];

    bay_path(path, bay, "manifest", -1);
    vfs_unlink(path);
    for (int i = 0; i < (int)HANGAR_MAX_CHUNKS; i++) {
        bay_path(path, bay, "mem", i);
        vfs_unlink(path);
    }
    for (int i = 0; i < HANGAR_MAX_WINS; i++) {
        bay_path(path, bay, "win", i);
        vfs_unlink(path);
    }
    bay_path(path, bay, "thumb", -1);
    vfs_unlink(path);
    bay_path(path, bay, NULL, -1);
    vfs_unlink(path);
}

static int load_manifest(int bay, struct hangar_manifest *m,
                         const char **why)
{
    char path[40];

    *why = "that Hangar bay is empty";
    if (bay < 0 || bay >= K_HANGAR_BAYS)
        return -1;
    bay_path(path, bay, "manifest", -1);
    if (read_file(path, m, sizeof(*m)) < 0)
        return -1;
    *why = hangar_manifest_check(m);
    return *why ? -1 : 0;
}

/* -------------------------------------------------- bays in flight */

static spinlock_t hangar_lock = SPINLOCK_INIT;
static bool bay_busy[K_HANGAR_BAYS];

struct park_job {
    bool used;
    int pid;
    volatile int done;
    int bay;
    char reason[REASON_MAX];
};
static struct park_job jobs[4];

static struct park_job *job_alloc(int pid)
{
    struct park_job *j = NULL;
    uint64_t f = spin_lock_irqsave(&hangar_lock);

    for (unsigned i = 0; i < sizeof(jobs) / sizeof(jobs[0]); i++) {
        if (jobs[i].used && jobs[i].pid == pid) {
            j = NULL;
            break;
        }
        if (!jobs[i].used && !j)
            j = &jobs[i];
    }
    if (j) {
        memset(j, 0, sizeof(*j));
        j->used = true;
        j->pid = pid;
        j->bay = -1;
    }
    spin_unlock_irqrestore(&hangar_lock, f);
    return j;
}

static void job_finish(int pid, int bay, const char *reason)
{
    uint64_t f = spin_lock_irqsave(&hangar_lock);

    for (unsigned i = 0; i < sizeof(jobs) / sizeof(jobs[0]); i++) {
        if (jobs[i].used && jobs[i].pid == pid && !jobs[i].done) {
            jobs[i].bay = bay;
            reason3(jobs[i].reason, reason, NULL, NULL);
            __atomic_store_n(&jobs[i].done, 1, __ATOMIC_RELEASE);
        }
    }
    spin_unlock_irqrestore(&hangar_lock, f);
}

static void job_free(struct park_job *j)
{
    uint64_t f = spin_lock_irqsave(&hangar_lock);
    j->used = false;
    spin_unlock_irqrestore(&hangar_lock, f);
}

static int bay_claim(int bay)
{
    int ok = 0;
    uint64_t f = spin_lock_irqsave(&hangar_lock);

    if (!bay_busy[bay]) {
        bay_busy[bay] = true;
        ok = 1;
    }
    spin_unlock_irqrestore(&hangar_lock, f);
    return ok;
}

static void bay_release(int bay)
{
    uint64_t f = spin_lock_irqsave(&hangar_lock);
    bay_busy[bay] = false;
    spin_unlock_irqrestore(&hangar_lock, f);
}

/* --------------------------------------------------- buffered writer */

struct wbuf {
    struct file *f;
    uint8_t *buf;
    size_t len;
    int failed;
};

static void wb_flush(struct wbuf *w)
{
    if (w->f && w->len && !w->failed && write_all(w->f, w->buf, w->len) < 0)
        w->failed = 1;
    w->len = 0;
}

static void wb_put(struct wbuf *w, const void *data, size_t len)
{
    const uint8_t *p = data;

    while (len && !w->failed) {
        size_t room = WBUF_BYTES - w->len;
        size_t n = len < room ? len : room;
        memcpy(w->buf + w->len, p, n);
        w->len += n;
        p += n;
        len -= n;
        if (w->len == WBUF_BYTES)
            wb_flush(w);
    }
}

static int wb_close(struct wbuf *w)
{
    wb_flush(w);
    if (w->f)
        vfs_close(w->f);
    w->f = NULL;
    return w->failed ? -1 : 0;
}

struct rbuf {
    struct file *f;
    uint8_t *buf;
    size_t len, pos;
    uint32_t left;      /* bytes of the file not yet buffered */
};

static int rb_open(struct rbuf *r, const char *path)
{
    struct k_stat st;

    if (vfs_stat(path, &st) < 0 || st.is_dir)
        return -1;
    r->f = vfs_open(path, O_RDONLY);
    r->len = r->pos = 0;
    r->left = st.size;
    return r->f ? 0 : -1;
}

static int rb_get(struct rbuf *r, void *data, size_t len)
{
    uint8_t *p = data;

    while (len) {
        if (r->pos == r->len) {
            size_t want = r->left < WBUF_BYTES ? r->left : WBUF_BYTES;
            if (want == 0 || read_all(r->f, r->buf, want) < 0)
                return -1;
            r->left -= (uint32_t)want;
            r->len = want;
            r->pos = 0;
        }
        size_t n = r->len - r->pos < len ? r->len - r->pos : len;
        memcpy(p, r->buf + r->pos, n);
        r->pos += n;
        p += n;
        len -= n;
    }
    return 0;
}

static int rb_eof(const struct rbuf *r)
{
    return r->pos == r->len && r->left == 0;
}

static void rb_close(struct rbuf *r)
{
    if (r->f)
        vfs_close(r->f);
    r->f = NULL;
}

/* ------------------------------------------------------------ parking */

struct page_list {
    uint64_t *va;
    uint32_t n, cap;
    uint64_t foreign;   /* first mapping we cannot account for */
    int overflow;
};

static int collect_visit(uint64_t va, uint64_t *pte, void *arg)
{
    struct page_list *pl = arg;
    uint64_t e = *pte;

    if (va >= USER_WIN_BASE && va < WIN_VA_END)
        return 0;                    /* window pixels are saved apart */
    if (!(e & (PTE_P | PTE_SWAP)))
        return 0;
    if (!vm_page_is_managed(current, va)) {
        pl->foreign = va ? va : 1;
        return 1;
    }
    if (pl->n == pl->cap) {
        pl->overflow = 1;
        return 1;
    }
    pl->va[pl->n++] = va;
    return 0;
}

static int page_is_zero(const uint8_t *p)
{
    const uint64_t *q = (const uint64_t *)p;

    for (unsigned i = 0; i < PAGE_SIZE / 8; i++)
        if (q[i])
            return 0;
    return 1;
}

/* Devices with no state of their own reopen as good as new. */
static bool stateless_device(const char *path)
{
    static const char *const ok[] = {
        "/dev/null", "/dev/zero", "/dev/random", "/dev/urandom",
    };

    for (unsigned i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
        if (strcmp(path, ok[i]) == 0)
            return true;
    return false;
}

/* Everything that would make a resumed copy behave differently from the
 * original is refused here, before anything touches the disk. */
static int park_preflight(struct hangar_manifest *m, char *reason)
{
    const char *who = current->name;
    int kids[1];

    if (current->pid <= 1 || !current->user) {
        reason3(reason, who, " is a system task and cannot be parked", NULL);
        return -1;
    }
    for (int i = 0;; i++) {
        struct hangar_win meta;
        const uint32_t *pix;
        int rc = wm_hangar_window(current->pid, i, &meta, &pix);

        if (rc < 0) {
            reason3(reason, "The desktop shell cannot park itself", NULL, NULL);
            return -1;
        }
        if (rc == 0)
            break;
        if (i >= HANGAR_MAX_WINS) {
            reason3(reason, who, " has more than 4 windows open", NULL);
            return -1;
        }
    }
    if (tcp_count_owned(current->pid) > 0) {
        reason3(reason, who, " has an open network connection; its peer "
                "cannot be parked with it", NULL);
        return -1;
    }
    if (udp_count_owned(current->pid) > 0) {
        reason3(reason, who, " is listening on a network port; parking "
                "would drop incoming packets", NULL);
        return -1;
    }
    if (task_child_pids(current->pid, kids, 1) > 0) {
        reason3(reason, who, " has child processes running", NULL);
        return -1;
    }
    for (int fd = 0; fd < MAX_OPEN_FILES; fd++) {
        struct file *f = current->files[fd];
        struct hangar_file *hf;

        if (!f)
            continue;               /* the console */
        if (!f->mnt) {
            reason3(reason, who, " has an open pipe", NULL);
            return -1;
        }
        if (!f->mnt->type || (strcmp(f->mnt->type->name, "kfs") != 0 &&
                              !stateless_device(f->path))) {
            reason3(reason, who, " has a device open (", f->path);
            rs_cat(reason, ")");
            return -1;
        }
        if (!f->path[0] || strlen(f->path) >= HANGAR_PATH_MAX) {
            reason3(reason, who, " has a file open by a path too long "
                    "to record", NULL);
            return -1;
        }
        hf = &m->files[m->nfile++];
        hf->fd = fd;
        hf->flags = f->flags;
        hf->pos = f->pos;
        strcpy(hf->path, f->path);
    }
    if (current->vm_area_count > HANGAR_MAX_AREAS) {
        reason3(reason, who, " has too many memory regions", NULL);
        return -1;
    }
    return 0;
}

static int park_memory(struct hangar_manifest *m, int bay, char *reason,
                       uint8_t *page, uint32_t *enc, struct wbuf *w)
{
    struct page_list pl;
    uint64_t chunk_bytes = 0;
    char path[40];
    int rc = -1;

    /* ELF-backed pages are re-read from the executable on demand; the
     * image must not depend on that file staying unchanged, so pull them
     * all in first and store them like anonymous memory. */
    for (int i = 0; i < current->vm_area_count; i++) {
        struct vm_area *a = &current->vm_areas[i];
        if (a->backing && a->file_size &&
            vm_fault_in_range(current, a->start, a->file_size, 0) < 0) {
            reason3(reason, "Not enough memory to load all of ",
                    current->name, " before parking");
            return -1;
        }
    }

    memset(&pl, 0, sizeof(pl));
    pl.cap = HANGAR_MAX_PAGES;
    pl.va = kmalloc((size_t)pl.cap * sizeof(uint64_t));
    if (!pl.va) {
        reason3(reason, "Not enough kernel memory to park ", current->name,
                NULL);
        return -1;
    }
    vmm_for_each_user_pte(current->pml4, collect_visit, &pl);
    if (pl.foreign) {
        reason3(reason, current->name, " has shared memory mapped that "
                "cannot be saved", NULL);
        goto out;
    }
    if (pl.overflow) {
        reason3(reason, current->name, " uses more than 64 MiB of memory",
                NULL);
        goto out;
    }

    m->mem_sum = HANGAR_FNV_INIT;
    for (uint32_t i = 0; i < pl.n; i++) {
        struct hangar_page hp;
        uint64_t *pte;
        size_t words, rec;

        if (copy_from_user(page, (const void *)pl.va[i], PAGE_SIZE) < 0) {
            reason3(reason, "Could not read the memory of ", current->name,
                    NULL);
            goto out;
        }
        if (page_is_zero(page))
            continue;                /* demand-zero brings it back */
        pte = vmm_get_pte(current->pml4, pl.va[i], 0);
        words = hangar_rle_encode((const uint32_t *)page, HANGAR_PAGE_WORDS,
                                  enc, hangar_rle_bound(HANGAR_PAGE_WORDS));
        hp.va = pl.va[i];
        hp.pte_flags = pte ? (uint32_t)(*pte & PTE_W) : 0;
        hp.words = (uint32_t)words;
        rec = sizeof(hp) + words * 4;

        if (!w->f || chunk_bytes + rec > HANGAR_CHUNK_BYTES) {
            if (w->f && wb_close(w) < 0)
                goto disk;
            if (m->nchunks == HANGAR_MAX_CHUNKS) {
                reason3(reason, current->name, " is too large to park",
                        NULL);
                goto out;
            }
            bay_path(path, bay, "mem", (int)m->nchunks++);
            w->f = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
            w->failed = 0;
            w->len = 0;
            chunk_bytes = 0;
            if (!w->f)
                goto disk;
        }
        wb_put(w, &hp, sizeof(hp));
        wb_put(w, enc, words * 4);
        m->mem_sum = hangar_fnv(m->mem_sum, &hp, sizeof(hp));
        m->mem_sum = hangar_fnv(m->mem_sum, enc, words * 4);
        m->mem_bytes += rec;
        chunk_bytes += rec;
        m->npages++;
        if (w->failed)
            goto disk;
    }
    if (w->f && wb_close(w) < 0)
        goto disk;
    rc = 0;
    goto out;
disk:
    reason3(reason, "The disk is full: no room in the Hangar for ",
            current->name, NULL);
out:
    if (w->f)
        wb_close(w);
    kfree(pl.va);
    return rc;
}

static int park_windows(struct hangar_manifest *m, int bay, char *reason,
                        struct wbuf *w)
{
    uint32_t *row = NULL, *thumb = NULL;
    const uint32_t *best = NULL;
    int best_w = 0, best_h = 0;
    char path[40];

    row = kmalloc(hangar_rle_bound(HANGAR_WIN_MAX_W) * 4);
    thumb = kmalloc((size_t)K_HANGAR_THUMB_W * K_HANGAR_THUMB_H * 4);
    if (!row || !thumb)
        goto nomem;

    for (int i = 0; i < HANGAR_MAX_WINS; i++) {
        struct hangar_win *hw = &m->wins[i];
        const uint32_t *pix;

        if (wm_hangar_window(current->pid, i, hw, &pix) != 1)
            break;
        m->nwin++;
        bay_path(path, bay, "win", i);
        w->f = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
        w->failed = 0;
        w->len = 0;
        if (!w->f)
            goto disk;
        hw->pix_sum = HANGAR_FNV_INIT;
        for (int y = 0; y < hw->h; y++) {
            const uint32_t *src = pix + (long)y * hw->w;
            uint32_t words = (uint32_t)hangar_rle_encode(
                src, (size_t)hw->w, row, hangar_rle_bound(HANGAR_WIN_MAX_W));
            wb_put(w, &words, 4);
            wb_put(w, row, (size_t)words * 4);
            hw->pix_sum = hangar_fnv(hw->pix_sum, src, (size_t)hw->w * 4);
        }
        if (wb_close(w) < 0)
            goto disk;
        if (!best || (long)hw->w * hw->h > (long)best_w * best_h) {
            best = pix;
            best_w = hw->w;
            best_h = hw->h;
        }
    }

    m->thumb_w = K_HANGAR_THUMB_W;
    m->thumb_h = K_HANGAR_THUMB_H;
    hangar_thumb(best, best_w, best_h, thumb, K_HANGAR_THUMB_W,
                 K_HANGAR_THUMB_H, THUMB_BG);
    m->thumb_sum = hangar_fnv(HANGAR_FNV_INIT, thumb,
                              (size_t)K_HANGAR_THUMB_W * K_HANGAR_THUMB_H * 4);
    bay_path(path, bay, "thumb", -1);
    if (write_file(path, thumb,
                   (size_t)K_HANGAR_THUMB_W * K_HANGAR_THUMB_H * 4) < 0)
        goto disk;
    kfree(row);
    kfree(thumb);
    return 0;

nomem:
    reason3(reason, "Not enough kernel memory to park ", current->name, NULL);
    goto fail;
disk:
    reason3(reason, "The disk is full: no room in the Hangar for ",
            current->name, NULL);
fail:
    if (w->f)
        wb_close(w);
    kfree(row);
    kfree(thumb);
    return -1;
}

static void capture_regs(struct hangar_regs *h, const struct regs *r)
{
    h->gpr[0] = r->rax;
    h->gpr[1] = r->rbx;
    h->gpr[2] = r->rcx;
    h->gpr[3] = r->rdx;
    h->gpr[4] = r->rsi;
    h->gpr[5] = r->rdi;
    h->gpr[6] = r->rbp;
    h->gpr[7] = r->r8;
    h->gpr[8] = r->r9;
    h->gpr[9] = r->r10;
    h->gpr[10] = r->r11;
    h->gpr[11] = r->r12;
    h->gpr[12] = r->r13;
    h->gpr[13] = r->r14;
    h->gpr[14] = r->r15;
    h->rip = r->rip;
    h->rflags = r->rflags;
    h->rsp = r->rsp;
}

static int park_self(struct regs *r, char *reason)
{
    struct hangar_manifest *m = kzalloc(sizeof(*m));
    uint8_t *page = kmalloc(PAGE_SIZE);
    uint32_t *enc = kmalloc(hangar_rle_bound(HANGAR_PAGE_WORDS) * 4);
    struct wbuf w;
    char path[40];
    int bay = -1, rc = -1;
    bool was;
    uint64_t f;

    memset(&w, 0, sizeof(w));
    w.buf = kmalloc(WBUF_BYTES);
    if (!m || !page || !enc || !w.buf) {
        reason3(reason, "Not enough kernel memory to park ", current->name,
                NULL);
        goto out;
    }

    /* The kernel never touches SSE, so the live FPU registers are the
     * app's; save them before anything can be scheduled in between. */
    f = irq_save();
    fpu_save(current->fpu_state);
    memcpy(m->fpu, current->fpu_state, HANGAR_FPU_BYTES);
    irq_restore(f);

    if (park_preflight(m, reason) < 0)
        goto out;

    was = io_begin();
    for (int i = 0; i < K_HANGAR_BAYS; i++) {
        if (!bay_exists(i) && bay_claim(i)) {
            bay = i;
            break;
        }
    }
    if (bay < 0) {
        io_end(was);
        reason3(reason, "All Hangar bays are occupied; resume or discard "
                "an app first", NULL, NULL);
        goto out;
    }
    bay_path(path, bay, NULL, -1);
    if (vfs_mkdir(path) < 0) {
        io_end(was);
        reason3(reason, "Could not create a Hangar bay on disk", NULL, NULL);
        goto release;
    }

    if (park_memory(m, bay, reason, page, enc, &w) < 0 ||
        park_windows(m, bay, reason, &w) < 0) {
        bay_remove(bay);
        io_end(was);
        goto release;
    }

    m->uid = current->uid;
    m->gid = current->gid;
    m->orig_pid = current->pid;
    m->parked_at = vfs_now();
    m->cap_mask = current->cap_mask;
    m->sig_mask = current->sig_mask;
    m->sig_pending = current->sig_pending;
    strncpy(m->name, current->name, sizeof(m->name) - 1);
    strncpy(m->unveil, current->unveil_prefix, sizeof(m->unveil) - 1);
    m->heap_start = current->user_heap_start;
    m->brk = current->user_brk;
    m->narea = (uint32_t)current->vm_area_count;
    for (int i = 0; i < current->vm_area_count; i++) {
        m->areas[i].start = current->vm_areas[i].start;
        m->areas[i].end = current->vm_areas[i].end;
        m->areas[i].pte_flags = current->vm_areas[i].pte_flags;
    }
    capture_regs(&m->regs, r);
    memcpy(m->sigact, current->sig_actions, sizeof(current->sig_actions));
    hangar_manifest_seal(m);

    bay_path(path, bay, "manifest", -1);
    if (write_file(path, m, sizeof(*m)) < 0) {
        bay_remove(bay);
        io_end(was);
        reason3(reason, "The disk is full: no room in the Hangar for ",
                current->name, NULL);
        goto release;
    }
    io_end(was);
    flight_record(FLIGHT_CAT_PROC, "park", (int16_t)current->pid,
                  (uint64_t)bay, m->npages, current->name);
    kprintf("hangar: parked %s (pid %d) in bay %d: %u pages, %u windows\n",
            current->name, current->pid, bay, m->npages, m->nwin);
    rc = bay;
release:
    bay_release(bay);
out:
    kfree(m);
    kfree(page);
    kfree(enc);
    kfree(w.buf);
    return rc;
}

void hangar_checkpoint(struct regs *r)
{
    char reason[REASON_MAX];
    int bay;

    if (!task_park_begin())
        return;
    reason[0] = '\0';
    bay = park_self(r, reason);
    job_finish(current->pid, bay, reason);
    if (bay >= 0) {
        uproc_record_exit(current->pid, 0);
        task_exit(0);
    }
    kprintf("hangar: %s\n", reason);
    __atomic_store_n(&current->park_state, 0, __ATOMIC_RELEASE);
}

static long op_park(int pid, char *reason)
{
    struct park_job *job;
    uint64_t start = timer_ticks();
    bool started = false;
    long rc = -1;
    int req;

    if (pid == current->pid) {
        reason3(reason, "An app is parked from the desktop or the hangar "
                "command, not by itself", NULL, NULL);
        return -1;
    }
    job = job_alloc(pid);
    if (!job) {
        reason3(reason, "That app is already being parked", NULL, NULL);
        return -1;
    }
    req = task_park_request(pid, current->uid);
    if (req == -1) {
        reason3(reason, "No such app, or it belongs to another user",
                NULL, NULL);
        goto out;
    }
    if (req == -3) {
        reason3(reason, "Mission-confined apps cannot be parked; their "
                "grants do not persist to disk", NULL, NULL);
        goto out;
    }
    if (req == -2) {
        reason3(reason, "That app is already being parked", NULL, NULL);
        goto out;
    }

    for (;;) {
        uint64_t now = timer_ticks();

        if (__atomic_load_n(&job->done, __ATOMIC_ACQUIRE)) {
            rc = job->bay;
            reason3(reason, job->reason, NULL, NULL);
            break;
        }
        if (!started && now - start > PARK_START_TICKS) {
            if (task_park_cancel(pid)) {
                reason3(reason, "The app is busy inside a system call and "
                        "did not reach a safe point; try again", NULL, NULL);
                break;
            }
            started = true;
        }
        if (!task_exists(pid)) {
            /* Finished between our checks? */
            if (__atomic_load_n(&job->done, __ATOMIC_ACQUIRE))
                continue;
            reason3(reason, "The app exited before it could be parked",
                    NULL, NULL);
            break;
        }
        if (now - start > PARK_TOTAL_TICKS) {
            reason3(reason, "Parking timed out", NULL, NULL);
            break;
        }
        task_sleep_ticks(2);
    }
out:
    job_free(job);
    return rc;
}

/* ------------------------------------------------------------ resuming */

struct resume_pkg {
    struct hangar_manifest m;
    int bay;
    uint64_t caps;
};

static void resume_fail(struct resume_pkg *pkg, const char *why)
{
    int bay = pkg->bay;

    kprintf("hangar: cannot resume bay %d (%s): %s\n", bay, pkg->m.name, why);
    flight_record(FLIGHT_CAT_PROC, "resume_fail", (int16_t)current->pid,
                  (uint64_t)bay, 0, why);
    kfree(pkg);
    bay_release(bay);
    current->hangar_io = false;
    uproc_record_exit(current->pid, 127);
    task_exit(127);
}

static int restore_memory(struct resume_pkg *pkg, uint8_t *page,
                          uint32_t *enc, struct rbuf *rb)
{
    struct hangar_manifest *m = &pkg->m;
    uint32_t sum = HANGAR_FNV_INIT, pages = 0;
    char path[40];

    for (uint32_t c = 0; c < m->nchunks; c++) {
        bay_path(path, pkg->bay, "mem", (int)c);
        if (rb_open(rb, path) < 0)
            return -1;
        while (!rb_eof(rb)) {
            struct hangar_page hp;
            uint64_t *pte, phys;

            if (rb_get(rb, &hp, sizeof(hp)) < 0 ||
                hp.words > hangar_rle_bound(HANGAR_PAGE_WORDS) ||
                (hp.va & (PAGE_SIZE - 1)) || hp.va >= HANGAR_USER_TOP ||
                (hp.va >= USER_WIN_BASE && hp.va < WIN_VA_END) ||
                !vm_page_is_managed(current, hp.va) ||
                rb_get(rb, enc, (size_t)hp.words * 4) < 0 ||
                hangar_rle_decode(enc, hp.words, (uint32_t *)page,
                                  HANGAR_PAGE_WORDS) < 0) {
                rb_close(rb);
                return -1;
            }
            sum = hangar_fnv(sum, &hp, sizeof(hp));
            sum = hangar_fnv(sum, enc, (size_t)hp.words * 4);
            pte = vmm_get_pte(current->pml4, hp.va, 0);
            if ((pte && *pte) || ++pages > m->npages) {
                rb_close(rb);
                return -1;
            }
            phys = pmm_alloc();
            if (!phys) {
                rb_close(rb);
                return -1;
            }
            memcpy(P2V(phys), page, PAGE_SIZE);
            vmm_map_page(current->pml4, hp.va, phys,
                         PTE_U | ((uint64_t)hp.pte_flags & PTE_W));
        }
        rb_close(rb);
    }
    return (pages == m->npages && sum == m->mem_sum) ? 0 : -1;
}

static int restore_window(struct resume_pkg *pkg, int i, uint32_t *row,
                          struct rbuf *rb, int *slot)
{
    struct hangar_win *hw = &pkg->m.wins[i];
    uint32_t *pix;
    uint32_t sum = HANGAR_FNV_INIT;
    char path[40];

    pix = wm_hangar_restore(hw, slot);
    if (!pix)
        return -1;
    bay_path(path, pkg->bay, "win", i);
    if (rb_open(rb, path) < 0)
        return -1;
    for (int y = 0; y < hw->h; y++) {
        uint32_t words;
        uint32_t *dst = pix + (long)y * hw->w;

        if (rb_get(rb, &words, 4) < 0 ||
            words > hangar_rle_bound((size_t)hw->w) ||
            rb_get(rb, row, (size_t)words * 4) < 0 ||
            hangar_rle_decode(row, words, dst, (size_t)hw->w) < 0) {
            rb_close(rb);
            return -1;
        }
        sum = hangar_fnv(sum, dst, (size_t)hw->w * 4);
    }
    rb_close(rb);
    return sum == hw->pix_sum ? 0 : -1;
}

static void resume_thunk(void *arg)
{
    struct resume_pkg *pkg = arg;
    struct hangar_manifest *m = &pkg->m;
    struct hangar_regs regs;
    struct rbuf rb;
    uint8_t *page = NULL;
    uint32_t *enc = NULL;
    int slots[HANGAR_MAX_WINS];
    uint64_t *pml4, f;

    current->hangar_io = true;
    current->uid = m->uid;
    current->gid = m->gid;

    pml4 = vmm_new_pml4();
    f = irq_save();
    current->pml4 = pml4;
    current->user = true;
    vmm_switch(pml4);
    irq_restore(f);

    for (uint32_t i = 0; i < m->narea; i++) {
        struct vm_area *a = &current->vm_areas[i];
        memset(a, 0, sizeof(*a));
        a->start = m->areas[i].start;
        a->end = m->areas[i].end;
        a->pte_flags = m->areas[i].pte_flags & PTE_W;
    }
    current->vm_area_count = (int)m->narea;
    current->user_heap_start = m->heap_start;
    current->user_brk = m->brk;

    page = kmalloc(PAGE_SIZE);
    enc = kmalloc(hangar_rle_bound(HANGAR_WIN_MAX_W) * 4 + PAGE_SIZE * 2);
    rb.buf = kmalloc(WBUF_BYTES);
    rb.f = NULL;
    if (!page || !enc || !rb.buf)
        resume_fail(pkg, "out of kernel memory");
    if (restore_memory(pkg, page, enc, &rb) < 0)
        resume_fail(pkg, "memory image is damaged");

    /* Files reopen with the app's own permissions. */
    current->hangar_io = false;
    for (uint32_t i = 0; i < m->nfile; i++) {
        const struct hangar_file *hf = &m->files[i];
        struct file *file = vfs_open(hf->path,
                                     hf->flags & ~(O_CREAT | O_TRUNC));
        if (!file)
            resume_fail(pkg, "an open file is gone");
        if (!stateless_device(hf->path) &&
            vfs_seek(file, (long)hf->pos, 0) < 0) {
            vfs_close(file);
            resume_fail(pkg, "an open file shrank");
        }
        current->files[hf->fd] = file;
    }
    current->hangar_io = true;

    for (uint32_t i = 0; i < m->nwin; i++)
        if (restore_window(pkg, (int)i, enc, &rb, &slots[i]) < 0)
            resume_fail(pkg, "window image is damaged");

    memcpy(current->sig_actions, m->sigact, sizeof(current->sig_actions));
    current->sig_mask = m->sig_mask & ~((1ULL << (SIGKILL - 1)) |
                                        (1ULL << (SIGSTOP - 1)));
    current->cap_mask = m->cap_mask & pkg->caps;
    strncpy(current->unveil_prefix, m->unveil,
            sizeof(current->unveil_prefix) - 1);
    current->unveil_prefix[sizeof(current->unveil_prefix) - 1] = '\0';

    bay_remove(pkg->bay);
    current->hangar_io = false;
    bay_release(pkg->bay);

    for (uint32_t i = 0; i < m->nwin; i++)
        wm_hangar_show(slots[i], m->wins[i].minimized);

    flight_record(FLIGHT_CAT_PROC, "resume", (int16_t)current->pid,
                  (uint64_t)pkg->bay, (uint64_t)m->orig_pid, m->name);
    kprintf("hangar: resumed %s from bay %d as pid %d\n", m->name,
            pkg->bay, current->pid);

    regs = m->regs;
    regs.rflags = (regs.rflags & 0x240FD5ULL) | 0x202ULL;
    kfree(page);
    kfree(enc);
    kfree(rb.buf);

    f = irq_save();
    memcpy(current->fpu_state, m->fpu, HANGAR_FPU_BYTES);
    kfree(pkg);
    fpu_restore(current->fpu_state);
    hangar_enter_user(&regs);
    (void)f;
}

static long op_resume(int bay, char *reason)
{
    struct resume_pkg *pkg;
    const char *why;
    uint64_t need;
    int pid;
    bool was;

    if (bay < 0 || bay >= K_HANGAR_BAYS) {
        reason3(reason, "No such Hangar bay", NULL, NULL);
        return -1;
    }
    if (!fb_present()) {
        reason3(reason, "Resuming needs the graphical desktop", NULL, NULL);
        return -1;
    }
    pkg = kzalloc(sizeof(*pkg));
    if (!pkg) {
        reason3(reason, "Not enough kernel memory to resume", NULL, NULL);
        return -1;
    }
    pkg->bay = bay;
    pkg->caps = current->cap_mask;
    if (!bay_claim(bay)) {
        kfree(pkg);
        reason3(reason, "That app is already being resumed", NULL, NULL);
        return -1;
    }
    was = io_begin();
    if (load_manifest(bay, &pkg->m, &why) < 0) {
        io_end(was);
        reason3(reason, "Cannot resume: ", why, NULL);
        goto fail;
    }
    io_end(was);
    if (current->uid != 0 && current->uid != pkg->m.uid) {
        reason3(reason, "That parked app belongs to another user", NULL, NULL);
        goto fail;
    }
    for (uint32_t i = 0; i < pkg->m.nfile; i++) {
        struct k_stat st;
        if (stateless_device(pkg->m.files[i].path))
            continue;
        if (vfs_stat(pkg->m.files[i].path, &st) < 0 ||
            st.size < pkg->m.files[i].pos) {
            reason3(reason, pkg->m.name, " needs ", pkg->m.files[i].path);
            rs_cat(reason, ", which is gone or shorter now");
            goto fail;
        }
    }
    need = pkg->m.npages + 512;
    for (uint32_t i = 0; i < pkg->m.nwin; i++)
        need += ((uint64_t)pkg->m.wins[i].w * pkg->m.wins[i].h * 4 +
                 PAGE_SIZE - 1) / PAGE_SIZE;
    if (pmm_free_pages() < need) {
        reason3(reason, "Not enough free memory to resume ", pkg->m.name,
                NULL);
        goto fail;
    }
    if (kthread_create_with_pid(resume_thunk, pkg, pkg->m.name, &pid) == NULL) {
        reason3(reason, "Could not create a process for ", pkg->m.name, NULL);
        goto fail;
    }
    return pid;
fail:
    bay_release(bay);
    kfree(pkg);
    return -1;
}

/* ------------------------------------------------------------- listing */

static int bay_visible(int bay, struct hangar_manifest *m)
{
    const char *why;

    if (load_manifest(bay, m, &why) < 0)
        return 0;
    return current->uid == 0 || current->uid == m->uid;
}

static uint64_t bay_bytes(int bay)
{
    char path[40];
    struct k_stat st;
    uint64_t total = 0;

    bay_path(path, bay, "manifest", -1);
    if (vfs_stat(path, &st) == 0)
        total += st.size;
    bay_path(path, bay, "thumb", -1);
    if (vfs_stat(path, &st) == 0)
        total += st.size;
    for (int i = 0; i < (int)HANGAR_MAX_CHUNKS; i++) {
        bay_path(path, bay, "mem", i);
        if (vfs_stat(path, &st) < 0)
            break;
        total += st.size;
    }
    for (int i = 0; i < HANGAR_MAX_WINS; i++) {
        bay_path(path, bay, "win", i);
        if (vfs_stat(path, &st) == 0)
            total += st.size;
    }
    return total;
}

static long op_list(uint64_t index, uint64_t uinfo)
{
    struct hangar_manifest *m = kmalloc(sizeof(*m));
    struct k_hangar_info info;
    uint64_t seen = 0;
    long rc = -1;
    bool was;

    if (!m)
        return -1;
    was = io_begin();
    for (int bay = 0; bay < K_HANGAR_BAYS; bay++) {
        if (!bay_visible(bay, m) || seen++ != index)
            continue;
        memset(&info, 0, sizeof(info));
        info.bay = (uint32_t)bay;
        info.uid = m->uid;
        info.parked_at = m->parked_at;
        info.pages = m->npages;
        info.bytes = bay_bytes(bay);
        info.orig_pid = m->orig_pid;
        info.nwins = m->nwin;
        info.nfiles = m->nfile;
        if (m->nwin) {
            info.x = m->wins[0].x;
            info.y = m->wins[0].y;
            info.width = (uint32_t)m->wins[0].w;
            info.height = (uint32_t)m->wins[0].h;
            memcpy(info.title, m->wins[0].title, sizeof(info.title));
        }
        memcpy(info.name, m->name, sizeof(info.name));
        rc = 0;
        break;
    }
    io_end(was);
    kfree(m);
    if (rc == 0 && copy_to_user((void *)uinfo, &info, sizeof(info)) < 0)
        rc = -1;
    return rc;
}

static long op_thumb(int bay, uint64_t ubuf)
{
    size_t bytes = (size_t)K_HANGAR_THUMB_W * K_HANGAR_THUMB_H * 4;
    struct hangar_manifest *m = kmalloc(sizeof(*m));
    uint32_t *thumb = kmalloc(bytes);
    char path[40];
    long rc = -1;
    bool was;

    if (m && thumb && bay >= 0 && bay < K_HANGAR_BAYS &&
        user_range_ok((const void *)ubuf, bytes)) {
        was = io_begin();
        bay_path(path, bay, "thumb", -1);
        if (bay_visible(bay, m) && m->thumb_w == K_HANGAR_THUMB_W &&
            m->thumb_h == K_HANGAR_THUMB_H &&
            read_file(path, thumb, bytes) == 0 &&
            hangar_fnv(HANGAR_FNV_INIT, thumb, bytes) == m->thumb_sum)
            rc = 0;
        io_end(was);
        if (rc == 0 && copy_to_user((void *)ubuf, thumb, bytes) < 0)
            rc = -1;
    }
    kfree(m);
    kfree(thumb);
    return rc;
}

static long op_discard(int bay)
{
    struct hangar_manifest *m;
    long rc = -1;
    bool was;

    if (bay < 0 || bay >= K_HANGAR_BAYS || !bay_claim(bay))
        return -1;
    m = kmalloc(sizeof(*m));
    if (m) {
        was = io_begin();
        if (bay_visible(bay, m)) {
            bay_remove(bay);
            flight_record(FLIGHT_CAT_PROC, "discard", (int16_t)current->pid,
                          (uint64_t)bay, 0, m->name);
            rc = 0;
        }
        io_end(was);
        kfree(m);
    }
    bay_release(bay);
    return rc;
}

long sys_hangar(uint64_t op, uint64_t a, uint64_t b, uint64_t c)
{
    char reason[REASON_MAX];
    long rc;

    (void)c;
    if (!current)
        return -1;
    reason[0] = '\0';
    switch (op) {
    case K_HANGAR_PARK:
        rc = op_park((int)a, reason);
        break;
    case K_HANGAR_LIST:
        return op_list(a, b);
    case K_HANGAR_THUMB:
        return op_thumb((int)a, b);
    case K_HANGAR_RESUME:
        rc = op_resume((int)a, reason);
        break;
    case K_HANGAR_DISCARD:
        return op_discard((int)a);
    default:
        return -1;
    }
    if (rc < 0 && b) {
        if (!reason[0])
            reason3(reason, "The Hangar could not complete that", NULL, NULL);
        copy_to_user((void *)b, reason, strlen(reason) + 1);
    }
    return rc;
}

/* ---------------------------------------------------------------- boot */

void hangar_init(void)
{
    struct hangar_manifest *m;
    struct k_stat st;
    int kept = 0;

    if (vfs_stat("/hangar", &st) < 0 && vfs_mkdir("/hangar") < 0) {
        kprintf("hangar: cannot create /hangar, parking disabled\n");
        return;
    }
    vfs_chmod("/hangar", 0700);

    /* A bay without a valid manifest is a park the machine lost power in
     * the middle of: the app had not exited yet, so drop the debris. */
    m = kmalloc(sizeof(*m));
    if (!m)
        return;
    for (int bay = 0; bay < K_HANGAR_BAYS; bay++) {
        const char *why;
        if (!bay_exists(bay))
            continue;
        if (load_manifest(bay, m, &why) < 0) {
            kprintf("hangar: clearing bay %d (%s)\n", bay, why);
            bay_remove(bay);
        } else {
            kept++;
        }
    }
    kfree(m);
    if (kept)
        kprintf("hangar: %d parked app%s waiting\n", kept, kept == 1 ? "" : "s");
}
