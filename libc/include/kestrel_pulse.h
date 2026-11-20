#pragma once

#include <kestrel.h>

/* A reader's position in the shared Pulse history. Start at seq 0 to
 * replay retained events, then keep the same cursor for live updates. */
struct pulse_cursor {
    uint64_t seq;
    uint32_t topics;
};

static inline int pulse_cursor_read(struct pulse_cursor *cursor,
                                    struct k_pulse *out, uint32_t capacity,
                                    uint32_t timeout_ms)
{
    struct k_pulse_query query = {
        .seq = cursor->seq,
        .topic_mask = cursor->topics,
        .max_out = capacity,
        .timeout_ms = timeout_ms,
    };
    int count = pulse_poll(&query, out);
    if (count >= 0)
        cursor->seq = query.seq;
    return count;
}

/* Skip retained history so the next read sees only later events. */
static inline int pulse_cursor_follow(struct pulse_cursor *cursor)
{
    return pulse_cursor_read(cursor, 0, 0, 0);
}
