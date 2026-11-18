/* sortby.c - sort @krec records on stdin by a named column.
 *
 * usage: sortby [-r] [-n] [-i] <field>
 */

#include <kestrel.h>
#include <krec.h>
#include <stdlib.h>
#include <string.h>

#define SORT_MAX_ROWS 16384

struct sort_row {
    char *blob;
    char *fields[KREC_MAX_COLS];
};

static void usage(void)
{
    krec_err("usage: sortby [-r] [-n] [-i] <field>\n");
}

static int row_cmp(const struct sort_row *a, const struct sort_row *b,
                   int col, enum krec_type t, int force_num,
                   int icase, int reverse)
{
    int c = krec_compare(t, a->fields[col], b->fields[col], force_num, icase);
    return reverse ? -c : c;
}

/* Stable bottom-up iterative merge sort (avoids stack recursion). */
static int sort_rows(struct sort_row *rows, int n, int col, enum krec_type t,
                     int force_num, int icase, int reverse)
{
    struct sort_row *tmp;
    int width, i;

    if (n <= 1)
        return 0;
    tmp = (struct sort_row *)malloc((unsigned long)n * sizeof(*tmp));
    if (!tmp)
        return -1;

    for (width = 1; width < n; width *= 2) {
        for (i = 0; i < n; i += 2 * width) {
            int mid = i + width;
            int end = i + 2 * width;
            int l = i, r, k = i;

            if (mid > n)
                mid = n;
            if (end > n)
                end = n;
            r = mid;

            while (l < mid && r < end) {
                if (row_cmp(&rows[l], &rows[r], col, t, force_num,
                            icase, reverse) <= 0)
                    tmp[k++] = rows[l++];
                else
                    tmp[k++] = rows[r++];
            }
            while (l < mid)
                tmp[k++] = rows[l++];
            while (r < end)
                tmp[k++] = rows[r++];
        }
        memcpy(rows, tmp, (unsigned long)n * sizeof(*rows));
    }

    free(tmp);
    return 0;
}

static void free_rows(struct sort_row *rows, int n)
{
    int i;

    for (i = 0; i < n; i++)
        free(rows[i].blob);
    free(rows);
}

int main(int argc, char **argv)
{
    struct krec_reader r;
    struct krec_schema schema;
    char line[KREC_LINE_MAX];
    char *fields[KREC_MAX_COLS];
    const char *field = 0;
    struct sort_row *rows = 0;
    int nrows = 0, cap = 0;
    int reverse = 0, force_num = 0, icase = 0;
    int col, hdr, rc, i;

    argc = krec_strip_cwd(argc, argv, 0);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            int j;
            for (j = 1; argv[i][j]; j++) {
                if (argv[i][j] == 'r')
                    reverse = 1;
                else if (argv[i][j] == 'n')
                    force_num = 1;
                else if (argv[i][j] == 'i')
                    icase = 1;
                else if (argv[i][j] == 'h') {
                    usage();
                    return 0;
                } else {
                    usage();
                    return 1;
                }
            }
        } else if (!field) {
            field = argv[i];
        } else {
            usage();
            return 1;
        }
    }

    if (!field) {
        usage();
        return 1;
    }

    krec_reader_init(&r, 0);
    hdr = krec_read_header(&r, &schema);
    if (hdr == 0)
        return 0;
    if (hdr < 0) {
        krec_err("sortby: invalid @krec header on stdin\n");
        return 1;
    }

    col = krec_find_col(&schema, field);
    if (col < 0) {
        krec_err("sortby: unknown field '%s'\n", field);
        return 1;
    }

    while ((rc = krec_next(&r, line, sizeof(line), fields, KREC_MAX_COLS)) == 1) {
        size_t total = 0, off = 0;
        char *blob;

        if (nrows >= SORT_MAX_ROWS) {
            krec_err("sortby: row limit (%d) exceeded\n", SORT_MAX_ROWS);
            free_rows(rows, nrows);
            return 1;
        }
        if (nrows >= cap) {
            int ncap = cap ? cap * 2 : 64;
            struct sort_row *nr;
            if (ncap > SORT_MAX_ROWS)
                ncap = SORT_MAX_ROWS;
            nr = (struct sort_row *)realloc(rows, (unsigned long)ncap * sizeof(*nr));
            if (!nr) {
                krec_err("sortby: out of memory\n");
                free_rows(rows, nrows);
                return 1;
            }
            rows = nr;
            cap = ncap;
        }

        for (i = 0; i < schema.ncols; i++)
            total += strlen(fields[i]) + 1;
        blob = (char *)malloc(total ? total : 1);
        if (!blob) {
            krec_err("sortby: out of memory\n");
            free_rows(rows, nrows);
            return 1;
        }
        rows[nrows].blob = blob;
        for (i = 0; i < schema.ncols; i++) {
            size_t len = strlen(fields[i]) + 1;
            memcpy(blob + off, fields[i], len);
            rows[nrows].fields[i] = blob + off;
            off += len;
        }
        nrows++;
    }

    if (rc < 0) {
        krec_err("sortby: malformed @krec record on stdin\n");
        free_rows(rows, nrows);
        return 1;
    }

    if (sort_rows(rows, nrows, col, schema.cols[col].type,
                  force_num, icase, reverse) < 0) {
        krec_err("sortby: out of memory\n");
        free_rows(rows, nrows);
        return 1;
    }

    if (krec_begin_schema(1, &schema) < 0) {
        free_rows(rows, nrows);
        return 1;
    }
    for (i = 0; i < nrows; i++) {
        if (krec_row_vals(1, schema.ncols,
                          (const char *const *)rows[i].fields) < 0) {
            free_rows(rows, nrows);
            return 1;
        }
    }

    free_rows(rows, nrows);
    return 0;
}
