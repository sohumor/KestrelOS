#pragma once

#include <stddef.h>

struct k_mem_chunk;

typedef struct {
    size_t remaining;
} k_mem_budget;

typedef struct {
    k_mem_budget *budget;
    struct k_mem_chunk *chunks;
} k_mem_scope;

/* The budget must outlive its scopes. Do not copy or reinitialize an
 * initialized budget or scope while it holds allocations. Serialize
 * operations on scopes that share a budget. */
void k_mem_budget_init(k_mem_budget *budget, size_t bytes);
size_t k_mem_budget_available(const k_mem_budget *budget);
void k_mem_scope_init(k_mem_scope *scope, k_mem_budget *budget);

/* Returns uninitialized, max_align_t-aligned storage. Zero sizes or
 * failed requests return NULL without changing the scope or budget. */
void *k_mem_scope_alloc(k_mem_scope *scope, size_t bytes);

/* Invalidates this scope's allocations and returns its backing credit.
 * The scope can be reused with the same budget after reset. */
void k_mem_scope_reset(k_mem_scope *scope);
