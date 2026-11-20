#include "kernel.h"
#include "vfs.h"
#include "mount.h"
#include "kheap.h"
#include "proc.h"
#include "spinlock.h"
#include "string.h"

/* Anonymous pipes.
 *
 * The ring buffer and the two reference counts are plain memory touched by
 * tasks on different CPUs, so every inspection or update of head/tail/
 * count/readers/writers runs under the pipe's IRQ-safe spin lock. The
 * blocked reader or writer parks atomically with releasing that lock and
 * rechecks the ring and peer state on wake. None of this runs in IRQ
 * context, so kmalloc() here is fine.
 *
 * Writing to a pipe with no readers currently returns an error; SIGPIPE is
 * not yet wired into this VFS operation. */

#define PIPE_BUF_SIZE 4096
#define PIPE_READABLE 1U
#define PIPE_WRITABLE 2U

struct pipe {
    spinlock_t lock;
    struct task_event_channel events;
    uint8_t *buf;               /* PIPE_BUF_SIZE bytes */
    uint32_t head;              /* next byte to read */
    uint32_t tail;              /* next byte to write */
    uint32_t count;             /* bytes currently buffered */
    int readers;                /* open read ends */
    int writers;                /* open write ends */
};

struct pipe_end {
    struct file f;
    struct pipe *pipe;
    int writable;
};

/* Copy out of the ring. Caller holds interrupts off and has checked that
 * there is something to copy. */
static unsigned long ring_take(struct pipe *p, uint8_t *dst, unsigned long n)
{
    unsigned long done = 0;

    while (done < n && p->count > 0) {
        uint32_t run = PIPE_BUF_SIZE - p->head;   /* to the wrap point */
        if (run > p->count)
            run = p->count;
        if (run > n - done)
            run = (uint32_t)(n - done);
        memcpy(dst + done, p->buf + p->head, run);
        p->head = (p->head + run) % PIPE_BUF_SIZE;
        p->count -= run;
        done += run;
    }
    return done;
}

/* Copy into the ring. Same contract as ring_take(). */
static unsigned long ring_put(struct pipe *p, const uint8_t *src,
                              unsigned long n)
{
    unsigned long done = 0;

    while (done < n && p->count < PIPE_BUF_SIZE) {
        uint32_t space = PIPE_BUF_SIZE - p->count;
        uint32_t run = PIPE_BUF_SIZE - p->tail;
        if (run > space)
            run = space;
        if (run > n - done)
            run = (uint32_t)(n - done);
        memcpy(p->buf + p->tail, src + done, run);
        p->tail = (p->tail + run) % PIPE_BUF_SIZE;
        p->count += run;
        done += run;
    }
    return done;
}

static long pipe_read(struct file *f, void *buf, unsigned long n, int blocking)
{
    struct pipe_end *end = (struct pipe_end *)f;
    struct pipe *p = end->pipe;
    unsigned long got;
    uint64_t fl;

    if (end->writable)
        return -1;              /* wrong end */
    if (n == 0)
        return 0;

    for (;;) {
        fl = spin_lock_irqsave(&p->lock);
        if (p->count > 0) {
            bool was_full = p->count == PIPE_BUF_SIZE;
            got = ring_take(p, (uint8_t *)buf, n);
            if (was_full)
                task_event_notify(&p->events, PIPE_WRITABLE);
            spin_unlock_irqrestore(&p->lock, fl);
            return (long)got;
        }
        if (p->writers <= 0 || !blocking) {
            spin_unlock_irqrestore(&p->lock, fl);
            return 0;           /* EOF or non-blocking empty */
        }
        if (task_event_wait_interruptible_locked(&p->events, PIPE_READABLE,
                                                 &p->lock, fl) < 0)
            return -1;
    }
}

static long pipe_write(struct file *f, const void *buf, unsigned long n)
{
    struct pipe_end *end = (struct pipe_end *)f;
    struct pipe *p = end->pipe;
    unsigned long done = 0;
    uint64_t fl;

    if (!end->writable)
        return -1;              /* wrong end */
    if (n == 0)
        return 0;

    while (done < n) {
        fl = spin_lock_irqsave(&p->lock);
        if (p->readers <= 0) {
            spin_unlock_irqrestore(&p->lock, fl);
            /* No signals here, so a broken pipe is just an error. Bytes
             * already handed over still count. */
            return done ? (long)done : -1;
        }
        if (p->count < PIPE_BUF_SIZE) {
            bool was_empty = p->count == 0;
            done += ring_put(p, (const uint8_t *)buf + done, n - done);
            if (was_empty)
                task_event_notify(&p->events, PIPE_READABLE);
            spin_unlock_irqrestore(&p->lock, fl);
            continue;
        }
        if (task_event_wait_interruptible_locked(&p->events, PIPE_WRITABLE,
                                                 &p->lock, fl) < 0)
            return done ? (long)done : -1;
    }
    return (long)done;
}

static void pipe_close(struct file *f)
{
    struct pipe_end *end = (struct pipe_end *)f;
    struct pipe *p = end->pipe;
    int dead = 0;
    uint64_t fl;

    fl = spin_lock_irqsave(&p->lock);
    if (end->writable) {
        if (p->writers > 0 && --p->writers == 0)
            task_event_notify(&p->events, PIPE_READABLE);
    } else {
        if (p->readers > 0 && --p->readers == 0)
            task_event_notify(&p->events, PIPE_WRITABLE);
    }
    if (p->readers <= 0 && p->writers <= 0)
        dead = 1;
    spin_unlock_irqrestore(&p->lock, fl);

    if (dead) {
        /* Both ends are gone, so no other task can still reach *p. */
        kfree(p->buf);
        kfree(p);
    }
    kfree(end);
}

static const struct fs_ops pipe_ops = {
    .read = pipe_read,
    .read_once = 1,
    .write = pipe_write,
    .close = pipe_close,
};

int vfs_pipe(struct file **read_end, struct file **write_end)
{
    struct pipe *p;
    struct pipe_end *r, *w;

    if (read_end == NULL || write_end == NULL)
        return -1;

    p = kzalloc(sizeof(*p));
    r = kzalloc(sizeof(*r));
    w = kzalloc(sizeof(*w));
    if (p == NULL || r == NULL || w == NULL)
        goto fail;

    p->buf = kmalloc(PIPE_BUF_SIZE);
    if (p->buf == NULL)
        goto fail;
    p->readers = 1;
    p->writers = 1;

    r->pipe = p;
    r->f.ops = &pipe_ops;
    r->f.flags = O_RDONLY;
    r->f.refs = 1;

    w->pipe = p;
    w->writable = 1;
    w->f.ops = &pipe_ops;
    w->f.flags = O_WRONLY;
    w->f.refs = 1;

    *read_end = &r->f;
    *write_end = &w->f;
    return 0;

fail:
    if (p)
        kfree(p);
    if (r)
        kfree(r);
    if (w)
        kfree(w);
    return -1;
}
