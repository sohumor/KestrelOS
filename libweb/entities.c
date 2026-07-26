/* entities.c - named and numeric character references, UTF-8, and the
 * ASCII folding the bitmap fonts need. See libweb/dom.h.
 *
 * The named-reference table below is the common subset of the HTML5
 * named character reference list: the complete legacy (HTML 4) set,
 * which is the set browsers must also match without a trailing
 * semicolon, plus the punctuation, Greek, arrow and mathematical names
 * that appear on real pages. The code points are published constants.
 *
 * Matching is longest-prefix, because that is what the spec requires:
 *   &notin;  ->  U+2209            (the name is "notin")
 *   &notit;  ->  U+00AC then "it;" (the legacy name "not" matched)
 *   &ampx    ->  U+0026 then "x"
 *   &apos    ->  no match; "apos" is not a legacy name
 */

#include <stdlib.h>
#include <string.h>

#include "dom.h"

struct ent {
    const char *name;
    unsigned short cp;      /* every entry here is inside the BMP */
    unsigned char legacy;   /* may match without a trailing ';'   */
};

static const struct ent g_ent[] = {
    /* ---- the legacy set: valid with or without the semicolon ---- */
    { "AElig",  198, 1 }, { "AMP",     38, 1 }, { "Aacute", 193, 1 },
    { "Acirc",  194, 1 }, { "Agrave", 192, 1 }, { "Aring",  197, 1 },
    { "Atilde", 195, 1 }, { "Auml",   196, 1 }, { "COPY",   169, 1 },
    { "Ccedil", 199, 1 }, { "ETH",    208, 1 }, { "Eacute", 201, 1 },
    { "Ecirc",  202, 1 }, { "Egrave", 200, 1 }, { "Euml",   203, 1 },
    { "GT",      62, 1 }, { "Iacute", 205, 1 }, { "Icirc",  206, 1 },
    { "Igrave", 204, 1 }, { "Iuml",   207, 1 }, { "LT",      60, 1 },
    { "Ntilde", 209, 1 }, { "Oacute", 211, 1 }, { "Ocirc",  212, 1 },
    { "Ograve", 210, 1 }, { "Oslash", 216, 1 }, { "Otilde", 213, 1 },
    { "Ouml",   214, 1 }, { "QUOT",    34, 1 }, { "REG",    174, 1 },
    { "THORN",  222, 1 }, { "Uacute", 218, 1 }, { "Ucirc",  219, 1 },
    { "Ugrave", 217, 1 }, { "Uuml",   220, 1 }, { "Yacute", 221, 1 },
    { "aacute", 225, 1 }, { "acirc",  226, 1 }, { "acute",  180, 1 },
    { "aelig",  230, 1 }, { "agrave", 224, 1 }, { "amp",     38, 1 },
    { "aring",  229, 1 }, { "atilde", 227, 1 }, { "auml",   228, 1 },
    { "brvbar", 166, 1 }, { "ccedil", 231, 1 }, { "cedil",  184, 1 },
    { "cent",   162, 1 }, { "copy",   169, 1 }, { "curren", 164, 1 },
    { "deg",    176, 1 }, { "divide", 247, 1 }, { "eacute", 233, 1 },
    { "ecirc",  234, 1 }, { "egrave", 232, 1 }, { "eth",    240, 1 },
    { "euml",   235, 1 }, { "frac12", 189, 1 }, { "frac14", 188, 1 },
    { "frac34", 190, 1 }, { "gt",      62, 1 }, { "iacute", 237, 1 },
    { "icirc",  238, 1 }, { "iexcl",  161, 1 }, { "igrave", 236, 1 },
    { "iquest", 191, 1 }, { "iuml",   239, 1 }, { "laquo",  171, 1 },
    { "lt",      60, 1 }, { "macr",   175, 1 }, { "micro",  181, 1 },
    { "middot", 183, 1 }, { "nbsp",   160, 1 }, { "not",    172, 1 },
    { "ntilde", 241, 1 }, { "oacute", 243, 1 }, { "ocirc",  244, 1 },
    { "ograve", 242, 1 }, { "ordf",   170, 1 }, { "ordm",   186, 1 },
    { "oslash", 248, 1 }, { "otilde", 245, 1 }, { "ouml",   246, 1 },
    { "para",   182, 1 }, { "plusmn", 177, 1 }, { "pound",  163, 1 },
    { "quot",    34, 1 }, { "raquo",  187, 1 }, { "reg",    174, 1 },
    { "sect",   167, 1 }, { "shy",    173, 1 }, { "sup1",   185, 1 },
    { "sup2",   178, 1 }, { "sup3",   179, 1 }, { "szlig",  223, 1 },
    { "thorn",  254, 1 }, { "times",  215, 1 }, { "uacute", 250, 1 },
    { "ucirc",  251, 1 }, { "ugrave", 249, 1 }, { "uml",    168, 1 },
    { "uuml",   252, 1 }, { "yacute", 253, 1 }, { "yen",    165, 1 },
    { "yuml",   255, 1 },

    /* ---- semicolon required from here down ---- */

    /* punctuation and spacing */
    { "apos",      39, 0 }, { "bdquo",   8222, 0 }, { "bull",    8226, 0 },
    { "dagger",  8224, 0 }, { "Dagger",  8225, 0 }, { "emsp",    8195, 0 },
    { "ensp",    8194, 0 }, { "euro",    8364, 0 }, { "frasl",   8260, 0 },
    { "hellip",  8230, 0 }, { "ldquo",   8220, 0 }, { "lsaquo",  8249, 0 },
    { "lsquo",   8216, 0 }, { "lrm",     8206, 0 }, { "mdash",   8212, 0 },
    { "ndash",   8211, 0 }, { "oline",   8254, 0 }, { "permil",  8240, 0 },
    { "prime",   8242, 0 }, { "Prime",   8243, 0 }, { "rdquo",   8221, 0 },
    { "rlm",     8207, 0 }, { "rsaquo",  8250, 0 }, { "rsquo",   8217, 0 },
    { "sbquo",   8218, 0 }, { "thinsp",  8201, 0 }, { "trade",   8482, 0 },
    { "zwj",     8205, 0 }, { "zwnj",    8204, 0 }, { "numsp",   8199, 0 },
    { "puncsp",  8200, 0 }, { "hairsp",  8202, 0 }, { "nldr",    8229, 0 },

    /* Latin extended and letterlike */
    { "OElig",    338, 0 }, { "oelig",    339, 0 }, { "Scaron",   352, 0 },
    { "scaron",   353, 0 }, { "Yuml",     376, 0 }, { "fnof",     402, 0 },
    { "circ",     710, 0 }, { "tilde",    732, 0 }, { "weierp",  8472, 0 },
    { "image",   8465, 0 }, { "real",    8476, 0 }, { "alefsym", 8501, 0 },
    { "ell",     8467, 0 },

    /* Greek */
    { "Alpha",    913, 0 }, { "Beta",     914, 0 }, { "Gamma",    915, 0 },
    { "Delta",    916, 0 }, { "Epsilon",  917, 0 }, { "Zeta",     918, 0 },
    { "Eta",      919, 0 }, { "Theta",    920, 0 }, { "Iota",     921, 0 },
    { "Kappa",    922, 0 }, { "Lambda",   923, 0 }, { "Mu",       924, 0 },
    { "Nu",       925, 0 }, { "Xi",       926, 0 }, { "Omicron",  927, 0 },
    { "Pi",       928, 0 }, { "Rho",      929, 0 }, { "Sigma",    931, 0 },
    { "Tau",      932, 0 }, { "Upsilon",  933, 0 }, { "Phi",      934, 0 },
    { "Chi",      935, 0 }, { "Psi",      936, 0 }, { "Omega",    937, 0 },
    { "alpha",    945, 0 }, { "beta",     946, 0 }, { "gamma",    947, 0 },
    { "delta",    948, 0 }, { "epsilon",  949, 0 }, { "zeta",     950, 0 },
    { "eta",      951, 0 }, { "theta",    952, 0 }, { "iota",     953, 0 },
    { "kappa",    954, 0 }, { "lambda",   955, 0 }, { "mu",       956, 0 },
    { "nu",       957, 0 }, { "xi",       958, 0 }, { "omicron",  959, 0 },
    { "pi",       960, 0 }, { "rho",      961, 0 }, { "sigmaf",   962, 0 },
    { "sigma",    963, 0 }, { "tau",      964, 0 }, { "upsilon",  965, 0 },
    { "phi",      966, 0 }, { "chi",      967, 0 }, { "psi",      968, 0 },
    { "omega",    969, 0 }, { "thetasym", 977, 0 }, { "upsih",    978, 0 },
    { "piv",      982, 0 },

    /* arrows */
    { "larr",    8592, 0 }, { "uarr",    8593, 0 }, { "rarr",    8594, 0 },
    { "darr",    8595, 0 }, { "harr",    8596, 0 }, { "varr",    8597, 0 },
    { "crarr",   8629, 0 }, { "lArr",    8656, 0 }, { "uArr",    8657, 0 },
    { "rArr",    8658, 0 }, { "dArr",    8659, 0 }, { "hArr",    8660, 0 },
    { "vArr",    8661, 0 }, { "nlarr",   8602, 0 }, { "nrarr",   8603, 0 },
    { "map",     8614, 0 }, { "lhard",   8637, 0 }, { "lharu",   8636, 0 },
    { "rharu",   8640, 0 }, { "rhard",   8641, 0 },

    /* mathematics */
    { "forall",  8704, 0 }, { "comp",    8705, 0 }, { "part",    8706, 0 },
    { "exist",   8707, 0 }, { "nexist",  8708, 0 }, { "empty",   8709, 0 },
    { "nabla",   8711, 0 }, { "isin",    8712, 0 }, { "notin",   8713, 0 },
    { "ni",      8715, 0 }, { "prod",    8719, 0 }, { "coprod",  8720, 0 },
    { "sum",     8721, 0 }, { "minus",   8722, 0 }, { "mnplus",  8723, 0 },
    { "lowast",  8727, 0 }, { "radic",   8730, 0 }, { "prop",    8733, 0 },
    { "infin",   8734, 0 }, { "ang",     8736, 0 }, { "and",     8743, 0 },
    { "or",      8744, 0 }, { "cap",     8745, 0 }, { "cup",     8746, 0 },
    { "int",     8747, 0 }, { "there4",  8756, 0 }, { "because", 8757, 0 },
    { "sim",     8764, 0 }, { "cong",    8773, 0 }, { "asymp",   8776, 0 },
    { "ne",      8800, 0 }, { "equiv",   8801, 0 }, { "le",      8804, 0 },
    { "ge",      8805, 0 }, { "sub",     8834, 0 }, { "sup",     8835, 0 },
    { "nsub",    8836, 0 }, { "nsup",    8837, 0 }, { "sube",    8838, 0 },
    { "supe",    8839, 0 }, { "oplus",   8853, 0 }, { "otimes",  8855, 0 },
    { "perp",    8869, 0 }, { "sdot",    8901, 0 }, { "lceil",   8968, 0 },
    { "rceil",   8969, 0 }, { "lfloor",  8970, 0 }, { "rfloor",  8971, 0 },
    { "lang",    9001, 0 }, { "rang",    9002, 0 }, { "loz",     9674, 0 },
    { "spades",  9824, 0 }, { "clubs",   9827, 0 }, { "hearts",  9829, 0 },
    { "diams",   9830, 0 }, { "star",    9734, 0 }, { "starf",   9733, 0 },
    { "check",  10003, 0 }, { "cross",  10007, 0 }, { "malt",   10016, 0 },
    { "sext",   10038, 0 }, { "lozf",    10731, 0 },

    /* box drawing and blocks that show up in ASCII-art pages */
    { "boxh",    9472, 0 }, { "boxv",    9474, 0 }, { "boxdr",   9484, 0 },
    { "boxdl",   9488, 0 }, { "boxur",   9492, 0 }, { "boxul",   9496, 0 },
    { "blk14",   9617, 0 }, { "blk12",   9618, 0 }, { "blk34",   9619, 0 },
    { "block",   9608, 0 },

    /* miscellaneous names browsers see constantly */
    { "sol",       47, 0 }, { "bsol",      92, 0 }, { "num",       35, 0 },
    { "dollar",    36, 0 }, { "percnt",    37, 0 }, { "ast",       42, 0 },
    { "plus",      43, 0 }, { "comma",     44, 0 }, { "period",    46, 0 },
    { "colon",     58, 0 }, { "semi",      59, 0 }, { "equals",    61, 0 },
    { "quest",     63, 0 }, { "commat",    64, 0 }, { "lsqb",      91, 0 },
    { "rsqb",      93, 0 }, { "Hat",       94, 0 }, { "lowbar",    95, 0 },
    { "grave",     96, 0 }, { "lcub",     123, 0 }, { "verbar",   124, 0 },
    { "rcub",     125, 0 }, { "excl",      33, 0 }, { "lpar",      40, 0 },
    { "rpar",      41, 0 }, { "hyphen",  8208, 0 }, { "dash",    8208, 0 },
    { "horbar",  8213, 0 }, { "Vert",    8214, 0 }, { "target",  8982, 0 },
    { "telrec",  8981, 0 }, { "phone",   9742, 0 }, { "female",  9792, 0 },
    { "male",    9794, 0 }, { "sung",    9834, 0 }, { "flat",    9837, 0 },
    { "natur",   9838, 0 }, { "sharp",   9839, 0 }, { "copysr",  8471, 0 },
    { "incare",  8453, 0 }, { "numero",  8470, 0 }, { "rx",      8478, 0 }
};

#define ENT_COUNT ((int)(sizeof g_ent / sizeof g_ent[0]))

int html_entity_count(void)
{
    return ENT_COUNT;
}

const char *html_entity_name(int i, unsigned long *cp, int *legacy)
{
    if (i < 0 || i >= ENT_COUNT)
        return 0;
    if (cp)
        *cp = g_ent[i].cp;
    if (legacy)
        *legacy = g_ent[i].legacy;
    return g_ent[i].name;
}

/* ------------------------------------------------------------------ *
 * Hash index
 * ------------------------------------------------------------------ */

static unsigned long ent_hash(const char *s, unsigned long n)
{
    unsigned long h = 2166136261UL;
    unsigned long i;

    for (i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619UL;
        h &= 0xffffffffUL;
    }
    return h;
}

void html_entity_index_init(struct html_entity_index *ix)
{
    int i;
    unsigned long j;

    if (!ix)
        return;
    for (j = 0; j < HTML_ENT_SLOTS; j++)
        ix->slot[j] = 0;
    for (i = 0; i < ENT_COUNT; i++) {
        unsigned long n = strlen(g_ent[i].name);
        unsigned long h = ent_hash(g_ent[i].name, n) & (HTML_ENT_SLOTS - 1);
        while (ix->slot[h])
            h = (h + 1) & (HTML_ENT_SLOTS - 1);
        ix->slot[h] = (unsigned short)(i + 1);
    }
}

/* Exact lookup of a counted name. Returns the table index or -1. */
static int ent_find(const struct html_entity_index *ix,
                    const char *s, unsigned long n)
{
    unsigned long h;

    if (n == 0 || n >= HTML_ENT_MAXNAME)
        return -1;
    h = ent_hash(s, n) & (HTML_ENT_SLOTS - 1);
    for (;;) {
        int idx = (int)ix->slot[h] - 1;
        if (idx < 0)
            return -1;
        if (strlen(g_ent[idx].name) == n &&
            memcmp(g_ent[idx].name, s, n) == 0)
            return idx;
        h = (h + 1) & (HTML_ENT_SLOTS - 1);
    }
}

int html_entity_lookup(const char *name, long len, unsigned long *cp)
{
    int i;
    unsigned long n;

    if (!name)
        return 0;
    n = (len < 0) ? strlen(name) : (unsigned long)len;
    for (i = 0; i < ENT_COUNT; i++) {
        if (strlen(g_ent[i].name) == n && memcmp(g_ent[i].name, name, n) == 0) {
            if (cp)
                *cp = g_ent[i].cp;
            return 1;
        }
    }
    return 0;
}

int html_entity_match(const struct html_entity_index *ix,
                      const char *s, unsigned long avail,
                      int *used, int *semi, unsigned long *cp)
{
    unsigned long maxl = avail;
    unsigned long l;
    int idx;

    if (!ix || !s || avail == 0)
        return 0;
    if (maxl > HTML_ENT_MAXNAME)
        maxl = HTML_ENT_MAXNAME;

    /* Longest semicolon-terminated match wins outright. */
    for (l = maxl; l >= 2; l--) {
        if (s[l - 1] != ';')
            continue;
        idx = ent_find(ix, s, l - 1);
        if (idx >= 0) {
            *used = (int)l;
            *semi = 1;
            *cp = g_ent[idx].cp;
            return 1;
        }
    }
    /* Otherwise the longest legacy name that is a prefix. */
    for (l = maxl; l >= 1; l--) {
        idx = ent_find(ix, s, l);
        if (idx >= 0 && g_ent[idx].legacy) {
            *used = (int)l;
            *semi = 0;
            *cp = g_ent[idx].cp;
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 * Numeric references
 * ------------------------------------------------------------------ */

/* The HTML5 replacement table for numeric references in 0x80..0x9F,
 * which authors write when they mean Windows-1252. */
static const unsigned short g_c1[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
};

unsigned long html_numeric_fixup(unsigned long cp)
{
    if (cp == 0 || cp > 0x10FFFFUL)
        return 0xFFFDUL;
    if (cp >= 0xD800UL && cp <= 0xDFFFUL)
        return 0xFFFDUL;
    if (cp >= 0x80UL && cp <= 0x9FUL)
        return g_c1[cp - 0x80];
    return cp;
}

/* ------------------------------------------------------------------ *
 * UTF-8
 * ------------------------------------------------------------------ */

int dom_utf8_encode(unsigned long cp, char *out)
{
    if (cp > 0x10FFFFUL || (cp >= 0xD800UL && cp <= 0xDFFFUL))
        cp = 0xFFFDUL;
    if (cp < 0x80UL) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800UL) {
        out[0] = (char)(0xC0UL | (cp >> 6));
        out[1] = (char)(0x80UL | (cp & 0x3FUL));
        return 2;
    }
    if (cp < 0x10000UL) {
        out[0] = (char)(0xE0UL | (cp >> 12));
        out[1] = (char)(0x80UL | ((cp >> 6) & 0x3FUL));
        out[2] = (char)(0x80UL | (cp & 0x3FUL));
        return 3;
    }
    out[0] = (char)(0xF0UL | (cp >> 18));
    out[1] = (char)(0x80UL | ((cp >> 12) & 0x3FUL));
    out[2] = (char)(0x80UL | ((cp >> 6) & 0x3FUL));
    out[3] = (char)(0x80UL | (cp & 0x3FUL));
    return 4;
}

unsigned long dom_utf8_decode(const char *s, unsigned long n,
                              unsigned long *cp)
{
    unsigned char c0;
    unsigned long need, i, v;

    if (n == 0) {
        *cp = 0;
        return 1;
    }
    c0 = (unsigned char)s[0];
    if (c0 < 0x80) {
        *cp = c0;
        return 1;
    }
    if ((c0 & 0xE0) == 0xC0) {
        need = 1;
        v = c0 & 0x1FUL;
    } else if ((c0 & 0xF0) == 0xE0) {
        need = 2;
        v = c0 & 0x0FUL;
    } else if ((c0 & 0xF8) == 0xF0) {
        need = 3;
        v = c0 & 0x07UL;
    } else {
        *cp = c0;           /* stray continuation or 0xFE/0xFF */
        return 1;
    }
    if (n < need + 1) {
        *cp = c0;
        return 1;
    }
    for (i = 1; i <= need; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c & 0xC0) != 0x80) {
            *cp = c0;
            return 1;
        }
        v = (v << 6) | (c & 0x3FUL);
    }
    /* Reject overlong forms and surrogates: they would let an attacker
     * smuggle a '<' past the tokenizer. */
    if ((need == 1 && v < 0x80UL) || (need == 2 && v < 0x800UL) ||
        (need == 3 && v < 0x10000UL) ||
        (v >= 0xD800UL && v <= 0xDFFFUL) || v > 0x10FFFFUL) {
        *cp = c0;
        return 1;
    }
    *cp = v;
    return need + 1;
}

/* ------------------------------------------------------------------ *
 * ASCII folding
 * ------------------------------------------------------------------ */

struct fold { unsigned short cp; const char *out; };

/* Latin-1 and Latin Extended-A letters lose their diacritics. German
 * and Nordic digraphs expand the way their languages transliterate
 * them, which is what the old flat renderer did and what reads best in
 * a monospaced ASCII font. */
static const struct fold g_fold[] = {
    { 0x00A0, " " },   { 0x00A1, "!" },   { 0x00A2, "c" },
    { 0x00A3, "GBP" }, { 0x00A4, "$" },   { 0x00A5, "JPY" },
    { 0x00A6, "|" },   { 0x00A7, "S" },   { 0x00A8, "\"" },
    { 0x00A9, "(c)" }, { 0x00AA, "a" },   { 0x00AB, "<<" },
    { 0x00AC, "!" },   { 0x00AD, "" },    { 0x00AE, "(R)" },
    { 0x00AF, "-" },   { 0x00B0, " deg" },{ 0x00B1, "+/-" },
    { 0x00B2, "2" },   { 0x00B3, "3" },   { 0x00B4, "'" },
    { 0x00B5, "u" },   { 0x00B6, "P" },   { 0x00B7, "*" },
    { 0x00B8, "," },   { 0x00B9, "1" },   { 0x00BA, "o" },
    { 0x00BB, ">>" },  { 0x00BC, "1/4" }, { 0x00BD, "1/2" },
    { 0x00BE, "3/4" }, { 0x00BF, "?" },
    { 0x00C0, "A" },   { 0x00C1, "A" },   { 0x00C2, "A" },
    { 0x00C3, "A" },   { 0x00C4, "Ae" },  { 0x00C5, "A" },
    { 0x00C6, "AE" },  { 0x00C7, "C" },   { 0x00C8, "E" },
    { 0x00C9, "E" },   { 0x00CA, "E" },   { 0x00CB, "E" },
    { 0x00CC, "I" },   { 0x00CD, "I" },   { 0x00CE, "I" },
    { 0x00CF, "I" },   { 0x00D0, "D" },   { 0x00D1, "N" },
    { 0x00D2, "O" },   { 0x00D3, "O" },   { 0x00D4, "O" },
    { 0x00D5, "O" },   { 0x00D6, "Oe" },  { 0x00D7, "x" },
    { 0x00D8, "O" },   { 0x00D9, "U" },   { 0x00DA, "U" },
    { 0x00DB, "U" },   { 0x00DC, "Ue" },  { 0x00DD, "Y" },
    { 0x00DE, "Th" },  { 0x00DF, "ss" },
    { 0x00E0, "a" },   { 0x00E1, "a" },   { 0x00E2, "a" },
    { 0x00E3, "a" },   { 0x00E4, "ae" },  { 0x00E5, "a" },
    { 0x00E6, "ae" },  { 0x00E7, "c" },   { 0x00E8, "e" },
    { 0x00E9, "e" },   { 0x00EA, "e" },   { 0x00EB, "e" },
    { 0x00EC, "i" },   { 0x00ED, "i" },   { 0x00EE, "i" },
    { 0x00EF, "i" },   { 0x00F0, "d" },   { 0x00F1, "n" },
    { 0x00F2, "o" },   { 0x00F3, "o" },   { 0x00F4, "o" },
    { 0x00F5, "o" },   { 0x00F6, "oe" },  { 0x00F7, "/" },
    { 0x00F8, "o" },   { 0x00F9, "u" },   { 0x00FA, "u" },
    { 0x00FB, "u" },   { 0x00FC, "ue" },  { 0x00FD, "y" },
    { 0x00FE, "th" },  { 0x00FF, "y" },
    { 0x0152, "OE" },  { 0x0153, "oe" },  { 0x0160, "S" },
    { 0x0161, "s" },   { 0x0178, "Y" },   { 0x017D, "Z" },
    { 0x017E, "z" },   { 0x0192, "f" },   { 0x02C6, "^" },
    { 0x02DC, "~" },   { 0x0393, "Gamma" },{ 0x0394, "Delta" },
    { 0x03A9, "Omega" },{ 0x03B1, "alpha" },{ 0x03B2, "beta" },
    { 0x03BC, "u" },   { 0x03C0, "pi" },
    { 0x2010, "-" },   { 0x2011, "-" },   { 0x2012, "-" },
    { 0x2013, "-" },   { 0x2014, "--" },  { 0x2015, "--" },
    { 0x2018, "'" },   { 0x2019, "'" },   { 0x201A, "," },
    { 0x201C, "\"" },  { 0x201D, "\"" },  { 0x201E, "\"" },
    { 0x2020, "+" },   { 0x2021, "++" },  { 0x2022, "*" },
    { 0x2026, "..." }, { 0x2030, "o/oo" },{ 0x2032, "'" },
    { 0x2033, "\"" },  { 0x2039, "<" },   { 0x203A, ">" },
    { 0x2044, "/" },   { 0x20AC, "EUR" }, { 0x2122, "(TM)" },
    { 0x2190, "<-" },  { 0x2191, "^" },   { 0x2192, "->" },
    { 0x2193, "v" },   { 0x2194, "<->" }, { 0x21D0, "<=" },
    { 0x21D2, "=>" },  { 0x21D4, "<=>" },
    { 0x2212, "-" },   { 0x2217, "*" },   { 0x221A, "sqrt" },
    { 0x221E, "inf" }, { 0x2248, "~=" },  { 0x2260, "!=" },
    { 0x2264, "<=" },  { 0x2265, ">=" },  { 0x2500, "-" },
    { 0x2502, "|" },   { 0x2588, "#" },   { 0x25CA, "<>" },
    { 0x2600, "*" },   { 0x2605, "*" },   { 0x2606, "*" },
    { 0x2611, "[x]" }, { 0x2713, "v" },   { 0x2717, "x" },
    { 0x2764, "<3" },  { 0x2665, "<3" },  { 0x2660, "^" },
    { 0x2663, "&" },   { 0x2666, "<>" },  { 0x200B, "" },
    { 0x200C, "" },    { 0x200D, "" },    { 0x200E, "" },
    { 0x200F, "" },    { 0x2007, " " },   { 0x2008, " " },
    { 0x2009, " " },   { 0x200A, " " },   { 0x2002, " " },
    { 0x2003, " " },   { 0xFEFF, "" },    { 0xFFFD, "?" }
};

#define FOLD_COUNT ((int)(sizeof g_fold / sizeof g_fold[0]))

static const char *fold_lookup(unsigned long cp)
{
    int i;

    for (i = 0; i < FOLD_COUNT; i++) {
        if (g_fold[i].cp == cp)
            return g_fold[i].out;
    }
    return "?";
}

unsigned long dom_fold_ascii(const char *utf8, unsigned long n,
                             char *out, unsigned long cap)
{
    unsigned long i = 0, w = 0;

    while (i < n) {
        unsigned long cp = 0;
        unsigned long adv = dom_utf8_decode(utf8 + i, n - i, &cp);
        const char *rep;
        char one[2];

        i += adv;
        if (cp == 9 || cp == 10 || cp == 13 || (cp >= 32 && cp < 127)) {
            one[0] = (char)cp;
            one[1] = 0;
            rep = one;
        } else {
            rep = fold_lookup(cp);
        }
        while (*rep) {
            if (out && cap > 0 && w + 1 < cap)
                out[w] = *rep;
            w++;
            rep++;
        }
    }
    if (out && cap > 0)
        out[(w < cap) ? w : cap - 1] = 0;
    return w;
}
