#include <kestrel.h>
#include <stdio.h>
#include <string.h>

#define PROBE "/bin/pulse-wait-probe"
#define UNUSED_TOPIC (1U << 31)

static int reader(const char *mode)
{
    struct k_pulse_query query = { 0 };
    struct k_pulse event;
    int matches = strcmp(mode, "event") == 0;
    int times_out = strcmp(mode, "timeout") == 0;
    query.topic_mask = matches ? K_TOPIC_PARCEL : UNUSED_TOPIC;
    if (pulse_poll(&query, 0) != 0)
        return 1;
    query.max_out = 1;
    query.timeout_ms = times_out ? 1500 : 5000;
    unsigned long start = uptime_ms();
    int n = pulse_poll(&query, &event);
    unsigned long elapsed = uptime_ms() - start;

    if (matches)
        return !(n == 1 && event.topic == K_TOPIC_PARCEL &&
                 event.kind == K_PULSE_PARCEL_NEW &&
                 strcmp(event.tag, "wait-probe") == 0 && elapsed < 4000);
    if (times_out)
        return !(n == 0 && elapsed >= 1500);
    return 1; /* Cancellation must terminate the process before returning. */
}

static int spawn_reader(const char *mode)
{
    char *args[] = { PROBE, (char *)mode, 0 };
    int pid = spawn(PROBE, args);
    if (pid < 0)
        return -1;
    unsigned long start = uptime_ms();
    while (uptime_ms() - start < 1000) {
        struct k_psinfo info;
        for (int i = 0; psinfo(i, &info) == 0; i++)
            if (info.pid == pid && info.state == K_STATE_SLEEPING)
                return pid;
        sleep_ms(10);
    }
    return -1;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strncmp(argv[1], "--cwd=", 6) != 0)
        return reader(argv[1]);

    int event = spawn_reader("event");
    int timeout = spawn_reader("timeout");
    if (event < 0 || timeout < 0 ||
        parcel_put(K_PARCEL_TEXT, "text/plain", "wait-probe", "x", 1) < 0 ||
        waitpid(event) != 0 || waitpid(timeout) != 0) {
        puts("FAIL Pulse event delivery or timeout");
        return 1;
    }
    puts("PASS Pulse matching event and unrelated-topic timeout");

    int cancelled = spawn_reader("cancel");
    unsigned long start = uptime_ms();
    if (cancelled < 0 || kill(cancelled, SIGKILL) < 0 ||
        waitpid(cancelled) != 128 + SIGKILL || uptime_ms() - start >= 3000) {
        puts("FAIL Pulse cancellation");
        return 1;
    }
    puts("PASS Pulse SIGKILL cancellation");
    return 0;
}
