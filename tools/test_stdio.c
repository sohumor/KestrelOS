/*
 * Focused host regression for KestrelOS libc/stdio.c.
 *
 * The target formatter is compiled as a separate object with its public
 * symbols renamed to k_*.  This file continues to use the host C library as
 * an independent oracle.  See tools/run-stdio-tests.sh for the exact build.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int k_printf(const char *fmt, ...);
int k_vprintf(const char *fmt, va_list ap);
int k_snprintf(char *buf, unsigned long size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
int k_vsnprintf(char *buf, unsigned long size, const char *fmt, va_list ap);

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define TEST_OUT 512
#define GUARD 16
#define RANDOM_CASES 10000
#define RANDOM_SEED UINT64_C(0x4b45535452454c31)

static int g_named;
static int g_failed;
static int g_capture;
static unsigned long g_capture_total;
static char g_capture_buf[TEST_OUT];

/*
 * libc/stdio.c's console path references target read/write.  The renamed
 * object resolves those calls here.  Capturing k_printf also proves that the
 * vprintf path shares the corrected format_core implementation.
 */
long k_write(int fd, const void *buf, unsigned long len)
{
    unsigned long room;
    unsigned long take;

    if (!g_capture || fd != 1)
        return (long)len;
    room = sizeof(g_capture_buf) - 1;
    take = len;
    if (g_capture_total >= room)
        take = 0;
    else if (take > room - g_capture_total)
        take = room - g_capture_total;
    if (take)
        memcpy(g_capture_buf + g_capture_total, buf, take);
    g_capture_total += len;
    if (g_capture_total < sizeof(g_capture_buf))
        g_capture_buf[g_capture_total] = '\0';
    else
        g_capture_buf[sizeof(g_capture_buf) - 1] = '\0';
    return (long)len;
}

long k_read(int fd, void *buf, unsigned long len)
{
    (void)fd;
    (void)buf;
    (void)len;
    return -1;
}

static int all_byte(const unsigned char *p, size_t n, unsigned char value)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (p[i] != value)
            return 0;
    return 1;
}

static void named_result(const char *name, int ok, const char *detail)
{
    g_named++;
    if (ok) {
        printf("  PASS %02d  %s\n", g_named, name);
        return;
    }
    g_failed++;
    printf("  FAIL %02d  %s", g_named, name);
    if (detail && detail[0])
        printf(" -- %s", detail);
    putchar('\n');
}

static void expect_format(const char *name, const char *want,
                          const char *fmt, ...)
{
    unsigned char storage[GUARD + TEST_OUT + GUARD];
    char *out = (char *)storage + GUARD;
    char detail[192];
    size_t want_len = strlen(want);
    va_list ap;
    int rc;
    int ok;

    memset(storage, 0xa5, sizeof(storage));
    va_start(ap, fmt);
    rc = k_vsnprintf(out, TEST_OUT, fmt, ap);
    va_end(ap);

    ok = rc == (int)want_len &&
         want_len < TEST_OUT &&
         memcmp(out, want, want_len) == 0 &&
         out[want_len] == '\0' &&
         all_byte(storage, GUARD, 0xa5) &&
         all_byte(storage + GUARD + TEST_OUT, GUARD, 0xa5);
    if (!ok) {
        snprintf(detail, sizeof(detail), "rc=%d want=%zu got-prefix='%.*s'",
                 rc, want_len, 80, out);
        named_result(name, 0, detail);
    } else {
        named_result(name, 1, 0);
    }
}

static void check_capacity(const char *name, unsigned long cap,
                           int null_destination)
{
    unsigned char storage[GUARD + 32 + GUARD];
    char *out = (char *)storage + GUARD;
    static const char text[] = "abcdef";
    unsigned long copied = 0;
    int rc;
    int ok;
    char detail[160];

    memset(storage, 0xc7, sizeof(storage));
    rc = k_snprintf(null_destination ? 0 : out, cap, "%s", text);
    ok = rc == 6;
    if (!null_destination) {
        if (cap > 0) {
            copied = cap - 1 < 6 ? cap - 1 : 6;
            ok = ok && memcmp(out, text, copied) == 0 &&
                 out[copied] == '\0';
        }
        ok = ok &&
             all_byte(storage, GUARD, 0xc7) &&
             all_byte(storage + GUARD + cap,
                      sizeof(storage) - GUARD - cap, 0xc7);
    }
    if (!ok) {
        snprintf(detail, sizeof(detail),
                 "cap=%lu null=%d rc=%d copied=%lu",
                 cap, null_destination, rc, copied);
        named_result(name, 0, detail);
    } else {
        named_result(name, 1, 0);
    }
}

typedef int (*child_test_fn)(void);

static void expect_child(const char *name, child_test_fn fn)
{
    int status;
    pid_t pid;
    char detail[128];

    fflush(0);
    pid = fork();
    if (pid < 0) {
        snprintf(detail, sizeof(detail), "fork failed: %s", strerror(errno));
        named_result(name, 0, detail);
        return;
    }
    if (pid == 0)
        _exit(fn() ? 0 : 1);
    if (waitpid(pid, &status, 0) < 0) {
        snprintf(detail, sizeof(detail), "waitpid failed: %s",
                 strerror(errno));
        named_result(name, 0, detail);
        return;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        named_result(name, 1, 0);
        return;
    }
    if (WIFSIGNALED(status))
        snprintf(detail, sizeof(detail), "child died from signal %d",
                 WTERMSIG(status));
    else if (WIFEXITED(status))
        snprintf(detail, sizeof(detail), "child exited %d",
                 WEXITSTATUS(status));
    else
        snprintf(detail, sizeof(detail), "unexpected wait status 0x%x",
                 status);
    named_result(name, 0, detail);
}

static int x509_shaped_call(void)
{
    static const char san[] = "kestrel-negative.invalid";
    static const char want[] =
        "the certificate is for 'kestrel-negative.invalid', "
        "not '10.0.2.2'";
    char out[192];
    int rc;

    rc = k_snprintf(out, sizeof(out),
                    "the certificate is for '%.*s'%s, not '%s'",
                    24, san, "", "10.0.2.2");
    return rc == (int)strlen(want) && strcmp(out, want) == 0;
}

static int mixed_later_args(void)
{
    char out[96];
    int rc = k_snprintf(out, sizeof(out), "%.*s|%s|%d",
                        3, "abcdef", "tail", 37);

    return rc == 11 && strcmp(out, "abc|tail|37") == 0;
}

static int nonstring_star_alignment(void)
{
    char out[96];
    int rc = k_snprintf(out, sizeof(out), "%.*d|%s", 3, 7, "tail");

    /*
     * Numeric precision is outside this patch's required semantics.  Either
     * ignoring it or implementing the standard zero padding is acceptable;
     * consuming its star exactly once and preserving the later pointer is
     * mandatory.
     */
    return (rc == 6 && strcmp(out, "7|tail") == 0) ||
           (rc == 8 && strcmp(out, "007|tail") == 0);
}

static int guard_slice_24(void)
{
    static const char san[] = "kestrel-negative.invalid";
    static const char fmt[] = "%.*s";
    long page = sysconf(_SC_PAGESIZE);
    unsigned char *map;
    char *slice;
    char out[64];
    int rc;
    int ok;

    if (page < 64)
        return 0;
    map = mmap(0, (size_t)page * 2, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED)
        return 0;
    if (mprotect(map + page, (size_t)page, PROT_NONE) != 0) {
        munmap(map, (size_t)page * 2);
        return 0;
    }
    slice = (char *)map + page - 24;
    memcpy(slice, san, 24);                 /* deliberately no terminating NUL */
    rc = k_snprintf(out, sizeof(out), fmt, 24, slice);
    ok = rc == 24 && memcmp(out, san, 24) == 0 && out[24] == '\0';
    munmap(map, (size_t)page * 2);
    return ok;
}

static int guard_zero_precision(void)
{
    long page = sysconf(_SC_PAGESIZE);
    unsigned char *map;
    char out[8];
    int rc;
    int ok;

    if (page < 64)
        return 0;
    map = mmap(0, (size_t)page * 2, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED)
        return 0;
    if (mprotect(map + page, (size_t)page, PROT_NONE) != 0) {
        munmap(map, (size_t)page * 2);
        return 0;
    }
    rc = k_snprintf(out, sizeof(out), "%.0s", (char *)map + page);
    ok = rc == 0 && out[0] == '\0';
    munmap(map, (size_t)page * 2);
    return ok;
}

static int call_k_vsnprintf(char *out, unsigned long outsz,
                            const char *fmt, ...)
{
    va_list ap;
    int rc;

    va_start(ap, fmt);
    rc = k_vsnprintf(out, outsz, fmt, ap);
    va_end(ap);
    return rc;
}

static void deterministic_tests(void)
{
    const char *null_string = 0;
    char out[96];
    int rc;
    int ok;

    printf("\n== deterministic formatter contract ==\n");
    expect_format("literal precision zero", "", "%.0s", "abcdef");
    expect_format("literal precision three", "abc", "%.3s", "abcdef");
    expect_format("bare dot means precision zero", "", "%.s", "abcdef");
    expect_format("precision longer than source", "abc", "%.20s", "abc");
    expect_format("precision stops at embedded NUL", "ab", "%.5s", "ab");
    expect_format("leading-zero literal precision", "abcd", "%.0004s",
                  "abcdef");
    expect_format("saturating decimal precision", "xy",
                  "%.999999999999999999999999999999999999s", "xy");

    expect_format("star precision zero", "", "%.*s", 0, "abcdef");
    expect_format("star precision exact 24", "kestrel-negative.invalid",
                  "%.*s", 24, "kestrel-negative.invalid");
    expect_format("star precision longer than source", "abc",
                  "%.*s", 50, "abc");
    expect_format("negative star omits precision", "abcdef",
                  "%.*s", -7, "abcdef");
    expect_format("star precision stops at embedded NUL", "ab",
                  "%.*s", 5, "ab");

    expect_format("literal precision with right width", "     abc",
                  "%8.3s", "abcdef");
    expect_format("literal precision with left width", "abc     ",
                  "%-8.3s", "abcdef");
    expect_format("star precision with right width", "     abc",
                  "%8.*s", 3, "abcdef");
    expect_format("star precision with left width", "abc     ",
                  "%-8.*s", 3, "abcdef");
    expect_format("width smaller than bounded text", "abcd",
                  "%2.4s", "abcdef");
    expect_format("negative star still honors width", "  abcdef",
                  "%8.*s", -1, "abcdef");

    expect_child("X.509-shaped star keeps later pointers aligned",
                 x509_shaped_call);
    expect_child("mixed star/string/integer consumed once",
                 mixed_later_args);
    expect_child("non-string star consumes precision once",
                 nonstring_star_alignment);

    expect_format("NULL string uses target replacement", "(null)",
                  "%s", null_string);
    expect_format("NULL with zero precision", "", "%.0s", null_string);
    expect_format("NULL with short literal precision", "(nu",
                  "%.3s", null_string);
    expect_format("NULL with short star precision", "(nul",
                  "%.*s", 4, null_string);

    check_capacity("size zero with NULL destination", 0, 1);
    check_capacity("size zero preserves non-NULL canaries", 0, 0);
    check_capacity("size one writes only NUL", 1, 0);
    check_capacity("truncated size returns full length", 4, 0);
    check_capacity("exact result plus NUL", 7, 0);
    check_capacity("larger capacity preserves outer canary", 12, 0);

    memset(out, 0xa5, sizeof(out));
    rc = call_k_vsnprintf(out, sizeof(out), "[%.*s]", 3, "abcdef");
    ok = rc == 5 && strcmp(out, "[abc]") == 0;
    named_result("vsnprintf path shares precision semantics", ok,
                 ok ? 0 : "expected '[abc]'");

    memset(g_capture_buf, 0, sizeof(g_capture_buf));
    g_capture_total = 0;
    g_capture = 1;
    rc = k_printf("[%8.3s]", "abcdef");
    g_capture = 0;
    ok = rc == 10 && g_capture_total == 10 &&
         strcmp(g_capture_buf, "[     abc]") == 0;
    named_result("vprintf console path shares precision semantics", ok,
                 ok ? 0 : "captured console output differed");

    expect_child("guard page: 24-byte non-NUL slice has no overread",
                 guard_slice_24);
    expect_child("guard page: zero precision reads no source byte",
                 guard_zero_precision);
}

static uint64_t prng_next(uint64_t *state)
{
    uint64_t x = *state;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * UINT64_C(2685821657736338717);
}

static int outside_untouched(const unsigned char *storage, size_t total,
                             size_t offset, size_t cap, unsigned char byte)
{
    return all_byte(storage, offset, byte) &&
           all_byte(storage + offset + cap, total - offset - cap, byte);
}

static int random_differential(void)
{
    static const char alphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_./";
    uint64_t state = RANDOM_SEED;
    int mismatches = 0;
    int first_case = -1;
    char first_fmt[64] = "";
    char first_src[80] = "";
    int first_precision = 0;
    unsigned long first_cap = 0;
    int first_krc = 0, first_hrc = 0;
    int i;

    printf("\n== seeded host differential ==\n");
    for (i = 0; i < RANDOM_CASES; i++) {
        unsigned char kstore[GUARD + 96 + GUARD];
        unsigned char hstore[GUARD + 96 + GUARD];
        char *kout = (char *)kstore + GUARD;
        char *hout = (char *)hstore + GUARD;
        char fmt[64];
        char src[80];
        int mode = (int)(prng_next(&state) % 3);
        int left = (int)(prng_next(&state) & 1);
        int width = (int)(prng_next(&state) % 25);
        int precision = (int)(prng_next(&state) % 89) - 8;
        int literal_precision = (int)(prng_next(&state) % 81);
        int source_len = (int)(prng_next(&state) % 65);
        unsigned long cap = (unsigned long)(prng_next(&state) % 65);
        unsigned long meaningful;
        int krc, hrc;
        int ok;
        int j;

        for (j = 0; j < source_len; j++)
            src[j] = alphabet[prng_next(&state) % (ARRAY_LEN(alphabet) - 1)];
        src[source_len] = '\0';

        if (mode == 0) {
            if (width)
                snprintf(fmt, sizeof(fmt), "L%%%s%d.%dsR",
                         left ? "-" : "", width, literal_precision);
            else
                snprintf(fmt, sizeof(fmt), "L%%%s.%dsR",
                         left ? "-" : "", literal_precision);
        } else if (mode == 1) {
            if (width)
                snprintf(fmt, sizeof(fmt), "L%%%s%d.*sR",
                         left ? "-" : "", width);
            else
                snprintf(fmt, sizeof(fmt), "L%%%s.*sR",
                         left ? "-" : "");
        } else {
            if (width)
                snprintf(fmt, sizeof(fmt), "L%%%s%d.sR",
                         left ? "-" : "", width);
            else
                snprintf(fmt, sizeof(fmt), "L%%%s.sR",
                         left ? "-" : "");
        }

        memset(kstore, 0xd3, sizeof(kstore));
        memset(hstore, 0xd3, sizeof(hstore));
        if (mode == 1) {
            krc = k_snprintf(cap ? kout : 0, cap, fmt, precision, src);
            hrc = snprintf(cap ? hout : 0, cap, fmt, precision, src);
        } else {
            krc = k_snprintf(cap ? kout : 0, cap, fmt, src);
            hrc = snprintf(cap ? hout : 0, cap, fmt, src);
        }

        meaningful = 0;
        if (cap) {
            int full = hrc < 0 ? 0 : hrc;
            meaningful = (unsigned long)full < cap - 1
                ? (unsigned long)full + 1 : cap;
        }
        ok = krc == hrc &&
             outside_untouched(kstore, sizeof(kstore), GUARD, cap, 0xd3) &&
             outside_untouched(hstore, sizeof(hstore), GUARD, cap, 0xd3) &&
             (meaningful == 0 || memcmp(kout, hout, meaningful) == 0);
        if (!ok) {
            mismatches++;
            if (first_case < 0) {
                first_case = i;
                snprintf(first_fmt, sizeof(first_fmt), "%s", fmt);
                snprintf(first_src, sizeof(first_src), "%s", src);
                first_precision = precision;
                first_cap = cap;
                first_krc = krc;
                first_hrc = hrc;
            }
        }
    }

    printf("  seed=0x%016llx cases=%d mismatches=%d\n",
           (unsigned long long)RANDOM_SEED, RANDOM_CASES, mismatches);
    if (mismatches) {
        printf("  first mismatch: case=%d fmt='%s' src='%s' "
               "precision=%d cap=%lu target_rc=%d host_rc=%d\n",
               first_case, first_fmt, first_src, first_precision, first_cap,
               first_krc, first_hrc);
    }
    return mismatches;
}

int main(int argc, char **argv)
{
    int random_mismatches;

    if (argc == 2 && strcmp(argv[1], "--x509-crash") == 0) {
        fprintf(stderr,
                "direct X.509 formatter probe: precision=24 (0x18)\n");
        fflush(stderr);
        if (!x509_shaped_call()) {
            fprintf(stderr, "direct X.509 formatter probe: wrong output\n");
            return 1;
        }
        fprintf(stderr, "direct X.509 formatter probe: PASS\n");
        return 0;
    }
    if (argc != 1) {
        fprintf(stderr, "usage: %s [--x509-crash]\n", argv[0]);
        return 2;
    }

    printf("Kestrel target stdio precision regression\n");
    deterministic_tests();
    random_mismatches = random_differential();

    printf("\nsummary: %d named deterministic assertions, %d failed; "
           "%d seeded differential cases, %d mismatches\n",
           g_named, g_failed, RANDOM_CASES, random_mismatches);
    return (g_failed || random_mismatches) ? 1 : 0;
}
