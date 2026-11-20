#include <mission.h>
#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int error_out(char *error, size_t size, const char *message)
{
    if (error && size) snprintf(error, size, "%s", message);
    return -1;
}

static char *load_text(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    char *text = malloc(MISSION_TEXT_MAX + 2);
    size_t used = 0;
    if (text) {
        while (used <= MISSION_TEXT_MAX) {
            long n = read(fd, text + used, MISSION_TEXT_MAX + 1 - used);
            if (n < 0) { free(text); text = NULL; break; }
            if (!n) break;
            used += (size_t)n;
        }
        if (text && used > MISSION_TEXT_MAX) { free(text); text = NULL; }
        if (text) text[used] = 0;
    }
    close(fd);
    return text;
}

int mission_load(const char *id, struct mission_manifest *out, char *error, size_t size)
{
    if (!id || !*id || strlen(id) > 48) return error_out(error, size, "invalid manifest id");
    for (const char *p = id; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
            return error_out(error, size, "invalid manifest id");
    char path[128];
    snprintf(path, sizeof(path), "/etc/missions/%s.mission", id);
    char *text = load_text(path);
    if (!text) return error_out(error, size, "cannot read manifest");
    int rc = mission_parse(text, strlen(text), out, error, size);
    free(text);
    return rc;
}

static int caller_home(char home[64])
{
    char *text = load_text("/etc/passwd");
    if (!text) return -1;
    int rc = -1;
    for (char *line = text; *line;) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        else next = line + strlen(line);
        char *fields[6];
        unsigned count = 0;
        if (*line && *line != '#') {
            fields[count++] = line;
            for (char *p = line; *p && count < 6; p++)
                if (*p == ':') { *p = 0; fields[count++] = p + 1; }
            if (count == 6) {
                uint64_t uid = 0;
                const char *p = fields[1];
                int valid = *p != 0;
                for (; *p; p++) {
                    if (*p < '0' || *p > '9' || uid > 0xffffffffU / 10U) { valid = 0; break; }
                    uid = uid * 10 + (unsigned)(*p - '0');
                }
                if (valid && uid == getuid_() && fields[4][0] == '/' && strlen(fields[4]) < 64) {
                    strcpy(home, fields[4]); rc = 0; break;
                }
            }
        }
        line = next;
    }
    free(text);
    return rc;
}

static int resolve_host(const char *name, uint32_t *ip, void *ctx)
{
    (void)ctx;
    return dns_resolve(name, ip);
}

int mission_build_current(const struct mission_manifest *m, const struct mission_options *o,
                          struct k_mission_grant *out, char *error, size_t size)
{
    char home[64] = "/";
    int need_home = 0;
    for (unsigned i = 0; i < m->declared.path_count; i++)
        if (m->declared.paths[i].prefix[0] == '~') need_home = 1;
    if (o) {
        for (unsigned i = 0; i < o->read_count; i++) if (o->read[i][0] == '~') need_home = 1;
        for (unsigned i = 0; i < o->write_count; i++) if (o->write[i][0] == '~') need_home = 1;
    }
    if (need_home && caller_home(home) < 0) return error_out(error, size, "cannot determine caller home");
    return mission_build(m, o, home, resolve_host, NULL, out, error, size);
}
