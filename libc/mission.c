#include <mission.h>
#include <string.h>

static int fail(char *error, size_t size, const char *message)
{
    if (error && size) {
        size_t n = strlen(message);
        if (n >= size) n = size - 1;
        memcpy(error, message, n);
        error[n] = 0;
    }
    return -1;
}

static int copy(char *out, size_t size, const char *s)
{
    size_t n = strlen(s);
    if (n >= size) return -1;
    memcpy(out, s, n + 1);
    return 0;
}

const char *mission_channel_name(unsigned c)
{
    static const char *const names[] = {
        "fs.read", "fs.write", "net", "display", "spawn", "ipc",
        "audio", "camera", "input", "random"
    };
    return c < K_MISSION_CHANNELS ? names[c] : "unknown";
}

int mission_channel_index(const char *name)
{
    for (unsigned c = 0; c < K_MISSION_CHANNELS; c++)
        if (!strcmp(name, mission_channel_name(c))) return (int)c;
    return -1;
}

unsigned mission_device_bit(const char *name)
{
    int c = mission_channel_index(name);
    return c >= K_MISSION_AUDIO ? 1U << (c - K_MISSION_AUDIO) : 0;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
    return s;
}

static char *word(char **cursor)
{
    char *p = trim(*cursor), *start = p;
    if (!*p) return NULL;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) *p++ = 0;
    *cursor = p;
    return start;
}

static int path_add(struct k_mission_grant *g, const char *prefix, unsigned mode)
{
    for (unsigned i = 0; i < g->path_count; i++)
        if (!strcmp(g->paths[i].prefix, prefix)) {
            g->paths[i].mode |= mode;
            return 0;
        }
    if (g->path_count == K_MISSION_PATHS) return -1;
    struct k_mission_path *p = &g->paths[g->path_count];
    if (copy(p->prefix, sizeof(p->prefix), prefix) < 0) return -1;
    p->mode = mode;
    g->path_count++;
    return 0;
}

int mission_parse(const char *text, size_t len, struct mission_manifest *out,
                  char *error, size_t error_size)
{
    static const char *const keys[] = {
        "app", "exec", "mission", "display", "fs.read", "fs.write",
        "net", "device", "ipc", "spawn"
    };
    unsigned seen = 0;
    if (!text || !out || !len || len > MISSION_TEXT_MAX)
        return fail(error, error_size, "empty or oversized manifest");
    memset(out, 0, sizeof(*out));
    struct k_mission_grant *g = &out->declared;
    size_t off = 0;
    while (off < len) {
        char line[512];
        size_t n = 0;
        while (off < len && text[off] != '\n') {
            if (!text[off] || n + 1 >= sizeof(line))
                return fail(error, error_size, "invalid or oversized line");
            line[n++] = text[off++];
        }
        if (off < len) off++;
        line[n] = 0;
        char *comment = strchr(line, '#');
        if (comment) *comment = 0;
        char *key = trim(line);
        if (!*key) continue;
        char *value = strchr(key, ':');
        if (!value) return fail(error, error_size, "expected key: value");
        *value++ = 0;
        key = trim(key); value = trim(value);
        unsigned k;
        for (k = 0; k < 10 && strcmp(key, keys[k]); k++) {}
        if (k == 10 || (seen & (1U << k)) || !*value)
            return fail(error, error_size, "unknown, duplicate or empty field");
        seen |= 1U << k;
        if (k < 3) {
            char *dst = k == 0 ? g->app : k == 1 ? out->exec : g->title;
            size_t size = k == 0 ? sizeof(g->app) : k == 1 ? sizeof(out->exec) : sizeof(g->title);
            if (copy(dst, size, value) < 0 || (k == 1 && value[0] != '/'))
                return fail(error, error_size, "invalid name, mission or executable");
        } else if (k == 3 || k == 9) {
            if (!strcmp(value, "yes")) g->cap_mask |= k == 3 ? KCAP_WM : KCAP_SPAWN;
            else if (strcmp(value, "no")) return fail(error, error_size, "expected yes or no");
        } else if (k == 4 || k == 5) {
            if (!strcmp(value, "none")) continue;
            char *p;
            while ((p = word(&value))) {
                if ((p[0] != '/' && !(p[0] == '~' && (!p[1] || p[1] == '/'))) ||
                    path_add(g, p, k == 4 ? K_MISSION_READ : K_MISSION_WRITE) < 0)
                    return fail(error, error_size, "invalid or too many paths");
            }
        } else if (k == 6) {
            if (!strcmp(value, "none")) g->net_mode = K_MISSION_NET_NONE;
            else if (!strcmp(value, "any")) g->net_mode = K_MISSION_NET_ANY;
            else {
                char *p = word(&value);
                if (strcmp(p, "host")) return fail(error, error_size, "invalid network policy");
                g->net_mode = K_MISSION_NET_HOSTS;
                while ((p = word(&value))) {
                    if (g->host_count == K_MISSION_HOSTS ||
                        copy(out->hosts[g->host_count++], MISSION_HOST_NAME, p) < 0)
                        return fail(error, error_size, "too many or oversized hosts");
                }
                if (!g->host_count) return fail(error, error_size, "host policy needs hosts");
            }
        } else {
            if (!strcmp(value, "none")) continue;
            char *p;
            while ((p = word(&value))) {
                unsigned bit = 0;
                if (k == 7) bit = mission_device_bit(p);
                else if (!strcmp(p, "talon")) bit = K_MISSION_IPC_TALON;
                else if (!strcmp(p, "pulse")) bit = K_MISSION_IPC_PULSE;
                else if (!strcmp(p, "signal")) bit = K_MISSION_IPC_SIGNAL;
                else if (!strcmp(p, "pipe")) bit = K_MISSION_IPC_PIPE;
                if (!bit) return fail(error, error_size, "unknown device or IPC kind");
                if (k == 7) g->device_bits |= bit;
                else g->ipc_bits |= bit;
            }
        }
    }
    if ((seen & 7) != 7) return fail(error, error_size, "app, exec and mission are required");
    if (error && error_size) error[0] = 0;
    return 0;
}

static int canonical(const char *raw, const char *home, char out[64])
{
    char expanded[256];
    if (!raw || !home || home[0] != '/') return -1;
    if (raw[0] == '~' && (!raw[1] || raw[1] == '/')) {
        size_t h = strlen(home), r = strlen(raw + 1);
        if (h + r >= sizeof(expanded)) return -1;
        memcpy(expanded, home, h);
        memcpy(expanded + h, raw + 1, r + 1);
        raw = expanded;
    }
    if (raw[0] != '/') return -1;
    size_t n = 1;
    out[0] = '/'; out[1] = 0;
    while (*raw) {
        while (*raw == '/') raw++;
        const char *p = raw;
        while (*raw && *raw != '/') raw++;
        size_t len = (size_t)(raw - p);
        if (!len) break;
        if (len == 1 && p[0] == '.') continue;
        if (len == 2 && p[0] == '.' && p[1] == '.') {
            while (n > 1 && out[n - 1] != '/') n--;
            if (n > 1) n--;
        } else {
            if (n + len + (n > 1) >= 64) return -1;
            if (n > 1) out[n++] = '/';
            memcpy(out + n, p, len); n += len;
        }
        out[n] = 0;
    }
    return 0;
}

static int beneath(const char *path, const char *prefix)
{
    if (!strcmp(prefix, "/")) return 1;
    size_t n = strlen(prefix);
    return !strncmp(path, prefix, n) && (!path[n] || path[n] == '/');
}

static int ipv4(const char *p, uint32_t *ip)
{
    uint32_t result = 0;
    for (unsigned i = 0; i < 4; i++) {
        unsigned n = 0, digits = 0;
        while (*p >= '0' && *p <= '9') {
            n = n * 10 + (unsigned)(*p++ - '0');
            if (++digits > 3 || n > 255) return -1;
        }
        if (!digits || (i < 3 ? *p++ != '.' : *p != 0)) return -1;
        result |= n << (8 * i);
    }
    *ip = result;
    return 0;
}

static int hosts(struct k_mission_grant *g, const char names[][MISSION_HOST_NAME],
                 mission_resolver resolve, void *ctx)
{
    for (unsigned i = 0; i < g->host_count; i++)
        if (ipv4(names[i], &g->hosts[i].ip_be) < 0 &&
            (!resolve || resolve(names[i], &g->hosts[i].ip_be, ctx) < 0)) return -1;
    return 0;
}

static void caps(struct k_mission_grant *g)
{
    g->cap_mask &= KCAP_WM | KCAP_SPAWN;
    for (unsigned i = 0; i < g->path_count; i++) {
        if (g->paths[i].mode & K_MISSION_READ) g->cap_mask |= KCAP_FS_READ;
        if (g->paths[i].mode & K_MISSION_WRITE) g->cap_mask |= KCAP_FS_WRITE | KCAP_FS_READ;
    }
    if (g->device_bits) g->cap_mask |= KCAP_FS_READ;
    if (g->device_bits & K_MISSION_DEVICE_AUDIO) g->cap_mask |= KCAP_FS_WRITE;
    if (g->net_mode) g->cap_mask |= KCAP_NET;
    if (g->ipc_bits) g->cap_mask |= KCAP_IPC;
}

int mission_build(const struct mission_manifest *m, const struct mission_options *o,
                  const char *home, mission_resolver resolve, void *ctx,
                  struct k_mission_grant *out, char *error, size_t error_size)
{
    struct k_mission_grant ceiling;
    if (!m || !out || m->declared.path_count > K_MISSION_PATHS ||
        m->declared.host_count > K_MISSION_HOSTS)
        return fail(error, error_size, "invalid manifest");
    ceiling = m->declared;
    for (unsigned i = 0; i < ceiling.path_count; i++) {
        char canon[64];
        if (canonical(ceiling.paths[i].prefix, home, canon) < 0)
            return fail(error, error_size, "cannot expand declared path");
        strcpy(ceiling.paths[i].prefix, canon);
    }
    caps(&ceiling);
    if (hosts(&ceiling, m->hosts, resolve, ctx) < 0)
        return fail(error, error_size, "cannot resolve declared host");
    *out = ceiling;
    if (o) {
        if (o->read_count > K_MISSION_PATHS || o->write_count > K_MISSION_PATHS ||
            o->host_count > K_MISSION_HOSTS || o->net_mode > K_MISSION_NET_HOSTS ||
            (o->drop_devices & ~15U)) return fail(error, error_size, "invalid narrowing options");
        memset(out->paths, 0, sizeof(out->paths)); out->path_count = 0;
        for (unsigned mode = 1; mode <= 2; mode++) {
            unsigned replace = mode == 1 ? o->read_set : o->write_set;
            unsigned count = mode == 1 ? o->read_count : o->write_count;
            const char (*paths)[64] = mode == 1 ? o->read : o->write;
            for (unsigned i = 0; i < (replace ? count : ceiling.path_count); i++) {
                char canon[64];
                if (!replace && !(ceiling.paths[i].mode & mode)) continue;
                if (canonical(replace ? paths[i] : ceiling.paths[i].prefix, home, canon) < 0 ||
                    path_add(out, canon, mode) < 0)
                    return fail(error, error_size, "invalid or too many granted paths");
            }
        }
        if (o->net_set) {
            out->net_mode = o->net_mode; out->host_count = o->host_count;
            memset(out->hosts, 0, sizeof(out->hosts));
            if ((out->net_mode != K_MISSION_NET_HOSTS && out->host_count) ||
                (out->net_mode == K_MISSION_NET_HOSTS && !out->host_count) ||
                hosts(out, o->hosts, resolve, ctx) < 0)
                return fail(error, error_size, "invalid or unresolved network grant");
        }
        out->device_bits &= ~o->drop_devices;
        if (o->no_ipc) out->ipc_bits = 0;
    }
    caps(out);
    if (out->cap_mask & ~ceiling.cap_mask)
        return fail(error, error_size, "grant widens capabilities");
    for (unsigned i = 0; i < out->path_count; i++) {
        unsigned allowed = 0;
        for (unsigned j = 0; j < ceiling.path_count; j++)
            if (beneath(out->paths[i].prefix, ceiling.paths[j].prefix))
                allowed |= ceiling.paths[j].mode;
        if (out->paths[i].mode & ~allowed) return fail(error, error_size, "grant widens filesystem paths");
    }
    if (out->net_mode && ceiling.net_mode != K_MISSION_NET_ANY) {
        if (ceiling.net_mode != K_MISSION_NET_HOSTS || out->net_mode != K_MISSION_NET_HOSTS)
            return fail(error, error_size, "grant widens network policy");
        for (unsigned i = 0; i < out->host_count; i++) {
            unsigned j;
            for (j = 0; j < ceiling.host_count; j++)
                if (out->hosts[i].ip_be == ceiling.hosts[j].ip_be) break;
            if (j == ceiling.host_count) return fail(error, error_size, "grant widens network hosts");
        }
    }
    if (error && error_size) error[0] = 0;
    return 0;
}
