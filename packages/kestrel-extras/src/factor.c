/* factor.c - print the prime factorisation of each argument.
 *
 * usage: factor <number>...
 *        factor            read numbers from stdin, one per line
 *
 * Trial division by 2, 3 and then the 6k+-1 wheel, which is plenty for
 * the 64-bit range once the small factors are stripped: the loop stops
 * at sqrt(n), so the worst case is a semiprime of two ~32-bit primes.
 */

#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Parse an unsigned decimal; returns 0 on anything that is not one. */
static int parse_u64(const char *s, unsigned long long *out)
{
    unsigned long long v = 0;
    int digits = 0;

    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '+')
        s++;
    while (*s >= '0' && *s <= '9') {
        unsigned d = (unsigned)(*s - '0');
        if (v > (~0ULL - d) / 10)
            return 0;                 /* overflows 64 bits */
        v = v * 10 + d;
        digits++;
        s++;
    }
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    if (!digits || *s != '\0')
        return 0;
    *out = v;
    return 1;
}

static void emit(unsigned long long f, int *first)
{
    if (*first)
        *first = 0;
    else
        printf(" ");
    printf("%llu", f);
}

static void factor(unsigned long long n)
{
    unsigned long long d;
    int first = 1;

    printf("%llu:", n);
    if (n < 2) {
        /* 0 and 1 have no factorisation; print the bare label. */
        printf("\n");
        return;
    }
    printf(" ");

    while (n % 2 == 0) {
        emit(2, &first);
        n /= 2;
    }
    while (n % 3 == 0) {
        emit(3, &first);
        n /= 3;
    }
    for (d = 5; d <= n / d; d += 6) {
        while (n % d == 0) {
            emit(d, &first);
            n /= d;
        }
        while (n % (d + 2) == 0) {
            emit(d + 2, &first);
            n /= d + 2;
        }
    }
    if (n > 1)
        emit(n, &first);
    printf("\n");
}

/* Pull the shell-injected "--cwd=<path>" argument, wherever it sits. */
static int strip_cwd_arg(int argc, char **argv)
{
    int i, out = 1;

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--cwd=", 6) != 0)
            argv[out++] = argv[i];
    }
    return out;
}

int main(int argc, char **argv)
{
    unsigned long long v;
    char line[64];
    int i, rc = 0;

    argc = strip_cwd_arg(argc, argv);

    if (argc < 2) {
        while (readline(line, sizeof(line)) != 0) {
            if (line[0] == '\0')
                continue;
            if (!parse_u64(line, &v)) {
                printf("factor: '%s' is not a non-negative integer\n", line);
                rc = 1;
                continue;
            }
            factor(v);
        }
        return rc;
    }

    for (i = 1; i < argc; i++) {
        if (!parse_u64(argv[i], &v)) {
            printf("factor: '%s' is not a non-negative integer\n", argv[i]);
            rc = 1;
            continue;
        }
        factor(v);
    }
    return rc;
}
