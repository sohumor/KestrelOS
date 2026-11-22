#include "pulse_stream.h"
#include "string.h"
#include "timer.h"

static void *record_at(const struct pulse_stream *stream, uint32_t index)
{
    return (uint8_t *)stream->records + index * stream->record_size;
}

void pulse_stream_publish_locked(struct pulse_stream *stream, uint32_t topic,
                                 const void *record)
{
    uint64_t seq = ++stream->latest;
    uint32_t index = (seq - 1) % stream->capacity;
    void *slot = record_at(stream, index);

    memcpy(slot, record, stream->record_size);
    memcpy(slot, &seq, sizeof(seq));
    stream->topics[index] = topic;
    task_event_notify(&stream->ready, topic);
}

static uint32_t drain_locked(const struct pulse_stream *stream, uint64_t *cursor,
                             uint32_t mask, uint8_t *out, uint32_t limit,
                             enum pulse_cursor_mode mode)
{
    uint64_t latest = stream->latest;
    uint32_t count = 0;

    if (limit && *cursor < latest) {
        uint64_t oldest = latest > stream->capacity
                        ? latest - stream->capacity + 1 : 1;
        uint64_t seq = *cursor + 1;
        if (seq < oldest)
            seq = oldest;

        for (;;) {
            uint32_t index = (seq - 1) % stream->capacity;
            if (stream->topics[index] & mask) {
                memcpy(out + count * stream->record_size,
                       record_at(stream, index), stream->record_size);
                *cursor = seq;
                if (++count == limit)
                    return count;
            }
            if (seq == latest)
                break;
            seq++;
        }
    }
    if (mode == PULSE_CURSOR_SCAN)
        *cursor = latest;
    return count;
}

long pulse_stream_read_locked(struct pulse_stream *stream, uint64_t *cursor,
                              uint32_t mask, void *out, uint32_t limit,
                              enum pulse_cursor_mode mode, uint64_t deadline,
                              uint64_t flags)
{
    for (;;) {
        uint32_t count = drain_locked(stream, cursor, mask, out, limit, mode);
        if (count || !limit || !sched_active || timer_ticks() >= deadline) {
            spin_unlock_irqrestore(stream->lock, flags);
            return count;
        }

        if (task_event_wait_locked(&stream->ready, mask, deadline,
                                   stream->lock, flags) < 0)
            return -1;
        flags = spin_lock_irqsave(stream->lock);
    }
}
