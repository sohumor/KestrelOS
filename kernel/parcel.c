#include "kernel.h"
#include "parcel.h"
#include "kheap.h"
#include "pmm.h"
#include "proc.h"
#include "string.h"
#include "spinlock.h"
#include "vfs.h"

#include "pulse.h"

#define PARCEL_MAX_UIDS    8
#define PARCEL_PMM_RESERVE 64

struct parcel_slot {
    bool valid;
    struct k_parcel_info info;
    uint8_t *data;
};

struct parcel_bucket {
    uint32_t uid;
    struct parcel_slot slots[K_PARCEL_MAX_SLOTS];
};

static struct parcel_bucket *buckets[PARCEL_MAX_UIDS];
static uint32_t next_seq;
static spinlock_t parcel_lock = SPINLOCK_INIT;

static void copy_str_field(char *dst, size_t cap, const char *src)
{
    size_t i = 0;

    if (!dst || cap == 0)
        return;
    if (src) {
        for (; i + 1 < cap && src[i]; i++)
            dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* Caller holds parcel_lock. */
static struct parcel_bucket *find_bucket_locked(uint32_t uid)
{
    for (int i = 0; i < PARCEL_MAX_UIDS; i++) {
        if (buckets[i] && buckets[i]->uid == uid)
            return buckets[i];
    }
    return NULL;
}

static int find_free_bucket_slot_locked(void)
{
    for (int i = 0; i < PARCEL_MAX_UIDS; i++) {
        if (!buckets[i])
            return i;
    }
    return -1;
}

long parcel_put(const struct k_parcel_put *req, const void *kdata)
{
    struct k_parcel_info info;
    struct parcel_bucket *b, *new_bucket = NULL;
    uint8_t *payload = NULL, *evicted = NULL;
    uint32_t uid, id;
    uint64_t f;

    if (!req)
        return -1;
    if (req->type == K_PARCEL_NONE || req->type > K_PARCEL_IMAGE)
        return -1;
    if (req->size > K_PARCEL_MAX_BYTES)
        return -1;
    if (req->size > 0 && !kdata)
        return -1;

    if (req->size > 2048) {
        uint64_t pages = ((uint64_t)req->size + PAGE_SIZE - 1) / PAGE_SIZE + 1;
        if (pmm_free_pages() < pages + PARCEL_PMM_RESERVE)
            return -1;
    }

    if (req->size > 0) {
        payload = kmalloc(req->size);
        if (!payload)
            return -1;
        memcpy(payload, kdata, req->size);
    }

    uid = current ? current->uid : 0;
    memset(&info, 0, sizeof(info));
    info.type = req->type;
    info.size = req->size;
    info.time = rtc_unix_time();
    info.src_pid = current ? current->pid : 0;
    copy_str_field(info.mime, sizeof(info.mime), req->mime);
    copy_str_field(info.label, sizeof(info.label), req->label);

    f = spin_lock_irqsave(&parcel_lock);
    b = find_bucket_locked(uid);
    if (!b) {
        int slot = find_free_bucket_slot_locked();
        if (slot < 0) {
            spin_unlock_irqrestore(&parcel_lock, f);
            kfree(payload);
            return -1;
        }
        spin_unlock_irqrestore(&parcel_lock, f);

        new_bucket = kzalloc(sizeof(*new_bucket));
        if (!new_bucket) {
            kfree(payload);
            return -1;
        }
        new_bucket->uid = uid;

        f = spin_lock_irqsave(&parcel_lock);
        b = find_bucket_locked(uid);
        if (!b) {
            slot = find_free_bucket_slot_locked();
            if (slot < 0) {
                spin_unlock_irqrestore(&parcel_lock, f);
                kfree(new_bucket);
                kfree(payload);
                return -1;
            }
            buckets[slot] = new_bucket;
            b = new_bucket;
            new_bucket = NULL;
        }
    }

    id = 100 + next_seq++;
    info.id = id;

    if (b->slots[K_PARCEL_MAX_SLOTS - 1].valid)
        evicted = b->slots[K_PARCEL_MAX_SLOTS - 1].data;
    for (int i = K_PARCEL_MAX_SLOTS - 1; i > 0; i--)
        b->slots[i] = b->slots[i - 1];

    b->slots[0].valid = true;
    b->slots[0].info = info;
    b->slots[0].data = payload;
    spin_unlock_irqrestore(&parcel_lock, f);

    if (new_bucket)
        kfree(new_bucket);
    if (evicted)
        kfree(evicted);

    pulse_emit(K_TOPIC_PARCEL, K_PULSE_PARCEL_NEW, id, req->type,
                      info.label);
    return (long)id;
}

long parcel_get(uint32_t slot_or_id, struct k_parcel_info *info_out,
                void *buf_out, uint32_t bufsz)
{
    struct parcel_bucket *b;
    struct parcel_slot *s = NULL;
    uint32_t uid = current ? current->uid : 0;
    uint32_t copied = 0;
    uint64_t f;

    if (!info_out && (!buf_out || bufsz == 0))
        return -1;

    f = spin_lock_irqsave(&parcel_lock);
    b = find_bucket_locked(uid);
    if (!b) {
        spin_unlock_irqrestore(&parcel_lock, f);
        return -1;
    }

    if (slot_or_id < K_PARCEL_MAX_SLOTS) {
        if (b->slots[slot_or_id].valid)
            s = &b->slots[slot_or_id];
    } else if (slot_or_id >= 100) {
        for (int i = 0; i < K_PARCEL_MAX_SLOTS; i++) {
            if (b->slots[i].valid && b->slots[i].info.id == slot_or_id) {
                s = &b->slots[i];
                break;
            }
        }
    }

    if (!s) {
        spin_unlock_irqrestore(&parcel_lock, f);
        return -1;
    }

    if (info_out)
        *info_out = s->info;
    if (buf_out && bufsz > 0 && s->data && s->info.size > 0) {
        copied = s->info.size < bufsz ? s->info.size : bufsz;
        memcpy(buf_out, s->data, copied);
    }
    spin_unlock_irqrestore(&parcel_lock, f);
    return (long)copied;
}

long parcel_list(uint32_t index, struct k_parcel_info *info_out)
{
    struct parcel_bucket *b;
    uint32_t uid = current ? current->uid : 0;
    uint64_t f;

    if (!info_out || index >= K_PARCEL_MAX_SLOTS)
        return -1;

    f = spin_lock_irqsave(&parcel_lock);
    b = find_bucket_locked(uid);
    if (!b || !b->slots[index].valid) {
        spin_unlock_irqrestore(&parcel_lock, f);
        return -1;
    }
    *info_out = b->slots[index].info;
    spin_unlock_irqrestore(&parcel_lock, f);
    return 0;
}
