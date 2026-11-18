/* pick.c - project columns from a @krec stream on stdin.
 *
 * usage: pick [-r|-c|-t] [-H] [col1,col2,... | col1 col2 ... | *]
 *   (default)   aligned human table
 *   -r, --krec  preserve @krec stream format
 *   -c, --csv   CSV output
 *   -t, --table aligned human table (explicit)
 *   -H          omit header row in table/CSV mode
 */

#include <kestrel.h>
#include <krec.h>
#include <stdlib.h>
#include <string.h>

#define PICK_MAX_ROWS 16384

enum out_mode {
    MODE_TABLE = 0,
    MODE_KREC,
    MODE_CSV
};

struct buf_row {
    char *blob;
    const char *fields[KREC_MAX_COLS];
};

static void usage(void)
{
    krec_err("usage: pick [-r|-c|-t] [-H] [col1,col2,... | col1 col2 ...]\n");
}

static void write_csv_cell(const char *s)
{
    const char *p;
    int need_quote = 0;

    for (p = s; *p; p++) {
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') {
            need_quote = 1;
            break;
        }
    }
    if (!need_quote) {
        write(1, s, strlen(s));
        return;
    }
    write(1, "\"", 1);
    for (p = s; *p; p++) {
        if (*p == '"')
            write(1, "\"\"", 2);
        else
            write(1, p, 1);
    }
    write(1, "\"", 1);
}

static void write_csv_row(int ncols, const char *const *vals)
{
    int i;

    for (i = 0; i < ncols; i++) {
        if (i > 0)
            write(1, ",", 1);
        write_csv_cell(vals[i] ? vals[i] : "");
    }
    write(1, "\n", 1);
}

static int add_col_spec(const struct krec_schema *in, const char *spec,
                        int *sel, struct krec_schema *out)
{
    char buf[256];
    char *p, *tok;

    if (strlen(spec) >= sizeof(buf)) {
        krec_err("pick: column list too long\n");
        return -1;
    }
    strcpy(buf, spec);
    p = buf;
    while (*p) {
        char *comma = strchr(p, ',');
        if (comma)
            *comma = '\0';
        tok = p;
        if (!tok[0]) {
            krec_err("pick: empty column name\n");
            return -1;
        }
        if (strcmp(tok, "*") == 0) {
            int j;
            for (j = 0; j < in->ncols; j++) {
                if (out->ncols >= KREC_MAX_COLS) {
                    krec_err("pick: too many columns (max %d)\n",
                             KREC_MAX_COLS);
                    return -1;
                }
                sel[out->ncols] = j;
                out->cols[out->ncols] = in->cols[j];
                out->ncols++;
            }
        } else {
            int idx = krec_find_col(in, tok);
            if (idx < 0) {
                krec_err("pick: unknown column '%s'\n", tok);
                return -1;
            }
            if (out->ncols >= KREC_MAX_COLS) {
                krec_err("pick: too many columns (max %d)\n", KREC_MAX_COLS);
                return -1;
            }
            sel[out->ncols] = idx;
            out->cols[out->ncols] = in->cols[idx];
            out->ncols++;
        }
        if (!comma)
            break;
        p = comma + 1;
    }
    return 0;
}

static void free_buf_rows(struct buf_row *rows, int n)
{
    int i;

    for (i = 0; i < n; i++)
        free(rows[i].blob);
    free(rows);
}

int main(int argc, char **argv)
{
    struct krec_reader r;
    struct krec_schema in_schema, out_schema;
    char line[KREC_LINE_MAX];
    char *fields[KREC_MAX_COLS];
    const char *proj[KREC_MAX_COLS];
    const char *specs[KREC_MAX_COLS];
    int sel[KREC_MAX_COLS];
    int nspecs = 0, no_header = 0, hdr, rc, i;
    enum out_mode mode = MODE_TABLE;

    argc = krec_strip_cwd(argc, argv, 0);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--krec") == 0 || strcmp(argv[i], "--raw") == 0) {
            mode = MODE_KREC;
        } else if (strcmp(argv[i], "--csv") == 0) {
            mode = MODE_CSV;
        } else if (strcmp(argv[i], "--table") == 0) {
            mode = MODE_TABLE;
        } else if (strcmp(argv[i], "--no-header") == 0) {
            no_header = 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            int j;
            for (j = 1; argv[i][j]; j++) {
                if (argv[i][j] == 'r')
                    mode = MODE_KREC;
                else if (argv[i][j] == 'c')
                    mode = MODE_CSV;
                else if (argv[i][j] == 't')
                    mode = MODE_TABLE;
                else if (argv[i][j] == 'H')
                    no_header = 1;
                else if (argv[i][j] == 'h') {
                    usage();
                    return 0;
                } else {
                    usage();
                    return 1;
                }
            }
        } else {
            if (nspecs >= KREC_MAX_COLS) {
                krec_err("pick: too many column arguments\n");
                return 1;
            }
            specs[nspecs++] = argv[i];
        }
    }

    krec_reader_init(&r, 0);
    hdr = krec_read_header(&r, &in_schema);
    if (hdr == 0)
        return 0;
    if (hdr < 0) {
        krec_err("pick: invalid @krec header on stdin\n");
        return 1;
    }

    memset(&out_schema, 0, sizeof(out_schema));
    if (nspecs == 0) {
        if (add_col_spec(&in_schema, "*", sel, &out_schema) < 0)
            return 1;
    } else {
        for (i = 0; i < nspecs; i++) {
            if (add_col_spec(&in_schema, specs[i], sel, &out_schema) < 0)
                return 1;
        }
    }

    if (mode == MODE_KREC) {
        if (krec_begin_schema(1, &out_schema) < 0)
            return 1;
        while ((rc = krec_next(&r, line, sizeof(line),
                               fields, KREC_MAX_COLS)) == 1) {
            for (i = 0; i < out_schema.ncols; i++)
                proj[i] = fields[sel[i]];
            if (krec_row_vals(1, out_schema.ncols, proj) < 0)
                return 1;
        }
        if (rc < 0) {
            krec_err("pick: malformed @krec record on stdin\n");
            return 1;
        }
        return 0;
    }

    if (mode == MODE_CSV) {
        if (!no_header) {
            for (i = 0; i < out_schema.ncols; i++)
                proj[i] = out_schema.cols[i].name;
            write_csv_row(out_schema.ncols, proj);
        }
        while ((rc = krec_next(&r, line, sizeof(line),
                               fields, KREC_MAX_COLS)) == 1) {
            for (i = 0; i < out_schema.ncols; i++)
                proj[i] = fields[sel[i]];
            write_csv_row(out_schema.ncols, proj);
        }
        if (rc < 0) {
            krec_err("pick: malformed @krec record on stdin\n");
            return 1;
        }
        return 0;
    }

    {
        struct buf_row *rows = 0;
        const char *const **row_ptrs = 0;
        int nrows = 0, cap = 0;

        while ((rc = krec_next(&r, line, sizeof(line),
                               fields, KREC_MAX_COLS)) == 1) {
            size_t total = 0, off = 0;
            char *blob;

            if (nrows >= PICK_MAX_ROWS) {
                krec_err("pick: row limit (%d) exceeded\n", PICK_MAX_ROWS);
                free_buf_rows(rows, nrows);
                return 1;
            }
            if (nrows >= cap) {
                int ncap = cap ? cap * 2 : 64;
                struct buf_row *nr;
                if (ncap > PICK_MAX_ROWS)
                    ncap = PICK_MAX_ROWS;
                nr = (struct buf_row *)realloc(rows,
                                               (unsigned long)ncap * sizeof(*nr));
                if (!nr) {
                    krec_err("pick: out of memory\n");
                    free_buf_rows(rows, nrows);
                    return 1;
                }
                rows = nr;
                cap = ncap;
            }

            for (i = 0; i < out_schema.ncols; i++)
                total += strlen(fields[sel[i]]) + 1;
            blob = (char *)malloc(total ? total : 1);
            if (!blob) {
                krec_err("pick: out of memory\n");
                free_buf_rows(rows, nrows);
                return 1;
            }
            rows[nrows].blob = blob;
            for (i = 0; i < out_schema.ncols; i++) {
                const char *src = fields[sel[i]];
                size_t len = strlen(src) + 1;
                size_t k;
                memcpy(blob + off, src, len);
                for (k = 0; k + 1 < len; k++) {
                    if (blob[off + k] == '\t' || blob[off + k] == '\n' ||
                        blob[off + k] == '\r')
                        blob[off + k] = ' ';
                }
                rows[nrows].fields[i] = blob + off;
                off += len;
            }
            nrows++;
        }

        if (rc < 0) {
            krec_err("pick: malformed @krec record on stdin\n");
            free_buf_rows(rows, nrows);
            return 1;
        }

        if (nrows > 0) {
            row_ptrs = (const char *const **)malloc(
                (unsigned long)nrows * sizeof(*row_ptrs));
            if (!row_ptrs) {
                krec_err("pick: out of memory\n");
                free_buf_rows(rows, nrows);
                return 1;
            }
            for (i = 0; i < nrows; i++)
                row_ptrs[i] = rows[i].fields;
        }

        krec_print_table(1, &out_schema, row_ptrs, nrows, !no_header);
        free(row_ptrs);
        free_buf_rows(rows, nrows);
    }
    return 0;
}
