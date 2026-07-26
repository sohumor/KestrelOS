/* init.c - PID 1 for KestrelOS.
 *
 * Prints /etc/motd if present, then spawns /bin/sh forever,
 * restarting it whenever it exits.
 */

#include <kestrel.h>
#include <stdio.h>

static void print_motd(void)
{
    char buf[512];
    long n;
    int fd;

    fd = open("/etc/motd", O_RDONLY);
    if (fd < 0)
        return;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, (unsigned long)n);
    close(fd);
}

int main(int argc, char **argv)
{
    char *sh_argv[2];
    int pid;

    (void)argc;
    (void)argv;

    print_motd();

    sh_argv[0] = "/bin/sh";
    sh_argv[1] = 0;

    for (;;) {
        pid = spawn("/bin/sh", sh_argv);
        if (pid < 0) {
            printf("init: cannot spawn /bin/sh\n");
            sleep_ms(1000);
            continue;
        }
        waitpid(pid);
        printf("sh exited, restarting\n");
        sleep_ms(200);
    }
}
