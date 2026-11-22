#include "kernel.h"
#include "pulse.h"
#include "pulse_stream.h"
#include "timer.h"
#include "flight.h"

#define PULSE_RING_SIZE 128
#define PULSE_MAX_OUT   64

static struct k_pulse pulse_ring[PULSE_RING_SIZE];
static uint32_t pulse_topics[PULSE_RING_SIZE];
static spinlock_t pulse_lock = SPINLOCK_INIT;
static struct pulse_stream system_pulse =
    PULSE_STREAM_INIT(&pulse_lock, pulse_ring, pulse_topics);

_Static_assert(offsetof(struct k_pulse, seq) == 0, "Pulse sequence prefix");

void pulse_emit(uint32_t topic, uint32_t kind, uint32_t arg0, uint32_t arg1,
                const char *tag)
{
    struct k_pulse p = {
        .topic = topic, .kind = kind, .arg0 = arg0, .arg1 = arg1,
    };
    if (tag) {
        for (size_t i = 0; i + 1 < sizeof(p.tag) && tag[i]; i++)
            p.tag[i] = tag[i];
    }
    uint64_t f = spin_lock_irqsave(&pulse_lock);
    pulse_stream_publish_locked(&system_pulse, topic, &p);
    spin_unlock_irqrestore(&pulse_lock, f);
    flight_record(FLIGHT_CAT_PULSE, "pulse_emit",
                  current ? (int16_t)current->pid : 0,
                  (uint64_t)kind, (uint64_t)topic, tag ? tag : "");
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

    uint64_t ticks = ((uint64_t)q->timeout_ms * TIMER_HZ + 999) / 1000;
    uint64_t f = spin_lock_irqsave(&pulse_lock);
    return pulse_stream_read_locked(&system_pulse, &q->seq, q->topic_mask,
                                    out, limit, PULSE_CURSOR_SCAN,
                                    timer_ticks() + ticks, f);
}
