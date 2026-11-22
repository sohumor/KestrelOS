/* sight.c - Sight: KestrelOS Graphical Universal Live Object Inspector.
 *
 * Interactive Avionics UI counterpart to /bin/inspect:
 *   - Left category rail: Processes, Windows, Talon Parcels, Pulses,
 *     Modules, Devices, Mounts, Network, @krec Stream
 *   - Top live filter bar (and path loader in @krec Stream)
 *   - Center scrollable object table (KEV_MOUSE_WHEEL + scrollbar)
 *   - Right structured Inspector Detail Card with object actions
 *     (Window Focus/Min/Restore/Max/Pin/WS 1..4/Close, Process SIGTERM/SIGKILL,
 *      and Pin to Talon for any selected object)
 *   - Zero-polling live refresh driven by pulse_poll(timeout_ms=0)
 *   - Full KEV_RESIZE responsive layout
 *
 * usage: sight [target]
 *   e.g. sight proc:5 | sight win:2 | sight /tmp/data.krec | sight net
 */

#include <kestrel.h>
#include <kestrel_cwd.h>
#include <kestrel_pulse.h>
#include <krec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"

#define MAX_PROCS     128
#define MAX_WINS      64
#define MAX_PULSES    128
#define MAX_MODS      64
#define MAX_DEVS      128
#define MAX_MOUNTS    32
#define MAX_NET_ROWS  12
#define MAX_KREC_ROWS 256
#define MAX_ITEMS     256
#define MAX_PATH_LEN  256
#define MAX_KV_ROWS   40

enum sight_cat {
    CAT_PROCS = 0,
    CAT_WINS,
    CAT_PARCELS,
    CAT_PULSES,
    CAT_MODS,
    CAT_DEVS,
    CAT_MOUNTS,
    CAT_NET,
    CAT_KREC,
    CAT_COUNT
};

struct net_row {
    char key[24];
    char val[80];
    uint32_t tint;
};

struct krec_row_item {
    char *blob;
    char *fields[KREC_MAX_COLS];
};

struct kv_pair {
    char key[24];
    char val[192];
    uint32_t color;
    int is_color_swatch;
    uint32_t swatch_rgb;
};

static const char *const cat_names[CAT_COUNT] = {
    "Processes",
    "Windows",
    "Talon Parcels",
    "Pulses",
    "Modules",
    "Devices",
    "Mounts",
    "Network",
    "@krec Stream",
};

static const char *const cat_headers[CAT_COUNT] = {
    "PID    PPID  UID   STATE     NAME",
    "WID   PID   WS  GEOM              FLAGS           TITLE",
    "SLOT  ID    TYPE    SIZE     LABEL",
    "SEQ    TOPIC   KIND         ARG0    TAG",
    "NAME             SIZE      REFS  STATE      DESC",
    "BUS       NAME            DRIVER      ID         CLASS",
    "MOUNT            TYPE    DEVICE     TOTAL_KB   FREE_KB",
    "PROPERTY          VALUE",
    "@krec / FS STREAM ROWS",
};

/* ---- state ----------------------------------------------------------- */

static char g_cwd[MAX_PATH_LEN] = "/";
static enum sight_cat cur_cat = CAT_PROCS;

static struct k_psinfo procs[MAX_PROCS];
static int nprocs;

static struct k_winsummary wins[MAX_WINS];
static int nwins;
static int wins_restricted;

static struct k_parcel_info parcels[K_PARCEL_MAX_SLOTS];
static int parcel_slots[K_PARCEL_MAX_SLOTS];
static char parcel_previews[K_PARCEL_MAX_SLOTS][160];
static char parcel_schemas[K_PARCEL_MAX_SLOTS][160];
static int nparcels;

static struct k_pulse pulses[MAX_PULSES];
static int npulses;
static struct pulse_cursor pulse_history = { .topics = K_TOPIC_ALL };

static struct k_modinfo mods[MAX_MODS];
static int nmods;

static struct k_devinfo devs[MAX_DEVS];
static int ndevs;

static struct k_mountinfo mounts[MAX_MOUNTS];
static int nmounts;

static struct k_netinfo net_info;
static int net_ok;
static struct net_row net_rows[MAX_NET_ROWS];
static int nnet_rows;

static struct krec_schema krec_sc;
static struct krec_row_item *krec_rows;
static int nkrec_rows;
static int krec_is_typed;
static char krec_source[MAX_PATH_LEN] = "(none)";
static char krec_fs_detail[256];

static int filtered[MAX_ITEMS];
static int nfiltered;

static gui_list main_list;
static char filter_buf[128];
static gui_textbox_state filter_tb;
static char last_filter[128];

static int detail_scroll;
static int last_sel_for_scroll = -1;
static enum sight_cat last_cat_for_scroll = CAT_COUNT;
static char status_msg[128] = "Live via pulse_poll";
static int need_repaint;
static int dirty = 1;

/* Startup pre-selection targets */
static int want_pid = -1;
static int want_wid = -1;
static int want_parcel = -1;

/* ---- thin syscalls --------------------------------------------------- */

static int modlist_at(int i, struct k_modinfo *out)
{
    return (int)syscall(SYS_MODLIST, i, (long)out, 0, 0);
}

static int devlist_at(int i, struct k_devinfo *out)
{
    return (int)syscall(SYS_DEVLIST, i, (long)out, 0, 0);
}

static int mountlist_at(int i, struct k_mountinfo *out)
{
    return (int)syscall(SYS_MOUNTLIST, i, (long)out, 0, 0);
}

/* ---- name helpers ---------------------------------------------------- */

static const char *proc_state_str(int s)
{
    switch (s) {
    case K_STATE_RUNNABLE: return "runnable";
    case K_STATE_RUNNING:  return "running";
    case K_STATE_SLEEPING: return "sleeping";
    case K_STATE_ZOMBIE:   return "zombie";
    case K_STATE_STOPPED:  return "stopped";
    default:               return "?";
    }
}

static const char *mod_state_str(uint32_t s)
{
    switch (s) {
    case K_MOD_LOADING:   return "loading";
    case K_MOD_LIVE:      return "live";
    case K_MOD_UNLOADING: return "unloading";
    default:              return "?";
    }
}

static const char *parcel_type_str(uint32_t t)
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

static const char *pulse_topic_str(uint32_t t)
{
    switch (t) {
    case K_TOPIC_PROC:   return "proc";
    case K_TOPIC_WIN:    return "win";
    case K_TOPIC_THEME:  return "theme";
    case K_TOPIC_FS:     return "fs";
    case K_TOPIC_NET:    return "net";
    case K_TOPIC_PARCEL: return "parcel";
    default:             return "other";
    }
}

static const char *pulse_kind_str(uint32_t k)
{
    switch (k) {
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
    default:                 return "?";
    }
}

static const char *pci_class_str(uint32_t class_id)
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

static void fmt_win_flags(uint32_t st, char *out, size_t outsz)
{
    size_t o = 0;
    int first = 1;

    out[0] = '\0';
    if (st & K_WIN_STATE_FOCUSED) {
        o += (size_t)snprintf(out + o, outsz - o, "foc");
        first = 0;
    }
    if (st & K_WIN_STATE_MINIMIZED) {
        o += (size_t)snprintf(out + o, outsz - o, "%smin", first ? "" : ",");
        first = 0;
    }
    if (st & K_WIN_STATE_MAXIMIZED) {
        o += (size_t)snprintf(out + o, outsz - o, "%smax", first ? "" : ",");
        first = 0;
    }
    if (st & K_WIN_STATE_PINNED) {
        o += (size_t)snprintf(out + o, outsz - o, "%spin", first ? "" : ",");
        first = 0;
    }
    if (first)
        snprintf(out, outsz, "-");
}

static void sanitize_text(const char *src, size_t len, char *out, size_t outsz)
{
    size_t i, o = 0;

    if (outsz == 0)
        return;
    for (i = 0; i < len && o + 1 < outsz; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\n' || c == '\r' || c == '\t')
            out[o++] = ' ';
        else if (c >= 0x20 && c < 0x7f)
            out[o++] = (char)c;
        else
            out[o++] = '.';
    }
    out[o] = '\0';
}

static int parse_hex_color(const char *s, uint32_t *rgb_out)
{
    uint32_t v = 0;
    size_t len;

    if (!s || s[0] != '#')
        return -1;
    len = strlen(s);
    if (len != 4 && len != 7 && len != 9)
        return -1;
    for (size_t i = 1; i < len; i++) {
        char c = s[i];
        uint32_t d;
        if (c >= '0' && c <= '9')      d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else return -1;
        v = (v << 4) | d;
    }
    if (len == 4) {
        uint32_t r = (v >> 8) & 0xF, g = (v >> 4) & 0xF, b = v & 0xF;
        v = (r * 17U << 16) | (g * 17U << 8) | (b * 17U);
    } else if (len == 9) {
        v >>= 8;
    }
    *rgb_out = v & 0xFFFFFFU;
    return 0;
}

static void resolve_path(const char *spec, char *out, size_t outsz)
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

/* ---- @krec stream loading -------------------------------------------- */

static void free_krec_rows(void)
{
    if (krec_rows) {
        for (int i = 0; i < nkrec_rows; i++)
            free(krec_rows[i].blob);
        free(krec_rows);
    }
    krec_rows = 0;
    nkrec_rows = 0;
    krec_is_typed = 0;
    memset(&krec_sc, 0, sizeof(krec_sc));
}

static void unescape_krec_field(char *s)
{
    char *r = s, *w = s;

    while (*r) {
        if (*r == '\\' && r[1] != '\0') {
            r++;
            switch (*r) {
            case 't':  *w++ = '\t'; break;
            case 'n':  *w++ = '\n'; break;
            case 'r':  *w++ = '\r'; break;
            case '\\': *w++ = '\\'; break;
            default:   *w++ = *r;   break;
            }
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static int parse_krec_text(const char *text, const char *src_label)
{
    const char *p = text;
    char line[KREC_LINE_MAX];
    int first = 1;

    free_krec_rows();
    snprintf(krec_source, sizeof(krec_source), "%s", src_label);
    krec_fs_detail[0] = '\0';

    krec_rows = (struct krec_row_item *)calloc(
        MAX_KREC_ROWS, sizeof(struct krec_row_item));
    if (!krec_rows)
        return -1;

    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        while (len > 0 && p[len - 1] == '\r')
            len--;
        if (len >= sizeof(line))
            len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        p = nl ? nl + 1 : "";

        if (len == 0)
            continue;

        if (first) {
            first = 0;
            if (krec_parse_header(line, &krec_sc) == 0) {
                krec_is_typed = 1;
                continue;
            }
            krec_is_typed = 0;
            krec_sc.ncols = 2;
            snprintf(krec_sc.cols[0].name, KREC_NAME_MAX, "line");
            krec_sc.cols[0].type = KREC_INT;
            snprintf(krec_sc.cols[1].name, KREC_NAME_MAX, "text");
            krec_sc.cols[1].type = KREC_STR;
        }

        if (nkrec_rows >= MAX_KREC_ROWS)
            break;

        if (krec_is_typed) {
            char *blob = strdup(line);
            int nf = 0;
            char *q;
            if (!blob)
                break;
            q = blob;
            krec_rows[nkrec_rows].fields[nf++] = q;
            while (*q) {
                if (*q == '\t') {
                    *q++ = '\0';
                    if (nf < krec_sc.ncols)
                        krec_rows[nkrec_rows].fields[nf++] = q;
                } else {
                    q++;
                }
            }
            while (nf < krec_sc.ncols)
                krec_rows[nkrec_rows].fields[nf++] = "";
            for (int c = 0; c < krec_sc.ncols; c++)
                unescape_krec_field(krec_rows[nkrec_rows].fields[c]);
            krec_rows[nkrec_rows].blob = blob;
            nkrec_rows++;
        } else {
            char *blob = (char *)malloc(len + 24);
            char *txt;
            if (!blob)
                break;
            snprintf(blob, 16, "%d", nkrec_rows + 1);
            txt = blob + 16;
            memcpy(txt, line, len + 1);
            krec_rows[nkrec_rows].blob = blob;
            krec_rows[nkrec_rows].fields[0] = blob;
            krec_rows[nkrec_rows].fields[1] = txt;
            nkrec_rows++;
        }
    }
    return 0;
}

static int load_krec_from_file(const char *raw_path)
{
    char path[MAX_PATH_LEN];
    struct k_stat st;

    resolve_path(raw_path, path, sizeof(path));
    if (stat_(path, &st) != 0) {
        snprintf(status_msg, sizeof(status_msg), "Cannot stat %s", path);
        return -1;
    }

    if (st.is_dir) {
        struct k_dirent de;
        int idx = 0;
        free_krec_rows();
        snprintf(krec_source, sizeof(krec_source), "%s (dir)", path);
        snprintf(krec_fs_detail, sizeof(krec_fs_detail),
                 "dir mode=0%03o uid=%u gid=%u",
                 st.mode & 0777U, st.uid, st.gid);
        krec_is_typed = 1;
        krec_sc.ncols = 4;
        snprintf(krec_sc.cols[0].name, KREC_NAME_MAX, "name");
        krec_sc.cols[0].type = KREC_STR;
        snprintf(krec_sc.cols[1].name, KREC_NAME_MAX, "kind");
        krec_sc.cols[1].type = KREC_STR;
        snprintf(krec_sc.cols[2].name, KREC_NAME_MAX, "size");
        krec_sc.cols[2].type = KREC_U64;
        snprintf(krec_sc.cols[3].name, KREC_NAME_MAX, "mode");
        krec_sc.cols[3].type = KREC_STR;

        krec_rows = (struct krec_row_item *)calloc(
            MAX_KREC_ROWS, sizeof(struct krec_row_item));
        if (!krec_rows)
            return -1;
        while (nkrec_rows < MAX_KREC_ROWS && readdir_at(path, idx++, &de) == 0) {
            char *blob;
            if (strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
                continue;
            de.name[sizeof(de.name) - 1] = '\0';
            blob = (char *)malloc(128);
            if (!blob)
                break;
            snprintf(blob, 64, "%s", de.name);
            snprintf(blob + 64, 16, "%s", de.is_dir ? "dir" : "file");
            snprintf(blob + 80, 24, "%u", de.size);
            snprintf(blob + 104, 16, "0%03o", de.mode & 0777U);
            krec_rows[nkrec_rows].blob = blob;
            krec_rows[nkrec_rows].fields[0] = blob;
            krec_rows[nkrec_rows].fields[1] = blob + 64;
            krec_rows[nkrec_rows].fields[2] = blob + 80;
            krec_rows[nkrec_rows].fields[3] = blob + 104;
            nkrec_rows++;
        }
        snprintf(status_msg, sizeof(status_msg), "Loaded dir %s (%d entries)",
                 path, nkrec_rows);
        return 0;
    }

    {
        uint32_t cap = st.size < 32768U ? st.size : 32768U;
        char *buf = (char *)malloc(cap + 1);
        int fd;
        long got = 0;

        if (!buf)
            return -1;
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            free(buf);
            snprintf(status_msg, sizeof(status_msg), "Cannot open %s", path);
            return -1;
        }
        while ((uint32_t)got < cap) {
            long n = read(fd, buf + got, cap - (uint32_t)got);
            if (n <= 0)
                break;
            got += n;
        }
        close(fd);
        buf[got] = '\0';
        parse_krec_text(buf, path);
        snprintf(krec_fs_detail, sizeof(krec_fs_detail),
                 "file size=%u mode=0%03o uid=%u gid=%u",
                 st.size, st.mode & 0777U, st.uid, st.gid);
        free(buf);
        snprintf(status_msg, sizeof(status_msg), "Loaded %s (%d rows)",
                 path, nkrec_rows);
    }
    return 0;
}

static int load_krec_from_parcel(uint32_t slot_or_id)
{
    struct k_parcel_info info;
    char *buf;
    long n;
    char lbl[64];

    if (parcel_get(slot_or_id, &info, 0, 0) < 0) {
        snprintf(status_msg, sizeof(status_msg), "Parcel %u not found",
                 slot_or_id);
        return -1;
    }
    if (info.size == 0 || info.size > K_PARCEL_MAX_BYTES) {
        snprintf(status_msg, sizeof(status_msg), "Parcel %u empty", info.id);
        return -1;
    }
    buf = (char *)malloc(info.size + 1);
    if (!buf)
        return -1;
    n = parcel_get(info.id, &info, buf, info.size);
    if (n <= 0) {
        free(buf);
        return -1;
    }
    buf[n] = '\0';
    snprintf(lbl, sizeof(lbl), "talon:parcel#%u (%s)",
             info.id, info.label[0] ? info.label : parcel_type_str(info.type));
    parse_krec_text(buf, lbl);
    free(buf);
    snprintf(status_msg, sizeof(status_msg), "Loaded %s (%d rows)",
             lbl, nkrec_rows);
    return 0;
}

static void load_krec_from_procs(void)
{
    char *buf = (char *)malloc(16384);
    size_t pos = 0, cap = 16384;
    struct k_psinfo p;

    if (!buf)
        return;
    pos += (size_t)snprintf(buf + pos, cap - pos,
                            "@krec pid:int ppid:int uid:int state:str name:str\n");
    for (int i = 0; psinfo(i, &p) == 0 && pos + 96 < cap; i++) {
        int n;
        p.name[sizeof(p.name) - 1] = '\0';
        n = snprintf(buf + pos, cap - pos, "%d\t%d\t%u\t%s\t%s\n",
                     p.pid, p.ppid, p.uid, proc_state_str(p.state), p.name);
        if (n > 0)
            pos += (size_t)n;
    }
    parse_krec_text(buf, "live:procs");
    free(buf);
    snprintf(status_msg, sizeof(status_msg), "Loaded live procs @krec (%d rows)",
             nkrec_rows);
}

/* ---- refresh & filtering --------------------------------------------- */

static int raw_count(enum sight_cat cat)
{
    switch (cat) {
    case CAT_PROCS:   return nprocs;
    case CAT_WINS:    return nwins;
    case CAT_PARCELS: return nparcels;
    case CAT_PULSES:  return npulses;
    case CAT_MODS:    return nmods;
    case CAT_DEVS:    return ndevs;
    case CAT_MOUNTS:  return nmounts;
    case CAT_NET:     return nnet_rows;
    case CAT_KREC:    return nkrec_rows;
    default:          return 0;
    }
}

static void format_row(enum sight_cat cat, int idx, char *out, size_t outsz)
{
    out[0] = '\0';
    switch (cat) {
    case CAT_PROCS: {
        const struct k_psinfo *p = &procs[idx];
        snprintf(out, outsz, "%-5d  %-4d  %-4u  %-8s  %s",
                 p->pid, p->ppid, p->uid, proc_state_str(p->state), p->name);
        break;
    }
    case CAT_WINS: {
        const struct k_winsummary *w = &wins[idx];
        char geom[24], fl[24];
        int ws = (int)((w->state >> K_WIN_STATE_WS_SHIFT) &
                       K_WIN_STATE_WS_MASK) + 1;
        snprintf(geom, sizeof(geom), "%ux%u+%d+%d",
                 w->width, w->height, w->x, w->y);
        fmt_win_flags(w->state, fl, sizeof(fl));
        snprintf(out, outsz, "%-4u  %-4d  %d   %-16s  %-14s  %s",
                 w->wid, w->pid, ws, geom, fl, w->title);
        break;
    }
    case CAT_PARCELS: {
        const struct k_parcel_info *p = &parcels[idx];
        snprintf(out, outsz, "%-4d  %-4u  %-6s  %-7u  %s",
                 parcel_slots[idx], p->id, parcel_type_str(p->type), p->size,
                 p->label[0] ? p->label : parcel_previews[idx]);
        break;
    }
    case CAT_PULSES: {
        const struct k_pulse *p = &pulses[idx];
        snprintf(out, outsz, "%-5llu  %-6s  %-11s  %-6u  %s",
                 (unsigned long long)p->seq, pulse_topic_str(p->topic),
                 pulse_kind_str(p->kind), p->arg0,
                 p->tag[0] ? p->tag : "-");
        break;
    }
    case CAT_MODS: {
        const struct k_modinfo *m = &mods[idx];
        snprintf(out, outsz, "%-15s  %-8u  %-4u  %-9s  %s",
                 m->name, m->size, m->refs, mod_state_str(m->state), m->desc);
        break;
    }
    case CAT_DEVS: {
        const struct k_devinfo *d = &devs[idx];
        char id_s[16];
        const char *cn = pci_class_str(d->class_id);
        if (d->vendor || d->device)
            snprintf(id_s, sizeof(id_s), "%04x:%04x", d->vendor, d->device);
        else
            snprintf(id_s, sizeof(id_s), "-");
        snprintf(out, outsz, "%-8s  %-14s  %-10s  %-9s  %s",
                 d->bus, d->name, d->bound ? d->driver : "-", id_s,
                 cn ? cn : "-");
        break;
    }
    case CAT_MOUNTS: {
        const struct k_mountinfo *m = &mounts[idx];
        unsigned long long tot = 0, fr = 0;
        if (m->block_size && m->blocks) {
            tot = (m->blocks * m->block_size) / 1024ULL;
            fr = (m->free_blocks * m->block_size) / 1024ULL;
        }
        snprintf(out, outsz, "%-15s  %-6s  %-9s  %-9llu  %llu",
                 m->path, m->fstype, m->device[0] ? m->device : "-", tot, fr);
        break;
    }
    case CAT_NET:
        snprintf(out, outsz, "%-16s  %s", net_rows[idx].key,
                 net_rows[idx].val);
        break;
    case CAT_KREC: {
        size_t o = 0;
        int lim = krec_sc.ncols < 5 ? krec_sc.ncols : 5;
        for (int c = 0; c < lim && o + 4 < outsz; c++) {
            int n = snprintf(out + o, outsz - o, "%s%s",
                             c > 0 ? " | " : "", krec_rows[idx].fields[c]);
            if (n > 0)
                o += (size_t)n;
        }
        break;
    }
    default:
        break;
    }
}

static int row_matches_filter(enum sight_cat cat, int idx)
{
    char line[256];

    if (!filter_buf[0])
        return 1;
    format_row(cat, idx, line, sizeof(line));
    if (krec_strcasestr(line, filter_buf))
        return 1;
    if (cat == CAT_PARCELS && krec_strcasestr(parcel_previews[idx], filter_buf))
        return 1;
    if (cat == CAT_KREC) {
        for (int c = 0; c < krec_sc.ncols; c++) {
            if (krec_strcasestr(krec_rows[idx].fields[c], filter_buf))
                return 1;
        }
    }
    return 0;
}

static void rebuild_filter(void)
{
    int total = raw_count(cur_cat);

    nfiltered = 0;
    for (int i = 0; i < total && nfiltered < MAX_ITEMS; i++) {
        if (row_matches_filter(cur_cat, i))
            filtered[nfiltered++] = i;
    }
    main_list.count = nfiltered;
    if (nfiltered == 0)
        main_list.sel = -1;
    else if (main_list.sel < 0)
        main_list.sel = 0;
    else if (main_list.sel >= nfiltered)
        main_list.sel = nfiltered - 1;
}

static int selected_raw_index(void)
{
    if (main_list.sel < 0 || main_list.sel >= nfiltered)
        return -1;
    return filtered[main_list.sel];
}

static void select_by_raw_match(int (*pred)(int idx, long arg), long arg)
{
    for (int i = 0; i < nfiltered; i++) {
        if (pred(filtered[i], arg)) {
            main_list.sel = i;
            return;
        }
    }
}

static int match_pid(int idx, long pid) { return procs[idx].pid == (int)pid; }
static int match_wid(int idx, long wid) { return wins[idx].wid == (uint32_t)wid; }
static int match_parcel(int idx, long id) { return parcels[idx].id == (uint32_t)id; }

static void refresh_procs(void)
{
    int prev_raw = (cur_cat == CAT_PROCS) ? selected_raw_index() : -1;
    int prev_pid = prev_raw >= 0 ? procs[prev_raw].pid : want_pid;

    nprocs = 0;
    while (nprocs < MAX_PROCS && psinfo(nprocs, &procs[nprocs]) == 0) {
        procs[nprocs].name[sizeof(procs[nprocs].name) - 1] = '\0';
        nprocs++;
    }
    if (cur_cat == CAT_PROCS) {
        rebuild_filter();
        if (prev_pid >= 0)
            select_by_raw_match(match_pid, prev_pid);
        want_pid = -1;
    }
}

static void refresh_wins(void)
{
    int prev_raw = (cur_cat == CAT_WINS) ? selected_raw_index() : -1;
    long prev_wid = prev_raw >= 0 ? (long)wins[prev_raw].wid : (long)want_wid;

    nwins = 0;
    wins_restricted = 0;
    if (gui_window_list(0, &wins[0]) == 0) {
        wins[0].title[sizeof(wins[0].title) - 1] = '\0';
        nwins = 1;
        while (nwins < MAX_WINS && gui_window_list(nwins, &wins[nwins]) == 0) {
            wins[nwins].title[sizeof(wins[nwins].title) - 1] = '\0';
            nwins++;
        }
    } else {
        wins_restricted = 1;
    }
    if (cur_cat == CAT_WINS) {
        rebuild_filter();
        if (prev_wid >= 0)
            select_by_raw_match(match_wid, prev_wid);
        want_wid = -1;
    }
}

static void refresh_parcels(void)
{
    int prev_raw = (cur_cat == CAT_PARCELS) ? selected_raw_index() : -1;
    long prev_id = prev_raw >= 0 ? (long)parcels[prev_raw].id
                                 : (long)want_parcel;

    nparcels = 0;
    for (int s = 0; s < K_PARCEL_MAX_SLOTS; s++) {
        struct k_parcel_info info;
        char raw[256];
        long got;

        if (parcel_list(s, &info) != 0)
            continue;
        info.mime[sizeof(info.mime) - 1] = '\0';
        info.label[sizeof(info.label) - 1] = '\0';
        parcels[nparcels] = info;
        parcel_slots[nparcels] = s;
        parcel_previews[nparcels][0] = '\0';
        parcel_schemas[nparcels][0] = '\0';

        if (info.size > 0) {
            uint32_t want = info.size < sizeof(raw) - 1
                                ? info.size : (uint32_t)(sizeof(raw) - 1);
            got = parcel_get(info.id, 0, raw, want);
            if (got > 0) {
                raw[got] = '\0';
                sanitize_text(raw, (size_t)got, parcel_previews[nparcels],
                              sizeof(parcel_previews[nparcels]));
                if (info.type == K_PARCEL_KREC ||
                    strncmp(raw, "@krec", 5) == 0) {
                    char hdr[256];
                    struct krec_schema sc;
                    long l = 0;
                    while (l < got && l + 1 < (long)sizeof(hdr) &&
                           raw[l] != '\n' && raw[l] != '\r') {
                        hdr[l] = raw[l];
                        l++;
                    }
                    hdr[l] = '\0';
                    if (krec_parse_header(hdr, &sc) == 0) {
                        size_t o = 0;
                        for (int c = 0; c < sc.ncols && o + 4 < 160; c++) {
                            int w = snprintf(parcel_schemas[nparcels] + o,
                                             160 - o, "%s%s:%s",
                                             c > 0 ? " " : "",
                                             sc.cols[c].name,
                                             krec_type_name(sc.cols[c].type));
                            if (w > 0)
                                o += (size_t)w;
                        }
                    }
                }
            }
        }
        nparcels++;
    }
    if (cur_cat == CAT_PARCELS) {
        rebuild_filter();
        if (prev_id >= 0)
            select_by_raw_match(match_parcel, prev_id);
        want_parcel = -1;
    }
}

static void push_pulse(const struct k_pulse *p)
{
    int keep = npulses < MAX_PULSES ? npulses : MAX_PULSES - 1;

    for (int i = keep; i > 0; i--)
        pulses[i] = pulses[i - 1];
    pulses[0] = *p;
    pulses[0].tag[sizeof(pulses[0].tag) - 1] = '\0';
    if (npulses < MAX_PULSES)
        npulses++;
}

static void init_pulses(void)
{
    struct k_pulse batch[64];
    int n;

    npulses = 0;
    pulse_history.seq = 0;
    for (;;) {
        n = pulse_cursor_read(&pulse_history, batch, 64, 0);
        if (n <= 0)
            break;
        for (int i = 0; i < n; i++)
            push_pulse(&batch[i]);
        if (n < 64)
            break;
    }
}

static void refresh_mods(void)
{
    nmods = 0;
    while (nmods < MAX_MODS && modlist_at(nmods, &mods[nmods]) == 0) {
        mods[nmods].name[sizeof(mods[nmods].name) - 1] = '\0';
        mods[nmods].desc[sizeof(mods[nmods].desc) - 1] = '\0';
        nmods++;
    }
    if (cur_cat == CAT_MODS)
        rebuild_filter();
}

static void refresh_devs(void)
{
    ndevs = 0;
    while (ndevs < MAX_DEVS && devlist_at(ndevs, &devs[ndevs]) == 0) {
        devs[ndevs].bus[sizeof(devs[ndevs].bus) - 1] = '\0';
        devs[ndevs].name[sizeof(devs[ndevs].name) - 1] = '\0';
        devs[ndevs].driver[sizeof(devs[ndevs].driver) - 1] = '\0';
        ndevs++;
    }
    if (cur_cat == CAT_DEVS)
        rebuild_filter();
}

static void refresh_mounts(void)
{
    nmounts = 0;
    while (nmounts < MAX_MOUNTS &&
           mountlist_at(nmounts, &mounts[nmounts]) == 0) {
        mounts[nmounts].path[sizeof(mounts[nmounts].path) - 1] = '\0';
        mounts[nmounts].fstype[sizeof(mounts[nmounts].fstype) - 1] = '\0';
        mounts[nmounts].device[sizeof(mounts[nmounts].device) - 1] = '\0';
        nmounts++;
    }
    if (cur_cat == CAT_MOUNTS)
        rebuild_filter();
}

static void add_net_row(const char *k, const char *v, uint32_t tint)
{
    if (nnet_rows >= MAX_NET_ROWS)
        return;
    snprintf(net_rows[nnet_rows].key, sizeof(net_rows[0].key), "%s", k);
    snprintf(net_rows[nnet_rows].val, sizeof(net_rows[0].val), "%s", v);
    net_rows[nnet_rows].tint = tint;
    nnet_rows++;
}

static void refresh_net(void)
{
    char ip[16], mask[16], gw[16], dns[16], net_s[16], bcast[16], mac[24];
    char pfx_s[16];
    int pfx;

    nnet_rows = 0;
    memset(&net_info, 0, sizeof(net_info));
    net_ok = (netinfo(&net_info) == 0);
    if (!net_ok) {
        add_net_row("Interface", "none (no NIC initialized)", GUI_ERROR);
    } else {
        add_net_row("Interface", "eth0", GUI_TEXT);
        add_net_row("Link State", net_info.up ? "up" : "down",
                    net_info.up ? GUI_OK : GUI_WARN);
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 net_info.mac[0], net_info.mac[1], net_info.mac[2],
                 net_info.mac[3], net_info.mac[4], net_info.mac[5]);
        add_net_row("MAC Address", mac, GUI_TEXT);
        add_net_row("IPv4 Address", ip_ntoa(net_info.ip, ip), GUI_ACCENT);
        pfx = mask_prefix_len(net_info.netmask);
        snprintf(pfx_s, sizeof(pfx_s), "/%d", pfx >= 0 ? pfx : 0);
        add_net_row("Netmask", ip_ntoa(net_info.netmask, mask), GUI_TEXT);
        add_net_row("Prefix", pfx >= 0 ? pfx_s : "non-contiguous", GUI_TEXT);
        add_net_row("Gateway", net_info.gateway ? ip_ntoa(net_info.gateway, gw)
                                                : "(unset)", GUI_TEXT);
        add_net_row("Nameserver", net_info.dns ? ip_ntoa(net_info.dns, dns)
                                               : "(unset)", GUI_TEXT);
        add_net_row("Network",
                    ip_ntoa(net_info.ip & net_info.netmask, net_s), GUI_TEXT);
        add_net_row("Broadcast",
                    ip_ntoa((net_info.ip & net_info.netmask) | ~net_info.netmask,
                            bcast), GUI_TEXT);
    }
    if (cur_cat == CAT_NET)
        rebuild_filter();
}

static void refresh_current_category(void)
{
    switch (cur_cat) {
    case CAT_PROCS:   refresh_procs(); break;
    case CAT_WINS:    refresh_wins(); break;
    case CAT_PARCELS: refresh_parcels(); break;
    case CAT_PULSES:  rebuild_filter(); break;
    case CAT_MODS:    refresh_mods(); break;
    case CAT_DEVS:    refresh_devs(); break;
    case CAT_MOUNTS:  refresh_mounts(); break;
    case CAT_NET:     refresh_net(); break;
    case CAT_KREC:
        if (nkrec_rows == 0 && strcmp(krec_source, "(none)") == 0) {
            for (int s = 0; s < K_PARCEL_MAX_SLOTS; s++) {
                struct k_parcel_info info;
                if (parcel_list(s, &info) == 0 &&
                    info.type == K_PARCEL_KREC) {
                    load_krec_from_parcel(info.id);
                    break;
                }
            }
        }
        rebuild_filter();
        break;
    default:
        break;
    }
}

static void refresh_all_counts(void)
{
    refresh_procs();
    refresh_wins();
    refresh_parcels();
    refresh_mods();
    refresh_devs();
    refresh_mounts();
    refresh_net();
    rebuild_filter();
}

static void check_live_pulses(void)
{
    struct k_pulse batch[32];
    int n, any = 0;
    uint32_t topics = 0;

    for (;;) {
        n = pulse_cursor_read(&pulse_history, batch, 32, 0);
        if (n <= 0)
            break;
        for (int i = 0; i < n; i++) {
            topics |= batch[i].topic;
            push_pulse(&batch[i]);
        }
        any = 1;
        if (n < 32)
            break;
    }
    if (!any)
        return;

    if (topics & K_TOPIC_PROC)
        refresh_procs();
    if (topics & K_TOPIC_WIN)
        refresh_wins();
    if (topics & K_TOPIC_PARCEL)
        refresh_parcels();
    if (topics & K_TOPIC_NET)
        refresh_net();
    if (topics & K_TOPIC_FS)
        refresh_mounts();
    if (cur_cat == CAT_PULSES)
        rebuild_filter();
    dirty = 1;
}

/* ---- list callbacks -------------------------------------------------- */

static const char *list_item_cb(void *ctx, int index)
{
    static char row_buf[256];

    (void)ctx;
    if (index < 0 || index >= nfiltered)
        return "";
    format_row(cur_cat, filtered[index], row_buf, sizeof(row_buf));
    return row_buf;
}

static uint32_t list_tint_cb(void *ctx, int index)
{
    int raw;

    (void)ctx;
    if (index < 0 || index >= nfiltered)
        return GUI_TEXT;
    raw = filtered[index];
    switch (cur_cat) {
    case CAT_PROCS:
        if (procs[raw].state == K_STATE_RUNNING)  return GUI_OK;
        if (procs[raw].state == K_STATE_ZOMBIE)   return GUI_WARN;
        if (procs[raw].state == K_STATE_STOPPED)  return GUI_ERROR;
        return GUI_TEXT;
    case CAT_WINS:
        if (wins[raw].state & K_WIN_STATE_FOCUSED)   return GUI_ACCENT;
        if (wins[raw].state & K_WIN_STATE_MINIMIZED) return GUI_TEXT_DIM;
        if (wins[raw].state & K_WIN_STATE_PINNED)    return GUI_OK;
        return GUI_TEXT;
    case CAT_PARCELS:
        if (parcels[raw].type == K_PARCEL_KREC) return GUI_ACCENT;
        if (parcels[raw].type == K_PARCEL_URL)  return GUI_OK;
        return GUI_TEXT;
    case CAT_PULSES:
        if (pulses[raw].topic == K_TOPIC_PROC)   return GUI_OK;
        if (pulses[raw].topic == K_TOPIC_WIN)    return GUI_ACCENT;
        if (pulses[raw].topic == K_TOPIC_PARCEL) return GUI_WARN;
        return GUI_TEXT;
    case CAT_DEVS:
        return devs[raw].bound ? GUI_OK : GUI_TEXT_DIM;
    case CAT_NET:
        return net_rows[raw].tint;
    default:
        return GUI_TEXT;
    }
}

/* ---- Talon pinning for ANY selected object --------------------------- */

static void pin_selected_to_talon(void)
{
    int raw = selected_raw_index();
    char payload[2048];
    char label[48];
    uint32_t ptype = K_PARCEL_TEXT;
    const char *mime = "text/plain";
    int pid = -1;

    if (cur_cat == CAT_NET && net_ok && raw < 0)
        raw = 0;
    if (raw < 0) {
        snprintf(status_msg, sizeof(status_msg), "Nothing selected to pin");
        return;
    }

    payload[0] = '\0';
    snprintf(label, sizeof(label), "sight:%s", cat_names[cur_cat]);

    switch (cur_cat) {
    case CAT_PROCS: {
        const struct k_psinfo *p = &procs[raw];
        snprintf(label, sizeof(label), "proc:%d %s", p->pid, p->name);
        snprintf(payload, sizeof(payload),
                 "proc:%d name=%s state=%s uid=%u ppid=%d",
                 p->pid, p->name, proc_state_str(p->state), p->uid, p->ppid);
        break;
    }
    case CAT_WINS: {
        const struct k_winsummary *w = &wins[raw];
        char fl[32];
        int ws = (int)((w->state >> K_WIN_STATE_WS_SHIFT) &
                       K_WIN_STATE_WS_MASK) + 1;
        fmt_win_flags(w->state, fl, sizeof(fl));
        snprintf(label, sizeof(label), "win:%u %s", w->wid, w->title);
        snprintf(payload, sizeof(payload),
                 "win:%u pid=%d ws=%d geom=%ux%u+%d+%d flags=%s title=%s",
                 w->wid, w->pid, ws, w->width, w->height, w->x, w->y,
                 fl, w->title);
        break;
    }
    case CAT_PARCELS: {
        const struct k_parcel_info *info = &parcels[raw];
        char *buf = (char *)malloc(info->size ? info->size + 1 : 1);
        if (buf && info->size > 0) {
            long got = parcel_get(info->id, 0, buf, info->size);
            if (got > 0) {
                pid = parcel_put(info->type, info->mime, info->label,
                                 buf, (uint32_t)got);
                free(buf);
                if (pid >= 0)
                    snprintf(status_msg, sizeof(status_msg),
                             "Promoted parcel #%u -> #%d", info->id, pid);
                else
                    snprintf(status_msg, sizeof(status_msg),
                             "parcel_put failed");
                refresh_parcels();
                return;
            }
        }
        free(buf);
        snprintf(payload, sizeof(payload), "%s", parcel_previews[raw]);
        snprintf(label, sizeof(label), "parcel:%u", info->id);
        break;
    }
    case CAT_PULSES: {
        const struct k_pulse *p = &pulses[raw];
        snprintf(label, sizeof(label), "pulse:%llu %s",
                 (unsigned long long)p->seq, pulse_kind_str(p->kind));
        snprintf(payload, sizeof(payload),
                 "pulse:%llu topic=%s kind=%s arg0=%u arg1=%u tag=%s",
                 (unsigned long long)p->seq, pulse_topic_str(p->topic),
                 pulse_kind_str(p->kind), p->arg0, p->arg1, p->tag);
        break;
    }
    case CAT_MODS: {
        const struct k_modinfo *m = &mods[raw];
        snprintf(label, sizeof(label), "mod:%s", m->name);
        snprintf(payload, sizeof(payload),
                 "mod:%s size=%u refs=%u state=%s desc=%s",
                 m->name, m->size, m->refs, mod_state_str(m->state), m->desc);
        break;
    }
    case CAT_DEVS: {
        const struct k_devinfo *d = &devs[raw];
        snprintf(label, sizeof(label), "dev:%s/%s", d->bus, d->name);
        snprintf(payload, sizeof(payload),
                 "dev:%s/%s driver=%s bound=%s id=%04x:%04x class=0x%04x",
                 d->bus, d->name, d->bound ? d->driver : "-",
                 d->bound ? "true" : "false",
                 d->vendor, d->device, d->class_id);
        break;
    }
    case CAT_MOUNTS: {
        const struct k_mountinfo *m = &mounts[raw];
        ptype = K_PARCEL_PATH;
        mime = "text/x-path";
        snprintf(label, sizeof(label), "mount:%s", m->path);
        snprintf(payload, sizeof(payload), "%s", m->path);
        break;
    }
    case CAT_NET: {
        char ip[16], gw[16], dns[16];
        if (net_ok) {
            snprintf(label, sizeof(label), "net:%s", net_rows[raw].key);
            snprintf(payload, sizeof(payload),
                     "eth0 state=%s ip=%s gw=%s dns=%s (%s=%s)",
                     net_info.up ? "up" : "down",
                     ip_ntoa(net_info.ip, ip),
                     ip_ntoa(net_info.gateway, gw),
                     ip_ntoa(net_info.dns, dns),
                     net_rows[raw].key, net_rows[raw].val);
        } else {
            snprintf(payload, sizeof(payload), "net: offline");
        }
        break;
    }
    case CAT_KREC: {
        size_t pos = 0;
        if (krec_is_typed && krec_sc.ncols > 0) {
            ptype = K_PARCEL_KREC;
            mime = "text/x-krec";
            pos += (size_t)snprintf(payload + pos, sizeof(payload) - pos,
                                    "@krec");
            for (int c = 0; c < krec_sc.ncols && pos + 32 < sizeof(payload); c++)
                pos += (size_t)snprintf(payload + pos, sizeof(payload) - pos,
                                        " %s:%s", krec_sc.cols[c].name,
                                        krec_type_name(krec_sc.cols[c].type));
            pos += (size_t)snprintf(payload + pos, sizeof(payload) - pos, "\n");
            for (int c = 0; c < krec_sc.ncols && pos + 8 < sizeof(payload); c++)
                pos += (size_t)snprintf(payload + pos, sizeof(payload) - pos,
                                        "%s%s", c > 0 ? "\t" : "",
                                        krec_rows[raw].fields[c]);
            snprintf(payload + pos, sizeof(payload) - pos, "\n");
            snprintf(label, sizeof(label), "krec:%s", krec_source);
        } else {
            snprintf(payload, sizeof(payload), "%s",
                     krec_sc.ncols >= 2 ? krec_rows[raw].fields[1]
                                        : krec_source);
            snprintf(label, sizeof(label), "sight:%s", krec_source);
        }
        break;
    }
    default:
        break;
    }

    pid = parcel_put(ptype, mime, label, payload, (uint32_t)strlen(payload));
    if (pid >= 0) {
        snprintf(status_msg, sizeof(status_msg),
                 "Pinned '%s' to Talon parcel #%d", label, pid);
        refresh_parcels();
    } else {
        snprintf(status_msg, sizeof(status_msg), "Pin to Talon failed");
    }
}

/* ---- detail card construction ---------------------------------------- */

static void add_kv(struct kv_pair *kvs, int *nkvs, int max_kvs,
                   const char *k, const char *v, uint32_t col)
{
    if (*nkvs >= max_kvs)
        return;
    snprintf(kvs[*nkvs].key, sizeof(kvs[0].key), "%s", k);
    snprintf(kvs[*nkvs].val, sizeof(kvs[0].val), "%s",
             (v && v[0]) ? v : "-");
    kvs[*nkvs].color = col;
    kvs[*nkvs].is_color_swatch = 0;
    kvs[*nkvs].swatch_rgb = 0;
    (*nkvs)++;
}

static void build_detail(int raw, char *title, size_t titlesz,
                         char *badge, size_t badgesz, uint32_t *badge_col,
                         struct kv_pair *kvs, int *nkvs)
{
    char buf[128];

    *nkvs = 0;
    *badge_col = GUI_ACCENT;
    snprintf(title, titlesz, "No selection");
    snprintf(badge, badgesz, "%s", cat_names[cur_cat]);
    if (raw < 0)
        return;

    switch (cur_cat) {
    case CAT_PROCS: {
        const struct k_psinfo *p = &procs[raw];
        snprintf(title, titlesz, "%s (PID %d)", p->name, p->pid);
        snprintf(badge, badgesz, "%s", proc_state_str(p->state));
        *badge_col = (p->state == K_STATE_RUNNING) ? GUI_OK
                   : (p->state == K_STATE_STOPPED) ? GUI_ERROR : GUI_ACCENT;
        snprintf(buf, sizeof(buf), "%d", p->pid);
        add_kv(kvs, nkvs, MAX_KV_ROWS, "PID", buf, GUI_TEXT);
        snprintf(buf, sizeof(buf), "%d", p->ppid);
        add_kv(kvs, nkvs, MAX_KV_ROWS, "PPID", buf, GUI_TEXT);
        snprintf(buf, sizeof(buf), "%u", p->uid);
        add_kv(kvs, nkvs, MAX_KV_ROWS, "UID", buf, GUI_TEXT);
        add_kv(kvs, nkvs, MAX_KV_ROWS, "State", proc_state_str(p->state),
               *badge_col);
        add_kv(kvs, nkvs, MAX_KV_ROWS, "Name", p->name, GUI_TEXT);
        break;
    }
    case CAT_WINS: {
        const struct k_winsummary *w = &wins[raw];
        char fl[48];
        int ws = (int)((w->state >> K_WIN_STATE_WS_SHIFT) &
                       K_WIN_STATE_WS_MASK) + 1;
        int act_ws = (int)((w->state >> K_WIN_STATE_ACTIVE_WS_SHIFT) &
                           K_WIN_STATE_WS_MASK) + 1;
        snprintf(title, titlesz, "%s", w->title[0] ? w->title : "(untitled)");
        snprintf(badge, badgesz, "WID %u", w->wid);
        *badge_col = (w->state & K_WIN_STATE_FOCUSED) ? GUI_OK : GUI_ACCENT;
        snprintf(buf, sizeof(buf), "%u", w->wid);
        add_kv(kvs, nkvs, MAX_KV_ROWS, "Window ID", buf, GUI_TEXT);
        snprintf(buf, sizeof(buf), "%d", w->pid);
        add_kv(kvs, nkvs, MAX_KV_ROWS, "Owner PID", buf, GUI_TEXT);
        snprintf(buf, sizeof(buf), "WS %d (active %d)", ws, act_ws);
        add_kv(kvs, nkvs, MAX_KV_ROWS, "Workspace", buf, GUI_ACCENT);
        snprintf(buf, sizeof(buf), "%ux%u at (%d, %d)",
                 w->width, w->height, w->x, w->y);
        add_kv(kvs, nkvs, MAX_KV_ROWS, "Geometry", buf, GUI_TEXT);
        fmt_win_flags(w->state, fl, sizeof(fl));
        add_kv(kvs, nkvs, MAX_KV_ROWS, "Flags", fl, GUI_TEXT);
        snprintf(buf, sizeof(buf), "0x%08x", w->state);
        add_kv(kvs, nkvs, MAX_KV_ROWS, "State Bits", buf, GUI_TEXT_DIM);
        break;
    }
    case CAT_PARCELS: {
        const struct k_parcel_info *p = &parcels[raw];
        snprintf(title, titlesz, "%s", p->label[0] ? p->label : "(parcel)");
        snprintf(badge, badgesz, "%s", parcel_type_str(p->type));
        *badge_col = p->type == K_PARCEL_KREC ? GUI_ACCENT : GUI_OK;
        snprintf(buf, sizeof(buf), "%u (slot %d)", p->id, parcel_slots[raw]);
        add_kv(kvs, nkvs, MAX_KV_ROWS, "Parcel ID", buf, GUI_TEXT);
        add_kv(kvs, nkvs, MAX_KV_ROWS, "MIME", p->mime, GUI_TEXT);
        snprintf(buf, sizeof(buf), "%u bytes", p->size);
        add_kv(kvs, nkvs, MAX_KV_ROWS, "Size", buf, GUI_TEXT);
        snprintf(buf, sizeof(buf), "%d", p->src_pid);
        add_kv(kvs, nkvs, MAX_KV_ROWS, "Source PID", buf, GUI_TEXT);
        if (parcel_schemas[raw][0])
            add_kv(kvs, nkvs, MAX_KV_ROWS, "@krec Schema",
                   parcel_schemas[raw], GUI_ACCENT);
        add_kv(kvs, nkvs, MAX_KV_ROWS, "Preview", parcel_previews[raw],
               GUI_TEXT);
        if (p->type == K_PARCEL_COLOR) {
            uint32_t rgb = 0;
            if (parse_hex_color(parcel_previews[raw], &rgb) == 0 &&
                *nkvs > 0) {
                kvs[*nkvs - 1].is_color_swatch = 1;
                kvs[*nkvs - 1].swatch_rgb = rgb;
            }
        }
        break;
    }
    case CAT_PULSES: {
        const struct k_pulse *p = &pulses[raw];
        snprintf(title, titlesz, "%s", pulse_kind_str(p->kind));
        snprintf(badge, badgesz, "%s", pulse_topic_str(p->topic));
        snprintf(buf, sizeof(buf), "%llu", (unsigned long long)p->seq);
        add_kv(kvs, nkvs, MAX_KV_ROWS, "Sequence", buf, GUI_ACCENT);
        add_kv(kvs, nkvs, MAX_KV_ROWS, "Topic", pulse_topic_str(p->topic),
               GUI_TEXT);
        add_kv(kvs, nkvs, MAX_KV_ROWS, "Kind", pulse_kind_str(p->kind),
               GUI_TEXT);
        snprintf(buf, sizeof(buf), "%u (0x%x)", p->arg0, p->arg0);
        add_kv(kvs, nkvs, MAX_KV_ROWS, "Arg0", buf, GUI_TEXT);
        snprintf(buf, sizeof(buf), "%u (0x%x)", p->arg1, p->arg1);
        add_kv(kvs, nkvs, MAX_KV_ROWS, "Arg1", buf, GUI_TEXT);
        add_kv(kvs, nkvs, MAX_KV_ROWS, "Tag", p->tag, GUI_TEXT);
        break;
    }
    case CAT_MODS: {
        const struct k_modinfo *m = &mods[raw];
        snprintf(title, titlesz, "%s", m->name);
        snprintf(badge, badgesz, "%s", mod_state_str(m->state));
        *badge_col = m->state == K_MOD_LIVE ? GUI_OK : GUI_WARN;
        add_kv(kvs, nkvs, MAX_KV_ROWS, "Name", m->name, GUI_TEXT);
        snprintf(buf, sizeof(buf), "%u bytes", m->size);
        add_kv(kvs, nkvs, MAX_KV_ROWS, "Memory", buf, GUI_TEXT);
        snprintf(buf, sizeof(buf), "%u", m->refs);
        add_kv(kvs, nkvs, MAX_KV_ROWS, "Ref Count", buf, GUI_TEXT);
        add_kv(kvs, nkvs, MAX_KV_ROWS, "State", mod_state_str(m->state),
               *badge_col);
        add_kv(kvs, nkvs, MAX_KV_ROWS, "Description", m->desc, GUI_TEXT);
        break;
    }
    case CAT_DEVS: {
        const struct k_devinfo *d = &devs[raw];
        const char *cn = pci_class_str(d->class_id);
        snprintf(title, titlesz, "%s", d->name);
        snprintf(badge, badgesz, "%s", d->bus);
        *badge_col = d->bound ? GUI_OK : GUI_WARN;
        add_kv(kvs, nkvs, MAX_KV_ROWS, "Bus", d->bus, GUI_TEXT);
        add_kv(kvs, nkvs, MAX_KV_ROWS, "Device", d->name, GUI_TEXT);
        add_kv(kvs, nkvs, MAX_KV_ROWS, "Driver",
               d->bound ? d->driver : "(unbound)", *badge_col);
        snprintf(buf, sizeof(buf), "%04x:%04x", d->vendor, d->device);
        add_kv(kvs, nkvs, MAX_KV_ROWS, "Vendor:Dev",
               (d->vendor || d->device) ? buf : "-", GUI_TEXT);
        if (cn)
            snprintf(buf, sizeof(buf), "%s (0x%04x)", cn, d->class_id);
        else
            snprintf(buf, sizeof(buf), "0x%04x", d->class_id);
        add_kv(kvs, nkvs, MAX_KV_ROWS, "PCI Class", buf, GUI_TEXT);
        break;
    }
    case CAT_MOUNTS: {
        const struct k_mountinfo *m = &mounts[raw];
        snprintf(title, titlesz, "%s", m->path);
        snprintf(badge, badgesz, "%s", m->fstype);
        add_kv(kvs, nkvs, MAX_KV_ROWS, "Mount Point", m->path, GUI_ACCENT);
        add_kv(kvs, nkvs, MAX_KV_ROWS, "Filesystem", m->fstype, GUI_TEXT);
        add_kv(kvs, nkvs, MAX_KV_ROWS, "Device",
               m->device[0] ? m->device : "-", GUI_TEXT);
        if (m->block_size && m->blocks) {
            unsigned long long tot = (m->blocks * m->block_size) / 1024ULL;
            unsigned long long fr = (m->free_blocks * m->block_size) / 1024ULL;
            snprintf(buf, sizeof(buf), "%llu KiB (%llu KiB free)", tot, fr);
            add_kv(kvs, nkvs, MAX_KV_ROWS, "Capacity", buf, GUI_TEXT);
            snprintf(buf, sizeof(buf), "%u bytes", m->block_size);
            add_kv(kvs, nkvs, MAX_KV_ROWS, "Block Size", buf, GUI_TEXT);
        }
        break;
    }
    case CAT_NET: {
        snprintf(title, titlesz, "%s", net_rows[raw].key);
        snprintf(badge, badgesz, "%s", net_ok && net_info.up ? "up" : "down");
        *badge_col = (net_ok && net_info.up) ? GUI_OK : GUI_WARN;
        add_kv(kvs, nkvs, MAX_KV_ROWS, net_rows[raw].key, net_rows[raw].val,
               net_rows[raw].tint);
        for (int i = 0; i < nnet_rows && *nkvs < MAX_KV_ROWS; i++) {
            if (i != raw)
                add_kv(kvs, nkvs, MAX_KV_ROWS, net_rows[i].key,
                       net_rows[i].val, GUI_TEXT_DIM);
        }
        break;
    }
    case CAT_KREC: {
        snprintf(title, titlesz, "Row %d", raw + 1);
        snprintf(badge, badgesz, "%s", krec_is_typed ? "@krec" : "text");
        add_kv(kvs, nkvs, MAX_KV_ROWS, "Source", krec_source, GUI_ACCENT);
        if (krec_fs_detail[0])
            add_kv(kvs, nkvs, MAX_KV_ROWS, "Stat", krec_fs_detail,
                   GUI_TEXT_DIM);
        for (int c = 0; c < krec_sc.ncols && *nkvs < MAX_KV_ROWS; c++) {
            char kbuf[48];
            snprintf(kbuf, sizeof(kbuf), "%s:%s", krec_sc.cols[c].name,
                     krec_type_name(krec_sc.cols[c].type));
            add_kv(kvs, nkvs, MAX_KV_ROWS, kbuf, krec_rows[raw].fields[c],
                   GUI_TEXT);
        }
        break;
    }
    default:
        break;
    }
}

/* ---- drawing --------------------------------------------------------- */

static void draw_wrapped_kv(gui_window *win, gui_rc area,
                            const struct kv_pair *kvs, int nkvs,
                            gui_ui *ui)
{
    int col_w = (area.w - 16) / GUI_FONT_W;
    int total_lines = 0;
    int cur_y = 0, vis_lines, max_scroll;
    gui_rc text_rc = area;

    if (col_w < 12)
        col_w = 12;

    for (int i = 0; i < nkvs; i++) {
        int vlen = (int)strlen(kvs[i].val);
        int lines = (vlen + col_w - 1) / col_w;
        if (lines < 1)
            lines = 1;
        total_lines += 1 + lines;
    }

    vis_lines = area.h / 18;
    if (vis_lines < 1)
        vis_lines = 1;
    max_scroll = total_lines - vis_lines;
    if (max_scroll < 0)
        max_scroll = 0;

    if (ui->wheel && gui_hit(area, ui->mx, ui->my)) {
        detail_scroll -= ui->wheel * 2;
        ui->wheel = 0;
    }
    if (detail_scroll < 0)
        detail_scroll = 0;
    if (detail_scroll > max_scroll)
        detail_scroll = max_scroll;

    if (total_lines > vis_lines) {
        text_rc.w -= 12;
        gui_scrollbar(win, gui_mkrc(area.x + area.w - 10, area.y, 10, area.h),
                      total_lines, vis_lines, &detail_scroll, ui);
    } else {
        detail_scroll = 0;
    }

    gui_push_clip(win, text_rc);
    {
        int line_idx = 0;
        col_w = (text_rc.w - 8) / GUI_FONT_W;
        if (col_w < 10)
            col_w = 10;
        for (int i = 0; i < nkvs; i++) {
            const char *v = kvs[i].val;
            int vlen = (int)strlen(v);
            int off = 0;

            if (line_idx >= detail_scroll &&
                cur_y + 18 <= text_rc.h) {
                int y = text_rc.y + cur_y;
                gui_draw_text_aa(win, text_rc.x + 2, y + 2, kvs[i].key,
                                 FONT_SMALL, FONT_BOLD, FONT_ROMAN,
                                 GUI_TEXT_DIM, GUI_TRANSPARENT);
                cur_y += 18;
            }
            line_idx++;

            do {
                int take = vlen - off;
                if (take > col_w)
                    take = col_w;
                if (line_idx >= detail_scroll &&
                    cur_y < text_rc.h) {
                    int y = text_rc.y + cur_y;
                    int xoff = text_rc.x + 4;
                    if (kvs[i].is_color_swatch && off == 0) {
                        gui_round_rect(win, gui_mkrc(xoff, y + 2, 14, 14),
                                       3, kvs[i].swatch_rgb);
                        gui_round_frame(win, gui_mkrc(xoff, y + 2, 14, 14),
                                        3, GUI_EDGE);
                        xoff += 20;
                    }
                    gui_text_n(win, xoff, y, v + off, take,
                               kvs[i].color, GUI_TRANSPARENT);
                    cur_y += 18;
                }
                off += take;
                line_idx++;
            } while (off < vlen);
        }
    }
    gui_pop_clip(win);
}

static void win_ctl_action(uint32_t wid, int action, const char *desc)
{
    if (gui_window_control(wid, action) == 0) {
        snprintf(status_msg, sizeof(status_msg), "%s (WID %u)", desc, wid);
        refresh_wins();
    } else {
        snprintf(status_msg, sizeof(status_msg),
                 "win_ctl denied for WID %u (desktop capability required)",
                 wid);
    }
    need_repaint = 1;
}

static void paint(gui_window *win, gui_ui *ui)
{
    int W = win->w, H = win->h;
    int top_h = 46, bot_h = 28, pad = 10;
    int rail_w = W >= 820 ? 156 : (W >= 620 ? 136 : 114);
    int detail_w = W >= 820 ? 304 : (W >= 620 ? 256 : (W - rail_w) / 2);
    int center_x, center_w, body_y, body_h;
    gui_rc filter_rc, refresh_rc, list_rc, detail_rc;
    int raw;

    if (detail_w < 160)
        detail_w = 160;
    center_x = rail_w + pad;
    center_w = W - rail_w - detail_w - pad * 3;
    if (center_w < 160) {
        center_w = 160;
        detail_w = W - rail_w - center_w - pad * 3;
        if (detail_w < 130)
            detail_w = 130;
    }
    body_y = top_h + 8;
    body_h = H - top_h - bot_h - 16;
    if (body_h < 80)
        body_h = 80;

    gui_clear(win, GUI_SURFACE);

    /* ---- Top Bar ---- */
    gui_hgradient(win, gui_mkrc(0, 0, W, top_h),
                  gui_mix(GUI_PANEL, GUI_ACCENT, 26), GUI_PANEL);
    gui_rect(win, 0, top_h - 1, W, 1, GUI_EDGE);
    gui_icon(win, 12, 11, 22, GUI_ICON_SEARCH, GUI_ACCENT);
    gui_draw_text_aa(win, 40, 11, "Sight", FONT_LARGE, FONT_BOLD, FONT_ROMAN,
                     GUI_TEXT, GUI_TRANSPARENT);

    {
        char seq_pill[32];
        int pw;
        snprintf(seq_pill, sizeof(seq_pill), "pulse #%llu",
                 (unsigned long long)pulse_history.seq);
        pw = (int)strlen(seq_pill) * 6 + 18;
        if (W >= 660)
            gui_draw_pill(win, gui_mkrc(112, 12, pw, 22),
                          gui_mix(GUI_OK, GUI_SURFACE, 80), (long)GUI_OK);
        if (W >= 660)
            gui_draw_text_aa(win, 121, 17, seq_pill, FONT_SMALL,
                             FONT_REGULAR, FONT_ROMAN, GUI_TEXT,
                             GUI_TRANSPARENT);
    }

    refresh_rc = gui_mkrc(W - 86, 10, 76, 26);
    {
        int fx = W >= 660 ? 250 : 120;
        int fw = refresh_rc.x - fx - 54;
        if (fw < 90)
            fw = 90;
        gui_text(win, fx, 15, cur_cat == CAT_KREC ? "Open:" : "Filter:",
                 GUI_TEXT_DIM, GUI_TRANSPARENT);
        filter_rc = gui_mkrc(fx + (cur_cat == CAT_KREC ? 44 : 58), 10,
                             fw - (cur_cat == CAT_KREC ? 0 : 14), 26);
    }
    if (gui_textbox(win, filter_rc, &filter_tb, ui)) {
        if (cur_cat == CAT_KREC && filter_buf[0] == '/') {
            load_krec_from_file(filter_buf);
            filter_buf[0] = '\0';
            filter_tb.len = 0;
            filter_tb.caret = 0;
            filter_tb.scroll = 0;
        }
        rebuild_filter();
        need_repaint = 1;
    }
    if (strcmp(filter_buf, last_filter) != 0) {
        snprintf(last_filter, sizeof(last_filter), "%s", filter_buf);
        rebuild_filter();
        need_repaint = 1;
    }
    if (gui_button(win, refresh_rc, "Refresh", ui)) {
        refresh_all_counts();
        snprintf(status_msg, sizeof(status_msg), "Refreshed %s",
                 cat_names[cur_cat]);
        need_repaint = 1;
    }

    /* ---- Left Category Rail ---- */
    gui_rect(win, 0, top_h, rail_w, H - top_h - bot_h,
             gui_mix(GUI_PANEL, GUI_BLACK, 14));
    gui_rect(win, rail_w - 1, top_h, 1, H - top_h - bot_h, GUI_EDGE);

    for (int c = 0; c < CAT_COUNT; c++) {
        int ry = top_h + 10 + c * 36;
        gui_rc r;
        char cnt[16];
        int cw, active = (c == (int)cur_cat);

        if (ry + 32 > H - bot_h)
            break;
        r = gui_mkrc(8, ry, rail_w - 16, 30);
        if (gui_button_ex(win, r, 0, ui, 1, active)) {
            cur_cat = (enum sight_cat)c;
            refresh_current_category();
            snprintf(status_msg, sizeof(status_msg), "Viewing %s (%d)",
                     cat_names[cur_cat], nfiltered);
            need_repaint = 1;
        }
        if (active)
            gui_rect(win, r.x, r.y + 5, 3, r.h - 10, GUI_ACCENT);
        gui_draw_text_aa_run(win, r.x + 9, r.y + 9, cat_names[c], -1,
                             FONT_SMALL, active ? FONT_BOLD : FONT_REGULAR,
                             FONT_ROMAN, active ? GUI_WHITE : GUI_TEXT,
                             GUI_TRANSPARENT, r.w - 40, 1);

        if (c == CAT_WINS && wins_restricted)
            snprintf(cnt, sizeof(cnt), "wm");
        else
            snprintf(cnt, sizeof(cnt), "%d", raw_count((enum sight_cat)c));
        cw = (int)strlen(cnt) * 6 + 10;
        gui_draw_pill(win, gui_mkrc(r.x + r.w - cw - 4, r.y + 6, cw, 18),
                      active ? GUI_ACCENT : GUI_SURFACE_ALT, -1L);
        gui_draw_text_aa(win, r.x + r.w - cw + 1, r.y + 9, cnt,
                         FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                         active ? GUI_TEXT_INV : GUI_TEXT_DIM, GUI_TRANSPARENT);
    }

    /* ---- Center Data List ---- */
    gui_round_rect(win, gui_mkrc(center_x, body_y, center_w, 22), 5,
                   GUI_PANEL);
    gui_push_clip_xy(win, center_x + 4, body_y, center_w - 8, 22);
    {
        char krec_hdr[160];
        const char *hdr_s = cat_headers[cur_cat];
        if (cur_cat == CAT_KREC && krec_sc.ncols > 0) {
            size_t ho = 0;
            int lim = krec_sc.ncols < 5 ? krec_sc.ncols : 5;
            krec_hdr[0] = '\0';
            for (int c = 0; c < lim && ho + 8 < sizeof(krec_hdr); c++) {
                int w = snprintf(krec_hdr + ho, sizeof(krec_hdr) - ho,
                                 "%s%s:%s", c > 0 ? " | " : "",
                                 krec_sc.cols[c].name,
                                 krec_type_name(krec_sc.cols[c].type));
                if (w > 0)
                    ho += (size_t)w;
            }
            hdr_s = krec_hdr;
        }
        gui_draw_text_aa(win, center_x + 6, body_y + 5, hdr_s,
                         FONT_SMALL, FONT_BOLD, FONT_ROMAN,
                         GUI_TEXT_DIM, GUI_TRANSPARENT);
    }
    gui_pop_clip(win);

    list_rc = gui_mkrc(center_x, body_y + 24, center_w, body_h - 24);
    main_list.row_h = 22;
    main_list.item = list_item_cb;
    main_list.tint = list_tint_cb;
    main_list.ctx = 0;
    (void)gui_listbox(win, list_rc, &main_list, ui);

    if (cur_cat == CAT_WINS && wins_restricted && nwins == 0) {
        gui_draw_text_aa_run(win, list_rc.x + 12, list_rc.y + 14,
                             "SYS_WIN_LIST restricted to desktop-shell",
                             -1, FONT_BODY, FONT_REGULAR, FONT_ROMAN,
                             GUI_WARN, GUI_TRANSPARENT, list_rc.w - 24, 1);
    } else if (cur_cat == CAT_KREC && nkrec_rows == 0) {
        gui_draw_text_aa_run(win, list_rc.x + 12, list_rc.y + 14,
                             "Enter /path above or click [From Talon] / [Procs @krec]",
                             -1, FONT_SMALL, FONT_REGULAR, FONT_ROMAN,
                             GUI_TEXT_DIM, GUI_TRANSPARENT, list_rc.w - 24, 1);
    }

    /* ---- Right Structured Inspector Detail Card ---- */
    detail_rc = gui_mkrc(center_x + center_w + pad, body_y, detail_w, body_h);
    gui_card(win, detail_rc);

    raw = selected_raw_index();
    if (raw != last_sel_for_scroll || cur_cat != last_cat_for_scroll) {
        detail_scroll = 0;
        last_sel_for_scroll = raw;
        last_cat_for_scroll = cur_cat;
    }

    {
        char dtitle[96], dbadge[32];
        uint32_t bcol = GUI_ACCENT;
        struct kv_pair kvs[MAX_KV_ROWS];
        int nkvs = 0;
        int act_h = (cur_cat == CAT_WINS) ? 108
                  : (cur_cat == CAT_PROCS || cur_cat == CAT_PARCELS ||
                     cur_cat == CAT_KREC || cur_cat == CAT_PULSES) ? 74 : 42;
        int kv_y = detail_rc.y + 48;
        int kv_h = detail_rc.h - 52 - act_h;
        int bw, ay;

        build_detail(raw, dtitle, sizeof(dtitle), dbadge, sizeof(dbadge),
                     &bcol, kvs, &nkvs);

        bw = (int)strlen(dbadge) * 6 + 16;
        if (bw > detail_rc.w / 2)
            bw = detail_rc.w / 2;
        gui_draw_text_aa_run(win, detail_rc.x + 12, detail_rc.y + 14, dtitle,
                             -1, FONT_BODY, FONT_BOLD, FONT_ROMAN,
                             GUI_TEXT, GUI_TRANSPARENT,
                             detail_rc.w - bw - 28, 1);
        gui_badge(win, gui_mkrc(detail_rc.x + detail_rc.w - bw - 10,
                                detail_rc.y + 12, bw, 22), dbadge, bcol);
        gui_rect(win, detail_rc.x + 10, detail_rc.y + 42,
                 detail_rc.w - 20, 1, GUI_EDGE);

        if (kv_h > 20)
            draw_wrapped_kv(win,
                            gui_mkrc(detail_rc.x + 10, kv_y,
                                     detail_rc.w - 20, kv_h),
                            kvs, nkvs, ui);

        ay = detail_rc.y + detail_rc.h - act_h + 4;
        gui_rect(win, detail_rc.x + 10, ay - 6, detail_rc.w - 20, 1, GUI_EDGE);

        if (cur_cat == CAT_WINS) {
            int enabled = (raw >= 0);
            uint32_t wid = enabled ? wins[raw].wid : 0;
            uint32_t st = enabled ? wins[raw].state : 0;
            int is_min = (st & K_WIN_STATE_MINIMIZED) != 0;
            int is_pin = (st & K_WIN_STATE_PINNED) != 0;
            int is_max = (st & K_WIN_STATE_MAXIMIZED) != 0;
            int cur_ws = (int)((st >> K_WIN_STATE_WS_SHIFT) &
                               K_WIN_STATE_WS_MASK);
            int bw4 = (detail_rc.w - 24 - 12) / 4;
            int bw5 = (detail_rc.w - 24 - 16) / 5;

            if (gui_button_ex(win, gui_mkrc(detail_rc.x + 12, ay, bw4, 28),
                              "Focus", ui, enabled, 0))
                win_ctl_action(wid, K_WIN_CTL_FOCUS, "Focused");
            if (gui_button_ex(win,
                              gui_mkrc(detail_rc.x + 12 + (bw4 + 4), ay, bw4, 28),
                              is_min ? "Restore" : "Min", ui, enabled, 0))
                win_ctl_action(wid,
                               is_min ? K_WIN_CTL_RESTORE : K_WIN_CTL_MINIMIZE,
                               is_min ? "Restored" : "Minimized");
            if (gui_button_ex(win,
                              gui_mkrc(detail_rc.x + 12 + 2 * (bw4 + 4), ay,
                                       bw4, 28),
                              "Max", ui, enabled, is_max))
                win_ctl_action(wid, K_WIN_CTL_MAXIMIZE, "Toggled maximize");
            if (gui_button_ex(win,
                              gui_mkrc(detail_rc.x + 12 + 3 * (bw4 + 4), ay,
                                       bw4, 28),
                              "Pin", ui, enabled, is_pin))
                win_ctl_action(wid, K_WIN_CTL_TOGGLE_PIN, "Toggled pin");

            ay += 34;
            for (int ws_i = 0; ws_i < 4; ws_i++) {
                char wsl[8];
                snprintf(wsl, sizeof(wsl), "WS%d", ws_i + 1);
                if (gui_button_ex(win,
                                  gui_mkrc(detail_rc.x + 12 + ws_i * (bw5 + 4),
                                           ay, bw5, 28),
                                  wsl, ui, enabled, enabled && cur_ws == ws_i)) {
                    char msg[32];
                    snprintf(msg, sizeof(msg), "Moved to WS %d", ws_i + 1);
                    win_ctl_action(wid, K_WIN_CTL_MOVE_WS | (ws_i << 8), msg);
                }
            }
            if (gui_button_ex(win,
                              gui_mkrc(detail_rc.x + 12 + 4 * (bw5 + 4),
                                       ay, bw5, 28),
                              "Close", ui, enabled, 0))
                win_ctl_action(wid, K_WIN_CTL_CLOSE, "Sent close");
            ay += 34;
        } else if (cur_cat == CAT_PROCS) {
            int can_sig = raw >= 0 && procs[raw].pid > 1 &&
                          procs[raw].pid != getpid();
            int bw2 = (detail_rc.w - 28) / 2;

            if (gui_button_ex(win, gui_mkrc(detail_rc.x + 12, ay, bw2, 28),
                              "SIGTERM", ui, can_sig, 0)) {
                if (kill(procs[raw].pid, SIGTERM) == 0)
                    snprintf(status_msg, sizeof(status_msg),
                             "Sent SIGTERM to PID %d", procs[raw].pid);
                else
                    snprintf(status_msg, sizeof(status_msg),
                             "SIGTERM failed for PID %d", procs[raw].pid);
                refresh_procs();
                need_repaint = 1;
            }
            if (gui_button_ex(win,
                              gui_mkrc(detail_rc.x + 16 + bw2, ay, bw2, 28),
                              "SIGKILL", ui, can_sig, 0)) {
                if (kill(procs[raw].pid, SIGKILL) == 0)
                    snprintf(status_msg, sizeof(status_msg),
                             "Sent SIGKILL to PID %d", procs[raw].pid);
                else
                    snprintf(status_msg, sizeof(status_msg),
                             "SIGKILL failed for PID %d", procs[raw].pid);
                refresh_procs();
                need_repaint = 1;
            }
            ay += 34;
        } else if (cur_cat == CAT_PARCELS) {
            int has = (raw >= 0);
            int is_k = has && parcels[raw].type == K_PARCEL_KREC;
            int bw2 = (detail_rc.w - 28) / 2;

            if (gui_button_ex(win, gui_mkrc(detail_rc.x + 12, ay, bw2, 28),
                              "Promote", ui, has, 0)) {
                pin_selected_to_talon();
                need_repaint = 1;
            }
            if (gui_button_ex(win,
                              gui_mkrc(detail_rc.x + 16 + bw2, ay, bw2, 28),
                              "Open @krec", ui, is_k, 0)) {
                load_krec_from_parcel(parcels[raw].id);
                cur_cat = CAT_KREC;
                rebuild_filter();
                need_repaint = 1;
            }
            ay += 34;
        } else if (cur_cat == CAT_KREC) {
            int bw2 = (detail_rc.w - 28) / 2;
            if (gui_button(win, gui_mkrc(detail_rc.x + 12, ay, bw2, 28),
                           "From Talon", ui)) {
                load_krec_from_parcel(0);
                rebuild_filter();
                need_repaint = 1;
            }
            if (gui_button(win, gui_mkrc(detail_rc.x + 16 + bw2, ay, bw2, 28),
                           "Procs @krec", ui)) {
                load_krec_from_procs();
                rebuild_filter();
                need_repaint = 1;
            }
            ay += 34;
        } else if (cur_cat == CAT_PULSES) {
            if (gui_button(win, gui_mkrc(detail_rc.x + 12, ay,
                                         detail_rc.w - 24, 28),
                           "Clear Pulse Log", ui)) {
                npulses = 0;
                rebuild_filter();
                snprintf(status_msg, sizeof(status_msg), "Pulse log cleared");
                need_repaint = 1;
            }
            ay += 34;
        }

        if (gui_button_ex(win, gui_mkrc(detail_rc.x + 12, ay,
                                        detail_rc.w - 24, 28),
                          "Pin to Talon", ui,
                          raw >= 0 || (cur_cat == CAT_NET && net_ok), 0)) {
            pin_selected_to_talon();
            need_repaint = 1;
        }
    }

    /* ---- Bottom Status Bar ---- */
    gui_rect(win, 0, H - bot_h, W, bot_h, GUI_PANEL);
    gui_rect(win, 0, H - bot_h, W, 1, GUI_EDGE);
    gui_push_clip_xy(win, 10, H - bot_h, W - 180, bot_h);
    gui_text(win, 10, H - bot_h + 6, status_msg, GUI_TEXT_DIM,
             GUI_TRANSPARENT);
    gui_pop_clip(win);

    {
        char right_s[48];
        snprintf(right_s, sizeof(right_s), "%d / %d items",
                 nfiltered, raw_count(cur_cat));
        gui_text(win, W - gui_text_w(right_s) - 10, H - bot_h + 6,
                 right_s, GUI_TEXT, GUI_TRANSPARENT);
    }
}

/* ---- startup target parsing ------------------------------------------ */

static int parse_i32_arg(const char *s, int *out)
{
    int64_t v = 0;

    if (!s || !s[0] || krec_parse_i64(s, &v) < 0 ||
        v < -2147483648LL || v > 2147483647LL)
        return -1;
    *out = (int)v;
    return 0;
}

static void apply_target_arg(const char *arg)
{
    int v = 0;

    if (!arg || !arg[0])
        return;
    if (strcmp(arg, "procs") == 0 || strcmp(arg, "proc") == 0) {
        cur_cat = CAT_PROCS;
    } else if (strncmp(arg, "proc:", 5) == 0 &&
               parse_i32_arg(arg + 5, &v) == 0) {
        cur_cat = CAT_PROCS;
        want_pid = v;
    } else if (strcmp(arg, "wins") == 0 || strcmp(arg, "win") == 0) {
        cur_cat = CAT_WINS;
    } else if (strncmp(arg, "win:", 4) == 0 &&
               parse_i32_arg(arg + 4, &v) == 0) {
        cur_cat = CAT_WINS;
        want_wid = v;
    } else if (strcmp(arg, "parcels") == 0 || strcmp(arg, "parcel") == 0) {
        cur_cat = CAT_PARCELS;
    } else if (strncmp(arg, "parcel:", 7) == 0 &&
               parse_i32_arg(arg + 7, &v) == 0) {
        cur_cat = CAT_PARCELS;
        want_parcel = v;
    } else if (strcmp(arg, "pulse") == 0 || strcmp(arg, "pulses") == 0) {
        cur_cat = CAT_PULSES;
    } else if (strcmp(arg, "mod") == 0 || strcmp(arg, "mods") == 0) {
        cur_cat = CAT_MODS;
    } else if (strcmp(arg, "dev") == 0 || strcmp(arg, "devs") == 0) {
        cur_cat = CAT_DEVS;
    } else if (strcmp(arg, "mount") == 0 || strcmp(arg, "mounts") == 0) {
        cur_cat = CAT_MOUNTS;
    } else if (strcmp(arg, "net") == 0) {
        cur_cat = CAT_NET;
    } else if (strncmp(arg, "fs:", 3) == 0) {
        cur_cat = CAT_KREC;
        load_krec_from_file(arg + 3);
    } else if (arg[0] == '/' || strchr(arg, '/') != 0 ||
               strstr(arg, ".krec") != 0) {
        cur_cat = CAT_KREC;
        load_krec_from_file(arg);
    }
}

int main(int argc, char **argv)
{
    gui_window *win;
    gui_ui ui;
    struct k_event ev;
    const char *target = 0;
    int sw = 1024, sh = 768, win_w = 880, win_h = 560;
    unsigned long last_blink = 0;

    snprintf(g_cwd, sizeof(g_cwd), "%s", kestrel_startup_cwd());
    if (argc > 1)
        target = argv[1];

    memset(&ui, 0, sizeof(ui));
    memset(&main_list, 0, sizeof(main_list));
    main_list.sel = -1;
    filter_tb.buf = filter_buf;
    filter_tb.cap = sizeof(filter_buf);

    init_pulses();
    if (target)
        apply_target_arg(target);
    refresh_all_counts();

    if (gui_screen(&sw, &sh) == 0) {
        if (win_w > sw - 40)
            win_w = sw - 40;
        if (win_h > sh - 80)
            win_h = sh - 80;
        if (win_w < 540)
            win_w = 540;
        if (win_h < 380)
            win_h = 380;
    }

    win = gui_open("Sight Inspector", 70, 50, win_w, win_h, 0);
    if (!win) {
        printf("sight: no window manager available\n");
        free_krec_rows();
        return 1;
    }

    while (win->open) {
        int got = gui_next_event(win, &ev, dirty ? 0 : 100);

        if (got < 0)
            break;
        gui_ui_begin(&ui);
        if (got > 0) {
            gui_ui_event(&ui, &ev);
            if (ev.type == KEV_CLOSE)
                break;
            if (ev.type == KEV_KEY && ev.key == 18) { /* Ctrl+R refresh */
                refresh_all_counts();
                ui.key = 0;
            }
            dirty = 1;
        }

        check_live_pulses();

        if (filter_tb.focus) {
            unsigned long phase = uptime_ms() / 500UL;
            if (phase != last_blink) {
                last_blink = phase;
                dirty = 1;
            }
        }

        if (!dirty)
            continue;
        need_repaint = 0;
        paint(win, &ui);
        if (gui_flush(win) != 0)
            break;
        dirty = need_repaint;
    }

    gui_close(win);
    free_krec_rows();
    return 0;
}
