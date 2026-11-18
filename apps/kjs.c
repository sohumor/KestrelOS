/* kjs.c - KestrelOS standalone & native GUI JavaScript scripting runtime.
 *
 * Powered by libjs (ES5 + Promises + ArrayBuffer/Uint8Array) with direct
 * bindings to KestrelOS system calls, Talon parcels, Kernel Pulses, and
 * immediate-mode libgui windows.
 *
 * Usage:
 *   kjs -e "<code>" [args...]
 *   kjs <file.js> [args...]
 *   kjs                          (reads piped stdin script, or starts REPL)
 */

#include <kestrel.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"
#include "js.h"

#define KJS_MAX_PATH    256
#define KJS_MAX_SCRIPT  (4UL * 1024UL * 1024UL)
#define KJS_WIN_TAG     0x4B57494EU

struct kjs_win {
    gui_window *win;
    int close_emitted;
};

struct kjs_rt {
    js_ctx *ctx;
    js_object *win_proto;
    char cwd[KJS_MAX_PATH];
    uint64_t pulse_seq;
};

static struct kjs_rt g_rt;

/* ------------------------------------------------------------------ I/O */

static void err_puts(const char *s)
{
    unsigned long len;
    unsigned long off = 0;

    if (!s)
        return;
    len = strlen(s);
    while (off < len) {
        long w = write(2, s + off, len - off);
        if (w <= 0)
            break;
        off += (unsigned long)w;
    }
}

static void err_printf(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

static void err_printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if ((unsigned long)n >= sizeof(buf))
        buf[sizeof(buf) - 1] = '\0';
    err_puts(buf);
}

static int stdin_is_tty(void)
{
    int d = dup2_(0, 3);

    if (d >= 0) {
        close(d);
        return 0;
    }
    return 1;
}

/* -------------------------------------------------------------- helpers */

static int hex_val(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F')
        return 10 + (c - 'A');
    return -1;
}

static void normalize_path(char *path)
{
    char out[KJS_MAX_PATH];
    char seg[KJS_MAX_PATH];
    const char *p = path;
    int len = 1;

    out[0] = '/';
    out[1] = '\0';
    while (*p) {
        int n = 0;

        while (*p == '/')
            p++;
        while (*p && *p != '/' && n < (int)sizeof(seg) - 1)
            seg[n++] = *p++;
        seg[n] = '\0';
        if (n == 0)
            break;
        if (strcmp(seg, ".") == 0)
            continue;
        if (strcmp(seg, "..") == 0) {
            while (len > 1 && out[len - 1] != '/')
                len--;
            if (len > 1)
                len--;
            out[len] = '\0';
            continue;
        }
        if (len > 1 && len < (int)sizeof(out) - 1)
            out[len++] = '/';
        for (int i = 0; seg[i] && len < (int)sizeof(out) - 1; i++)
            out[len++] = seg[i];
        out[len] = '\0';
    }
    strncpy(path, out, KJS_MAX_PATH - 1);
    path[KJS_MAX_PATH - 1] = '\0';
}

static void resolve_path(const char *path, char *out, unsigned long outsz)
{
    if (!path || !path[0]) {
        snprintf(out, outsz, "%s", g_rt.cwd);
    } else if (path[0] == '/') {
        snprintf(out, outsz, "%s", path);
    } else if (strcmp(g_rt.cwd, "/") == 0) {
        snprintf(out, outsz, "/%s", path);
    } else {
        snprintf(out, outsz, "%s/%s", g_rt.cwd, path);
    }
    normalize_path(out);
}

static const char *ps_state_name(int state)
{
    switch (state) {
    case K_STATE_RUNNABLE: return "runnable";
    case K_STATE_RUNNING:  return "running";
    case K_STATE_SLEEPING: return "sleeping";
    case K_STATE_ZOMBIE:   return "zombie";
    case K_STATE_STOPPED:  return "stopped";
    default:               return "unknown";
    }
}

static const char *parcel_type_name(uint32_t t)
{
    switch (t) {
    case K_PARCEL_TEXT:   return "text";
    case K_PARCEL_PATH:   return "path";
    case K_PARCEL_URL:    return "url";
    case K_PARCEL_COLOR:  return "color";
    case K_PARCEL_NUMBER: return "number";
    case K_PARCEL_KREC:   return "krec";
    case K_PARCEL_IMAGE:  return "image";
    default:              return "unknown";
    }
}

static const char *parcel_default_mime(uint32_t t)
{
    switch (t) {
    case K_PARCEL_TEXT:   return "text/plain";
    case K_PARCEL_PATH:   return "text/x-path";
    case K_PARCEL_URL:    return "text/uri-list";
    case K_PARCEL_COLOR:  return "text/x-color";
    case K_PARCEL_NUMBER: return "text/x-number";
    case K_PARCEL_KREC:   return "text/x-krec";
    case K_PARCEL_IMAGE:  return "application/octet-stream";
    default:              return "application/octet-stream";
    }
}

static int parse_parcel_type_arg(js_ctx *ctx, js_value v, uint32_t *out)
{
    if (js_is_number(v)) {
        int32_t n = 0;
        if (js_to_int32(ctx, v, &n) != JS_OK)
            return -1;
        if (n < K_PARCEL_TEXT || n > K_PARCEL_IMAGE)
            return -1;
        *out = (uint32_t)n;
        return 0;
    }
    {
        const char *s = js_to_cstring(ctx, v);
        if (!s)
            return -1;
        if (strcmp(s, "text") == 0)   { *out = K_PARCEL_TEXT;   return 0; }
        if (strcmp(s, "path") == 0)   { *out = K_PARCEL_PATH;   return 0; }
        if (strcmp(s, "url") == 0)    { *out = K_PARCEL_URL;    return 0; }
        if (strcmp(s, "color") == 0)  { *out = K_PARCEL_COLOR;  return 0; }
        if (strcmp(s, "number") == 0) { *out = K_PARCEL_NUMBER; return 0; }
        if (strcmp(s, "krec") == 0)   { *out = K_PARCEL_KREC;   return 0; }
        if (strcmp(s, "image") == 0)  { *out = K_PARCEL_IMAGE;  return 0; }
    }
    return -1;
}

static int parse_color_arg(js_ctx *ctx, js_value v, uint32_t *out)
{
    if (js_is_string(v)) {
        const char *s = js_to_cstring(ctx, v);
        if (!s)
            return JS_THROW;
        if (s[0] == '#')
            s++;
        else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
            s += 2;
        if (strlen(s) == 6) {
            uint32_t c = 0;
            for (int i = 0; i < 6; i++) {
                int hv = hex_val(s[i]);
                if (hv < 0)
                    goto numeric;
                c = (c << 4) | (uint32_t)hv;
            }
            *out = c & 0x00FFFFFFU;
            return JS_OK;
        }
        if (strlen(s) == 3) {
            int r = hex_val(s[0]), g = hex_val(s[1]), b = hex_val(s[2]);
            if (r >= 0 && g >= 0 && b >= 0) {
                *out = ((uint32_t)(r * 17) << 16) |
                       ((uint32_t)(g * 17) << 8) | (uint32_t)(b * 17);
                return JS_OK;
            }
        }
    }
numeric:
    {
        uint32_t u = 0;
        if (js_to_uint32(ctx, v, &u) != JS_OK)
            return JS_THROW;
        *out = u & 0x00FFFFFFU;
        return JS_OK;
    }
}

static int read_fd_all(int fd, char **out_buf, unsigned long *out_len)
{
    unsigned long cap = 4096;
    unsigned long len = 0;
    char *buf = malloc(cap);

    if (!buf)
        return -1;
    for (;;) {
        if (len + 1024 + 1 > cap) {
            unsigned long ncap = cap * 2;
            char *nbuf;
            if (ncap > KJS_MAX_SCRIPT + 1)
                ncap = KJS_MAX_SCRIPT + 1;
            if (ncap <= cap) {
                free(buf);
                return -1;
            }
            nbuf = realloc(buf, ncap);
            if (!nbuf) {
                free(buf);
                return -1;
            }
            buf = nbuf;
            cap = ncap;
        }
        {
            long n = read(fd, buf + len, cap - len - 1);
            if (n < 0) {
                free(buf);
                return -1;
            }
            if (n == 0)
                break;
            len += (unsigned long)n;
        }
    }
    buf[len] = '\0';
    *out_buf = buf;
    if (out_len)
        *out_len = len;
    return 0;
}

/* ----------------------------------------------------------- print / OS */

static void kjs_print_sink(void *user, const char *text)
{
    (void)user;
    printf("%s\n", text ? text : "");
}

static int js_fn_print(js_ctx *ctx, js_value this_val,
                       int argc, js_value *argv, js_value *ret)
{
    (void)this_val;
    for (int i = 0; i < argc; i++) {
        js_value s;
        const char *bytes;
        unsigned long len = 0;

        if (i > 0)
            putchar(' ');
        if (js_to_string(ctx, argv[i], &s) != JS_OK)
            return JS_THROW;
        bytes = js_string_bytes(s, &len);
        if (bytes && len > 0)
            write(1, bytes, len);
    }
    putchar('\n');
    *ret = js_undefined();
    return JS_OK;
}

static int js_fn_uptime(js_ctx *ctx, js_value this_val,
                        int argc, js_value *argv, js_value *ret)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    *ret = js_number((double)uptime_ms());
    return JS_OK;
}

static int js_fn_pid(js_ctx *ctx, js_value this_val,
                     int argc, js_value *argv, js_value *ret)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    *ret = js_number((double)getpid());
    return JS_OK;
}

static int js_fn_uid(js_ctx *ctx, js_value this_val,
                     int argc, js_value *argv, js_value *ret)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    *ret = js_number((double)getuid_());
    return JS_OK;
}

static int js_fn_sleep(js_ctx *ctx, js_value this_val,
                       int argc, js_value *argv, js_value *ret)
{
    int32_t ms = 0;

    (void)this_val;
    if (argc >= 1 && !js_is_undefined(argv[0])) {
        if (js_to_int32(ctx, argv[0], &ms) != JS_OK)
            return JS_THROW;
    }
    if (ms > 0)
        sleep_ms((unsigned long)ms);
    *ret = js_undefined();
    return JS_OK;
}

static int js_fn_read_file(js_ctx *ctx, js_value this_val,
                           int argc, js_value *argv, js_value *ret)
{
    const char *arg;
    char path[KJS_MAX_PATH];
    struct k_stat st;
    char *buf = 0;
    unsigned long len = 0;
    int fd;

    (void)this_val;
    if (argc < 1) {
        *ret = js_null();
        return JS_OK;
    }
    arg = js_to_cstring(ctx, argv[0]);
    if (!arg)
        return JS_THROW;
    resolve_path(arg, path, sizeof(path));
    if (stat_(path, &st) != 0 || st.is_dir) {
        *ret = js_null();
        return JS_OK;
    }
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        *ret = js_null();
        return JS_OK;
    }
    if (read_fd_all(fd, &buf, &len) != 0) {
        close(fd);
        *ret = js_null();
        return JS_OK;
    }
    close(fd);
    *ret = js_mkstring(ctx, buf, len);
    free(buf);
    return js_is_string(*ret) ? JS_OK : JS_THROW;
}

static int js_fn_write_file(js_ctx *ctx, js_value this_val,
                            int argc, js_value *argv, js_value *ret)
{
    const char *arg;
    char path[KJS_MAX_PATH];
    js_value s;
    const char *bytes;
    unsigned long len = 0, off = 0;
    int fd;

    (void)this_val;
    if (argc < 2) {
        *ret = js_number(-1);
        return JS_OK;
    }
    arg = js_to_cstring(ctx, argv[0]);
    if (!arg)
        return JS_THROW;
    resolve_path(arg, path, sizeof(path));
    if (js_to_string(ctx, argv[1], &s) != JS_OK)
        return JS_THROW;
    bytes = js_string_bytes(s, &len);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        *ret = js_number(-1);
        return JS_OK;
    }
    while (off < len) {
        long w = write(fd, bytes + off, len - off);
        if (w <= 0) {
            close(fd);
            *ret = js_number(-1);
            return JS_OK;
        }
        off += (unsigned long)w;
    }
    close(fd);
    *ret = js_number((double)off);
    return JS_OK;
}

static int js_fn_stat(js_ctx *ctx, js_value this_val,
                      int argc, js_value *argv, js_value *ret)
{
    const char *arg;
    char path[KJS_MAX_PATH];
    struct k_stat st;
    js_object *obj;
    js_value ov;

    (void)this_val;
    if (argc < 1) {
        *ret = js_null();
        return JS_OK;
    }
    arg = js_to_cstring(ctx, argv[0]);
    if (!arg)
        return JS_THROW;
    resolve_path(arg, path, sizeof(path));
    if (stat_(path, &st) != 0) {
        *ret = js_null();
        return JS_OK;
    }
    obj = js_new_object(ctx);
    if (!obj)
        return JS_THROW;
    ov = js_object_value(obj);
    if (js_set(ctx, ov, "size", js_number((double)st.size)) != JS_OK ||
        js_set(ctx, ov, "isDir", js_bool(st.is_dir != 0)) != JS_OK ||
        js_set(ctx, ov, "mode", js_number((double)st.mode)) != JS_OK ||
        js_set(ctx, ov, "uid", js_number((double)st.uid)) != JS_OK ||
        js_set(ctx, ov, "gid", js_number((double)st.gid)) != JS_OK ||
        js_set(ctx, ov, "mtime", js_number((double)st.mtime)) != JS_OK)
        return JS_THROW;
    *ret = ov;
    return JS_OK;
}

static int js_fn_readdir(js_ctx *ctx, js_value this_val,
                         int argc, js_value *argv, js_value *ret)
{
    char path[KJS_MAX_PATH];
    struct k_stat st;
    struct k_dirent de;
    js_object *arr;
    int i = 0;

    (void)this_val;
    if (argc < 1 || js_is_undefined(argv[0])) {
        snprintf(path, sizeof(path), "%s", g_rt.cwd);
    } else {
        const char *arg = js_to_cstring(ctx, argv[0]);
        if (!arg)
            return JS_THROW;
        resolve_path(arg, path, sizeof(path));
    }
    if (stat_(path, &st) != 0 || !st.is_dir) {
        *ret = js_null();
        return JS_OK;
    }
    arr = js_new_array(ctx);
    if (!arr)
        return JS_THROW;
    while (readdir_at(path, i++, &de) == 0) {
        js_object *ent;
        js_value ev;

        de.name[sizeof(de.name) - 1] = '\0';
        if (strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
            continue;
        ent = js_new_object(ctx);
        if (!ent)
            return JS_THROW;
        ev = js_object_value(ent);
        if (js_set(ctx, ev, "name", js_mkcstring(ctx, de.name)) != JS_OK ||
            js_set(ctx, ev, "size", js_number((double)de.size)) != JS_OK ||
            js_set(ctx, ev, "isDir", js_bool(de.is_dir != 0)) != JS_OK ||
            js_set(ctx, ev, "mode", js_number((double)de.mode)) != JS_OK ||
            js_set(ctx, ev, "mtime", js_number((double)de.mtime)) != JS_OK ||
            js_array_push(ctx, arr, ev) != JS_OK)
            return JS_THROW;
    }
    *ret = js_object_value(arr);
    return JS_OK;
}

static int js_fn_mem_info(js_ctx *ctx, js_value this_val,
                          int argc, js_value *argv, js_value *ret)
{
    uint64_t total_kb = 0, free_kb = 0;
    js_object *obj;
    js_value ov;

    (void)this_val;
    (void)argc;
    (void)argv;
    if (meminfo(&total_kb, &free_kb) != 0) {
        *ret = js_null();
        return JS_OK;
    }
    obj = js_new_object(ctx);
    if (!obj)
        return JS_THROW;
    ov = js_object_value(obj);
    if (js_set(ctx, ov, "totalKb", js_number((double)total_kb)) != JS_OK ||
        js_set(ctx, ov, "freeKb", js_number((double)free_kb)) != JS_OK)
        return JS_THROW;
    *ret = ov;
    return JS_OK;
}

static int js_fn_ps(js_ctx *ctx, js_value this_val,
                    int argc, js_value *argv, js_value *ret)
{
    struct k_psinfo ps;
    js_object *arr;
    int i = 0;

    (void)this_val;
    (void)argc;
    (void)argv;
    arr = js_new_array(ctx);
    if (!arr)
        return JS_THROW;
    while (psinfo(i++, &ps) == 0) {
        js_object *obj = js_new_object(ctx);
        js_value ov;

        if (!obj)
            return JS_THROW;
        ps.name[sizeof(ps.name) - 1] = '\0';
        ov = js_object_value(obj);
        if (js_set(ctx, ov, "pid", js_number((double)ps.pid)) != JS_OK ||
            js_set(ctx, ov, "ppid", js_number((double)ps.ppid)) != JS_OK ||
            js_set(ctx, ov, "uid", js_number((double)ps.uid)) != JS_OK ||
            js_set(ctx, ov, "state",
                   js_mkcstring(ctx, ps_state_name(ps.state))) != JS_OK ||
            js_set(ctx, ov, "name", js_mkcstring(ctx, ps.name)) != JS_OK ||
            js_array_push(ctx, arr, ov) != JS_OK)
            return JS_THROW;
    }
    *ret = js_object_value(arr);
    return JS_OK;
}

static int js_fn_parcel_put(js_ctx *ctx, js_value this_val,
                            int argc, js_value *argv, js_value *ret)
{
    uint32_t ptype = K_PARCEL_TEXT;
    const char *label = "";
    js_value text_val;
    const char *bytes;
    unsigned long len = 0;
    int id;

    (void)this_val;
    if (argc < 3) {
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "parcelPut(type, label, text) requires 3 args");
    }
    if (parse_parcel_type_arg(ctx, argv[0], &ptype) != 0) {
        if (js_is_undefined(js_exception(ctx)))
            return js_throw_error(ctx, JS_ERR_TYPE, "invalid parcel type");
        return JS_THROW;
    }
    if (!js_is_undefined(argv[1]) && !js_is_null(argv[1])) {
        label = js_to_cstring(ctx, argv[1]);
        if (!label)
            return JS_THROW;
    }
    if (js_to_string(ctx, argv[2], &text_val) != JS_OK)
        return JS_THROW;
    bytes = js_string_bytes(text_val, &len);
    if (len > K_PARCEL_MAX_BYTES)
        len = K_PARCEL_MAX_BYTES;
    id = parcel_put(ptype, parcel_default_mime(ptype), label, bytes,
                    (uint32_t)len);
    *ret = js_number((double)id);
    return JS_OK;
}

static int js_fn_parcel_get(js_ctx *ctx, js_value this_val,
                            int argc, js_value *argv, js_value *ret)
{
    uint32_t slot = 0;
    struct k_parcel_info info;
    char *buf;
    long got;
    js_object *obj;
    js_value ov, tv;

    (void)this_val;
    if (argc >= 1 && !js_is_undefined(argv[0])) {
        if (js_to_uint32(ctx, argv[0], &slot) != JS_OK)
            return JS_THROW;
    }
    if (parcel_get(slot, &info, 0, 0) < 0) {
        *ret = js_null();
        return JS_OK;
    }
    if (info.size > K_PARCEL_MAX_BYTES) {
        *ret = js_null();
        return JS_OK;
    }
    buf = malloc(info.size ? info.size + 1 : 1);
    if (!buf)
        return js_throw_error(ctx, JS_ERR_ERROR, "out of memory");
    got = parcel_get(slot, &info, buf, info.size);
    if (got < 0) {
        free(buf);
        *ret = js_null();
        return JS_OK;
    }
    buf[got] = '\0';
    info.label[sizeof(info.label) - 1] = '\0';
    info.mime[sizeof(info.mime) - 1] = '\0';

    obj = js_new_object(ctx);
    if (!obj) {
        free(buf);
        return JS_THROW;
    }
    tv = js_mkstring(ctx, buf, (unsigned long)got);
    free(buf);
    if (!js_is_string(tv))
        return JS_THROW;
    ov = js_object_value(obj);
    if (js_set(ctx, ov, "id", js_number((double)info.id)) != JS_OK ||
        js_set(ctx, ov, "type",
               js_mkcstring(ctx, parcel_type_name(info.type))) != JS_OK ||
        js_set(ctx, ov, "label", js_mkcstring(ctx, info.label)) != JS_OK ||
        js_set(ctx, ov, "mime", js_mkcstring(ctx, info.mime)) != JS_OK ||
        js_set(ctx, ov, "text", tv) != JS_OK)
        return JS_THROW;
    *ret = ov;
    return JS_OK;
}

static int js_fn_pulse_poll(js_ctx *ctx, js_value this_val,
                            int argc, js_value *argv, js_value *ret)
{
    uint32_t mask = K_TOPIC_ALL;
    int32_t max_out = 16;
    struct k_pulse_query q;
    struct k_pulse *evs = 0;
    int n;
    js_object *res, *arr;
    js_value rv;

    (void)this_val;
    if (argc >= 1 && !js_is_undefined(argv[0])) {
        if (js_to_uint32(ctx, argv[0], &mask) != JS_OK)
            return JS_THROW;
    }
    if (argc >= 2 && !js_is_undefined(argv[1])) {
        if (js_to_int32(ctx, argv[1], &max_out) != JS_OK)
            return JS_THROW;
    }
    if (max_out < 0)
        max_out = 0;
    if (max_out > 64)
        max_out = 64;

    q.seq = g_rt.pulse_seq;
    if (argc >= 3 && !js_is_undefined(argv[2])) {
        double d = 0;
        if (js_to_integer(ctx, argv[2], &d) != JS_OK)
            return JS_THROW;
        q.seq = d > 0 ? (uint64_t)d : 0;
    }
    q.topic_mask = mask;
    q.max_out = (uint32_t)max_out;
    q.timeout_ms = 0;
    q._pad = 0;

    if (max_out > 0) {
        evs = malloc((unsigned long)max_out * sizeof(*evs));
        if (!evs)
            return js_throw_error(ctx, JS_ERR_ERROR, "out of memory");
    }
    n = pulse_poll(&q, evs);
    if (n >= 0)
        g_rt.pulse_seq = q.seq;

    res = js_new_object(ctx);
    arr = js_new_array(ctx);
    if (!res || !arr) {
        free(evs);
        return JS_THROW;
    }
    for (int i = 0; i < n; i++) {
        js_object *eo = js_new_object(ctx);
        js_value evv;

        if (!eo) {
            free(evs);
            return JS_THROW;
        }
        evs[i].tag[sizeof(evs[i].tag) - 1] = '\0';
        evv = js_object_value(eo);
        if (js_set(ctx, evv, "seq", js_number((double)evs[i].seq)) != JS_OK ||
            js_set(ctx, evv, "topic", js_number((double)evs[i].topic)) != JS_OK ||
            js_set(ctx, evv, "kind", js_number((double)evs[i].kind)) != JS_OK ||
            js_set(ctx, evv, "arg0", js_number((double)evs[i].arg0)) != JS_OK ||
            js_set(ctx, evv, "arg1", js_number((double)evs[i].arg1)) != JS_OK ||
            js_set(ctx, evv, "tag", js_mkcstring(ctx, evs[i].tag)) != JS_OK ||
            js_array_push(ctx, arr, evv) != JS_OK) {
            free(evs);
            return JS_THROW;
        }
    }
    free(evs);
    rv = js_object_value(res);
    if (js_set(ctx, rv, "seq", js_number((double)q.seq)) != JS_OK ||
        js_set(ctx, rv, "events", js_object_value(arr)) != JS_OK)
        return JS_THROW;
    *ret = rv;
    return JS_OK;
}

/* ----------------------------------------------------------- GUI window */

static void kjs_win_finalizer(void *user, void *host)
{
    struct kjs_win *kw = (struct kjs_win *)host;

    (void)user;
    if (!kw)
        return;
    if (kw->win) {
        gui_close(kw->win);
        kw->win = 0;
    }
    free(kw);
}

static struct kjs_win *win_self(js_ctx *ctx, js_value this_val)
{
    struct kjs_win *kw = (struct kjs_win *)js_host_ptr(this_val, KJS_WIN_TAG);

    if (!kw)
        js_throw_error(ctx, JS_ERR_TYPE, "illegal Window receiver");
    return kw;
}

static int win_get_width(js_ctx *ctx, js_value this_val,
                         int argc, js_value *argv, js_value *ret)
{
    struct kjs_win *kw = win_self(ctx, this_val);

    (void)argc;
    (void)argv;
    if (!kw)
        return JS_THROW;
    *ret = js_number(kw->win ? (double)kw->win->w : 0.0);
    return JS_OK;
}

static int win_get_height(js_ctx *ctx, js_value this_val,
                          int argc, js_value *argv, js_value *ret)
{
    struct kjs_win *kw = win_self(ctx, this_val);

    (void)argc;
    (void)argv;
    if (!kw)
        return JS_THROW;
    *ret = js_number(kw->win ? (double)kw->win->h : 0.0);
    return JS_OK;
}

static int win_get_closed(js_ctx *ctx, js_value this_val,
                          int argc, js_value *argv, js_value *ret)
{
    struct kjs_win *kw = win_self(ctx, this_val);

    (void)argc;
    (void)argv;
    if (!kw)
        return JS_THROW;
    *ret = js_bool(!kw->win || !kw->win->open);
    return JS_OK;
}

static int win_clear(js_ctx *ctx, js_value this_val,
                     int argc, js_value *argv, js_value *ret)
{
    struct kjs_win *kw = win_self(ctx, this_val);
    uint32_t col = GUI_SURFACE;

    if (!kw)
        return JS_THROW;
    if (argc >= 1 && !js_is_undefined(argv[0])) {
        if (parse_color_arg(ctx, argv[0], &col) != JS_OK)
            return JS_THROW;
    }
    if (kw->win && kw->win->open)
        gui_clear(kw->win, col);
    *ret = js_undefined();
    return JS_OK;
}

static int win_fill_rect(js_ctx *ctx, js_value this_val,
                         int argc, js_value *argv, js_value *ret)
{
    struct kjs_win *kw = win_self(ctx, this_val);
    int32_t x, y, w, h;
    uint32_t col = GUI_TEXT;

    if (!kw)
        return JS_THROW;
    if (argc < 5)
        return js_throw_error(ctx, JS_ERR_TYPE, "fillRect requires 5 args");
    if (js_to_int32(ctx, argv[0], &x) != JS_OK ||
        js_to_int32(ctx, argv[1], &y) != JS_OK ||
        js_to_int32(ctx, argv[2], &w) != JS_OK ||
        js_to_int32(ctx, argv[3], &h) != JS_OK ||
        parse_color_arg(ctx, argv[4], &col) != JS_OK)
        return JS_THROW;
    if (kw->win && kw->win->open)
        gui_rect(kw->win, x, y, w, h, col);
    *ret = js_undefined();
    return JS_OK;
}

static int win_round_rect(js_ctx *ctx, js_value this_val,
                          int argc, js_value *argv, js_value *ret)
{
    struct kjs_win *kw = win_self(ctx, this_val);
    int32_t x, y, w, h, r;
    uint32_t col = GUI_PANEL;

    if (!kw)
        return JS_THROW;
    if (argc < 6)
        return js_throw_error(ctx, JS_ERR_TYPE, "roundRect requires 6 args");
    if (js_to_int32(ctx, argv[0], &x) != JS_OK ||
        js_to_int32(ctx, argv[1], &y) != JS_OK ||
        js_to_int32(ctx, argv[2], &w) != JS_OK ||
        js_to_int32(ctx, argv[3], &h) != JS_OK ||
        js_to_int32(ctx, argv[4], &r) != JS_OK ||
        parse_color_arg(ctx, argv[5], &col) != JS_OK)
        return JS_THROW;
    if (kw->win && kw->win->open)
        gui_fill_round_rect(kw->win, gui_mkrc(x, y, w, h), r, col);
    *ret = js_undefined();
    return JS_OK;
}

static int win_line(js_ctx *ctx, js_value this_val,
                    int argc, js_value *argv, js_value *ret)
{
    struct kjs_win *kw = win_self(ctx, this_val);
    int32_t x0, y0, x1, y1, width = 1;
    uint32_t col = GUI_TEXT;

    if (!kw)
        return JS_THROW;
    if (argc < 5)
        return js_throw_error(ctx, JS_ERR_TYPE, "line requires 5 args");
    if (js_to_int32(ctx, argv[0], &x0) != JS_OK ||
        js_to_int32(ctx, argv[1], &y0) != JS_OK ||
        js_to_int32(ctx, argv[2], &x1) != JS_OK ||
        js_to_int32(ctx, argv[3], &y1) != JS_OK ||
        parse_color_arg(ctx, argv[4], &col) != JS_OK)
        return JS_THROW;
    if (argc >= 6 && !js_is_undefined(argv[5])) {
        if (js_to_int32(ctx, argv[5], &width) != JS_OK)
            return JS_THROW;
    }
    if (kw->win && kw->win->open) {
        if (width > 1)
            gui_line_w(kw->win, x0, y0, x1, y1, col, width);
        else
            gui_line(kw->win, x0, y0, x1, y1, col);
    }
    *ret = js_undefined();
    return JS_OK;
}

static int win_circle(js_ctx *ctx, js_value this_val,
                      int argc, js_value *argv, js_value *ret)
{
    struct kjs_win *kw = win_self(ctx, this_val);
    int32_t cx, cy, r;
    uint32_t col = GUI_ACCENT;
    int fill = 0;

    if (!kw)
        return JS_THROW;
    if (argc < 4)
        return js_throw_error(ctx, JS_ERR_TYPE, "circle requires 4 args");
    if (js_to_int32(ctx, argv[0], &cx) != JS_OK ||
        js_to_int32(ctx, argv[1], &cy) != JS_OK ||
        js_to_int32(ctx, argv[2], &r) != JS_OK ||
        parse_color_arg(ctx, argv[3], &col) != JS_OK)
        return JS_THROW;
    if (argc >= 5)
        fill = js_to_boolean(argv[4]);
    if (kw->win && kw->win->open) {
        if (fill)
            gui_fill_circle(kw->win, cx, cy, r, col);
        else
            gui_draw_circle(kw->win, cx, cy, r, col);
    }
    *ret = js_undefined();
    return JS_OK;
}

static int win_text(js_ctx *ctx, js_value this_val,
                    int argc, js_value *argv, js_value *ret)
{
    struct kjs_win *kw = win_self(ctx, this_val);
    int32_t x, y, sz = FONT_BODY;
    int weight = FONT_REGULAR;
    const char *s;
    uint32_t col = GUI_TEXT;
    int next_x = 0;

    if (!kw)
        return JS_THROW;
    if (argc < 3)
        return js_throw_error(ctx, JS_ERR_TYPE, "text requires at least 3 args");
    if (js_to_int32(ctx, argv[0], &x) != JS_OK ||
        js_to_int32(ctx, argv[1], &y) != JS_OK)
        return JS_THROW;
    s = js_to_cstring(ctx, argv[2]);
    if (!s)
        return JS_THROW;
    if (argc >= 4 && !js_is_undefined(argv[3])) {
        if (parse_color_arg(ctx, argv[3], &col) != JS_OK)
            return JS_THROW;
    }
    if (argc >= 5 && !js_is_undefined(argv[4])) {
        if (js_to_int32(ctx, argv[4], &sz) != JS_OK)
            return JS_THROW;
    }
    if (argc >= 6 && js_to_boolean(argv[5]))
        weight = FONT_BOLD;
    if (kw->win && kw->win->open)
        next_x = gui_draw_text_aa(kw->win, x, y, s, sz, weight, FONT_ROMAN,
                                  col, GUI_TRANSPARENT);
    *ret = js_number((double)next_x);
    return JS_OK;
}

static int win_sparkline(js_ctx *ctx, js_value this_val,
                         int argc, js_value *argv, js_value *ret)
{
    struct kjs_win *kw = win_self(ctx, this_val);
    int32_t x, y, w, h, max_val = 0;
    uint32_t col = GUI_ACCENT;
    long fill_col;
    int sstack[128];
    int *samples = sstack;
    unsigned long count = 0;

    if (!kw)
        return JS_THROW;
    if (argc < 6)
        return js_throw_error(ctx, JS_ERR_TYPE, "sparkline requires 6 args");
    if (js_to_int32(ctx, argv[0], &x) != JS_OK ||
        js_to_int32(ctx, argv[1], &y) != JS_OK ||
        js_to_int32(ctx, argv[2], &w) != JS_OK ||
        js_to_int32(ctx, argv[3], &h) != JS_OK ||
        parse_color_arg(ctx, argv[5], &col) != JS_OK)
        return JS_THROW;
    fill_col = (long)col;
    if (argc >= 7 && !js_is_undefined(argv[6])) {
        if (js_to_int32(ctx, argv[6], &max_val) != JS_OK)
            return JS_THROW;
    }
    if (argc >= 8 && !js_is_undefined(argv[7])) {
        if (js_is_null(argv[7]) || (js_is_bool(argv[7]) && !js_to_boolean(argv[7]))) {
            fill_col = -1L;
        } else {
            uint32_t fc = 0;
            if (parse_color_arg(ctx, argv[7], &fc) != JS_OK)
                return JS_THROW;
            fill_col = (long)fc;
        }
    }

    if (!js_is_object(argv[4])) {
        *ret = js_undefined();
        return JS_OK;
    }
    if (js_is_array(argv[4])) {
        count = js_array_length(argv[4].u.obj);
    } else {
        js_value lv;
        uint32_t u = 0;
        if (js_get(ctx, argv[4], "length", &lv) != JS_OK)
            return JS_THROW;
        if (js_to_uint32(ctx, lv, &u) != JS_OK)
            return JS_THROW;
        count = u;
    }
    if (count == 0) {
        *ret = js_undefined();
        return JS_OK;
    }
    if (count > 1024)
        count = 1024;
    if (count > 128) {
        samples = malloc(count * sizeof(int));
        if (!samples)
            return js_throw_error(ctx, JS_ERR_ERROR, "out of memory");
    }
    for (unsigned long i = 0; i < count; i++) {
        js_value elem;
        double d = 0;

        if (js_is_array(argv[4])) {
            if (js_array_get(ctx, argv[4].u.obj, i, &elem) != JS_OK)
                goto fail;
        } else {
            char idx[16];
            snprintf(idx, sizeof(idx), "%lu", i);
            if (js_get(ctx, argv[4], idx, &elem) != JS_OK)
                goto fail;
        }
        if (js_to_number(ctx, elem, &d) != JS_OK)
            goto fail;
        samples[i] = (d >= 0.0 && d <= 2147483647.0) ? (int)d : 0;
    }
    if (kw->win && kw->win->open)
        gui_draw_sparkline(kw->win, gui_mkrc(x, y, w, h), samples, (int)count,
                           max_val, col, fill_col);
    if (samples != sstack)
        free(samples);
    *ret = js_undefined();
    return JS_OK;

fail:
    if (samples != sstack)
        free(samples);
    return JS_THROW;
}

static int win_flush(js_ctx *ctx, js_value this_val,
                     int argc, js_value *argv, js_value *ret)
{
    struct kjs_win *kw = win_self(ctx, this_val);
    int ok = 0;

    (void)argc;
    (void)argv;
    if (!kw)
        return JS_THROW;
    if (kw->win && kw->win->open)
        ok = (gui_flush(kw->win) == 0);
    *ret = js_bool(ok);
    return JS_OK;
}

static const char *kev_type_name(uint32_t t)
{
    switch (t) {
    case KEV_KEY:         return "key";
    case KEV_MOUSE_DOWN:  return "down";
    case KEV_MOUSE_UP:    return "up";
    case KEV_MOUSE_MOVE:  return "move";
    case KEV_MOUSE_WHEEL: return "wheel";
    case KEV_CLOSE:       return "close";
    case KEV_RESIZE:      return "resize";
    case KEV_FOCUS:       return "focus";
    case KEV_THEME:       return "theme";
    default:              return "none";
    }
}

static js_value make_event_obj(js_ctx *ctx, const char *type, int32_t x,
                               int32_t y, double key, uint32_t buttons)
{
    js_object *o = js_new_object(ctx);
    js_value ov;

    if (!o)
        return js_undefined();
    ov = js_object_value(o);
    if (js_set(ctx, ov, "type", js_mkcstring(ctx, type)) != JS_OK ||
        js_set(ctx, ov, "x", js_number((double)x)) != JS_OK ||
        js_set(ctx, ov, "y", js_number((double)y)) != JS_OK ||
        js_set(ctx, ov, "key", js_number(key)) != JS_OK ||
        js_set(ctx, ov, "buttons", js_number((double)buttons)) != JS_OK)
        return js_undefined();
    return ov;
}

static int win_poll_event(js_ctx *ctx, js_value this_val,
                          int argc, js_value *argv, js_value *ret)
{
    struct kjs_win *kw = win_self(ctx, this_val);
    int32_t timeout_ms = 0;
    struct k_event ev;
    int got;

    if (!kw)
        return JS_THROW;
    if (argc >= 1 && !js_is_undefined(argv[0])) {
        if (js_to_int32(ctx, argv[0], &timeout_ms) != JS_OK)
            return JS_THROW;
    }
    if (timeout_ms < 0)
        timeout_ms = 0;
    if (!kw->win) {
        *ret = js_null();
        return JS_OK;
    }
    if (!kw->win->open) {
        if (!kw->close_emitted) {
            kw->close_emitted = 1;
            *ret = make_event_obj(ctx, "close", 0, 0, 0.0, 0);
            return js_is_object(*ret) ? JS_OK : JS_THROW;
        }
        *ret = js_null();
        return JS_OK;
    }

    got = gui_next_event(kw->win, &ev, timeout_ms);
    if (got < 0) {
        kw->close_emitted = 1;
        *ret = make_event_obj(ctx, "close", 0, 0, 0.0, 0);
        return js_is_object(*ret) ? JS_OK : JS_THROW;
    }
    if (got == 0) {
        *ret = js_null();
        return JS_OK;
    }
    if (ev.type == KEV_CLOSE)
        kw->close_emitted = 1;
    {
        double k = (ev.type == KEV_MOUSE_WHEEL)
                       ? (double)(int32_t)ev.key
                       : (double)ev.key;
        *ret = make_event_obj(ctx, kev_type_name(ev.type), ev.x, ev.y, k,
                              ev.buttons);
        return js_is_object(*ret) ? JS_OK : JS_THROW;
    }
}

static int win_sleep(js_ctx *ctx, js_value this_val,
                     int argc, js_value *argv, js_value *ret)
{
    struct kjs_win *kw = win_self(ctx, this_val);

    if (!kw)
        return JS_THROW;
    return js_fn_sleep(ctx, this_val, argc, argv, ret);
}

static int win_close(js_ctx *ctx, js_value this_val,
                     int argc, js_value *argv, js_value *ret)
{
    struct kjs_win *kw = win_self(ctx, this_val);

    (void)argc;
    (void)argv;
    if (!kw)
        return JS_THROW;
    if (kw->win) {
        gui_close(kw->win);
        kw->win = 0;
    }
    kw->close_emitted = 1;
    *ret = js_undefined();
    return JS_OK;
}

static int js_fn_open_window(js_ctx *ctx, js_value this_val,
                             int argc, js_value *argv, js_value *ret)
{
    const char *title = "KestrelJS";
    int32_t w = 520, h = 360;
    int sw = 1024, sh = 768;
    int x = 100, y = 70;
    gui_window *win;
    struct kjs_win *kw;
    js_object *obj;

    (void)this_val;
    if (argc >= 1 && !js_is_undefined(argv[0])) {
        title = js_to_cstring(ctx, argv[0]);
        if (!title)
            return JS_THROW;
    }
    if (argc >= 2 && !js_is_undefined(argv[1])) {
        if (js_to_int32(ctx, argv[1], &w) != JS_OK)
            return JS_THROW;
    }
    if (argc >= 3 && !js_is_undefined(argv[2])) {
        if (js_to_int32(ctx, argv[2], &h) != JS_OK)
            return JS_THROW;
    }
    if (w <= 0 || h <= 0) {
        *ret = js_null();
        return JS_OK;
    }
    if (gui_screen(&sw, &sh) == 0) {
        x = (sw - w) / 2;
        y = (sh - h) / 2;
        if (x < 24)
            x = 24;
        if (y < 44)
            y = 44;
    }
    win = gui_open(title, x, y, w, h, 0);
    if (!win) {
        *ret = js_null();
        return JS_OK;
    }
    kw = malloc(sizeof(*kw));
    if (!kw) {
        gui_close(win);
        return js_throw_error(ctx, JS_ERR_ERROR, "out of memory");
    }
    kw->win = win;
    kw->close_emitted = 0;
    obj = js_new_host(ctx, kw, KJS_WIN_TAG, g_rt.win_proto);
    if (!obj) {
        gui_close(win);
        free(kw);
        return JS_THROW;
    }
    *ret = js_object_value(obj);
    return JS_OK;
}

/* ----------------------------------------------------------- setup/run */

static int init_runtime(int user_argc, char **user_argv)
{
    js_config cfg;
    js_object *glob, *kobj, *args_arr, *wp;

    js_config_default(&cfg);
    cfg.max_steps = 0; /* interactive/GUI scripts run until exit */
    cfg.print = kjs_print_sink;
    g_rt.ctx = js_new(&cfg);
    if (!g_rt.ctx)
        return -1;
    if (js_host_finalizer(g_rt.ctx, KJS_WIN_TAG, kjs_win_finalizer) != 0)
        return -1;

    glob = js_global(g_rt.ctx).u.obj;
    if (js_define_native(g_rt.ctx, glob, "print", js_fn_print, 1) != JS_OK)
        return -1;

    wp = js_new_object(g_rt.ctx);
    if (!wp)
        return -1;
    g_rt.win_proto = wp;
    if (js_define_accessor(g_rt.ctx, wp, "width", win_get_width, 0, 1) != JS_OK ||
        js_define_accessor(g_rt.ctx, wp, "height", win_get_height, 0, 1) != JS_OK ||
        js_define_accessor(g_rt.ctx, wp, "closed", win_get_closed, 0, 1) != JS_OK ||
        js_define_native(g_rt.ctx, wp, "clear", win_clear, 1) != JS_OK ||
        js_define_native(g_rt.ctx, wp, "fillRect", win_fill_rect, 5) != JS_OK ||
        js_define_native(g_rt.ctx, wp, "roundRect", win_round_rect, 6) != JS_OK ||
        js_define_native(g_rt.ctx, wp, "line", win_line, 5) != JS_OK ||
        js_define_native(g_rt.ctx, wp, "circle", win_circle, 4) != JS_OK ||
        js_define_native(g_rt.ctx, wp, "text", win_text, 4) != JS_OK ||
        js_define_native(g_rt.ctx, wp, "sparkline", win_sparkline, 6) != JS_OK ||
        js_define_native(g_rt.ctx, wp, "flush", win_flush, 0) != JS_OK ||
        js_define_native(g_rt.ctx, wp, "pollEvent", win_poll_event, 1) != JS_OK ||
        js_define_native(g_rt.ctx, wp, "sleep", win_sleep, 1) != JS_OK ||
        js_define_native(g_rt.ctx, wp, "close", win_close, 0) != JS_OK)
        return -1;

    kobj = js_new_object(g_rt.ctx);
    args_arr = js_new_array(g_rt.ctx);
    if (!kobj || !args_arr)
        return -1;
    for (int i = 0; i < user_argc; i++) {
        if (js_array_push(g_rt.ctx, args_arr,
                          js_mkcstring(g_rt.ctx, user_argv[i])) != JS_OK)
            return -1;
    }

    if (js_define_enum(g_rt.ctx, kobj, "cwd",
                       js_mkcstring(g_rt.ctx, g_rt.cwd)) != JS_OK ||
        js_define_enum(g_rt.ctx, kobj, "args",
                       js_object_value(args_arr)) != JS_OK ||
        js_define_native(g_rt.ctx, kobj, "uptime", js_fn_uptime, 0) != JS_OK ||
        js_define_native(g_rt.ctx, kobj, "pid", js_fn_pid, 0) != JS_OK ||
        js_define_native(g_rt.ctx, kobj, "uid", js_fn_uid, 0) != JS_OK ||
        js_define_native(g_rt.ctx, kobj, "sleep", js_fn_sleep, 1) != JS_OK ||
        js_define_native(g_rt.ctx, kobj, "readFile", js_fn_read_file, 1) != JS_OK ||
        js_define_native(g_rt.ctx, kobj, "writeFile", js_fn_write_file, 2) != JS_OK ||
        js_define_native(g_rt.ctx, kobj, "stat", js_fn_stat, 1) != JS_OK ||
        js_define_native(g_rt.ctx, kobj, "readdir", js_fn_readdir, 1) != JS_OK ||
        js_define_native(g_rt.ctx, kobj, "memInfo", js_fn_mem_info, 0) != JS_OK ||
        js_define_native(g_rt.ctx, kobj, "ps", js_fn_ps, 0) != JS_OK ||
        js_define_native(g_rt.ctx, kobj, "parcelPut", js_fn_parcel_put, 3) != JS_OK ||
        js_define_native(g_rt.ctx, kobj, "parcelGet", js_fn_parcel_get, 1) != JS_OK ||
        js_define_native(g_rt.ctx, kobj, "pulsePoll", js_fn_pulse_poll, 2) != JS_OK ||
        js_define_native(g_rt.ctx, kobj, "openWindow", js_fn_open_window, 3) != JS_OK)
        return -1;

    if (js_define(g_rt.ctx, glob, "Kestrel", js_object_value(kobj)) != JS_OK)
        return -1;
    return 0;
}

static void report_exception(js_ctx *ctx, js_value err)
{
    const char *msg = js_error_text(ctx, err);

    err_printf("%s\n", msg ? msg : "uncaught exception");
}

static int eval_chunk(const char *src, const char *name, int print_result)
{
    js_value res = js_undefined();
    int rc;

    rc = js_eval(g_rt.ctx, src, name, &res);
    if (rc != JS_OK || js_fatal(g_rt.ctx)) {
        report_exception(g_rt.ctx, res);
        js_clear_exception(g_rt.ctx);
        return 1;
    }
    if (js_run_jobs(g_rt.ctx, 4096) < 0 || js_fatal(g_rt.ctx)) {
        js_value ex = js_exception(g_rt.ctx);
        report_exception(g_rt.ctx,
                         js_is_undefined(ex) ? js_mkcstring(g_rt.ctx, "promise job error") : ex);
        js_clear_exception(g_rt.ctx);
        return 1;
    }
    if (print_result && !js_is_undefined(res)) {
        const char *s = js_to_cstring(g_rt.ctx, res);
        if (s)
            printf("%s\n", s);
        else
            js_clear_exception(g_rt.ctx);
    }
    return 0;
}

static int run_repl(void)
{
    char line[512];

    printf("KestrelJS (libjs ES5) - type .exit or Ctrl-D to quit\n");
    for (;;) {
        char *p;

        printf("kjs> ");
        if (!readline(line, (int)sizeof(line))) {
            putchar('\n');
            break;
        }
        p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            continue;
        if (strcmp(p, ".exit") == 0 || strcmp(p, ".quit") == 0)
            break;
        if (strcmp(p, ".help") == 0) {
            printf("Builtins: print, console.log, Kestrel.{cwd,args,uptime,pid,uid,\n");
            printf("  readFile,writeFile,stat,readdir,memInfo,ps,parcelPut,parcelGet,\n");
            printf("  pulsePoll,openWindow}\n");
            continue;
        }
        js_reset_steps(g_rt.ctx);
        (void)eval_chunk(p, "<repl>", 1);
        if (js_fatal(g_rt.ctx))
            return 1;
    }
    return 0;
}

static void usage(void)
{
    printf("usage:\n");
    printf("  kjs -e \"<code>\" [args...]\n");
    printf("  kjs <file.js> [args...]\n");
    printf("  kjs                  (piped stdin or interactive REPL)\n");
}

int main(int argc, char **argv)
{
    const char *eval_code = 0;
    const char *file_arg = 0;
    int user_arg_start = argc;
    int i, rc = 0;

    memset(&g_rt, 0, sizeof(g_rt));
    strcpy(g_rt.cwd, "/");

    /* Strip --cwd=<path> first, preserving order of script and user args. */
    {
        int out = 1;
        for (i = 1; i < argc; i++) {
            if (strncmp(argv[i], "--cwd=", 6) == 0) {
                snprintf(g_rt.cwd, sizeof(g_rt.cwd), "%s", argv[i] + 6);
                normalize_path(g_rt.cwd);
            } else {
                argv[out++] = argv[i];
            }
        }
        argv[out] = 0;
        argc = out;
    }

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else if (strcmp(argv[i], "-e") == 0) {
            if (i + 1 >= argc) {
                err_puts("kjs: -e requires a script string\n");
                return 1;
            }
            eval_code = argv[i + 1];
            user_arg_start = i + 2;
            break;
        } else if (strcmp(argv[i], "--") == 0) {
            if (i + 1 < argc) {
                file_arg = argv[i + 1];
                user_arg_start = i + 2;
            }
            break;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            err_printf("kjs: unknown option '%s'\n", argv[i]);
            return 1;
        } else {
            file_arg = argv[i];
            user_arg_start = i + 1;
            break;
        }
    }

    if (user_arg_start < argc && strcmp(argv[user_arg_start], "--") == 0)
        user_arg_start++;

    if (init_runtime(argc - user_arg_start, argv + user_arg_start) != 0) {
        err_puts("kjs: failed to initialize JavaScript context\n");
        if (g_rt.ctx)
            js_free(g_rt.ctx);
        return 1;
    }

    if (eval_code) {
        rc = eval_chunk(eval_code, "<eval>", 0);
    } else if (file_arg) {
        char path[KJS_MAX_PATH];
        struct k_stat st;
        char *src = 0;
        int fd;

        resolve_path(file_arg, path, sizeof(path));
        if (stat_(path, &st) != 0) {
            err_printf("kjs: cannot open '%s'\n", path);
            rc = 1;
        } else if (st.is_dir) {
            err_printf("kjs: '%s' is a directory\n", path);
            rc = 1;
        } else if ((fd = open(path, O_RDONLY)) < 0) {
            err_printf("kjs: cannot open '%s'\n", path);
            rc = 1;
        } else if (read_fd_all(fd, &src, 0) != 0) {
            close(fd);
            err_printf("kjs: failed reading '%s'\n", path);
            rc = 1;
        } else {
            close(fd);
            rc = eval_chunk(src, path, 0);
            free(src);
        }
    } else if (!stdin_is_tty()) {
        char *src = 0;
        if (read_fd_all(0, &src, 0) != 0) {
            err_puts("kjs: failed reading stdin\n");
            rc = 1;
        } else {
            rc = eval_chunk(src, "<stdin>", 0);
            free(src);
        }
    } else {
        rc = run_repl();
    }

    js_free(g_rt.ctx);
    return rc;
}
