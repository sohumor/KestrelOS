#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <mission.h>

static const char manifest[] =
    "# test ceiling\r\n"
    "app: Editor\nexec: /bin/edit\nmission: Edit selected notes\n"
    "display: yes # windows\nfs.read: ~/notes /doc\nfs.write: ~/notes\n"
    "net: host example.test 10.0.2.2\ndevice: random audio\n"
    "ipc: talon pulse signal pipe\nspawn: yes\n";
static char error[96];
static unsigned queries;

static int resolve(const char *name, uint32_t *ip, void *ctx)
{
    assert(ctx == &queries);
    queries++;
    if (!strcmp(name, "example.test") || !strcmp(name, "alias.test")) {
        *ip = 0x04030201U;
        return 0;
    }
    return -1;
}

static void parse(void)
{
    struct mission_manifest m;
    assert(mission_parse(manifest, strlen(manifest), &m, error, sizeof(error)) == 0);
    assert(!strcmp(m.exec, "/bin/edit") && !strcmp(m.declared.app, "Editor"));
    assert(m.declared.path_count == 2 && m.declared.paths[0].mode == 3);
    assert(m.declared.ipc_bits == K_MISSION_IPC_ALL);
    assert(m.declared.device_bits == (K_MISSION_DEVICE_RANDOM | K_MISSION_DEVICE_AUDIO));
    assert(m.declared.net_mode == K_MISSION_NET_HOSTS && m.declared.host_count == 2);
    assert(!queries); /* Parsing does not perform DNS. */
    struct k_mission_grant g;
    assert(mission_build(&m, NULL, "/home/pilot", resolve, &queries, &g, error, sizeof(error)) == 0);
    assert(queries == 1 && g.hosts[0].ip_be == 0x04030201U && g.hosts[1].ip_be == 0x0202000aU);
    assert(!strcmp(g.paths[0].prefix, "/home/pilot/notes"));
    assert(g.cap_mask == KCAP_USER_DEF);
    assert(!error[0]);
    puts("mission manifest: parse, home expansion and grant-time DNS passed");
}

static void narrowing(void)
{
    struct mission_manifest m;
    struct mission_options o = {0};
    struct k_mission_grant g;
    assert(mission_parse(manifest, strlen(manifest), &m, error, sizeof(error)) == 0);
    o.read_set = o.write_set = o.net_set = 1;
    o.read_count = o.write_count = o.host_count = 1;
    strcpy(o.read[0], "~/notes/private/../today");
    strcpy(o.write[0], "~/notes/today");
    o.net_mode = K_MISSION_NET_HOSTS;
    strcpy(o.hosts[0], "alias.test");
    o.drop_devices = K_MISSION_DEVICE_AUDIO;
    o.no_ipc = 1;
#define BUILD() mission_build(&m, &o, "/home/pilot", resolve, &queries, &g, error, sizeof(error))
    assert(BUILD() == 0);
    assert(g.path_count == 1 && g.paths[0].mode == 3);
    assert(!strcmp(g.paths[0].prefix, "/home/pilot/notes/today"));
    assert(g.device_bits == K_MISSION_DEVICE_RANDOM && !g.ipc_bits && !(g.cap_mask & KCAP_IPC));
    assert(g.host_count == 1 && g.hosts[0].ip_be == 0x04030201U);
    o.drop_caps = KCAP_WM | KCAP_SPAWN;
    o.no_ipc = 0; o.drop_ipc = K_MISSION_IPC_TALON;
    assert(BUILD() == 0);
    assert(!(g.cap_mask & (KCAP_WM | KCAP_SPAWN)));
    assert(g.ipc_bits == (K_MISSION_IPC_ALL & ~K_MISSION_IPC_TALON));
    o.drop_caps = KCAP_NET; assert(BUILD() < 0);
    o.drop_caps = 0; o.drop_ipc = 16; assert(BUILD() < 0);
    o.drop_ipc = 0;
    strcpy(o.read[0], "~/notesX"); assert(BUILD() < 0);
    strcpy(o.read[0], "~/notes/../secret"); assert(BUILD() < 0);
    strcpy(o.read[0], "/doc"); strcpy(o.write[0], "/doc"); assert(BUILD() < 0);
    strcpy(o.write[0], "~/notes"); strcpy(o.hosts[0], "1.2.3.5"); assert(BUILD() < 0);
    strcpy(o.hosts[0], "missing.test"); assert(BUILD() < 0);
    o.net_mode = K_MISSION_NET_ANY; o.host_count = 0; assert(BUILD() < 0);
    o.net_mode = K_MISSION_NET_NONE; assert(BUILD() == 0);
    assert(!(g.cap_mask & KCAP_NET));
    o.read_count = o.write_count = 0; assert(BUILD() == 0);
    assert(g.path_count == 0);
    o.read_count = K_MISSION_PATHS + 1; assert(BUILD() < 0);
#undef BUILD
    const char *none = "app: Minimal\nexec: /bin/gcalc\nmission: Calculate\n";
    assert(mission_parse(none, strlen(none), &m, error, sizeof(error)) == 0);
    memset(&o, 0, sizeof(o));
    assert(mission_build(&m, NULL, "/", NULL, NULL, &g, error, sizeof(error)) == 0);
    assert(!g.cap_mask && !g.path_count);
    o.net_set = 1; o.net_mode = K_MISSION_NET_HOSTS; o.host_count = 1;
    strcpy(o.hosts[0], "10.0.2.2");
    assert(mission_build(&m, &o, "/", NULL, NULL, &g, error, sizeof(error)) < 0);
    puts("mission manifest: narrow-only paths, modes, hosts, device and IPC options passed");
}

static void invalid(void)
{
    const char *bad[] = {
        "", "# comment only", "app: x\nexec: /bin/x", "app x",
        "app: x\nexec: /bin/x\nmission: x\nunknown: yes",
        "app: x\nexec: /bin/x\nmission: x\napp: duplicate",
        "app: x\nexec: relative\nmission: x",
        "app: x\nexec: /bin/x\nmission: x\ndisplay: maybe",
        "app: x\nexec: /bin/x\nmission: x\nfs.read: ~another/notes",
        "app: x\nexec: /bin/x\nmission: x\nfs.write: relative",
        "app: x\nexec: /bin/x\nmission: x\nnet: host",
        "app: x\nexec: /bin/x\nmission: x\nnet: sometimes",
        "app: x\nexec: /bin/x\nmission: x\nnet: host a b c d e",
        "app: x\nexec: /bin/x\nmission: x\ndevice: disk",
        "app: x\nexec: /bin/x\nmission: x\nipc: none pulse",
        "app: x\nexec: /bin/x\nmission: x\nspawn: yes please",
        "app: x\nexec: /bin/x\nmission: x\nfs.read: /1 /2 /3 /4 /5 /6 /7 /8 /9",
    };
    struct mission_manifest m;
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        assert(mission_parse(bad[i], strlen(bad[i]), &m, error, sizeof(error)) < 0);
        assert(error[0]);
    }
    char oversized[MISSION_TEXT_MAX + 1];
    memset(oversized, 'x', sizeof(oversized));
    assert(mission_parse(oversized, sizeof(oversized), &m, error, sizeof(error)) < 0);
    assert(mission_parse("app: x\0hidden", 13, &m, error, sizeof(error)) < 0);
    assert(mission_parse(NULL, 10, &m, error, sizeof(error)) < 0);
    puts("mission manifest: empty, malformed, unknown, duplicate and oversized inputs rejected");
}

static void shipped(void)
{
    static const char *const ids[] = {
        "sight", "notes", "kjs", "edit", "browser", "terminal", "gfiles",
        "sysmon", "gpaint", "gcalc", "gclock", "settings", "about",
        "cat", "curl", "ping", "sh", "missionprobe"
    };
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        char path[128], text[MISSION_TEXT_MAX + 1];
        snprintf(path, sizeof(path), "rootfs/etc/missions/%s.mission", ids[i]);
        FILE *f = fopen(path, "rb");
        assert(f);
        size_t n = fread(text, 1, sizeof(text), f);
        assert(!ferror(f) && n < sizeof(text)); fclose(f);
        struct mission_manifest m;
        struct k_mission_grant g;
        assert(mission_parse(text, n, &m, error, sizeof(error)) == 0);
        assert(mission_build(&m, NULL, "/home/kestrel", NULL, NULL, &g, error, sizeof(error)) == 0);
        assert((g.cap_mask & ~KCAP_USER_DEF) == 0);
    }
    puts("mission manifest: all 18 shipped manifests parsed and built");
}

int main(void)
{
    parse(); narrowing(); invalid(); shipped();
    puts("mission manifest: all tests passed");
    return 0;
}
