/* rngtest - verify the kernel CSPRNG's syscall and device interfaces. */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

static int all_zero(const unsigned char *p, unsigned long n)
{
    for (unsigned long i = 0; i < n; i++)
        if (p[i])
            return 0;
    return 1;
}

static int device_read(const char *path, unsigned char out[32])
{
    int fd = open(path, O_RDONLY);
    long n;
    if (fd < 0)
        return -1;
    n = read(fd, out, 32);
    close(fd);
    return n == 32 ? 0 : -1;
}

int main(void)
{
    unsigned char a[32], b[32], c[32], d[32];

    if (getrandom(a, sizeof(a), 0) != (long)sizeof(a) ||
        getrandom(b, sizeof(b), GRND_RANDOM) != (long)sizeof(b) ||
        device_read("/dev/urandom", c) < 0 ||
        device_read("/dev/random", d) < 0) {
        printf("rngtest: entropy interface failed\n");
        return 1;
    }
    if (all_zero(a, sizeof(a)) || all_zero(b, sizeof(b)) ||
        all_zero(c, sizeof(c)) || all_zero(d, sizeof(d)) ||
        memcmp(a, b, sizeof(a)) == 0 || memcmp(c, d, sizeof(c)) == 0) {
        printf("rngtest: repeated or zero output\n");
        return 1;
    }
    printf("rngtest: getrandom + random devices verified\n");
    return 0;
}
