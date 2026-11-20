#pragma once

#include <stdint.h>
#include "spinlock.h"

/* Anonymous pipes. A pipe is a 4 KiB ring buffer shared by two struct
 * file ends (see vfs.h): one read end and one write end, each of which
 * may itself be shared by several fds through dup2 or spawn. */

#define PIPE_BUF_SIZE 4096

struct file;

struct pipe {
    spinlock_t lock;
    uint8_t *buf;               /* PIPE_BUF_SIZE bytes */
    uint32_t head;              /* next byte to read */
    uint32_t tail;              /* next byte to write */
    uint32_t count;             /* bytes currently buffered */
    int readers;                /* open read ends */
    int writers;                /* open write ends */
};

/* Create a pipe and its two ends, each with refs = 1. Returns 0, or -1
 * (nothing allocated) on failure. Close each end with vfs_close(). */
int pipe_create(struct file **read_end, struct file **write_end);

/* Read/write a pipe end. Called by the VFS; buf must be a kernel buffer.
 *   read:  blocks while empty and a writer exists (if blocking != 0),
 *          0 at EOF (or when empty and blocking == 0), -1 on misuse/kill
 *   write: blocks while full and a reader exists, -1 when the readers are
 *          all gone before anything was written or on kill */
long pipe_read(struct file *f, void *buf, unsigned long n, int blocking);
long pipe_write(struct file *f, const void *buf, unsigned long n);

/* Drop this end. Frees the ring buffer once both sides are gone. Does not
 * free the struct file itself; vfs_close() owns that. */
void pipe_close(struct file *f);
