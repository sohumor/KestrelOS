#include <memscope.h>
#include <stdint.h>
#include <stdlib.h>

#define CHUNK_BYTES 4096

struct k_mem_chunk {
    struct k_mem_chunk *next;
    size_t bytes;
    size_t used;
    _Alignas(max_align_t) unsigned char payload[];
};

void k_mem_budget_init(k_mem_budget *budget, size_t bytes)
{
    budget->remaining = bytes;
}

size_t k_mem_budget_available(const k_mem_budget *budget)
{
    return budget->remaining;
}

void k_mem_scope_init(k_mem_scope *scope, k_mem_budget *budget)
{
    scope->budget = budget;
    scope->chunks = NULL;
}

void *k_mem_scope_alloc(k_mem_scope *scope, size_t bytes)
{
    struct k_mem_chunk *chunk = scope->chunks;
    size_t need;

    if (!bytes || bytes > SIZE_MAX - sizeof(*chunk))
        return NULL;
    if (chunk) {
        const size_t align = _Alignof(max_align_t);
        size_t pad = (align - chunk->used % align) % align;
        size_t free_bytes = chunk->bytes - sizeof(*chunk) - chunk->used;

        if (pad <= free_bytes && bytes <= free_bytes - pad) {
            void *result = chunk->payload + chunk->used + pad;
            chunk->used += pad + bytes;
            return result;
        }
    }

    need = sizeof(*chunk) + bytes;
    if (need > scope->budget->remaining)
        return NULL;
    if (need < CHUNK_BYTES && scope->budget->remaining >= CHUNK_BYTES)
        need = CHUNK_BYTES;
    chunk = malloc(need);
    if (!chunk)
        return NULL;
    chunk->next = scope->chunks;
    chunk->bytes = need;
    chunk->used = bytes;
    scope->chunks = chunk;
    scope->budget->remaining -= need;
    return chunk->payload;
}

void k_mem_scope_reset(k_mem_scope *scope)
{
    while (scope->chunks) {
        struct k_mem_chunk *chunk = scope->chunks;

        scope->chunks = chunk->next;
        scope->budget->remaining += chunk->bytes;
        free(chunk);
    }
}
