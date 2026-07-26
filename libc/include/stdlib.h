#pragma once

/* KestrelOS libc: heap, conversions, process exit (freestanding). */

void *malloc(unsigned long size);
void free(void *ptr);
void *calloc(unsigned long n, unsigned long size);
void *realloc(void *ptr, unsigned long size);

int atoi(const char *s);
long atol(const char *s);

__attribute__((noreturn)) void exit(int code);

int abs(int v);

/* Simple LCG PRNG; not cryptographic. */
#define RAND_MAX 0x7fffffff
int rand(void);
void srand(unsigned int seed);
