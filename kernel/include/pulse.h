#pragma once

#include <stdint.h>
#include "kestrel_abi.h"

/* Monotonic kernel event stream ("pulses"). Producers emit small tagged
 * records into a fixed static circular ring; consumers poll or sleep until
 * matching records newer than q->seq arrive. */

void pulse_emit(uint32_t topic, uint32_t kind, uint32_t arg0, uint32_t arg1,
                const char *tag);

/* Copy matching (topic & q->topic_mask) records with seq > q->seq into out[]
 * up to q->max_out (capped at 64). Updates q->seq and returns count (>= 0),
 * or -1 on invalid arguments or pending kill. */
long pulse_poll(struct k_pulse_query *q, struct k_pulse *out);
