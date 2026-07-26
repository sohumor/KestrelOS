/* help.c - list every KestrelOS command with a one-line description.
 *
 * usage: help [substring]
 * The table below must name every executable installed into /bin: it is
 * the only in-OS way to discover a command. When an app is added to (or
 * removed from) apps/, add (or remove) its row here as well; `ls /bin`
 * is the cross-check.
 *
 * The full list is longer than a 25-row console, which has no scrollback,
 * so a substring narrows it: "help net", "help file", "help dir".
 */

#include <stdio.h>
#include <string.h>

struct cmd {
    const char *name;
    const char *desc;
};

/* A row with a NULL desc is a section heading, not a command. */
static const struct cmd commands[] = {
    { "shell and session", 0 },
    { "help",      "this list" },
    { "sh",        "the shell: history, tab completion, redirection" },
    { "minsh",     "minimal fallback shell (no editing, no redirection)" },
    { "init",      "PID 1: prints /etc/motd, keeps a shell running" },
    { "clear",     "clear the screen" },
    { "sleep",     "pause for N seconds" },
    { "yes",       "repeat a string until 'q' is pressed" },
    { "true",      "do nothing, successfully (exit 0)" },
    { "false",     "do nothing, unsuccessfully (exit 1)" },

    { "files and directories", 0 },
    { "ls",        "list a directory (dirs first, sizes, entry count)" },
    { "cat",       "print file contents" },
    { "cp",        "copy a file" },
    { "mv",        "move (rename) a file" },
    { "rm",        "remove files" },
    { "mkdir",     "create directories" },
    { "touch",     "create empty files" },
    { "find",      "walk a directory tree printing paths (-name SUB)" },
    { "tree",      "indented recursive directory listing" },
    { "du",        "recursive byte totals per directory" },
    { "hexdump",   "hex + ascii dump of a file" },

    { "text", 0 },
    { "echo",      "print arguments joined by spaces" },
    { "head",      "print the first lines of a file (-n N)" },
    { "tail",      "print the last lines of a file (-n N)" },
    { "grep",      "print lines containing a literal substring (-i, -n)" },
    { "wc",        "count lines, words and bytes (-l, -w, -c)" },
    { "writefile", "type text into a file, finish with ctrl-D" },
    { "edit",      "full-screen text editor" },

    { "system", 0 },
    { "ps",        "list processes (PID, STATE, NAME)" },
    { "free",      "physical memory usage in KiB and MiB" },
    { "uptime",    "time since boot as h:mm:ss" },
    { "date",      "print the RTC wall clock" },
    { "sysinfo",   "system overview (cpu, memory, net, uptime)" },
    { "calc",      "64-bit integer expression evaluator" },
    { "halt",      "power off the machine" },
    { "reboot",    "restart the machine" },

    { "network", 0 },
    { "ping",      "ICMP echo round-trip time to a host" },
    { "nslookup",  "resolve a hostname via DNS" },
    { "udp",       "send/receive raw UDP datagrams" },

    { "games", 0 },
    { "snake",     "the classic snake game" },
};

int main(int argc, char **argv)
{
    unsigned long i;
    const char *filter = 0;
    int nmatch = 0, j;

    /* The shell appends "--cwd=<path>"; help has no use for it. */
    for (j = 1; j < argc; j++) {
        if (strncmp(argv[j], "--cwd=", 6) != 0 && !filter)
            filter = argv[j];
    }

    if (filter) {
        printf("KestrelOS commands matching \"%s\":\n", filter);
        for (i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
            if (!commands[i].desc)
                continue;
            if (!strstr(commands[i].name, filter) &&
                !strstr(commands[i].desc, filter))
                continue;
            printf("  %-10s %s\n", commands[i].name, commands[i].desc);
            nmatch++;
        }
        if (!nmatch)
            puts("  (none -- run help with no argument for the full list)");
        return nmatch ? 0 : 1;
    }

    puts("KestrelOS commands (in /bin):");
    for (i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        if (!commands[i].desc)
            printf("\n%s:\n", commands[i].name);
        else
            printf("  %-10s %s\n", commands[i].name, commands[i].desc);
    }
    puts("");
    puts("shell builtins: cd, pwd, exit, help, history, clear, which, set");
    puts("the shell redirects with > >> <; ';' separates, '#' comments,");
    puts("$? and $PWD expand, tab completes, up/down recalls history");
    puts("paths may be absolute (/doc/welcome.md) or relative to the cwd");
    puts("\"help <substring>\" lists just the matching commands");
    return 0;
}
