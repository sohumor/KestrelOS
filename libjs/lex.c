/* libjs: the tokenizer.
 *
 * Produces every ES5 token type. Two things here are less obvious than
 * they look:
 *
 *  - `nl_before` is recorded on every token, because automatic semicolon
 *    insertion is a parser decision made on exactly that information.
 *  - Whether `/` opens a regular expression or divides depends on the
 *    grammar, not the characters. The lexer uses the standard previous-
 *    token heuristic, refined with a small parenthesis stack so that
 *    `if (x) /re/.test(s)` lexes correctly. It is a heuristic; see the
 *    deviation list in js.h.
 *
 * Source is treated as bytes. UTF-8 sequences pass through unchanged and
 * bytes >= 0x80 are accepted inside identifiers, which is more permissive
 * than ES5 but never rejects valid code.
 */

#define JS_INTERNAL
#include "js.h"

#include <string.h>

static const struct { const char *w; int tok; } kw_table[] = {
    { "break", TK_BREAK }, { "case", TK_CASE }, { "catch", TK_CATCH },
    { "continue", TK_CONTINUE }, { "debugger", TK_DEBUGGER },
    { "default", TK_DEFAULT }, { "delete", TK_DELETE }, { "do", TK_DO },
    { "else", TK_ELSE }, { "false", TK_FALSE }, { "finally", TK_FINALLY },
    { "for", TK_FOR }, { "function", TK_FUNCTION }, { "if", TK_IF },
    { "in", TK_IN }, { "instanceof", TK_INSTANCEOF }, { "new", TK_NEW },
    { "null", TK_NULL_KW }, { "return", TK_RETURN }, { "switch", TK_SWITCH },
    { "this", TK_THIS }, { "throw", TK_THROW }, { "true", TK_TRUE },
    { "try", TK_TRY }, { "typeof", TK_TYPEOF }, { "var", TK_VAR },
    { "const", TK_CONST }, { "let", TK_LET }, { "void", TK_VOID },
    { "while", TK_WHILE }, { "with", TK_WITH },
    { 0, 0 }
};

const char *js_token_name(int tok)
{
    static const char *punct[] = {
        "end of input", "identifier", "number", "string", "regular expression",
        "{", "}", "(", ")", "[", "]", ".", ";", ",",
        "<", ">", "<=", ">=", "==", "!=", "===", "!==",
        "+", "-", "*", "/", "%", "++", "--", "<<", ">>", ">>>",
        "&", "|", "^", "!", "~", "&&", "||", "?", ":", "=",
        "+=", "-=", "*=", "/=", "%=", "<<=", ">>=", ">>>=", "&=", "|=", "^="
    };
    int i;

    if (tok >= 0 && tok < (int)(sizeof(punct) / sizeof(punct[0])))
        return punct[tok];
    for (i = 0; kw_table[i].w; i++)
        if (kw_table[i].tok == tok)
            return kw_table[i].w;
    return "token";
}

static int is_id_start(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '_' || c == '$' || c >= 0x80;
}

static int is_id_part(unsigned char c)
{
    return is_id_start(c) || (c >= '0' && c <= '9');
}

static int is_digit(unsigned char c) { return c >= '0' && c <= '9'; }

static int hexval(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void js_lex_init(js_lexer *lx, js_ctx *ctx, const char *src)
{
    memset(lx, 0, sizeof(*lx));
    lx->ctx = ctx;
    lx->src = src;
    lx->len = strlen(src);
    lx->line = 1;
    lx->tok = TK_EOF;
    lx->prev_tok = -1;
}

static int lex_error(js_lexer *lx, const char *msg)
{
    lx->error = 1;
    return js_throw_error(lx->ctx, JS_ERR_SYNTAX, "%s (line %d)", msg, lx->line);
}

/* Would a `/` here start a regular expression? */
static int regex_allowed(js_lexer *lx)
{
    switch (lx->prev_tok) {
    case -1:                 /* start of input */
        return 1;
    case TK_IDENT: case TK_NUM: case TK_STR: case TK_REGEX:
    case TK_RBRACKET: case TK_INC: case TK_DEC:
    case TK_THIS: case TK_TRUE: case TK_FALSE: case TK_NULL_KW:
        return 0;
    case TK_RPAREN:
        /* `)` closing an if/while/for header is followed by a statement, so
         * a regex is legal there; otherwise it ends an expression. */
        return lx->last_paren == 1;
    case TK_RBRACE:
        return 1;            /* a block far more often than an object literal */
    default:
        return 1;
    }
}

/* Append one code point as UTF-8. */
static int utf8_put(char *b, int n, unsigned long cp)
{
    if (cp < 0x80) { b[n++] = (char)cp; }
    else if (cp < 0x800) {
        b[n++] = (char)(0xC0 | (cp >> 6));
        b[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        b[n++] = (char)(0xE0 | (cp >> 12));
        b[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        b[n++] = (char)(0xF0 | (cp >> 18));
        b[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[n++] = (char)(0x80 | (cp & 0x3F));
    }
    return n;
}

static int lex_string(js_lexer *lx, char quote)
{
    unsigned long start = lx->pos;
    unsigned long raw;
    char stackbuf[256];
    char *buf;
    int n = 0;

    /* Bound the decoded length: escapes never expand beyond their source. */
    for (raw = start; raw < lx->len; raw++) {
        if (lx->src[raw] == '\\') { raw++; continue; }
        if (lx->src[raw] == quote) break;
        if (lx->src[raw] == '\n' || lx->src[raw] == '\r')
            return lex_error(lx, "unterminated string literal");
    }
    if (raw >= lx->len)
        return lex_error(lx, "unterminated string literal");
    if (raw - start + 4 <= sizeof(stackbuf))
        buf = stackbuf;
    else {
        buf = (char *)js_alloc_raw(lx->ctx, raw - start + 4);
        if (!buf) return JS_THROW;
    }

    while (lx->pos < lx->len && lx->src[lx->pos] != quote) {
        char c = lx->src[lx->pos];
        if (c != '\\') { buf[n++] = c; lx->pos++; continue; }
        lx->pos++;
        if (lx->pos >= lx->len)
            return lex_error(lx, "unterminated string literal");
        c = lx->src[lx->pos++];
        switch (c) {
        case 'n': buf[n++] = '\n'; break;
        case 't': buf[n++] = '\t'; break;
        case 'r': buf[n++] = '\r'; break;
        case 'b': buf[n++] = '\b'; break;
        case 'f': buf[n++] = '\f'; break;
        case 'v': buf[n++] = '\v'; break;
        case '0':
            if (lx->pos < lx->len && is_digit((unsigned char)lx->src[lx->pos]))
                return lex_error(lx, "octal escapes are not supported");
            buf[n++] = '\0';
            break;
        case 'x': {
            int h1, h2;
            if (lx->pos + 1 >= lx->len) return lex_error(lx, "bad \\x escape");
            h1 = hexval((unsigned char)lx->src[lx->pos]);
            h2 = hexval((unsigned char)lx->src[lx->pos + 1]);
            if (h1 < 0 || h2 < 0) return lex_error(lx, "bad \\x escape");
            lx->pos += 2;
            n = utf8_put(buf, n, (unsigned long)(h1 * 16 + h2));
            break;
        }
        case 'u': {
            unsigned long cp = 0;
            int i;
            if (lx->pos + 3 >= lx->len) return lex_error(lx, "bad \\u escape");
            for (i = 0; i < 4; i++) {
                int h = hexval((unsigned char)lx->src[lx->pos + i]);
                if (h < 0) return lex_error(lx, "bad \\u escape");
                cp = cp * 16 + (unsigned long)h;
            }
            lx->pos += 4;
            /* Combine a surrogate pair so astral characters survive. */
            if (cp >= 0xD800 && cp <= 0xDBFF && lx->pos + 5 < lx->len &&
                lx->src[lx->pos] == '\\' && lx->src[lx->pos + 1] == 'u') {
                unsigned long lo = 0;
                int ok = 1;
                for (i = 0; i < 4; i++) {
                    int h = hexval((unsigned char)lx->src[lx->pos + 2 + i]);
                    if (h < 0) { ok = 0; break; }
                    lo = lo * 16 + (unsigned long)h;
                }
                if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    lx->pos += 6;
                }
            }
            n = utf8_put(buf, n, cp);
            break;
        }
        case '\r':
            if (lx->pos < lx->len && lx->src[lx->pos] == '\n') lx->pos++;
            lx->line++;
            break;                       /* line continuation */
        case '\n':
            lx->line++;
            break;
        default:
            buf[n++] = c;
            break;
        }
    }
    if (lx->pos >= lx->len)
        return lex_error(lx, "unterminated string literal");
    lx->pos++;                           /* closing quote */
    lx->tok = TK_STR;
    lx->text = js_str_new(lx->ctx, buf, (unsigned long)n);
    return lx->text ? JS_OK : JS_THROW;
}

static int lex_number(js_lexer *lx)
{
    unsigned long start = lx->pos;
    unsigned long used = 0;

    if (lx->src[lx->pos] == '0' && lx->pos + 1 < lx->len &&
        (lx->src[lx->pos + 1] == 'x' || lx->src[lx->pos + 1] == 'X')) {
        double v = 0;
        unsigned long i = lx->pos + 2;
        if (i >= lx->len || hexval((unsigned char)lx->src[i]) < 0)
            return lex_error(lx, "malformed hexadecimal literal");
        for (; i < lx->len; i++) {
            int h = hexval((unsigned char)lx->src[i]);
            if (h < 0) break;
            v = v * 16 + h;
        }
        lx->pos = i;
        lx->num = v;
        lx->tok = TK_NUM;
    } else if (lx->src[lx->pos] == '0' && lx->pos + 1 < lx->len &&
               lx->src[lx->pos + 1] >= '0' && lx->src[lx->pos + 1] <= '7') {
        /* Legacy octal. Only if every digit is octal; 08 and 09 are decimal. */
        unsigned long i = lx->pos + 1;
        double v = 0;
        while (i < lx->len && lx->src[i] >= '0' && lx->src[i] <= '7') i++;
        if (i < lx->len && (lx->src[i] == '8' || lx->src[i] == '9' ||
                            lx->src[i] == '.' || lx->src[i] == 'e' ||
                            lx->src[i] == 'E')) {
            goto decimal;
        }
        for (i = lx->pos + 1; i < lx->len && lx->src[i] >= '0' && lx->src[i] <= '7'; i++)
            v = v * 8 + (lx->src[i] - '0');
        lx->pos = i;
        lx->num = v;
        lx->tok = TK_NUM;
    } else {
decimal:
        lx->num = js_strtod(lx->src + start, lx->len - start, &used);
        if (used == 0)
            return lex_error(lx, "malformed number");
        lx->pos = start + used;
        lx->tok = TK_NUM;
    }
    if (lx->pos < lx->len && is_id_part((unsigned char)lx->src[lx->pos]))
        return lex_error(lx, "identifier immediately after a number");
    return JS_OK;
}

static int lex_regex(js_lexer *lx)
{
    unsigned long start = lx->pos;      /* just past the opening '/' */
    int in_class = 0;

    while (lx->pos < lx->len) {
        char c = lx->src[lx->pos];
        if (c == '\\') {
            if (lx->pos + 1 >= lx->len) break;
            lx->pos += 2;
            continue;
        }
        if (c == '\n' || c == '\r')
            return lex_error(lx, "unterminated regular expression");
        if (c == '[') in_class = 1;
        else if (c == ']') in_class = 0;
        else if (c == '/' && !in_class) break;
        lx->pos++;
    }
    if (lx->pos >= lx->len)
        return lex_error(lx, "unterminated regular expression");
    lx->text = js_str_new(lx->ctx, lx->src + start, lx->pos - start);
    lx->pos++;                           /* closing '/' */
    {
        unsigned long fs = lx->pos;
        while (lx->pos < lx->len && is_id_part((unsigned char)lx->src[lx->pos]))
            lx->pos++;
        lx->flags = js_str_new(lx->ctx, lx->src + fs, lx->pos - fs);
    }
    if (!lx->text || !lx->flags)
        return JS_THROW;
    lx->tok = TK_REGEX;
    return JS_OK;
}

int js_lex_next(js_lexer *lx)
{
    const char *s = lx->src;
    int r;

    if (lx->error || lx->ctx->fatal)
        return JS_THROW;
    lx->prev_tok = lx->tok;
    lx->nl_before = 0;
    lx->text = 0;
    lx->flags = 0;

    for (;;) {
        while (lx->pos < lx->len) {
            unsigned char c = (unsigned char)s[lx->pos];
            if (c == '\n') { lx->line++; lx->nl_before = 1; lx->pos++; continue; }
            if (c == '\r') {
                lx->line++; lx->nl_before = 1; lx->pos++;
                if (lx->pos < lx->len && s[lx->pos] == '\n') lx->pos++;
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\v' || c == '\f') { lx->pos++; continue; }
            /* U+00A0, U+2028, U+2029 and U+FEFF as UTF-8 */
            if (c == 0xC2 && lx->pos + 1 < lx->len &&
                (unsigned char)s[lx->pos + 1] == 0xA0) { lx->pos += 2; continue; }
            if (c == 0xE2 && lx->pos + 2 < lx->len &&
                (unsigned char)s[lx->pos + 1] == 0x80 &&
                ((unsigned char)s[lx->pos + 2] == 0xA8 ||
                 (unsigned char)s[lx->pos + 2] == 0xA9)) {
                lx->pos += 3; lx->line++; lx->nl_before = 1; continue;
            }
            if (c == 0xEF && lx->pos + 2 < lx->len &&
                (unsigned char)s[lx->pos + 1] == 0xBB &&
                (unsigned char)s[lx->pos + 2] == 0xBF) { lx->pos += 3; continue; }
            break;
        }
        if (lx->pos + 1 < lx->len && s[lx->pos] == '/' && s[lx->pos + 1] == '/') {
            lx->pos += 2;
            while (lx->pos < lx->len && s[lx->pos] != '\n' && s[lx->pos] != '\r')
                lx->pos++;
            continue;
        }
        if (lx->pos + 1 < lx->len && s[lx->pos] == '/' && s[lx->pos + 1] == '*') {
            lx->pos += 2;
            for (;;) {
                if (lx->pos + 1 >= lx->len)
                    return lex_error(lx, "unterminated comment");
                if (s[lx->pos] == '*' && s[lx->pos + 1] == '/') { lx->pos += 2; break; }
                if (s[lx->pos] == '\n' || s[lx->pos] == '\r') {
                    lx->line++;
                    lx->nl_before = 1;   /* a multi-line comment counts for ASI */
                }
                lx->pos++;
            }
            continue;
        }
        break;
    }

    lx->tok_line = lx->line;
    if (lx->pos >= lx->len) { lx->tok = TK_EOF; return JS_OK; }

    {
        unsigned char c = (unsigned char)s[lx->pos];

        if (is_id_start(c)) {
            unsigned long start = lx->pos;
            int i;
            while (lx->pos < lx->len && is_id_part((unsigned char)s[lx->pos]))
                lx->pos++;
            lx->text = js_str_intern(lx->ctx, s + start, lx->pos - start);
            if (!lx->text) return JS_THROW;
            for (i = 0; kw_table[i].w; i++)
                if (lx->text->len == strlen(kw_table[i].w) &&
                    memcmp(lx->text->data, kw_table[i].w, lx->text->len) == 0) {
                    lx->tok = kw_table[i].tok;
                    return JS_OK;
                }
            lx->tok = TK_IDENT;
            return JS_OK;
        }
        if (is_digit(c) ||
            (c == '.' && lx->pos + 1 < lx->len && is_digit((unsigned char)s[lx->pos + 1])))
            return lex_number(lx);
        if (c == '"' || c == '\'') {
            lx->pos++;
            return lex_string(lx, (char)c);
        }
    }

#define P1(ch, t)  if (s[lx->pos] == (ch)) { lx->pos++; lx->tok = (t); return JS_OK; }
#define P2(a, b, t) if (s[lx->pos] == (a) && lx->pos + 1 < lx->len && s[lx->pos+1] == (b)) \
                        { lx->pos += 2; lx->tok = (t); return JS_OK; }
#define P3(a, b, c2, t) if (s[lx->pos] == (a) && lx->pos + 2 < lx->len && \
                            s[lx->pos+1] == (b) && s[lx->pos+2] == (c2)) \
                        { lx->pos += 3; lx->tok = (t); return JS_OK; }
#define P4(a, b, c2, d, t) if (s[lx->pos] == (a) && lx->pos + 3 < lx->len && \
                               s[lx->pos+1] == (b) && s[lx->pos+2] == (c2) && \
                               s[lx->pos+3] == (d)) \
                        { lx->pos += 4; lx->tok = (t); return JS_OK; }

    P4('>', '>', '>', '=', TK_USHR_A)
    P3('>', '>', '>', TK_USHR)
    P3('=', '=', '=', TK_SEQ)
    P3('!', '=', '=', TK_SNE)
    P3('<', '<', '=', TK_SHL_A)
    P3('>', '>', '=', TK_SHR_A)
    P2('<', '<', TK_SHL)
    P2('>', '>', TK_SHR)
    P2('<', '=', TK_LE)
    P2('>', '=', TK_GE)
    P2('=', '=', TK_EQ)
    P2('!', '=', TK_NE)
    P2('+', '+', TK_INC)
    P2('-', '-', TK_DEC)
    P2('&', '&', TK_ANDAND)
    P2('|', '|', TK_OROR)
    P2('+', '=', TK_ADD_A)
    P2('-', '=', TK_SUB_A)
    P2('*', '=', TK_MUL_A)
    P2('%', '=', TK_MOD_A)
    P2('&', '=', TK_BAND_A)
    P2('|', '=', TK_BOR_A)
    P2('^', '=', TK_BXOR_A)

    if (s[lx->pos] == '/') {
        if (lx->pos + 1 < lx->len && s[lx->pos + 1] == '=' && !regex_allowed(lx)) {
            lx->pos += 2; lx->tok = TK_DIV_A; return JS_OK;
        }
        if (regex_allowed(lx)) {
            lx->pos++;
            return lex_regex(lx);
        }
        if (lx->pos + 1 < lx->len && s[lx->pos + 1] == '=') {
            lx->pos += 2; lx->tok = TK_DIV_A; return JS_OK;
        }
        lx->pos++; lx->tok = TK_DIV; return JS_OK;
    }

    if (s[lx->pos] == '(') {
        lx->pos++;
        if (lx->paren_sp < 63) {
            lx->paren_sp++;
            lx->paren_kind[lx->paren_sp] =
                (lx->prev_tok == TK_IF || lx->prev_tok == TK_WHILE ||
                 lx->prev_tok == TK_FOR || lx->prev_tok == TK_WITH) ? 1 : 0;
        }
        lx->tok = TK_LPAREN;
        return JS_OK;
    }
    if (s[lx->pos] == ')') {
        lx->pos++;
        lx->last_paren = (lx->paren_sp > 0 && lx->paren_sp < 64)
                             ? lx->paren_kind[lx->paren_sp] : 0;
        if (lx->paren_sp > 0)
            lx->paren_sp--;
        lx->tok = TK_RPAREN;
        return JS_OK;
    }

    P1('{', TK_LBRACE) P1('}', TK_RBRACE)
    P1('[', TK_LBRACKET) P1(']', TK_RBRACKET)
    P1('.', TK_DOT) P1(';', TK_SEMI) P1(',', TK_COMMA)
    P1('<', TK_LT) P1('>', TK_GT)
    P1('+', TK_ADD) P1('-', TK_SUB) P1('*', TK_MUL) P1('%', TK_MOD)
    P1('&', TK_BAND) P1('|', TK_BOR) P1('^', TK_BXOR)
    P1('!', TK_NOT) P1('~', TK_BNOT)
    P1('?', TK_QUESTION) P1(':', TK_COLON) P1('=', TK_ASSIGN)

#undef P1
#undef P2
#undef P3
#undef P4

    r = lex_error(lx, "unexpected character");
    return r;
}
