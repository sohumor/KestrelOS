#include <kestrel.h>
#include <mission.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    puts("usage: mission list | show <id> | ps");
    puts("       mission run <id> [--fs-read P]... [--fs-write P]...");
    puts("         [--no-net|--net-host H]... [--no-device D] [--no-ipc] [--bg] [-- args]");
    puts("       mission revoke <mid> <channel|path:N|host:N> | end <mid>");
}

static int number(const char *s, unsigned *out)
{
    uint64_t n = 0;
    if (!*s) return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || n > 0xffffffffU / 10U) return -1;
        n = n * 10 + (unsigned)(*s - '0');
        if (n > 0xffffffffU) return -1;
    }
    *out = (unsigned)n;
    return 0;
}

static int enabled(const struct k_mission_grant *g, unsigned c)
{
    static const uint64_t bits[] = {KCAP_FS_READ, KCAP_FS_WRITE, KCAP_NET, KCAP_WM, KCAP_SPAWN, KCAP_IPC};
    return c < K_MISSION_AUDIO ? (g->cap_mask & bits[c]) != 0 :
           (g->device_bits & (1U << (c - K_MISSION_AUDIO))) != 0;
}

static void print_grant(const struct k_mission_grant *g)
{
    printf("  capabilities:");
    for (unsigned c = 0; c < K_MISSION_CHANNELS; c++)
        if (enabled(g, c)) printf(" %s", mission_channel_name(c));
    printf("\n  ipc: 0x%x\n", g->ipc_bits);
    for (unsigned i = 0; i < g->path_count; i++)
        printf("  path:%u %s%s %s%s\n", i,
               g->paths[i].mode & K_MISSION_READ ? "r" : "",
               g->paths[i].mode & K_MISSION_WRITE ? "w" : "",
               g->paths[i].prefix, g->paths[i].revoked ? " revoked" : "");
    printf("  net: %s\n", g->net_mode == K_MISSION_NET_ANY ? "any" :
           g->net_mode == K_MISSION_NET_HOSTS ? "hosts" : "none");
    for (unsigned i = 0; i < g->host_count; i++) {
        char ip[16];
        printf("  host:%u %s%s\n", i, ip_ntoa(g->hosts[i].ip_be, ip),
               g->hosts[i].revoked ? " revoked" : "");
    }
}

static int list(void)
{
    struct k_dirent de;
    struct mission_manifest m;
    char error[96];
    for (int i = 0; readdir_at("/etc/missions", i, &de) == 0; i++) {
        size_t n = strlen(de.name);
        if (de.is_dir || n < 9 || strcmp(de.name + n - 8, ".mission")) continue;
        de.name[n - 8] = 0;
        if (mission_load(de.name, &m, error, sizeof(error)) < 0) {
            printf("%s: %s\n", de.name, error);
            return 1;
        }
        printf("%s: %s — %s\n", de.name, m.declared.app, m.declared.title);
    }
    return 0;
}

static int show(const char *id)
{
    struct mission_manifest m;
    struct k_mission_grant g;
    char error[96];
    if (mission_load(id, &m, error, sizeof(error)) < 0 ||
        mission_build_current(&m, NULL, &g, error, sizeof(error)) < 0) {
        printf("mission: %s\n", error); return 1;
    }
    printf("%s: %s\n  exec: %s\n  mission: %s\n", id, g.app, m.exec, g.title);
    print_grant(&g);
    return 0;
}

static int ps(void)
{
    struct k_mission_info m;
    unsigned count = 0;
    for (unsigned i = 0; mission_info(i, &m) == 0; i++) {
        count++;
        printf("mission %u app=%s tasks=%u pids=", m.id, m.grant.app, m.task_count);
        for (unsigned p = 0; p < m.pid_count; p++) printf("%s%d", p ? "," : "", m.pids[p]);
        printf("\n  mission: %s\n", m.grant.title);
        for (unsigned c = 0; c < K_MISSION_CHANNELS; c++)
            printf("  %s %s uses=%lu denies=%lu\n", mission_channel_name(c),
                   m.revoked & (1ULL << c) ? "revoked" : enabled(&m.grant, c) ? "active" : "off",
                   (unsigned long)m.counters[c].uses, (unsigned long)m.counters[c].denies);
        printf("  last_deny=%s\n", m.last_deny[0] ? m.last_deny : "-");
        print_grant(&m.grant);
    }
    if (!count) puts("no live missions");
    return 0;
}

static int add_arg(char list[][64], unsigned *count, unsigned max, const char *arg)
{
    if (*count >= max || strlen(arg) >= 64) return -1;
    strcpy(list[(*count)++], arg);
    return 0;
}

static int run(int argc, char **argv, char *cwd)
{
    struct mission_manifest *m = calloc(1, sizeof(*m));
    struct mission_options *o = calloc(1, sizeof(*o));
    struct k_mission_grant g;
    char error[96];
    int bg = 0, start = argc, result = 1;
    if (!m || !o) { puts("mission: out of memory"); goto done; }
    for (int i = 3; i < argc; i++) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--")) { start = i + 1; break; }
        if (!strcmp(arg, "--bg")) bg = 1;
        else if (!strcmp(arg, "--no-ipc")) o->no_ipc = 1;
        else if (!strcmp(arg, "--no-net")) {
            if (o->host_count) goto invalid;
            o->net_set = 1; o->net_mode = K_MISSION_NET_NONE;
        } else if (!strcmp(arg, "--fs-read") || !strcmp(arg, "--fs-write")) {
            int write = !strcmp(arg, "--fs-write");
            if (++i == argc) goto invalid;
            if (write) o->write_set = 1; else o->read_set = 1;
            if (add_arg(write ? o->write : o->read, write ? &o->write_count : &o->read_count,
                        K_MISSION_PATHS, argv[i]) < 0) goto invalid;
        } else if (!strcmp(arg, "--net-host")) {
            if (++i == argc || (o->net_set && o->net_mode == K_MISSION_NET_NONE)) goto invalid;
            o->net_set = 1; o->net_mode = K_MISSION_NET_HOSTS;
            if (add_arg(o->hosts, &o->host_count, K_MISSION_HOSTS, argv[i]) < 0) goto invalid;
        } else if (!strcmp(arg, "--no-device")) {
            if (++i == argc) goto invalid;
            unsigned bit = mission_device_bit(argv[i]);
            if (!bit) goto invalid;
            o->drop_devices |= bit;
        } else goto invalid;
    }
    if (mission_load(argv[2], m, error, sizeof(error)) < 0 ||
        mission_build_current(m, o, &g, error, sizeof(error)) < 0) {
        printf("mission: %s\n", error); goto done;
    }
    char *child[17];
    int n = 0;
    child[n++] = m->exec;
    for (int i = start; i < argc; i++) {
        if (n >= 15) goto invalid;
        child[n++] = argv[i];
    }
    if (cwd) child[n++] = cwd;
    child[n] = NULL;
    uint32_t mid;
    int pid = mission_spawn_id(m->exec, child, &g, &mid);
    if (pid < 0) { puts("mission: launch denied or mission table full"); goto done; }
    if (bg) { printf("mission %u pid %d\n", mid, pid); result = 0; }
    else result = waitpid(pid);
    goto done;
invalid:
    puts("mission: invalid, conflicting or excessive options/arguments"); usage();
done:
    free(o); free(m);
    return result;
}

int main(int argc, char **argv)
{
    char *cwd = NULL;
    if (argc > 1 && !strncmp(argv[argc - 1], "--cwd=", 6)) cwd = argv[--argc];
    if (argc == 2 && !strcmp(argv[1], "list")) return list();
    if (argc == 2 && !strcmp(argv[1], "ps")) return ps();
    if (argc == 3 && !strcmp(argv[1], "show")) return show(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "run")) return run(argc, argv, cwd);
    unsigned id, arg = 0, op;
    if (argc >= 3 && number(argv[2], &id) == 0 && id) {
        if (argc == 3 && !strcmp(argv[1], "end")) op = K_MISSION_END;
        else if (argc == 4 && !strcmp(argv[1], "revoke")) {
            int c = mission_channel_index(argv[3]);
            if (c >= 0) { op = K_MISSION_REVOKE_CAP; arg = (unsigned)c; }
            else if (!strncmp(argv[3], "path:", 5) && number(argv[3] + 5, &arg) == 0)
                op = K_MISSION_REVOKE_PATH;
            else if (!strncmp(argv[3], "host:", 5) && number(argv[3] + 5, &arg) == 0)
                op = K_MISSION_REVOKE_HOST;
            else { usage(); return 1; }
        } else { usage(); return 1; }
        if (mission_ctl(id, op, arg) < 0) { puts("mission: control denied or invalid target"); return 1; }
        printf("mission %u: %s%s\n", id, op == K_MISSION_END ? "ending" : "revoked ",
               op == K_MISSION_END ? "" : argv[3]);
        return 0;
    }
    usage();
    return 1;
}
