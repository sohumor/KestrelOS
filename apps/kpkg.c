/* kpkg.c - the KestrelOS package manager.
 *
 *   kpkg install <name|file.kpkg>   install, resolving dependencies
 *   kpkg remove  <name>             remove exactly what it installed
 *   kpkg list                       installed packages and versions
 *   kpkg info    <name>             metadata and file list
 *   kpkg search  <text>             search the repository index
 *   kpkg verify  [name]             re-hash installed files
 *   kpkg update                     refresh the repository index
 *
 * The .kpkg container and the index format are specified in
 * docs/packages.md; tools/mkpkg.py and tools/mkrepo.py are the other
 * implementation of both.
 *
 * Everything that writes needs uid 0. Every install is safe to
 * interrupt: the database entry under /var/pkg/db/<name>/ is written
 * only once extraction has fully succeeded, so a package is either
 * registered and present or absent and unregistered.
 */

#include <kestrel.h>
#include <http.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SHA-256 comes from libc (libc/sha256.c, libc/include/sha256.h), which
 * lands alongside this file. Until that header exists the prototypes
 * below stand in for it; they must match it exactly. */
#if defined(__has_include)
#  if __has_include(<sha256.h>)
#    include <sha256.h>
#    define KPKG_HAVE_SHA256_H 1
#  endif
#endif
#ifndef KPKG_HAVE_SHA256_H
struct sha256_ctx {
    uint32_t state[8];
    uint64_t bitlen;
    uint32_t buflen;
    uint8_t buf[64];
};
void sha256_init(struct sha256_ctx *c);
void sha256_update(struct sha256_ctx *c, const void *data, unsigned long len);
void sha256_final(struct sha256_ctx *c, uint8_t out[32]);
#endif

/* ---- format ---------------------------------------------------------- */

#define KPKG_VERSION      1
#define KPKG_HDR_SIZE     64
#define KPKG_ENT_SIZE     192
#define KPKG_DIGEST       32
#define KPKG_TYPE_FILE    0
#define KPKG_TYPE_DIR     1

struct kpkg_hdr {
    char magic[4];              /* "KPKG" */
    uint32_t version;
    uint32_t header_size;
    uint32_t meta_off;
    uint32_t meta_len;
    uint32_t ftab_off;
    uint32_t ftab_count;
    uint32_t data_off;
    uint32_t data_len;
    uint32_t total_size;        /* whole file, trailing digest included */
    uint32_t flags;
    uint32_t reserved[5];
};

struct kpkg_file {
    char path[128];
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t size;
    uint32_t offset;            /* relative to data_off */
    uint32_t type;
    uint8_t sha256[KPKG_DIGEST];
    uint8_t pad[8];
};

_Static_assert(sizeof(struct kpkg_hdr) == KPKG_HDR_SIZE,
               "kpkg header must be 64 bytes");
_Static_assert(sizeof(struct kpkg_file) == KPKG_ENT_SIZE,
               "kpkg file entry must be 192 bytes");

/* ---- layout ---------------------------------------------------------- */

#define CONF_PATH     "/etc/kpkg.conf"
#define VAR_DIR       "/var"
#define PKG_DIR       "/var/pkg"
#define DB_DIR        "/var/pkg/db"
#define CACHE_DIR     "/var/pkg/cache"
#define INDEX_CACHE   "/var/pkg/index.kpi"
#define INDEX_NAME    "index.kpi"
#define DEFAULT_REPO  "/var/pkg/repo"

#define MAX_PKG_BYTES  (4UL * 1024 * 1024)
#define MAX_INDEX      (256UL * 1024)
#define MAX_DEPTH      16
#define NAME_MAX_      64
#define PATH_MAX_      192
#define IO_CHUNK       4096

/* ---- tiny syscall wrappers -------------------------------------------
 * Named with a kp_ prefix so they cannot collide with libc wrappers for
 * the same syscalls landing in <kestrel.h>. */

static int kp_getuid(void)
{
    return (int)syscall(SYS_GETUID, 0, 0, 0, 0);
}

static int kp_chmod(const char *path, unsigned mode)
{
    return (int)syscall(SYS_CHMOD, (long)path, (long)mode, 0, 0);
}

static int kp_chown(const char *path, unsigned uid, unsigned gid)
{
    return (int)syscall(SYS_CHOWN, (long)path, (long)uid, (long)gid, 0);
}

static unsigned long kp_time(void)
{
    long t = syscall(SYS_TIME, 0, 0, 0, 0);
    return t < 0 ? 0UL : (unsigned long)t;
}

/* ---- string helpers --------------------------------------------------- */

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/* Case-insensitive substring search. */
static int ci_contains(const char *hay, const char *needle)
{
    unsigned long i, j, hn = strlen(hay), nn = strlen(needle);

    if (nn == 0)
        return 1;
    if (nn > hn)
        return 0;
    for (i = 0; i + nn <= hn; i++) {
        for (j = 0; j < nn; j++) {
            if (lower((unsigned char)hay[i + j]) !=
                lower((unsigned char)needle[j]))
                break;
        }
        if (j == nn)
            return 1;
    }
    return 0;
}

/* "0755" for a permission word; libc printf has no %o. */
static void oct_str(unsigned mode, char out[8])
{
    out[0] = '0';
    out[1] = (char)('0' + ((mode >> 6) & 7));
    out[2] = (char)('0' + ((mode >> 3) & 7));
    out[3] = (char)('0' + (mode & 7));
    out[4] = '\0';
}

static unsigned parse_oct(const char *s)
{
    unsigned v = 0;

    while (*s >= '0' && *s <= '7')
        v = (v << 3) | (unsigned)(*s++ - '0');
    return v & 0777;
}

static unsigned long parse_dec(const char *s)
{
    unsigned long v = 0;

    while (*s >= '0' && *s <= '9')
        v = v * 10 + (unsigned long)(*s++ - '0');
    return v;
}

static void hex_str(const unsigned char *d, unsigned long n, char *out)
{
    static const char hx[] = "0123456789abcdef";
    unsigned long i;

    for (i = 0; i < n; i++) {
        out[i * 2] = hx[d[i] >> 4];
        out[i * 2 + 1] = hx[d[i] & 15];
    }
    out[n * 2] = '\0';
}

/* Copy the next whitespace-delimited field; returns the new cursor, or 0
 * if the line ran out. */
static const char *next_field(const char *p, char *out, unsigned long outsz)
{
    unsigned long n = 0;

    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '\0')
        return 0;
    while (*p && *p != ' ' && *p != '\t') {
        if (n + 1 < outsz)
            out[n++] = *p;
        p++;
    }
    out[n] = '\0';
    return p;
}

static void trim(char *s)
{
    unsigned long n = strlen(s);

    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
}

/* ---- file helpers ------------------------------------------------------ */

/* Read a whole file into a malloc'd buffer with a trailing NUL. */
static int read_file(const char *path, unsigned char **out, unsigned long *len,
                     unsigned long cap)
{
    struct k_stat st;
    unsigned char *buf;
    unsigned long got = 0;
    int fd;

    if (stat_(path, &st) < 0)
        return -1;
    if (st.is_dir)
        return -1;
    if (st.size > cap)
        return -2;
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    buf = malloc((unsigned long)st.size + 1);
    if (buf == 0) {
        close(fd);
        return -3;
    }
    while (got < st.size) {
        long n = read(fd, buf + got, st.size - got);
        if (n <= 0)
            break;
        got += (unsigned long)n;
    }
    close(fd);
    if (got != st.size) {
        free(buf);
        return -1;
    }
    buf[got] = '\0';
    *out = buf;
    *len = got;
    return 0;
}

static int write_file(const char *path, const void *data, unsigned long len)
{
    const unsigned char *p = data;
    unsigned long done = 0;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);

    if (fd < 0)
        return -1;
    while (done < len) {
        unsigned long chunk = len - done;
        long n;
        if (chunk > IO_CHUNK)
            chunk = IO_CHUNK;
        n = write(fd, p + done, chunk);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        done += (unsigned long)n;
    }
    close(fd);
    return 0;
}

static int is_dir(const char *path)
{
    struct k_stat st;

    return stat_(path, &st) == 0 && st.is_dir;
}

static int exists(const char *path)
{
    struct k_stat st;

    return stat_(path, &st) == 0;
}

/* mkdir -p. Returns 0 if the directory exists afterwards. */
static int mkdirs(const char *path)
{
    char tmp[PATH_MAX_];
    unsigned long i, n = strlen(path);

    if (n == 0 || n >= sizeof(tmp))
        return -1;
    memcpy(tmp, path, n + 1);
    for (i = 1; i <= n; i++) {
        if (tmp[i] != '/' && tmp[i] != '\0')
            continue;
        {
            char save = tmp[i];
            tmp[i] = '\0';
            if (!is_dir(tmp)) {
                if (mkdir_(tmp) < 0 && !is_dir(tmp)) {
                    tmp[i] = save;
                    return -1;
                }
            }
            tmp[i] = save;
        }
    }
    return is_dir(path) ? 0 : -1;
}

/* Create the parent directories of a file path. */
static int mkdirs_for(const char *path)
{
    char dir[PATH_MAX_];
    const char *slash = strrchr(path, '/');
    unsigned long n;

    if (slash == 0 || slash == path)
        return 0;                       /* lives in / */
    n = (unsigned long)(slash - path);
    if (n >= sizeof(dir))
        return -1;
    memcpy(dir, path, n);
    dir[n] = '\0';
    return mkdirs(dir);
}

/* Reject anything that is not a plain absolute path: kpkg must never be
 * talked into writing outside the tree the package declares. */
static int path_is_safe(const char *p)
{
    unsigned long i, n = strlen(p);

    if (n == 0 || n >= 128 || p[0] != '/')
        return 0;
    for (i = 0; i < n; i++) {
        if (p[i] == ' ' || p[i] == '\t' || p[i] == '\n' || p[i] == '\r')
            return 0;                   /* would break the database format */
    }
    if (strstr(p, "/../") || strstr(p, "//"))
        return 0;
    if (n >= 3 && strcmp(p + n - 3, "/..") == 0)
        return 0;
    return 1;
}

/* ---- SHA-256 ----------------------------------------------------------- */

static void digest_of(const void *data, unsigned long len,
                      unsigned char out[KPKG_DIGEST])
{
    struct sha256_ctx c;

    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

/* Hash a file on disk without slurping it. */
static int digest_of_file(const char *path, unsigned char out[KPKG_DIGEST],
                          unsigned long *size_out)
{
    struct sha256_ctx c;
    unsigned char buf[IO_CHUNK];
    unsigned long total = 0;
    int fd = open(path, O_RDONLY);
    long n;

    if (fd < 0)
        return -1;
    sha256_init(&c);
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        sha256_update(&c, buf, (unsigned long)n);
        total += (unsigned long)n;
    }
    close(fd);
    sha256_final(&c, out);
    if (size_out)
        *size_out = total;
    return n < 0 ? -1 : 0;
}

/* ---- configuration ------------------------------------------------------ */

struct conf {
    char repo[256];
    char cache[PATH_MAX_];
};

static struct conf g_conf;

static void conf_load(void)
{
    unsigned char *buf = 0;
    unsigned long len = 0;
    char *p;

    snprintf(g_conf.repo, sizeof(g_conf.repo), "%s", DEFAULT_REPO);
    snprintf(g_conf.cache, sizeof(g_conf.cache), "%s", CACHE_DIR);

    if (read_file(CONF_PATH, &buf, &len, 8192) != 0)
        return;                          /* defaults are fine without it */

    p = (char *)buf;
    while (*p) {
        char *line = p;
        char *eq;
        while (*p && *p != '\n')
            p++;
        if (*p == '\n')
            *p++ = '\0';
        while (*line == ' ' || *line == '\t')
            line++;
        if (*line == '#' || *line == '\0')
            continue;
        eq = strchr(line, '=');
        if (eq == 0)
            eq = strchr(line, ':');
        if (eq == 0)
            continue;
        *eq = '\0';
        trim(line);
        eq++;
        while (*eq == ' ' || *eq == '\t')
            eq++;
        trim(eq);
        if (strcmp(line, "repo") == 0)
            snprintf(g_conf.repo, sizeof(g_conf.repo), "%s", eq);
        else if (strcmp(line, "cache") == 0)
            snprintf(g_conf.cache, sizeof(g_conf.cache), "%s", eq);
    }
    /* A trailing slash would double up when joining file names. */
    {
        unsigned long n = strlen(g_conf.repo);
        while (n > 1 && g_conf.repo[n - 1] == '/')
            g_conf.repo[--n] = '\0';
    }
    free(buf);
}

static int repo_is_http(void)
{
    return strncmp(g_conf.repo, "http://", 7) == 0;
}

static int repo_is_https(void)
{
    return strncmp(g_conf.repo, "https://", 8) == 0;
}

/* Where the usable copy of the index lives for the configured repo. */
static void index_path(char *out, unsigned long outsz)
{
    if (repo_is_http() || repo_is_https())
        snprintf(out, outsz, "%s", INDEX_CACHE);
    else
        snprintf(out, outsz, "%s/%s", g_conf.repo, INDEX_NAME);
}

/* ---- repository index --------------------------------------------------- */

struct idxent {
    char name[NAME_MAX_];
    char version[32];
    char sha[KPKG_DIGEST * 2 + 1];
    char file[96];
    char depends[256];
    char desc[192];
    unsigned long size;
};

/* Parse one index line. Fields: name version size sha filename depends
 * description... (the description is the rest of the line). */
static int idx_parse(const char *line, struct idxent *e)
{
    char num[32];
    const char *p = line;

    memset(e, 0, sizeof(*e));
    p = next_field(p, e->name, sizeof(e->name));
    if (!p)
        return -1;
    p = next_field(p, e->version, sizeof(e->version));
    if (!p)
        return -1;
    p = next_field(p, num, sizeof(num));
    if (!p)
        return -1;
    e->size = parse_dec(num);
    p = next_field(p, e->sha, sizeof(e->sha));
    if (!p)
        return -1;
    p = next_field(p, e->file, sizeof(e->file));
    if (!p)
        return -1;
    p = next_field(p, e->depends, sizeof(e->depends));
    if (!p)
        return -1;
    if (strcmp(e->depends, "-") == 0)
        e->depends[0] = '\0';
    while (*p == ' ' || *p == '\t')
        p++;
    snprintf(e->desc, sizeof(e->desc), "%s", p);
    return 0;
}

/* Walk the index, calling fn for each entry. fn returns 1 to stop. */
typedef int (*idx_fn)(const struct idxent *e, void *ctx);

static int idx_walk(idx_fn fn, void *ctx, int quiet)
{
    char path[PATH_MAX_];
    unsigned char *buf = 0;
    unsigned long len = 0;
    struct idxent e;
    char *p;
    int rc, stopped = 0;

    index_path(path, sizeof(path));
    rc = read_file(path, &buf, &len, MAX_INDEX);
    if (rc != 0) {
        if (!quiet)
            printf("kpkg: no repository index at %s%s\n", path,
                   (repo_is_http() || repo_is_https())
                       ? " (run `kpkg update`)" : "");
        return -1;
    }

    p = (char *)buf;
    while (*p && !stopped) {
        char *line = p;
        while (*p && *p != '\n')
            p++;
        if (*p == '\n')
            *p++ = '\0';
        trim(line);
        if (line[0] == '#' || line[0] == '\0')
            continue;
        if (idx_parse(line, &e) < 0)
            continue;
        if (fn(&e, ctx))
            stopped = 1;
    }
    free(buf);
    return stopped ? 1 : 0;
}

struct find_ctx {
    const char *want;
    struct idxent out;
    int found;
};

static int find_cb(const struct idxent *e, void *ctx)
{
    struct find_ctx *f = ctx;

    if (strcmp(e->name, f->want) == 0) {
        f->out = *e;
        f->found = 1;
        return 1;
    }
    return 0;
}

static int idx_find(const char *name, struct idxent *out, int quiet)
{
    struct find_ctx f;

    f.want = name;
    f.found = 0;
    if (idx_walk(find_cb, &f, quiet) < 0)
        return -1;
    if (!f.found)
        return -1;
    *out = f.out;
    return 0;
}

/* ---- in-memory package -------------------------------------------------- */

struct pkg {
    unsigned char *raw;
    unsigned long size;
    struct kpkg_hdr h;
    unsigned long count;
    char name[NAME_MAX_];
    char version[32];
    char desc[192];
    char depends[256];
    char arch[32];
    char instsize[32];
};

static void pkg_free(struct pkg *p)
{
    free(p->raw);
    p->raw = 0;
    p->size = 0;
}

static void pkg_entry(const struct pkg *p, unsigned long i,
                      struct kpkg_file *out)
{
    memcpy(out, p->raw + p->h.ftab_off + i * KPKG_ENT_SIZE, KPKG_ENT_SIZE);
    out->path[sizeof(out->path) - 1] = '\0';
}

static const unsigned char *pkg_data(const struct pkg *p,
                                     const struct kpkg_file *e)
{
    return p->raw + p->h.data_off + e->offset;
}

/* Look a "key: value" line up in the metadata block. */
static void meta_get(const char *meta, unsigned long meta_len, const char *key,
                     char *out, unsigned long outsz)
{
    unsigned long klen = strlen(key), i = 0;

    out[0] = '\0';
    while (i < meta_len) {
        unsigned long start = i, end;
        while (i < meta_len && meta[i] != '\n')
            i++;
        end = i;
        if (i < meta_len)
            i++;
        if (end - start <= klen + 1)
            continue;
        if (strncmp(meta + start, key, klen) != 0 || meta[start + klen] != ':')
            continue;
        {
            unsigned long vs = start + klen + 1, n = 0;
            while (vs < end && (meta[vs] == ' ' || meta[vs] == '\t'))
                vs++;
            while (vs < end && n + 1 < outsz)
                out[n++] = meta[vs++];
            while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\r'))
                n--;
            out[n] = '\0';
        }
        return;
    }
}

/* Load and structurally validate a .kpkg, then check its trailing
 * digest. Every offset is bounds-checked before anything is trusted. */
static int pkg_load(const char *path, struct pkg *p, int quiet)
{
    unsigned char digest[KPKG_DIGEST];
    unsigned long body, i;
    int rc;

    memset(p, 0, sizeof(*p));
    rc = read_file(path, &p->raw, &p->size, MAX_PKG_BYTES);
    if (rc != 0) {
        if (!quiet) {
            if (rc == -2)
                printf("kpkg: %s: larger than %lu bytes\n", path,
                       MAX_PKG_BYTES);
            else if (rc == -3)
                printf("kpkg: out of memory reading %s\n", path);
            else
                printf("kpkg: cannot read %s\n", path);
        }
        return -1;
    }
    if (p->size < KPKG_HDR_SIZE + KPKG_DIGEST) {
        if (!quiet)
            printf("kpkg: %s: too small to be a package\n", path);
        pkg_free(p);
        return -1;
    }
    memcpy(&p->h, p->raw, KPKG_HDR_SIZE);
    if (memcmp(p->h.magic, "KPKG", 4) != 0) {
        if (!quiet)
            printf("kpkg: %s: not a .kpkg file (bad magic)\n", path);
        pkg_free(p);
        return -1;
    }
    if (p->h.version != KPKG_VERSION || p->h.header_size != KPKG_HDR_SIZE) {
        if (!quiet)
            printf("kpkg: %s: format version %u, this kpkg speaks %u\n",
                   path, (unsigned)p->h.version, KPKG_VERSION);
        pkg_free(p);
        return -1;
    }
    if (p->h.total_size != p->size) {
        if (!quiet)
            printf("kpkg: %s: header says %u bytes, file is %lu\n", path,
                   (unsigned)p->h.total_size, p->size);
        pkg_free(p);
        return -1;
    }

    body = p->size - KPKG_DIGEST;
    p->count = p->h.ftab_count;
    if (p->h.meta_off < KPKG_HDR_SIZE ||
        p->h.meta_off + p->h.meta_len > body ||
        p->h.ftab_off < KPKG_HDR_SIZE ||
        p->h.ftab_off + p->count * KPKG_ENT_SIZE > body ||
        p->h.data_off < KPKG_HDR_SIZE ||
        p->h.data_off + p->h.data_len > body) {
        if (!quiet)
            printf("kpkg: %s: corrupt (block outside the file)\n", path);
        pkg_free(p);
        return -1;
    }

    digest_of(p->raw, body, digest);
    if (memcmp(digest, p->raw + body, KPKG_DIGEST) != 0) {
        if (!quiet)
            printf("kpkg: %s: SHA-256 mismatch, refusing to use it\n", path);
        pkg_free(p);
        return -1;
    }

    {
        const char *meta = (const char *)(p->raw + p->h.meta_off);
        meta_get(meta, p->h.meta_len, "name", p->name, sizeof(p->name));
        meta_get(meta, p->h.meta_len, "version", p->version,
                 sizeof(p->version));
        meta_get(meta, p->h.meta_len, "description", p->desc, sizeof(p->desc));
        meta_get(meta, p->h.meta_len, "depends", p->depends,
                 sizeof(p->depends));
        meta_get(meta, p->h.meta_len, "arch", p->arch, sizeof(p->arch));
        meta_get(meta, p->h.meta_len, "size", p->instsize,
                 sizeof(p->instsize));
    }
    if (p->name[0] == '\0' || p->version[0] == '\0') {
        if (!quiet)
            printf("kpkg: %s: metadata has no name or version\n", path);
        pkg_free(p);
        return -1;
    }

    for (i = 0; i < p->count; i++) {
        struct kpkg_file e;
        pkg_entry(p, i, &e);
        if (!path_is_safe(e.path)) {
            if (!quiet)
                printf("kpkg: %s: unsafe path in package\n", path);
            pkg_free(p);
            return -1;
        }
        if (e.type == KPKG_TYPE_FILE &&
            (unsigned long)e.offset + e.size > p->h.data_len) {
            if (!quiet)
                printf("kpkg: %s: %s runs past the payload\n", path, e.path);
            pkg_free(p);
            return -1;
        }
    }
    return 0;
}

/* ---- installed-package database ----------------------------------------- */

struct dbfile {
    unsigned mode, uid, gid;
    unsigned long size;
    char sha[KPKG_DIGEST * 2 + 1];
    int type;
    char path[128];
};

static void db_dir_of(const char *name, char *out, unsigned long outsz)
{
    snprintf(out, outsz, "%s/%s", DB_DIR, name);
}

static int db_installed(const char *name)
{
    char path[PATH_MAX_];

    snprintf(path, sizeof(path), "%s/%s/files", DB_DIR, name);
    return exists(path);
}

static int db_meta(const char *name, const char *key, char *out,
                   unsigned long outsz)
{
    char path[PATH_MAX_];
    unsigned char *buf = 0;
    unsigned long len = 0;

    out[0] = '\0';
    snprintf(path, sizeof(path), "%s/%s/meta", DB_DIR, name);
    if (read_file(path, &buf, &len, 65536) != 0)
        return -1;
    meta_get((const char *)buf, len, key, out, outsz);
    free(buf);
    return out[0] ? 0 : -1;
}

static int db_parse_line(const char *line, struct dbfile *f)
{
    char field[64];
    const char *p = line;

    memset(f, 0, sizeof(*f));
    p = next_field(p, field, sizeof(field));
    if (!p)
        return -1;
    f->mode = parse_oct(field);
    p = next_field(p, field, sizeof(field));
    if (!p)
        return -1;
    f->uid = (unsigned)parse_dec(field);
    p = next_field(p, field, sizeof(field));
    if (!p)
        return -1;
    f->gid = (unsigned)parse_dec(field);
    p = next_field(p, field, sizeof(field));
    if (!p)
        return -1;
    f->size = parse_dec(field);
    p = next_field(p, f->sha, sizeof(f->sha));
    if (!p)
        return -1;
    p = next_field(p, field, sizeof(field));
    if (!p)
        return -1;
    f->type = field[0] == 'd' ? KPKG_TYPE_DIR : KPKG_TYPE_FILE;
    p = next_field(p, f->path, sizeof(f->path));
    if (!p)
        return -1;
    return 0;
}

/* Load a package's file list. Returns the count, or -1. */
static int db_files(const char *name, struct dbfile **out)
{
    char path[PATH_MAX_];
    unsigned char *buf = 0;
    unsigned long len = 0;
    struct dbfile *list;
    char *p;
    int n = 0, cap = 0;

    snprintf(path, sizeof(path), "%s/%s/files", DB_DIR, name);
    if (read_file(path, &buf, &len, 1024UL * 1024) != 0)
        return -1;

    for (p = (char *)buf; *p; p++)
        if (*p == '\n')
            cap++;
    cap += 2;
    list = malloc((unsigned long)cap * sizeof(*list));
    if (list == 0) {
        free(buf);
        return -1;
    }

    p = (char *)buf;
    while (*p && n < cap) {
        char *line = p;
        while (*p && *p != '\n')
            p++;
        if (*p == '\n')
            *p++ = '\0';
        trim(line);
        if (line[0] == '\0' || line[0] == '#')
            continue;
        if (db_parse_line(line, &list[n]) == 0)
            n++;
    }
    free(buf);
    *out = list;
    return n;
}

/* Call fn for every installed package name. fn returns 1 to stop. */
typedef int (*db_fn)(const char *name, void *ctx);

static int db_walk(db_fn fn, void *ctx)
{
    struct k_dirent de;
    int i = 0, stopped = 0;

    while (readdir_at(DB_DIR, i, &de) == 0) {
        i++;
        de.name[sizeof(de.name) - 1] = '\0';
        if (de.name[0] == '.' || !de.is_dir)
            continue;
        if (fn(de.name, ctx)) {
            stopped = 1;
            break;
        }
    }
    return stopped;
}

/* ---- ownership conflicts ------------------------------------------------ */

struct conflict_ctx {
    const struct pkg *p;
    const char *self;
    int conflicts;
    int report;
};

static int conflict_cb(const char *name, void *ctx)
{
    struct conflict_ctx *c = ctx;
    struct dbfile *list;
    int n, i;
    unsigned long j;

    if (strcmp(name, c->self) == 0)
        return 0;
    n = db_files(name, &list);
    if (n < 0)
        return 0;
    for (i = 0; i < n; i++) {
        if (list[i].type == KPKG_TYPE_DIR)
            continue;              /* directories are shared happily */
        for (j = 0; j < c->p->count; j++) {
            struct kpkg_file e;
            pkg_entry(c->p, j, &e);
            if (e.type == KPKG_TYPE_DIR)
                continue;
            if (strcmp(e.path, list[i].path) != 0)
                continue;
            c->conflicts++;
            if (c->report)
                printf("kpkg: %s is already owned by %s\n", e.path, name);
        }
    }
    free(list);
    return 0;
}

static int count_conflicts(const struct pkg *p, int report)
{
    struct conflict_ctx c;

    c.p = p;
    c.self = p->name;
    c.conflicts = 0;
    c.report = report;
    db_walk(conflict_cb, &c);
    return c.conflicts;
}

struct owner_ctx {
    const char *path;
    const char *skip;
    int found;
};

static int owner_cb(const char *name, void *ctx)
{
    struct owner_ctx *o = ctx;
    struct dbfile *list;
    int n, i;

    if (strcmp(name, o->skip) == 0)
        return 0;
    n = db_files(name, &list);
    if (n < 0)
        return 0;
    for (i = 0; i < n; i++) {
        if (strcmp(list[i].path, o->path) == 0) {
            o->found = 1;
            break;
        }
    }
    free(list);
    return o->found;
}

static int other_owner(const char *path, const char *skip)
{
    struct owner_ctx o;

    o.path = path;
    o.skip = skip;
    o.found = 0;
    db_walk(owner_cb, &o);
    return o.found;
}

/* ---- extraction ---------------------------------------------------------- */

static int extract(const struct pkg *p)
{
    unsigned char digest[KPKG_DIGEST];
    unsigned long i;

    for (i = 0; i < p->count; i++) {
        struct kpkg_file e;
        pkg_entry(p, i, &e);

        if (e.type == KPKG_TYPE_DIR) {
            if (mkdirs(e.path) < 0) {
                printf("kpkg: cannot create directory %s\n", e.path);
                return -1;
            }
        } else {
            digest_of(pkg_data(p, &e), e.size, digest);
            if (memcmp(digest, e.sha256, KPKG_DIGEST) != 0) {
                printf("kpkg: %s: payload digest mismatch\n", e.path);
                return -1;
            }
            if (mkdirs_for(e.path) < 0) {
                printf("kpkg: cannot create the parent of %s\n", e.path);
                return -1;
            }
            if (write_file(e.path, pkg_data(p, &e), e.size) < 0) {
                printf("kpkg: cannot write %s\n", e.path);
                return -1;
            }
        }
        /* Ownership first: chown on some systems clears mode bits, and
         * the package's mode must be what survives. */
        kp_chown(e.path, e.uid, e.gid);
        kp_chmod(e.path, e.mode);
    }
    return 0;
}

/* Write the database entry. Called only after extract() succeeded. */
static int db_write(const struct pkg *p)
{
    char dir[PATH_MAX_];
    char path[PATH_MAX_];
    char *text;
    unsigned long cap, used = 0, i;
    int rc;

    db_dir_of(p->name, dir, sizeof(dir));
    if (mkdirs(dir) < 0) {
        printf("kpkg: cannot create %s\n", dir);
        return -1;
    }

    cap = p->h.meta_len + 128;
    text = malloc(cap);
    if (text == 0)
        return -1;
    memcpy(text, p->raw + p->h.meta_off, p->h.meta_len);
    used = p->h.meta_len;
    used += (unsigned long)snprintf(text + used, cap - used,
                                    "installed: %lu\n", kp_time());
    snprintf(path, sizeof(path), "%s/meta", dir);
    rc = write_file(path, text, used);
    free(text);
    if (rc < 0) {
        printf("kpkg: cannot write %s\n", path);
        return -1;
    }

    /* Worst case per line: mode, three 10-digit numbers, 64 hex digits,
     * a type letter, a 127-byte path and the separators. */
    cap = p->count * 256 + 128;
    text = malloc(cap);
    if (text == 0)
        return -1;
    used = 0;
    for (i = 0; i < p->count; i++) {
        struct kpkg_file e;
        char mode[8], sha[KPKG_DIGEST * 2 + 1];
        pkg_entry(p, i, &e);
        oct_str(e.mode, mode);
        if (e.type == KPKG_TYPE_DIR)
            snprintf(sha, sizeof(sha), "-");
        else
            hex_str(e.sha256, KPKG_DIGEST, sha);
        used += (unsigned long)snprintf(text + used, cap - used,
                                        "%s %u %u %u %s %s %s\n", mode,
                                        (unsigned)e.uid, (unsigned)e.gid,
                                        (unsigned)e.size, sha,
                                        e.type == KPKG_TYPE_DIR ? "d" : "f",
                                        e.path);
        if (used >= cap) {
            free(text);
            printf("kpkg: file list too large\n");
            return -1;
        }
    }
    snprintf(path, sizeof(path), "%s/files", dir);
    rc = write_file(path, text, used);
    free(text);
    if (rc < 0) {
        printf("kpkg: cannot write %s\n", path);
        return -1;
    }
    /* Only root reads or writes the database. */
    kp_chown(dir, 0, 0);
    kp_chmod(dir, 0700);
    return 0;
}

/* Drop files the previous version owned that the new one does not. */
static void prune_old(const char *name, const struct pkg *p)
{
    struct dbfile *old;
    int n = db_files(name, &old), i;
    unsigned long j;

    if (n < 0)
        return;
    for (i = n - 1; i >= 0; i--) {
        int still = 0;
        for (j = 0; j < p->count; j++) {
            struct kpkg_file e;
            pkg_entry(p, j, &e);
            if (strcmp(e.path, old[i].path) == 0) {
                still = 1;
                break;
            }
        }
        if (still || other_owner(old[i].path, name))
            continue;
        unlink_(old[i].path);          /* directories go only if empty */
    }
    free(old);
}

/* ---- install ------------------------------------------------------------- */

struct instate {
    char stack[MAX_DEPTH][NAME_MAX_];
    int depth;
    int force;
    int installed;
};

static int install_by_name(struct instate *st, const char *name);

static int stack_has(const struct instate *st, const char *name)
{
    int i;

    for (i = 0; i < st->depth; i++)
        if (strcmp(st->stack[i], name) == 0)
            return 1;
    return 0;
}

/* Install a package that is already on disk. `want_sha` is the digest
 * the repository index promised, or 0 for a local file. */
static int install_file(struct instate *st, const char *file,
                        const char *want_sha)
{
    struct pkg p;
    char dep[NAME_MAX_];
    const char *d;
    int was_installed;

    if (pkg_load(file, &p, 0) < 0)
        return -1;

    if (want_sha && want_sha[0]) {
        unsigned char digest[KPKG_DIGEST];
        char hex[KPKG_DIGEST * 2 + 1];
        digest_of(p.raw, p.size - KPKG_DIGEST, digest);
        hex_str(digest, KPKG_DIGEST, hex);
        if (strcmp(hex, want_sha) != 0) {
            printf("kpkg: %s: does not match the digest in the index\n",
                   p.name);
            pkg_free(&p);
            return -1;
        }
    }

    if (stack_has(st, p.name)) {
        printf("kpkg: dependency cycle involving %s\n", p.name);
        pkg_free(&p);
        return -1;
    }
    was_installed = db_installed(p.name);
    if (was_installed && !st->force) {
        char have[32];
        db_meta(p.name, "version", have, sizeof(have));
        printf("kpkg: %s %s is already installed (--force to reinstall)\n",
               p.name, have[0] ? have : "?");
        pkg_free(&p);
        return 0;
    }

    /* Dependencies first, so a package is never live before them. */
    if (st->depth >= MAX_DEPTH) {
        printf("kpkg: dependency chain deeper than %d\n", MAX_DEPTH);
        pkg_free(&p);
        return -1;
    }
    snprintf(st->stack[st->depth], NAME_MAX_, "%s", p.name);
    st->depth++;

    d = p.depends;
    while (*d) {
        unsigned long n = 0;
        while (*d == ' ' || *d == ',')
            d++;
        while (*d && *d != ',' && n + 1 < sizeof(dep))
            dep[n++] = *d++;
        while (*d && *d != ',')          /* an over-long name cannot stall */
            d++;
        while (n > 0 && dep[n - 1] == ' ')
            n--;
        dep[n] = '\0';
        if (n == 0)
            continue;
        if (db_installed(dep))
            continue;
        printf("kpkg: %s requires %s\n", p.name, dep);
        if (install_by_name(st, dep) < 0) {
            st->depth--;
            pkg_free(&p);
            return -1;
        }
    }
    st->depth--;

    if (count_conflicts(&p, 1) > 0 && !st->force) {
        printf("kpkg: refusing to overwrite another package's files "
               "(--force overrides)\n");
        pkg_free(&p);
        return -1;
    }

    if (extract(&p) < 0) {
        printf("kpkg: %s: extraction failed, nothing recorded\n", p.name);
        pkg_free(&p);
        return -1;
    }
    if (was_installed)
        prune_old(p.name, &p);
    if (db_write(&p) < 0) {
        pkg_free(&p);
        return -1;
    }

    printf("kpkg: installed %s %s (%lu files)\n", p.name, p.version, p.count);
    st->installed++;
    pkg_free(&p);
    return 0;
}

/* Fetch <repo>/<file> into the cache and return its local path. */
static int fetch_to_cache(const char *file, char *out, unsigned long outsz)
{
    char url[HTTP_URL_MAX];
    char *body = 0;
    unsigned long len = 0;
    int status = 0, rc;

    if (mkdirs(g_conf.cache) < 0) {
        printf("kpkg: cannot create %s\n", g_conf.cache);
        return -1;
    }
    snprintf(url, sizeof(url), "%s/%s", g_conf.repo, file);
    printf("kpkg: fetching %s\n", url);
    rc = http_get(url, &body, &len, &status);
    if (rc != HTTP_OK) {
        printf("kpkg: %s: %s\n", url, http_strerror(rc));
        return -1;
    }
    if (status != 200) {
        printf("kpkg: %s: HTTP %d %s\n", url, status,
               http_status_text(status));
        free(body);
        return -1;
    }
    snprintf(out, outsz, "%s/%s", g_conf.cache, file);
    if (write_file(out, body, len) < 0) {
        printf("kpkg: cannot write %s\n", out);
        free(body);
        return -1;
    }
    free(body);
    printf("kpkg: %lu bytes -> %s\n", len, out);
    return 0;
}

static int install_by_name(struct instate *st, const char *name)
{
    struct idxent e;
    char file[PATH_MAX_];

    if (idx_find(name, &e, 0) < 0) {
        printf("kpkg: %s: not in the repository index\n", name);
        return -1;
    }
    if (repo_is_https()) {
        printf("kpkg: %s\n", http_strerror(HTTP_EHTTPS));
        return -1;
    }
    if (repo_is_http()) {
        if (fetch_to_cache(e.file, file, sizeof(file)) < 0)
            return -1;
    } else {
        snprintf(file, sizeof(file), "%s/%s", g_conf.repo, e.file);
    }
    return install_file(st, file, e.sha);
}

/* ---- commands ------------------------------------------------------------ */

static const char *g_cwd = "/";

static void resolve_path(const char *tok, char *out, unsigned long outsz)
{
    if (tok[0] == '/')
        snprintf(out, outsz, "%s", tok);
    else if (g_cwd[0] == '/' && g_cwd[1] == '\0')
        snprintf(out, outsz, "/%s", tok);
    else
        snprintf(out, outsz, "%s/%s", g_cwd, tok);
}

static int need_root(const char *what)
{
    if (kp_getuid() != 0) {
        printf("kpkg: %s needs root\n", what);
        return -1;
    }
    return 0;
}

static int looks_like_file(const char *arg)
{
    unsigned long n = strlen(arg);

    if (n > 5 && strcmp(arg + n - 5, ".kpkg") == 0)
        return 1;
    return strchr(arg, '/') != 0;
}

static int cmd_install(int argc, char **argv, int force)
{
    struct instate st;
    int i, rc = 0, any = 0;

    if (need_root("install") < 0)
        return 1;
    memset(&st, 0, sizeof(st));
    st.force = force;

    /* The database lives here whether or not anything is installed. */
    mkdirs(DB_DIR);

    for (i = 0; i < argc; i++) {
        if (strncmp(argv[i], "--", 2) == 0)
            continue;
        any = 1;
        st.depth = 0;
        if (looks_like_file(argv[i])) {
            char path[PATH_MAX_];
            resolve_path(argv[i], path, sizeof(path));
            if (install_file(&st, path, 0) < 0)
                rc = 1;
        } else if (install_by_name(&st, argv[i]) < 0) {
            rc = 1;
        }
    }
    if (!any) {
        printf("usage: kpkg install [--force] <name|file.kpkg>...\n");
        return 1;
    }
    return rc;
}

static int cmd_remove(int argc, char **argv)
{
    char dir[PATH_MAX_];
    char path[PATH_MAX_];
    struct dbfile *list;
    const char *name;
    int n, i, kept = 0, gone = 0;

    if (need_root("remove") < 0)
        return 1;
    if (argc < 1) {
        printf("usage: kpkg remove <name>\n");
        return 1;
    }
    name = argv[0];
    if (!db_installed(name)) {
        printf("kpkg: %s is not installed\n", name);
        return 1;
    }
    n = db_files(name, &list);
    if (n < 0) {
        printf("kpkg: cannot read the file list for %s\n", name);
        return 1;
    }

    /* Reverse order so a directory is tried after its contents. */
    for (i = n - 1; i >= 0; i--) {
        if (other_owner(list[i].path, name)) {
            if (list[i].type != KPKG_TYPE_DIR) {
                printf("kpkg: keeping %s (owned by another package too)\n",
                       list[i].path);
                kept++;
            }
            continue;
        }
        if (!exists(list[i].path))
            continue;
        if (unlink_(list[i].path) == 0)
            gone++;
        else if (list[i].type != KPKG_TYPE_DIR)
            printf("kpkg: cannot remove %s\n", list[i].path);
    }
    free(list);

    db_dir_of(name, dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/files", dir);
    unlink_(path);
    snprintf(path, sizeof(path), "%s/meta", dir);
    unlink_(path);
    unlink_(dir);

    printf("kpkg: removed %s (%d files", name, gone);
    if (kept)
        printf(", %d kept", kept);
    printf(")\n");
    return 0;
}

struct list_ctx {
    int count;
};

static int list_cb(const char *name, void *ctx)
{
    struct list_ctx *l = ctx;
    char version[32], desc[192];

    if (!db_installed(name))
        return 0;
    db_meta(name, "version", version, sizeof(version));
    db_meta(name, "description", desc, sizeof(desc));
    printf("%-18s %-10s %s\n", name, version[0] ? version : "?", desc);
    l->count++;
    return 0;
}

static int cmd_list(void)
{
    struct list_ctx l;

    l.count = 0;
    if (!is_dir(DB_DIR)) {
        printf("kpkg: no packages installed\n");
        return 0;
    }
    db_walk(list_cb, &l);
    if (l.count == 0)
        printf("kpkg: no packages installed\n");
    else
        printf("%d package%s installed\n", l.count, l.count == 1 ? "" : "s");
    return 0;
}

static int cmd_info(int argc, char **argv)
{
    struct dbfile *list;
    struct idxent e;
    const char *name;
    char value[192];
    int n, i;
    static const char *const keys[] = {
        "name", "version", "arch", "description", "depends", "maintainer",
        "size", "files", "installed", 0
    };

    if (argc < 1) {
        printf("usage: kpkg info <name>\n");
        return 1;
    }
    name = argv[0];

    if (!db_installed(name)) {
        if (idx_find(name, &e, 1) == 0) {
            printf("name:        %s\n", e.name);
            printf("version:     %s\n", e.version);
            printf("description: %s\n", e.desc);
            printf("depends:     %s\n", e.depends[0] ? e.depends : "-");
            printf("file:        %s (%lu bytes)\n", e.file, e.size);
            printf("sha256:      %s\n", e.sha);
            printf("status:      available, not installed\n");
            return 0;
        }
        printf("kpkg: %s is not installed and not in the index\n", name);
        return 1;
    }

    for (i = 0; keys[i]; i++) {
        if (db_meta(name, keys[i], value, sizeof(value)) == 0)
            printf("%-12s %s\n", keys[i], value);
    }
    n = db_files(name, &list);
    if (n < 0) {
        printf("kpkg: cannot read the file list\n");
        return 1;
    }
    printf("\n%-6s %-5s %-5s %10s %s\n", "mode", "uid", "gid", "size", "path");
    for (i = 0; i < n; i++) {
        char mode[8];
        oct_str(list[i].mode, mode);
        printf("%s%s %-5u %-5u %10lu %s\n",
               list[i].type == KPKG_TYPE_DIR ? "d" : "-", mode + 1,
               list[i].uid, list[i].gid, list[i].size, list[i].path);
    }
    free(list);
    return 0;
}

struct search_ctx {
    const char *text;
    int hits;
};

static int search_cb(const struct idxent *e, void *ctx)
{
    struct search_ctx *s = ctx;

    if (!ci_contains(e->name, s->text) && !ci_contains(e->desc, s->text))
        return 0;
    printf("%-18s %-10s %s%s\n", e->name, e->version, e->desc,
           db_installed(e->name) ? "  [installed]" : "");
    s->hits++;
    return 0;
}

static int cmd_search(int argc, char **argv)
{
    struct search_ctx s;

    if (argc < 1) {
        printf("usage: kpkg search <text>\n");
        return 1;
    }
    s.text = argv[0];
    s.hits = 0;
    if (idx_walk(search_cb, &s, 0) < 0)
        return 1;
    if (s.hits == 0) {
        printf("kpkg: nothing matches '%s'\n", s.text);
        return 1;
    }
    return 0;
}

struct verify_ctx {
    int bad;
    int checked;
};

static int verify_one(const char *name, struct verify_ctx *v)
{
    struct dbfile *list;
    int n, i, bad = 0;

    n = db_files(name, &list);
    if (n < 0) {
        printf("kpkg: %s: cannot read the file list\n", name);
        return -1;
    }
    for (i = 0; i < n; i++) {
        unsigned char digest[KPKG_DIGEST];
        char hex[KPKG_DIGEST * 2 + 1];
        unsigned long size = 0;
        struct k_stat st;

        if (list[i].type == KPKG_TYPE_DIR) {
            if (!is_dir(list[i].path)) {
                printf("MISSING  %s (directory)\n", list[i].path);
                bad++;
            }
            continue;
        }
        if (stat_(list[i].path, &st) < 0) {
            printf("MISSING  %s\n", list[i].path);
            bad++;
            continue;
        }
        if (digest_of_file(list[i].path, digest, &size) < 0) {
            printf("UNREAD   %s\n", list[i].path);
            bad++;
            continue;
        }
        hex_str(digest, KPKG_DIGEST, hex);
        if (size != list[i].size || strcmp(hex, list[i].sha) != 0) {
            printf("MODIFIED %s\n", list[i].path);
            bad++;
            continue;
        }
        if (st.mode != list[i].mode || st.uid != list[i].uid ||
            st.gid != list[i].gid) {
            char want[8], have[8];
            oct_str(list[i].mode, want);
            oct_str(st.mode, have);
            printf("CHANGED  %s (mode/owner %s %u:%u, expected %s %u:%u)\n",
                   list[i].path, have, st.uid, st.gid, want, list[i].uid,
                   list[i].gid);
            bad++;
        }
    }
    free(list);
    v->checked++;
    v->bad += bad;
    printf("%-18s %s (%d file%s)\n", name, bad ? "FAILED" : "ok", n,
           n == 1 ? "" : "s");
    return bad ? -1 : 0;
}

static int verify_cb(const char *name, void *ctx)
{
    if (db_installed(name))
        verify_one(name, ctx);
    return 0;
}

static int cmd_verify(int argc, char **argv)
{
    struct verify_ctx v;

    v.bad = 0;
    v.checked = 0;
    if (argc >= 1) {
        if (!db_installed(argv[0])) {
            printf("kpkg: %s is not installed\n", argv[0]);
            return 1;
        }
        verify_one(argv[0], &v);
    } else {
        db_walk(verify_cb, &v);
        if (v.checked == 0) {
            printf("kpkg: no packages installed\n");
            return 0;
        }
    }
    if (v.bad) {
        printf("kpkg: %d problem%s found\n", v.bad, v.bad == 1 ? "" : "s");
        return 1;
    }
    printf("kpkg: everything matches\n");
    return 0;
}

struct count_ctx {
    int n;
};

static int count_cb(const struct idxent *e, void *ctx)
{
    struct count_ctx *c = ctx;

    (void)e;
    c->n++;
    return 0;
}

static int cmd_update(void)
{
    char url[HTTP_URL_MAX];
    char path[PATH_MAX_];
    char *body = 0;
    unsigned long len = 0;
    struct count_ctx c;
    int status = 0, rc;

    if (need_root("update") < 0)
        return 1;

    if (repo_is_https()) {
        printf("kpkg: %s\n", http_strerror(HTTP_EHTTPS));
        return 1;
    }
    if (!repo_is_http()) {
        index_path(path, sizeof(path));
        if (!exists(path)) {
            printf("kpkg: local repository %s has no %s\n", g_conf.repo,
                   INDEX_NAME);
            return 1;
        }
        c.n = 0;
        idx_walk(count_cb, &c, 0);
        printf("kpkg: local repository %s, %d package%s\n", g_conf.repo, c.n,
               c.n == 1 ? "" : "s");
        return 0;
    }

    snprintf(url, sizeof(url), "%s/%s", g_conf.repo, INDEX_NAME);
    printf("kpkg: fetching %s\n", url);
    rc = http_get(url, &body, &len, &status);
    if (rc != HTTP_OK) {
        printf("kpkg: %s: %s\n", url, http_strerror(rc));
        return 1;
    }
    if (status != 200) {
        printf("kpkg: %s: HTTP %d %s\n", url, status,
               http_status_text(status));
        free(body);
        return 1;
    }
    if (mkdirs(PKG_DIR) < 0 || write_file(INDEX_CACHE, body, len) < 0) {
        printf("kpkg: cannot write %s\n", INDEX_CACHE);
        free(body);
        return 1;
    }
    free(body);
    kp_chmod(INDEX_CACHE, 0644);
    c.n = 0;
    idx_walk(count_cb, &c, 0);
    printf("kpkg: index updated, %d package%s (%lu bytes)\n", c.n,
           c.n == 1 ? "" : "s", len);
    return 0;
}

static void usage(void)
{
    printf("usage: kpkg <command> [args]\n"
           "  install [--force] <name|file.kpkg>...  install packages\n"
           "  remove <name>                          remove a package\n"
           "  list                                   installed packages\n"
           "  info <name>                            metadata and files\n"
           "  search <text>                          search the index\n"
           "  verify [name]                          re-hash installed files\n"
           "  update                                 refresh the index\n"
           "\nrepository: %s\n", g_conf.repo);
}

/* Pull the shell-injected "--cwd=<path>" argument, wherever it sits. */
static int strip_cwd_arg(int argc, char **argv)
{
    int i, out = 1;

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--cwd=", 6) == 0)
            g_cwd = argv[i] + 6;
        else
            argv[out++] = argv[i];
    }
    return out;
}

int main(int argc, char **argv)
{
    const char *cmd;
    int force = 0, i, out = 1;

    argc = strip_cwd_arg(argc, argv);
    conf_load();

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--force") == 0 || strcmp(argv[i], "-f") == 0)
            force = 1;
        else
            argv[out++] = argv[i];
    }
    argc = out;

    if (argc < 2) {
        usage();
        return 1;
    }
    cmd = argv[1];

    if (strcmp(cmd, "install") == 0)
        return cmd_install(argc - 2, argv + 2, force);
    if (strcmp(cmd, "remove") == 0)
        return cmd_remove(argc - 2, argv + 2);
    if (strcmp(cmd, "list") == 0)
        return cmd_list();
    if (strcmp(cmd, "info") == 0)
        return cmd_info(argc - 2, argv + 2);
    if (strcmp(cmd, "search") == 0)
        return cmd_search(argc - 2, argv + 2);
    if (strcmp(cmd, "verify") == 0)
        return cmd_verify(argc - 2, argv + 2);
    if (strcmp(cmd, "update") == 0)
        return cmd_update();
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "-h") == 0) {
        usage();
        return 0;
    }

    printf("kpkg: unknown command '%s'\n", cmd);
    usage();
    return 1;
}
