#pragma once

#include <stddef.h>

#include "proc.h"
#include "spinlock.h"

/* A bounded lane with caller-owned storage. Every record begins with a
 * uint64_t sequence, assigned by the lane. The ring and topic mask table
 * have the same nonzero capacity and outlive all readers. The lock may
 * also protect the state being published. */
struct pulse_stream {
    struct task_event_channel ready;
    spinlock_t *lock;
    void *records;
    uint32_t *topics;
    size_t record_size;
    uint32_t capacity;
    uint64_t latest;
};

#define PULSE_STREAM_INIT(guard, ring, masks) { \
    .lock = (guard), .records = (ring), .topics = (masks), \
    .record_size = sizeof((ring)[0]), \
    .capacity = sizeof(ring) / sizeof((ring)[0]), \
}

enum pulse_cursor_mode {
    PULSE_CURSOR_SCAN,       /* advance past filtered records; limit 0 follows */
    PULSE_CURSOR_MATCH,      /* advance only when a record is delivered */
};

void pulse_stream_publish_locked(struct pulse_stream *stream, uint32_t topic,
                                 const void *record);

/* Enter with the stream lock held; returns with it released even on
 * cancellation. Timeout is expressed as an absolute scheduler deadline. */
long pulse_stream_read_locked(struct pulse_stream *stream, uint64_t *cursor,
                              uint32_t mask, void *out, uint32_t limit,
                              enum pulse_cursor_mode mode, uint64_t deadline,
                              uint64_t flags);
