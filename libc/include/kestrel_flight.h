#pragma once

#include <krec.h>

struct flight_record_columns {
    int tick, cpu, pid, cat, event, arg0, detail;
};

struct flight_record_view {
    uint64_t tick;
    int cpu, pid;
    const char *cat, *event, *arg0, *detail;
};

static inline int flight_record_bind(const struct krec_schema *schema,
                                     struct flight_record_columns *cols)
{
    cols->tick = krec_find_col(schema, "tick");
    cols->cpu = krec_find_col(schema, "cpu");
    cols->pid = krec_find_col(schema, "pid");
    cols->cat = krec_find_col(schema, "cat");
    cols->event = krec_find_col(schema, "event");
    cols->arg0 = krec_find_col(schema, "arg0");
    cols->detail = krec_find_col(schema, "detail");
    if (cols->tick < 0 || cols->cpu < 0 || cols->pid < 0 || cols->cat < 0 ||
        cols->event < 0 || cols->arg0 < 0 || cols->detail < 0)
        return -1;
    return 0;
}

/* Text fields borrow the row buffer until the next krec_next() call. */
static inline int flight_record_decode(const struct flight_record_columns *cols,
                                       char *const *fields,
                                       struct flight_record_view *record)
{
    uint64_t tick;
    int64_t cpu, pid;

    if (krec_parse_u64(fields[cols->tick], &tick) < 0 ||
        krec_parse_i64(fields[cols->cpu], &cpu) < 0 ||
        krec_parse_i64(fields[cols->pid], &pid) < 0)
        return -1;

    record->tick = tick;
    record->cpu = (int)cpu;
    record->pid = (int)pid;
    record->cat = fields[cols->cat];
    record->event = fields[cols->event];
    record->arg0 = fields[cols->arg0];
    record->detail = fields[cols->detail];
    return 0;
}
