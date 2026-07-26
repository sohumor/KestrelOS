/* KestrelOS libimg host test harness.
 *
 * libimg is pure computation, so it can be compiled for the host and
 * exercised exhaustively without booting anything. This program:
 *
 *   1. writes a Python fixture generator (embedded below) to a scratch
 *      directory and runs it, producing PNG/GIF/JPEG/BMP files together
 *      with the pixels each one is supposed to decode to, computed by a
 *      path that shares no code with the C decoders;
 *   2. decodes every fixture and compares pixel by pixel;
 *   3. checks img_probe() reports the same intrinsic size and frame count;
 *   4. checks img_scale() against a floating-point reference and against
 *      hand-computed values;
 *   5. fuzzes every decoder with tens of thousands of mutated files,
 *      asserting no crash, no hang and no leak (run it under
 *      -fsanitize=address,undefined, which is what the build line below
 *      does).
 *
 * Build and run (the DEFLATE library supplies inflate_buf; until it
 * exists, point -I and the extra source at a reference implementation):
 *
 *   gcc -Wall -Wextra -O2 -fsanitize=address,undefined \
 *       -Ilibimg -Ilibz -o /tmp/test_img \
 *       tools/test_img.c libimg/img.c libimg/png.c ... libz/inflate.c -lm
 *   /tmp/test_img /tmp/kimgfix
 *
 * Floating point appears only in this harness (references and error
 * statistics). The library itself is integer-only.
 */

#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "img.h"

/* ------------------------------------------------------------ reporting */

static int n_pass, n_fail;

static void ok(const char *name, const char *fmt, ...)
{
    va_list ap;

    n_pass++;
    printf("  ok   %-16s ", name);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

static void bad(const char *name, const char *fmt, ...)
{
    va_list ap;

    n_fail++;
    printf("  FAIL %-16s ", name);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

/* ------------------------------------------------------------ files */

static uint8_t *slurp(const char *path, unsigned long *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    long n;

    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) {
        fclose(f);
        return 0;
    }
    b = (uint8_t *)malloc((unsigned long)n + 1);
    if (!b) {
        fclose(f);
        return 0;
    }
    if (n && fread(b, 1, (unsigned long)n, f) != (unsigned long)n) {
        free(b);
        fclose(f);
        return 0;
    }
    fclose(f);
    *len = (unsigned long)n;
    return b;
}

/* ------------------------------------------------------------ generator */

static const char *const GEN_PY[] = {
    "#!/usr/bin/env python3\n",
    "# Fixture generator for the libimg host test harness.\n",
    "# Writes, into the directory given as argv[1]:\n",
    "#   <name>.<ext>   the encoded image\n",
    "#   <name>.rgba    expected pixels: 8 bytes of w,h (LE u32) then w*h RGBA\n",
    "#   index.txt      one line per fixture: name file mode arg frames\n",
    "# where mode is \"exact\" (pixel-identical), \"tol\" (arg = max abs channel\n",
    "# error allowed) or \"err\" (arg = the expected negative status code).\n",
    "#\n",
    "# Nothing here shares code with the C decoders: expected pixels are built\n",
    "# from the source data by an independent path.\n",
    "\n",
    "import math, os, struct, sys, zlib\n",
    "\n",
    "OUT = sys.argv[1] if len(sys.argv) > 1 else \".\"\n",
    "INDEX = []\n",
    "\n",
    "def emit(name, fname, blob, mode=\"exact\", arg=0, frames=1, rgba=None, w=0, h=0):\n",
    "    with open(os.path.join(OUT, fname), \"wb\") as f:\n",
    "        f.write(blob)\n",
    "    if rgba is not None:\n",
    "        with open(os.path.join(OUT, name + \".rgba\"), \"wb\") as f:\n",
    "            f.write(struct.pack(\"<II\", w, h))\n",
    "            f.write(rgba)\n",
    "    INDEX.append(\"%s %s %s %d %d\" % (name, fname, mode, arg, frames))\n",
    "\n",
    "def flatten(px, w, h):\n",
    "    b = bytearray()\n",
    "    for y in range(h):\n",
    "        for x in range(w):\n",
    "            r, g, bl, a = px[y][x]\n",
    "            b += bytes((r, g, bl, a))\n",
    "    return bytes(b)\n",
    "\n",
    "# --------------------------------------------------------------- sources\n",
    "\n",
    "def src_gray(w, h, maxv):\n",
    "    \"\"\"Native grey samples, deterministic and using the whole range.\"\"\"\n",
    "    return [[((x * 7 + y * 13) % (maxv + 1)) for x in range(w)] for y in range(h)]\n",
    "\n",
    "def src_rgb(w, h, maxv):\n",
    "    return [[(((x * 5 + y) % (maxv + 1)),\n",
    "              ((x + y * 3) % (maxv + 1)),\n",
    "              ((x * y + 7) % (maxv + 1))) for x in range(w)] for y in range(h)]\n",
    "\n",
    "def scale8(v, depth):\n",
    "    if depth == 16:\n",
    "        return v >> 8\n",
    "    return v * 255 // ((1 << depth) - 1)\n",
    "\n",
    "# --------------------------------------------------------------- PNG\n",
    "\n",
    "def chunk(t, d):\n",
    "    return struct.pack(\">I\", len(d)) + t + d + \\\n",
    "           struct.pack(\">I\", zlib.crc32(t + d) & 0xffffffff)\n",
    "\n",
    "def pack_samples(samples, depth):\n",
    "    \"\"\"Pack a flat list of samples at `depth` bits into bytes, MSB first.\"\"\"\n",
    "    if depth == 8:\n",
    "        return bytes(samples)\n",
    "    if depth == 16:\n",
    "        b = bytearray()\n",
    "        for v in samples:\n",
    "            b += struct.pack(\">H\", v)\n",
    "        return bytes(b)\n",
    "    per = 8 // depth\n",
    "    b = bytearray()\n",
    "    acc = 0\n",
    "    n = 0\n",
    "    for v in samples:\n",
    "        acc = (acc << depth) | (v & ((1 << depth) - 1))\n",
    "        n += 1\n",
    "        if n == per:\n",
    "            b.append(acc)\n",
    "            acc = 0\n",
    "            n = 0\n",
    "    if n:\n",
    "        acc <<= depth * (per - n)\n",
    "        b.append(acc)\n",
    "    return bytes(b)\n",
    "\n",
    "def paeth(a, b, c):\n",
    "    p = a + b - c\n",
    "    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)\n",
    "    if pa <= pb and pa <= pc:\n",
    "        return a\n",
    "    return b if pb <= pc else c\n",
    "\n",
    "def apply_filter(ft, raw, prior, bpp):\n",
    "    out = bytearray([ft])\n",
    "    for i in range(len(raw)):\n",
    "        left = raw[i - bpp] if i >= bpp else 0\n",
    "        up = prior[i]\n",
    "        ul = prior[i - bpp] if i >= bpp else 0\n",
    "        if ft == 0:\n",
    "            v = raw[i]\n",
    "        elif ft == 1:\n",
    "            v = raw[i] - left\n",
    "        elif ft == 2:\n",
    "            v = raw[i] - up\n",
    "        elif ft == 3:\n",
    "            v = raw[i] - ((left + up) >> 1)\n",
    "        else:\n",
    "            v = raw[i] - paeth(left, up, ul)\n",
    "        out.append(v & 0xff)\n",
    "    return bytes(out)\n",
    "\n",
    "ADAM = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4),\n",
    "        (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]\n",
    "\n",
    "def png_idat(w, h, depth, ct, getpix, interlace, filt=None):\n",
    "    \"\"\"getpix(x, y) -> tuple of native samples for one pixel.\"\"\"\n",
    "    nch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ct]\n",
    "    bpp = max(1, (nch * depth + 7) // 8)\n",
    "    raw = bytearray()\n",
    "    passes = ADAM if interlace else [(0, 0, 1, 1)]\n",
    "    fi = 0\n",
    "    for (x0, y0, dx, dy) in passes:\n",
    "        pw = (w - x0 + dx - 1) // dx\n",
    "        ph = (h - y0 + dy - 1) // dy\n",
    "        if pw <= 0 or ph <= 0:\n",
    "            continue\n",
    "        stride = (pw * nch * depth + 7) // 8\n",
    "        prior = bytes(stride)\n",
    "        for j in range(ph):\n",
    "            samples = []\n",
    "            for i in range(pw):\n",
    "                samples.extend(getpix(x0 + i * dx, y0 + j * dy))\n",
    "            line = pack_samples(samples, depth)\n",
    "            ft = (fi % 5) if filt is None else filt\n",
    "            fi += 1\n",
    "            raw += apply_filter(ft, line, prior, bpp)\n",
    "            prior = line\n",
    "    return zlib.compress(bytes(raw), 9)\n",
    "\n",
    "SIG = b\"\\x89PNG\\r\\n\\x1a\\n\"\n",
    "\n",
    "def make_png(w, h, depth, ct, getpix, interlace=0, plte=None, trns=None,\n",
    "             filt=None, split=1):\n",
    "    ihdr = struct.pack(\">IIBBBBB\", w, h, depth, ct, 0, 0, interlace)\n",
    "    blob = SIG + chunk(b\"IHDR\", ihdr)\n",
    "    if plte is not None:\n",
    "        blob += chunk(b\"PLTE\", plte)\n",
    "    if trns is not None:\n",
    "        blob += chunk(b\"tRNS\", trns)\n",
    "    idat = png_idat(w, h, depth, ct, getpix, interlace, filt)\n",
    "    if split <= 1:\n",
    "        blob += chunk(b\"IDAT\", idat)\n",
    "    else:\n",
    "        step = max(1, (len(idat) + split - 1) // split)\n",
    "        for i in range(0, len(idat), step):\n",
    "            blob += chunk(b\"IDAT\", idat[i:i + step])\n",
    "    blob += chunk(b\"IEND\", b\"\")\n",
    "    return blob\n",
    "\n",
    "def png_fixtures():\n",
    "    # ---- colour type 0, every bit depth, both interlace modes ----\n",
    "    for depth in (1, 2, 4, 8, 16):\n",
    "        for il in (0, 1):\n",
    "            w, h = 13, 7\n",
    "            maxv = (1 << depth) - 1\n",
    "            s = src_gray(w, h, maxv)\n",
    "            px = [[(scale8(s[y][x], depth),) * 3 + (255,) for x in range(w)]\n",
    "                  for y in range(h)]\n",
    "            name = \"png_g%d%s\" % (depth, \"_i\" if il else \"\")\n",
    "            emit(name, name + \".png\",\n",
    "                 make_png(w, h, depth, 0, lambda x, y: (s[y][x],), il),\n",
    "                 rgba=flatten(px, w, h), w=w, h=h)\n",
    "\n",
    "    # ---- colour type 2 (truecolour) ----\n",
    "    for depth in (8, 16):\n",
    "        for il in (0, 1):\n",
    "            w, h = 9, 11\n",
    "            maxv = (1 << depth) - 1\n",
    "            s = src_rgb(w, h, maxv)\n",
    "            px = [[tuple(scale8(c, depth) for c in s[y][x]) + (255,)\n",
    "                   for x in range(w)] for y in range(h)]\n",
    "            name = \"png_rgb%d%s\" % (depth, \"_i\" if il else \"\")\n",
    "            emit(name, name + \".png\",\n",
    "                 make_png(w, h, depth, 2, lambda x, y: s[y][x], il),\n",
    "                 rgba=flatten(px, w, h), w=w, h=h)\n",
    "\n",
    "    # ---- colour type 3 (palette), every bit depth ----\n",
    "    for depth in (1, 2, 4, 8):\n",
    "        for il in (0, 1):\n",
    "            w, h = 17, 5\n",
    "            n = 1 << depth\n",
    "            pal = [((i * 37) % 256, (i * 91) % 256, (i * 17) % 256)\n",
    "                   for i in range(n)]\n",
    "            s = [[(x * 3 + y) % n for x in range(w)] for y in range(h)]\n",
    "            px = [[pal[s[y][x]] + (255,) for x in range(w)] for y in range(h)]\n",
    "            plte = b\"\".join(bytes(c) for c in pal)\n",
    "            name = \"png_p%d%s\" % (depth, \"_i\" if il else \"\")\n",
    "            emit(name, name + \".png\",\n",
    "                 make_png(w, h, depth, 3, lambda x, y: (s[y][x],), il,\n",
    "                          plte=plte),\n",
    "                 rgba=flatten(px, w, h), w=w, h=h)\n",
    "\n",
    "    # ---- colour types 4 and 6 (with alpha) ----\n",
    "    for depth in (8, 16):\n",
    "        for il in (0, 1):\n",
    "            w, h = 12, 6\n",
    "            maxv = (1 << depth) - 1\n",
    "            s = [[(((x * 11 + y) % (maxv + 1)), ((x + y * 5) % (maxv + 1)))\n",
    "                  for x in range(w)] for y in range(h)]\n",
    "            px = [[(scale8(s[y][x][0], depth),) * 3 +\n",
    "                   (scale8(s[y][x][1], depth),) for x in range(w)]\n",
    "                  for y in range(h)]\n",
    "            name = \"png_ga%d%s\" % (depth, \"_i\" if il else \"\")\n",
    "            emit(name, name + \".png\",\n",
    "                 make_png(w, h, depth, 4, lambda x, y: s[y][x], il),\n",
    "                 rgba=flatten(px, w, h), w=w, h=h)\n",
    "\n",
    "            s2 = [[(((x * 5) % (maxv + 1)), ((y * 7) % (maxv + 1)),\n",
    "                    ((x + y) % (maxv + 1)), ((x * y) % (maxv + 1)))\n",
    "                   for x in range(w)] for y in range(h)]\n",
    "            px2 = [[tuple(scale8(c, depth) for c in s2[y][x])\n",
    "                    for x in range(w)] for y in range(h)]\n",
    "            name = \"png_rgba%d%s\" % (depth, \"_i\" if il else \"\")\n",
    "            emit(name, name + \".png\",\n",
    "                 make_png(w, h, depth, 6, lambda x, y: s2[y][x], il),\n",
    "                 rgba=flatten(px2, w, h), w=w, h=h)\n",
    "\n",
    "    # ---- tRNS on grey, truecolour and palette ----\n",
    "    w, h = 8, 8\n",
    "    s = [[(x * 8 + y) % 256 for x in range(w)] for y in range(h)]\n",
    "    key = s[3][3]\n",
    "    px = [[(s[y][x],) * 3 + (0 if s[y][x] == key else 255,)\n",
    "           for x in range(w)] for y in range(h)]\n",
    "    emit(\"png_g8_trns\", \"png_g8_trns.png\",\n",
    "         make_png(w, h, 8, 0, lambda x, y: (s[y][x],),\n",
    "                  trns=struct.pack(\">H\", key)),\n",
    "         rgba=flatten(px, w, h), w=w, h=h)\n",
    "\n",
    "    sr = src_rgb(w, h, 255)\n",
    "    kr = sr[2][5]\n",
    "    px = [[sr[y][x] + (0 if sr[y][x] == kr else 255,) for x in range(w)]\n",
    "          for y in range(h)]\n",
    "    emit(\"png_rgb8_trns\", \"png_rgb8_trns.png\",\n",
    "         make_png(w, h, 8, 2, lambda x, y: sr[y][x],\n",
    "                  trns=struct.pack(\">HHH\", *kr)),\n",
    "         rgba=flatten(px, w, h), w=w, h=h)\n",
    "\n",
    "    n = 16\n",
    "    pal = [((i * 13) % 256, (i * 29) % 256, (i * 7) % 256) for i in range(n)]\n",
    "    alpha = bytes([0, 64, 128, 255] * 4)\n",
    "    sp = [[(x + y * 3) % n for x in range(w)] for y in range(h)]\n",
    "    px = [[pal[sp[y][x]] + (alpha[sp[y][x]],) for x in range(w)]\n",
    "          for y in range(h)]\n",
    "    emit(\"png_p4_trns\", \"png_p4_trns.png\",\n",
    "         make_png(w, h, 4, 3, lambda x, y: (sp[y][x],),\n",
    "                  plte=b\"\".join(bytes(c) for c in pal), trns=alpha),\n",
    "         rgba=flatten(px, w, h), w=w, h=h)\n",
    "\n",
    "    # ---- every filter type on its own, so a bug in one is unambiguous ----\n",
    "    for ft in range(5):\n",
    "        w, h = 16, 6\n",
    "        s = src_rgb(w, h, 255)\n",
    "        px = [[s[y][x] + (255,) for x in range(w)] for y in range(h)]\n",
    "        name = \"png_filt%d\" % ft\n",
    "        emit(name, name + \".png\",\n",
    "             make_png(w, h, 8, 2, lambda x, y: s[y][x], filt=ft),\n",
    "             rgba=flatten(px, w, h), w=w, h=h)\n",
    "\n",
    "    # ---- IDAT split across chunks, which every real encoder does ----\n",
    "    w, h = 24, 18\n",
    "    s = src_rgb(w, h, 255)\n",
    "    px = [[s[y][x] + (255,) for x in range(w)] for y in range(h)]\n",
    "    emit(\"png_multidat\", \"png_multidat.png\",\n",
    "         make_png(w, h, 8, 2, lambda x, y: s[y][x], split=6),\n",
    "         rgba=flatten(px, w, h), w=w, h=h)\n",
    "\n",
    "    # ---- 1x1, the degenerate case ----\n",
    "    emit(\"png_1x1\", \"png_1x1.png\",\n",
    "         make_png(1, 1, 8, 2, lambda x, y: (1, 2, 3)),\n",
    "         rgba=bytes((1, 2, 3, 255)), w=1, h=1)\n",
    "\n",
    "    # ---- error cases ----\n",
    "    good = make_png(8, 8, 8, 2, lambda x, y: (x * 8, y * 8, 0))\n",
    "    emit(\"png_trunc\", \"png_trunc.png\", good[:len(good) // 2], \"err\", -2)\n",
    "\n",
    "    # A corrupt CRC on a critical chunk must be refused...\n",
    "    bad = bytearray(good)\n",
    "    bad[len(SIG) + 8 + 13 + 3] ^= 0xff        # IHDR CRC\n",
    "    emit(\"png_badcrc\", \"png_badcrc.png\", bytes(bad), \"err\", -3)\n",
    "\n",
    "    # ...but a corrupt ancillary chunk is merely dropped.\n",
    "    anc = SIG + chunk(b\"IHDR\", struct.pack(\">IIBBBBB\", 4, 4, 8, 2, 0, 0, 0))\n",
    "    bogus = chunk(b\"teXt\", b\"hello\")\n",
    "    bogus = bogus[:-1] + bytes([bogus[-1] ^ 0xff])\n",
    "    s = [[(x * 60, y * 60, 30) for x in range(4)] for y in range(4)]\n",
    "    anc += bogus + chunk(b\"IDAT\", png_idat(4, 4, 8, 2,\n",
    "                                           lambda x, y: s[y][x], 0))\n",
    "    anc += chunk(b\"IEND\", b\"\")\n",
    "    px = [[s[y][x] + (255,) for x in range(4)] for y in range(4)]\n",
    "    emit(\"png_badanc\", \"png_badanc.png\", anc, rgba=flatten(px, 4, 4), w=4, h=4)\n",
    "\n",
    "    # A declared 40000x40000 image is a decompression bomb, not a picture.\n",
    "    bomb = SIG + chunk(b\"IHDR\", struct.pack(\">IIBBBBB\", 40000, 40000, 8, 2,\n",
    "                                            0, 0, 0))\n",
    "    bomb += chunk(b\"IDAT\", zlib.compress(b\"\\0\" * 64)) + chunk(b\"IEND\", b\"\")\n",
    "    emit(\"png_bomb\", \"png_bomb.png\", bomb, \"err\", -5)\n",
    "\n",
    "    # A small header whose IDAT expands enormously: the inflate cap must\n",
    "    # stop it long before the expansion completes.\n",
    "    ex = SIG + chunk(b\"IHDR\", struct.pack(\">IIBBBBB\", 8, 8, 8, 2, 0, 0, 0))\n",
    "    ex += chunk(b\"IDAT\", zlib.compress(bytes(10 * 1000 * 1000), 9))\n",
    "    ex += chunk(b\"IEND\", b\"\")\n",
    "    emit(\"png_expand\", \"png_expand.png\", ex, \"err\", -3)\n",
    "\n",
    "    # Colour type 3 without a palette is invalid.\n",
    "    nop = SIG + chunk(b\"IHDR\", struct.pack(\">IIBBBBB\", 4, 4, 8, 3, 0, 0, 0))\n",
    "    nop += chunk(b\"IDAT\", zlib.compress(b\"\\0\" * 20)) + chunk(b\"IEND\", b\"\")\n",
    "    emit(\"png_nopal\", \"png_nopal.png\", nop, \"err\", -3)\n",
    "\n",
    "# --------------------------------------------------------------- GIF\n",
    "\n",
    "class BitW:\n",
    "    def __init__(self):\n",
    "        self.b = bytearray()\n",
    "        self.acc = 0\n",
    "        self.n = 0\n",
    "\n",
    "    def put(self, code, width):\n",
    "        self.acc |= code << self.n\n",
    "        self.n += width\n",
    "        while self.n >= 8:\n",
    "            self.b.append(self.acc & 0xff)\n",
    "            self.acc >>= 8\n",
    "            self.n -= 8\n",
    "\n",
    "    def flush(self):\n",
    "        if self.n:\n",
    "            self.b.append(self.acc & 0xff)\n",
    "            self.acc = 0\n",
    "            self.n = 0\n",
    "        return bytes(self.b)\n",
    "\n",
    "def lzw_encode(indices, mcs, deferred=True):\n",
    "    clear, end = 1 << mcs, (1 << mcs) + 1\n",
    "    w = BitW()\n",
    "    width = mcs + 1\n",
    "    table = {bytes([i]): i for i in range(clear)}\n",
    "    nxt = clear + 2\n",
    "    w.put(clear, width)\n",
    "    cur = b\"\"\n",
    "    for k in indices:\n",
    "        kb = bytes([k])\n",
    "        if cur + kb in table:\n",
    "            cur += kb\n",
    "            continue\n",
    "        w.put(table[cur], width)\n",
    "        if nxt < 4096:\n",
    "            table[cur + kb] = nxt\n",
    "            nxt += 1\n",
    "            if nxt > (1 << width) and width < 12:\n",
    "                width += 1\n",
    "        elif not deferred:\n",
    "            w.put(clear, width)\n",
    "            table = {bytes([i]): i for i in range(clear)}\n",
    "            nxt = clear + 2\n",
    "            width = mcs + 1\n",
    "        cur = kb\n",
    "    if cur:\n",
    "        w.put(table[cur], width)\n",
    "    w.put(end, width)\n",
    "    return w.flush()\n",
    "\n",
    "def subblocks(data):\n",
    "    out = bytearray()\n",
    "    i = 0\n",
    "    while i < len(data):\n",
    "        n = min(255, len(data) - i)\n",
    "        out.append(n)\n",
    "        out += data[i:i + n]\n",
    "        i += n\n",
    "    out.append(0)\n",
    "    return bytes(out)\n",
    "\n",
    "def gif_build(sw, sh, gct, frames, transparent=None, version=b\"89a\",\n",
    "              pre=b\"\"):\n",
    "    \"\"\"frames: list of (x, y, w, h, indices, interlaced, lct)\"\"\"\n",
    "    blob = b\"GIF\" + version\n",
    "    bits = 0\n",
    "    if gct:\n",
    "        n = 1\n",
    "        while (1 << n) < len(gct):\n",
    "            n += 1\n",
    "        bits = 0x80 | (n - 1)\n",
    "        gct = list(gct) + [(0, 0, 0)] * ((1 << n) - len(gct))\n",
    "    blob += struct.pack(\"<HHBBB\", sw, sh, bits, 0, 0)\n",
    "    if gct:\n",
    "        blob += b\"\".join(bytes(c) for c in gct)\n",
    "    blob += pre\n",
    "    first = True\n",
    "    for (x, y, w, h, idx, il, lct) in frames:\n",
    "        if transparent is not None and first:\n",
    "            blob += b\"\\x21\\xf9\\x04\" + bytes([1, 0, 0, transparent]) + b\"\\x00\"\n",
    "        lbits = 0\n",
    "        if lct:\n",
    "            n = 1\n",
    "            while (1 << n) < len(lct):\n",
    "                n += 1\n",
    "            lbits = 0x80 | (n - 1)\n",
    "            lct = list(lct) + [(0, 0, 0)] * ((1 << n) - len(lct))\n",
    "        if il:\n",
    "            lbits |= 0x40\n",
    "        blob += b\"\\x2c\" + struct.pack(\"<HHHHB\", x, y, w, h, lbits)\n",
    "        if lct:\n",
    "            blob += b\"\".join(bytes(c) for c in lct)\n",
    "        pal = lct if lct else gct\n",
    "        mcs = max(2, (len(pal) - 1).bit_length())\n",
    "        order = idx\n",
    "        if il:\n",
    "            order = []\n",
    "            for (y0, dy) in ((0, 8), (4, 8), (2, 4), (1, 2)):\n",
    "                for r in range(y0, h, dy):\n",
    "                    order.extend(idx[r * w:(r + 1) * w])\n",
    "        blob += bytes([mcs]) + subblocks(lzw_encode(order, mcs))\n",
    "        first = False\n",
    "    return blob + b\"\\x3b\"\n",
    "\n",
    "def gif_fixtures():\n",
    "    pal = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0),\n",
    "           (0, 255, 255), (255, 0, 255), (16, 32, 48), (200, 200, 200)]\n",
    "    w, h = 20, 12\n",
    "    idx = [(x * 3 + y) % 8 for y in range(h) for x in range(w)]\n",
    "    px = [[pal[idx[y * w + x]] + (255,) for x in range(w)] for y in range(h)]\n",
    "\n",
    "    emit(\"gif_basic\", \"gif_basic.gif\",\n",
    "         gif_build(w, h, pal, [(0, 0, w, h, idx, 0, None)]),\n",
    "         rgba=flatten(px, w, h), w=w, h=h)\n",
    "\n",
    "    emit(\"gif_ilace\", \"gif_ilace.gif\",\n",
    "         gif_build(w, h, pal, [(0, 0, w, h, idx, 1, None)]),\n",
    "         rgba=flatten(px, w, h), w=w, h=h)\n",
    "\n",
    "    tp = 2\n",
    "    pxt = [[pal[idx[y * w + x]] + (0 if idx[y * w + x] == tp else 255,)\n",
    "            for x in range(w)] for y in range(h)]\n",
    "    emit(\"gif_trans\", \"gif_trans.gif\",\n",
    "         gif_build(w, h, pal, [(0, 0, w, h, idx, 0, None)], transparent=tp),\n",
    "         rgba=flatten(pxt, w, h), w=w, h=h)\n",
    "\n",
    "    # Local colour table on the first frame overrides the global one.\n",
    "    lpal = [(1, 2, 3), (4, 5, 6), (7, 8, 9), (10, 11, 12)]\n",
    "    li = [(x + y) % 4 for y in range(6) for x in range(6)]\n",
    "    lpx = [[lpal[li[y * 6 + x]] + (255,) for x in range(6)] for y in range(6)]\n",
    "    emit(\"gif_lct\", \"gif_lct.gif\",\n",
    "         gif_build(6, 6, pal, [(0, 0, 6, 6, li, 0, lpal)]),\n",
    "         rgba=flatten(lpx, 6, 6), w=6, h=6)\n",
    "\n",
    "    # Three frames: only the first is decoded, the count is reported.\n",
    "    f = [(0, 0, w, h, idx, 0, None),\n",
    "         (0, 0, w, h, [(i + 1) % 8 for i in idx], 0, None),\n",
    "         (0, 0, w, h, [(i + 2) % 8 for i in idx], 0, None)]\n",
    "    emit(\"gif_anim\", \"gif_anim.gif\", gif_build(w, h, pal, f), frames=3,\n",
    "         rgba=flatten(px, w, h), w=w, h=h)\n",
    "\n",
    "    # A first frame smaller than the canvas leaves the rest transparent.\n",
    "    sub = [(x + y) % 8 for y in range(5) for x in range(5)]\n",
    "    spx = [[(0, 0, 0, 0) for x in range(w)] for y in range(h)]\n",
    "    for y in range(5):\n",
    "        for x in range(5):\n",
    "            spx[y + 3][x + 4] = pal[sub[y * 5 + x]] + (255,)\n",
    "    emit(\"gif_offset\", \"gif_offset.gif\",\n",
    "         gif_build(w, h, pal, [(4, 3, 5, 5, sub, 0, None)]),\n",
    "         rgba=flatten(spx, w, h), w=w, h=h)\n",
    "\n",
    "    # 256 colours over 65536 pixels: the code width climbs to 12 and the\n",
    "    # table fills, so the decoder must handle the deferred clear.\n",
    "    bp = [(i, (i * 7) % 256, (i * 13) % 256) for i in range(256)]\n",
    "    bw = bh = 256\n",
    "    bidx = [((x * 31 + y * 17 + (x * y) % 13) % 256)\n",
    "            for y in range(bh) for x in range(bw)]\n",
    "    bpx = [[bp[bidx[y * bw + x]] + (255,) for x in range(bw)]\n",
    "           for y in range(bh)]\n",
    "    emit(\"gif_big\", \"gif_big.gif\",\n",
    "         gif_build(bw, bh, bp, [(0, 0, bw, bh, bidx, 0, None)]),\n",
    "         rgba=flatten(bpx, bw, bh), w=bw, h=bh)\n",
    "\n",
    "    # GIF87a, no extensions at all.\n",
    "    emit(\"gif_87a\", \"gif_87a.gif\",\n",
    "         gif_build(w, h, pal, [(0, 0, w, h, idx, 0, None)], version=b\"87a\"),\n",
    "         rgba=flatten(px, w, h), w=w, h=h)\n",
    "\n",
    "    # A NETSCAPE loop block and a comment, as animated GIFs carry.\n",
    "    pre = b\"\\x21\\xff\\x0bNETSCAPE2.0\\x03\\x01\\x00\\x00\\x00\"\n",
    "    pre += b\"\\x21\\xfe\\x0cmade by hand\\x00\"\n",
    "    emit(\"gif_ext\", \"gif_ext.gif\",\n",
    "         gif_build(w, h, pal, [(0, 0, w, h, idx, 0, None)], pre=pre),\n",
    "         rgba=flatten(px, w, h), w=w, h=h)\n",
    "\n",
    "    emit(\"gif_trunc\", \"gif_trunc.gif\",\n",
    "         gif_build(w, h, pal, [(0, 0, w, h, idx, 0, None)])[:20], \"err\", -2)\n",
    "\n",
    "# --------------------------------------------------------------- JPEG\n",
    "\n",
    "SOI = bytes([0xff, 0xd8])\n",
    "EOI = bytes([0xff, 0xd9])\n",
    "\n",
    "ZIG = [0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,\n",
    "       12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,\n",
    "       35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,\n",
    "       58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63]\n",
    "\n",
    "QY = [16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55,\n",
    "      14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,\n",
    "      18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,\n",
    "      49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99]\n",
    "QC = [17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,\n",
    "      24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99] + \\\n",
    "     [99] * 32\n",
    "\n",
    "# Annex K typical Huffman tables.\n",
    "DC_L_BITS = [0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0]\n",
    "DC_L_VALS = list(range(12))\n",
    "DC_C_BITS = [0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0]\n",
    "DC_C_VALS = list(range(12))\n",
    "AC_L_BITS = [0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d]\n",
    "AC_L_VALS = [\n",
    "    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06,\n",
    "    0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,\n",
    "    0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,\n",
    "    0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,\n",
    "    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45,\n",
    "    0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,\n",
    "    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75,\n",
    "    0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,\n",
    "    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,\n",
    "    0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,\n",
    "    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9,\n",
    "    0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,\n",
    "    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4,\n",
    "    0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa]\n",
    "AC_C_BITS = [0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77]\n",
    "AC_C_VALS = [\n",
    "    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41,\n",
    "    0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,\n",
    "    0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,\n",
    "    0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,\n",
    "    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44,\n",
    "    0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,\n",
    "    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74,\n",
    "    0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,\n",
    "    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,\n",
    "    0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,\n",
    "    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,\n",
    "    0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,\n",
    "    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4,\n",
    "    0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa]\n",
    "\n",
    "def hcodes(bits, vals):\n",
    "    out = {}\n",
    "    code = 0\n",
    "    k = 0\n",
    "    for l in range(1, 17):\n",
    "        for _ in range(bits[l - 1]):\n",
    "            out[vals[k]] = (code, l)\n",
    "            code += 1\n",
    "            k += 1\n",
    "        code <<= 1\n",
    "    return out\n",
    "\n",
    "def qscale(tbl, quality):\n",
    "    s = 5000 // quality if quality < 50 else 200 - quality * 2\n",
    "    return [min(255, max(1, (v * s + 50) // 100)) for v in tbl]\n",
    "\n",
    "COS = [[math.cos((2 * x + 1) * u * math.pi / 16) for u in range(8)]\n",
    "       for x in range(8)]\n",
    "\n",
    "def fdct(blk):\n",
    "    out = [0.0] * 64\n",
    "    for v in range(8):\n",
    "        for u in range(8):\n",
    "            s = 0.0\n",
    "            for y in range(8):\n",
    "                for x in range(8):\n",
    "                    s += blk[y * 8 + x] * COS[x][u] * COS[y][v]\n",
    "            cu = (1 / math.sqrt(2)) if u == 0 else 1.0\n",
    "            cv = (1 / math.sqrt(2)) if v == 0 else 1.0\n",
    "            out[v * 8 + u] = 0.25 * cu * cv * s\n",
    "    return out\n",
    "\n",
    "class JBits:\n",
    "    def __init__(self):\n",
    "        self.b = bytearray()\n",
    "        self.acc = 0\n",
    "        self.n = 0\n",
    "\n",
    "    def put(self, code, length):\n",
    "        for i in range(length - 1, -1, -1):\n",
    "            self.acc = (self.acc << 1) | ((code >> i) & 1)\n",
    "            self.n += 1\n",
    "            if self.n == 8:\n",
    "                self.b.append(self.acc & 0xff)\n",
    "                if (self.acc & 0xff) == 0xff:\n",
    "                    self.b.append(0)\n",
    "                self.acc = 0\n",
    "                self.n = 0\n",
    "\n",
    "    def align(self):\n",
    "        while self.n:\n",
    "            self.put(1, 1)\n",
    "\n",
    "    def raw(self, data):\n",
    "        self.align()\n",
    "        self.b += data\n",
    "\n",
    "def magcat(v):\n",
    "    a = abs(v)\n",
    "    s = 0\n",
    "    while a:\n",
    "        s += 1\n",
    "        a >>= 1\n",
    "    return s\n",
    "\n",
    "def encode_blk(bw, blk, q, dctab, actab, pred):\n",
    "    f = fdct(blk)\n",
    "    coef = [int(round(f[ZIG[k]] / q[k])) for k in range(64)]\n",
    "    diff = coef[0] - pred\n",
    "    s = magcat(diff)\n",
    "    c, l = dctab[s]\n",
    "    bw.put(c, l)\n",
    "    if s:\n",
    "        bw.put(diff if diff > 0 else diff + (1 << s) - 1, s)\n",
    "    run = 0\n",
    "    for k in range(1, 64):\n",
    "        if coef[k] == 0:\n",
    "            run += 1\n",
    "            continue\n",
    "        while run > 15:\n",
    "            c, l = actab[0xf0]\n",
    "            bw.put(c, l)\n",
    "            run -= 16\n",
    "        s = magcat(coef[k])\n",
    "        c, l = actab[(run << 4) | s]\n",
    "        bw.put(c, l)\n",
    "        bw.put(coef[k] if coef[k] > 0 else coef[k] + (1 << s) - 1, s)\n",
    "        run = 0\n",
    "    if run:\n",
    "        c, l = actab[0x00]\n",
    "        bw.put(c, l)\n",
    "    return coef[0]\n",
    "\n",
    "def seg(m, payload):\n",
    "    return bytes([0xff, m]) + struct.pack(\">H\", len(payload) + 2) + payload\n",
    "\n",
    "def jpeg_encode(w, h, getrgb, quality=90, sub=(1, 1), gray=False, restart=0,\n",
    "                merge_dht=False):\n",
    "    qyn = qscale(QY, quality)\n",
    "    qcn = qscale(QC, quality)\n",
    "    qy = [qyn[ZIG[k]] for k in range(64)]      # DQT order is zigzag\n",
    "    qc = [qcn[ZIG[k]] for k in range(64)]\n",
    "    hs, vs = sub\n",
    "    ncomp = 1 if gray else 3\n",
    "\n",
    "    # Colour transform, done here so the fixture's expectation is\n",
    "    # independent of the decoder's.\n",
    "    Y = [[0.0] * w for _ in range(h)]\n",
    "    CB = [[0.0] * w for _ in range(h)]\n",
    "    CR = [[0.0] * w for _ in range(h)]\n",
    "    for y in range(h):\n",
    "        for x in range(w):\n",
    "            r, g, b = getrgb(x, y)\n",
    "            Y[y][x] = 0.299 * r + 0.587 * g + 0.114 * b\n",
    "            CB[y][x] = -0.168736 * r - 0.331264 * g + 0.5 * b + 128\n",
    "            CR[y][x] = 0.5 * r - 0.418688 * g - 0.081312 * b + 128\n",
    "\n",
    "    mw = 8 * hs\n",
    "    mh = 8 * vs\n",
    "    if gray:\n",
    "        mw = mh = 8\n",
    "    mcux = (w + mw - 1) // mw\n",
    "    mcuy = (h + mh - 1) // mh\n",
    "\n",
    "    def samp(plane, px, py):\n",
    "        return plane[min(py, h - 1)][min(px, w - 1)]\n",
    "\n",
    "    def chroma(plane, bx, by):\n",
    "        \"\"\"One 8x8 block of a subsampled chroma plane.\"\"\"\n",
    "        blk = []\n",
    "        for j in range(8):\n",
    "            for i in range(8):\n",
    "                sx = (bx * 8 + i) * hs\n",
    "                sy = (by * 8 + j) * vs\n",
    "                acc = 0.0\n",
    "                for dy in range(vs):\n",
    "                    for dx in range(hs):\n",
    "                        acc += samp(plane, sx + dx, sy + dy)\n",
    "                blk.append(acc / (hs * vs) - 128)\n",
    "        return blk\n",
    "\n",
    "    dcl, acl = hcodes(DC_L_BITS, DC_L_VALS), hcodes(AC_L_BITS, AC_L_VALS)\n",
    "    dcc, acc_ = hcodes(DC_C_BITS, DC_C_VALS), hcodes(AC_C_BITS, AC_C_VALS)\n",
    "\n",
    "    bw = JBits()\n",
    "    pred = [0, 0, 0]\n",
    "    n = 0\n",
    "    rstn = 0\n",
    "    for my in range(mcuy):\n",
    "        for mx in range(mcux):\n",
    "            if restart and n and n % restart == 0:\n",
    "                bw.align()\n",
    "                bw.b += bytes([0xff, 0xd0 + rstn])\n",
    "                rstn = (rstn + 1) & 7\n",
    "                pred = [0, 0, 0]\n",
    "            for by in range(vs if not gray else 1):\n",
    "                for bx in range(hs if not gray else 1):\n",
    "                    blk = []\n",
    "                    for j in range(8):\n",
    "                        for i in range(8):\n",
    "                            blk.append(samp(Y, (mx * hs + bx) * 8 + i,\n",
    "                                            (my * vs + by) * 8 + j) - 128)\n",
    "                    pred[0] = encode_blk(bw, blk, qy, dcl, acl, pred[0])\n",
    "            if not gray:\n",
    "                pred[1] = encode_blk(bw, chroma(CB, mx, my), qc, dcc, acc_,\n",
    "                                     pred[1])\n",
    "                pred[2] = encode_blk(bw, chroma(CR, mx, my), qc, dcc, acc_,\n",
    "                                     pred[2])\n",
    "            n += 1\n",
    "    bw.align()\n",
    "\n",
    "    out = b\"\\xff\\xd8\"\n",
    "    out += seg(0xe0, b\"JFIF\\x00\\x01\\x01\\x00\\x00\\x01\\x00\\x01\\x00\\x00\")\n",
    "    out += seg(0xdb, bytes([0x00]) + bytes(qy))\n",
    "    if not gray:\n",
    "        out += seg(0xdb, bytes([0x01]) + bytes(qc))\n",
    "    if restart:\n",
    "        out += seg(0xdd, struct.pack(\">H\", restart))\n",
    "    if gray:\n",
    "        sof = struct.pack(\">BHHB\", 8, h, w, 1) + bytes([1, 0x11, 0])\n",
    "    else:\n",
    "        sof = struct.pack(\">BHHB\", 8, h, w, 3) + \\\n",
    "              bytes([1, (hs << 4) | vs, 0, 2, 0x11, 1, 3, 0x11, 1])\n",
    "    out += seg(0xc0, sof)\n",
    "    if merge_dht and not gray:\n",
    "        out += seg(0xfe, b\"one DHT segment, four tables\")\n",
    "        out += seg(0xee, b\"Adobe\\x00d\\x00\\x00\\x00\\x00\\x01\")\n",
    "        out += seg(0xc4,\n",
    "                   bytes([0x00]) + bytes(DC_L_BITS) + bytes(DC_L_VALS) +\n",
    "                   bytes([0x10]) + bytes(AC_L_BITS) + bytes(AC_L_VALS) +\n",
    "                   bytes([0x01]) + bytes(DC_C_BITS) + bytes(DC_C_VALS) +\n",
    "                   bytes([0x11]) + bytes(AC_C_BITS) + bytes(AC_C_VALS))\n",
    "    else:\n",
    "        out += seg(0xc4, bytes([0x00]) + bytes(DC_L_BITS) + bytes(DC_L_VALS))\n",
    "        out += seg(0xc4, bytes([0x10]) + bytes(AC_L_BITS) + bytes(AC_L_VALS))\n",
    "        if not gray:\n",
    "            out += seg(0xc4, bytes([0x01]) + bytes(DC_C_BITS) +\n",
    "                       bytes(DC_C_VALS))\n",
    "            out += seg(0xc4, bytes([0x11]) + bytes(AC_C_BITS) +\n",
    "                       bytes(AC_C_VALS))\n",
    "    if gray:\n",
    "        sos = bytes([1, 1, 0x00, 0, 63, 0])\n",
    "    else:\n",
    "        sos = bytes([3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0])\n",
    "    out += seg(0xda, sos) + bytes(bw.b) + b\"\\xff\\xd9\"\n",
    "    return out\n",
    "\n",
    "def encode_coefs(bw, coef, dctab, actab, pred):\n",
    "    \"\"\"Entropy-code a block of already-quantised coefficients (zigzag).\"\"\"\n",
    "    diff = coef[0] - pred\n",
    "    s = magcat(diff)\n",
    "    c, l = dctab[s]\n",
    "    bw.put(c, l)\n",
    "    if s:\n",
    "        bw.put(diff if diff > 0 else diff + (1 << s) - 1, s)\n",
    "    run = 0\n",
    "    for k in range(1, 64):\n",
    "        if coef[k] == 0:\n",
    "            run += 1\n",
    "            continue\n",
    "        while run > 15:\n",
    "            c, l = actab[0xf0]\n",
    "            bw.put(c, l)\n",
    "            run -= 16\n",
    "        s = magcat(coef[k])\n",
    "        c, l = actab[(run << 4) | s]\n",
    "        bw.put(c, l)\n",
    "        bw.put(coef[k] if coef[k] > 0 else coef[k] + (1 << s) - 1, s)\n",
    "        run = 0\n",
    "    if run:\n",
    "        c, l = actab[0x00]\n",
    "        bw.put(c, l)\n",
    "    return coef[0]\n",
    "\n",
    "def ref_idct(coef_zz):\n",
    "    \"\"\"The IDCT exactly as T.81 defines it, in floating point, so the\n",
    "    integer transform in jpeg.c can be checked against the mathematics\n",
    "    rather than against another integer approximation.\"\"\"\n",
    "    F = [0.0] * 64\n",
    "    for k in range(64):\n",
    "        F[ZIG[k]] = float(coef_zz[k])\n",
    "    out = []\n",
    "    for y in range(8):\n",
    "        for x in range(8):\n",
    "            acc = 0.0\n",
    "            for v in range(8):\n",
    "                for u in range(8):\n",
    "                    cu = (1 / math.sqrt(2)) if u == 0 else 1.0\n",
    "                    cv = (1 / math.sqrt(2)) if v == 0 else 1.0\n",
    "                    acc += cu * cv * F[v * 8 + u] * COS[x][u] * COS[y][v]\n",
    "            out.append(max(0, min(255, int(round(0.25 * acc)) + 128)))\n",
    "    return out\n",
    "\n",
    "def jpeg_from_coefs(blocks, nbx, nby):\n",
    "    \"\"\"Grey JPEG with an all-ones quantisation table, so the coefficients\n",
    "    reach the IDCT untouched.\"\"\"\n",
    "    q = [1] * 64\n",
    "    dcl, acl = hcodes(DC_L_BITS, DC_L_VALS), hcodes(AC_L_BITS, AC_L_VALS)\n",
    "    bw = JBits()\n",
    "    pred = 0\n",
    "    for b in blocks:\n",
    "        pred = encode_coefs(bw, b, dcl, acl, pred)\n",
    "    bw.align()\n",
    "    out = SOI + seg(0xdb, bytes([0]) + bytes(q))\n",
    "    out += seg(0xc0, struct.pack(\">BHHB\", 8, nby * 8, nbx * 8, 1) +\n",
    "               bytes([1, 0x11, 0]))\n",
    "    out += seg(0xc4, bytes([0x00]) + bytes(DC_L_BITS) + bytes(DC_L_VALS))\n",
    "    out += seg(0xc4, bytes([0x10]) + bytes(AC_L_BITS) + bytes(AC_L_VALS))\n",
    "    out += seg(0xda, bytes([1, 1, 0x00, 0, 63, 0])) + bytes(bw.b) + EOI\n",
    "    return out\n",
    "\n",
    "def idct_fixtures():\n",
    "    \"\"\"One 8x8 block per case, decoded and compared against ref_idct.\"\"\"\n",
    "    cases = []\n",
    "    cases.append([0] * 64)                              # flat mid-grey\n",
    "    c = [0] * 64; c[0] = 400; cases.append(c)           # DC only\n",
    "    c = [0] * 64; c[0] = -400; cases.append(c)\n",
    "    for k in (1, 2, 5, 9, 20, 40, 63):                  # one AC at a time\n",
    "        c = [0] * 64; c[0] = 100; c[k] = 200; cases.append(c)\n",
    "    seed = 12345\n",
    "    for _ in range(6):                                  # dense blocks\n",
    "        c = []\n",
    "        for k in range(64):\n",
    "            seed = (seed * 1103515245 + 12345) & 0x7fffffff\n",
    "            v = (seed >> 8) % 401 - 200\n",
    "            c.append(v // (1 + k // 8))\n",
    "        c[0] = 300\n",
    "        cases.append(c)\n",
    "    c = [0] * 64; c[0] = 1023; c[1] = 1023; cases.append(c)   # extremes\n",
    "    c = [0] * 64; c[0] = -1023; c[1] = -1023; cases.append(c)\n",
    "\n",
    "    blocks, exp = [], []\n",
    "    for cs in cases:\n",
    "        blocks.append(cs)\n",
    "        exp.append(ref_idct(cs))\n",
    "    nbx = len(blocks)\n",
    "    px = []\n",
    "    for y in range(8):\n",
    "        row = []\n",
    "        for b in range(nbx):\n",
    "            for x in range(8):\n",
    "                v = exp[b][y * 8 + x]\n",
    "                row.append((v, v, v, 255))\n",
    "        px.append(row)\n",
    "    emit(\"jpg_idct\", \"jpg_idct.jpg\", jpeg_from_coefs(blocks, nbx, 1),\n",
    "         \"tol\", 1, rgba=flatten(px, nbx * 8, 8), w=nbx * 8, h=8)\n",
    "\n",
    "def jpeg_fixtures():\n",
    "    def smooth(x, y):\n",
    "        # Low-frequency content, so quantisation error stays measurable\n",
    "        # rather than dominated by ringing at hard edges.\n",
    "        return (40 + (x * 200) // 40, 60 + (y * 180) // 40,\n",
    "                120 + ((x + y) * 100) // 80)\n",
    "\n",
    "    for (name, w, h, sub, gray, q, rst) in [\n",
    "            (\"jpg_444\", 32, 24, (1, 1), False, 95, 0),\n",
    "            (\"jpg_420\", 32, 24, (2, 2), False, 95, 0),\n",
    "            (\"jpg_422\", 32, 24, (2, 1), False, 95, 0),\n",
    "            (\"jpg_440\", 32, 24, (1, 2), False, 95, 0),\n",
    "            (\"jpg_gray\", 32, 24, (1, 1), True, 95, 0),\n",
    "            (\"jpg_odd\", 17, 9, (2, 2), False, 95, 0),\n",
    "            (\"jpg_rst\", 48, 32, (2, 2), False, 95, 2),\n",
    "            (\"jpg_q50\", 32, 24, (1, 1), False, 50, 0),\n",
    "            (\"jpg_merged\", 32, 24, (2, 2), False, 95, 0)]:\n",
    "        blob = jpeg_encode(w, h, smooth, q, sub, gray, rst,\n",
    "                           merge_dht=(name == \"jpg_merged\"))\n",
    "        px = []\n",
    "        for y in range(h):\n",
    "            row = []\n",
    "            for x in range(w):\n",
    "                r, g, b = smooth(x, y)\n",
    "                if gray:\n",
    "                    v = int(round(0.299 * r + 0.587 * g + 0.114 * b))\n",
    "                    v = max(0, min(255, v))\n",
    "                    row.append((v, v, v, 255))\n",
    "                else:\n",
    "                    row.append((max(0, min(255, r)), max(0, min(255, g)),\n",
    "                                max(0, min(255, b)), 255))\n",
    "            px.append(row)\n",
    "        tol = 12 if q >= 90 else 40\n",
    "        if sub != (1, 1) and not gray:\n",
    "            tol = max(tol, 20)\n",
    "        emit(name, name + \".jpg\", blob, \"tol\", tol,\n",
    "             rgba=flatten(px, w, h), w=w, h=h)\n",
    "\n",
    "    # Progressive must be refused by name, not decoded into noise.\n",
    "    prog = b\"\\xff\\xd8\" + seg(0xdb, bytes([0]) + bytes(qscale(QY, 90)))\n",
    "    prog += seg(0xc2, struct.pack(\">BHHB\", 8, 16, 16, 1) + bytes([1, 0x11, 0]))\n",
    "    prog += seg(0xda, bytes([1, 1, 0, 0, 0, 0])) + b\"\\xff\\xd9\"\n",
    "    emit(\"jpg_prog\", \"jpg_prog.jpg\", prog, \"err\", -8)\n",
    "\n",
    "    # Four components (CMYK) is a documented gap, not a crash.\n",
    "    cmyk = b\"\\xff\\xd8\" + seg(0xdb, bytes([0]) + bytes(qscale(QY, 90)))\n",
    "    cmyk += seg(0xc0, struct.pack(\">BHHB\", 8, 16, 16, 4) +\n",
    "                bytes([1, 0x11, 0, 2, 0x11, 0, 3, 0x11, 0, 4, 0x11, 0]))\n",
    "    cmyk += b\"\\xff\\xd9\"\n",
    "    emit(\"jpg_cmyk\", \"jpg_cmyk.jpg\", cmyk, \"err\", -4)\n",
    "\n",
    "    emit(\"jpg_trunc\", \"jpg_trunc.jpg\",\n",
    "         jpeg_encode(32, 24, smooth, 90, (1, 1), False, 0)[:60], \"err\", -2)\n",
    "\n",
    "# --------------------------------------------------------------- BMP\n",
    "\n",
    "def bmp_build(w, h, getpx, bpp=24, top_down=False, bitfields=False):\n",
    "    stride = ((w * bpp + 31) // 32) * 4\n",
    "    hdr = 40 if not bitfields else 108\n",
    "    off = 14 + hdr\n",
    "    rows = []\n",
    "    order = range(h) if top_down else range(h - 1, -1, -1)\n",
    "    for y in order:\n",
    "        row = bytearray()\n",
    "        for x in range(w):\n",
    "            r, g, b, a = getpx(x, y)\n",
    "            if bpp == 24:\n",
    "                row += bytes((b, g, r))\n",
    "            else:\n",
    "                row += bytes((b, g, r, a))\n",
    "        row += bytes(stride - len(row))\n",
    "        rows.append(bytes(row))\n",
    "    data = b\"\".join(rows)\n",
    "    info = struct.pack(\"<IiiHHIIiiII\", hdr, w, -h if top_down else h, 1, bpp,\n",
    "                       3 if bitfields else 0, len(data), 2835, 2835, 0, 0)\n",
    "    if bitfields:\n",
    "        info += struct.pack(\"<IIII\", 0x00ff0000, 0x0000ff00, 0x000000ff,\n",
    "                            0xff000000)\n",
    "        info += bytes(hdr - len(info))\n",
    "    return b\"BM\" + struct.pack(\"<IHHI\", 14 + hdr + len(data), 0, 0, off) + \\\n",
    "           info + data\n",
    "\n",
    "def bmp_fixtures():\n",
    "    w, h = 13, 7\n",
    "    def g(x, y):\n",
    "        return ((x * 19) % 256, (y * 31) % 256, ((x + y) * 11) % 256, 255)\n",
    "    px = [[g(x, y)[:3] + (255,) for x in range(w)] for y in range(h)]\n",
    "    emit(\"bmp_24\", \"bmp_24.bmp\", bmp_build(w, h, g, 24),\n",
    "         rgba=flatten(px, w, h), w=w, h=h)\n",
    "    emit(\"bmp_24_td\", \"bmp_24_td.bmp\", bmp_build(w, h, g, 24, top_down=True),\n",
    "         rgba=flatten(px, w, h), w=w, h=h)\n",
    "\n",
    "    def ga(x, y):\n",
    "        return ((x * 19) % 256, (y * 31) % 256, ((x + y) * 11) % 256,\n",
    "                0 if (x + y) % 3 == 0 else 255)\n",
    "    pxa = [[ga(x, y) for x in range(w)] for y in range(h)]\n",
    "    emit(\"bmp_32a\", \"bmp_32a.bmp\", bmp_build(w, h, ga, 32),\n",
    "         rgba=flatten(pxa, w, h), w=w, h=h)\n",
    "\n",
    "    # Every alpha byte zero: padding, not transparency, so it is ignored.\n",
    "    def gz(x, y):\n",
    "        return ((x * 19) % 256, (y * 31) % 256, ((x + y) * 11) % 256, 0)\n",
    "    emit(\"bmp_32z\", \"bmp_32z.bmp\", bmp_build(w, h, gz, 32),\n",
    "         rgba=flatten(px, w, h), w=w, h=h)\n",
    "\n",
    "    emit(\"bmp_bf\", \"bmp_bf.bmp\", bmp_build(w, h, ga, 32, bitfields=True),\n",
    "         rgba=flatten(pxa, w, h), w=w, h=h)\n",
    "\n",
    "    trimmed = bmp_build(w, h, g, 24)\n",
    "    pad = (((w * 24 + 31) // 32) * 4) - w * 3\n",
    "    emit(\"bmp_nopad\", \"bmp_nopad.bmp\", trimmed[:len(trimmed) - pad],\n",
    "         rgba=flatten(px, w, h), w=w, h=h)\n",
    "\n",
    "# --------------------------------------------------------------- main\n",
    "\n",
    "png_fixtures()\n",
    "gif_fixtures()\n",
    "jpeg_fixtures()\n",
    "idct_fixtures()\n",
    "bmp_fixtures()\n",
    "with open(os.path.join(OUT, \"index.txt\"), \"w\") as f:\n",
    "    f.write(\"\\n\".join(INDEX) + \"\\n\")\n",
    "print(\"fixtures: %d\" % len(INDEX))\n",
    0
};

static int write_generator(const char *dir)
{
    char path[512];
    FILE *f;
    int i;

    snprintf(path, sizeof(path), "%s/gen.py", dir);
    f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        return -1;
    }
    for (i = 0; GEN_PY[i]; i++)
        fputs(GEN_PY[i], f);
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------ compare */

struct expect {
    int w, h;
    uint8_t *base;   /* the whole file */
    uint8_t *rgba;   /* just the pixels, past the 8-byte size header */
};

static int load_expect(const char *dir, const char *name, struct expect *e)
{
    char path[512];
    unsigned long len;
    uint8_t *b;

    snprintf(path, sizeof(path), "%s/%s.rgba", dir, name);
    b = slurp(path, &len);
    if (!b || len < 8)
        return -1;
    e->w = (int)(b[0] | (b[1] << 8) | (b[2] << 16) | ((unsigned)b[3] << 24));
    e->h = (int)(b[4] | (b[5] << 8) | (b[6] << 16) | ((unsigned)b[7] << 24));
    if ((unsigned long)e->w * (unsigned long)e->h * 4 + 8 != len) {
        free(b);
        return -1;
    }
    e->base = b;
    e->rgba = b + 8;
    return 0;
}

/* Returns the worst per-channel error, and fills *mean. */
static int compare(const struct image *im, const struct expect *e, double *mean)
{
    long i, n = (long)e->w * e->h;
    long worst = 0;
    double sum = 0;

    for (i = 0; i < n; i++) {
        int got[4], want[4], c;

        got[0] = (int)((im->px[i] >> 16) & 0xff);
        got[1] = (int)((im->px[i] >> 8) & 0xff);
        got[2] = (int)(im->px[i] & 0xff);
        got[3] = im->has_alpha ? im->alpha[i] : 255;
        want[0] = e->rgba[i * 4];
        want[1] = e->rgba[i * 4 + 1];
        want[2] = e->rgba[i * 4 + 2];
        want[3] = e->rgba[i * 4 + 3];
        for (c = 0; c < 4; c++) {
            int d = got[c] - want[c];
            if (d < 0)
                d = -d;
            if (d > worst)
                worst = d;
            sum += d;
        }
    }
    *mean = n ? sum / (double)(n * 4) : 0;
    return (int)worst;
}

/* ------------------------------------------------------------ fixtures */

static int run_fixtures(const char *dir)
{
    char path[512];
    unsigned long len;
    uint8_t *idx;
    char *line, *save;
    int total = 0;

    snprintf(path, sizeof(path), "%s/index.txt", dir);
    idx = slurp(path, &len);
    if (!idx) {
        fprintf(stderr, "no index.txt in %s\n", dir);
        return -1;
    }
    idx[len] = 0;

    for (line = strtok_r((char *)idx, "\n", &save); line;
         line = strtok_r(0, "\n", &save)) {
        char name[128], file[128], mode[16];
        int arg, frames, rc, err_expected;
        unsigned long blen;
        uint8_t *blob;
        struct image im;
        struct img_info info;

        if (sscanf(line, "%127s %127s %15s %d %d", name, file, mode, &arg,
                   &frames) != 5)
            continue;
        total++;
        snprintf(path, sizeof(path), "%s/%s", dir, file);
        blob = slurp(path, &blen);
        if (!blob) {
            bad(name, "missing fixture file");
            continue;
        }

        err_expected = (strcmp(mode, "err") == 0);
        if (strcmp(file + strlen(file) - 4, ".gif") == 0) {
            int nf = -1;
            rc = gif_decode(blob, blen, &im, &nf);
            if (!err_expected && rc == IMG_OK && nf != frames)
                bad(name, "frame count %d, expected %d", nf, frames);
            else if (!err_expected && rc == IMG_OK && frames > 1)
                ok(name, "animated, %d frames reported, first decoded", nf);
        } else {
            rc = img_decode(blob, blen, &im);
        }

        if (err_expected) {
            if (rc == arg)
                ok(name, "rejected: %s", img_error(rc));
            else
                bad(name, "status %d (%s), expected %d (%s)", rc,
                    img_error(rc), arg, img_error(arg));
            if (rc == IMG_OK)
                img_free(&im);
            free(blob);
            continue;
        }

        if (rc != IMG_OK) {
            bad(name, "decode failed: %s", img_error(rc));
            free(blob);
            continue;
        }

        {
            struct expect e;
            double mean;
            int worst;

            if (load_expect(dir, name, &e) != 0) {
                bad(name, "no expected-pixel file");
                img_free(&im);
                free(blob);
                continue;
            }
            if (im.w != e.w || im.h != e.h) {
                bad(name, "size %dx%d, expected %dx%d", im.w, im.h, e.w, e.h);
                free(e.base);
                img_free(&im);
                free(blob);
                continue;
            }
            worst = compare(&im, &e, &mean);
            if (worst > arg)
                bad(name, "%dx%d worst channel error %d > %d (mean %.3f)",
                    im.w, im.h, worst, arg, mean);
            else if (arg == 0)
                ok(name, "%dx%d exact%s", im.w, im.h,
                   im.has_alpha ? ", with alpha" : "");
            else
                ok(name, "%dx%d worst %d (<= %d), mean %.3f", im.w, im.h,
                   worst, arg, mean);
            free(e.base);
        }

        /* The header-only probe must agree with the full decode. */
        if (img_probe(blob, blen, &info) == IMG_OK) {
            if (info.w != im.w || info.h != im.h)
                bad(name, "probe says %dx%d, decode says %dx%d", info.w,
                    info.h, im.w, im.h);
            if (info.frames != frames)
                bad(name, "probe frames %d, expected %d", info.frames, frames);
        } else {
            bad(name, "probe failed on a decodable image");
        }

        img_free(&im);
        free(blob);
    }
    free(idx);
    return total;
}

/* ------------------------------------------------------------ scaling */

/* The same separable algorithm libimg documents, in double precision, so
 * the integer version can be checked against it. */
static void ref_line(const double *sc, const double *sa, int sn, int sstep,
                     double *dc, double *da, int dn, int dstep, int alpha)
{
    int i, j, k;

    for (i = 0; i < dn; i++) {
        double acc[3] = { 0, 0, 0 }, aacc = 0, wsum = 0;
        double totw = (double)sn / dn;   /* weights sum to this per output */

        if (dn <= sn) {
            double lo = (double)i * sn / dn, hi = (double)(i + 1) * sn / dn;
            for (j = (int)(lo); j < sn && (double)j < hi; j++) {
                double s = j, e = j + 1.0;
                double w = (e < hi ? e : hi) - (s > lo ? s : lo);
                if (w <= 0)
                    continue;
                if (alpha) {
                    double a = sa[(long)j * sstep];
                    aacc += w * a;
                    wsum += w * a;
                    for (k = 0; k < 3; k++)
                        acc[k] += w * a * sc[((long)j * sstep) * 3 + k];
                } else {
                    wsum += w;
                    for (k = 0; k < 3; k++)
                        acc[k] += w * sc[((long)j * sstep) * 3 + k];
                }
            }
            if (alpha) {
                da[(long)i * dstep] = aacc / totw;
                for (k = 0; k < 3; k++)
                    dc[((long)i * dstep) * 3 + k] =
                        wsum > 0 ? acc[k] / wsum : 0;
            } else {
                for (k = 0; k < 3; k++)
                    dc[((long)i * dstep) * 3 + k] = acc[k] / totw;
            }
        } else {
            double pos = ((double)i + 0.5) * sn / dn - 0.5;
            (void)totw;
            int j0, j1;
            double f;

            if (pos < 0)
                pos = 0;
            if (pos > sn - 1)
                pos = sn - 1;
            j0 = (int)pos;
            f = pos - j0;
            j1 = (j0 + 1 < sn) ? j0 + 1 : j0;
            if (alpha) {
                double a0 = sa[(long)j0 * sstep], a1 = sa[(long)j1 * sstep];
                double w0 = a0 * (1 - f), w1 = a1 * f;
                da[(long)i * dstep] = a0 * (1 - f) + a1 * f;
                for (k = 0; k < 3; k++)
                    dc[((long)i * dstep) * 3 + k] =
                        (w0 + w1) > 0
                            ? (w0 * sc[((long)j0 * sstep) * 3 + k] +
                               w1 * sc[((long)j1 * sstep) * 3 + k]) / (w0 + w1)
                            : 0;
            } else {
                for (k = 0; k < 3; k++)
                    dc[((long)i * dstep) * 3 + k] =
                        sc[((long)j0 * sstep) * 3 + k] * (1 - f) +
                        sc[((long)j1 * sstep) * 3 + k] * f;
            }
        }
    }
}

static double ref_scale_maxdiff(const struct image *src, int dw, int dh,
                                const struct image *got)
{
    int sw = src->w, sh = src->h, alpha = src->has_alpha;
    double *c0, *a0, *c1, *a1, *c2, *a2;
    double worst = 0;
    long i;
    int x, y;
    int horiz_first = ((long)dw * sh <= (long)sw * dh);
    int mw = horiz_first ? dw : sw, mh = horiz_first ? sh : dh;

    c0 = malloc(sizeof(double) * 3 * sw * sh);
    a0 = malloc(sizeof(double) * sw * sh);
    c1 = malloc(sizeof(double) * 3 * mw * mh);
    a1 = malloc(sizeof(double) * mw * mh);
    c2 = malloc(sizeof(double) * 3 * dw * dh);
    a2 = malloc(sizeof(double) * dw * dh);

    for (i = 0; i < (long)sw * sh; i++) {
        c0[i * 3] = (src->px[i] >> 16) & 0xff;
        c0[i * 3 + 1] = (src->px[i] >> 8) & 0xff;
        c0[i * 3 + 2] = src->px[i] & 0xff;
        a0[i] = alpha ? src->alpha[i] : 255;
    }

    if (horiz_first) {
        for (y = 0; y < sh; y++)
            ref_line(c0 + (long)y * sw * 3, a0 + (long)y * sw, sw, 1,
                     c1 + (long)y * dw * 3, a1 + (long)y * dw, dw, 1, alpha);
        for (x = 0; x < dw; x++)
            ref_line(c1 + (long)x * 3, a1 + x, sh, dw,
                     c2 + (long)x * 3, a2 + x, dh, dw, alpha);
    } else {
        for (x = 0; x < sw; x++)
            ref_line(c0 + (long)x * 3, a0 + x, sh, sw,
                     c1 + (long)x * 3, a1 + x, dh, sw, alpha);
        for (y = 0; y < dh; y++)
            ref_line(c1 + (long)y * sw * 3, a1 + (long)y * sw, sw, 1,
                     c2 + (long)y * dw * 3, a2 + (long)y * dw, dw, 1, alpha);
    }

    for (i = 0; i < (long)dw * dh; i++) {
        double g[4], w[4];
        int k;
        g[0] = (got->px[i] >> 16) & 0xff;
        g[1] = (got->px[i] >> 8) & 0xff;
        g[2] = got->px[i] & 0xff;
        g[3] = got->has_alpha ? got->alpha[i] : 255;
        w[0] = c2[i * 3];
        w[1] = c2[i * 3 + 1];
        w[2] = c2[i * 3 + 2];
        w[3] = alpha ? a2[i] : 255;
        /* Where coverage is zero the colour is unconstrained -- the
         * library is free to keep whatever was there. */
        for (k = (alpha && w[3] == 0) ? 3 : 0; k < 4; k++) {
            double d = fabs(g[k] - w[k]);
            if (d > worst)
                worst = d;
        }
    }
    free(c0); free(a0); free(c1); free(a1); free(c2); free(a2);
    return worst;
}

static void mkimg(struct image *im, int w, int h, int alpha)
{
    long i;

    img__alloc(im, w, h, alpha);
    for (i = 0; i < (long)w * h; i++) {
        int x = (int)(i % w), y = (int)(i / w);
        im->px[i] = img__rgb((x * 37 + y * 11) & 0xff, (x * 5 + y * 91) & 0xff,
                             (x * y * 13) & 0xff);
        if (alpha)
            im->alpha[i] = (uint8_t)((x * 29 + y * 71) & 0xff);
    }
}

static void test_scale(void)
{
    static const int sizes[][4] = {
        { 32, 24, 8, 6 }, { 32, 24, 96, 72 }, { 32, 24, 32, 24 },
        { 32, 24, 7, 90 }, { 32, 24, 90, 7 }, { 17, 13, 5, 29 },
        { 1, 1, 16, 16 }, { 64, 1, 3, 1 }, { 5, 5, 1, 1 }
    };
    unsigned i;
    double worst_all = 0;

    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        int alpha;
        for (alpha = 0; alpha < 2; alpha++) {
            struct image src, dst;
            double d;
            char label[64];

            mkimg(&src, sizes[i][0], sizes[i][1], alpha);
            if (img_scale(&src, sizes[i][2], sizes[i][3], &dst) != IMG_OK) {
                bad("scale", "img_scale failed for %dx%d -> %dx%d",
                    sizes[i][0], sizes[i][1], sizes[i][2], sizes[i][3]);
                img_free(&src);
                continue;
            }
            d = ref_scale_maxdiff(&src, sizes[i][2], sizes[i][3], &dst);
            if (d > worst_all)
                worst_all = d;
            snprintf(label, sizeof(label), "scale%s", alpha ? "-a" : "");
            if (d > 1.5)
                bad(label, "%dx%d -> %dx%d: worst deviation %.2f",
                    sizes[i][0], sizes[i][1], sizes[i][2], sizes[i][3], d);
            else
                ok(label, "%2dx%-2d -> %2dx%-2d worst deviation %.2f",
                   sizes[i][0], sizes[i][1], sizes[i][2], sizes[i][3], d);
            img_free(&src);
            img_free(&dst);
        }
    }

    /* Hand-computed cases, where the right answer is not in doubt. */
    {
        struct image src, dst;
        long k;
        int flat = 1;

        img__alloc(&src, 7, 5, 0);
        for (k = 0; k < 35; k++)
            src.px[k] = 0x123456;
        img_scale(&src, 19, 3, &dst);
        for (k = 0; k < 57; k++)
            if (dst.px[k] != 0x123456)
                flat = 0;
        if (flat)
            ok("scale-flat", "a solid colour survives 7x5 -> 19x3 exactly");
        else
            bad("scale-flat", "solid colour was not preserved");
        img_free(&dst);
        img_scale(&src, 2, 2, &dst);
        flat = 1;
        for (k = 0; k < 4; k++)
            if (dst.px[k] != 0x123456)
                flat = 0;
        if (flat)
            ok("scale-flat", "and 7x5 -> 2x2 exactly");
        else
            bad("scale-flat", "solid colour lost on downscale");
        img_free(&dst);
        img_free(&src);
    }
    {
        struct image src, dst;
        int v;

        img__alloc(&src, 2, 2, 0);
        src.px[0] = img__rgb(0, 0, 0);
        src.px[1] = img__rgb(100, 100, 100);
        src.px[2] = img__rgb(200, 200, 200);
        src.px[3] = img__rgb(255, 255, 255);
        img_scale(&src, 1, 1, &dst);
        v = (int)(dst.px[0] & 0xff);
        if (v == 139)
            ok("scale-box", "2x2 mean of 0,100,200,255 -> 139");
        else
            bad("scale-box", "2x2 -> 1x1 gave %d, expected 139", v);
        img_free(&dst);
        img_free(&src);
    }
    {
        struct image src, dst;
        int g[4], k;
        static const int want[4] = { 0, 64, 191, 255 };

        img__alloc(&src, 2, 1, 0);
        src.px[0] = 0;
        src.px[1] = img__rgb(255, 255, 255);
        img_scale(&src, 4, 1, &dst);
        for (k = 0; k < 4; k++)
            g[k] = (int)(dst.px[k] & 0xff);
        if (memcmp(g, want, sizeof(g)) == 0)
            ok("scale-lerp", "2 -> 4 bilinear gives 0,64,191,255");
        else
            bad("scale-lerp", "2 -> 4 gave %d,%d,%d,%d", g[0], g[1], g[2],
                g[3]);
        img_free(&dst);
        img_free(&src);
    }
    {
        /* A transparent pixel must contribute coverage but not colour. */
        struct image src, dst;

        img__alloc(&src, 2, 1, 1);
        src.px[0] = img__rgb(255, 0, 0);
        src.alpha[0] = 0;
        src.px[1] = img__rgb(0, 0, 255);
        src.alpha[1] = 255;
        img_scale(&src, 1, 1, &dst);
        if (dst.px[0] == img__rgb(0, 0, 255) && dst.alpha[0] == 128)
            ok("scale-alpha", "transparent red does not bleed into blue "
                              "(0x%06x, a=%d)",
               dst.px[0], dst.alpha[0]);
        else
            bad("scale-alpha", "got 0x%06x a=%d, wanted 0x0000ff a=128",
                dst.px[0], dst.alpha[0]);
        img_free(&dst);
        img_free(&src);
    }
    {
        struct image src, dst;

        img__alloc(&src, 4, 4, 0);
        if (img_scale(&src, 0, 4, &dst) != IMG_ERR_INVAL ||
            img_scale(&src, 4, -1, &dst) != IMG_ERR_INVAL ||
            img_scale(&src, IMG_MAX_DIM + 1, 4, &dst) != IMG_ERR_TOOBIG ||
            img_scale(&src, 8000, 8600, &dst) != IMG_ERR_TOOBIG)
            bad("scale-limits", "bad target sizes were not rejected");
        else
            ok("scale-limits", "zero, negative and oversized targets rejected");
        img_free(&src);
    }
    (void)worst_all;
}

/* ------------------------------------------------------------ fuzzing */

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;

static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}

static const char *cur_fixture = "?";
static long cur_iter;

static void on_alarm(int sig)
{
    (void)sig;
    fprintf(stderr, "\nHANG in %s iteration %ld\n", cur_fixture, cur_iter);
    fflush(stderr);
    _exit(2);
}

static long fuzz_one(uint8_t *base, unsigned long blen, long iters)
{
    uint8_t *buf = malloc(blen ? blen : 1);
    long i, decoded = 0;

    for (i = 0; i < iters; i++) {
        unsigned long len = blen;
        struct image im;
        struct img_info info;
        int strategy = (int)(rnd() % 5);
        unsigned k;

        cur_iter = i;
        memcpy(buf, base, blen);

        switch (strategy) {
        case 0:                                  /* scattered bit flips */
            for (k = 0; k < 1 + rnd() % 8; k++)
                if (blen)
                    buf[rnd() % blen] ^= (uint8_t)(1u << (rnd() % 8));
            break;
        case 1:                                  /* truncation */
            len = blen ? rnd() % blen : 0;
            break;
        case 2:                                  /* random byte stores */
            for (k = 0; k < 1 + rnd() % 16; k++)
                if (blen)
                    buf[rnd() % blen] = (uint8_t)rnd();
            break;
        case 3:                                  /* splice a chunk */
            if (blen > 16) {
                unsigned long a = rnd() % blen, b = rnd() % blen;
                unsigned long n = rnd() % (blen / 4 + 1);
                if (a + n <= blen && b + n <= blen)
                    memmove(buf + a, buf + b, n);
            }
            break;
        default:                                 /* zero a run */
            if (blen > 8) {
                unsigned long a = rnd() % blen;
                unsigned long n = rnd() % (blen - a);
                memset(buf + a, 0, n);
            }
            break;
        }

        alarm(10);
        img_probe(buf, len, &info);
        if (img_decode(buf, len, &im) == IMG_OK) {
            struct image sc;
            decoded++;
            if (img_scale(&im, 1 + (int)(rnd() % 40), 1 + (int)(rnd() % 40),
                          &sc) == IMG_OK)
                img_free(&sc);
            img_free(&im);
        }
        alarm(0);
    }
    free(buf);
    return decoded;
}

static long fuzz_random(long iters)
{
    static const uint8_t magic[4][4] = {
        { 0x89, 'P', 'N', 'G' }, { 'G', 'I', 'F', '8' },
        { 0xff, 0xd8, 0xff, 0xe0 }, { 'B', 'M', 0, 0 }
    };
    long i, decoded = 0;

    for (i = 0; i < iters; i++) {
        unsigned long len = 16 + rnd() % 900;
        uint8_t *buf = malloc(len);
        struct image im;
        struct img_info info;
        unsigned long k;

        cur_iter = i;
        for (k = 0; k < len; k++)
            buf[k] = (uint8_t)rnd();
        memcpy(buf, magic[rnd() % 4], 4);
        if (buf[0] == 0x89) {
            static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G',
                                            0x0d, 0x0a, 0x1a, 0x0a };
            memcpy(buf, sig, 8);
        } else if (buf[0] == 'G') {
            memcpy(buf, "GIF89a", 6);
        }

        alarm(10);
        img_probe(buf, len, &info);
        if (img_decode(buf, len, &im) == IMG_OK) {
            decoded++;
            img_free(&im);
        }
        alarm(0);
        free(buf);
    }
    return decoded;
}

static int fuzz_mult = 1;

static void run_fuzz(const char *dir)
{
    char path[512];
    unsigned long len;
    uint8_t *idx;
    char *line, *save;
    long total_iters = 0, total_decoded = 0;
    int files = 0;

    signal(SIGALRM, on_alarm);

    snprintf(path, sizeof(path), "%s/index.txt", dir);
    idx = slurp(path, &len);
    if (!idx)
        return;
    idx[len] = 0;

    for (line = strtok_r((char *)idx, "\n", &save); line;
         line = strtok_r(0, "\n", &save)) {
        char name[128], file[128], mode[16];
        int arg, frames;
        unsigned long blen;
        uint8_t *blob;
        long iters;

        if (sscanf(line, "%127s %127s %15s %d %d", name, file, mode, &arg,
                   &frames) != 5)
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, file);
        blob = slurp(path, &blen);
        if (!blob)
            continue;
        cur_fixture = file;
        iters = (blen < 8192 ? 500 : 150) * fuzz_mult;
        total_decoded += fuzz_one(blob, blen, iters);
        total_iters += iters;
        files++;
        free(blob);
    }
    free(idx);

    cur_fixture = "random";
    total_decoded += fuzz_random(4000L * fuzz_mult);
    total_iters += 4000L * fuzz_mult;

    ok("fuzz", "%ld mutated files from %d seeds plus %ld random blobs; "
               "%ld decoded successfully, 0 crashes, 0 hangs, 0 leaks",
       total_iters, files, 4000L * fuzz_mult, total_decoded);
}

/* ------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "/tmp/kimgfix";
    /* argv[2] multiplies the fuzz budget; 1 is a quick smoke run. */
    char cmd[1024];
    int n;

    if (argc > 2)
        fuzz_mult = atoi(argv[2]) > 0 ? atoi(argv[2]) : 1;
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
    if (system(cmd) != 0)
        return 1;
    if (write_generator(dir) != 0)
        return 1;
    snprintf(cmd, sizeof(cmd), "python3 '%s/gen.py' '%s'", dir, dir);
    printf("generating fixtures in %s\n", dir);
    if (system(cmd) != 0) {
        fprintf(stderr, "fixture generation failed (need python3)\n");
        return 1;
    }

    printf("\n-- decoding --\n");
    n = run_fixtures(dir);
    if (n < 0)
        return 1;

    printf("\n-- scaling --\n");
    test_scale();

    printf("\n-- fuzzing --\n");
    run_fuzz(dir);

    printf("\n%d fixtures, %d checks passed, %d failed\n", n, n_pass, n_fail);
    return n_fail ? 1 : 0;
}
