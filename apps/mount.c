/* mount.c - list, attach and detach filesystems.
 *
 *   mount                              list what is mounted
 *   mount -t <fstype> <device> <path>  mount a filesystem
 *   mount -u <path>                    unmount one
 *
 * Listing reads /dev/mounts, the kernel's own view of the mount table,
 * so it costs no syscall of its own. Each line there is
 *
 *   <device> <mountpoint> <fstype> <block size> <blocks> <free blocks>
 *
 * with "-" for a filesystem that has no block device.
 */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

/* Until abi/kestrel_abi.h carries them. */
#ifndef SYS_MOUNT
#define SYS_MOUNT  56   /* (path, fstype, devname) -> 0 / -1 */
#endif
#ifndef SYS_UMOUNT
#define SYS_UMOUNT 57   /* (path) -> 0 / -1 */
#endif

#define MOUNTS_PATH "/dev/mounts"
#define TABLE_MAX   2048
#define FIELD_MAX   64

/* Pull the shell-injected trailing "--cwd=<path>" argument, if any. */
static int strip_cwd_arg(int argc, char **argv)
{
    if (argc > 1 && strncmp(argv[argc - 1], "--cwd=", 6) == 0)
        return argc - 1;
    return argc;
}

/* Copy the next whitespace-delimited field of *pp into out, advancing
 * *pp past it. Returns 1 on a field, 0 at end of line. */
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

/* Read the whole of /dev/mounts into buf. Returns the length, or -1. */
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

static int list_mounts(void)
{
    char buf[TABLE_MAX];
    char dev[FIELD_MAX], path[FIELD_MAX], type[FIELD_MAX];
    const char *p;

    if (read_table(buf, sizeof(buf)) < 0) {
        printf("mount: cannot read %s\n", MOUNTS_PATH);
        return 1;
    }

    p = buf;
    while (*p) {
        if (!next_field(&p, dev, sizeof(dev)))
            break;
        if (!next_field(&p, path, sizeof(path)) ||
            !next_field(&p, type, sizeof(type)))
            break;
        /* A filesystem with no block device is named after its type,
         * the way devfs is its own source. */
        printf("%s on %s type %s\n",
               strcmp(dev, "-") == 0 ? type : dev, path, type);
        /* Skip the statistics and the newline. */
        while (*p && *p != '\n')
            p++;
        if (*p == '\n')
            p++;
    }
    return 0;
}

/* The kernel names block devices without a directory ("hda"), so a
 * /dev/-qualified argument is accepted and trimmed. */
static const char *dev_name(const char *s)
{
    if (strncmp(s, "/dev/", 5) == 0)
        return s + 5;
    return s;
}

static int usage(void)
{
    printf("usage: mount\n");
    printf("       mount -t <fstype> <device> <mountpoint>\n");
    printf("       mount -u <mountpoint>\n");
    return 1;
}

int main(int argc, char **argv)
{
    argc = strip_cwd_arg(argc, argv);

    if (argc == 1)
        return list_mounts();

    if (strcmp(argv[1], "-u") == 0) {
        if (argc != 3)
            return usage();
        if (syscall(SYS_UMOUNT, (long)argv[2], 0, 0, 0) < 0) {
            printf("mount: cannot unmount %s\n", argv[2]);
            return 1;
        }
        return 0;
    }

    if (strcmp(argv[1], "-t") == 0) {
        if (argc != 5)
            return usage();
        if (syscall(SYS_MOUNT, (long)argv[4], (long)argv[2],
                    (long)dev_name(argv[3]), 0) < 0) {
            printf("mount: cannot mount %s on %s\n", argv[3], argv[4]);
            return 1;
        }
        return 0;
    }

    return usage();
}
