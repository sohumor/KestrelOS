#pragma once
#include <stddef.h>
#include "kestrel_abi.h"

int mission_canon(const char *path, char *out, size_t size);
int mission_grant_valid(const struct k_mission_grant *g);
int mission_grant_subset(const struct k_mission_grant *g,
                         const struct k_mission_grant *ceiling);
int mission_core_path(const struct k_mission_info *m, const char *exec,
                      const char *path, int write);
int mission_core_host(const struct k_mission_info *m, uint32_t ip);
struct mission_rate {
    uint64_t use[K_MISSION_CHANNELS], deny[K_MISSION_CHANNELS];
};
int mission_core_count(struct k_mission_info *m, struct mission_rate *rate, unsigned channel, int allow,
                       uint64_t tick_ms, const char *detail);
uint64_t mission_channel_cap(unsigned channel);
int mission_cap_channel(uint64_t cap);
int mission_device_channel(const char *canonical_path);

struct task;
int mission_create(const struct k_mission_grant *g, const char *exec);
void mission_put(int id);
void mission_inherit(struct task *child, const struct task *parent);
void mission_limit_current(void);
int mission_cap(uint64_t cap, int allow);
int mission_path(const char *path, int write);
int mission_host(uint32_t ip);
int mission_ipc(unsigned bit);
int mission_device(unsigned channel);
int mission_ctl(int id, int op, unsigned arg);
int mission_info(unsigned index, struct k_mission_info *out);
uint32_t mission_snapshot_krec(char *buf, uint32_t cap);
