/* flight.c - KestrelOS Avionics Flight Recorder CLI (/bin/flight).
 *
 * Reads the kernel flight recorder snapshot from /dev/flight (@krec stream)
 * and presents filtered tables, raw @krec output, or per-category / per-CPU
 * summary statistics.
 *
 * Usage:
 *   flight                 formatted table of recent events + summary
 *   flight --krec [-r]     raw @krec stream output (supports --cat / --pid / -n)
 *   flight --cat <cat>     filter by category (proc, fs, vm, pulse, talon, net)
 *   flight --pid <pid>     filter by process id
 *   flight --stats         counts per category and per CPU
 *   flight -n <count>      show only the last <count> matching events
 */

#include <kestrel.h>
#include <krec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLIGHT_DEV   "/dev/flight"
#define NUM_CATS     6
#define MAX_CPUS     16

static const char *const cat_names[NUM_CATS] = {
    "proc", "fs", "vm", "pulse", "talon", "net"
};

struct flight_row {
    uint64_t tick;
    int      cpu;
    int      pid;
    char     cat[12];
    char     event[20];
    char     arg0[24];
    char     detail[32];
};

static int cat_index(const char *name)
{
    for (int i = 0; i < NUM_CATS; i++) {
        if (strcmp(name, cat_names[i]) == 0)
            return i;
    }
    return -1;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src ? src : "");
}

static void usage(void)
{
    printf("usage: flight [--krec|-r] [--cat <cat>] [--pid <pid>] [--stats] [-n <count>]\n");
    printf("  categories: proc, fs, vm, pulse, talon, net\n");
}

static int stream_raw_krec(void)
{
    char buf[512];
    int fd;
    long n;

    fd = open(FLIGHT_DEV, O_RDONLY);
    if (fd < 0) {
        krec_err("flight: cannot open %s\n", FLIGHT_DEV);
        return 1;
    }
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        long off = 0;
        while (off < n) {
            long w = write(1, buf + off, (unsigned long)(n - off));
            if (w <= 0) {
                close(fd);
                return 1;
            }
            off += w;
        }
    }
    close(fd);
    return n < 0 ? 1 : 0;
}

int main(int argc, char **argv)
{
    int krec_flag = 0;
    int stats_flag = 0;
    const char *want_cat = 0;
    int have_pid = 0;
    int want_pid = 0;
    int limit = -1;

    argc = krec_strip_cwd(argc, argv, 0);
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--krec") == 0 || strcmp(arg, "-r") == 0) {
            krec_flag = 1;
        } else if (strcmp(arg, "--stats") == 0) {
            stats_flag = 1;
        } else if (strcmp(arg, "--cat") == 0 && i + 1 < argc) {
            want_cat = argv[++i];
        } else if (strncmp(arg, "--cat=", 6) == 0) {
            want_cat = arg + 6;
        } else if (strcmp(arg, "--pid") == 0 && i + 1 < argc) {
            int64_t v = 0;
            if (krec_parse_i64(argv[++i], &v) < 0 || v < -32768 || v > 32767) {
                krec_err("flight: invalid pid '%s'\n", argv[i]);
                return 1;
            }
            have_pid = 1;
            want_pid = (int)v;
        } else if (strncmp(arg, "--pid=", 6) == 0) {
            int64_t v = 0;
            if (krec_parse_i64(arg + 6, &v) < 0 || v < -32768 || v > 32767) {
                krec_err("flight: invalid pid '%s'\n", arg + 6);
                return 1;
            }
            have_pid = 1;
            want_pid = (int)v;
        } else if (strcmp(arg, "-n") == 0 && i + 1 < argc) {
            int64_t v = 0;
            if (krec_parse_i64(argv[++i], &v) < 0 || v < 0 || v > 1000000) {
                krec_err("flight: -n needs a non-negative count\n");
                return 1;
            }
            limit = (int)v;
        } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage();
            return 0;
        } else {
            krec_err("flight: unknown option '%s'\n", arg);
            usage();
            return 1;
        }
    }

    if (want_cat && cat_index(want_cat) < 0) {
        krec_err("flight: unknown category '%s' (expected: proc, fs, vm, pulse, talon, net)\n",
                 want_cat);
        return 1;
    }

    if (krec_flag && !stats_flag && !want_cat && !have_pid && limit < 0)
        return stream_raw_krec();

    int fd = open(FLIGHT_DEV, O_RDONLY);
    if (fd < 0) {
        krec_err("flight: cannot open %s\n", FLIGHT_DEV);
        return 1;
    }

    struct krec_reader r;
    struct krec_schema schema;
    krec_reader_init(&r, fd);
    int hdr = krec_read_header(&r, &schema);
    if (hdr <= 0) {
        close(fd);
        if (hdr < 0) {
            krec_err("flight: invalid @krec header from %s\n", FLIGHT_DEV);
            return 1;
        }
        return 0;
    }

    int c_tick   = krec_find_col(&schema, "tick");
    int c_cpu    = krec_find_col(&schema, "cpu");
    int c_pid    = krec_find_col(&schema, "pid");
    int c_cat    = krec_find_col(&schema, "cat");
    int c_event  = krec_find_col(&schema, "event");
    int c_arg0   = krec_find_col(&schema, "arg0");
    int c_detail = krec_find_col(&schema, "detail");
    if (c_tick < 0 || c_cpu < 0 || c_pid < 0 || c_cat < 0 ||
        c_event < 0 || c_arg0 < 0 || c_detail < 0) {
        close(fd);
        krec_err("flight: unexpected schema in %s\n", FLIGHT_DEV);
        return 1;
    }

    int cap = 256;
    int total_read = 0;
    int matched = 0;
    struct flight_row *rows = calloc((unsigned long)cap, sizeof(*rows));
    if (!rows) {
        close(fd);
        krec_err("flight: out of memory\n");
        return 1;
    }

    char line[512];
    char *fields[KREC_MAX_COLS];
    int rc;
    while ((rc = krec_next(&r, line, sizeof(line), fields, KREC_MAX_COLS)) == 1) {
        uint64_t tick = 0;
        int64_t cpu = 0, pid = 0;

        total_read++;
        if (krec_parse_u64(fields[c_tick], &tick) < 0 ||
            krec_parse_i64(fields[c_cpu], &cpu) < 0 ||
            krec_parse_i64(fields[c_pid], &pid) < 0)
            continue;

        if (want_cat && strcmp(fields[c_cat], want_cat) != 0)
            continue;
        if (have_pid && (int)pid != want_pid)
            continue;

        if (matched >= cap) {
            int ncap = cap * 2;
            struct flight_row *tmp = realloc(rows, (unsigned long)ncap * sizeof(*rows));
            if (!tmp) {
                free(rows);
                close(fd);
                krec_err("flight: out of memory\n");
                return 1;
            }
            rows = tmp;
            cap = ncap;
        }

        struct flight_row *e = &rows[matched++];
        e->tick = tick;
        e->cpu = (int)cpu;
        e->pid = (int)pid;
        copy_str(e->cat, sizeof(e->cat), fields[c_cat]);
        copy_str(e->event, sizeof(e->event), fields[c_event]);
        copy_str(e->arg0, sizeof(e->arg0), fields[c_arg0]);
        copy_str(e->detail, sizeof(e->detail), fields[c_detail]);
    }
    close(fd);

    if (rc < 0) {
        free(rows);
        krec_err("flight: malformed record in %s\n", FLIGHT_DEV);
        return 1;
    }

    int start = 0;
    if (limit >= 0 && matched > limit)
        start = matched - limit;

    if (krec_flag) {
        if (krec_begin_schema(1, &schema) < 0) {
            free(rows);
            return 1;
        }
        for (int i = start; i < matched; i++) {
            char tick_s[24], cpu_s[16], pid_s[16];
            snprintf(tick_s, sizeof(tick_s), "%llu",
                     (unsigned long long)rows[i].tick);
            snprintf(cpu_s, sizeof(cpu_s), "%d", rows[i].cpu);
            snprintf(pid_s, sizeof(pid_s), "%d", rows[i].pid);
            if (krec_row(1, 7, tick_s, cpu_s, pid_s, rows[i].cat,
                         rows[i].event, rows[i].arg0, rows[i].detail) < 0) {
                free(rows);
                return 1;
            }
        }
        free(rows);
        return 0;
    }

    int cat_counts[NUM_CATS];
    int cpu_counts[MAX_CPUS];
    memset(cat_counts, 0, sizeof(cat_counts));
    memset(cpu_counts, 0, sizeof(cpu_counts));
    int max_cpu_seen = -1;

    for (int i = start; i < matched; i++) {
        int ci = cat_index(rows[i].cat);
        if (ci >= 0)
            cat_counts[ci]++;
        if (rows[i].cpu >= 0 && rows[i].cpu < MAX_CPUS) {
            cpu_counts[rows[i].cpu]++;
            if (rows[i].cpu > max_cpu_seen)
                max_cpu_seen = rows[i].cpu;
        }
    }

    int shown = matched - start;
    if (stats_flag) {
        struct k_cpuinfo cinfo;
        int ncpus = max_cpu_seen + 1;
        if (cpuinfo(&cinfo) == 0 && (int)cinfo.online > ncpus)
            ncpus = (int)cinfo.online;
        if (ncpus > MAX_CPUS)
            ncpus = MAX_CPUS;
        if (ncpus < 1)
            ncpus = 1;

        if (shown > 0) {
            printf("Flight Recorder Statistics (%d events, %d retained, ticks %llu..%llu)\n\n",
                   shown, total_read,
                   (unsigned long long)rows[start].tick,
                   (unsigned long long)rows[matched - 1].tick);
        } else {
            printf("Flight Recorder Statistics (0 matching events, %d retained)\n\n",
                   total_read);
        }
        printf("%-10s %8s\n", "CATEGORY", "COUNT");
        for (int i = 0; i < NUM_CATS; i++)
            printf("%-10s %8d\n", cat_names[i], cat_counts[i]);
        printf("\n%-10s %8s\n", "CPU", "COUNT");
        for (int i = 0; i < ncpus; i++)
            printf("cpu%-7d %8d\n", i, cpu_counts[i]);
        free(rows);
        return 0;
    }

    printf("%-10s %-3s %-5s %-6s %-14s %-18s %s\n",
           "TICK", "CPU", "PID", "CAT", "EVENT", "ARG0", "DETAIL");
    for (int i = start; i < matched; i++) {
        printf("%-10llu %-3d %-5d %-6s %-14s %-18s %s\n",
               (unsigned long long)rows[i].tick,
               rows[i].cpu, rows[i].pid, rows[i].cat,
               rows[i].event, rows[i].arg0, rows[i].detail);
    }

    if (shown > 0) {
        printf("\nflight: %d shown (%d retained, ticks %llu..%llu) "
               "[proc=%d fs=%d vm=%d pulse=%d talon=%d net=%d]\n",
               shown, total_read,
               (unsigned long long)rows[start].tick,
               (unsigned long long)rows[matched - 1].tick,
               cat_counts[0], cat_counts[1], cat_counts[2],
               cat_counts[3], cat_counts[4], cat_counts[5]);
    } else {
        printf("\nflight: 0 matching events (%d retained)\n", total_read);
    }

    free(rows);
    return 0;
}
