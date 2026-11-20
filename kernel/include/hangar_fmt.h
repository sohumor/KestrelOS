#pragma once

/* Hangar image format: a parked process on disk.
 *
 * A bay is a directory /hangar/bayNN holding
 *   mem.N     page records (struct hangar_page + RLE words), <= 3 MiB each
 *   win.N     one parked window's pixels, row by row (u32 count + RLE)
 *   thumb     K_HANGAR_THUMB_W x K_HANGAR_THUMB_H ARGB preview
 *   manifest  struct hangar_manifest, written last: a bay without a valid
 *             manifest is an interrupted park and is never resumed.
 *
 * Pure C with no kernel dependencies so tools/test_hangar.c can exercise
 * the encoder, decoder and validator on the host. */

#include <stddef.h>
#include <stdint.h>

#define HANGAR_MAGIC        0x52474E48U   /* "HNGR" */
#define HANGAR_VERSION      1U
#define HANGAR_MAX_AREAS    16
#define HANGAR_MAX_FILES    16
#define HANGAR_MAX_WINS     4
#define HANGAR_PATH_MAX     96
#define HANGAR_NAME_MAX     32
#define HANGAR_TITLE_MAX    64
#define HANGAR_FPU_BYTES    512
#define HANGAR_SIGACT_BYTES 1024
#define HANGAR_CHUNK_BYTES  (3U << 20)
#define HANGAR_MAX_PAGES    16384U        /* 64 MiB of resident memory */
#define HANGAR_MAX_CHUNKS   32U
#define HANGAR_WIN_MAX_W    2560
#define HANGAR_WIN_MAX_H    1440
#define HANGAR_PAGE_WORDS   1024U
#define HANGAR_USER_TOP     0x0000800000000000ULL
#define HANGAR_RLE_RUN      0x80000000U

/* General registers in struct regs order (rax .. r15), then the iret
 * state that matters to ring 3. Selectors are not stored: resume always
 * uses the user code/data selectors. */
struct hangar_regs {
    uint64_t gpr[15];
    uint64_t rip;
    uint64_t rflags;
    uint64_t rsp;
};

struct hangar_area {
    uint64_t start;
    uint64_t end;
    uint64_t pte_flags;
};

struct hangar_file {
    int32_t  fd;
    int32_t  flags;
    uint32_t pos;
    uint32_t reserved;
    char     path[HANGAR_PATH_MAX];
};

struct hangar_win {
    uint32_t uwid;            /* the window id the process knows */
    uint32_t flags;           /* K_WIN_* */
    int32_t  x, y, w, h;
    int32_t  orig_x, orig_y, orig_w, orig_h;
    uint8_t  minimized, maximized, pinned, workspace;
    uint32_t pix_sum;         /* FNV-1a over the decoded pixels */
    char     title[HANGAR_TITLE_MAX];
};

struct hangar_page {
    uint64_t va;
    uint32_t pte_flags;
    uint32_t words;           /* RLE words that follow */
};

struct hangar_manifest {
    uint32_t magic;
    uint32_t version;
    uint32_t size;            /* sizeof(struct hangar_manifest) */
    uint32_t checksum;        /* FNV-1a with this field zero */

    uint32_t uid, gid;
    int32_t  orig_pid;
    uint32_t parked_at;       /* Unix seconds */
    uint64_t cap_mask;
    uint64_t sig_mask;
    uint64_t sig_pending;
    char     name[HANGAR_NAME_MAX];
    char     unveil[64];
    uint64_t heap_start;
    uint64_t brk;

    uint32_t narea, nfile, nwin, npages;
    uint32_t nchunks;
    uint32_t mem_sum;         /* FNV-1a over every mem.N byte, in order */
    uint64_t mem_bytes;
    uint32_t thumb_w, thumb_h;
    uint32_t thumb_sum;
    uint32_t reserved;

    struct hangar_area areas[HANGAR_MAX_AREAS];
    struct hangar_file files[HANGAR_MAX_FILES];
    struct hangar_win  wins[HANGAR_MAX_WINS];
    struct hangar_regs regs;
    uint8_t  fpu[HANGAR_FPU_BYTES];
    uint8_t  sigact[HANGAR_SIGACT_BYTES];
};

#define HANGAR_FNV_INIT 0x811C9DC5U

uint32_t hangar_fnv(uint32_t h, const void *data, size_t len);

/* Worst-case encoded size of n words. */
size_t hangar_rle_bound(size_t n);

/* Encode n words into dst (cap words). Returns words written, 0 if dst is
 * too small. Runs of three or more equal words become one run token. */
size_t hangar_rle_encode(const uint32_t *src, size_t n,
                         uint32_t *dst, size_t cap);

/* Decode exactly n words. Returns 0 only when src is consumed completely
 * and fills dst exactly; malformed input never writes past dst. */
int hangar_rle_decode(const uint32_t *src, size_t nsrc,
                      uint32_t *dst, size_t n);

/* Box-filter a w x h window into a tw x th thumbnail, letterboxed on bg. */
void hangar_thumb(const uint32_t *pix, int w, int h,
                  uint32_t *out, int tw, int th, uint32_t bg);

void hangar_manifest_seal(struct hangar_manifest *m);

/* 0 when the manifest is internally consistent; otherwise a short static
 * description of the first problem found. */
const char *hangar_manifest_check(const struct hangar_manifest *m);
