#include <kestrel.h>
#include <stdio.h>
#include <string.h>

#define PROBE "/bin/pulse-wait-probe"
#define UNUSED_TOPIC (1U << 31)
#define READY_FD 3

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
    if (write(READY_FD, "R", 1) != 1)
        return 1;
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
    int ready[2];
    if (pipe_(ready) < 0)
        return -1;

    char *args[] = { PROBE, (char *)mode, 0 };
    struct k_spawn_caps caps = {
        .fds = { { .child_fd = READY_FD, .parent_fd = ready[1] } },
        .n_fds = 1,
    };
    int pid = spawn_caps(PROBE, args, &caps);
    close(ready[1]);

    /* The host watchdog bounds startup, which can sleep on KFS. Start
     * the park deadline only once the reader has primed its Pulse cursor. */
    char marker;
    int armed = pid >= 0 && read(ready[0], &marker, 1) == 1 && marker == 'R';
    close(ready[0]);
    unsigned long start = uptime_ms();
    while (armed && uptime_ms() - start < 1000) {
        struct k_psinfo info;
        for (int i = 0; psinfo(i, &info) == 0; i++) {
            if (info.pid == pid && info.state == K_STATE_SLEEPING)
                return pid;
        }
        sleep_ms(10);
    }
    printf("FAIL Pulse %s reader did not park (pid=%d, ready=%d)\n",
           mode, pid, armed);
    return -1;
}

static int check_event(const char *mode)
{
    int pid = spawn_reader(mode);
    return pid >= 0 &&
           parcel_put(K_PARCEL_TEXT, "text/plain", "wait-probe", "x", 1) >= 0 &&
           waitpid(pid) == 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strncmp(argv[1], "--cwd=", 6) != 0)
        return reader(argv[1]);

    if (!check_event("event") || !check_event("timeout")) {
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
