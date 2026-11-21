/*
 * Host regression for KestrelOS libc/string.c.
 *
 * The real target implementation is compiled into this translation unit
 * below with every public symbol renamed to k_*.  Verification helpers are
 * written by hand, so the host C library stays an independent oracle; this
 * mirrors the objcopy rename used by tools/run-stdio-tests.sh but works
 * inside a single `tools/build.py test-host` compile step (including on
 * Windows, where libc's `unsigned long` parameters differ from size_t and
 * the target file must not see the host <string.h> prototypes).
 *
 * Build: -Ilibc/include makes the <string.h>/<stdlib.h>/<stdio.h> inside the
 * target file resolve to KestrelOS's own freestanding headers.  Only the
 * printf declaration is used from the host side; the host CRT provides the
 * implementation at link time.  strdup/strndup inside the target call the
 * host malloc through KestrelOS's `void *malloc(unsigned long)` prototype.
 */

#include <stdio.h>

#define memset  k_memset
#define memcpy  k_memcpy
#define memmove k_memmove
#define memcmp  k_memcmp
#define memchr  k_memchr
#define memrchr k_memrchr
#define strlen  k_strlen
#define strnlen k_strnlen
#define strcmp  k_strcmp
#define strncmp k_strncmp
#define strcpy  k_strcpy
#define strncpy k_strncpy
#define strcat  k_strcat
#define strncat k_strncat
#define strchr  k_strchr
#define strrchr k_strrchr
#define strspn  k_strspn
#define strcspn k_strcspn
#define strpbrk k_strpbrk
#define strstr  k_strstr
#define strtok  k_strtok
#define strdup  k_strdup
#define strndup k_strndup

#include "../libc/string.c"

static int g_named;
static int g_failed;

static void named_result(const char *name, int ok, const char *detail)
{
    g_named++;
    if (ok) {
        printf("  PASS %02d  %s\n", g_named, name);
        return;
    }
    g_failed++;
    printf("  FAIL %02d  %s", g_named, name);
    if (detail && detail[0])
        printf(" -- %s", detail);
    printf("\n");
}

/* Independent byte compare: no libc calls, so it is a true oracle. */
static int bytes_eq(const void *a, const void *b, unsigned long n)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;

    while (n--) {
        if (*pa++ != *pb++)
            return 0;
    }
    return 1;
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int all_byte(const unsigned char *p, unsigned long n, unsigned char v)
{
    while (n--) {
        if (*p++ != v)
            return 0;
    }
    return 1;
}

static void test_memory(void)
{
    unsigned char buf[16];
    unsigned char src[16];
    int i;
    int ok;

    for (i = 0; i < 16; i++)
        src[i] = (unsigned char)(i + 1);

    k_memset(buf, 0xa5, sizeof(buf));
    named_result("memset fills every byte", all_byte(buf, 16, 0xa5), 0);
    named_result("memset returns dst", k_memset(buf, 0, 0) == buf, 0);
    named_result("memset n=0 keeps buffer", all_byte(buf, 16, 0xa5), 0);

    k_memset(buf, 0, sizeof(buf));
    named_result("memcpy copies", k_memcpy(buf, src, 16) == buf &&
                 bytes_eq(buf, src, 16), 0);
    named_result("memcpy n=0 keeps buffer",
                 k_memcpy(buf, src, 0) == buf && bytes_eq(buf, src, 16), 0);

    /* memmove overlap in both directions inside one buffer. */
    k_memcpy(buf, src, 16);
    named_result("memmove forward overlap",
                 k_memmove(buf + 2, buf, 8) == buf + 2 &&
                 bytes_eq(buf + 2, src, 8) && buf[0] == 1 && buf[1] == 2,
                 0);
    k_memcpy(buf, src, 16);
    named_result("memmove backward overlap",
                 k_memmove(buf, buf + 2, 8) == buf &&
                 bytes_eq(buf, src + 2, 8) && buf[8] == 9,
                 0);
    named_result("memmove same pointer",
                 k_memmove(buf, buf, 8) == buf && bytes_eq(buf, src + 2, 8),
                 0);

    k_memcpy(buf, src, 16);
    named_result("memcmp equal", k_memcmp(src, buf, 8) == 0, 0);
    named_result("memcmp less", k_memcmp(src, src + 1, 8) < 0, 0);
    named_result("memcmp greater", k_memcmp(src + 1, src, 8) > 0, 0);
    named_result("memcmp n=0 equal", k_memcmp(src, buf, 0) == 0, 0);

    k_memcpy(buf, src, 16);
    named_result("memchr finds first", k_memchr(buf, 5, 16) == buf + 4, 0);
    named_result("memchr no match", k_memchr(buf, 0x77, 16) == 0, 0);
    named_result("memchr n=0", k_memchr(buf, 1, 0) == 0, 0);
    named_result("memchr short n", k_memchr(buf, 5, 4) == 0, 0);
    named_result("memchr truncated int matches low byte",
                 k_memchr(buf, 0x100 + 3, 16) == buf + 2, 0);
    named_result("memrchr finds last", k_memrchr(buf, 9, 16) == buf + 8, 0);
    named_result("memrchr no match", k_memrchr(buf, 0x77, 16) == 0, 0);
    named_result("memrchr n=0", k_memrchr(buf, 1, 0) == 0, 0);

    buf[3] = 7;     /* duplicate value: first vs last index */
    ok = k_memchr(buf, 7, 16) == buf + 3 && k_memrchr(buf, 7, 16) == buf + 6;
    named_result("memchr/memrchr pick correct duplicate", ok, 0);
}

static void test_length_compare(void)
{
    named_result("strlen basic", k_strlen("kestrel") == 7, 0);
    named_result("strlen empty", k_strlen("") == 0, 0);

    named_result("strnlen shorter than max", k_strnlen("abc", 8) == 3, 0);
    named_result("strnlen clamps at max", k_strnlen("abcdef", 4) == 4, 0);
    named_result("strnlen exact fit", k_strnlen("abc", 3) == 3, 0);
    named_result("strnlen max 0", k_strnlen("abc", 0) == 0, 0);
    named_result("strnlen empty", k_strnlen("", 9) == 0, 0);

    named_result("strcmp equal", k_strcmp("abc", "abc") == 0, 0);
    named_result("strcmp prefix less", k_strcmp("abc", "abd") < 0, 0);
    named_result("strcmp longer greater", k_strcmp("abcd", "abc") > 0, 0);
    named_result("strcmp unsigned byte order",
                 k_strcmp("\xff", "a") > 0, 0);

    named_result("strncmp equal within n", k_strncmp("abc", "abd", 2) == 0,
                 0);
    named_result("strncmp differ within n", k_strncmp("abc", "abd", 3) < 0,
                 0);
    named_result("strncmp n=0", k_strncmp("abc", "xyz", 0) == 0, 0);
    named_result("strncmp stops at NUL",
                 k_strncmp("ab", "abc", 4) != 0 &&
                 k_strncmp("abc", "abc", 9) == 0, 0);
}

static void test_copy_concat(void)
{
    char buf[32];

    named_result("strcpy copies + NUL", k_strcpy(buf, "kestrel") == buf &&
                 str_eq(buf, "kestrel"), 0);

    k_memset(buf, 0xcc, sizeof(buf));
    named_result("strncpy copies without padding",
                 k_strncpy(buf, "ab", 5) == buf &&
                 str_eq(buf, "ab"), 0);
    named_result("strncpy pads with NULs",
                 buf[2] == 0 && buf[3] == 0 && buf[4] == 0, 0);
    k_memset(buf, 0xcc, sizeof(buf));
    named_result("strncpy truncated keeps no NUL",
                 k_strncpy(buf, "abcdefgh", 4) == buf &&
                 bytes_eq(buf, "abcd", 4), 0);
    named_result("strncpy n=0", k_strncpy(buf, "x", 0) == buf &&
                 (unsigned char)buf[4] == 0xcc, 0);

    k_strcpy(buf, "kest");
    named_result("strcat appends", k_strcat(buf, "rel") == buf &&
                 str_eq(buf, "kestrel"), 0);
    k_strcpy(buf, "kest");
    named_result("strncat appends + NUL", k_strncat(buf, "relos", 3) == buf &&
                 str_eq(buf, "kestrel"), 0);
    k_strcpy(buf, "ab");
    named_result("strncat n=0 still terminates",
                 k_strncat(buf, "zz", 0) == buf && str_eq(buf, "ab"), 0);
    k_strcpy(buf, "ab");
    named_result("strncat full src", k_strncat(buf, "cd", 9) == buf &&
                 str_eq(buf, "abcd"), 0);
}

static void test_search(void)
{
    static const char text[] = "one two three";

    named_result("strchr found", k_strchr(text, 'w') == text + 5, 0);
    named_result("strchr missing", k_strchr(text, 'z') == 0, 0);
    named_result("strchr NUL returns terminator",
                 k_strchr(text, '\0') == text + 13, 0);
    named_result("strchr truncated int matches low byte",
                 k_strchr(text, 'w' + 0x100) == text + 5, 0);

    named_result("strrchr last occurrence", k_strrchr(text, 'e') == text + 12,
                 0);
    named_result("strrchr missing", k_strrchr(text, 'z') == 0, 0);
    named_result("strrchr NUL returns terminator",
                 k_strrchr(text, '\0') == text + 13, 0);

    named_result("strspn all accepted", k_strspn("aabbcc", "abc") == 6, 0);
    named_result("strspn stops at reject", k_strspn("aabxcc", "abc") == 3, 0);
    named_result("strspn empty set", k_strspn("abc", "") == 0, 0);
    named_result("strspn empty string", k_strspn("", "abc") == 0, 0);

    named_result("strcspn stops at reject", k_strcspn("abcxde", "xd") == 3,
                 0);
    named_result("strcspn no reject present",
                 k_strcspn("abc", "xyz") == 3, 0);
    named_result("strcspn reject at start", k_strcspn("abc", "a") == 0, 0);
    named_result("strcspn empty set", k_strcspn("abc", "") == 3, 0);

    {
        static const char word[] = "hello";

        named_result("strpbrk returns earliest member",
                     k_strpbrk(word, "ol") == k_strchr(word, 'l'), 0);
        named_result("strpbrk no member", k_strpbrk(word, "xyz") == 0, 0);
        named_result("strpbrk empty set", k_strpbrk(word, "") == 0, 0);
    }

    named_result("strstr found", k_strstr(text, "two") == text + 4, 0);
    named_result("strstr missing", k_strstr(text, "four") == 0, 0);
    named_result("strstr empty needle", k_strstr(text, "") == text, 0);
    {
        static const char aab[] = "aababc";

        named_result("strstr partial mismatch retries",
                     k_strstr(aab, "abc") == aab + 3, 0);
    }
}

static void test_tok_dup(void)
{
    char text[] = " one,two;;three ";
    char *t;
    int ok;

    t = k_strtok(text, ",; ");
    ok = t != 0 && str_eq(t, "one");
    t = k_strtok(0, ",; ");
    ok = ok && t != 0 && str_eq(t, "two");
    t = k_strtok(0, ",; ");
    ok = ok && t != 0 && str_eq(t, "three");
    t = k_strtok(0, ",; ");
    ok = ok && t == 0;
    named_result("strtok skips and splits runs", ok, 0);
    named_result("strtok exhausted stays NULL", k_strtok(0, ",; ") == 0, 0);

    {
        char again[] = "a.b";
        ok = k_strtok(again, ".") != 0 && k_strtok(0, ".") != 0 &&
             k_strtok(0, ".") == 0;
        named_result("strtok restarts on new string", ok, 0);
    }
    {
        char none[] = "xyz";
        named_result("strtok all-delimiters NULL",
                     k_strtok(none, "xyz") == 0, 0);
    }

    {
        char *d = k_strdup("kestrel");
        ok = d != 0 && str_eq(d, "kestrel");
        if (d)
            ok = ok && k_strlen(d) == 7;
        named_result("strdup duplicates", ok, 0);
        free(d);

        d = k_strndup("kestrel", 3);
        ok = d != 0 && str_eq(d, "kes") && k_strlen(d) == 3;
        named_result("strndup truncates", ok, 0);
        free(d);

        d = k_strndup("kestrel", 20);
        ok = d != 0 && str_eq(d, "kestrel");
        named_result("strndup short source", ok, 0);
        free(d);

        d = k_strndup("", 5);
        ok = d != 0 && d[0] == '\0';
        named_result("strndup empty", ok, 0);
        free(d);
    }
}

int main(void)
{
    printf("Kestrel target string.c regression\n");
    test_memory();
    test_length_compare();
    test_copy_concat();
    test_search();
    test_tok_dup();

    printf("\nsummary: %d named assertions, %d failed\n", g_named, g_failed);
    return g_failed ? 1 : 0;
}
