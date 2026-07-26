/* insmod.c - load a kernel module.
 *
 *   insmod hello                 loads /lib/modules/hello.kmod
 *   insmod /lib/modules/hello.kmod
 *   insmod ./build/hello.kmod    any path works
 *
 * The kernel does the reading, relocating and binding; this just names
 * the file. Failures are reported to the kernel log with the offending
 * symbol or relocation, so "see dmesg" is a real instruction here.
 */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

#ifndef SYS_INSMOD
#define SYS_INSMOD 53
#endif

#define MAX_PATH 256
#define MODULE_DIR "/lib/modules"

static const char *g_cwd = "/";

/* Pull the shell-injected trailing "--cwd=<path>" argument, if any. */
static int strip_cwd_arg(int argc, char **argv)
{
    if (argc > 1 && strncmp(argv[argc - 1], "--cwd=", 6) == 0) {
        g_cwd = argv[argc - 1] + 6;
        return argc - 1;
    }
    return argc;
}

/* A bare name means the system module directory; anything with a slash is
 * a path, relative to the cwd if it does not start with one. */
static void resolve(const char *tok, char *out, unsigned long outsz)
{
    if (!strchr(tok, '/'))
        snprintf(out, outsz, "%s/%s.kmod", MODULE_DIR, tok);
    else if (tok[0] == '/')
        snprintf(out, outsz, "%s", tok);
    else if (g_cwd[0] == '/' && g_cwd[1] == '\0')
        snprintf(out, outsz, "/%s", tok);
    else
        snprintf(out, outsz, "%s/%s", g_cwd, tok);
}

int main(int argc, char **argv)
{
    char path[MAX_PATH];

    argc = strip_cwd_arg(argc, argv);
    if (argc != 2) {
        printf("usage: insmod <name|path.kmod>\n");
        return 1;
    }

    resolve(argv[1], path, sizeof(path));
    if (syscall(SYS_INSMOD, (long)path, 0, 0, 0) != 0) {
        printf("insmod: %s: load failed (run dmesg for the reason)\n", path);
        return 1;
    }
    return 0;
}
