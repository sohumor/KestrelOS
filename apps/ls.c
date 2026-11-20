/* ls.c - list directory contents.
 *
 * usage: ls [-a] [-r] [path...]
 * Always long-style: "TYPE  SIZE  NAME", directories first (marked
 * 'd'), aligned columns, trailing entry count. "." and ".." are hidden
 * (and not counted) unless -a is given. With -r, emits @krec records.
 * With no path arguments the shell-provided cwd is listed.
 */

#include <kestrel.h>
#include <krec.h>
#include <stdio.h>
#include <string.h>

#define MAX_PATH    256

static const char *g_cwd = "/";

/* Join a possibly relative path with the cwd. */
static void resolve(const char *tok, char *out, unsigned long outsz)
{
    if (tok[0] == '/')
        snprintf(out, outsz, "%s", tok);
    else if (g_cwd[0] == '/' && g_cwd[1] == '\0')
        snprintf(out, outsz, "/%s", tok);
    else
        snprintf(out, outsz, "%s/%s", g_cwd, tok);
}

static int is_dot(const char *name)
{
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

static void emit_krec_entry(const char *name, int is_dir, uint32_t size,
                            uint32_t uid, uint32_t gid, uint32_t mode)
{
    char size_s[24], uid_s[16], gid_s[16], mode_s[16];

    snprintf(size_s, sizeof(size_s), "%u", size);
    snprintf(uid_s, sizeof(uid_s), "%u", uid);
    snprintf(gid_s, sizeof(gid_s), "%u", gid);
    snprintf(mode_s, sizeof(mode_s), "0%03o", mode & 0777U);
    krec_row(1, 6, name, is_dir ? "dir" : "file", size_s, uid_s, gid_s, mode_s);
}

static int list_path(const char *path, int show_all, int rflag)
{
    struct k_dirent ent;
    struct k_stat st;
    int i, pass, count = 0;

    if (readdir_at(path, 0, &ent) != 0) {
        if (stat_(path, &st) != 0) {
            if (rflag)
                krec_err("ls: cannot access %s\n", path);
            else
                printf("ls: cannot access %s\n", path);
            return 1;
        }
        if (!st.is_dir) {
            if (rflag) {
                emit_krec_entry(path, 0, st.size, st.uid, st.gid, st.mode);
            } else {
                printf("- %8u  %s\n", st.size, path);
                printf("1 entry\n");
            }
            return 0;
        }
    }

    for (pass = 0; pass < 2; pass++) {
        for (i = 0; readdir_at(path, i, &ent) == 0; i++) {
            ent.name[sizeof(ent.name) - 1] = '\0';
            if ((pass == 0) != (ent.is_dir != 0))
                continue;
            if (!show_all && is_dot(ent.name))
                continue;
            if (rflag) {
                emit_krec_entry(ent.name, ent.is_dir != 0,
                                ent.size, ent.uid, ent.gid, ent.mode);
            } else {
                printf("%c %8u  %s\n", ent.is_dir ? 'd' : '-',
                       ent.size, ent.name);
            }
            count++;
        }
    }
    if (!rflag)
        printf("%d entr%s\n", count, count == 1 ? "y" : "ies");
    return 0;
}

int main(int argc, char **argv)
{
    static const struct krec_col cols[] = {
        { "name", KREC_STR },
        { "type", KREC_STR },
        { "size", KREC_U64 },
        { "uid",  KREC_INT },
        { "gid",  KREC_INT },
        { "mode", KREC_STR },
    };
    char path[MAX_PATH];
    int i, rc = 0, show_all = 0, rflag = 0, paths = 0;

    argc = krec_strip_cwd(argc, argv, &g_cwd);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--krec") == 0) {
            rflag = 1;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            int j;
            for (j = 1; argv[i][j]; j++) {
                if (argv[i][j] == 'a')
                    show_all = 1;
                else if (argv[i][j] == 'r')
                    rflag = 1;
                else {
                    printf("usage: ls [-a] [-r] [path...]\n");
                    return 1;
                }
            }
        } else {
            paths++;
        }
    }

    if (rflag && krec_begin(1, cols, 6) < 0)
        return 1;

    if (paths == 0)
        return list_path(g_cwd, show_all, rflag);

    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0')
            continue;
        resolve(argv[i], path, sizeof(path));
        if (!rflag && paths > 1)
            printf("%s:\n", path);
        if (list_path(path, show_all, rflag) != 0)
            rc = 1;
    }
    return rc;
}
