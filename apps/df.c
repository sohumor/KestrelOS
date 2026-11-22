/* df.c - report per-mount block usage.
 *
 * Reads /dev/mounts, whose lines are
 *
 *   <device> <mountpoint> <fstype> <block size> <blocks> <free blocks>
 *
 * and converts the filesystem's own block accounting into KiB, the unit
 * everyone actually reads. A filesystem that stores nothing (devfs)
 * reports zero blocks and is shown with dashes rather than a fake 0%.
 *
 *   df        sizes in KiB
 *   df -h     sizes scaled to K / M / G
 *   df -r     emit @krec records
 */

#include <kestrel.h>
#include <krec.h>
#include <stdio.h>
#include <string.h>

#define MOUNTS_PATH "/dev/mounts"
#define TABLE_MAX   2048
#define FIELD_MAX   64

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

static long read_table(char *buf, unsigned long size)
{
    unsigned long got = 0;
    long n;
    int fd;

    fd = open(MOUNTS_PATH, O_RDONLY);
    if (fd < 0)
        return -1;
    while (got + 1 < size) {
        n = read(fd, buf + got, size - got - 1);
        if (n <= 0)
            break;
        got += (unsigned long)n;
    }
    close(fd);
    buf[got] = '\0';
    return (long)got;
}

/* Round to the largest unit that leaves a whole number of digits. */
static void human(unsigned long long kib, char *out, unsigned long outsz)
{
    static const char unit[] = { 'K', 'M', 'G', 'T' };
    unsigned long long v = kib;
    int u = 0;

    while (v >= 1024 && u < 3) {
        v /= 1024;
        u++;
    }
    snprintf(out, outsz, "%llu%c", v, unit[u]);
}

int main(int argc, char **argv)
{
    static const struct krec_col cols[] = {
        { "mount",    KREC_STR },
        { "fs",       KREC_STR },
        { "total_kb", KREC_U64 },
        { "used_kb",  KREC_U64 },
        { "free_kb",  KREC_U64 },
        { "dev",      KREC_STR },
    };
    char buf[TABLE_MAX];
    char dev[FIELD_MAX], path[FIELD_MAX], type[FIELD_MAX];
    char bs[FIELD_MAX], nb[FIELD_MAX], nf[FIELD_MAX];
    char c1[24], c2[24], c3[24];
    const char *p;
    int hflag = 0, rflag = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--krec") == 0) {
            rflag = 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("usage: df [-h] [-r]\n");
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            for (int j = 1; argv[i][j]; j++) {
                if (argv[i][j] == 'h')
                    hflag = 1;
                else if (argv[i][j] == 'r')
                    rflag = 1;
                else {
                    printf("usage: df [-h] [-r]\n");
                    return 1;
                }
            }
        } else {
            printf("usage: df [-h] [-r]\n");
            return 1;
        }
    }

    if (read_table(buf, sizeof(buf)) < 0) {
        if (rflag)
            krec_err("df: cannot read %s\n", MOUNTS_PATH);
        else
            printf("df: cannot read %s\n", MOUNTS_PATH);
        return 1;
    }

    if (rflag) {
        if (krec_begin(1, cols, 6) < 0)
            return 1;
    } else {
        printf("%-10s %-6s %10s %10s %10s %5s %s\n",
               "Filesystem", "Type", hflag ? "Size" : "1K-blocks",
               "Used", "Avail", "Use%", "Mounted on");
    }

    p = buf;
    while (*p) {
        unsigned long long bsz, blocks, freeb, total_kb, free_kb, used_kb;
        unsigned pct;

        if (!next_field(&p, dev, sizeof(dev)))
            break;
        if (!next_field(&p, path, sizeof(path)) ||
            !next_field(&p, type, sizeof(type)) ||
            !next_field(&p, bs, sizeof(bs)) ||
            !next_field(&p, nb, sizeof(nb)) ||
            !next_field(&p, nf, sizeof(nf)))
            break;
        while (*p && *p != '\n')
            p++;
        if (*p == '\n')
            p++;

        bsz = to_u64(bs);
        blocks = to_u64(nb);
        freeb = to_u64(nf);

        /* A filesystem with no block device is named after its type. */
        if (strcmp(dev, "-") == 0) {
            strncpy(dev, type, sizeof(dev) - 1);
            dev[sizeof(dev) - 1] = '\0';
        }

        if (bsz == 0 || blocks == 0) {
            if (rflag) {
                krec_row(1, 6, path, type, "0", "0", "0", dev);
            } else {
                /* Nothing stored here: no honest numbers to print. */
                printf("%-10s %-6s %10s %10s %10s %5s %s\n",
                       dev, type, "-", "-", "-", "-", path);
            }
            continue;
        }

        total_kb = blocks * bsz / 1024;
        free_kb = freeb * bsz / 1024;
        used_kb = total_kb - free_kb;

        if (rflag) {
            snprintf(c1, sizeof(c1), "%llu", total_kb);
            snprintf(c2, sizeof(c2), "%llu", used_kb);
            snprintf(c3, sizeof(c3), "%llu", free_kb);
            krec_row(1, 6, path, type, c1, c2, c3, dev);
            continue;
        }

        pct = (unsigned)(blocks ? (blocks - freeb) * 100 / blocks : 0);

        if (hflag) {
            human(total_kb, c1, sizeof(c1));
            human(used_kb, c2, sizeof(c2));
            human(free_kb, c3, sizeof(c3));
        } else {
            snprintf(c1, sizeof(c1), "%llu", total_kb);
            snprintf(c2, sizeof(c2), "%llu", used_kb);
            snprintf(c3, sizeof(c3), "%llu", free_kb);
        }
        printf("%-10s %-6s %10s %10s %10s %4u%% %s\n",
               dev, type, c1, c2, c3, pct, path);
    }
    return 0;
}
