/* yes.c - repeat a string until 'q' is pressed.
 *
 * usage: yes [string...]   (default "y")
 * The console is polled non-blockingly and 'q'/'Q' stops the loop at
 * once. ESC and ctrl-C are accepted too, but do not work today: the
 * console driver holds a lone ESC back waiting for the rest of a
 * possible ANSI sequence, and discards ctrl-C (0x03) outright instead of
 * queueing it for the foreground process. Use 'q'.
 */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

#define LINESZ 256

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
    char line[LINESZ];
    unsigned long len = 0;
    int i;


    if (argc < 2) {
        snprintf(line, sizeof(line), "y");
    } else {
        line[0] = '\0';
        for (i = 1; i < argc; i++) {
            len = strlen(line);
            snprintf(line + len, sizeof(line) - len, "%s%s",
                     i > 1 ? " " : "", argv[i]);
        }
    }
    len = strlen(line);
    line[len] = '\n';
    len++;

    for (;;) {
        if (quit_requested())
            break;
        write(1, line, len);
        yield_();
    }
    return 0;
}
