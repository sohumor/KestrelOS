/* lsblk.c - list the registered block devices.
 *
 * Reads /dev/blocks, the kernel's view of the block device registry,
 * whose lines are
 *
 *   <name> <block size> <blocks>
 *
 * so listing disks needs no syscall of its own -- the same file the
 * kernel uses to populate /dev is the one this reads.
 */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

#define BLOCKS_PATH "/dev/blocks"
#define TABLE_MAX   1024
#define FIELD_MAX   64

static int strip_cwd_arg(int argc, char **argv)
{
    if (argc > 1 && strncmp(argv[argc - 1], "--cwd=", 6) == 0)
        return argc - 1;
    return argc;
}

static int next_field(const char **pp, char *out, unsigned long outsz)
{
    const char *p = *pp;
    unsigned long i = 0;

    while (*p == ' ')
        p++;
    if (*p == '\0' || *p == '\n') {
        *pp = p;
        return 0;
    }
    while (*p && *p != ' ' && *p != '\n') {
        if (i + 1 < outsz)
            out[i++] = *p;
        p++;
    }
    out[i] = '\0';
    *pp = p;
    return 1;
}

static unsigned long long to_u64(const char *s)
{
    unsigned long long v = 0;

    while (*s >= '0' && *s <= '9')
        v = v * 10 + (unsigned long long)(*s++ - '0');
    return v;
}

/* Round to the largest unit that leaves a whole number of digits. */
static void human(unsigned long long bytes, char *out, unsigned long outsz)
{
    static const char unit[] = { 'B', 'K', 'M', 'G', 'T' };
    unsigned long long v = bytes;
    int u = 0;

    while (v >= 1024 && u < 4) {
        v /= 1024;
        u++;
    }
    snprintf(out, outsz, "%llu%c", v, unit[u]);
}

int main(int argc, char **argv)
{
    char buf[TABLE_MAX];
    char name[FIELD_MAX], bs[FIELD_MAX], nb[FIELD_MAX];
    char size[24];
    const char *p;
    unsigned long got = 0;
    long n;
    int fd, count = 0;

    argc = strip_cwd_arg(argc, argv);
    if (argc > 1) {
        printf("usage: lsblk\n");
        return 1;
    }

    fd = open(BLOCKS_PATH, O_RDONLY);
    if (fd < 0) {
        printf("lsblk: cannot read %s\n", BLOCKS_PATH);
        return 1;
    }
    while (got + 1 < sizeof(buf)) {
        n = read(fd, buf + got, sizeof(buf) - got - 1);
        if (n <= 0)
            break;
        got += (unsigned long)n;
    }
    close(fd);
    buf[got] = '\0';

    printf("%-8s %8s %10s %12s %s\n",
           "NAME", "SIZE", "BLKSZ", "BLOCKS", "DEVICE");

    p = buf;
    while (*p) {
        unsigned long long bsz, blocks;

        if (!next_field(&p, name, sizeof(name)))
            break;
        if (!next_field(&p, bs, sizeof(bs)) ||
            !next_field(&p, nb, sizeof(nb)))
            break;
        while (*p && *p != '\n')
            p++;
        if (*p == '\n')
            p++;

        bsz = to_u64(bs);
        blocks = to_u64(nb);
        human(blocks * bsz, size, sizeof(size));
        printf("%-8s %8s %10llu %12llu /dev/%s\n",
               name, size, bsz, blocks, name);
        count++;
    }

    if (count == 0)
        printf("(no block devices)\n");
    return 0;
}
