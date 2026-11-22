/* where.c - filter @krec records on stdin by a predicate.
 *
 * usage: where [-i] [-n] [-v] "field op value"
 *        where [-i] [-n] [-v] field op value
 *
 * Operators: ==, =, !=, >, >=, <, <=, ~ (substring), !~ (not substring).
 */

#include <kestrel.h>
#include <krec.h>
#include <stdio.h>
#include <string.h>

static void usage(void)
{
    krec_err("usage: where [-i] [-n] [-v] \"field op value\"\n"
             "       where [-i] [-n] [-v] field op value\n");
}

static int is_valid_op(const char *op)
{
    return strcmp(op, "==") == 0 || strcmp(op, "=") == 0 ||
           strcmp(op, "!=") == 0 || strcmp(op, ">") == 0 ||
           strcmp(op, ">=") == 0 || strcmp(op, "<") == 0 ||
           strcmp(op, "<=") == 0 || strcmp(op, "~") == 0 ||
           strcmp(op, "!~") == 0;
}

static int is_op_char(char c)
{
    return c == '=' || c == '!' || c == '>' || c == '<' || c == '~';
}

static int parse_single_expr(const char *expr, char *field, size_t fsz,
                             char *op, size_t osz, char *val, size_t vsz)
{
    const char *p = expr;
    size_t flen = 0, vlen;

    while (*p == ' ' || *p == '\t')
        p++;
    if (!*p)
        return -1;

    while (*p && *p != ' ' && *p != '\t' && !is_op_char(*p)) {
        if (flen + 1 >= fsz)
            return -1;
        field[flen++] = *p++;
    }
    field[flen] = '\0';
    if (flen == 0)
        return -1;

    while (*p == ' ' || *p == '\t')
        p++;

    if ((p[0] == '=' && p[1] == '=') || (p[0] == '!' && p[1] == '=') ||
        (p[0] == '>' && p[1] == '=') || (p[0] == '<' && p[1] == '=') ||
        (p[0] == '!' && p[1] == '~')) {
        if (osz < 3)
            return -1;
        op[0] = p[0];
        op[1] = p[1];
        op[2] = '\0';
        p += 2;
    } else if (*p == '=' || *p == '>' || *p == '<' || *p == '~') {
        if (osz < 2)
            return -1;
        op[0] = *p++;
        op[1] = '\0';
    } else {
        return -1;
    }

    while (*p == ' ' || *p == '\t')
        p++;
    vlen = strlen(p);
    while (vlen > 0 && (p[vlen - 1] == ' ' || p[vlen - 1] == '\t' ||
                        p[vlen - 1] == '\r' || p[vlen - 1] == '\n'))
        vlen--;

    if (vlen >= 2 && ((p[0] == '\'' && p[vlen - 1] == '\'') ||
                      (p[0] == '"' && p[vlen - 1] == '"'))) {
        p++;
        vlen -= 2;
    }
    if (vlen >= vsz)
        return -1;
    memcpy(val, p, vlen);
    val[vlen] = '\0';
    return 0;
}

static int compare_numeric(enum krec_type t, const char *cell, const char *val,
                           int *cmp_out)
{
    if (t == KREC_HEX) {
        uint64_t a, b;
        if (krec_parse_hex(cell, &a) == 0 && krec_parse_hex(val, &b) == 0) {
            *cmp_out = (a > b) - (a < b);
            return 0;
        }
        return -1;
    }
    if (t == KREC_BYTES) {
        uint64_t a, b;
        if (krec_parse_bytes(cell, &a) == 0 && krec_parse_bytes(val, &b) == 0) {
            *cmp_out = (a > b) - (a < b);
            return 0;
        }
        return -1;
    }
    if (t == KREC_U64) {
        uint64_t a, b;
        int64_t sb;
        if (krec_parse_u64(cell, &a) == 0) {
            if (krec_parse_u64(val, &b) == 0) {
                *cmp_out = (a > b) - (a < b);
                return 0;
            }
            if (krec_parse_i64(val, &sb) == 0 && sb < 0) {
                *cmp_out = 1;
                return 0;
            }
        }
        return -1;
    }

    {
        int64_t ia, ib;
        uint64_t ua, ub;
        if (krec_parse_i64(cell, &ia) == 0 && krec_parse_i64(val, &ib) == 0) {
            *cmp_out = (ia > ib) - (ia < ib);
            return 0;
        }
        if (krec_parse_u64(cell, &ua) == 0 && krec_parse_u64(val, &ub) == 0) {
            *cmp_out = (ua > ub) - (ua < ub);
            return 0;
        }
    }
    return -1;
}

static int apply_cmp_op(const char *op, int cmp)
{
    if (strcmp(op, "==") == 0 || strcmp(op, "=") == 0)
        return cmp == 0;
    if (strcmp(op, "!=") == 0)
        return cmp != 0;
    if (strcmp(op, ">") == 0)
        return cmp > 0;
    if (strcmp(op, ">=") == 0)
        return cmp >= 0;
    if (strcmp(op, "<") == 0)
        return cmp < 0;
    if (strcmp(op, "<=") == 0)
        return cmp <= 0;
    return 0;
}

static int eval_pred(enum krec_type t, const char *cell, const char *op,
                     const char *val, int icase, int force_num)
{
    int cmp = 0;
    int is_eq = (strcmp(op, "==") == 0 || strcmp(op, "=") == 0 ||
                 strcmp(op, "!=") == 0);

    if (strcmp(op, "~") == 0 || strcmp(op, "!~") == 0) {
        int hit = (icase ? krec_strcasestr(cell, val) : strstr(cell, val)) != 0;
        return op[0] == '!' ? !hit : hit;
    }

    if (force_num || krec_is_numeric_type(t)) {
        if (compare_numeric(t, cell, val, &cmp) == 0)
            return apply_cmp_op(op, cmp);
        if (!is_eq)
            return 0;
        cmp = icase ? krec_strcasecmp(cell, val) : strcmp(cell, val);
        return apply_cmp_op(op, cmp);
    }

    if (t == KREC_BOOL) {
        int ba, bb;
        if (krec_parse_bool(cell, &ba) == 0 && krec_parse_bool(val, &bb) == 0) {
            cmp = (ba > bb) - (ba < bb);
            return apply_cmp_op(op, cmp);
        }
        if (!is_eq)
            return 0;
    }

    if (t == KREC_IP) {
        uint32_t ipa, ipb;
        if (krec_parse_ip(cell, &ipa) == 0 && krec_parse_ip(val, &ipb) == 0) {
            cmp = (ipa > ipb) - (ipa < ipb);
            return apply_cmp_op(op, cmp);
        }
        if (!is_eq)
            return 0;
    }

    cmp = icase ? krec_strcasecmp(cell, val) : strcmp(cell, val);
    return apply_cmp_op(op, cmp);
}

int main(int argc, char **argv)
{
    struct krec_reader r;
    struct krec_schema schema;
    char line[KREC_LINE_MAX];
    char *fields[KREC_MAX_COLS];
    char field[KREC_NAME_MAX], op[8], val[512];
    const char *pos[4];
    int npos = 0, icase = 0, force_num = 0, invert = 0, end_opts = 0;
    int col, rc, hdr;
    int i;

    for (i = 1; i < argc; i++) {
        if (!end_opts && strcmp(argv[i], "--") == 0) {
            end_opts = 1;
        } else if (!end_opts && strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else if (!end_opts && npos < 2 &&
                   argv[i][0] == '-' && argv[i][1] != '\0') {
            int j;
            for (j = 1; argv[i][j]; j++) {
                if (argv[i][j] == 'i')
                    icase = 1;
                else if (argv[i][j] == 'n')
                    force_num = 1;
                else if (argv[i][j] == 'v')
                    invert = 1;
                else if (argv[i][j] == 'h') {
                    usage();
                    return 0;
                } else {
                    usage();
                    return 1;
                }
            }
        } else {
            if (npos >= 4) {
                usage();
                return 1;
            }
            pos[npos++] = argv[i];
        }
    }

    if (npos == 1) {
        if (parse_single_expr(pos[0], field, sizeof(field),
                              op, sizeof(op), val, sizeof(val)) < 0) {
            krec_err("where: invalid expression '%s'\n", pos[0]);
            usage();
            return 1;
        }
    } else if (npos == 3) {
        snprintf(field, sizeof(field), "%s", pos[0]);
        snprintf(op, sizeof(op), "%s", pos[1]);
        snprintf(val, sizeof(val), "%s", pos[2]);
        if (!is_valid_op(op)) {
            krec_err("where: unknown operator '%s'\n", op);
            usage();
            return 1;
        }
    } else {
        usage();
        return 1;
    }

    krec_reader_init(&r, 0);
    hdr = krec_read_header(&r, &schema);
    if (hdr == 0)
        return 0;
    if (hdr < 0) {
        krec_err("where: invalid @krec header on stdin\n");
        return 1;
    }

    col = krec_find_col(&schema, field);
    if (col < 0) {
        krec_err("where: unknown field '%s'\n", field);
        return 1;
    }

    if (krec_begin_schema(1, &schema) < 0)
        return 1;

    while ((rc = krec_next(&r, line, sizeof(line), fields, KREC_MAX_COLS)) == 1) {
        int m = eval_pred(schema.cols[col].type, fields[col], op, val,
                          icase, force_num);
        if (invert)
            m = !m;
        if (m) {
            if (krec_row_vals(1, schema.ncols, (const char *const *)fields) < 0)
                return 1;
        }
    }

    if (rc < 0) {
        krec_err("where: malformed @krec record on stdin\n");
        return 1;
    }
    return 0;
}
