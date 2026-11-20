#include <memscope.h>
#include <stdint.h>
#include <stdio.h>

static int check_scopes(void)
{
    const size_t total = 12288;
    k_mem_budget budget;
    k_mem_scope doc, parser;
    unsigned char *p, *q;
    size_t available;

    k_mem_budget_init(&budget, total);
    k_mem_scope_init(&doc, &budget);
    k_mem_scope_init(&parser, &budget);
    p = k_mem_scope_alloc(&doc, 5000);
    q = k_mem_scope_alloc(&parser, 1);
    if (!p || !q || (uintptr_t)p % _Alignof(max_align_t))
        return -1;
    p[0] = 17;
    p[4999] = 23;
    *q = 31;
    available = k_mem_budget_available(&budget);
    if (available >= total - 5001 ||
        k_mem_scope_alloc(&doc, 5000) ||
        k_mem_scope_alloc(&doc, SIZE_MAX) ||
        k_mem_scope_alloc(&doc, 0) ||
        k_mem_budget_available(&budget) != available || *q != 31)
        return -1;

    unsigned char *r = k_mem_scope_alloc(&parser, 1);
    if (!r || r == q || (uintptr_t)r % _Alignof(max_align_t) ||
        k_mem_budget_available(&budget) != available)
        return -1;
    *r = 41;
    if (*q != 31)
        return -1;

    k_mem_scope_reset(&parser);
    if (p[0] != 17 || p[4999] != 23 ||
        !k_mem_scope_alloc(&doc, 5000))
        return -1;
    k_mem_scope_reset(&doc);
    k_mem_scope_reset(&doc);
    if (k_mem_budget_available(&budget) != total)
        return -1;

    k_mem_budget_init(&budget, 256);
    if (!k_mem_scope_alloc(&parser, 1))
        return -1;
    size_t one = 256 - k_mem_budget_available(&budget);
    if (one <= 1 || one >= 256)
        return -1;
    k_mem_scope_reset(&parser);

    k_mem_budget_init(&budget, one);
    if (!k_mem_scope_alloc(&parser, 1) ||
        k_mem_budget_available(&budget) != 0 ||
        k_mem_scope_alloc(&doc, 1))
        return -1;
    k_mem_scope_reset(&parser);
    if (!k_mem_scope_alloc(&doc, 1))
        return -1;
    k_mem_scope_reset(&doc);
    return k_mem_budget_available(&budget) == one ? 0 : -1;
}

int main(void)
{
    if (check_scopes() != 0) {
        puts("FAIL libc memory scopes");
        return 1;
    }
    puts("PASS libc memory scopes: aligned allocations, shared cap, independent reset and credit reuse");
    return 0;
}
