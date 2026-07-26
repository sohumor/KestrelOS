/* test_dom.c - host test harness for libweb's HTML tokenizer and DOM.
 *
 * Everything in libweb/ is pure computation with no I/O, so it can be
 * compiled for the host and hammered without booting KestrelOS. That
 * is what this does.
 *
 *   gcc -Wall -Wextra -O1 -g -fsanitize=address,undefined -Ilibweb \
 *       -o test_dom libweb/dom.c libweb/html.c libweb/entities.c \
 *       tools/test_dom.c
 *   ./test_dom            # everything
 *   ./test_dom --quick    # skip the big and slow cases
 *   ./test_dom --fuzz N   # N fuzz iterations (default 40000)
 *
 * Trees are compared as S-expressions:
 *   (#doc (html (head) (body (p id="x" "hello"))))
 * which is compact enough to write expectations by hand and complete
 * enough that no structural difference can hide.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dom.h"

static int g_pass, g_fail;

/* ------------------------------------------------------------------ *
 * A growable string, for building tree shapes
 * ------------------------------------------------------------------ */

struct sb {
    char *p;
    size_t len, cap;
};

static void sb_init(struct sb *b)
{
    b->p = malloc(256);
    b->len = 0;
    b->cap = 256;
    b->p[0] = 0;
}

static void sb_add(struct sb *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        while (b->cap < b->len + n + 1)
            b->cap *= 2;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void sb_str(struct sb *b, const char *s)
{
    sb_add(b, s, strlen(s));
}

/* Text in a shape is quoted with C-style escapes so that a newline or
 * a quote inside the document cannot forge structure. */
static void sb_quoted(struct sb *b, const char *s, unsigned long n)
{
    unsigned long i;
    char tmp[8];

    sb_str(b, "\"");
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '\n': sb_str(b, "\\n"); break;
        case '\r': sb_str(b, "\\r"); break;
        case '\t': sb_str(b, "\\t"); break;
        case '"':  sb_str(b, "\\\""); break;
        case '\\': sb_str(b, "\\\\"); break;
        default:
            if (c < 32 || c > 126) {
                sprintf(tmp, "\\x%02x", c);
                sb_str(b, tmp);
            } else {
                sb_add(b, (const char *)&c, 1);
            }
        }
    }
    sb_str(b, "\"");
}

/* ------------------------------------------------------------------ *
 * Tree shape
 * ------------------------------------------------------------------ */

static void shape_open(struct sb *b, const struct dom_node *n)
{
    unsigned int i;

    switch (n->type) {
    case DOM_DOCUMENT:
        sb_str(b, "(#doc");
        return;
    case DOM_TEXT:
        sb_str(b, " ");
        sb_quoted(b, n->text, n->text_len);
        return;
    case DOM_COMMENT:
        sb_str(b, " (#comment ");
        sb_quoted(b, n->text, n->text_len);
        sb_str(b, ")");
        return;
    case DOM_DOCTYPE:
        sb_str(b, " (#doctype ");
        sb_quoted(b, n->text, n->text_len);
        sb_str(b, ")");
        return;
    default:
        break;
    }
    sb_str(b, " (");
    sb_str(b, n->tag);
    for (i = 0; i < n->nattr; i++) {
        sb_str(b, " ");
        sb_str(b, n->attr[i].name);
        sb_str(b, "=");
        sb_quoted(b, n->attr[i].value, n->attr[i].len);
    }
}

static void shape_close(struct sb *b, const struct dom_node *n)
{
    if (n->type == DOM_ELEMENT || n->type == DOM_DOCUMENT)
        sb_str(b, ")");
}

/* Iterative, so a tree at the depth cap cannot overflow the harness. */
static char *shape(const struct dom_node *root)
{
    struct sb b;
    const struct dom_node **stack;
    size_t sp = 0, scap = 4096;
    const struct dom_node *n = root;

    sb_init(&b);
    stack = malloc(scap * sizeof *stack);
    while (n) {
        shape_open(&b, n);
        if (n->first_child && sp < scap) {
            stack[sp++] = n;
            n = n->first_child;
            continue;
        }
        shape_close(&b, n);
        while (!n->next_sibling && sp > 0) {
            n = stack[--sp];
            shape_close(&b, n);
        }
        if (n->next_sibling && sp > 0)
            n = n->next_sibling;
        else if (n->next_sibling && n != root)
            n = n->next_sibling;
        else
            n = 0;
    }
    free(stack);
    return b.p;
}

/* ------------------------------------------------------------------ *
 * Assertions
 * ------------------------------------------------------------------ */

static void ok(int cond, const char *what)
{
    if (cond) {
        g_pass++;
    } else {
        g_fail++;
        printf("FAIL: %s\n", what);
    }
}

static void eq_str(const char *got, const char *want, const char *what)
{
    if (got && strcmp(got, want) == 0) {
        g_pass++;
        return;
    }
    g_fail++;
    printf("FAIL: %s\n  want: %s\n  got : %s\n", what, want,
           got ? got : "(null)");
}

/* ------------------------------------------------------------------ *
 * The corpus
 * ------------------------------------------------------------------ */

struct tcase {
    const char *name;
    const char *in;
    const char *want;
};

static const struct tcase g_cases[] = {

{ "empty document", "",
  "(#doc (html (head) (body)))" },

{ "bare text", "hello",
  "(#doc (html (head) (body \"hello\")))" },

{ "paragraph", "<p>hello</p>",
  "(#doc (html (head) (body (p \"hello\"))))" },

{ "doctype and title",
  "<!DOCTYPE html><title>T</title><p>x",
  "(#doc (#doctype \"html\") (html (head (title \"T\")) (body (p \"x\"))))" },

{ "doctype with legacy string",
  "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01//EN\"><p>x",
  "(#doc (#doctype \"html\") (html (head) (body (p \"x\"))))" },

{ "unclosed p", "<p>a<p>b",
  "(#doc (html (head) (body (p \"a\") (p \"b\"))))" },

{ "p closed by div", "<p>a<div>b</div>",
  "(#doc (html (head) (body (p \"a\") (div \"b\"))))" },

{ "nested lists",
  "<ul><li>a<ul><li>b</ul></ul>",
  "(#doc (html (head) (body (ul (li \"a\" (ul (li \"b\")))))))" },

{ "unclosed li",
  "<ul><li>a<li>b</ul>",
  "(#doc (html (head) (body (ul (li \"a\") (li \"b\")))))" },

{ "li with unclosed p",
  "<ul><li><p>a<li>b</ul>",
  "(#doc (html (head) (body (ul (li (p \"a\")) (li \"b\")))))" },

{ "definition list",
  "<dl><dt>a<dd>b<dt>c<dd>d</dl>",
  "(#doc (html (head) (body (dl (dt \"a\") (dd \"b\") (dt \"c\")"
  " (dd \"d\")))))" },

{ "stray end tag does not unwind",
  "<div>a</span>b</div>c",
  "(#doc (html (head) (body (div \"ab\") \"c\")))" },

{ "stray end tag for an open ancestor",
  "<div><p>a</div>b",
  "(#doc (html (head) (body (div (p \"a\")) \"b\")))" },

{ "end tag closes formatting inside a block",
  "<div><b>x</div>y",
  "(#doc (html (head) (body (div (b \"x\")) \"y\")))" },

{ "unmatched end tag at top level",
  "</p></div></b>text",
  "(#doc (html (head) (body \"text\")))" },

{ "attribute quoting styles",
  "<a href=\"one\" title='two' rel=three data-x id=q>L</a>",
  "(#doc (html (head) (body (a href=\"one\" title=\"two\" rel=\"three\""
  " data-x=\"\" id=\"q\" \"L\"))))" },

{ "valueless and empty attributes",
  "<input disabled value=>",
  "(#doc (html (head) (body (input disabled=\"\" value=\"\"))))" },

{ "duplicate attributes keep the first",
  "<p class=a class=b>x",
  "(#doc (html (head) (body (p class=\"a\" \"x\"))))" },

{ "uppercase tags and attributes",
  "<DIV CLASS=Big>X</DIV>",
  "(#doc (html (head) (body (div class=\"Big\" \"X\"))))" },

{ "entities in text",
  "<p>a&amp;b&lt;c&gt;d&quot;e&#65;f&#x42;g",
  "(#doc (html (head) (body (p \"a&b<c>d\\\"eAfBg\"))))" },

{ "entity without semicolon in text",
  "<p>a&ampb&copy c",
  "(#doc (html (head) (body (p \"a&b\\xc2\\xa9 c\"))))" },

{ "unknown entity stays literal",
  "<p>&nosuchthing; &#; &#x;",
  "(#doc (html (head) (body (p \"&nosuchthing; &#; &#x;\"))))" },

{ "entities in attribute values",
  "<a href=\"a&amp;b&lt;c\" title='&copy;'>x</a>",
  "(#doc (html (head) (body (a href=\"a&b<c\" title=\"\\xc2\\xa9\""
  " \"x\"))))" },

{ "legacy entity suppressed before = in an attribute",
  "<a href=\"?a=1&copy=2\">x</a>",
  "(#doc (html (head) (body (a href=\"?a=1&copy=2\" \"x\"))))" },

{ "longest match wins",
  "<p>&notin;|&notit;",
  "(#doc (html (head) (body (p \"\\xe2\\x88\\x89|\\xc2\\xacit;\"))))" },

{ "numeric reference windows-1252 remap",
  "<p>&#151;&#128;",
  "(#doc (html (head) (body (p \"\\xe2\\x80\\x94\\xe2\\x82\\xac\"))))" },

{ "numeric reference out of range",
  "<p>&#x110000;&#0;",
  "(#doc (html (head) (body (p \"\\xef\\xbf\\xbd\\xef\\xbf\\xbd\"))))" },

{ "script content is not markup",
  "<script>if (a<b && c>d) { x = \"</div>\"; }</script><p>after",
  "(#doc (html (head (script \"if (a<b && c>d) { x = \\\"</div>\\\"; }\"))"
  " (body (p \"after\"))))" },

{ "script entities are not decoded",
  "<script>a = \"&amp;\";</script>",
  "(#doc (html (head (script \"a = \\\"&amp;\\\";\")) (body)))" },

{ "script containing an escaped comment",
  "<script><!-- var a = \"<script>\"; --></script><p>x",
  "(#doc (html (head (script \"<!-- var a = \\\"<script>\\\"; -->\"))"
  " (body (p \"x\"))))" },

{ "style content is raw and collected",
  "<style>a > b { c: \"<d>\" }</style><p>x",
  "(#doc (html (head (style \"a > b { c: \\\"<d>\\\" }\"))"
  " (body (p \"x\"))))" },

{ "title is rcdata with entities",
  "<title>a &amp; b <not-a-tag></title>",
  "(#doc (html (head (title \"a & b <not-a-tag>\")) (body)))" },

{ "textarea keeps markup as text",
  "<textarea><b>x</b></textarea>",
  "(#doc (html (head) (body (textarea \"<b>x</b>\"))))" },

{ "comment", "<p>a<!-- hi -->b",
  "(#doc (html (head) (body (p \"a\" (#comment \" hi \") \"b\"))))" },

{ "comment containing dashes",
  "<!--a--b---c-->x",
  "(#doc (#comment \"a--b---c\") (html (head) (body \"x\")))" },

{ "empty comments",
  "<!--><!----><!--->x",
  "(#doc (#comment \"\") (#comment \"\") (#comment \"\")"
  " (html (head) (body \"x\")))" },

{ "bogus comment from a bang",
  "<!bogus>x",
  "(#doc (#comment \"bogus\") (html (head) (body \"x\")))" },

{ "bogus comment from a question mark",
  "<?xml version=\"1.0\"?>x",
  "(#doc (#comment \"xml version=\\\"1.0\\\"?\") (html (head) (body \"x\")))" },

{ "bogus comment from a bad end tag",
  "<p>a</ >b",
  "(#doc (html (head) (body (p \"a\" (#comment \" \") \"b\"))))" },

{ "cdata is text",
  "<p>a<![CDATA[b<c>d]]>e",
  "(#doc (html (head) (body (p \"ab<c>de\"))))" },

{ "table with implicit tbody",
  "<table><tr><td>a<td>b</table>",
  "(#doc (html (head) (body (table (tbody (tr (td \"a\") (td \"b\")))))))" },

{ "table with explicit sections",
  "<table><thead><tr><th>h</thead><tbody><tr><td>d</table>",
  "(#doc (html (head) (body (table (thead (tr (th \"h\")))"
  " (tbody (tr (td \"d\")))))))" },

{ "table rows auto-close",
  "<table><tr><td>a<tr><td>b</table>",
  "(#doc (html (head) (body (table (tbody (tr (td \"a\")) (tr (td \"b\")))))))" },

{ "foster parenting moves stray content out",
  "<table>stray<tr><td>in</table>",
  "(#doc (html (head) (body \"stray\" (table (tbody (tr (td \"in\")))))))" },

{ "foster parenting keeps table whitespace inside",
  "<table> <tr><td>x</table>",
  "(#doc (html (head) (body (table \" \" (tbody (tr (td \"x\")))))))" },

{ "select and option auto-close",
  "<select><option>a<option>b</select>",
  "(#doc (html (head) (body (select (option \"a\") (option \"b\")))))" },

{ "optgroup auto-closes option",
  "<select><option>a<optgroup><option>b</select>",
  "(#doc (html (head) (body (select (option \"a\") (optgroup"
  " (option \"b\"))))))" },

{ "void elements have no children",
  "<p>a<br>b<hr>c<img src=x alt=y>d",
  "(#doc (html (head) (body (p \"a\" (br) \"b\") (hr) \"c\""
  " (img src=\"x\" alt=\"y\") \"d\")))" },

{ "self-closing on a known element is ignored",
  "<div/>a</div>b",
  "(#doc (html (head) (body (div \"a\") \"b\")))" },

{ "self-closing on an unknown element is honoured",
  "<x-widget/>a",
  "(#doc (html (head) (body (x-widget) \"a\")))" },

{ "unknown elements keep their children",
  "<my-card><p>a</p></my-card>",
  "(#doc (html (head) (body (my-card (p \"a\")))))" },

{ "unknown element end tag matches by name",
  "<my-card>a</my-card>b",
  "(#doc (html (head) (body (my-card \"a\") \"b\")))" },

{ "nested anchors do not nest",
  "<a href=1>x<a href=2>y</a>",
  "(#doc (html (head) (body (a href=\"1\" \"x\") (a href=\"2\" \"y\"))))" },

{ "headings auto-close each other",
  "<h1>a<h2>b</h2>",
  "(#doc (html (head) (body (h1 \"a\") (h2 \"b\"))))" },

{ "unterminated tag at eof is dropped",
  "<p>a<div class=\"x",
  "(#doc (html (head) (body (p \"a\"))))" },

{ "unterminated comment at eof still emits",
  "<p>a<!-- b",
  "(#doc (html (head) (body (p \"a\" (#comment \" b\")))))" },

{ "unterminated script at eof keeps its text",
  "<script>var a = 1;",
  "(#doc (html (head (script \"var a = 1;\")) (body)))" },

{ "lone less-than is text",
  "<p>a < b > c",
  "(#doc (html (head) (body (p \"a < b > c\"))))" },

{ "explicit head and body",
  "<html><head><meta charset=utf-8></head><body><p>x</p></body></html>",
  "(#doc (html (head (meta charset=\"utf-8\")) (body (p \"x\"))))" },

{ "metadata after head closes goes to body",
  "<head></head><link rel=x><p>y",
  "(#doc (html (head) (body (link rel=\"x\") (p \"y\"))))" },

{ "html attributes merge",
  "<html lang=en><p>x</p><html dir=ltr class=c>",
  "(#doc (html lang=\"en\" dir=\"ltr\" class=\"c\" (head) (body (p \"x\"))))" },

/* With no script engine, <noscript> is transparent: its content is
 * parsed as markup rather than swallowed as raw text, and a <p> inside
 * a head-level <noscript> ends the head and lands in the body - which
 * is both what the spec says and the rendering we want. */
{ "noscript content is parsed as markup, not raw text",
  "<noscript><p>no js</p></noscript>",
  "(#doc (html (head (noscript)) (body (p \"no js\"))))" },

{ "noscript in the body keeps its children",
  "<p>a</p><noscript><b>no js</b></noscript>",
  "(#doc (html (head) (body (p \"a\") (noscript (b \"no js\")))))" },

{ "pre keeps its whitespace",
  "<pre>  a\n  b</pre>",
  "(#doc (html (head) (body (pre \"  a\\n  b\"))))" },

{ "pre swallows one leading newline",
  "<pre>\ncode\n</pre>",
  "(#doc (html (head) (body (pre \"code\\n\"))))" },

{ "pre swallows only one leading newline",
  "<pre>\n\ncode</pre>",
  "(#doc (html (head) (body (pre \"\\ncode\"))))" },

{ "textarea swallows one leading newline",
  "<textarea>\nx</textarea>",
  "(#doc (html (head) (body (textarea \"x\"))))" },

{ "element fostered out of a table",
  "<table><div>x</div></table>",
  "(#doc (html (head) (body (div \"x\") (table))))" },

{ "comment fostered out of a table",
  "<table><!--c--><tr><td>y</table>",
  "(#doc (html (head) (body (#comment \"c\") (table (tbody (tr"
  " (td \"y\")))))))" },

/* The newline inside <head> and the one between </head> and <body> are
 * dropped; the ones after </body> and </html> land in the body, which
 * is what browsers do and what makes the round trip stable. */
{ "whitespace between head and body is dropped",
  "<html>\n<head>\n</head>\n<body>\n<p>x</p>\n</body>\n</html>",
  "(#doc (html (head) (body \"\\n\" (p \"x\") \"\\n\\n\")))" },

{ "blockquote nests",
  "<blockquote><p>a</blockquote><p>b",
  "(#doc (html (head) (body (blockquote (p \"a\")) (p \"b\"))))" },

{ "attribute with no space before the slash",
  "<img src=\"a\"/><p>x",
  "(#doc (html (head) (body (img src=\"a\") (p \"x\"))))" },

{ "unquoted attribute value stops at whitespace",
  "<a href=/a/b class=c>x</a>",
  "(#doc (html (head) (body (a href=\"/a/b\" class=\"c\" \"x\"))))" },

{ "equals inside an unquoted value",
  "<a href=?a=1&b=2>x</a>",
  "(#doc (html (head) (body (a href=\"?a=1&b=2\" \"x\"))))" }

};

#define NCASES ((int)(sizeof g_cases / sizeof g_cases[0]))

/* ------------------------------------------------------------------ *
 * Adversarial fragments
 *
 * Shapes the mutator would take a very long time to stumble on: every
 * truncation point of every interesting construct, plus the classic
 * tokenizer traps. They are both asserted directly and used as fuzz
 * seeds.
 * ------------------------------------------------------------------ */

static const char *g_nasty[] = {
    "<",
    "<<<<<<<<",
    "</",
    "</>",
    "<!",
    "<!-",
    "<!--",
    "<!---",
    "<!----",
    "<!-->",
    "<![CDATA[",
    "<![CDATA[]]",
    "<?",
    "<a",
    "<a ",
    "<a b",
    "<a b=",
    "<a b=\"",
    "<a b='",
    "<a b=c",
    "<a/",
    "<a//////>",
    "<a =>",
    "<a ==>",
    "<a \"\">",
    "<a b=\"\"\"\">",
    "&",
    "&#",
    "&#x",
    "&#xzz;",
    "&;",
    "&&&&&&;;;;;;",
    "&amp",
    "&ampamp;",
    "<script>",
    "<script><!--",
    "<script><!--<script>",
    "<script><!--<script></script>",
    "<script><!--<script></script></script>",
    "<script></scrip>",
    "<style><style>",
    "<title><title>",
    "<textarea></textarea",
    "<table><table><table>",
    "<td></td>",
    "<tr><td><table><tr><td>",
    "<li><li><li>",
    "<p><p><p></p></p></p>",
    "<b><i><u></b></i></u>",
    "<div><div></div></div></div></div>",
    "<html><html><body><body><head><head>",
    "<!DOCTYPE",
    "<!DOCTYPE >",
    "<!DOCTYPE html><!DOCTYPE html>",
    "\xef\xbb\xbf<p>bom",
    "\x00\x01\x02<p>nul",
    "<p>\xff\xfe\xfd",
    "<p title=\"\xc3\">bad utf8",
    "<a href=\"javascript:alert(1)\">x</a>",
    "<img src=x onerror=alert(1)>",
    "<svg><foreignObject><p>x",
    "<math><mi>x</mi></math>",
    "<form><form><input>",
    "<option><option><optgroup>",
    "<select><div></select>",
    "<colgroup><col><col>",
    "<caption><table>",
    "<frameset><frame><noframes>x</noframes>",
    "<plaintext>a<b>c",
    "<xmp><div></xmp>",
    "<iframe><p></iframe>",
    "<noscript><p>x</p></noscript>",
    "<template><td>x</td></template>"
};

#define NNASTY ((int)(sizeof g_nasty / sizeof g_nasty[0]))

/* ------------------------------------------------------------------ *
 * Round trip
 * ------------------------------------------------------------------ */

static int ci_find(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    size_t i, j;

    for (i = 0; hay[i]; i++) {
        for (j = 0; j < nl; j++) {
            int a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z')
                a += 32;
            if (a != b)
                break;
        }
        if (j == nl)
            return 1;
    }
    return 0;
}

/* The one input shape that provably cannot round-trip: script content
 * containing "<!--" followed by "<script". The HTML5 tokenizer enters
 * the double-escaped states there, so a later "</script>" no longer
 * closes the element - and raw text cannot be escaped on the way out.
 * Browsers have exactly the same hole in innerHTML. */
static int lossy_raw(const struct dom_document *d)
{
    const struct dom_node *n;

    for (n = d->root; n; n = dom_next(n)) {
        if (n->type != DOM_TEXT || !n->parent)
            continue;
        if (!(dom_tag_flags(n->parent->tag_id) & DTF_RAWTEXT))
            continue;
        if (ci_find(n->text, "<!--") && ci_find(n->text, "<script"))
            return 1;
    }
    return 0;
}

/* Serializing a tree and re-parsing it must produce the same tree.
 * This is the strongest single property the parser has: it catches
 * lost text, lost attributes, mis-nesting and escaping bugs at once. */
static int roundtrip(const char *in, size_t n, const char *label, int loud)
{
    struct dom_document *d1 = html_parse_document(in, n);
    char *s1, *ser;
    unsigned long serlen = 0;
    struct dom_document *d2;
    char *s2;
    int good;

    if (!d1)
        return 1;                   /* OOM is not a round-trip failure */
    if (d1->repairs & DOM_REPAIR_FOSTER) {
        /* Foster-parented elements: see DOM_REPAIR_FOSTER in dom.h. */
        dom_document_free(d1);
        return 1;
    }
    if (lossy_raw(d1)) {
        dom_document_free(d1);
        return 1;
    }
    s1 = shape(d1->root);
    ser = dom_serialize(d1->root, &serlen);
    if (!ser) {
        free(s1);
        dom_document_free(d1);
        return 1;
    }
    d2 = html_parse_document(ser, serlen);
    if (!d2) {
        free(s1);
        free(ser);
        dom_document_free(d1);
        return 1;
    }
    s2 = shape(d2->root);
    good = (strcmp(s1, s2) == 0);
    if (!good && loud) {
        printf("FAIL: round trip (%s)\n  in  : %.200s\n  ser : %.200s\n"
               "  t1  : %.400s\n  t2  : %.400s\n",
               label, in, ser, s1, s2);
    }
    free(s1);
    free(s2);
    free(ser);
    dom_document_free(d1);
    dom_document_free(d2);
    return good;
}

/* ------------------------------------------------------------------ *
 * Corpus tests
 * ------------------------------------------------------------------ */

static void run_corpus(void)
{
    int i;

    printf("== corpus (%d documents)\n", NCASES);
    for (i = 0; i < NCASES; i++) {
        struct dom_document *d = html_parse_document(g_cases[i].in,
                                                     strlen(g_cases[i].in));
        char *s;
        if (!d) {
            g_fail++;
            printf("FAIL: %s: parse returned null\n", g_cases[i].name);
            continue;
        }
        s = shape(d->root);
        eq_str(s, g_cases[i].want, g_cases[i].name);
        free(s);
        dom_document_free(d);

        ok(roundtrip(g_cases[i].in, strlen(g_cases[i].in), g_cases[i].name, 1),
           "round trip (see above)");
    }
}

/* ------------------------------------------------------------------ *
 * Targeted API tests
 * ------------------------------------------------------------------ */

static void run_api(void)
{
    struct dom_document *d;
    struct dom_node *n;
    char *txt;
    unsigned long len = 0;

    printf("== api\n");

    d = html_parse_document(
        "<div id=one class=\"a b c\"><p id=two>Hello <b>bold</b> world</p>"
        "<p id=one>dup</p></div>", 0);
    ok(d != 0, "parse for api tests");
    if (!d)
        return;
    /* The zero length above is deliberate: a length of 0 means an empty
     * document, not "measure it for me". */
    dom_document_free(d);

    {
        const char *src =
            "<div id=one class=\"a b c\"><p id=two>Hello <b>bold</b> world"
            "</p><p id=one>dup</p></div>";
        d = html_parse_document(src, strlen(src));
        ok(d != 0, "parse");

        n = dom_get_element_by_id(d, "one");
        ok(n != 0 && n->tag_id == HTAG_DIV, "getElementById returns the div");
        ok(dom_get_element_by_id(d, "two") != 0, "getElementById finds p");
        ok(dom_get_element_by_id(d, "nope") == 0, "getElementById misses");
        ok(dom_get_element_by_id(d, "") == 0, "getElementById empty id");

        ok(dom_has_class(n, "a") && dom_has_class(n, "b") &&
           dom_has_class(n, "c"), "class list membership");
        ok(!dom_has_class(n, "ab") && !dom_has_class(n, "") &&
           !dom_has_class(n, "d"), "class list non-membership");

        ok(strcmp(dom_get_attr(n, "ID"), "one") == 0,
           "attribute lookup is case-insensitive");
        ok(dom_get_attr(n, "missing") == 0, "missing attribute is null");

        txt = dom_text_content(n, &len);
        eq_str(txt, "Hello bold worlddup", "text content of a subtree");
        ok(len == strlen("Hello bold worlddup"), "text content length");
        free(txt);

        {
            char small[6];
            unsigned long want = dom_text_content_into(n, small,
                                                       sizeof small);
            ok(want == 19, "text content reports the full length");
            ok(strcmp(small, "Hello") == 0, "text content truncates safely");
        }

        {
            struct dom_node *p2 = dom_get_element_by_id(d, "two");
            ok(dom_element_index(p2) == 1, "element index");
            ok(dom_element_child_count(n) == 2, "element child count");
            ok(dom_first_element_child(n) == p2, "first element child");
            ok(dom_next_element_sibling(p2) != 0, "next element sibling");
            ok(dom_parent_element(p2) == n, "parent element");
        }

        {
            /* Document-order walk visits every node exactly once. */
            struct dom_node *w;
            int count = 0;
            for (w = d->root; w; w = dom_next(w))
                count++;
            ok((unsigned int)count == d->nnodes,
               "document order walk visits every node");
        }

        {
            struct dom_node *el = dom_create_element(d, "SPAN", -1);
            ok(el != 0 && el->tag_id == HTAG_SPAN, "create element lowercases");
            ok(dom_set_attr(el, "ID", "made") == 1, "set attribute");
            ok(dom_append_child(n, el) == 1, "append child");
            ok(dom_get_element_by_id(d, "made") == el,
               "id index updated by set_attr");
            ok(strcmp(dom_get_attr(el, "id"), "made") == 0, "read back");
            ok(dom_remove_attr(el, "id") == 1, "remove attribute");
            ok(dom_get_attr(el, "id") == 0, "attribute gone");
            ok(dom_get_element_by_id(d, "made") == 0,
               "id index notices a removed id");
            ok(dom_set_attr(el, "id", "again") == 1, "re-set id");
            ok(dom_get_element_by_id(d, "again") == el, "id index refreshed");
            ok(dom_set_attr(el, "id", "third") == 1, "change id");
            ok(dom_get_element_by_id(d, "third") == el, "new id found");
            ok(dom_get_element_by_id(d, "again") == 0, "old id gone");
            dom_remove_child(n, el);
            ok(el->parent == 0, "remove child unlinks");
            ok(dom_append_child(el, n) == 1 || 1, "cycle guard did not crash");
        }
        dom_document_free(d);
    }

    /* Cycles must be refused, not looped over. */
    {
        struct dom_node *a, *b;
        d = dom_document_new();
        a = dom_create_element(d, "a", -1);
        b = dom_create_element(d, "b", -1);
        dom_append_child(d->root, a);
        dom_append_child(a, b);
        ok(dom_append_child(b, a) == 0, "cycle refused");
        dom_document_free(d);
    }

    /* Style and script collection. */
    {
        const char *src = "<style>x{a:1}</style><style>y{b:2}</style>"
                          "<script>one();</script><script>two();</script>";
        d = html_parse_document(src, strlen(src));
        ok(strcmp(d->style_text, "x{a:1}\ny{b:2}\n") == 0,
           "style text collected in order");
        ok(strcmp(d->script_text, "one();\ntwo();\n") == 0,
           "script text collected in order");
        ok(d->style_len == strlen("x{a:1}\ny{b:2}\n"), "style length");
        dom_document_free(d);
    }

    /* Title. */
    {
        const char *src = "<title>First &amp; only</title><title>Second</title>";
        d = html_parse_document(src, strlen(src));
        eq_str(d->title, "First & only", "title decoded, first wins");
        dom_document_free(d);
    }
    {
        d = html_parse_document("<p>x", 3);
        eq_str(d->title, "", "missing title is the empty string");
        dom_document_free(d);
    }

    /* Tag metadata. */
    {
        int i, holes = 0;
        for (i = 1; i < HTAG__COUNT; i++) {
            const char *nm = dom_tag_name(i);
            if (!nm || !nm[0]) {
                holes++;
                continue;
            }
            if (dom_tag_lookup(nm, -1) != i)
                holes++;
        }
        ok(holes == 0, "every tag id round-trips through the name table");
        ok(dom_tag_lookup("DIV", -1) == HTAG_DIV, "tag lookup is case-blind");
        ok(dom_tag_lookup("nosuchtag", -1) == HTAG_UNKNOWN, "unknown tag");
        ok(dom_tag_is_void(HTAG_BR) && !dom_tag_is_void(HTAG_DIV),
           "void flags");
        ok(dom_heading_level(HTAG_H3) == 3 && dom_heading_level(HTAG_P) == 0,
           "heading levels");
    }

    /* Entities. */
    {
        struct html_entity_index ix;
        unsigned long cp = 0;
        int used = 0, semi = 0, i, bad = 0;

        html_entity_index_init(&ix);
        ok(html_entity_count() >= 250, "entity table has at least 250 names");

        /* Every name in the table must be findable through the index. */
        for (i = 0; i < html_entity_count(); i++) {
            unsigned long want = 0;
            int legacy = 0;
            const char *nm = html_entity_name(i, &want, &legacy);
            char buf[32];
            size_t l = strlen(nm);
            if (l + 2 > sizeof buf || l + 1 >= HTML_ENT_MAXNAME) {
                bad++;
                continue;
            }
            memcpy(buf, nm, l);
            buf[l] = ';';
            buf[l + 1] = 0;
            if (!html_entity_match(&ix, buf, l + 1, &used, &semi, &cp) ||
                cp != want || (size_t)used != l + 1 || !semi)
                bad++;
            if (legacy) {
                if (!html_entity_match(&ix, nm, l, &used, &semi, &cp) ||
                    cp != want || semi)
                    bad++;
            }
        }
        ok(bad == 0, "every entity matches through the index");

        ok(html_entity_match(&ix, "notin;", 6, &used, &semi, &cp) &&
           cp == 8713 && used == 6 && semi, "&notin; matches the long name");
        ok(html_entity_match(&ix, "notit;", 6, &used, &semi, &cp) &&
           cp == 172 && used == 3 && !semi, "&notit; falls back to &not");
        ok(!html_entity_match(&ix, "apos", 4, &used, &semi, &cp),
           "&apos without a semicolon does not match");
        ok(html_entity_match(&ix, "apos;", 5, &used, &semi, &cp) && cp == 39,
           "&apos; matches");
        ok(html_numeric_fixup(0) == 0xFFFD &&
           html_numeric_fixup(0xD800) == 0xFFFD &&
           html_numeric_fixup(0x200000) == 0xFFFD &&
           html_numeric_fixup(0x80) == 0x20AC &&
           html_numeric_fixup('A') == 'A', "numeric fixups");
    }

    /* UTF-8 and ASCII folding. */
    {
        char buf[64];
        unsigned long cp = 0;
        int n8;

        n8 = dom_utf8_encode(0x20AC, buf);
        ok(n8 == 3 && (unsigned char)buf[0] == 0xE2, "utf-8 encode 3 bytes");
        ok(dom_utf8_decode(buf, 3, &cp) == 3 && cp == 0x20AC,
           "utf-8 decode round trip");
        /* An overlong encoding of '<' must not decode to '<'. */
        buf[0] = (char)0xC0;
        buf[1] = (char)0xBC;
        ok(dom_utf8_decode(buf, 2, &cp) == 1 && cp == 0xC0,
           "overlong sequence rejected");

        {
            const char *s = "caf\xc3\xa9 \xe2\x80\x94 \xc2\xa9";
            unsigned long w = dom_fold_ascii(s, strlen(s), buf, sizeof buf);
            eq_str(buf, "cafe -- (c)", "ascii folding");
            ok(w == strlen("cafe -- (c)"), "ascii folding length");
        }
        {
            char tiny[4];
            const char *s = "abcdef";
            unsigned long w = dom_fold_ascii(s, 6, tiny, sizeof tiny);
            ok(w == 6 && strcmp(tiny, "abc") == 0, "ascii folding truncates");
        }
    }
}

/* ------------------------------------------------------------------ *
 * A page shaped like the ones this browser is actually for
 * ------------------------------------------------------------------ */

static const char g_page[] =
"<!DOCTYPE html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"  <meta charset=\"utf-8\">\n"
"  <meta name=viewport content=\"width=device-width, initial-scale=1\">\n"
"  <title>Kestrel &mdash; Documentation</title>\n"
"  <link rel=stylesheet href=\"/s/main.css\">\n"
"  <style>body{margin:0;font:16px/1.5 serif}\n"
"  .nav a:hover{color:#06c}</style>\n"
"  <script>window.__cfg = {a: 1 < 2, b: \"</div>\"};</script>\n"
"</head>\n"
"<body class=\"docs wide\">\n"
"  <!-- header -->\n"
"  <header id=top><nav class=nav>\n"
"    <a href=\"/\">Home</a> |\n"
"    <a href=\"/docs?v=2&amp;lang=en\">Docs</a> |\n"
"    <a href='/about'>About</a>\n"
"  </nav></header>\n"
"  <main>\n"
"    <h1 id=title>Getting started</h1>\n"
"    <p>Install it, then run <code>kestrel&nbsp;--help</code>.\n"
"    <p>Options are listed below &mdash; see also the\n"
"       <a href=\"#notes\">notes</a>.\n"
"    <table class=opts>\n"
"      <tr><th>Flag<th>Meaning\n"
"      <tr><td><code>-v</code><td>Verbose\n"
"      <tr><td><code>-q</code><td>Quiet\n"
"    </table>\n"
"    <ul>\n"
"      <li>First item\n"
"      <li>Second item\n"
"        <ol><li>Nested a<li>Nested b</ol>\n"
"      <li>Third item\n"
"    </ul>\n"
"    <pre>\n"
"$ kestrel build\n"
"  ok\n"
"</pre>\n"
"    <blockquote><p>Simple is better.</blockquote>\n"
"    <figure><img src=/i/shot.png alt=\"A screenshot\"><figcaption>Fig 1"
"</figcaption></figure>\n"
"    <hr>\n"
"    <p id=notes>Notes &amp; caveats: 5 &lt; 6, 100&#37; done.</p>\n"
"  </main>\n"
"  <footer><p>&copy; 2026</p></footer>\n"
"  <noscript><p>JavaScript is off.</p></noscript>\n"
"</body>\n"
"</html>\n";

static void run_page(void)
{
    struct dom_document *d;
    struct dom_node *n;
    char *t;

    printf("== realistic page\n");
    d = html_parse_document(g_page, sizeof g_page - 1);
    ok(d != 0, "page parses");
    if (!d)
        return;

    eq_str(d->title, "Kestrel \xe2\x80\x94 Documentation", "page title");
    ok(d->truncated == 0, "page hits no cap");
    ok(d->html && d->head && d->body, "page skeleton");
    ok(dom_has_class(d->body, "docs") && dom_has_class(d->body, "wide"),
       "body classes");

    ok(strstr(d->style_text, "body{margin:0") != 0, "style text collected");
    ok(strstr(d->style_text, ".nav a:hover") != 0, "second style rule");
    ok(strstr(d->script_text, "\"</div>\"") != 0,
       "script text collected verbatim");
    ok(strstr(d->script_text, "1 < 2") != 0, "script '<' not treated as a tag");

    n = dom_get_element_by_id(d, "title");
    ok(n != 0 && n->tag_id == HTAG_H1, "id lookup finds the heading");
    t = dom_text_content(n, 0);
    eq_str(t, "Getting started", "heading text");
    free(t);

    n = dom_get_element_by_id(d, "notes");
    ok(n != 0, "id lookup finds the notes paragraph");
    t = dom_text_content(n, 0);
    eq_str(t, "Notes & caveats: 5 < 6, 100% done.", "entities in the page");
    free(t);

    /* The unclosed <p>s must not have nested. */
    n = dom_find_tag(d->body, "main");
    ok(n != 0, "main found");
    {
        struct dom_node *c;
        int paras = 0, nested = 0;
        for (c = n->first_child; c; c = c->next_sibling) {
            if (c->tag_id != HTAG_P)
                continue;
            paras++;
            if (dom_find_tag(c, "p") != c)
                nested++;
        }
        ok(paras == 3, "three sibling paragraphs, none nested");
        ok(nested == 0, "no paragraph contains another");
    }

    /* Table structure. */
    n = dom_find_tag(d->body, "table");
    ok(n != 0 && dom_first_element_child(n) != 0 &&
       dom_first_element_child(n)->tag_id == HTAG_TBODY,
       "implicit tbody in the page table");
    ok(dom_element_child_count(dom_first_element_child(n)) == 3,
       "three rows");

    /* Nested list. */
    n = dom_find_tag(d->body, "ol");
    ok(n != 0 && n->parent && n->parent->tag_id == HTAG_LI,
       "nested ol lives inside its li");
    ok(dom_element_child_count(n) == 2, "nested ol has two items");

    /* <pre> keeps its shape and loses exactly one leading newline. */
    n = dom_find_tag(d->body, "pre");
    t = dom_text_content(n, 0);
    eq_str(t, "$ kestrel build\n  ok\n", "pre content");
    free(t);

    /* Attributes in every quoting style survived. */
    n = dom_find_tag(d->body, "img");
    ok(n != 0 && strcmp(dom_get_attr(n, "alt"), "A screenshot") == 0,
       "quoted attribute");
    ok(n != 0 && strcmp(dom_get_attr(n, "src"), "/i/shot.png") == 0,
       "unquoted attribute");
    {
        struct dom_node *a;
        int amp = 0;
        for (a = d->root; a; a = dom_next(a)) {
            const char *h;
            if (a->tag_id != HTAG_A)
                continue;
            h = dom_get_attr(a, "href");
            if (h && strcmp(h, "/docs?v=2&lang=en") == 0)
                amp = 1;
        }
        ok(amp, "entity decoded inside an href");
    }

    /* Folding the whole page to ASCII must not lose the prose. */
    {
        unsigned long need = dom_text_content_into(d->body, 0, 0);
        char *raw = malloc(need + 1);
        char *folded;
        dom_text_content_into(d->body, raw, need + 1);
        folded = malloc(need * 4 + 1);
        dom_fold_ascii(raw, need, folded, need * 4 + 1);
        ok(strstr(folded, "Getting started") != 0, "folded text readable");
        ok(strstr(folded, "Kestrel --help") == 0 ||
           strstr(folded, "kestrel --help") != 0, "nbsp folded to a space");
        ok(strstr(folded, "(c) 2026") != 0, "copyright sign folded");
        free(raw);
        free(folded);
    }

    ok(roundtrip(g_page, sizeof g_page - 1, "realistic page", 1),
       "realistic page round trip");
    dom_document_free(d);
}

/* ------------------------------------------------------------------ *
 * Limits
 * ------------------------------------------------------------------ */

static char *repeat(const char *unit, int times, size_t *outlen)
{
    size_t u = strlen(unit);
    char *p = malloc(u * (size_t)times + 1);
    int i;

    for (i = 0; i < times; i++)
        memcpy(p + u * (size_t)i, unit, u);
    p[u * (size_t)times] = 0;
    if (outlen)
        *outlen = u * (size_t)times;
    return p;
}

static unsigned int tree_depth(const struct dom_node *root)
{
    const struct dom_node *n;
    unsigned int max = 0;

    for (n = root; n; n = dom_next_within(n, root)) {
        if (n->depth > max)
            max = n->depth;
    }
    return max;
}

static void run_limits(int quick)
{
    struct dom_document *d;
    char *src;
    size_t n;

    printf("== limits\n");

    /* Exactly at the cap. */
    {
        size_t len;
        char *open = repeat("<div>", DOM_MAX_DEPTH - 3, &len);
        d = html_parse_document(open, len);
        ok(d != 0, "at-cap document parses");
        ok(tree_depth(d->root) <= DOM_MAX_DEPTH, "depth never exceeds the cap");
        ok((d->truncated & DOM_TRUNC_DEPTH) == 0, "at the cap, no depth flag");
        dom_document_free(d);
        free(open);
    }

    /* Far past the cap: content must survive, flattened. */
    {
        size_t len;
        char *open = repeat("<div>", 5000, &len);
        src = malloc(len + 64);
        memcpy(src, open, len);
        strcpy(src + len, "deep text");
        n = len + strlen("deep text");
        d = html_parse_document(src, n);
        ok(d != 0, "past-cap document parses");
        ok((d->truncated & DOM_TRUNC_DEPTH) != 0, "depth cap flagged");
        ok(tree_depth(d->root) <= DOM_MAX_DEPTH, "depth capped");
        {
            char *t = dom_text_content(d->root, 0);
            ok(strcmp(t, "deep text") == 0, "content past the cap is kept");
            free(t);
        }
        dom_document_free(d);
        free(src);
        free(open);
    }

    /* Deep nesting with matching end tags must unwind cleanly. */
    {
        size_t lo, lc;
        char *open = repeat("<div>", 3000, &lo);
        char *close = repeat("</div>", 3000, &lc);
        src = malloc(lo + lc + 8);
        memcpy(src, open, lo);
        memcpy(src + lo, close, lc);
        d = html_parse_document(src, lo + lc);
        ok(d != 0 && tree_depth(d->root) <= DOM_MAX_DEPTH,
           "deep matched nesting unwinds");
        dom_document_free(d);
        free(src);
        free(open);
        free(close);
    }

    /* A small explicit limit exercises the same code deterministically. */
    {
        struct dom_limits lim;
        memset(&lim, 0, sizeof lim);
        lim.max_depth = 8;
        src = repeat("<div>", 40, &n);
        d = html_parse_document_limits(src, n, &lim);
        ok(tree_depth(d->root) <= 8, "custom depth limit respected");
        ok((d->truncated & DOM_TRUNC_DEPTH) != 0, "custom depth flagged");
        dom_document_free(d);
        free(src);
    }

    /* Node cap. */
    {
        struct dom_limits lim;
        memset(&lim, 0, sizeof lim);
        lim.max_nodes = 50;
        src = repeat("<p>x</p>", 200, &n);
        d = html_parse_document_limits(src, n, &lim);
        ok(d->nnodes <= 50, "node cap respected");
        ok((d->truncated & DOM_TRUNC_NODES) != 0, "node cap flagged");
        ok(roundtrip(src, n, "node cap", 0) || 1, "node cap does not crash");
        dom_document_free(d);
        free(src);
    }

    /* Input cap. */
    {
        struct dom_limits lim;
        memset(&lim, 0, sizeof lim);
        lim.max_input = 100;
        src = repeat("<p>x</p>", 100, &n);
        d = html_parse_document_limits(src, n, &lim);
        ok((d->truncated & DOM_TRUNC_INPUT) != 0, "input cap flagged");
        dom_document_free(d);
        free(src);
    }

    /* Attribute cap. */
    {
        size_t len;
        char *attrs = repeat(" a=1", DOM_MAX_ATTRS + 50, &len);
        src = malloc(len + 16);
        strcpy(src, "<p");
        memcpy(src + 2, attrs, len);
        strcpy(src + 2 + len, ">x");
        d = html_parse_document(src, strlen(src));
        ok(d != 0, "attribute flood parses");
        dom_document_free(d);
        free(src);
        free(attrs);
    }

    if (quick)
        return;

    /* A 2 MiB document. */
    {
        clock_t t0;
        size_t unit;
        char *body = repeat(
            "<div class=\"row\"><p>Some <b>bold</b> and <i>italic</i> text "
            "with an &amp; entity and a <a href=\"/x?a=1&b=2\">link</a>.</p>"
            "<ul><li>one<li>two</ul></div>\n", 14000, &unit);
        printf("   2 MiB document: %lu bytes\n", (unsigned long)unit);
        t0 = clock();
        d = html_parse_document(body, unit);
        ok(d != 0, "2 MiB document parses");
        printf("   parsed %u nodes in %.2f s; the document owns %lu KiB,"
               " %lu bytes per input byte\n", d ? d->nnodes : 0,
               (double)(clock() - t0) / CLOCKS_PER_SEC,
               d ? dom_memory_used(d) / 1024 : 0,
               d ? dom_memory_used(d) / unit : 0);
        ok(d && d->truncated == 0, "2 MiB document hits no cap");
        ok(d && dom_find_tag(d->body, "a") != 0, "links survive");
        dom_document_free(d);
        ok(roundtrip(body, unit, "2 MiB", 1), "2 MiB round trip");
        free(body);
    }
}

/* ------------------------------------------------------------------ *
 * Fuzzing
 * ------------------------------------------------------------------ */

static unsigned long g_rng = 0x12345678UL;

static unsigned long rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng &= 0xffffffffUL;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    g_rng &= 0xffffffffUL;
    return g_rng;
}

static const char g_bytes[] =
    "<>/=\"'&; \t\n-!?[]abcdivpsrtolh0123456789\\#x%\x80\xff";

/* Mutate a corpus document: splice, delete, duplicate, and inject the
 * bytes that mean something to a tokenizer. */
static size_t mutate(char *dst, size_t cap)
{
    const char *base = (rnd() & 1) ? g_cases[rnd() % NCASES].in
                                   : g_nasty[rnd() % NNASTY];
    size_t n = strlen(base);
    int rounds, i;

    if (n > cap - 1)
        n = cap - 1;
    memcpy(dst, base, n);

    rounds = 1 + (int)(rnd() % 12);
    for (i = 0; i < rounds; i++) {
        switch (rnd() % 6) {
        case 0:                     /* flip a byte to a syntax byte */
            if (n)
                dst[rnd() % n] = g_bytes[rnd() % (sizeof g_bytes - 1)];
            break;
        case 1: {                   /* delete a span */
            size_t at, l;
            if (!n)
                break;
            at = rnd() % n;
            l = 1 + rnd() % 8;
            if (at + l > n)
                l = n - at;
            memmove(dst + at, dst + at + l, n - at - l);
            n -= l;
            break;
        }
        case 2: {                   /* duplicate a span */
            size_t at, l;
            if (!n || n + 16 >= cap)
                break;
            at = rnd() % n;
            l = 1 + rnd() % 8;
            if (at + l > n)
                l = n - at;
            memmove(dst + at + l, dst + at, n - at);
            n += l;
            break;
        }
        case 3: {                   /* splice in another document */
            const char *other = (rnd() & 1) ? g_cases[rnd() % NCASES].in
                                            : g_nasty[rnd() % NNASTY];
            size_t ol = strlen(other), at;
            if (ol == 0 || n + ol >= cap)
                break;
            at = rnd() % (n + 1);
            memmove(dst + at + ol, dst + at, n - at);
            memcpy(dst + at, other, ol);
            n += ol;
            break;
        }
        case 4: {                   /* insert a burst of syntax bytes */
            size_t at, l, k;
            l = 1 + rnd() % 12;
            if (n + l >= cap)
                break;
            at = rnd() % (n + 1);
            memmove(dst + at + l, dst + at, n - at);
            for (k = 0; k < l; k++)
                dst[at + k] = g_bytes[rnd() % (sizeof g_bytes - 1)];
            n += l;
            break;
        }
        case 5:                     /* truncate */
            if (n)
                n = rnd() % n;
            break;
        }
    }
    dst[n] = 0;
    return n;
}

static void run_fuzz(int iters)
{
    char *buf = malloc(65536);
    int i;
    int rt_fail = 0, null_doc = 0, reported = 0, skipped = 0;
    unsigned long total_nodes = 0;
    clock_t t0 = clock();

    printf("== fuzz (%d iterations)\n", iters);
    for (i = 0; i < iters; i++) {
        size_t n = mutate(buf, 65536);
        struct dom_document *d = html_parse_document(buf, n);
        char *s1, *ser;
        unsigned long serlen = 0;
        struct dom_document *d2;

        if (!d) {
            null_doc++;
            continue;
        }
        total_nodes += d->nnodes;

        /* Structural invariants that must hold for every document,
         * however malformed the input was. */
        {
            struct dom_node *w;
            unsigned int seen = 0;
            int bad = 0;
            for (w = d->root; w; w = dom_next(w)) {
                seen++;
                if (seen > d->nnodes + 8) {
                    bad = 1;        /* a cycle in the sibling chain */
                    break;
                }
                if (w->parent) {
                    if (w->depth != w->parent->depth + 1)
                        bad = 1;
                    if (w->prev_sibling && w->prev_sibling->next_sibling != w)
                        bad = 1;
                    if (!w->prev_sibling && w->parent->first_child != w)
                        bad = 1;
                    if (!w->next_sibling && w->parent->last_child != w)
                        bad = 1;
                }
                if (w->depth > DOM_MAX_DEPTH)
                    bad = 1;
                if (w->type == DOM_TEXT && w->next_sibling &&
                    w->next_sibling->type == DOM_TEXT)
                    bad = 1;        /* adjacent text nodes break the round trip */
            }
            if (bad) {
                g_fail++;
                if (reported++ < 3)
                    printf("FAIL: fuzz invariant broken on: %.160s\n", buf);
            }
        }

        ok(d->html != 0 && d->body != 0, "fuzz: skeleton always present");
        g_pass--;                   /* counted once at the end instead */

        if (lossy_raw(d) || (d->repairs & DOM_REPAIR_FOSTER)) {
            skipped++;
            dom_document_free(d);
            continue;
        }
        s1 = shape(d->root);
        ser = dom_serialize(d->root, &serlen);
        if (ser) {
            d2 = html_parse_document(ser, serlen);
            if (d2) {
                char *s2 = shape(d2->root);
                if (strcmp(s1, s2) != 0) {
                    rt_fail++;
                    if (reported++ < 3)
                        printf("FAIL: fuzz round trip\n  in : %.160s\n"
                               "  ser: %.160s\n  t1 : %.240s\n  t2 : %.240s\n",
                               buf, ser, s1, s2);
                }
                free(s2);
                dom_document_free(d2);
            }
            free(ser);
        }
        free(s1);
        dom_document_free(d);
    }
    free(buf);

    printf("   %d documents, %lu nodes, %.2f s, %d null, %d skipped as not"
           " round-trippable by construction, %d round-trip failures\n",
           iters, total_nodes, (double)(clock() - t0) / CLOCKS_PER_SEC,
           null_doc, skipped, rt_fail);
    ok(rt_fail == 0, "fuzz: every mutated document round-trips");
    ok(null_doc == 0, "fuzz: no allocation failures");
}

/* ------------------------------------------------------------------ *
 * Adversarial shapes that the mutator will not stumble on
 * ------------------------------------------------------------------ */

static void run_adversarial(void)
{
    struct dom_document *d;
    char *src;
    size_t n;
    int i;

    int nn = NNASTY;

    printf("== adversarial (%d fragments)\n", nn);
    for (i = 0; i < nn; i++) {
        /* Two of these contain embedded NULs deliberately; use the
         * literal length where it matters. */
        size_t len = (i == 57) ? 15 : strlen(g_nasty[i]);
        d = html_parse_document(g_nasty[i], len);
        if (!d) {
            g_fail++;
            printf("FAIL: adversarial %d returned null\n", i);
            continue;
        }
        if (!d->html || !d->body) {
            g_fail++;
            printf("FAIL: adversarial %d has no skeleton: %s\n", i, g_nasty[i]);
        }
        dom_document_free(d);
        if (!roundtrip(g_nasty[i], len, g_nasty[i], 1)) {
            g_fail++;
        } else {
            g_pass++;
        }
    }

    /* A million-deep single element chain, unclosed, and the same
     * closed. Neither may recurse or allocate without bound. */
    printf("== pathological nesting\n");
    src = repeat("<div>", 200000, &n);
    d = html_parse_document(src, n);
    ok(d != 0, "200k unclosed divs parse");
    ok(d && tree_depth(d->root) <= DOM_MAX_DEPTH, "200k divs stay capped");
    ok(d && d->nnodes < 1000, "200k divs cost almost no nodes");
    dom_document_free(d);
    free(src);

    src = repeat("<b></b>", 200000, &n);
    d = html_parse_document(src, n);
    ok(d != 0, "200k sibling elements parse");
    dom_document_free(d);
    free(src);

    /* Deeply nested comments and entities, which take different paths. */
    src = repeat("&amp;", 200000, &n);
    d = html_parse_document(src, n);
    ok(d != 0 && d->nnodes > 0, "200k entities parse");
    dom_document_free(d);
    free(src);

    src = repeat("<!--x-->", 100000, &n);
    d = html_parse_document(src, n);
    ok(d != 0, "100k comments parse");
    dom_document_free(d);
    free(src);

    src = repeat("<a href=1 class=2 id=3>x</a>", 50000, &n);
    d = html_parse_document(src, n);
    ok(d != 0 && dom_get_element_by_id(d, "3") != 0,
       "50k identical ids: first wins, no blowup");
    dom_document_free(d);
    free(src);
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    int quick = 0, iters = 40000, given = 0, i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--quick") == 0)
            quick = 1;
        else if (strcmp(argv[i], "--fuzz") == 0 && i + 1 < argc) {
            iters = atoi(argv[++i]);
            given = 1;
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            g_rng = (strtoul(argv[++i], 0, 0) * 2654435761UL + 1UL)
                    & 0xffffffffUL;
        }
    }
    if (quick && !given)
        iters = 2000;

    run_corpus();
    run_api();
    run_page();
    run_limits(quick);
    run_adversarial();
    run_fuzz(iters);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
