/* Host regression test for KestrelOS libc/string.c edge cases.
 *
 * The target implementation is #included directly under k_* names so the
 * host C library (and sanitizer string interceptors) can never satisfy a
 * call that is meant to exercise our code.  Build:
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -g -Ilibc/include \
 *      tools/test_kstring.c -o build/host-tests/test_kstring
 *
 * Note: this libc provides no strnlen/memchr/strtok; the closest bounds,
 * not-found and signedness edges of the functions it does provide are
 * covered instead. */

#include <assert.h>
#include <stddef.h>

#define memset   k_memset
#define memcpy   k_memcpy
#define memmove  k_memmove
#define memcmp   k_memcmp
#define strlen   k_strlen
#define strcmp   k_strcmp
#define strncmp  k_strncmp
#define strcpy   k_strcpy
#define strncpy  k_strncpy
#define strcat   k_strcat
#define strncat  k_strncat
#define strchr   k_strchr
#define strrchr  k_strrchr
#define strstr   k_strstr
#define strdup   k_strdup
#include "../libc/string.c"
#undef memset
#undef memcpy
#undef memmove
#undef memcmp
#undef strlen
#undef strcmp
#undef strncmp
#undef strcpy
#undef strncpy
#undef strcat
#undef strncat
#undef strchr
#undef strrchr
#undef strstr
#undef strdup

int puts(const char *s); /* host libc; libc/include/stdio.h stays unused */

#define GUARD 0xAB

static void test_memset(void)
{
    char buf[8];
    assert(k_memset(buf, 'x', sizeof(buf)) == buf);
    for (size_t i = 0; i < sizeof(buf); i++)
        assert(buf[i] == 'x');
    /* c is converted to unsigned char, not truncated weirdly. */
    assert(k_memset(buf, 0x1FF, 3) == buf);
    assert(buf[0] == (char)0xFF && buf[1] == (char)0xFF && buf[2] == (char)0xFF);
    assert(buf[3] == 'x');
    /* n == 0 touches nothing. */
    k_memset(buf, 'z', 0);
    assert(buf[0] == (char)0xFF);
}

static void test_memcpy(void)
{
    char dst[10];
    const char src[6] = {'a', 'b', 'c', 'd', 'e', 'f'};
    dst[6] = (char)GUARD;
    assert(k_memcpy(dst, src, 6) == dst);
    assert(k_memcmp(dst, src, 6) == 0);
    assert(dst[6] == (char)GUARD); /* exactly n bytes written */
    k_memcpy(dst, "ZZ", 0);
    assert(dst[0] == 'a');
    /* High-bit bytes copy verbatim. */
    assert(k_memcpy(dst, "\x80\xff", 2) == dst);
    assert((unsigned char)dst[0] == 0x80 && (unsigned char)dst[1] == 0xff);
}

static void test_memmove(void)
{
    /* Overlap, destination below source: forward copy must not smear. */
    char buf[16] = "0123456789abcde";
    assert(k_memmove(buf, buf + 4, 8) == buf);
    assert(k_strcmp(buf, "456789ab89abcde") == 0);
    /* Overlap, destination above source: backward copy must not smear. */
    {
        char b2[16] = "0123456789abcde";
        k_memmove(b2 + 4, b2, 8);
        assert(k_strcmp(b2, "012301234567cde") == 0);
    }
    /* Same pointer and n == 0 are no-ops. */
    k_memmove(buf, buf, 5);
    assert(k_strcmp(buf, "456789ab89abcde") == 0);
    k_memmove(buf, buf + 1, 0);
    assert(k_strcmp(buf, "456789ab89abcde") == 0);
}

static void test_memcmp(void)
{
    assert(k_memcmp("abc", "abc", 3) == 0);
    assert(k_memcmp("abc", "abd", 3) != 0);
    assert(k_memcmp("abc", "abd", 2) == 0);   /* stop before difference */
    assert(k_memcmp("", "zzz", 0) == 0);      /* n == 0 always equal */
    /* Unsigned byte ordering: 0xff must compare above 0x01, not wrap. */
    assert(k_memcmp("\xff", "\x01", 1) > 0);
    assert(k_memcmp("\x01", "\xff", 1) < 0);
    /* Difference in the last compared byte only. */
    assert(k_memcmp("aaaZ", "aaaY", 4) > 0);
}

static void test_strlen(void)
{
    assert(k_strlen("") == 0);
    assert(k_strlen("a") == 1);
    assert(k_strlen("kestrel") == 7);
    /* High bytes are ordinary characters. */
    assert(k_strlen("\x80\xff\xfe") == 3);
    {
        static char big[1025];
        k_memset(big, 'q', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        assert(k_strlen(big) == 1024);
    }
}

static void test_strcmp(void)
{
    assert(k_strcmp("", "") == 0);
    assert(k_strcmp("same", "same") == 0);
    assert(k_strcmp("abc", "abd") < 0);
    assert(k_strcmp("abd", "abc") > 0);
    /* Prefix sorts before the longer string. */
    assert(k_strcmp("abc", "abcd") < 0);
    assert(k_strcmp("abcd", "abc") > 0);
    /* Signedness: 0x80 byte must compare as unsigned 128, not -128.
     * A signed-char implementation wrongly reports < 0 here. */
    assert(k_strcmp("\x80", "a") > 0);
    assert(k_strcmp("a", "\x80") < 0);
    assert(k_strcmp("\xff", "z") > 0);
    /* First byte may already differ. */
    assert(k_strcmp("b", "a") > 0);
}

static void test_strncmp(void)
{
    assert(k_strncmp("abc", "abc", 3) == 0);
    assert(k_strncmp("abc", "abd", 2) == 0);  /* bound hides difference */
    assert(k_strncmp("abc", "abd", 3) != 0);
    /* n == 0 compares nothing. */
    assert(k_strncmp("abc", "xyz", 0) == 0);
    /* n larger than the strings stops at the NUL, not at n. */
    assert(k_strncmp("a", "a", 100) == 0);
    assert(k_strncmp("a", "b", 100) < 0);
    /* Signedness through the bounded path too. */
    assert(k_strncmp("\x80", "a", 1) > 0);
    assert(k_strncmp("abc", "abcd", 3) == 0);
    assert(k_strncmp("abc", "abcd", 4) < 0);
}

static void test_strcpy(void)
{
    char dst[16];
    assert(k_strcpy(dst, "kestrel") == dst);
    assert(k_strcmp(dst, "kestrel") == 0);
    assert(k_strcpy(dst, "") == dst);
    assert(dst[0] == '\0');
}

static void test_strncpy(void)
{
    char dst[8];
    /* src shorter than n: copies, then zero-pads to exactly n bytes. */
    k_memset(dst, 'x', sizeof(dst));
    assert(k_strncpy(dst, "ab", 5) == dst);
    assert(dst[0] == 'a' && dst[1] == 'b');
    assert(dst[2] == '\0' && dst[3] == '\0' && dst[4] == '\0');
    assert(dst[5] == 'x' && dst[7] == 'x');   /* nothing past n */
    /* src exactly n long: no NUL written, exactly n bytes copied. */
    k_memset(dst, 'x', sizeof(dst));
    assert(k_strncpy(dst, "abcdefg", 7) == dst);
    assert(k_memcmp(dst, "abcdefg", 7) == 0);
    assert(dst[7] == 'x');                    /* unterminated, untouched */
    /* src longer than n: truncated copy, no NUL. */
    k_memset(dst, 'x', sizeof(dst));
    k_strncpy(dst, "0123456789", 4);
    assert(k_memcmp(dst, "0123", 4) == 0);
    assert(dst[4] == 'x');
    /* n == 0 writes nothing at all. */
    k_strncpy(dst, "y", 0);
    assert(dst[0] == '0');
}

static void test_strcat_strncat(void)
{
    char dst[16] = "foo";
    assert(k_strcat(dst, "bar") == dst);
    assert(k_strcmp(dst, "foobar") == 0);
    k_strcat(dst, "");
    assert(k_strcmp(dst, "foobar") == 0);
    /* strncat always leaves a NUL and appends at most n chars. */
    dst[6] = '\0';
    assert(k_strncat(dst, "XYZ", 2) == dst);
    assert(k_strcmp(dst, "foobarXY") == 0);
    assert(dst[8] == '\0');
    /* n == 0 appends nothing but still writes the terminator. */
    assert(k_strncat(dst, "zzz", 0) == dst);
    assert(k_strcmp(dst, "foobarXY") == 0);
    /* n larger than src appends the whole string once. */
    assert(k_strncat(dst, "q", 50) == dst);
    assert(k_strcmp(dst, "foobarXYq") == 0);
}

static void test_strchr(void)
{
    const char *s = "abca";
    assert(k_strchr(s, 'a') == s);
    assert(k_strchr(s, 'b') == s + 1);
    assert(k_strchr(s, 'z') == NULL);
    assert(k_strchr("", 'x') == NULL);
    /* Searching for NUL must return the terminator, not NULL. */
    assert(k_strchr(s, '\0') == s + 4);
    /* c is taken as char: high bytes match high bytes. */
    assert(k_strchr("a\x80" "b", 0x80) != NULL);
    assert(k_strchr("a\x80" "b", 0x80)[0] == (char)0x80);
    /* c beyond char range truncates: 0x180 -> 0x80. */
    assert(k_strchr("a\x80" "b", 0x180) != NULL);
}

static void test_strrchr(void)
{
    const char *s = "abca";
    assert(k_strrchr(s, 'a') == s + 3);
    assert(k_strrchr(s, 'c') == s + 2);
    assert(k_strrchr(s, 'z') == NULL);
    assert(k_strrchr("", 'x') == NULL);
    /* POSIX: strrchr(s, 0) yields the terminator. */
    assert(k_strrchr(s, '\0') == s + 4);
}

static void test_strstr(void)
{
    const char *hay = "the quick brown fox";
    /* Empty needle matches immediately. */
    assert(k_strstr(hay, "") == hay);
    assert(k_strstr("", "") != NULL && *k_strstr("", "") == '\0');
    /* Found at start, middle, and ending at the last byte. */
    assert(k_strstr(hay, "the") == hay);
    assert(k_strstr(hay, "quick") == hay + 4);
    assert(k_strstr(hay, "fox") == hay + 16);
    /* Not found, and needle longer than haystack. */
    assert(k_strstr(hay, "wolf") == NULL);
    assert(k_strstr("abc", "abcdef") == NULL);
    assert(k_strstr("", "x") == NULL);
    /* Repeated prefix must not stop the scan early: "aaab" / "aab". */
    assert(k_strstr("aaab", "aab") != NULL);
    assert(k_strstr("aaab", "aab") - (const char *)"aaab" == 1);
    /* Needle equal to haystack. */
    assert(k_strstr("exact", "exact") != NULL);
}

static void test_strdup(void)
{
    char *dup = k_strdup("duplicate me");
    assert(dup != NULL);
    assert(k_strcmp(dup, "duplicate me") == 0);
    /* Independent storage: mutating the copy keeps the source intact. */
    const char *orig = "immutable";
    char *copy = k_strdup(orig);
    assert(copy != NULL && copy != orig);
    copy[0] = 'X';
    assert(orig[0] == 'i');
    char *empty = k_strdup("");
    assert(empty != NULL && empty[0] == '\0');
    free(dup);
    free(copy);
    free(empty);
}

int main(void)
{
    test_memset();
    test_memcpy();
    test_memmove();
    test_memcmp();
    test_strlen();
    test_strcmp();
    test_strncmp();
    test_strcpy();
    test_strncpy();
    test_strcat_strncat();
    test_strchr();
    test_strrchr();
    test_strstr();
    test_strdup();
    puts("kstring: memset/memcpy/memmove/memcmp, strlen, strcmp/strncmp "
         "signedness, strcpy/strncpy bounds, strcat/strncat, strchr/strrchr "
         "NUL+miss, strstr edges, strdup independence PASS");
    return 0;
}
