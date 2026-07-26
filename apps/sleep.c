/* sleep.c - pause for a number of seconds.
 *
 * The wait is split into short naps so the console can be polled between
 * them: 'q', ESC or ctrl-C aborts. (The console driver currently drops
 * ctrl-C, so 'q' is the reliable key - see the note in yes.c.)
 */

#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SECS 86400UL        /* one day; longer is a typo, not a request */
#define TICK_MS  100UL          /* poll the console this often */

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

static int quit_requested(void)
{
    char buf[16];
    long n, i;

    n = read_nb(0, buf, sizeof(buf));
    for (i = 0; i < n; i++) {
        if (buf[i] == 'q' || buf[i] == 'Q' || buf[i] == 27 || buf[i] == 3)
            return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    unsigned long left;
    long secs;
    const char *s;

    argc = strip_cwd_arg(argc, argv);
    if (argc != 2) {
        printf("usage: sleep <seconds>\n");
        return 1;
    }

    for (s = argv[1]; *s; s++) {
        if (*s < '0' || *s > '9') {
            printf("sleep: invalid interval '%s'\n", argv[1]);
            return 1;
        }
    }

    /* atol() would wrap on a very long digit string, so bound the length
     * first, then the value: an out-of-range interval is an error, not a
     * silent clamp that wedges the only console for a day. */
    for (s = argv[1]; s[0] == '0' && s[1]; s++)
        ;
    secs = strlen(s) > 5 ? -1 : atol(s);
    if (secs < 0 || (unsigned long)secs > MAX_SECS) {
        printf("sleep: interval too large (max %lu)\n", MAX_SECS);
        return 1;
    }

    for (left = (unsigned long)secs * 1000UL; left > 0; ) {
        unsigned long nap = left < TICK_MS ? left : TICK_MS;

        if (quit_requested()) {
            printf("sleep: interrupted\n");
            return 1;
        }
        sleep_ms(nap);
        left -= nap;
    }
    return 0;
}
