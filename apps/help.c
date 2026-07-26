/* help.c - list every KestrelOS command with a one-line description. */

#include <stdio.h>

struct cmd {
    const char *name;
    const char *desc;
};

static const struct cmd commands[] = {
    { "help",      "this list" },
    { "ls",        "list a directory (dirs first, sizes, entry count)" },
    { "cat",       "print file contents" },
    { "echo",      "print arguments joined by spaces" },
    { "ps",        "list processes (PID, STATE, NAME)" },
    { "free",      "physical memory usage in KiB and MiB" },
    { "uptime",    "time since boot as h:mm:ss" },
    { "clear",     "clear the screen" },
    { "rm",        "remove files" },
    { "mkdir",     "create directories" },
    { "writefile", "type text into a file, finish with ctrl-D" },
    { "hexdump",   "hex + ascii dump of a file" },
    { "edit",      "full-screen text editor" },
    { "snake",     "the classic snake game" },
    { "sysinfo",   "system overview (cpu, memory, net, uptime)" },
    { "ping",      "ICMP echo round-trip time to a host" },
    { "nslookup",  "resolve a hostname via DNS" },
    { "udp",       "send/receive raw UDP datagrams" },
};

int main(int argc, char **argv)
{
    unsigned long i;

    (void)argc;
    (void)argv;

    puts("KestrelOS commands (in /bin):");
    for (i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
        printf("  %-10s %s\n", commands[i].name, commands[i].desc);
    puts("");
    puts("shell builtins: cd <dir>, pwd, exit [code], help");
    puts("paths may be absolute (/doc/welcome.md) or relative to the cwd");
    return 0;
}
