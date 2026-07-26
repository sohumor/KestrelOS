/* halt.c - stop the machine, through init when it is available.
 *
 * Writes "halt" to /run/shutdown so init can stop the services in reverse
 * order first. If init has not taken the machine down within ~3 seconds
 * (or is not running at all) the kernel power syscall is invoked directly.
 */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

#define RUN_DIR       "/run"
#define SHUTDOWN_PATH "/run/shutdown"
#define WAIT_MS       3000
#define STEP_MS       100

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

/* 1 when pid 1 is a running init that can service the request. */
static int init_present(void)
{
    struct k_psinfo pi;
    int i;

    for (i = 0; psinfo(i, &pi) == 0; i++)
        if (pi.pid == 1)
            return strstr(pi.name, "init") != 0;
    return 0;
}

/* Ask init to shut down. Returns 0 when the request was filed. */
static int request(const char *what)
{
    struct k_stat st;
    char line[32];
    long n;
    int fd;

    if (!init_present())
        return -1;
    if (stat_(RUN_DIR, &st) < 0 || !st.is_dir) {
        if (mkdir_(RUN_DIR) < 0 || stat_(RUN_DIR, &st) < 0)
            return -1;
    }
    fd = open(SHUTDOWN_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return -1;
    /* init only acts on a newline-terminated word, so the whole request
     * lands in one write and can never be read half-formed. */
    snprintf(line, sizeof(line), "%s\n", what);
    n = write(fd, line, strlen(line));
    close(fd);
    return n < 0 ? -1 : 0;
}

int main(int argc, char **argv)
{
    int waited;

    argc = strip_cwd_arg(argc, argv);
    if (argc > 1) {
        printf("usage: halt\n");
        return 1;
    }

    printf("Stopping KestrelOS...\n");

    if (request("halt") == 0) {
        for (waited = 0; waited < WAIT_MS; waited += STEP_MS)
            sleep_ms(STEP_MS);
        printf("halt: init did not respond in %d ms, stopping now\n", WAIT_MS);
    } else {
        printf("halt: init is not available, stopping now\n");
    }

    printf("System halted. It is safe to power off.\n");
    sleep_ms(100);
    syscall(SYS_POWER, K_POWER_HALT, 0, 0, 0);

    /* Only reached if the kernel could not stop the machine. */
    printf("halt: failed\n");
    return 1;
}
