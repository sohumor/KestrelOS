/* talon.c - CLI frontend for KestrelOS Talon typed clipboard parcels.
 *
 * usage:
 *   talon ls|list [-r]
 *   talon put [-t type] [-m mime] [-l label] [value|-]
 *   talon get [-i] [-r] [slot_or_id]
 *   talon copy [-t type] [-m mime] [-l label] <file>
 *   talon promote <slot_or_id>
 */

#include <kestrel.h>
#include <krec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PATH 256
#define CTRL_D   4

static const char *g_cwd = "/";

static void resolve(const char *tok, char *out, unsigned long outsz)
{
    if (tok[0] == '/')
        snprintf(out, outsz, "%s", tok);
    else if (g_cwd[0] == '/' && g_cwd[1] == '\0')
        snprintf(out, outsz, "/%s", tok);
    else
        snprintf(out, outsz, "%s/%s", g_cwd, tok);
}

static int write_all(int fd, const void *buf, unsigned long len)
{
    const char *p = (const char *)buf;
    unsigned long off = 0;

    while (off < len) {
        long w = write(fd, p + off, len - off);
        if (w <= 0)
            return -1;
        off += (unsigned long)w;
    }
    return 0;
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
    default:              return "?";
    }
}

static int parse_parcel_type(const char *s, uint32_t *out)
{
    if (!s || !out)
        return -1;
    if (strcmp(s, "text") == 0)   { *out = K_PARCEL_TEXT;   return 0; }
    if (strcmp(s, "path") == 0)   { *out = K_PARCEL_PATH;   return 0; }
    if (strcmp(s, "url") == 0)    { *out = K_PARCEL_URL;    return 0; }
    if (strcmp(s, "color") == 0)  { *out = K_PARCEL_COLOR;  return 0; }
    if (strcmp(s, "number") == 0) { *out = K_PARCEL_NUMBER; return 0; }
    if (strcmp(s, "krec") == 0)   { *out = K_PARCEL_KREC;   return 0; }
    if (strcmp(s, "image") == 0)  { *out = K_PARCEL_IMAGE;  return 0; }
    return -1;
}

static const char *default_mime(uint32_t t)
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

static int parse_u32(const char *s, uint32_t *out)
{
    uint64_t v = 0;

    if (!s || !s[0])
        return -1;
    if (krec_parse_u64(s, &v) < 0 || v > 0xffffffffULL)
        return -1;
    *out = (uint32_t)v;
    return 0;
}

static int is_hex_digit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static int is_krec_payload(const char *buf, uint32_t size)
{
    return size >= 6 && strncmp(buf, "@krec", 5) == 0 &&
           (buf[5] == ' ' || buf[5] == '\t');
}

static uint32_t detect_type(const char *buf, uint32_t size, int allow_path)
{
    uint32_t len = size;

    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        len--;

    if (is_krec_payload(buf, size))
        return K_PARCEL_KREC;
    if (len >= 7 && (strncmp(buf, "http://", 7) == 0 ||
                     (len >= 8 && strncmp(buf, "https://", 8) == 0))) {
        uint32_t i;
        int ok = 1;
        for (i = 0; i < len; i++) {
            if (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n' ||
                buf[i] == '\r') {
                ok = 0;
                break;
            }
        }
        if (ok)
            return K_PARCEL_URL;
    }
    if (buf[0] == '#' && (len == 4 || len == 7 || len == 9)) {
        uint32_t i;
        int ok = 1;
        for (i = 1; i < len; i++) {
            if (!is_hex_digit(buf[i])) {
                ok = 0;
                break;
            }
        }
        if (ok)
            return K_PARCEL_COLOR;
    }
    if (len > 0 && len < 48) {
        uint32_t i = 0;
        int digits = 0, dots = 0, ok = 1;
        if (buf[0] == '+' || buf[0] == '-')
            i = 1;
        for (; i < len; i++) {
            if (buf[i] >= '0' && buf[i] <= '9') {
                digits = 1;
            } else if (buf[i] == '.' && !dots) {
                dots = 1;
            } else {
                ok = 0;
                break;
            }
        }
        if (ok && digits)
            return K_PARCEL_NUMBER;
    }
    if (allow_path && len > 0 && len < 256 && buf[0] == '/') {
        uint32_t i;
        int ok = 1;
        for (i = 0; i < len; i++) {
            if (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n' ||
                buf[i] == '\r') {
                ok = 0;
                break;
            }
        }
        if (ok)
            return K_PARCEL_PATH;
    }
    return K_PARCEL_TEXT;
}

static void make_label_from_payload(const char *buf, uint32_t size,
                                    char *out, size_t outsz)
{
    uint32_t i = 0, o = 0;

    if (outsz == 0)
        return;
    while (i < size && (buf[i] == ' ' || buf[i] == '\t' ||
                        buf[i] == '\n' || buf[i] == '\r'))
        i++;
    while (i < size && o + 1 < outsz && buf[i] != '\n' && buf[i] != '\r') {
        char c = buf[i++];
        if (c == '\t')
            c = ' ';
        else if ((unsigned char)c < 0x20)
            continue;
        out[o++] = c;
    }
    out[o] = '\0';
    if (o == 0)
        snprintf(out, outsz, "parcel");
}

static void usage(void)
{
    printf("usage:\n");
    printf("  talon ls [-r]\n");
    printf("  talon put [-t text|path|url|color|number|krec|image] "
           "[-m mime] [-l label] [value|-]\n");
    printf("  talon get [-i] [-r] [slot_or_id]\n");
    printf("  talon copy [-t type] [-m mime] [-l label] <file>\n");
    printf("  talon promote <slot_or_id>\n");
}

static int cmd_list(int argc, char **argv)
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
    };
    struct k_parcel_info info;
    int rflag = 0, count = 0, i;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--krec") == 0) {
            rflag = 1;
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else {
            usage();
            return 1;
        }
    }

    if (rflag) {
        if (krec_begin(1, cols, 8) < 0)
            return 1;
    } else {
        printf("%-4s %-5s %-7s %7s %5s  %-18s %s\n",
               "SLOT", "ID", "TYPE", "SIZE", "PID", "MIME", "LABEL");
    }

    for (i = 0; i < K_PARCEL_MAX_SLOTS; i++) {
        if (parcel_list(i, &info) != 0)
            continue;
        info.mime[sizeof(info.mime) - 1] = '\0';
        info.label[sizeof(info.label) - 1] = '\0';
        count++;
        if (rflag) {
            char slot_s[16], id_s[16], size_s[24], time_s[24], pid_s[16];
            snprintf(slot_s, sizeof(slot_s), "%d", i);
            snprintf(id_s, sizeof(id_s), "%u", info.id);
            snprintf(size_s, sizeof(size_s), "%u", info.size);
            snprintf(time_s, sizeof(time_s), "%u", info.time);
            snprintf(pid_s, sizeof(pid_s), "%d", info.src_pid);
            krec_row(1, 8, slot_s, id_s, parcel_type_name(info.type),
                     size_s, time_s, pid_s, info.mime, info.label);
        } else {
            printf("%-4d %-5u %-7s %7u %5d  %-18s %s\n",
                   i, info.id, parcel_type_name(info.type), info.size,
                   info.src_pid,
                   info.mime[0] ? info.mime : "-",
                   info.label[0] ? info.label : "-");
        }
    }

    if (!rflag && count == 0)
        printf("(parcel ring is empty)\n");
    return 0;
}

static long read_fd_bounded(int fd, int stop_ctrl_d, char *buf, uint32_t cap)
{
    uint32_t len = 0;

    for (;;) {
        char chunk[512];
        long n = read(fd, chunk, sizeof(chunk));
        long i;
        if (n < 0)
            return -1;
        if (n == 0)
            break;
        for (i = 0; i < n; i++) {
            if (stop_ctrl_d && chunk[i] == CTRL_D)
                return (long)len;
            if (len >= cap)
                return -2;
            buf[len++] = chunk[i];
        }
    }
    return (long)len;
}

static int cmd_put(int argc, char **argv)
{
    const char *mime = 0, *label = 0, *val_arg = 0;
    char auto_label[48];
    uint32_t type = 0;
    int has_type = 0, from_stdin = 0, end_opts = 0, i, id;
    char *heap_buf = 0;
    const char *data = 0;
    uint32_t size = 0;

    for (i = 2; i < argc; i++) {
        if (!end_opts && strcmp(argv[i], "--") == 0) {
            end_opts = 1;
        } else if (!end_opts && strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            if (parse_parcel_type(argv[++i], &type) < 0) {
                krec_err("talon: unknown parcel type '%s'\n", argv[i]);
                return 1;
            }
            has_type = 1;
        } else if (!end_opts && strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            mime = argv[++i];
        } else if (!end_opts && strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            label = argv[++i];
        } else if (!end_opts && (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0)) {
            usage();
            return 0;
        } else if (!end_opts && strcmp(argv[i], "-") == 0 && !val_arg) {
            from_stdin = 1;
            val_arg = argv[i];
        } else if (!end_opts && argv[i][0] == '-' && argv[i][1] != '\0' &&
                   !(argv[i][1] >= '0' && argv[i][1] <= '9')) {
            usage();
            return 1;
        } else if (!val_arg) {
            val_arg = argv[i];
        } else {
            usage();
            return 1;
        }
    }

    if (!val_arg)
        from_stdin = 1;

    if (from_stdin) {
        long got;
        int stop_eod = (!has_type || type != K_PARCEL_IMAGE);
        heap_buf = (char *)malloc(K_PARCEL_MAX_BYTES + 1);
        if (!heap_buf) {
            krec_err("talon: out of memory\n");
            return 1;
        }
        got = read_fd_bounded(0, stop_eod, heap_buf, K_PARCEL_MAX_BYTES);
        if (got == -2) {
            free(heap_buf);
            krec_err("talon: input exceeds %d bytes\n", K_PARCEL_MAX_BYTES);
            return 1;
        }
        if (got < 0) {
            free(heap_buf);
            krec_err("talon: failed to read stdin\n");
            return 1;
        }
        if (got == 0) {
            free(heap_buf);
            krec_err("talon: empty input\n");
            return 1;
        }
        heap_buf[got] = '\0';
        data = heap_buf;
        size = (uint32_t)got;
    } else {
        unsigned long len = strlen(val_arg);
        if (len > K_PARCEL_MAX_BYTES) {
            krec_err("talon: value exceeds %d bytes\n", K_PARCEL_MAX_BYTES);
            return 1;
        }
        data = val_arg;
        size = (uint32_t)len;
    }

    if (!has_type)
        type = detect_type(data, size, !from_stdin);
    if (!mime)
        mime = default_mime(type);
    if (!label) {
        make_label_from_payload(data, size, auto_label, sizeof(auto_label));
        label = auto_label;
    }

    id = parcel_put(type, mime, label, data, size);
    free(heap_buf);
    if (id < 0) {
        krec_err("talon: parcel_put failed\n");
        return 1;
    }
    printf("parcel %d (%s, %u bytes)\n", id, parcel_type_name(type), size);
    return 0;
}

static int cmd_get(int argc, char **argv)
{
    static const struct krec_col cols[] = {
        { "id",      KREC_INT },
        { "type",    KREC_STR },
        { "size",    KREC_U64 },
        { "time",    KREC_U64 },
        { "src_pid", KREC_INT },
        { "mime",    KREC_STR },
        { "label",   KREC_STR },
    };
    struct k_parcel_info info;
    uint32_t target = 0;
    int info_only = 0, rflag = 0, has_target = 0, i;
    char *buf = 0;
    long n;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0) {
            info_only = 1;
        } else if (strcmp(argv[i], "-r") == 0 ||
                   strcmp(argv[i], "--krec") == 0) {
            rflag = 1;
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else if (argv[i][0] == '-') {
            usage();
            return 1;
        } else if (!has_target) {
            if (parse_u32(argv[i], &target) < 0) {
                krec_err("talon: invalid slot or id '%s'\n", argv[i]);
                return 1;
            }
            has_target = 1;
        } else {
            usage();
            return 1;
        }
    }

    if (parcel_get(target, &info, 0, 0) < 0) {
        krec_err("talon: parcel %u not found\n", target);
        return 1;
    }
    info.mime[sizeof(info.mime) - 1] = '\0';
    info.label[sizeof(info.label) - 1] = '\0';

    if (info_only) {
        if (rflag) {
            char id_s[16], size_s[24], time_s[24], pid_s[16];
            if (krec_begin(1, cols, 7) < 0)
                return 1;
            snprintf(id_s, sizeof(id_s), "%u", info.id);
            snprintf(size_s, sizeof(size_s), "%u", info.size);
            snprintf(time_s, sizeof(time_s), "%u", info.time);
            snprintf(pid_s, sizeof(pid_s), "%d", info.src_pid);
            krec_row(1, 7, id_s, parcel_type_name(info.type), size_s,
                     time_s, pid_s, info.mime, info.label);
        } else {
            printf("id:      %u\n", info.id);
            printf("type:    %s\n", parcel_type_name(info.type));
            printf("size:    %u\n", info.size);
            printf("time:    %u\n", info.time);
            printf("src_pid: %d\n", info.src_pid);
            printf("mime:    %s\n", info.mime[0] ? info.mime : "-");
            printf("label:   %s\n", info.label[0] ? info.label : "-");
        }
        return 0;
    }

    if (info.size > K_PARCEL_MAX_BYTES) {
        krec_err("talon: invalid parcel size\n");
        return 1;
    }
    buf = (char *)malloc(info.size ? info.size : 1);
    if (!buf) {
        krec_err("talon: out of memory\n");
        return 1;
    }
    n = parcel_get(target, &info, buf, info.size);
    if (n < 0) {
        free(buf);
        krec_err("talon: failed to read parcel %u\n", target);
        return 1;
    }
    if (n > 0 && write_all(1, buf, (unsigned long)n) < 0) {
        free(buf);
        return 1;
    }
    free(buf);
    return 0;
}

static int cmd_copy(int argc, char **argv)
{
    char path[MAX_PATH];
    struct k_stat st;
    const char *file_arg = 0, *mime = 0, *label = 0, *base;
    uint32_t type = 0;
    int has_type = 0, fd, id, i;
    char *buf;
    long got;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            if (parse_parcel_type(argv[++i], &type) < 0) {
                krec_err("talon: unknown parcel type '%s'\n", argv[i]);
                return 1;
            }
            has_type = 1;
        } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            mime = argv[++i];
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            label = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else if (argv[i][0] == '-') {
            usage();
            return 1;
        } else if (!file_arg) {
            file_arg = argv[i];
        } else {
            usage();
            return 1;
        }
    }

    if (!file_arg) {
        usage();
        return 1;
    }
    resolve(file_arg, path, sizeof(path));
    if (stat_(path, &st) != 0) {
        krec_err("talon: cannot access %s\n", path);
        return 1;
    }
    if (st.is_dir) {
        krec_err("talon: %s is a directory\n", path);
        return 1;
    }
    if (st.size > K_PARCEL_MAX_BYTES) {
        krec_err("talon: %s exceeds %d bytes (%u)\n",
                 path, K_PARCEL_MAX_BYTES, st.size);
        return 1;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        krec_err("talon: cannot open %s\n", path);
        return 1;
    }
    buf = (char *)malloc(K_PARCEL_MAX_BYTES + 1);
    if (!buf) {
        close(fd);
        krec_err("talon: out of memory\n");
        return 1;
    }
    got = read_fd_bounded(fd, 0, buf, K_PARCEL_MAX_BYTES);
    close(fd);
    if (got == -2) {
        free(buf);
        krec_err("talon: %s exceeds %d bytes\n", path, K_PARCEL_MAX_BYTES);
        return 1;
    }
    if (got < 0) {
        free(buf);
        krec_err("talon: failed reading %s\n", path);
        return 1;
    }
    buf[got] = '\0';

    if (!has_type)
        type = is_krec_payload(buf, (uint32_t)got) ? K_PARCEL_KREC
                                                  : K_PARCEL_TEXT;
    if (!mime)
        mime = default_mime(type);
    if (!label) {
        base = strrchr(path, '/');
        label = base ? base + 1 : path;
    }

    id = parcel_put(type, mime, label, buf, (uint32_t)got);
    free(buf);
    if (id < 0) {
        krec_err("talon: parcel_put failed\n");
        return 1;
    }
    printf("parcel %d (%s, %ld bytes)\n", id, parcel_type_name(type), got);
    return 0;
}

static int cmd_promote(int argc, char **argv)
{
    struct k_parcel_info info;
    uint32_t target = 0;
    char *buf;
    long got;
    int id;

    if (argc != 3) {
        usage();
        return 1;
    }
    if (strcmp(argv[2], "-h") == 0 || strcmp(argv[2], "--help") == 0) {
        usage();
        return 0;
    }
    if (parse_u32(argv[2], &target) < 0) {
        krec_err("talon: invalid slot or id '%s'\n", argv[2]);
        return 1;
    }
    if (parcel_get(target, &info, 0, 0) < 0) {
        krec_err("talon: parcel %u not found\n", target);
        return 1;
    }
    if (info.size > K_PARCEL_MAX_BYTES) {
        krec_err("talon: invalid parcel size\n");
        return 1;
    }
    buf = (char *)malloc(info.size ? info.size : 1);
    if (!buf) {
        krec_err("talon: out of memory\n");
        return 1;
    }
    got = parcel_get(target, &info, buf, info.size);
    if (got < 0) {
        free(buf);
        krec_err("talon: failed to read parcel %u\n", target);
        return 1;
    }
    info.mime[sizeof(info.mime) - 1] = '\0';
    info.label[sizeof(info.label) - 1] = '\0';
    id = parcel_put(info.type, info.mime, info.label, buf, (uint32_t)got);
    free(buf);
    if (id < 0) {
        krec_err("talon: parcel_put failed\n");
        return 1;
    }
    printf("parcel %d (%s, %ld bytes, promoted from %u)\n",
           id, parcel_type_name(info.type), got, info.id);
    return 0;
}

int main(int argc, char **argv)
{
    const char *sub;

    argc = krec_strip_cwd(argc, argv, &g_cwd);
    if (argc < 2) {
        usage();
        return 1;
    }

    sub = argv[1];
    if (strcmp(sub, "ls") == 0 || strcmp(sub, "list") == 0)
        return cmd_list(argc, argv);
    if (strcmp(sub, "put") == 0)
        return cmd_put(argc, argv);
    if (strcmp(sub, "get") == 0)
        return cmd_get(argc, argv);
    if (strcmp(sub, "copy") == 0)
        return cmd_copy(argc, argv);
    if (strcmp(sub, "promote") == 0)
        return cmd_promote(argc, argv);
    if (strcmp(sub, "help") == 0 || strcmp(sub, "-h") == 0 ||
        strcmp(sub, "--help") == 0) {
        usage();
        return 0;
    }

    krec_err("talon: unknown subcommand '%s'\n", sub);
    usage();
    return 1;
}
