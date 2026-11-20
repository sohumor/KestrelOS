/* kfshole - exercise sparse KFS reads through the file syscalls. */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const long tail = 11 * 4096 + 7;
    unsigned char buf[512];
    long pos = 0;
    int fd = open("/run/kfs-hole-test", O_RDWR | O_CREAT | O_TRUNC);
    if (fd < 0) {
        puts("kfshole: open failed");
        return 1;
    }
    if (seek(fd, tail, SEEK_SET) != tail ||
        write(fd, "X", 1) != 1 || seek(fd, 0, SEEK_SET) != 0) {
        puts("kfshole: seek/write setup failed");
        close(fd);
        return 1;
    }

    for (;;) {
        long want = tail + 1 - pos;
        if (want > (long)sizeof(buf))
            want = sizeof(buf);
        memset(buf, 0xff, sizeof(buf));
        long n = read(fd, buf, sizeof(buf));
        if (n != want) {
            printf("kfshole: read at %ld returned %ld, expected %ld\n",
                   pos, n, want);
            close(fd);
            return 1;
        }
        if (n == 0)
            break;
        for (long i = 0; i < n; i++) {
            if (buf[i] != (pos + i == tail ? 'X' : 0)) {
                printf("kfshole: unexpected byte at %ld\n", pos + i);
                close(fd);
                return 1;
            }
        }
        pos += n;
    }

    close(fd);
    puts("kfshole: zero-filled gap, tail and EOF verified");
    return 0;
}
