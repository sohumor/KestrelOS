/* Host tests for the Hangar image format (kernel/hangar_fmt.c): the
 * page/pixel RLE codec must round-trip any input and reject malformed
 * streams without writing out of bounds; the manifest validator must
 * accept a sealed park and refuse tampered or hostile ones. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hangar_fmt.h"

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static uint32_t rng_state = 0x1234567U;

static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void roundtrip(const uint32_t *src, size_t n, size_t *enc_words)
{
    size_t cap = hangar_rle_bound(n);
    uint32_t *enc = malloc(cap * 4 + 4);
    uint32_t *dec = malloc(n * 4 + 4);
    size_t words = hangar_rle_encode(src, n, enc, cap);

    CHECK(n == 0 || words > 0);
    CHECK(words <= cap);
    CHECK(hangar_rle_decode(enc, words, dec, n) == 0);
    CHECK(memcmp(src, dec, n * 4) == 0);
    /* The exact length is part of the contract. */
    if (n > 0) {
        CHECK(hangar_rle_decode(enc, words, dec, n - 1) != 0);
        CHECK(hangar_rle_decode(enc, words, dec, n + 1) != 0);
    }
    if (enc_words)
        *enc_words = words;
    free(enc);
    free(dec);
}

static void test_rle(void)
{
    static uint32_t page[HANGAR_PAGE_WORDS];
    size_t words;

    /* Zero page collapses to a single run. */
    memset(page, 0, sizeof(page));
    roundtrip(page, HANGAR_PAGE_WORDS, &words);
    CHECK(words == 2);

    /* Random data stays within the bound. */
    for (int i = 0; i < (int)HANGAR_PAGE_WORDS; i++)
        page[i] = rng();
    roundtrip(page, HANGAR_PAGE_WORDS, &words);
    CHECK(words == HANGAR_PAGE_WORDS + 1);

    /* Solid window row with a cursor in the middle. */
    for (int i = 0; i < (int)HANGAR_PAGE_WORDS; i++)
        page[i] = 0xFF1E293BU;
    page[500] = 0xFFFFFFFFU;
    page[501] = 0xFFFFFFFFU;
    roundtrip(page, HANGAR_PAGE_WORDS, &words);
    CHECK(words <= 8);

    /* Mixed short runs, pairs and singles, many lengths. */
    for (int trial = 0; trial < 2000; trial++) {
        size_t n = 1 + rng() % 700;
        for (size_t i = 0; i < n; i++)
            page[i] = (rng() % 4 == 0) ? rng() : (uint32_t)(rng() % 3);
        roundtrip(page, n, NULL);
    }

    /* Malformed streams: zero counts, truncated runs and literals,
     * counts overflowing the output. */
    {
        uint32_t out[8], bad1[] = { 0 }, bad2[] = { HANGAR_RLE_RUN | 4 },
                 bad3[] = { 5, 1, 2 }, bad4[] = { HANGAR_RLE_RUN | 9, 7 },
                 bad5[] = { 0x7FFFFFFFU, 1 };
        CHECK(hangar_rle_decode(bad1, 1, out, 8) != 0);
        CHECK(hangar_rle_decode(bad2, 1, out, 4) != 0);
        CHECK(hangar_rle_decode(bad3, 3, out, 5) != 0);
        CHECK(hangar_rle_decode(bad4, 2, out, 8) != 0);
        CHECK(hangar_rle_decode(bad5, 2, out, 8) != 0);
    }
    /* Encoder refuses a too-small destination instead of overflowing. */
    {
        uint32_t tiny[3];
        for (int i = 0; i < 64; i++)
            page[i] = rng();
        CHECK(hangar_rle_encode(page, 64, tiny, 3) == 0);
    }
}

static void test_thumb(void)
{
    int w = 400, h = 100;
    uint32_t *pix = malloc((size_t)w * h * 4);
    uint32_t thumb[160 * 100];

    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            pix[y * w + x] = x < w / 2 ? 0xFFFF0000U : 0xFF0000FFU;
    hangar_thumb(pix, w, h, thumb, 160, 100, 0xFF000000U);
    /* 4:1 window in a 16:10 box: letterboxed top and bottom. */
    CHECK(thumb[0] == 0xFF000000U);
    CHECK(thumb[50 * 160 + 10] == 0xFFFF0000U);
    CHECK(thumb[50 * 160 + 150] == 0xFF0000FFU);
    CHECK(thumb[99 * 160 + 80] == 0xFF000000U);
    hangar_thumb(NULL, 0, 0, thumb, 160, 100, 0xFF123456U);
    CHECK(thumb[0] == 0xFF123456U && thumb[160 * 100 - 1] == 0xFF123456U);
    free(pix);
}

static void make_manifest(struct hangar_manifest *m)
{
    memset(m, 0, sizeof(*m));
    m->uid = 1000;
    strcpy(m->name, "edit");
    m->heap_start = 0x600000;
    m->brk = 0x640000;
    m->narea = 1;
    m->areas[0].start = 0x400000;
    m->areas[0].end = 0x420000;
    m->nfile = 1;
    m->files[0].fd = 3;
    strcpy(m->files[0].path, "/home/user/notes.txt");
    m->nwin = 1;
    m->wins[0].w = 640;
    m->wins[0].h = 480;
    strcpy(m->wins[0].title, "Editor - notes.txt");
    m->npages = 40;
    m->nchunks = 1;
    m->regs.rip = 0x401000;
    m->regs.rsp = 0x7FFFFFFFD000ULL;
    m->thumb_w = 160;
    m->thumb_h = 100;
    hangar_manifest_seal(m);
}

static void test_manifest(void)
{
    struct hangar_manifest m, t;

    make_manifest(&m);
    CHECK(hangar_manifest_check(&m) == NULL);

    /* Any bit flip breaks the seal. */
    t = m;
    t.regs.rip ^= 1;
    CHECK(hangar_manifest_check(&t) != NULL);
    t = m;
    t.magic = 0;
    CHECK(hangar_manifest_check(&t) != NULL);
    t = m;
    t.version = 99;
    hangar_manifest_seal(&t);
    t.version = 99;
    CHECK(hangar_manifest_check(&t) != NULL);

    /* Hostile but correctly sealed images are still refused. */
#define REFUSE(stmt) do { t = m; stmt; hangar_manifest_seal(&t); \
        CHECK(hangar_manifest_check(&t) != NULL); } while (0)
    REFUSE(t.regs.rip = 0xFFFF800000001000ULL);
    REFUSE(t.regs.rsp = HANGAR_USER_TOP);
    REFUSE(t.narea = HANGAR_MAX_AREAS + 1);
    REFUSE(t.areas[0].end = t.areas[0].start);
    REFUSE(t.areas[0].end = 0xFFFF800000000000ULL);
    REFUSE(t.nfile = HANGAR_MAX_FILES + 1);
    REFUSE(t.files[0].fd = 16);
    REFUSE(t.files[0].path[0] = 'x');
    REFUSE(memset(t.files[0].path, 'a', sizeof(t.files[0].path)));
    REFUSE((t.nfile = 2, t.files[1] = t.files[0]));
    REFUSE(t.nwin = HANGAR_MAX_WINS + 1);
    REFUSE(t.wins[0].w = 0);
    REFUSE(t.wins[0].h = HANGAR_WIN_MAX_H + 1);
    REFUSE(t.wins[0].workspace = 4);
    REFUSE((t.nwin = 2, t.wins[1] = t.wins[0]));
    REFUSE(memset(t.name, 'n', sizeof(t.name)));
    REFUSE(t.npages = HANGAR_MAX_PAGES + 1);
    REFUSE(t.nchunks = 0);
    REFUSE(t.brk = t.heap_start - 1);
    REFUSE(t.thumb_w = 4096);
#undef REFUSE
}

int main(void)
{
    test_rle();
    test_thumb();
    test_manifest();
    if (failures) {
        fprintf(stderr, "test_hangar: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_hangar: RLE codec, thumbnails and manifest validation OK\n");
    return 0;
}
