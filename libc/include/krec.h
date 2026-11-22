#pragma once

/* KestrelOS `@krec` typed structured record stream format and helpers.
 *
 * Wire format:
 *   Line 1:   @krec <col1>:<type1> <col2>:<type2> ...\n
 *   Line 2+:  <val1>\t<val2>\t...\n
 *
 * Supported types: str, int, u64, hex, bytes, ip, bool.
 * Column names are [a-zA-Z0-9_]. Field values escape '\', '\t', '\n', '\r'
 * as '\\', '\t', '\n', '\r' on write and unescape on read.
 */

#include <stddef.h>
#include <stdint.h>

#define KREC_MAX_COLS 32
#define KREC_NAME_MAX 32
#define KREC_LINE_MAX 4096

enum krec_type {
    KREC_STR = 0,
    KREC_INT,
    KREC_U64,
    KREC_HEX,
    KREC_BYTES,
    KREC_IP,
    KREC_BOOL
};

struct krec_col {
    char name[KREC_NAME_MAX];
    enum krec_type type;
};

struct krec_schema {
    int ncols;
    struct krec_col cols[KREC_MAX_COLS];
};

struct krec_reader {
    int fd;
    int tty;
    int eof;
    int err;
    int pos;
    int len;
    int has_schema;
    struct krec_schema schema;
    char buf[512];
};

/* Type metadata */
const char *krec_type_name(enum krec_type t);
int krec_parse_type(const char *s, enum krec_type *out);
int krec_is_numeric_type(enum krec_type t);

/* Writer API (uses raw Kestrel file descriptors; pass fd=1 for stdout) */
int krec_begin(int fd, const struct krec_col *cols, int ncols);
int krec_begin_schema(int fd, const struct krec_schema *s);
int krec_escape_fd(int fd, const char *s);
int krec_escape_print(const char *s);
int krec_row_vals(int fd, int ncols, const char *const *vals);
int krec_row(int fd, int ncols, ...);

/* Reader & schema API */
int krec_parse_header(const char *line, struct krec_schema *s);
int krec_find_col(const struct krec_schema *s, const char *name);
void krec_reader_init(struct krec_reader *r, int fd);
int krec_read_header(struct krec_reader *r, struct krec_schema *out);
/* Returns 1 per row, 0 on EOF, -1 on non-krec or parse error. */
int krec_next(struct krec_reader *r, char *buf, size_t bufsz,
              char **fields, int max_fields);

/* Typed value parsers & comparison */
int krec_parse_i64(const char *s, int64_t *out);
int krec_parse_u64(const char *s, uint64_t *out);
int krec_parse_hex(const char *s, uint64_t *out);
int krec_parse_bytes(const char *s, uint64_t *out);
int krec_parse_bool(const char *s, int *out);
int krec_parse_ip(const char *s, uint32_t *out_host);
int krec_compare(enum krec_type t, const char *a, const char *b,
                 int force_numeric, int icase);

/* Aligned table printer */
void krec_print_table(int fd, const struct krec_schema *s,
                      const char *const *const *rows, int nrows,
                      int show_header);

/* Utility helpers for pipeline and CLI apps */
int krec_strcasecmp(const char *a, const char *b);
const char *krec_strcasestr(const char *hay, const char *needle);
void krec_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
