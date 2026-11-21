/* KestrelOS libc: string and memory routines, written from scratch. */

#include <string.h>
#include <stdlib.h>

void *memset(void *dst, int c, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;

    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    if (d == s || n == 0)
        return dst;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, unsigned long n)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;

    while (n--) {
        if (*pa != *pb)
            return (int)*pa - (int)*pb;
        pa++;
        pb++;
    }
    return 0;
}

void *memchr(const void *p, int c, unsigned long n)
{
    const unsigned char *b = (const unsigned char *)p;
    unsigned char ch = (unsigned char)c;

    while (n--) {
        if (*b == ch)
            return (void *)b;
        b++;
    }
    return 0;
}

void *memrchr(const void *p, int c, unsigned long n)
{
    const unsigned char *b = (const unsigned char *)p;
    unsigned char ch = (unsigned char)c;

    while (n > 0) {
        n--;
        if (b[n] == ch)
            return (void *)(b + n);
    }
    return 0;
}

unsigned long strlen(const char *s)
{
    const char *p = s;

    while (*p)
        p++;
    return (unsigned long)(p - s);
}

unsigned long strnlen(const char *s, unsigned long max)
{
    unsigned long n = 0;

    while (n < max && s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, unsigned long n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0)
        return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;

    while ((*d++ = *src++) != '\0')
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, unsigned long n)
{
    unsigned long i = 0;

    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = '\0';
    return dst;
}

char *strcat(char *dst, const char *src)
{
    char *d = dst;

    while (*d)
        d++;
    while ((*d++ = *src++) != '\0')
        ;
    return dst;
}

char *strncat(char *dst, const char *src, unsigned long n)
{
    char *d = dst;

    while (*d)
        d++;
    while (n-- && *src)
        *d++ = *src++;
    *d = '\0';      /* always NUL-terminate */
    return dst;
}

char *strchr(const char *s, int c)
{
    char ch = (char)c;

    for (;; s++) {
        if (*s == ch)
            return (char *)s;
        if (*s == '\0')
            return 0;
    }
}

char *strrchr(const char *s, int c)
{
    char ch = (char)c;
    const char *last = 0;

    for (;; s++) {
        if (*s == ch)
            last = s;
        if (*s == '\0')
            return (char *)last;
    }
}

unsigned long strspn(const char *s, const char *accept)
{
    unsigned long n = 0;

    while (s[n] && strchr(accept, s[n]))
        n++;
    return n;
}

unsigned long strcspn(const char *s, const char *reject)
{
    unsigned long n = 0;

    while (s[n] && !strchr(reject, s[n]))
        n++;
    return n;
}

char *strpbrk(const char *s, const char *accept)
{
    for (; *s; s++) {
        if (strchr(accept, *s))
            return (char *)s;
    }
    return 0;
}

char *strstr(const char *hay, const char *needle)
{
    unsigned long nlen = strlen(needle);

    if (nlen == 0)
        return (char *)hay;
    for (; *hay; hay++) {
        if (*hay == *needle && strncmp(hay, needle, nlen) == 0)
            return (char *)hay;
    }
    return 0;
}

char *strtok(char *s, const char *delim)
{
    static char *save;
    char *tok;

    if (s == 0)
        s = save;
    if (s == 0)
        return 0;

    s += strspn(s, delim);
    if (*s == '\0') {
        save = s;
        return 0;
    }
    tok = s;
    s += strcspn(s, delim);
    if (*s != '\0')
        *s++ = '\0';
    save = s;
    return tok;
}

char *strdup(const char *s)
{
    unsigned long len = strlen(s);
    char *p = (char *)malloc(len + 1);

    if (p)
        memcpy(p, s, len + 1);
    return p;
}

char *strndup(const char *s, unsigned long n)
{
    unsigned long len = strnlen(s, n);
    char *p = (char *)malloc(len + 1);

    if (p) {
        memcpy(p, s, len);
        p[len] = '\0';
    }
    return p;
}
