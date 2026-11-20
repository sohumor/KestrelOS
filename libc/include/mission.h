#pragma once
#include <stddef.h>
#include <kestrel_abi.h>

#define MISSION_TEXT_MAX 4096
#define MISSION_HOST_NAME 64

struct mission_manifest {
    char exec[128];
    struct k_mission_grant declared;
    char hosts[K_MISSION_HOSTS][MISSION_HOST_NAME];
};

struct mission_options {
    unsigned read_set, write_set, read_count, write_count;
    char read[K_MISSION_PATHS][64], write[K_MISSION_PATHS][64];
    unsigned net_set, net_mode, host_count;
    char hosts[K_MISSION_HOSTS][MISSION_HOST_NAME];
    unsigned drop_devices, no_ipc;
    unsigned drop_ipc;
    uint64_t drop_caps;
};

typedef int (*mission_resolver)(const char *name, uint32_t *ip, void *ctx);
int mission_parse(const char *text, size_t len, struct mission_manifest *out,
                  char *error, size_t error_size);
int mission_build(const struct mission_manifest *manifest,
                  const struct mission_options *options, const char *home,
                  mission_resolver resolve, void *ctx, struct k_mission_grant *out,
                  char *error, size_t error_size);
int mission_load(const char *id, struct mission_manifest *out,
                 char *error, size_t error_size);
int mission_build_current(const struct mission_manifest *manifest,
                          const struct mission_options *options,
                          struct k_mission_grant *out, char *error, size_t error_size);
int mission_spawn(const char *path, char *const argv[], const struct k_mission_grant *grant);
int mission_spawn_id(const char *path, char *const argv[], const struct k_mission_grant *grant,
                     uint32_t *mission_id);
int mission_ctl(uint32_t id, unsigned op, unsigned arg);
int mission_info(unsigned index, struct k_mission_info *out);
const char *mission_channel_name(unsigned channel);
int mission_channel_index(const char *name);
unsigned mission_device_bit(const char *name);
