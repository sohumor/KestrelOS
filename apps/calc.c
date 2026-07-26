/* calc.c - 64-bit integer expression evaluator.
 *
 * usage: calc "2*(3+4)"   (multiple argv tokens are joined with spaces)
 * Grammar (recursive descent):
 *   expr   := term (('+' | '-') term)*
 *   term   := unary (('*' | '/' | '%') unary)*
 *   unary  := ('-' | '+') unary | primary
 *   primary:= number | '(' expr ')'
 */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

#define EXPR_SZ   512
#define MAX_DEPTH 64

#define ERR_NONE   0
#define ERR_SYNTAX 1
#define ERR_DIVZ   2

static const char *g_p;
static int g_err;
static int g_depth;

/* Pull the shell-injected "--cwd=<path>" argument, wherever it sits. */
static int strip_cwd_arg(int argc, char **argv)
{
    int i, out = 1;

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--cwd=", 6) != 0)
            argv[out++] = argv[i];
    }
    return out;
}

static void skip_ws(void)
{
    while (*g_p == ' ' || *g_p == '\t')
        g_p++;
}

static long parse_expr(void);

static long parse_primary(void)
{
    long v = 0;

    skip_ws();
    if (*g_p == '(') {
        g_p++;
        v = parse_expr();
        skip_ws();
        if (*g_p != ')') {
            if (!g_err)
                g_err = ERR_SYNTAX;
            return 0;
        }
        g_p++;
        return v;
    }

    if (*g_p < '0' || *g_p > '9') {
        if (!g_err)
            g_err = ERR_SYNTAX;
        return 0;
    }
    while (*g_p >= '0' && *g_p <= '9') {
        v = v * 10 + (*g_p - '0');
        g_p++;
    }
    return v;
}

static long parse_unary(void)
{
    long v;

    if (g_depth >= MAX_DEPTH) {
        if (!g_err)
            g_err = ERR_SYNTAX;
        return 0;
    }

    skip_ws();
    if (*g_p == '-') {
        g_p++;
        g_depth++;
        v = -parse_unary();
        g_depth--;
        return v;
    }
    if (*g_p == '+') {
        g_p++;
        g_depth++;
        v = parse_unary();
        g_depth--;
        return v;
    }

    g_depth++;
    v = parse_primary();
    g_depth--;
    return v;
}

static long parse_term(void)
{
    long v = parse_unary();

    for (;;) {
        char op;
        long r;

        if (g_err)
            return 0;
        skip_ws();
        op = *g_p;
        if (op != '*' && op != '/' && op != '%')
            return v;
        g_p++;

        r = parse_unary();
        if (g_err)
            return 0;

        if (op == '*') {
            v = v * r;
        } else if (r == 0) {
            g_err = ERR_DIVZ;
            return 0;
        } else if (r == -1) {
            /* Avoids the LONG_MIN / -1 divide fault. */
            v = (op == '/') ? -v : 0;
        } else if (op == '/') {
            v = v / r;
        } else {
            v = v % r;
        }
    }
}

static long parse_expr(void)
{
    long v;

    if (g_depth >= MAX_DEPTH) {
        if (!g_err)
            g_err = ERR_SYNTAX;
        return 0;
    }

    g_depth++;
    v = parse_term();
    g_depth--;

    for (;;) {
        char op;
        long r;

        if (g_err)
            return 0;
        skip_ws();
        op = *g_p;
        if (op != '+' && op != '-')
            return v;
        g_p++;

        g_depth++;
        r = parse_term();
        g_depth--;
        if (g_err)
            return 0;

        v = (op == '+') ? v + r : v - r;
    }
}

int main(int argc, char **argv)
{
    char expr[EXPR_SZ];
    unsigned long len;
    long v;
    int i;

    argc = strip_cwd_arg(argc, argv);
    if (argc < 2) {
        printf("usage: calc \"<expression>\"\n");
        return 1;
    }

    expr[0] = '\0';
    for (i = 1; i < argc; i++) {
        len = strlen(expr);
        snprintf(expr + len, sizeof(expr) - len, "%s%s",
                 i > 1 ? " " : "", argv[i]);
    }

    g_p = expr;
    g_err = ERR_NONE;
    g_depth = 0;

    v = parse_expr();
    skip_ws();
    if (!g_err && *g_p != '\0')
        g_err = ERR_SYNTAX;

    if (g_err == ERR_DIVZ) {
        printf("divide by zero\n");
        return 1;
    }
    if (g_err) {
        printf("syntax error\n");
        return 1;
    }

    printf("%ld\n", v);
    return 0;
}
