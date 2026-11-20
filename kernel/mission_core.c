#include "mission.h"

static int equal(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int terminated(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (!s[i]) return 1;
    return 0;
}

int mission_canon(const char *path, char *out, size_t size)
{
    size_t n = 1;
    if (!path || *path != '/' || size < 2) return -1;
    out[0] = '/'; out[1] = 0;
    while (*path) {
        while (*path == '/') path++;
        const char *p = path;
        while (*path && *path != '/') path++;
        size_t len = (size_t)(path - p);
        if (!len) break;
        if (len == 1 && p[0] == '.') continue;
        if (len == 2 && p[0] == '.' && p[1] == '.') {
            while (n > 1 && out[n - 1] != '/') n--;
            if (n > 1) n--;
        } else {
            if (n + len + (n > 1) >= size) return -1;
            if (n > 1) out[n++] = '/';
            for (size_t i = 0; i < len; i++) out[n++] = p[i];
        }
        out[n] = 0;
    }
    return 0;
}

static int beneath(const char *path, const char *prefix)
{
    if (prefix[0] == '/' && !prefix[1]) return 1;
    while (*prefix && *prefix == *path) { prefix++; path++; }
    return !*prefix && (!*path || *path == '/');
}

uint64_t mission_channel_cap(unsigned c)
{
    static const uint64_t caps[] = {
        KCAP_FS_READ, KCAP_FS_WRITE, KCAP_NET, KCAP_WM, KCAP_SPAWN, KCAP_IPC
    };
    return c < 6 ? caps[c] : 0;
}

int mission_cap_channel(uint64_t cap)
{
    for (int c = 0; c < 6; c++)
        if (mission_channel_cap((unsigned)c) == cap) return c;
    return -1;
}

int mission_device_channel(const char *path)
{
    static const struct { const char *path; int channel; } devices[] = {
        {"/dev/audio", K_MISSION_AUDIO}, {"/dev/mixer", K_MISSION_AUDIO},
        {"/dev/camera", K_MISSION_CAMERA}, {"/dev/mouse", K_MISSION_INPUT},
        {"/dev/keyboard", K_MISSION_INPUT}, {"/dev/random", K_MISSION_RANDOM},
        {"/dev/urandom", K_MISSION_RANDOM}
    };
    for (unsigned i = 0; i < sizeof(devices) / sizeof(devices[0]); i++)
        if (equal(path, devices[i].path)) return devices[i].channel;
    return -1;
}

int mission_grant_valid(const struct k_mission_grant *g)
{
    char canon[64];
    if (!g || !terminated(g->app, sizeof(g->app)) ||
        !terminated(g->title, sizeof(g->title)) ||
        g->path_count > K_MISSION_PATHS || g->host_count > K_MISSION_HOSTS ||
        g->net_mode > K_MISSION_NET_HOSTS || (g->cap_mask & ~KCAP_USER_DEF) ||
        (g->device_bits & ~15U) || (g->ipc_bits & ~K_MISSION_IPC_ALL) || g->reserved)
        return 0;
    if ((g->net_mode == K_MISSION_NET_NONE) != !(g->cap_mask & KCAP_NET)) return 0;
    if (g->net_mode != K_MISSION_NET_HOSTS && g->host_count) return 0;
    if (!(g->cap_mask & KCAP_IPC) && g->ipc_bits) return 0;
    for (unsigned i = 0; i < g->path_count; i++) {
        const struct k_mission_path *p = &g->paths[i];
        if (!terminated(p->prefix, sizeof(p->prefix)) ||
            mission_canon(p->prefix, canon, sizeof(canon)) < 0 ||
            !equal(canon, p->prefix) || !p->mode || (p->mode & ~3U) || p->revoked)
            return 0;
    }
    for (unsigned i = 0; i < g->host_count; i++)
        if (g->hosts[i].revoked) return 0;
    return 1;
}

int mission_grant_subset(const struct k_mission_grant *g,
                         const struct k_mission_grant *p)
{
    if (!mission_grant_valid(g) || !p || (g->cap_mask & ~p->cap_mask) ||
        (g->device_bits & ~p->device_bits) || (g->ipc_bits & ~p->ipc_bits)) return 0;
    for (unsigned i = 0; i < g->path_count; i++) {
        unsigned modes = 0;
        for (unsigned j = 0; j < p->path_count; j++)
            if (!p->paths[j].revoked && beneath(g->paths[i].prefix, p->paths[j].prefix))
                modes |= p->paths[j].mode;
        if (g->paths[i].mode & ~modes) return 0;
    }
    if (g->net_mode == K_MISSION_NET_NONE) return 1;
    if (p->net_mode == K_MISSION_NET_ANY) return 1;
    if (p->net_mode != K_MISSION_NET_HOSTS || g->net_mode != K_MISSION_NET_HOSTS) return 0;
    for (unsigned i = 0; i < g->host_count; i++) {
        int found = 0;
        for (unsigned j = 0; j < p->host_count; j++)
            if (!p->hosts[j].revoked && g->hosts[i].ip_be == p->hosts[j].ip_be) found = 1;
        if (!found) return 0;
    }
    return 1;
}

int mission_core_path(const struct k_mission_info *m, const char *exec,
                      const char *path, int write)
{
    char canon[256];
    const struct k_mission_grant *g = &m->grant;
    unsigned c = write ? K_MISSION_FS_WRITE : K_MISSION_FS_READ;
    if (mission_canon(path, canon, sizeof(canon)) < 0) return 0;
    int mode_allowed = !(m->revoked & (1ULL << c)) &&
                       (g->cap_mask & mission_channel_cap(c));
    if (beneath(canon, "/dev")) {
        if (equal(canon, "/dev/null")) return 1;
        int d = mission_device_channel(canon);
        return mode_allowed && d >= 0 &&
               (g->device_bits & (1U << (d - K_MISSION_AUDIO))) &&
               !(m->revoked & (1ULL << d));
    }
    if (!write && ((exec && equal(canon, exec)) || beneath(canon, "/lib") ||
                   equal(canon, "/etc/desktop.conf") || equal(canon, "/etc/theme"))) return 1;
    if (!mode_allowed) return 0;
    for (unsigned i = 0; i < g->path_count; i++)
        if (!g->paths[i].revoked && (g->paths[i].mode & (write ? 2U : 1U)) &&
            beneath(canon, g->paths[i].prefix)) return 1;
    return 0;
}

int mission_core_host(const struct k_mission_info *m, uint32_t ip)
{
    const struct k_mission_grant *g = &m->grant;
    if ((m->revoked & (1ULL << K_MISSION_NET)) || !(g->cap_mask & KCAP_NET)) return 0;
    if (g->net_mode == K_MISSION_NET_ANY) return 1;
    if (g->net_mode == K_MISSION_NET_HOSTS)
        for (unsigned i = 0; i < g->host_count; i++)
            if (!g->hosts[i].revoked && g->hosts[i].ip_be == ip) return 1;
    return 0;
}

int mission_core_count(struct k_mission_info *m, struct mission_rate *rate, unsigned c, int allow,
                       uint64_t now, const char *detail)
{
    if (c >= K_MISSION_CHANNELS) return 0;
    struct k_mission_counter *p = &m->counters[c];
    uint64_t *count = allow ? &p->uses : &p->denies;
    uint64_t *last = allow ? &p->last_use_tick : &p->last_deny_tick;
    uint64_t *pulse = allow ? &rate->use[c] : &rate->deny[c];
    int emit = !*count || now - *pulse >= (allow ? 250U : 50U);
    (*count)++;
    *last = now;
    if (emit) *pulse = now;
    if (!allow) {
        size_t i = 0;
        if (detail) for (; i + 1 < sizeof(m->last_deny) && detail[i]; i++)
            m->last_deny[i] = detail[i];
        m->last_deny[i] = 0;
    }
    return emit;
}
