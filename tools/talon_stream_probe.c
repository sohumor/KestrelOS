#include "../kernel/parcel.c"
#include "initcall.h"

#define WATCH_UID 4101
#define FOREIGN_UID 4102

static int watch_result;

static bool put(uint32_t type, const char *label)
{
    struct k_parcel_put req = { .type = type };
    copy_str_field(req.label, sizeof(req.label), label);
    return parcel_put(&req, NULL) >= 0;
}

static void watch_talon(void *arg)
{
    (void)arg;
    current->uid = WATCH_UID;
    uint64_t cursor = 0;
    struct parcel_change event;
    bool ok = parcel_watch(&cursor, &event, 1, 2000) == 1 &&
              cursor == 1 && event.seq == 1 &&
              event.info.type == K_PARCEL_TEXT &&
              strcmp(event.info.label, "own-live") == 0;
    __atomic_store_n(&watch_result, ok ? 1 : -1, __ATOMIC_RELEASE);
}

static void watch_existing_talon(void *arg)
{
    current->uid = WATCH_UID;
    uint64_t cursor = (uintptr_t)arg;
    struct parcel_change event;
    bool ok = parcel_watch(&cursor, &event, 1, 2000) == 1 &&
              event.seq == (uintptr_t)arg + 1 && cursor == event.seq &&
              strcmp(event.info.label, "existing-lane") == 0;
    __atomic_store_n(&watch_result, ok ? 1 : -1, __ATOMIC_RELEASE);
}

static bool parked(int pid)
{
    struct k_psinfo info;
    for (int i = 0; task_psinfo(i, &info) == 0; i++)
        if (info.pid == pid)
            return info.state == K_STATE_SLEEPING;
    return false;
}

static bool check_stream(void)
{
    int pid;
    if (!kthread_create_with_pid(watch_talon, NULL, "talon-watch", &pid))
        return false;

    uint64_t deadline = timer_ticks() + 50;
    while (!parked(pid) && timer_ticks() < deadline)
        task_sleep_ticks(1);
    uint64_t flags = spin_lock_irqsave(&parcel_lock);
    bool waiting_without_bucket = parcel_watchers && !find_bucket_locked(WATCH_UID);
    spin_unlock_irqrestore(&parcel_lock, flags);
    if (!waiting_without_bucket)
        return false;

    current->uid = FOREIGN_UID;
    struct parcel_change changes[K_PARCEL_MAX_SLOTS];
    uint64_t cursor = 0;
    if (!put(K_PARCEL_URL, "foreign") || !parked(pid) ||
        parcel_watch(&cursor, changes, 1, 0) != 1 || cursor != 1 ||
        strcmp(changes[0].info.label, "foreign") != 0)
        return false;

    current->uid = WATCH_UID;
    if (!put(K_PARCEL_TEXT, "own-live"))
        return false;
    deadline = timer_ticks() + 50;
    while (!__atomic_load_n(&watch_result, __ATOMIC_ACQUIRE) &&
           timer_ticks() < deadline)
        task_sleep_ticks(1);
    if (__atomic_load_n(&watch_result, __ATOMIC_ACQUIRE) != 1)
        return false;

    for (int i = 0; i < K_PARCEL_MAX_SLOTS + 2; i++)
        if (!put(K_PARCEL_NUMBER, "retained"))
            return false;
    cursor = 0;
    struct k_parcel_info info;
    if (parcel_watch(&cursor, changes, 2, 0) != 2 || cursor != 2 ||
        changes[0].seq != 1 || changes[1].seq != 2 ||
        strcmp(changes[0].info.label, "own-live") != 0 ||
        changes[1].info.type != K_PARCEL_NUMBER ||
        parcel_get(changes[0].info.id, &info, NULL, 0) >= 0)
        return false;

    for (int i = 0; i < PARCEL_HISTORY; i++)
        if (!put(K_PARCEL_NUMBER, "retained"))
            return false;
    const uint64_t latest = 1 + K_PARCEL_MAX_SLOTS + 2 + PARCEL_HISTORY;
    cursor = 0;
    if (parcel_watch(&cursor, changes, 2, 0) != 2 ||
        changes[0].seq != latest - PARCEL_HISTORY + 1 ||
        changes[1].seq != changes[0].seq + 1)
        return false;
    for (int left = PARCEL_HISTORY - 2; left > 0;) {
        int batch = left < K_PARCEL_MAX_SLOTS ? left : K_PARCEL_MAX_SLOTS;
        if (parcel_watch(&cursor, changes, K_PARCEL_MAX_SLOTS, 0) != batch)
            return false;
        left -= batch;
    }
    if (cursor != latest || parcel_watch(&cursor, changes, 1, 30) != 0 ||
        cursor != latest)
        return false;

    __atomic_store_n(&watch_result, 0, __ATOMIC_RELEASE);
    if (!kthread_create_with_pid(watch_existing_talon, (void *)(uintptr_t)latest,
                                 "talon-watch", &pid))
        return false;
    deadline = timer_ticks() + 50;
    while (!parked(pid) && timer_ticks() < deadline)
        task_sleep_ticks(1);
    if (!parked(pid))
        return false;
    current->uid = FOREIGN_UID;
    if (!put(K_PARCEL_URL, "foreign-lane") || !parked(pid))
        return false;
    current->uid = WATCH_UID;
    if (!put(K_PARCEL_TEXT, "existing-lane"))
        return false;
    deadline = timer_ticks() + 50;
    while (!__atomic_load_n(&watch_result, __ATOMIC_ACQUIRE) &&
           timer_ticks() < deadline)
        task_sleep_ticks(1);
    return __atomic_load_n(&watch_result, __ATOMIC_ACQUIRE) == 1;
}

static int talon_stream_probe(void)
{
    uint32_t uid = current->uid;
    bool ok = check_stream();
    current->uid = uid;
    if (!ok) {
        kprintf("FAIL Talon owner stream\n");
        return -1;
    }
    kprintf("PASS Talon owner stream: UID isolation, live wake, replay gap and timeout\n");
    return 0;
}

late_initcall(talon_stream_probe);
