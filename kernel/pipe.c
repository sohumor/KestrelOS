#include "kernel.h"
#include "pipe.h"
#include "vfs.h"
#include "kheap.h"
#include "proc.h"
#include "string.h"

/* Anonymous pipes.
 *
 * The ring buffer and the two reference counts are plain memory touched by
 * tasks on different CPUs, so every inspection or update of head/tail/
 * count/readers/writers runs under the pipe's IRQ-safe spin lock. The
 * critical sections only contain a bounded copy; blocking is always done
 * with the lock dropped, by sleeping a tick and re-checking. None of this
 * runs in IRQ context, so kmalloc() here is fine.
 *
 * Writing to a pipe with no readers currently returns an error; SIGPIPE is
 * not yet wired into this VFS operation. */

int pipe_create(struct file **read_end, struct file **write_end)
{
    struct pipe *p;
    struct file *r, *w;

    if (read_end == NULL || write_end == NULL)
        return -1;

    p = kzalloc(sizeof(struct pipe));
    r = kzalloc(sizeof(struct file));
    w = kzalloc(sizeof(struct file));
    if (p == NULL || r == NULL || w == NULL)
        goto fail;

    p->buf = kmalloc(PIPE_BUF_SIZE);
    if (p->buf == NULL)
        goto fail;
    p->readers = 1;
    p->writers = 1;

    r->type = FILE_PIPE;
    r->pipe = p;
    r->writable = 0;
    r->flags = O_RDONLY;
    r->refs = 1;

    w->type = FILE_PIPE;
    w->pipe = p;
    w->writable = 1;
    w->flags = O_WRONLY;
    w->refs = 1;

    *read_end = r;
    *write_end = w;
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

long pipe_read(struct file *f, void *buf, unsigned long n)
{
    struct pipe *p;
    unsigned long got;
    uint64_t fl;

    if (f == NULL || f->type != FILE_PIPE || f->pipe == NULL || buf == NULL)
        return -1;
    if (f->writable)
        return -1;              /* wrong end */
    p = f->pipe;
    if (n == 0)
        return 0;

    for (;;) {
        fl = spin_lock_irqsave(&p->lock);
        if (p->count > 0) {
            got = ring_take(p, (uint8_t *)buf, n);
            spin_unlock_irqrestore(&p->lock, fl);
            return (long)got;
        }
        if (p->writers <= 0) {
            spin_unlock_irqrestore(&p->lock, fl);
            return 0;           /* EOF: last writer closed */
        }
        spin_unlock_irqrestore(&p->lock, fl);
        /* Empty but still writable: wait. task_sleep_ticks() is safe here
         * because the pipe holds no lock of its own. */
        task_sleep_ticks(1);
    }
}

long pipe_write(struct file *f, const void *buf, unsigned long n)
{
    struct pipe *p;
    unsigned long done = 0;
    uint64_t fl;

    if (f == NULL || f->type != FILE_PIPE || f->pipe == NULL || buf == NULL)
        return -1;
    if (!f->writable)
        return -1;              /* wrong end */
    p = f->pipe;
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
            done += ring_put(p, (const uint8_t *)buf + done, n - done);
            spin_unlock_irqrestore(&p->lock, fl);
            continue;
        }
        spin_unlock_irqrestore(&p->lock, fl);
        task_sleep_ticks(1);    /* full: wait for the reader to drain it */
    }
    return (long)done;
}

void pipe_close(struct file *f)
{
    struct pipe *p;
    int dead = 0;
    uint64_t fl;

    if (f == NULL || f->type != FILE_PIPE || f->pipe == NULL)
        return;
    p = f->pipe;

    fl = spin_lock_irqsave(&p->lock);
    if (f->writable) {
        if (p->writers > 0)
            p->writers--;
    } else {
        if (p->readers > 0)
            p->readers--;
    }
    if (p->readers <= 0 && p->writers <= 0)
        dead = 1;
    f->pipe = NULL;
    spin_unlock_irqrestore(&p->lock, fl);

    if (dead) {
        /* Both ends are gone, so no other task can still reach *p. */
        kfree(p->buf);
        kfree(p);
    }
}
