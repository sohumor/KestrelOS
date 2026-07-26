#pragma once

/* KestrelOS libc: string and memory routines (freestanding). */

void *memset(void *dst, int c, unsigned long n);
void *memcpy(void *dst, const void *src, unsigned long n);
void *memmove(void *dst, const void *src, unsigned long n);
int memcmp(const void *a, const void *b, unsigned long n);

unsigned long strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, unsigned long n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, unsigned long n);
char *strcat(char *dst, const char *src);
char *strncat(char *dst, const char *src, unsigned long n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *hay, const char *needle);
char *strdup(const char *s);
