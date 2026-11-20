#include "kernel.h"
#include "pulse.h"
#include "proc.h"
#include "timer.h"
#include "string.h"
#include "spinlock.h"
#include "flight.h"

#define PULSE_RING_SIZE 128
#define PULSE_MAX_OUT   64

static struct k_pulse pulse_ring[PULSE_RING_SIZE];
static uint64_t next_seq = 1;
static spinlock_t pulse_lock = SPINLOCK_INIT;
static struct task_event_channel pulse_channel;

void pulse_emit(uint32_t topic, uint32_t kind, uint32_t arg0, uint32_t arg1,
                const char *tag)
{
    uint64_t f = spin_lock_irqsave(&pulse_lock);
    uint64_t seq = next_seq++;
    struct k_pulse *p = &pulse_ring[(seq - 1) % PULSE_RING_SIZE];

    p->seq = seq;
    p->topic = topic;
    p->kind = kind;
    p->arg0 = arg0;
    p->arg1 = arg1;
    memset(p->tag, 0, sizeof(p->tag));
    if (tag) {
        for (size_t i = 0; i + 1 < sizeof(p->tag) && tag[i]; i++)
            p->tag[i] = tag[i];
    }
    task_event_notify(&pulse_channel, topic);
    spin_unlock_irqrestore(&pulse_lock, f);
    flight_record(FLIGHT_CAT_PULSE, "pulse_emit",
                  current ? (int16_t)current->pid : 0,
                  (uint64_t)kind, (uint64_t)topic, tag ? tag : "");
}

static uint32_t pulse_drain_locked(struct k_pulse_query *q, struct k_pulse *out,
                                   uint32_t limit)
{
    uint64_t latest = next_seq - 1;
    uint32_t count = 0;

    if (limit > 0 && latest > q->seq) {
        uint64_t oldest = latest > PULSE_RING_SIZE
                        ? latest - PULSE_RING_SIZE + 1 : 1;
        uint64_t start = q->seq + 1;
        if (start < oldest)
            start = oldest;

        for (uint64_t s = start; s <= latest && count < limit; s++) {
            const struct k_pulse *e = &pulse_ring[(s - 1) % PULSE_RING_SIZE];
            if (e->seq == s && (e->topic & q->topic_mask))
                out[count++] = *e;
        }
    }

    if (count > 0 && count == limit)
        q->seq = out[count - 1].seq;
    else
        q->seq = latest;

    return count;
}

long pulse_poll(struct k_pulse_query *q, struct k_pulse *out)
{
    if (!q)
        return -1;

    uint32_t limit = q->max_out;
    if (limit > PULSE_MAX_OUT)
        limit = PULSE_MAX_OUT;
    if (limit > 0 && !out)
        return -1;

    uint64_t f = spin_lock_irqsave(&pulse_lock);
    uint32_t count = pulse_drain_locked(q, out, limit);
    if (count > 0 || limit == 0 || q->timeout_ms == 0) {
        spin_unlock_irqrestore(&pulse_lock, f);
        return (long)count;
    }

    uint64_t timeout_ms = q->timeout_ms;
    if (timeout_ms > (1ULL << 40))
        timeout_ms = 1ULL << 40;
    uint64_t ticks = (timeout_ms * TIMER_HZ + 999) / 1000;
    if (ticks == 0)
        ticks = 1;
    uint64_t deadline = timer_ticks() + ticks;

    while (timer_ticks() < deadline) {
        if (task_event_wait_locked(&pulse_channel, q->topic_mask, deadline,
                                   &pulse_lock, f) < 0)
            return -1;
        f = spin_lock_irqsave(&pulse_lock);
        count = pulse_drain_locked(q, out, limit);
        if (count > 0)
            break;
    }
    spin_unlock_irqrestore(&pulse_lock, f);
    return (long)count;
}
