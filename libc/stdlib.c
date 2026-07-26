/* KestrelOS libc: heap, conversions, PRNG, process exit.
 *
 * malloc is a brk-based free-list allocator: a singly linked list of
 * contiguous blocks in address order, each preceded by a 32-byte
 * header (payload stays 16-byte aligned). Growth happens through
 * SYS_BRK in 16 KiB steps. Allocation is first-fit with splitting;
 * free() coalesces adjacent free blocks.
 */

#include <stdlib.h>
#include <string.h>
#include <kestrel.h>

#define HEAP_ALIGN     16UL
#define HEAP_GROW_STEP (16UL * 1024UL)

struct heap_block {
    unsigned long size;         /* payload bytes, multiple of 16 */
    unsigned long free;         /* 1 = available                 */
    struct heap_block *next;    /* next block in address order   */
    unsigned long _pad;         /* keep header 32 bytes           */
};

#define HEADER_SIZE   (sizeof(struct heap_block))
#define MIN_SPLIT     (HEADER_SIZE + HEAP_ALIGN)

static struct heap_block *heap_head;    /* first block, address order */
static unsigned long heap_break;        /* current program break      */

static unsigned long align_up(unsigned long v, unsigned long a)
{
    return (v + a - 1) & ~(a - 1);
}

/* Grow the heap enough to hold one more block of `payload` bytes.
 * Returns the fresh (free) block, or 0 on failure. */
static struct heap_block *heap_grow(unsigned long payload)
{
    unsigned long need = align_up(HEADER_SIZE + payload, HEAP_GROW_STEP);
    unsigned long old_brk, new_brk;
    struct heap_block *blk;

    if (heap_break == 0) {
        heap_break = (unsigned long)brk_(0);
        if (heap_break == 0)
            return 0;
        heap_break = align_up(heap_break, HEAP_ALIGN);
    }

    old_brk = heap_break;
    new_brk = (unsigned long)brk_((void *)(old_brk + need));
    if (new_brk < old_brk + need)
        return 0;
    heap_break = new_brk;

    blk = (struct heap_block *)old_brk;
    blk->size = (new_brk - old_brk) - HEADER_SIZE;
    blk->free = 1;
    blk->next = 0;
    blk->_pad = 0;

    if (heap_head == 0) {
        heap_head = blk;
    } else {
        struct heap_block *tail = heap_head;
        while (tail->next)
            tail = tail->next;
        tail->next = blk;
        /* merge with a free tail block that ends at old_brk */
        if (tail->free &&
            (unsigned long)tail + HEADER_SIZE + tail->size == old_brk) {
            tail->size += HEADER_SIZE + blk->size;
            tail->next = 0;
            blk = tail;
        }
    }
    return blk;
}

/* Split `blk` if the remainder can hold a header plus 16 bytes. */
static void block_split(struct heap_block *blk, unsigned long size)
{
    if (blk->size >= size + MIN_SPLIT) {
        struct heap_block *rest =
            (struct heap_block *)((char *)blk + HEADER_SIZE + size);
        rest->size = blk->size - size - HEADER_SIZE;
        rest->free = 1;
        rest->next = blk->next;
        rest->_pad = 0;
        blk->size = size;
        blk->next = rest;
    }
}

static void heap_coalesce(void)
{
    struct heap_block *blk = heap_head;

    while (blk && blk->next) {
        if (blk->free && blk->next->free &&
            (unsigned long)blk + HEADER_SIZE + blk->size ==
                (unsigned long)blk->next) {
            blk->size += HEADER_SIZE + blk->next->size;
            blk->next = blk->next->next;
        } else {
            blk = blk->next;
        }
    }
}

void *malloc(unsigned long size)
{
    struct heap_block *blk;
    unsigned long want;

    if (size == 0)
        return 0;
    want = align_up(size, HEAP_ALIGN);

    for (blk = heap_head; blk; blk = blk->next) {
        if (blk->free && blk->size >= want) {
            block_split(blk, want);
            blk->free = 0;
            return (char *)blk + HEADER_SIZE;
        }
    }

    blk = heap_grow(want);
    if (blk == 0)
        return 0;
    block_split(blk, want);
    blk->free = 0;
    return (char *)blk + HEADER_SIZE;
}

void free(void *ptr)
{
    struct heap_block *blk;

    if (ptr == 0)
        return;
    blk = (struct heap_block *)((char *)ptr - HEADER_SIZE);
    blk->free = 1;
    heap_coalesce();
}

void *calloc(unsigned long n, unsigned long size)
{
    unsigned long total;
    void *p;

    if (n != 0 && size > (unsigned long)-1 / n)
        return 0;
    total = n * size;
    p = malloc(total ? total : 1);
    if (p)
        memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, unsigned long size)
{
    struct heap_block *blk;
    void *np;
    unsigned long copy;

    if (ptr == 0)
        return malloc(size);
    if (size == 0) {
        free(ptr);
        return 0;
    }

    blk = (struct heap_block *)((char *)ptr - HEADER_SIZE);
    if (blk->size >= size)
        return ptr;

    np = malloc(size);
    if (np == 0)
        return 0;
    copy = blk->size < size ? blk->size : size;
    memcpy(np, ptr, copy);
    free(ptr);
    return np;
}

/* ---- conversions ---- */

static long parse_long(const char *s)
{
    long v = 0;
    int neg = 0;

    while (*s == ' ' || *s == '\t' || *s == '\n' ||
           *s == '\r' || *s == '\f' || *s == '\v')
        s++;
    if (*s == '+') {
        s++;
    } else if (*s == '-') {
        neg = 1;
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return neg ? -v : v;
}

int atoi(const char *s)
{
    return (int)parse_long(s);
}

long atol(const char *s)
{
    return parse_long(s);
}

int abs(int v)
{
    return v < 0 ? -v : v;
}

/* ---- PRNG: xorshift64 (not cryptographic) ---- */

#define RAND_STATE_INIT 88172645463325252ULL

static unsigned long long rand_state = RAND_STATE_INIT;

int rand(void)
{
    unsigned long long x = rand_state;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rand_state = x;
    return (int)((x >> 33) & 0x7fffffffULL);
}

void srand(unsigned int seed)
{
    /* mix the seed into the base state; keep it nonzero */
    rand_state = RAND_STATE_INIT ^
                 ((unsigned long long)seed * 0x9E3779B97F4A7C15ULL);
    if (rand_state == 0)
        rand_state = RAND_STATE_INIT;
}

/* ---- process exit ---- */

void exit(int code)
{
    syscall(SYS_EXIT, code, 0, 0, 0);
    for (;;)
        ;   /* SYS_EXIT does not return */
}
