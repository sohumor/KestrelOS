/* inspect.c - KestrelOS Universal Live Object Inspector (CLI).
 *
 * Inspects live kernel and userspace objects: processes, compositor windows,
 * Talon parcels, network configuration, VFS paths/@krec files, modules,
 * devices, mounts, block devices, kernel pulses, and system telemetry.
 *
 * usage:
 *   inspect [target] [-r] [--pin] [-n N]
 *
 * targets:
 *   sys                 System overview (default when no target is given)
 *   procs | proc:<pid>  Process table or single process
 *   wins  | win:<wid>   Compositor windows or single window
 *   parcels | parcel:<id> Talon parcels or decoded single parcel
 *   net                 Network interface configuration
 *   fs:<path> | /<path> File/directory stat, preview, and @krec schema
 *   mod                 Loaded kernel modules
 *   dev                 Hardware device tree
 *   mount               Mounted filesystems
 *   blk                 Block devices
 *   pulse [-n N]        Recent kernel pulses
 */

#include <kestrel.h>
#include <kestrel_cwd.h>
#include <kestrel_pulse.h>
#include <krec.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PATH_LEN   256
#define MAX_PULSES     128
#define MAX_WINS       64
#define FS_PREVIEW_MAX 1024

enum target_kind {
    TGT_SYS = 0,
    TGT_PROCS,
    TGT_PROC_ONE,
    TGT_WINS,
    TGT_WIN_ONE,
    TGT_PARCELS,
    TGT_PARCEL_ONE,
    TGT_NET,
    TGT_FS,
    TGT_MOD,
    TGT_DEV,
    TGT_MOUNT,
    TGT_BLK,
    TGT_PULSE
};

struct capture_buf {
    int enabled;
    char *data;
    size_t len;
    size_t cap;
    int truncated;
};

static const char *g_cwd = "/";
static struct capture_buf g_cap;

/* ---- output & capture helpers ---------------------------------------- */

static void cap_init(int enable)
{
    memset(&g_cap, 0, sizeof(g_cap));
    if (!enable)
        return;
    g_cap.data = (char *)malloc(K_PARCEL_MAX_BYTES + 1);
    if (!g_cap.data)
        return;
    g_cap.enabled = 1;
    g_cap.cap = K_PARCEL_MAX_BYTES;
    g_cap.data[0] = '\0';
}

static void cap_append_n(const char *s, size_t n)
{
    size_t take;

    if (!g_cap.enabled || !s || n == 0)
        return;
    if (g_cap.len >= g_cap.cap) {
        g_cap.truncated = 1;
        return;
    }
    take = n;
    if (g_cap.len + take > g_cap.cap) {
        take = g_cap.cap - g_cap.len;
        g_cap.truncated = 1;
    }
    memcpy(g_cap.data + g_cap.len, s, take);
    g_cap.len += take;
    g_cap.data[g_cap.len] = '\0';
}

static void out_printf(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

static void out_printf(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if ((size_t)n >= sizeof(buf))
        n = (int)sizeof(buf) - 1;
    write(1, buf, (unsigned long)n);
    cap_append_n(buf, (size_t)n);
}

static void cap_krec_escape(const char *s)
{
    if (!s || !g_cap.enabled)
        return;
    for (; *s; s++) {
        switch (*s) {
        case '\\': cap_append_n("\\\\", 2); break;
        case '\t': cap_append_n("\\t", 2);  break;
        case '\n': cap_append_n("\\n", 2);  break;
        case '\r': cap_append_n("\\r", 2);  break;
        default:   cap_append_n(s, 1);      break;
        }
    }
}

static int emit_krec_begin(const struct krec_col *cols, int ncols)
{
    int i;

    if (krec_begin(1, cols, ncols) < 0)
        return -1;
    if (g_cap.enabled) {
        cap_append_n("@krec", 5);
        for (i = 0; i < ncols; i++) {
            cap_append_n(" ", 1);
            cap_append_n(cols[i].name, strlen(cols[i].name));
            cap_append_n(":", 1);
            const char *tn = krec_type_name(cols[i].type);
            cap_append_n(tn, strlen(tn));
        }
        cap_append_n("\n", 1);
    }
    return 0;
}

static int emit_krec_row(int ncols, ...)
{
    const char *vals[KREC_MAX_COLS];
    va_list ap;
    int i;

    if (ncols <= 0 || ncols > KREC_MAX_COLS)
        return -1;
    va_start(ap, ncols);
    for (i = 0; i < ncols; i++)
        vals[i] = va_arg(ap, const char *);
    va_end(ap);

    if (krec_row_vals(1, ncols, vals) < 0)
        return -1;
    if (g_cap.enabled) {
        for (i = 0; i < ncols; i++) {
            if (i > 0)
                cap_append_n("\t", 1);
            cap_krec_escape(vals[i] ? vals[i] : "");
        }
        cap_append_n("\n", 1);
    }
    return 0;
}

/* ---- thin syscall wrappers ------------------------------------------- */

static int modlist_at(int idx, struct k_modinfo *out)
{
    return (int)syscall(SYS_MODLIST, idx, (long)out, 0, 0);
}

static int devlist_at(int idx, struct k_devinfo *out)
{
    return (int)syscall(SYS_DEVLIST, idx, (long)out, 0, 0);
}

static int mountlist_at(int idx, struct k_mountinfo *out)
{
    return (int)syscall(SYS_MOUNTLIST, idx, (long)out, 0, 0);
}

static int blklist_at(int idx, struct k_blkinfo *out)
{
    return (int)syscall(SYS_BLKLIST, idx, (long)out, 0, 0);
}

static int winlist_at(int idx, struct k_winsummary *out)
{
    return (int)syscall(SYS_WIN_LIST, idx, (long)out, 0, 0);
}

static int fbinfo_get(struct k_fbinfo *out)
{
    return (int)syscall(SYS_FBINFO, (long)out, 0, 0, 0);
}

/* ---- formatting helpers ---------------------------------------------- */

static void card_kv(const char *key, const char *val)
{
    out_printf("  %-14s : %s\n", key, (val && val[0]) ? val : "-");
}

static void card_header(const char *title)
{
    out_printf("[%s]\n", title);
}

static const char *proc_state_name(int state)
{
    switch (state) {
    case K_STATE_RUNNABLE: return "runnable";
    case K_STATE_RUNNING:  return "running";
    case K_STATE_SLEEPING: return "sleeping";
    case K_STATE_ZOMBIE:   return "zombie";
    case K_STATE_STOPPED:  return "stopped";
    default:               return "?";
    }
}

static const char *mod_state_name(uint32_t state)
{
    switch (state) {
    case K_MOD_LOADING:   return "loading";
    case K_MOD_LIVE:      return "live";
    case K_MOD_UNLOADING: return "unloading";
    default:              return "?";
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
    default:              return "none";
    }
}

static const char *pulse_topic_name(uint32_t topic)
{
    switch (topic) {
    case K_TOPIC_PROC:   return "proc";
    case K_TOPIC_WIN:    return "win";
    case K_TOPIC_THEME:  return "theme";
    case K_TOPIC_FS:     return "fs";
    case K_TOPIC_NET:    return "net";
    case K_TOPIC_PARCEL: return "parcel";
    default:             return "other";
    }
}

static const char *pulse_kind_name(uint32_t kind)
{
    switch (kind) {
    case K_PULSE_PROC_SPAWN: return "proc_spawn";
    case K_PULSE_PROC_EXIT:  return "proc_exit";
    case K_PULSE_WIN_OPEN:   return "win_open";
    case K_PULSE_WIN_CLOSE:  return "win_close";
    case K_PULSE_WIN_FOCUS:  return "win_focus";
    case K_PULSE_WIN_WS:     return "win_ws";
    case K_PULSE_THEME_SET:  return "theme_set";
    case K_PULSE_FS_CHANGE:  return "fs_change";
    case K_PULSE_NET_CHANGE: return "net_change";
    case K_PULSE_PARCEL_NEW: return "parcel_new";
    default:                 return "unknown";
    }
}

static const char *pci_class_name(uint32_t class_id)
{
    switch (class_id >> 8) {
    case 0x00: return "legacy";
    case 0x01: return "storage";
    case 0x02: return "network";
    case 0x03: return "display";
    case 0x04: return "multimedia";
    case 0x05: return "memory";
    case 0x06: return "bridge";
    case 0x07: return "comm";
    case 0x08: return "system";
    case 0x09: return "input";
    case 0x0C: return "serialbus";
    case 0x0D: return "wireless";
    default:   return 0;
    }
}

static void fmt_mode(uint32_t mode, char out[10])
{
    static const char rwx[3] = { 'r', 'w', 'x' };
    int i;

    for (i = 0; i < 9; i++)
        out[i] = (mode & (0400U >> i)) ? rwx[i % 3] : '-';
    out[9] = '\0';
}

static int is_leap_year(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static void fmt_time_utc(uint32_t secs_u, char *out, size_t outsz)
{
    static const int mdays[12] = { 31, 28, 31, 30, 31, 30,
                                   31, 31, 30, 31, 30, 31 };
    unsigned long days, rem;
    int year = 1970, mon = 1, hour, min, sec;

    if (secs_u == 0) {
        snprintf(out, outsz, "0 (unknown)");
        return;
    }
    days = (unsigned long)secs_u / 86400UL;
    rem = (unsigned long)secs_u % 86400UL;
    hour = (int)(rem / 3600UL);
    min = (int)((rem / 60UL) % 60UL);
    sec = (int)(rem % 60UL);

    for (;;) {
        unsigned long len = is_leap_year(year) ? 366UL : 365UL;
        if (days < len)
            break;
        days -= len;
        year++;
    }
    for (;;) {
        int mlen = mdays[mon - 1] + (mon == 2 && is_leap_year(year) ? 1 : 0);
        if (days < (unsigned long)mlen)
            break;
        days -= (unsigned long)mlen;
        mon++;
    }
    snprintf(out, outsz, "%04d-%02d-%02d %02d:%02d:%02d UTC",
             year, mon, (int)days + 1, hour, min, sec);
}

static void sanitize_preview(const char *src, size_t len, char *out,
                             size_t outsz, size_t max_chars)
{
    size_t i = 0, o = 0, nonprint = 0;

    if (outsz == 0)
        return;
    for (i = 0; i < len && i < 64; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == 0 || (c < 0x20 && c != '\n' && c != '\r' && c != '\t') ||
            c >= 0x7f)
            nonprint++;
    }
    if (len > 0 && nonprint > len / 3) {
        size_t lim = len < 16 ? len : 16;
        for (i = 0; i < lim && o + 4 < outsz; i++) {
            int n = snprintf(out + o, outsz - o, "%s%02x",
                             i > 0 ? " " : "", (unsigned char)src[i]);
            if (n > 0)
                o += (size_t)n;
        }
        if (len > lim && o + 4 < outsz)
            snprintf(out + o, outsz - o, " ...");
        else
            out[o] = '\0';
        return;
    }

    for (i = 0; i < len && o + 1 < outsz && o < max_chars; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\n' || c == '\r') {
            if (o + 2 < outsz) {
                out[o++] = '\\';
                out[o++] = 'n';
            }
        } else if (c == '\t') {
            out[o++] = ' ';
        } else if (c >= 0x20 && c < 0x7f) {
            out[o++] = (char)c;
        } else {
            out[o++] = '.';
        }
    }
    if (i < len && o + 4 < outsz)
        snprintf(out + o, outsz - o, "...");
    else
        out[o] = '\0';
}

static void resolve_fs_path(const char *spec, char *out, size_t outsz)
{
    char joined[MAX_PATH_LEN];
    char clean[MAX_PATH_LEN];
    char seg[MAX_PATH_LEN];
    const char *p;
    size_t len = 1;

    if (!spec || !spec[0])
        spec = ".";
    if (spec[0] == '/')
        snprintf(joined, sizeof(joined), "%s", spec);
    else if (g_cwd[0] == '/' && g_cwd[1] == '\0')
        snprintf(joined, sizeof(joined), "/%s", spec);
    else
        snprintf(joined, sizeof(joined), "%s/%s", g_cwd, spec);

    clean[0] = '/';
    clean[1] = '\0';
    p = joined;
    while (*p) {
        size_t n = 0;
        while (*p == '/')
            p++;
        while (*p && *p != '/' && n + 1 < sizeof(seg))
            seg[n++] = *p++;
        seg[n] = '\0';
        if (n == 0)
            break;
        if (strcmp(seg, ".") == 0)
            continue;
        if (strcmp(seg, "..") == 0) {
            while (len > 1 && clean[len - 1] != '/')
                len--;
            if (len > 1)
                len--;
            clean[len] = '\0';
            continue;
        }
        if (len > 1 && len + 1 < sizeof(clean))
            clean[len++] = '/';
        for (size_t i = 0; seg[i] && len + 1 < sizeof(clean); i++)
            clean[len++] = seg[i];
        clean[len] = '\0';
    }
    snprintf(out, outsz, "%s", clean);
}

static void format_win_flags(uint32_t state, char *out, size_t outsz)
{
    int first = 1;
    size_t o = 0;

    out[0] = '\0';
    if (state & K_WIN_STATE_FOCUSED) {
        o += (size_t)snprintf(out + o, outsz - o, "focused");
        first = 0;
    }
    if (state & K_WIN_STATE_MINIMIZED) {
        o += (size_t)snprintf(out + o, outsz - o, "%sminimized",
                              first ? "" : ",");
        first = 0;
    }
    if (state & K_WIN_STATE_MAXIMIZED) {
        o += (size_t)snprintf(out + o, outsz - o, "%smaximized",
                              first ? "" : ",");
        first = 0;
    }
    if (state & K_WIN_STATE_PINNED) {
        o += (size_t)snprintf(out + o, outsz - o, "%spinned",
                              first ? "" : ",");
        first = 0;
    }
    if (first)
        snprintf(out, outsz, "normal");
}

static int mask_prefix_len(uint32_t mask_be)
{
    uint32_t m = ((mask_be & 0xFFU) << 24) | ((mask_be & 0xFF00U) << 8) |
                 ((mask_be & 0xFF0000U) >> 8) |
                 ((mask_be & 0xFF000000U) >> 24);
    int bits = 0;

    while (m & 0x80000000U) {
        bits++;
        m <<= 1;
    }
    return m ? -1 : bits;
}

static void format_krec_schema(const struct krec_schema *s, char *out,
                               size_t outsz)
{
    size_t o = 0;
    int i;

    if (!s || s->ncols <= 0 || outsz == 0) {
        if (outsz > 0)
            out[0] = '\0';
        return;
    }
    out[0] = '\0';
    for (i = 0; i < s->ncols && o + 4 < outsz; i++) {
        int n = snprintf(out + o, outsz - o, "%s%s:%s",
                         i > 0 ? " " : "", s->cols[i].name,
                         krec_type_name(s->cols[i].type));
        if (n <= 0)
            break;
        if (o + (size_t)n >= outsz) {
            o = outsz - 1;
            break;
        }
        o += (size_t)n;
    }
}

/* ---- inspectors ------------------------------------------------------ */

static int inspect_procs(int rflag, int has_pid, int target_pid, int limit)
{
    static const struct krec_col cols[] = {
        { "pid",   KREC_INT },
        { "ppid",  KREC_INT },
        { "uid",   KREC_INT },
        { "state", KREC_STR },
        { "name",  KREC_STR },
    };
    struct k_psinfo pi;
    int i, shown = 0;

    if (rflag) {
        if (emit_krec_begin(cols, 5) < 0)
            return 1;
        for (i = 0; psinfo(i, &pi) == 0; i++) {
            char pid_s[16], ppid_s[16], uid_s[16];
            pi.name[sizeof(pi.name) - 1] = '\0';
            if (has_pid && pi.pid != target_pid)
                continue;
            if (limit >= 0 && shown >= limit)
                break;
            snprintf(pid_s, sizeof(pid_s), "%d", pi.pid);
            snprintf(ppid_s, sizeof(ppid_s), "%d", pi.ppid);
            snprintf(uid_s, sizeof(uid_s), "%u", pi.uid);
            emit_krec_row(5, pid_s, ppid_s, uid_s,
                          proc_state_name(pi.state), pi.name);
            shown++;
        }
        if (has_pid && shown == 0) {
            krec_err("inspect: pid %d not found\n", target_pid);
            return 1;
        }
        return 0;
    }

    if (has_pid) {
        for (i = 0; psinfo(i, &pi) == 0; i++) {
            if (pi.pid == target_pid) {
                char buf[64];
                int nwin = 0;
                struct k_winsummary ws;
                pi.name[sizeof(pi.name) - 1] = '\0';
                card_header("process");
                snprintf(buf, sizeof(buf), "%d", pi.pid);
                card_kv("PID", buf);
                snprintf(buf, sizeof(buf), "%d", pi.ppid);
                card_kv("PPID", buf);
                snprintf(buf, sizeof(buf), "%u", pi.uid);
                card_kv("UID", buf);
                card_kv("STATE", proc_state_name(pi.state));
                card_kv("NAME", pi.name);
                for (int wi = 0; winlist_at(wi, &ws) == 0; wi++) {
                    if (ws.pid == pi.pid)
                        nwin++;
                }
                if (nwin > 0) {
                    snprintf(buf, sizeof(buf), "%d", nwin);
                    card_kv("WINDOWS", buf);
                }
                return 0;
            }
        }
        krec_err("inspect: pid %d not found\n", target_pid);
        return 1;
    }

    out_printf("%5s  %5s  %4s  %-9s  %s\n",
               "PID", "PPID", "UID", "STATE", "NAME");
    for (i = 0; psinfo(i, &pi) == 0; i++) {
        if (limit >= 0 && shown >= limit)
            break;
        pi.name[sizeof(pi.name) - 1] = '\0';
        out_printf("%5d  %5d  %4u  %-9s  %s\n",
                   pi.pid, pi.ppid, pi.uid,
                   proc_state_name(pi.state), pi.name);
        shown++;
    }
    return 0;
}

static int inspect_wins(int rflag, int has_wid, uint32_t target_wid, int limit)
{
    static const struct krec_col cols[] = {
        { "wid",       KREC_INT  },
        { "pid",       KREC_INT  },
        { "ws",        KREC_INT  },
        { "x",         KREC_INT  },
        { "y",         KREC_INT  },
        { "width",     KREC_INT  },
        { "height",    KREC_INT  },
        { "focused",   KREC_BOOL },
        { "minimized", KREC_BOOL },
        { "pinned",    KREC_BOOL },
        { "maximized", KREC_BOOL },
        { "title",     KREC_STR  },
    };
    struct k_winsummary ws;
    int i, shown = 0;

    if (rflag) {
        if (emit_krec_begin(cols, 12) < 0)
            return 1;
        for (i = 0; winlist_at(i, &ws) == 0; i++) {
            char wid_s[16], pid_s[16], ws_s[8], x_s[16], y_s[16];
            char w_s[16], h_s[16];
            int wspace = (int)((ws.state >> K_WIN_STATE_WS_SHIFT) &
                               K_WIN_STATE_WS_MASK) + 1;
            ws.title[sizeof(ws.title) - 1] = '\0';
            if (has_wid && ws.wid != target_wid)
                continue;
            if (limit >= 0 && shown >= limit)
                break;
            snprintf(wid_s, sizeof(wid_s), "%u", ws.wid);
            snprintf(pid_s, sizeof(pid_s), "%d", ws.pid);
            snprintf(ws_s, sizeof(ws_s), "%d", wspace);
            snprintf(x_s, sizeof(x_s), "%d", ws.x);
            snprintf(y_s, sizeof(y_s), "%d", ws.y);
            snprintf(w_s, sizeof(w_s), "%u", ws.width);
            snprintf(h_s, sizeof(h_s), "%u", ws.height);
            emit_krec_row(12, wid_s, pid_s, ws_s, x_s, y_s, w_s, h_s,
                          (ws.state & K_WIN_STATE_FOCUSED) ? "true" : "false",
                          (ws.state & K_WIN_STATE_MINIMIZED) ? "true" : "false",
                          (ws.state & K_WIN_STATE_PINNED) ? "true" : "false",
                          (ws.state & K_WIN_STATE_MAXIMIZED) ? "true" : "false",
                          ws.title);
            shown++;
        }
        if (has_wid && shown == 0) {
            krec_err("inspect: window %u not found or inaccessible\n",
                     target_wid);
            return 1;
        }
        return 0;
    }

    if (has_wid) {
        for (i = 0; winlist_at(i, &ws) == 0; i++) {
            if (ws.wid == target_wid) {
                char buf[96], flags_s[64];
                int wspace = (int)((ws.state >> K_WIN_STATE_WS_SHIFT) &
                                   K_WIN_STATE_WS_MASK) + 1;
                int act_ws = (int)((ws.state >> K_WIN_STATE_ACTIVE_WS_SHIFT) &
                                   K_WIN_STATE_WS_MASK) + 1;
                ws.title[sizeof(ws.title) - 1] = '\0';
                format_win_flags(ws.state, flags_s, sizeof(flags_s));
                card_header("window");
                snprintf(buf, sizeof(buf), "%u", ws.wid);
                card_kv("WID", buf);
                snprintf(buf, sizeof(buf), "%d", ws.pid);
                card_kv("PID", buf);
                card_kv("TITLE", ws.title);
                snprintf(buf, sizeof(buf), "%d (active: %d)", wspace, act_ws);
                card_kv("WORKSPACE", buf);
                snprintf(buf, sizeof(buf), "%ux%u+%d+%d",
                         ws.width, ws.height, ws.x, ws.y);
                card_kv("GEOMETRY", buf);
                card_kv("FLAGS", flags_s);
                snprintf(buf, sizeof(buf), "0x%x", ws.flags);
                card_kv("WIN_FLAGS", buf);
                snprintf(buf, sizeof(buf), "0x%08x", ws.state);
                card_kv("STATE_HEX", buf);
                return 0;
            }
        }
        krec_err("inspect: window %u not found or inaccessible\n", target_wid);
        return 1;
    }

    out_printf("%-4s %-5s %-3s %-18s %-22s %s\n",
               "WID", "PID", "WS", "GEOMETRY", "FLAGS", "TITLE");
    for (i = 0; winlist_at(i, &ws) == 0; i++) {
        char geom[24], flags_s[64];
        int wspace = (int)((ws.state >> K_WIN_STATE_WS_SHIFT) &
                           K_WIN_STATE_WS_MASK) + 1;
        if (limit >= 0 && shown >= limit)
            break;
        ws.title[sizeof(ws.title) - 1] = '\0';
        snprintf(geom, sizeof(geom), "%ux%u+%d+%d",
                 ws.width, ws.height, ws.x, ws.y);
        format_win_flags(ws.state, flags_s, sizeof(flags_s));
        out_printf("%-4u %-5d %-3d %-18s %-22s %s\n",
                   ws.wid, ws.pid, wspace, geom, flags_s, ws.title);
        shown++;
    }
    if (shown == 0)
        out_printf("(no accessible windows; compositor restricts SYS_WIN_LIST to desktop shell or none open)\n");
    return 0;
}

static void inspect_parcel_preview(uint32_t slot_or_id,
                                   const struct k_parcel_info *info,
                                   char *preview, size_t prevsz,
                                   char *schema_out, size_t schemasz)
{
    char raw[512];
    uint32_t want;
    long got;

    preview[0] = '\0';
    if (schemasz > 0)
        schema_out[0] = '\0';

    if (info->size == 0) {
        snprintf(preview, prevsz, "(empty)");
        return;
    }
    want = info->size < sizeof(raw) - 1 ? info->size : (uint32_t)(sizeof(raw) - 1);
    got = parcel_get(slot_or_id, 0, raw, want);
    if (got <= 0) {
        snprintf(preview, prevsz, "(unreadable)");
        return;
    }
    raw[got] = '\0';
    sanitize_preview(raw, (size_t)got, preview, prevsz, 96);

    if (schemasz > 0 && (info->type == K_PARCEL_KREC ||
                         (got >= 6 && strncmp(raw, "@krec", 5) == 0))) {
        char line[512];
        struct krec_schema sc;
        size_t l = 0;
        while (l < (size_t)got && l + 1 < sizeof(line) &&
               raw[l] != '\n' && raw[l] != '\r') {
            line[l] = raw[l];
            l++;
        }
        line[l] = '\0';
        if (krec_parse_header(line, &sc) == 0)
            format_krec_schema(&sc, schema_out, schemasz);
    }
}

static int inspect_parcels(int rflag, int has_id, uint32_t target_id, int limit)
{
    static const struct krec_col cols[] = {
        { "slot",    KREC_INT },
        { "id",      KREC_INT },
        { "type",    KREC_STR },
        { "size",    KREC_U64 },
        { "time",    KREC_U64 },
        { "src_pid", KREC_INT },
        { "mime",    KREC_STR },
        { "label",   KREC_STR },
        { "preview", KREC_STR },
    };
    struct k_parcel_info info;
    char preview[128], schema_str[256];
    int i, shown = 0;

    if (has_id) {
        long check = parcel_get(target_id, &info, 0, 0);
        if (check < 0) {
            if (rflag)
                emit_krec_begin(cols, 9);
            krec_err("inspect: parcel %u not found\n", target_id);
            return 1;
        }
        info.mime[sizeof(info.mime) - 1] = '\0';
        info.label[sizeof(info.label) - 1] = '\0';
        inspect_parcel_preview(info.id, &info, preview, sizeof(preview),
                               schema_str, sizeof(schema_str));
        if (rflag) {
            char slot_s[16], id_s[16], size_s[24], time_s[24], pid_s[16];
            int found_slot = -1;
            struct k_parcel_info tmp;
            for (int s = 0; s < K_PARCEL_MAX_SLOTS; s++) {
                if (parcel_list(s, &tmp) == 0 && tmp.id == info.id) {
                    found_slot = s;
                    break;
                }
            }
            if (emit_krec_begin(cols, 9) < 0)
                return 1;
            snprintf(slot_s, sizeof(slot_s), "%d",
                     found_slot >= 0 ? found_slot : 0);
            snprintf(id_s, sizeof(id_s), "%u", info.id);
            snprintf(size_s, sizeof(size_s), "%u", info.size);
            snprintf(time_s, sizeof(time_s), "%u", info.time);
            snprintf(pid_s, sizeof(pid_s), "%d", info.src_pid);
            emit_krec_row(9, slot_s, id_s, parcel_type_name(info.type), size_s,
                          time_s, pid_s, info.mime, info.label, preview);
            return 0;
        }
        {
            char buf[64];
            card_header("parcel");
            snprintf(buf, sizeof(buf), "%u", info.id);
            card_kv("ID", buf);
            card_kv("TYPE", parcel_type_name(info.type));
            snprintf(buf, sizeof(buf), "%u bytes", info.size);
            card_kv("SIZE", buf);
            fmt_time_utc(info.time, buf, sizeof(buf));
            card_kv("TIME", buf);
            snprintf(buf, sizeof(buf), "%d", info.src_pid);
            card_kv("SRC_PID", buf);
            card_kv("MIME", info.mime[0] ? info.mime : "-");
            card_kv("LABEL", info.label[0] ? info.label : "-");
            if (schema_str[0])
                card_kv("SCHEMA", schema_str);
            card_kv("PREVIEW", preview);
        }
        return 0;
    }

    if (rflag) {
        if (emit_krec_begin(cols, 9) < 0)
            return 1;
        for (i = 0; i < K_PARCEL_MAX_SLOTS; i++) {
            char slot_s[16], id_s[16], size_s[24], time_s[24], pid_s[16];
            if (parcel_list(i, &info) != 0)
                continue;
            if (limit >= 0 && shown >= limit)
                break;
            info.mime[sizeof(info.mime) - 1] = '\0';
            info.label[sizeof(info.label) - 1] = '\0';
            inspect_parcel_preview(info.id, &info, preview, sizeof(preview),
                                   schema_str, sizeof(schema_str));
            snprintf(slot_s, sizeof(slot_s), "%d", i);
            snprintf(id_s, sizeof(id_s), "%u", info.id);
            snprintf(size_s, sizeof(size_s), "%u", info.size);
            snprintf(time_s, sizeof(time_s), "%u", info.time);
            snprintf(pid_s, sizeof(pid_s), "%d", info.src_pid);
            emit_krec_row(9, slot_s, id_s, parcel_type_name(info.type),
                          size_s, time_s, pid_s, info.mime, info.label,
                          preview);
            shown++;
        }
        return 0;
    }

    out_printf("%-4s %-5s %-7s %7s %5s  %-16s %-18s %s\n",
               "SLOT", "ID", "TYPE", "SIZE", "PID", "MIME", "LABEL", "PREVIEW");
    for (i = 0; i < K_PARCEL_MAX_SLOTS; i++) {
        if (parcel_list(i, &info) != 0)
            continue;
        if (limit >= 0 && shown >= limit)
            break;
        info.mime[sizeof(info.mime) - 1] = '\0';
        info.label[sizeof(info.label) - 1] = '\0';
        inspect_parcel_preview(info.id, &info, preview, sizeof(preview),
                               schema_str, sizeof(schema_str));
        out_printf("%-4d %-5u %-7s %7u %5d  %-16s %-18s %s\n",
                   i, info.id, parcel_type_name(info.type), info.size,
                   info.src_pid,
                   info.mime[0] ? info.mime : "-",
                   info.label[0] ? info.label : "-",
                   preview);
        shown++;
    }
    if (shown == 0)
        out_printf("(parcel ring is empty)\n");
    return 0;
}

static int inspect_net(int rflag)
{
    static const struct krec_col cols[] = {
        { "iface",     KREC_STR  },
        { "up",        KREC_BOOL },
        { "mac",       KREC_STR  },
        { "ip",        KREC_IP   },
        { "netmask",   KREC_IP   },
        { "prefix",    KREC_INT  },
        { "gateway",   KREC_IP   },
        { "dns",       KREC_IP   },
        { "network",   KREC_IP   },
        { "broadcast", KREC_IP   },
    };
    struct k_netinfo ni;
    char mac[24], ip[16], mask[16], gw[16], dns[16], net_s[16], bcast[16];
    char pfx_s[16];
    uint32_t net_ip, bcast_ip;
    int pfx;

    if (netinfo(&ni) != 0) {
        if (rflag)
            emit_krec_begin(cols, 10);
        krec_err("inspect: no network interface available\n");
        return 1;
    }

    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             ni.mac[0], ni.mac[1], ni.mac[2],
             ni.mac[3], ni.mac[4], ni.mac[5]);
    ip_ntoa(ni.ip, ip);
    ip_ntoa(ni.netmask, mask);
    ip_ntoa(ni.gateway, gw);
    ip_ntoa(ni.dns, dns);
    net_ip = ni.ip & ni.netmask;
    bcast_ip = net_ip | ~ni.netmask;
    ip_ntoa(net_ip, net_s);
    ip_ntoa(bcast_ip, bcast);
    pfx = mask_prefix_len(ni.netmask);
    snprintf(pfx_s, sizeof(pfx_s), "%d", pfx);

    if (rflag) {
        if (emit_krec_begin(cols, 10) < 0)
            return 1;
        emit_krec_row(10, "eth0", ni.up ? "true" : "false", mac, ip, mask,
                      pfx_s, gw, dns, net_s, bcast);
        return 0;
    }

    card_header("network");
    card_kv("IFACE", "eth0");
    card_kv("STATE", ni.up ? "up" : "down");
    card_kv("MAC", mac);
    card_kv("IP", ip);
    card_kv("NETMASK", mask);
    if (pfx >= 0) {
        snprintf(pfx_s, sizeof(pfx_s), "/%d", pfx);
        card_kv("PREFIX", pfx_s);
    } else {
        card_kv("PREFIX", "non-contiguous");
    }
    card_kv("GATEWAY", ni.gateway ? gw : "(unset)");
    card_kv("DNS", ni.dns ? dns : "(unset)");
    card_kv("NETWORK", net_s);
    card_kv("BROADCAST", bcast);
    return 0;
}

static int inspect_fs(const char *spec, int rflag)
{
    static const struct krec_col cols[] = {
        { "path",   KREC_STR },
        { "kind",   KREC_STR },
        { "mode",   KREC_STR },
        { "uid",    KREC_INT },
        { "gid",    KREC_INT },
        { "size",   KREC_U64 },
        { "mtime",  KREC_U64 },
        { "detail", KREC_STR },
    };
    char path[MAX_PATH_LEN];
    struct k_stat st;
    char mode_rwx[10], mode_full[32], uid_s[16], gid_s[16], size_s[24];
    char mtime_s[32], detail[256], schema_str[256];

    resolve_fs_path(spec, path, sizeof(path));
    if (stat_(path, &st) != 0) {
        if (rflag)
            emit_krec_begin(cols, 8);
        krec_err("inspect: cannot stat %s\n", path);
        return 1;
    }

    fmt_mode(st.mode, mode_rwx);
    snprintf(mode_full, sizeof(mode_full), "0%03o (%s)", st.mode & 0777U,
             mode_rwx);
    snprintf(uid_s, sizeof(uid_s), "%u", st.uid);
    snprintf(gid_s, sizeof(gid_s), "%u", st.gid);
    snprintf(size_s, sizeof(size_s), "%u", st.size);
    detail[0] = '\0';
    schema_str[0] = '\0';

    if (st.is_dir) {
        struct k_dirent de;
        int idx = 0, count = 0;
        size_t o = 0;
        char sample[160];

        sample[0] = '\0';
        while (readdir_at(path, idx++, &de) == 0) {
            de.name[sizeof(de.name) - 1] = '\0';
            if (strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
                continue;
            if (count < 6 && o + 4 < sizeof(sample)) {
                int n = snprintf(sample + o, sizeof(sample) - o, "%s%s%s",
                                 count > 0 ? ", " : "", de.name,
                                 de.is_dir ? "/" : "");
                if (n > 0)
                    o += (size_t)n;
            }
            count++;
        }
        snprintf(detail, sizeof(detail), "%d entries%s%s",
                 count, sample[0] ? ": " : "", sample);
    } else {
        char buf[FS_PREVIEW_MAX];
        int fd = open(path, O_RDONLY);
        long n = 0;

        if (fd >= 0) {
            n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n < 0)
                n = 0;
            buf[n] = '\0';
        }
        if (n >= 6 && strncmp(buf, "@krec", 5) == 0 &&
            (buf[5] == ' ' || buf[5] == '\t')) {
            char line[512];
            struct krec_schema sc;
            long l = 0;
            while (l < n && l + 1 < (long)sizeof(line) &&
                   buf[l] != '\n' && buf[l] != '\r') {
                line[l] = buf[l];
                l++;
            }
            line[l] = '\0';
            if (krec_parse_header(line, &sc) == 0)
                format_krec_schema(&sc, schema_str, sizeof(schema_str));
        }
        if (n > 0)
            sanitize_preview(buf, (size_t)n, detail, sizeof(detail), 140);
        else
            snprintf(detail, sizeof(detail), "(empty)");
    }

    if (rflag) {
        char oct_s[16], mt_raw[24];
        if (emit_krec_begin(cols, 8) < 0)
            return 1;
        snprintf(oct_s, sizeof(oct_s), "0%03o", st.mode & 0777U);
        snprintf(mt_raw, sizeof(mt_raw), "%u", st.mtime);
        emit_krec_row(8, path, st.is_dir ? "dir" : "file", oct_s, uid_s,
                      gid_s, size_s, mt_raw,
                      schema_str[0] ? schema_str : detail);
        return 0;
    }

    card_header("fs");
    card_kv("PATH", path);
    card_kv("TYPE", st.is_dir ? "directory" : "file");
    card_kv("MODE", mode_full);
    card_kv("UID", uid_s);
    card_kv("GID", gid_s);
    snprintf(size_s, sizeof(size_s), "%u bytes", st.size);
    card_kv("SIZE", size_s);
    fmt_time_utc(st.mtime, mtime_s, sizeof(mtime_s));
    card_kv("MTIME", mtime_s);
    if (st.is_dir) {
        card_kv("ENTRIES", detail);
    } else {
        if (schema_str[0])
            card_kv("KREC_SCHEMA", schema_str);
        card_kv("PREVIEW", detail);
    }
    return 0;
}

static int inspect_mods(int rflag, int limit)
{
    static const struct krec_col cols[] = {
        { "name",  KREC_STR },
        { "size",  KREC_U64 },
        { "refs",  KREC_INT },
        { "state", KREC_STR },
        { "desc",  KREC_STR },
    };
    struct k_modinfo mi;
    int i, shown = 0;

    if (rflag) {
        if (emit_krec_begin(cols, 5) < 0)
            return 1;
        for (i = 0; modlist_at(i, &mi) == 0; i++) {
            char size_s[24], refs_s[16];
            if (limit >= 0 && shown >= limit)
                break;
            mi.name[sizeof(mi.name) - 1] = '\0';
            mi.desc[sizeof(mi.desc) - 1] = '\0';
            snprintf(size_s, sizeof(size_s), "%u", mi.size);
            snprintf(refs_s, sizeof(refs_s), "%u", mi.refs);
            emit_krec_row(5, mi.name, size_s, refs_s,
                          mod_state_name(mi.state), mi.desc);
            shown++;
        }
        return 0;
    }

    out_printf("%-16s %8s %5s  %-10s %s\n",
               "NAME", "SIZE", "REFS", "STATE", "DESCRIPTION");
    for (i = 0; modlist_at(i, &mi) == 0; i++) {
        if (limit >= 0 && shown >= limit)
            break;
        mi.name[sizeof(mi.name) - 1] = '\0';
        mi.desc[sizeof(mi.desc) - 1] = '\0';
        out_printf("%-16s %8u %5u  %-10s %s\n",
                   mi.name, mi.size, mi.refs,
                   mod_state_name(mi.state), mi.desc);
        shown++;
    }
    if (shown == 0)
        out_printf("(no modules loaded)\n");
    return 0;
}

static int inspect_devs(int rflag, int limit)
{
    static const struct krec_col cols[] = {
        { "bus",    KREC_STR  },
        { "name",   KREC_STR  },
        { "driver", KREC_STR  },
        { "bound",  KREC_BOOL },
        { "vendor", KREC_HEX  },
        { "device", KREC_HEX  },
        { "class",  KREC_STR  },
    };
    struct k_devinfo di;
    int i, shown = 0;

    if (rflag) {
        if (emit_krec_begin(cols, 7) < 0)
            return 1;
        for (i = 0; devlist_at(i, &di) == 0; i++) {
            char ven_s[16], dev_s[16], cls_s[24];
            const char *cn = pci_class_name(di.class_id);
            if (limit >= 0 && shown >= limit)
                break;
            di.bus[sizeof(di.bus) - 1] = '\0';
            di.name[sizeof(di.name) - 1] = '\0';
            di.driver[sizeof(di.driver) - 1] = '\0';
            snprintf(ven_s, sizeof(ven_s), "0x%04x", di.vendor);
            snprintf(dev_s, sizeof(dev_s), "0x%04x", di.device);
            if (cn)
                snprintf(cls_s, sizeof(cls_s), "%s", cn);
            else if (di.class_id)
                snprintf(cls_s, sizeof(cls_s), "0x%04x", di.class_id);
            else
                snprintf(cls_s, sizeof(cls_s), "-");
            emit_krec_row(7, di.bus, di.name, di.bound ? di.driver : "-",
                          di.bound ? "true" : "false", ven_s, dev_s, cls_s);
            shown++;
        }
        return 0;
    }

    out_printf("%-10s %-16s %-12s %-11s %s\n",
               "BUS", "NAME", "DRIVER", "PCI_ID", "CLASS");
    for (i = 0; devlist_at(i, &di) == 0; i++) {
        char id_s[16], cls_s[24];
        const char *cn = pci_class_name(di.class_id);
        if (limit >= 0 && shown >= limit)
            break;
        di.bus[sizeof(di.bus) - 1] = '\0';
        di.name[sizeof(di.name) - 1] = '\0';
        di.driver[sizeof(di.driver) - 1] = '\0';
        if (di.vendor || di.device)
            snprintf(id_s, sizeof(id_s), "%04x:%04x", di.vendor, di.device);
        else
            snprintf(id_s, sizeof(id_s), "-");
        if (cn)
            snprintf(cls_s, sizeof(cls_s), "%s", cn);
        else if (di.class_id)
            snprintf(cls_s, sizeof(cls_s), "0x%04x", di.class_id);
        else
            snprintf(cls_s, sizeof(cls_s), "-");
        out_printf("%-10s %-16s %-12s %-11s %s\n",
                   di.bus, di.name, di.bound ? di.driver : "-", id_s, cls_s);
        shown++;
    }
    if (shown == 0)
        out_printf("(no devices)\n");
    return 0;
}

static int inspect_mounts(int rflag, int limit)
{
    static const struct krec_col cols[] = {
        { "path",     KREC_STR },
        { "fstype",   KREC_STR },
        { "device",   KREC_STR },
        { "total_kb", KREC_U64 },
        { "used_kb",  KREC_U64 },
        { "free_kb",  KREC_U64 },
    };
    struct k_mountinfo mi;
    int i, shown = 0;

    if (rflag) {
        if (emit_krec_begin(cols, 6) < 0)
            return 1;
        for (i = 0; mountlist_at(i, &mi) == 0; i++) {
            char tot_s[24], used_s[24], free_s[24];
            unsigned long long tot_kb = 0, free_kb = 0, used_kb = 0;
            if (limit >= 0 && shown >= limit)
                break;
            mi.path[sizeof(mi.path) - 1] = '\0';
            mi.fstype[sizeof(mi.fstype) - 1] = '\0';
            mi.device[sizeof(mi.device) - 1] = '\0';
            if (mi.block_size && mi.blocks) {
                tot_kb = (mi.blocks * mi.block_size) / 1024ULL;
                free_kb = (mi.free_blocks * mi.block_size) / 1024ULL;
                used_kb = tot_kb - free_kb;
            }
            snprintf(tot_s, sizeof(tot_s), "%llu", tot_kb);
            snprintf(used_s, sizeof(used_s), "%llu", used_kb);
            snprintf(free_s, sizeof(free_s), "%llu", free_kb);
            emit_krec_row(6, mi.path, mi.fstype,
                          mi.device[0] ? mi.device : mi.fstype,
                          tot_s, used_s, free_s);
            shown++;
        }
        return 0;
    }

    out_printf("%-16s %-8s %-10s %10s %10s %10s\n",
               "MOUNT", "TYPE", "DEVICE", "TOTAL_KB", "USED_KB", "FREE_KB");
    for (i = 0; mountlist_at(i, &mi) == 0; i++) {
        if (limit >= 0 && shown >= limit)
            break;
        mi.path[sizeof(mi.path) - 1] = '\0';
        mi.fstype[sizeof(mi.fstype) - 1] = '\0';
        mi.device[sizeof(mi.device) - 1] = '\0';
        if (mi.block_size && mi.blocks) {
            unsigned long long tot_kb = (mi.blocks * mi.block_size) / 1024ULL;
            unsigned long long free_kb =
                (mi.free_blocks * mi.block_size) / 1024ULL;
            unsigned long long used_kb = tot_kb - free_kb;
            out_printf("%-16s %-8s %-10s %10llu %10llu %10llu\n",
                       mi.path, mi.fstype,
                       mi.device[0] ? mi.device : mi.fstype,
                       tot_kb, used_kb, free_kb);
        } else {
            out_printf("%-16s %-8s %-10s %10s %10s %10s\n",
                       mi.path, mi.fstype,
                       mi.device[0] ? mi.device : mi.fstype,
                       "-", "-", "-");
        }
        shown++;
    }
    if (shown == 0)
        out_printf("(no mounts)\n");
    return 0;
}

static int inspect_blks(int rflag, int limit)
{
    static const struct krec_col cols[] = {
        { "name",       KREC_STR },
        { "block_size", KREC_U64 },
        { "blocks",     KREC_U64 },
        { "bytes",      KREC_U64 },
    };
    struct k_blkinfo bi;
    int i, shown = 0;

    if (rflag) {
        if (emit_krec_begin(cols, 4) < 0)
            return 1;
        for (i = 0; blklist_at(i, &bi) == 0; i++) {
            char bs_s[24], nb_s[24], by_s[24];
            if (limit >= 0 && shown >= limit)
                break;
            bi.name[sizeof(bi.name) - 1] = '\0';
            snprintf(bs_s, sizeof(bs_s), "%u", bi.block_size);
            snprintf(nb_s, sizeof(nb_s), "%llu",
                     (unsigned long long)bi.blocks);
            snprintf(by_s, sizeof(by_s), "%llu",
                     (unsigned long long)(bi.blocks * bi.block_size));
            emit_krec_row(4, bi.name, bs_s, nb_s, by_s);
            shown++;
        }
        return 0;
    }

    out_printf("%-10s %8s %12s %12s\n", "NAME", "BLKSZ", "BLOCKS", "SIZE_KB");
    for (i = 0; blklist_at(i, &bi) == 0; i++) {
        if (limit >= 0 && shown >= limit)
            break;
        bi.name[sizeof(bi.name) - 1] = '\0';
        out_printf("%-10s %8u %12llu %12llu\n",
                   bi.name, bi.block_size, (unsigned long long)bi.blocks,
                   (unsigned long long)((bi.blocks * bi.block_size) / 1024ULL));
        shown++;
    }
    if (shown == 0)
        out_printf("(no block devices)\n");
    return 0;
}

static int inspect_pulses(int rflag, int limit)
{
    static const struct krec_col cols[] = {
        { "seq",   KREC_U64 },
        { "topic", KREC_STR },
        { "kind",  KREC_STR },
        { "arg0",  KREC_U64 },
        { "arg1",  KREC_U64 },
        { "tag",   KREC_STR },
    };
    struct k_pulse all[MAX_PULSES];
    struct pulse_cursor cursor = { .topics = K_TOPIC_ALL };
    int total = 0, start = 0, i;

    while (total < MAX_PULSES) {
        int want = MAX_PULSES - total;
        int n;
        if (want > 64)
            want = 64;
        n = pulse_cursor_read(&cursor, &all[total], (uint32_t)want, 0);
        if (n <= 0)
            break;
        total += n;
        if (n < want)
            break;
    }

    if (limit >= 0 && total > limit)
        start = total - limit;

    if (rflag) {
        if (emit_krec_begin(cols, 6) < 0)
            return 1;
        for (i = start; i < total; i++) {
            char seq_s[24], a0_s[24], a1_s[24];
            all[i].tag[sizeof(all[i].tag) - 1] = '\0';
            snprintf(seq_s, sizeof(seq_s), "%llu",
                     (unsigned long long)all[i].seq);
            snprintf(a0_s, sizeof(a0_s), "%u", all[i].arg0);
            snprintf(a1_s, sizeof(a1_s), "%u", all[i].arg1);
            emit_krec_row(6, seq_s, pulse_topic_name(all[i].topic),
                          pulse_kind_name(all[i].kind), a0_s, a1_s,
                          all[i].tag);
        }
        return 0;
    }

    out_printf("%-6s %-7s %-12s %8s %8s  %s\n",
               "SEQ", "TOPIC", "KIND", "ARG0", "ARG1", "TAG");
    for (i = start; i < total; i++) {
        all[i].tag[sizeof(all[i].tag) - 1] = '\0';
        out_printf("%-6llu %-7s %-12s %8u %8u  %s\n",
                   (unsigned long long)all[i].seq,
                   pulse_topic_name(all[i].topic),
                   pulse_kind_name(all[i].kind),
                   all[i].arg0, all[i].arg1,
                   all[i].tag[0] ? all[i].tag : "-");
    }
    if (total - start == 0)
        out_printf("(no pulses in ring; latest seq %llu)\n",
                   (unsigned long long)cursor.seq);
    return 0;
}

static int inspect_sys(int rflag)
{
    static const struct krec_col cols[] = {
        { "uptime_ms",    KREC_U64  },
        { "cpus_online",  KREC_INT  },
        { "cpus_total",   KREC_INT  },
        { "mem_used_kb",  KREC_U64  },
        { "mem_total_kb", KREC_U64  },
        { "swap_used_kb", KREC_U64  },
        { "swap_total_kb",KREC_U64  },
        { "procs",        KREC_INT  },
        { "wins",         KREC_INT  },
        { "parcels",      KREC_INT  },
        { "mods",         KREC_INT  },
        { "devs",         KREC_INT  },
        { "mounts",       KREC_INT  },
        { "net_up",       KREC_BOOL },
        { "ip",           KREC_IP   },
    };
    struct k_cpuinfo cpu;
    struct k_netinfo net;
    struct k_fbinfo fb;
    struct k_psinfo ps;
    struct k_winsummary ws;
    struct k_parcel_info pi;
    struct k_modinfo mi;
    struct k_devinfo di;
    struct k_mountinfo mni;
    struct pulse_cursor cursor = { .topics = K_TOPIC_ALL };
    uint64_t mem_tot = 0, mem_free = 0, mem_used = 0;
    uint64_t sw_tot = 0, sw_used = 0;
    unsigned long up_ms = uptime_ms();
    int nprocs = 0, nwins = 0, nparcels = 0, nmods = 0, ndevs = 0, nmounts = 0;
    int win_restricted = 0, has_net = 0;
    char ip_s[16];

    memset(&cpu, 0, sizeof(cpu));
    cpuinfo(&cpu);
    if (meminfo(&mem_tot, &mem_free) == 0 && mem_tot >= mem_free)
        mem_used = mem_tot - mem_free;
    swapinfo(&sw_tot, &sw_used);

    while (psinfo(nprocs, &ps) == 0)
        nprocs++;
    if (winlist_at(0, &ws) == 0) {
        nwins = 1;
        while (winlist_at(nwins, &ws) == 0)
            nwins++;
    } else {
        win_restricted = 1;
    }
    for (int i = 0; i < K_PARCEL_MAX_SLOTS; i++) {
        if (parcel_list(i, &pi) == 0)
            nparcels++;
    }
    while (modlist_at(nmods, &mi) == 0)
        nmods++;
    while (devlist_at(ndevs, &di) == 0)
        ndevs++;
    while (mountlist_at(nmounts, &mni) == 0)
        nmounts++;

    memset(&net, 0, sizeof(net));
    has_net = (netinfo(&net) == 0);
    if (has_net)
        ip_ntoa(net.ip, ip_s);
    else
        strcpy(ip_s, "0.0.0.0");

    memset(&fb, 0, sizeof(fb));
    fbinfo_get(&fb);

    pulse_cursor_follow(&cursor);

    if (rflag) {
        char up_s[24], co_s[16], ct_s[16], mu_s[24], mt_s[24];
        char su_s[24], st_s[24], np_s[16], nw_s[16], npar_s[16];
        char nm_s[16], nd_s[16], nmn_s[16];
        if (emit_krec_begin(cols, 15) < 0)
            return 1;
        snprintf(up_s, sizeof(up_s), "%lu", up_ms);
        snprintf(co_s, sizeof(co_s), "%u", cpu.online);
        snprintf(ct_s, sizeof(ct_s), "%u", cpu.discovered);
        snprintf(mu_s, sizeof(mu_s), "%llu", (unsigned long long)mem_used);
        snprintf(mt_s, sizeof(mt_s), "%llu", (unsigned long long)mem_tot);
        snprintf(su_s, sizeof(su_s), "%llu", (unsigned long long)sw_used);
        snprintf(st_s, sizeof(st_s), "%llu", (unsigned long long)sw_tot);
        snprintf(np_s, sizeof(np_s), "%d", nprocs);
        snprintf(nw_s, sizeof(nw_s), "%d", nwins);
        snprintf(npar_s, sizeof(npar_s), "%d", nparcels);
        snprintf(nm_s, sizeof(nm_s), "%d", nmods);
        snprintf(nd_s, sizeof(nd_s), "%d", ndevs);
        snprintf(nmn_s, sizeof(nmn_s), "%d", nmounts);
        emit_krec_row(15, up_s, co_s, ct_s, mu_s, mt_s, su_s, st_s,
                      np_s, nw_s, npar_s, nm_s, nd_s, nmn_s,
                      (has_net && net.up) ? "true" : "false", ip_s);
        return 0;
    }

    {
        char buf[96];
        unsigned long secs = up_ms / 1000UL;
        card_header("system");
        snprintf(buf, sizeof(buf), "%luh %02lum %02lus (%lu ms)",
                 secs / 3600UL, (secs / 60UL) % 60UL, secs % 60UL, up_ms);
        card_kv("UPTIME", buf);
        snprintf(buf, sizeof(buf), "%u online / %u discovered (apic %u)",
                 cpu.online, cpu.discovered, cpu.apic_id);
        card_kv("CPU", buf);
        snprintf(buf, sizeof(buf), "%llu / %llu MiB (%llu / %llu KiB)",
                 (unsigned long long)(mem_used / 1024ULL),
                 (unsigned long long)(mem_tot / 1024ULL),
                 (unsigned long long)mem_used,
                 (unsigned long long)mem_tot);
        card_kv("RAM", buf);
        snprintf(buf, sizeof(buf), "%llu / %llu MiB (%llu / %llu KiB)",
                 (unsigned long long)(sw_used / 1024ULL),
                 (unsigned long long)(sw_tot / 1024ULL),
                 (unsigned long long)sw_used,
                 (unsigned long long)sw_tot);
        card_kv("SWAP", buf);
        if (fb.present)
            snprintf(buf, sizeof(buf), "%ux%u %ubpp", fb.width, fb.height,
                     fb.bpp);
        else
            snprintf(buf, sizeof(buf), "text mode");
        card_kv("DISPLAY", buf);
        if (has_net && net.up)
            snprintf(buf, sizeof(buf), "up (%s)", ip_s);
        else
            snprintf(buf, sizeof(buf), has_net ? "down" : "none");
        card_kv("NETWORK", buf);
        snprintf(buf, sizeof(buf), "%d", nprocs);
        card_kv("PROCESSES", buf);
        if (win_restricted)
            snprintf(buf, sizeof(buf), "restricted (desktop-shell only)");
        else
            snprintf(buf, sizeof(buf), "%d", nwins);
        card_kv("WINDOWS", buf);
        snprintf(buf, sizeof(buf), "%d / %d slots", nparcels,
                 K_PARCEL_MAX_SLOTS);
        card_kv("PARCELS", buf);
        snprintf(buf, sizeof(buf), "%d modules, %d devices, %d mounts",
                 nmods, ndevs, nmounts);
        card_kv("KERNEL", buf);
        snprintf(buf, sizeof(buf), "seq %llu", (unsigned long long)cursor.seq);
        card_kv("PULSES", buf);
        printf("\n");
        printf("usage: inspect [sys|procs|proc:<pid>|wins|win:<wid>|"
               "parcels|parcel:<id>|\n");
        printf("                net|fs:<path>|/<path>|mod|dev|mount|blk|"
               "pulse] [-r] [--pin] [-n N]\n");
    }
    return 0;
}

static void usage(void)
{
    printf("usage: inspect [target] [-r] [--pin] [-n N]\n");
    printf("  targets:\n");
    printf("    sys                  system telemetry overview (default)\n");
    printf("    procs | proc:<pid>   process table or single process\n");
    printf("    wins  | win:<wid>    compositor windows or single window\n");
    printf("    parcels | parcel:<id> Talon clipboard parcels\n");
    printf("    net                  live network interface config\n");
    printf("    fs:<path> | /<path>  VFS file/dir stat, preview & @krec schema\n");
    printf("    mod | dev | mount | blk | pulse\n");
    printf("  flags:\n");
    printf("    -r, --krec           emit @krec typed record stream\n");
    printf("    --pin                push summary/@krec to Talon via parcel_put\n");
    printf("    -n N                 limit list/pulse output to N entries\n");
}

static int parse_u32_strict(const char *s, uint32_t *out)
{
    uint64_t v = 0;

    if (!s || !s[0] || krec_parse_u64(s, &v) < 0 || v > 0xffffffffULL)
        return -1;
    *out = (uint32_t)v;
    return 0;
}

static int parse_i32_strict(const char *s, int *out)
{
    int64_t v = 0;

    if (!s || !s[0] || krec_parse_i64(s, &v) < 0 ||
        v < -2147483648LL || v > 2147483647LL)
        return -1;
    *out = (int)v;
    return 0;
}

int main(int argc, char **argv)
{
    const char *target = 0;
    int rflag = 0, pin_flag = 0, limit = -1;
    enum target_kind kind = TGT_SYS;
    const char *fs_spec = 0;
    int pid_val = 0;
    uint32_t u32_val = 0;
    int i, rc = 0;

    g_cwd = kestrel_startup_cwd();
    if (!g_cwd || !g_cwd[0])
        g_cwd = "/";

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--krec") == 0) {
            rflag = 1;
        } else if (strcmp(argv[i], "--pin") == 0) {
            pin_flag = 1;
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            if (parse_i32_strict(argv[++i], &limit) < 0 || limit < 0) {
                krec_err("inspect: -n requires a non-negative integer\n");
                return 1;
            }
        } else if (strncmp(argv[i], "-n", 2) == 0 && argv[i][2] != '\0') {
            if (parse_i32_strict(argv[i] + 2, &limit) < 0 || limit < 0) {
                krec_err("inspect: -n requires a non-negative integer\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            krec_err("inspect: unknown option '%s'\n", argv[i]);
            return 1;
        } else if (!target) {
            target = argv[i];
        } else {
            krec_err("inspect: unexpected argument '%s'\n", argv[i]);
            return 1;
        }
    }

    if (!target || strcmp(target, "sys") == 0) {
        kind = TGT_SYS;
    } else if (strcmp(target, "procs") == 0 || strcmp(target, "proc") == 0) {
        kind = TGT_PROCS;
    } else if (strncmp(target, "proc:", 5) == 0) {
        if (parse_i32_strict(target + 5, &pid_val) < 0 || pid_val < 0) {
            krec_err("inspect: invalid pid '%s'\n", target + 5);
            return 1;
        }
        kind = TGT_PROC_ONE;
    } else if (strcmp(target, "wins") == 0 || strcmp(target, "win") == 0) {
        kind = TGT_WINS;
    } else if (strncmp(target, "win:", 4) == 0) {
        if (parse_u32_strict(target + 4, &u32_val) < 0) {
            krec_err("inspect: invalid window id '%s'\n", target + 4);
            return 1;
        }
        kind = TGT_WIN_ONE;
    } else if (strcmp(target, "parcels") == 0 ||
               strcmp(target, "parcel") == 0) {
        kind = TGT_PARCELS;
    } else if (strncmp(target, "parcel:", 7) == 0) {
        if (parse_u32_strict(target + 7, &u32_val) < 0) {
            krec_err("inspect: invalid parcel id '%s'\n", target + 7);
            return 1;
        }
        kind = TGT_PARCEL_ONE;
    } else if (strcmp(target, "net") == 0) {
        kind = TGT_NET;
    } else if (strncmp(target, "fs:", 3) == 0) {
        kind = TGT_FS;
        fs_spec = target + 3;
    } else if (target[0] == '/' || strcmp(target, ".") == 0 ||
               strcmp(target, "..") == 0 || strncmp(target, "./", 2) == 0 ||
               strncmp(target, "../", 3) == 0 || strchr(target, '/') != 0) {
        kind = TGT_FS;
        fs_spec = target;
    } else if (strcmp(target, "mod") == 0 || strcmp(target, "mods") == 0) {
        kind = TGT_MOD;
    } else if (strcmp(target, "dev") == 0 || strcmp(target, "devs") == 0) {
        kind = TGT_DEV;
    } else if (strcmp(target, "mount") == 0 ||
               strcmp(target, "mounts") == 0) {
        kind = TGT_MOUNT;
    } else if (strcmp(target, "blk") == 0 || strcmp(target, "blks") == 0) {
        kind = TGT_BLK;
    } else if (strcmp(target, "pulse") == 0 ||
               strcmp(target, "pulses") == 0) {
        kind = TGT_PULSE;
    } else {
        krec_err("inspect: unknown target '%s'\n", target);
        return 1;
    }

    cap_init(pin_flag);

    switch (kind) {
    case TGT_SYS:
        rc = inspect_sys(rflag);
        break;
    case TGT_PROCS:
        rc = inspect_procs(rflag, 0, 0, limit);
        break;
    case TGT_PROC_ONE:
        rc = inspect_procs(rflag, 1, pid_val, -1);
        break;
    case TGT_WINS:
        rc = inspect_wins(rflag, 0, 0, limit);
        break;
    case TGT_WIN_ONE:
        rc = inspect_wins(rflag, 1, u32_val, -1);
        break;
    case TGT_PARCELS:
        rc = inspect_parcels(rflag, 0, 0, limit);
        break;
    case TGT_PARCEL_ONE:
        rc = inspect_parcels(rflag, 1, u32_val, -1);
        break;
    case TGT_NET:
        rc = inspect_net(rflag);
        break;
    case TGT_FS:
        rc = inspect_fs(fs_spec, rflag);
        break;
    case TGT_MOD:
        rc = inspect_mods(rflag, limit);
        break;
    case TGT_DEV:
        rc = inspect_devs(rflag, limit);
        break;
    case TGT_MOUNT:
        rc = inspect_mounts(rflag, limit);
        break;
    case TGT_BLK:
        rc = inspect_blks(rflag, limit);
        break;
    case TGT_PULSE:
        rc = inspect_pulses(rflag, limit);
        break;
    }

    if (pin_flag && rc == 0 && g_cap.enabled && g_cap.len > 0) {
        char label[48];
        uint32_t ptype = rflag ? K_PARCEL_KREC : K_PARCEL_TEXT;
        const char *mime = rflag ? "text/x-krec" : "text/plain";
        int pid;

        snprintf(label, sizeof(label), "inspect:%s",
                 target ? target : "sys");
        pid = parcel_put(ptype, mime, label, g_cap.data, (uint32_t)g_cap.len);
        if (pid < 0) {
            krec_err("inspect: failed to pin to Talon\n");
            rc = 1;
        } else {
            krec_err("inspect: pinned parcel %d (%s, %u bytes%s)\n",
                     pid, parcel_type_name(ptype), (unsigned)g_cap.len,
                     g_cap.truncated ? ", truncated" : "");
        }
    }

    free(g_cap.data);
    return rc;
}
