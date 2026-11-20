#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mission.h"

static struct k_mission_grant grant(void)
{
    struct k_mission_grant g = {0};
    strcpy(g.app, "Editor");
    strcpy(g.title, "Edit notes");
    g.cap_mask = KCAP_FS_READ | KCAP_FS_WRITE | KCAP_NET | KCAP_IPC;
    g.path_count = 2;
    strcpy(g.paths[0].prefix, "/home/pilot/notes");
    g.paths[0].mode = K_MISSION_READ | K_MISSION_WRITE;
    strcpy(g.paths[1].prefix, "/share");
    g.paths[1].mode = K_MISSION_READ;
    g.net_mode = K_MISSION_NET_HOSTS;
    g.host_count = 1;
    g.hosts[0].ip_be = 0x0100007f;
    g.device_bits = K_MISSION_DEVICE_RANDOM;
    g.ipc_bits = K_MISSION_IPC_TALON | K_MISSION_IPC_PULSE;
    return g;
}

static void narrowing(void)
{
    struct k_mission_grant p = grant(), g = p;
    assert(mission_grant_valid(&g));
    assert(mission_grant_subset(&g, &p));
    strcpy(g.paths[0].prefix, "/home/pilot/notes/private");
    g.paths[0].mode = K_MISSION_READ;
    g.device_bits = 0;
    g.ipc_bits = K_MISSION_IPC_PULSE;
    assert(mission_grant_subset(&g, &p));
    g.cap_mask |= KCAP_SPAWN;
    assert(!mission_grant_subset(&g, &p));
    g = p; g.paths[1].mode |= K_MISSION_WRITE;
    assert(!mission_grant_subset(&g, &p));
    g = p; strcpy(g.paths[0].prefix, "/home/pilot/notesX");
    assert(!mission_grant_subset(&g, &p));
    g = p; strcpy(g.paths[0].prefix, "/");
    assert(!mission_grant_subset(&g, &p));
    g = p; g.net_mode = K_MISSION_NET_ANY; g.host_count = 0;
    assert(!mission_grant_subset(&g, &p));
    g = p; g.hosts[0].ip_be++;
    assert(!mission_grant_subset(&g, &p));
    g = p; g.device_bits |= K_MISSION_DEVICE_CAMERA;
    assert(!mission_grant_subset(&g, &p));
    g = p; g.ipc_bits |= K_MISSION_IPC_SIGNAL;
    assert(!mission_grant_subset(&g, &p));
    g = p; p.paths[0].revoked = 1;
    assert(!mission_grant_subset(&g, &p));
    p = grant(); p.hosts[0].revoked = 1;
    assert(!mission_grant_subset(&g, &p));
    p = grant(); p.cap_mask &= ~KCAP_FS_WRITE;
    assert(!mission_grant_subset(&g, &p));
    memset(&g, 0, sizeof(g));
    assert(mission_grant_valid(&g));
    assert(mission_grant_subset(&g, &p));
    assert(!mission_grant_subset(NULL, &p));
    assert(!mission_grant_subset(&g, NULL));
    puts("mission: grant narrowing passed");
}

static void invalid(void)
{
    struct k_mission_grant g = grant();
    g.path_count = K_MISSION_PATHS + 1;
    assert(!mission_grant_valid(&g));
    g = grant(); g.host_count = K_MISSION_HOSTS + 1;
    assert(!mission_grant_valid(&g));
    g = grant(); memset(g.app, 'X', sizeof(g.app));
    assert(!mission_grant_valid(&g));
    g = grant(); memset(g.title, 'X', sizeof(g.title));
    assert(!mission_grant_valid(&g));
    g = grant(); memset(g.paths[0].prefix, 'X', sizeof(g.paths[0].prefix));
    assert(!mission_grant_valid(&g));
    g = grant(); strcpy(g.paths[0].prefix, "/home/pilot/notes/../secret");
    assert(!mission_grant_valid(&g));
    g = grant(); strcpy(g.paths[0].prefix, "~/notes");
    assert(!mission_grant_valid(&g));
    g = grant(); g.paths[0].mode = 0;
    assert(!mission_grant_valid(&g));
    g = grant(); g.paths[0].mode = 4;
    assert(!mission_grant_valid(&g));
    g = grant(); g.cap_mask |= KCAP_ADMIN;
    assert(!mission_grant_valid(&g));
    g = grant(); g.net_mode = K_MISSION_NET_NONE;
    assert(!mission_grant_valid(&g));
    g = grant(); g.net_mode = 3;
    assert(!mission_grant_valid(&g));
    g = grant(); g.reserved = 1;
    assert(!mission_grant_valid(&g));
    assert(!mission_grant_valid(NULL));
    puts("mission: malformed grants rejected");
}

static void paths(void)
{
    struct k_mission_info m = {0};
    m.grant = grant();
#define PATH(p, w) mission_core_path(&m, "/bin/edit", p, w)
    assert(PATH("/home/pilot/notes", 0));
    assert(PATH("/home/pilot/notes/file", 1));
    assert(PATH("/home//pilot/notes/./x/../file", 1));
    assert(!PATH("/home/pilot/notesX/file", 0));
    assert(!PATH("/home/pilot/notes/../secret", 0));
    assert(!PATH("/home/pilot/notes/../../../../etc/passwd", 0));
    assert(PATH("/share/readme", 0));
    assert(!PATH("/share/readme", 1));
    assert(!PATH("~/notes/file", 0));
    assert(!PATH("relative", 0));
    assert(!PATH("", 0));
    assert(!PATH(NULL, 0));
    assert(PATH("/bin/edit", 0));
    assert(!PATH("/bin/edit", 1));
    assert(!PATH("/bin/editX", 0));
    assert(PATH("/lib/libgreet.so", 0));
    assert(!PATH("/lib/libgreet.so", 1));
    assert(!PATH("/library/file", 0));
    assert(PATH("/etc/theme", 0));
    assert(PATH("/etc/desktop.conf", 0));
    assert(!PATH("/etc/theme.bak", 0));
    assert(!PATH("/etc/theme", 1));
    assert(PATH("/dev/null", 0));
    assert(PATH("/dev/null", 1));
    assert(PATH("/dev/urandom", 0));
    assert(!PATH("/dev/hda", 0));
    assert(!mission_core_path(&m, "/dev/hda", "/dev/hda", 0));
    strcpy(m.grant.paths[0].prefix, "/");
    assert(!PATH("/dev/hda", 0));
    assert(!PATH("/dev/../dev/camera", 0));
    m.grant = grant();
    m.grant.paths[0].revoked = 1;
    assert(!PATH("/home/pilot/notes/file", 0));
    m.revoked = 1ULL << K_MISSION_RANDOM;
    assert(!PATH("/dev/random", 0));
    m.revoked = 1ULL << K_MISSION_FS_READ;
    assert(!PATH("/share/readme", 0));
    m.revoked = 0;
    m.grant.cap_mask &= ~KCAP_FS_WRITE;
    assert(!PATH("/home/pilot/notes/file", 1));
    memset(&m, 0, sizeof(m));
    assert(!PATH("/home/pilot/notes/file", 0));
    assert(PATH("/bin/edit", 0));
    assert(PATH("/lib/runtime.so", 0));
    assert(PATH("/dev/null", 1));
    char canon[64], tiny[2];
    assert(mission_canon("/a/../../b//./c/..", canon, sizeof(canon)) == 0);
    assert(!strcmp(canon, "/b"));
    assert(mission_canon("/too-long", tiny, sizeof(tiny)) < 0);
    assert(mission_canon("/", canon, 1) < 0);
#undef PATH
    puts("mission: paths, runtime exceptions, devices and revoke passed");
}

static void hosts(void)
{
    struct k_mission_info m = {0};
    m.grant = grant();
    assert(mission_core_host(&m, 0x0100007f));
    assert(!mission_core_host(&m, 0x0200007f));
    m.grant.hosts[0].revoked = 1;
    assert(!mission_core_host(&m, 0x0100007f));
    m.grant.host_count = 0;
    assert(!mission_core_host(&m, 0x0100007f));
    m.grant.net_mode = K_MISSION_NET_ANY;
    assert(mission_core_host(&m, 0x0200007f));
    m.revoked = 1ULL << K_MISSION_NET;
    assert(!mission_core_host(&m, 0x0200007f));
    m.revoked = 0; m.grant.cap_mask &= ~KCAP_NET;
    assert(!mission_core_host(&m, 0x0200007f));
    memset(&m, 0, sizeof(m));
    assert(!mission_core_host(&m, 0));
    puts("mission: host allow/deny and revoke passed");
}

static void counters(void)
{
    struct k_mission_info m = {0};
    struct mission_rate rate = {0};
    assert(mission_core_count(&m, &rate, 0, 1, 0, "read"));
    assert(!mission_core_count(&m, &rate, 0, 1, 100, "read"));
    assert(!mission_core_count(&m, &rate, 0, 1, 249, "read"));
    assert(m.counters[0].last_use_tick == 249);
    assert(mission_core_count(&m, &rate, 0, 1, 250, "read"));
    assert(mission_core_count(&m, &rate, 0, 0, 0, "/secret"));
    assert(!mission_core_count(&m, &rate, 0, 0, 49, "/other"));
    assert(!strcmp(m.last_deny, "/other"));
    assert(mission_core_count(&m, &rate, 0, 0, 50, "/last"));
    assert(m.counters[0].uses == 4 && m.counters[0].denies == 3);
    assert(m.counters[0].last_use_tick == 250 && m.counters[0].last_deny_tick == 50);
    assert(mission_core_count(&m, &rate, 1, 1, 1, NULL));
    assert(mission_core_count(&m, &rate, 1, 0, 1, NULL));
    assert(!m.last_deny[0]);
    assert(!mission_core_count(&m, &rate, K_MISSION_CHANNELS, 0, 1000, "bad"));
    assert(mission_channel_cap(K_MISSION_DISPLAY) == KCAP_WM);
    assert(mission_cap_channel(KCAP_SPAWN) == K_MISSION_SPAWN);
    assert(mission_cap_channel(KCAP_ADMIN) == -1);
    assert(!mission_channel_cap(K_MISSION_RANDOM));
    puts("mission: counters and independent 250/50 ms pulse limits passed");
}

int main(void)
{
    narrowing(); invalid(); paths(); hosts(); counters();
    puts("mission: all tests passed");
    return 0;
}
