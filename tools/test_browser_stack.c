/*
 * Whole-browser 64 KiB stack gate.
 *
 * This host harness includes the real apps/browser.c translation unit and
 * invokes its renamed main() for both text and graphical modes.  The browser
 * is linked to the real DOM, HTML, CSS, layout, paint, URL, and font sources.
 * Only OS syscalls, the unused network edge, and window creation/events are
 * deterministic shims.
 *
 * A generated local page contains a block-display stylesheet, 128 nested
 * divs, and a deepest STACK-DEEPEST link.  The browser runs on an explicitly
 * supplied 64 KiB pthread stack surrounded by inaccessible guard pages.
 * Address-varying bytes prefill that stack, allowing the harness to report
 * the whole-entry high-water mark after the browser exits.
 *
 * Build and run with tools/run-browser-stack.sh; it supplies target-like -O2
 * flags and emits per-object -fstack-usage files in an isolated QA directory.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define main browser_program_main
#include "../apps/browser.c"
#undef main

/* Stop the host seam macros from affecting the shim implementations below. */
#undef stat_
#undef netinfo
#undef cpuinfo
#undef syscall

#define TEST_STACK_BYTES (64UL * 1024UL)
#define STACK_LIMIT      49152UL
#define LOW_CANARY_BYTES 256UL
#define FIXTURE_CAP      8192

static int gui_flushes;
static unsigned long gui_changed_pixels;
static uint64_t gui_checksum;
static int gui_event_sent;

/* ------------------------------------------------------------------ *
 * Kestrel OS edge shims
 * ------------------------------------------------------------------ */

int browser_host_stat(const char *path, struct k_stat *out)
{
    struct stat st;

    if (!path || !out || stat(path, &st) != 0)
        return -1;
    memset(out, 0, sizeof(*out));
    out->size = (uint64_t)st.st_size;
    out->is_dir = S_ISDIR(st.st_mode) ? 1u : 0u;
    return 0;
}

int browser_host_netinfo(struct k_netinfo *out)
{
    if (out)
        memset(out, 0, sizeof(*out));
    return 0;
}

int browser_host_cpuinfo(struct k_cpuinfo *out)
{
    if (!out)
        return -1;
    memset(out, 0, sizeof(*out));
    out->discovered = 4;
    out->online = 4;
    return 0;
}

long browser_host_syscall(long n, long a, long b, long c, long d)
{
    (void)b;
    (void)c;
    (void)d;
    if (n == SYS_FBINFO && a) {
        struct k_fbinfo *fb = (struct k_fbinfo *)(uintptr_t)a;

        memset(fb, 0, sizeof(*fb));
        fb->width = 1280;
        fb->height = 720;
        fb->pitch = 1280 * 4;
        fb->bpp = 32;
        fb->present = 1;
        return 0;
    }
    return -1;
}

/* ------------------------------------------------------------------ *
 * Process-lifetime service shims.  Local-page tests never use a transport;
 * these exist only so the real browser entry and runtime lifetime execute.
 * ------------------------------------------------------------------ */

struct http_client *http_client_new(void)
{
    return (struct http_client *)malloc(1);
}

void http_client_free(struct http_client *c)
{
    free(c);
}

void http_client_drop_connections(struct http_client *c)
{
    (void)c;
}

void http_client_set_agent(struct http_client *c, const char *agent)
{
    (void)c;
    (void)agent;
}

struct cookie_jar *http_client_jar(struct http_client *c)
{
    (void)c;
    return 0;
}

int cookie_jar_load(struct cookie_jar *j, const char *file, long now)
{
    (void)j;
    (void)file;
    (void)now;
    return 0;
}

int cookie_jar_save(struct cookie_jar *j, const char *file, long now)
{
    (void)j;
    (void)file;
    (void)now;
    return 0;
}

int http_register_scheme(const char *scheme, http_transport_fn fn, void *user)
{
    (void)scheme;
    (void)fn;
    (void)user;
    return HTTP_OK;
}

void http_set_inflate(http_inflate_fn fn)
{
    (void)fn;
}

int http_fetch(struct http_client *c, const struct http_request *req,
               struct http_response *res)
{
    (void)c;
    (void)req;
    if (res)
        memset(res, 0, sizeof(*res));
    return HTTP_E_CONNECT;
}

void http_response_free(struct http_response *r)
{
    (void)r;
}

const char *http_header_get(const struct http_response *r, const char *name)
{
    (void)r;
    (void)name;
    return 0;
}

const char *http_error_text(int err)
{
    (void)err;
    return "host transport disabled";
}

int inflate_buf(const void *src, unsigned long slen, void **out,
                unsigned long *olen, int wrapper)
{
    (void)src;
    (void)slen;
    (void)wrapper;
    if (out)
        *out = 0;
    if (olen)
        *olen = 0;
    return INFLATE_ERR_STATE;
}

int img_decode(const void *data, unsigned long len, struct image *out)
{
    (void)data;
    (void)len;
    memset(out, 0, sizeof(*out));
    return IMG_ERR_FORMAT;
}

void img_free(struct image *im)
{
    if (im)
        memset(im, 0, sizeof(*im));
}

void tls_options_default(struct tls_options *o)
{
    if (o)
        memset(o, 0, sizeof(*o));
}

struct x509_store *tls_default_store(void)
{
    static unsigned char dummy_store;

    return (struct x509_store *)(void *)&dummy_store;
}

void tls_default_store_free(void)
{
}

int tls_transport_open(const char *host, int port, int timeout_ms,
                       void *user, struct tls_transport *out)
{
    (void)host;
    (void)port;
    (void)timeout_ms;
    (void)user;
    (void)out;
    return HTTP_E_CONNECT;
}

const char *tls_last_transport_error(void)
{
    return "host transport disabled";
}

void tls_add_entropy(const void *data, unsigned long len)
{
    (void)data;
    (void)len;
}

/* ------------------------------------------------------------------ *
 * Window edge shims.  Drawing and page painting remain the real code.
 * ------------------------------------------------------------------ */

gui_window *gui_open(const char *title, int x, int y, int w, int h,
                     unsigned flags)
{
    gui_window *win;
    size_t pixels;

    (void)title;
    (void)x;
    (void)y;
    (void)flags;
    if (w <= 0 || h <= 0 ||
        (size_t)w > SIZE_MAX / (size_t)h / sizeof(uint32_t))
        return 0;
    pixels = (size_t)w * (size_t)h;
    win = (gui_window *)calloc(1, sizeof(*win));
    if (!win)
        return 0;
    win->px = (uint32_t *)calloc(pixels, sizeof(*win->px));
    if (!win->px) {
        free(win);
        return 0;
    }
    win->w = w;
    win->h = h;
    win->open = 1;
    win->clip = gui_mkrc(0, 0, w, h);
    return win;
}

void gui_close(gui_window *win)
{
    if (!win)
        return;
    free(win->px);
    free(win);
}

int gui_flush(gui_window *win)
{
    size_t i, pixels;
    uint64_t hash = UINT64_C(1469598103934665603);
    unsigned long changed = 0;

    if (!win || !win->px)
        return -1;
    pixels = (size_t)win->w * (size_t)win->h;
    for (i = 0; i < pixels; i++) {
        uint32_t px = win->px[i];

        if (px != C_BG)
            changed++;
        hash ^= px;
        hash *= UINT64_C(1099511628211);
    }
    gui_flushes++;
    gui_changed_pixels = changed;
    gui_checksum = hash;
    return 0;
}

int gui_next_event(gui_window *win, struct k_event *ev, int timeout_ms)
{
    (void)win;
    (void)timeout_ms;
    if (!ev)
        return -1;
    memset(ev, 0, sizeof(*ev));
    if (!gui_event_sent) {
        gui_event_sent = 1;
        ev->type = KEV_CLOSE;
        return 1;
    }
    return -1;
}

/* ------------------------------------------------------------------ *
 * Fixture and guarded-stack runner
 * ------------------------------------------------------------------ */

static int append_text(char *buf, size_t cap, size_t *used, const char *text)
{
    size_t n = strlen(text);

    if (n >= cap - *used)
        return -1;
    memcpy(buf + *used, text, n);
    *used += n;
    buf[*used] = 0;
    return 0;
}

static int make_fixture(char path[64], size_t *fixture_len)
{
    char page[FIXTURE_CAP];
    size_t used = 0;
    int fd, i;
    ssize_t wrote;

    if (append_text(page, sizeof(page), &used,
                    "<!doctype html><html><head><style>"
                    "html,body,div,a{display:block}"
                    "div{margin:0;padding:0;border:0}"
                    "</style><title>Browser stack gate</title></head><body>"
                    "<p>STACK-GATE-TOP</p>"
                    "<p id=\"module-proof\">MODULE-NOT-RUN</p>"
                    "<script type=\"module\">\n"
                    "const proof='STACK-MODULE-OK';\n"
                    "document.getElementById('module-proof').textContent=proof;\n"
                    "export const ready=true;\n"
                    "</script>") != 0)
        return -1;
    for (i = 0; i < 128; i++)
        if (append_text(page, sizeof(page), &used, "<div>") != 0)
            return -1;
    if (append_text(page, sizeof(page), &used,
                    "<a id=\"deep\" href=\"#deep\">STACK-DEEPEST</a>") != 0)
        return -1;
    for (i = 0; i < 128; i++)
        if (append_text(page, sizeof(page), &used, "</div>") != 0)
            return -1;
    if (append_text(page, sizeof(page), &used, "</body></html>") != 0 ||
        used >= FIXTURE_CAP)
        return -1;

    snprintf(path, 64, "/tmp/kestrel-browser-stack-XXXXXX");
    fd = mkstemp(path);
    if (fd < 0)
        return -1;
    wrote = write(fd, page, used);
    if (close(fd) != 0 || wrote < 0 || (size_t)wrote != used) {
        unlink(path);
        return -1;
    }
    *fixture_len = used;
    return 0;
}

struct thread_args {
    const char *mode;
    const char *path;
    uintptr_t entry_sp;
    int rc;
};

static uintptr_t current_stack_pointer(void)
{
    uintptr_t sp;

    __asm__ volatile("mov %%rsp,%0" : "=r" (sp));
    return sp;
}

static void *run_browser(void *opaque)
{
    struct thread_args *a = (struct thread_args *)opaque;

    if (strcmp(a->mode, "text") == 0) {
        char *argv[] = {
            (char *)"browser", (char *)"-t", (char *)"-l", (char *)"-v",
            (char *)a->path, 0
        };

        /* Measure from the real browser entry, excluding pthread's host-only
         * startup frames above this point.  The inaccessible lower guard
         * still protects the complete 64 KiB mapping. */
        a->entry_sp = current_stack_pointer();
        a->rc = browser_program_main(5, argv);
    } else {
        char *argv[] = {
            (char *)"browser", (char *)a->path, 0
        };

        a->entry_sp = current_stack_pointer();
        a->rc = browser_program_main(2, argv);
    }
    return 0;
}

static unsigned char stack_pattern(size_t i)
{
    return (unsigned char)(0x5Au ^ (unsigned char)(i * 131u) ^
                           (unsigned char)(i >> 8));
}

static int guarded_run(const char *mode, const char *path, size_t fixture_len)
{
    long page_size = sysconf(_SC_PAGESIZE);
    size_t map_len;
    unsigned char *mapping, *stack;
    size_t i, first_changed, entry_offset, used, headroom, host_overhead;
    int canary_ok = 1;
    pthread_attr_t attr;
    pthread_t thread;
    struct thread_args args;
    int rc, attr_ready = 0, failed = 0;

    if (page_size <= 0 ||
        TEST_STACK_BYTES % (unsigned long)page_size != 0) {
        fprintf(stderr, "invalid host page size %ld\n", page_size);
        return 1;
    }
    map_len = TEST_STACK_BYTES + 2 * (size_t)page_size;
    mapping = mmap(0, map_len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    stack = mapping + page_size;
    if (mprotect(stack, TEST_STACK_BYTES, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect");
        munmap(mapping, map_len);
        return 1;
    }
    for (i = 0; i < TEST_STACK_BYTES; i++)
        stack[i] = stack_pattern(i);

    memset(&args, 0, sizeof(args));
    args.mode = mode;
    args.path = path;
    gui_flushes = 0;
    gui_changed_pixels = 0;
    gui_checksum = 0;
    gui_event_sent = 0;

    rc = pthread_attr_init(&attr);
    if (rc == 0) {
        attr_ready = 1;
        rc = pthread_attr_setguardsize(&attr, 0);
    }
    if (rc == 0)
        rc = pthread_attr_setstack(&attr, stack, TEST_STACK_BYTES);
    if (rc == 0)
        rc = pthread_create(&thread, &attr, run_browser, &args);
    if (attr_ready)
        pthread_attr_destroy(&attr);
    if (rc != 0) {
        fprintf(stderr, "pthread setup failed: %s\n", strerror(rc));
        munmap(mapping, map_len);
        return 1;
    }
    rc = pthread_join(thread, 0);
    if (rc != 0) {
        fprintf(stderr, "pthread_join failed: %s\n", strerror(rc));
        munmap(mapping, map_len);
        return 1;
    }

    first_changed = TEST_STACK_BYTES;
    for (i = 0; i < TEST_STACK_BYTES; i++) {
        if (stack[i] != stack_pattern(i)) {
            first_changed = i;
            break;
        }
    }
    if (args.entry_sp < (uintptr_t)stack ||
        args.entry_sp > (uintptr_t)(stack + TEST_STACK_BYTES)) {
        fprintf(stderr, "browser entry stack pointer is outside test stack\n");
        munmap(mapping, map_len);
        return 1;
    }
    entry_offset = (size_t)(args.entry_sp - (uintptr_t)stack);
    host_overhead = TEST_STACK_BYTES - entry_offset;
    used = first_changed == TEST_STACK_BYTES || first_changed >= entry_offset
        ? 0 : entry_offset - first_changed;
    headroom = TEST_STACK_BYTES - used;
    for (i = 0; i < LOW_CANARY_BYTES; i++)
        if (stack[i] != stack_pattern(i))
            canary_ok = 0;

    if (args.rc != 0 || used > STACK_LIMIT || !canary_ok)
        failed = 1;
    if (strcmp(mode, "gui") == 0 &&
        (gui_flushes < 2 || gui_changed_pixels == 0 || gui_checksum == 0))
        failed = 1;

    printf("STACK-GATE mode=%s fixture=%lu stack=%lu used=%lu "
           "headroom=%lu limit=%lu host_overhead=%lu guard=ok "
           "canary=%s rc=%d",
           mode, (unsigned long)fixture_len, TEST_STACK_BYTES,
           (unsigned long)used, (unsigned long)headroom, STACK_LIMIT,
           (unsigned long)host_overhead,
           canary_ok ? "ok" : "DAMAGED", args.rc);
    if (strcmp(mode, "gui") == 0)
        printf(" flushes=%d changed_pixels=%lu checksum=%016llx",
               gui_flushes, gui_changed_pixels,
               (unsigned long long)gui_checksum);
    printf("\n");

    munmap(mapping, map_len);
    return failed;
}

int main(int argc, char **argv)
{
    char path[64];
    size_t fixture_len;
    int rc;

    setvbuf(stdout, 0, _IONBF, 0);
    if (argc != 2 ||
        (strcmp(argv[1], "text") != 0 && strcmp(argv[1], "gui") != 0)) {
        fprintf(stderr, "usage: %s text|gui\n", argv[0]);
        return 2;
    }
    if (make_fixture(path, &fixture_len) != 0) {
        fprintf(stderr, "cannot generate browser stack fixture: %s\n",
                strerror(errno));
        return 1;
    }
    rc = guarded_run(argv[1], path, fixture_len);
    unlink(path);
    return rc;
}
