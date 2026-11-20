# Console scroll measurement

Measured on Linux, QEMU TCG, 128 MiB, two vCPUs, snapshot ATA disk,
rtl8139 NIC, headless display with the normal VBE framebuffer. Changes are
based on `origin/main`; only the host profiler from PR #53 was used, not
that branch's kernel or userspace changes. `apps/ls.c` is unchanged.

## Method

Build each version with `make -j8`. Fetch the measurement tool with:

```sh
git fetch origin pull/53/head:profile
git show profile:tools/profile.py > build/profile-pr53.py
```

Run the following once before and once after the console change, changing
`stage` accordingly. Each sample boots a fresh snapshot VM, logs in, sends
one command and waits for the next shell prompt. The reported command-end
delta is host monotonic wall time, including serial echo/output and QMP
sampling overhead, but excluding boot/login. Sampling is 200 Hz across both
CPUs (so percentages include the idle CPU). Do not run builds or other VMs
concurrently with these measurements.

```sh
KESTREL_E2E_TIMEOUT_SCALE=6 PYTHONPATH=tools python3 -u - <<'PY'
import pathlib, runpy, sys
p = runpy.run_path('build/profile-pr53.py')
p['collect'].__globals__['ROOT'] = pathlib.Path.cwd()
stage = 'baseline'  # 'after' for the optimized kernel
for label, cmd in [('ls', 'ls /bin'),
                   ('stress', 'kjs -e "for(var i=0;i<2000;i++) console.log(i);"')]:
    for i in range(1, 4):
        sys.argv = ['profile', '--cmd', cmd,
                    '--out', f'build/{stage}-{label}-{i}.txt',
                    '--json', f'build/{stage}-{label}-{i}.json']
        assert p['main']() == 0
PY
```

The baseline stress initially exceeded the profiler's default 30-second
command timeout; those incomplete attempts are excluded. The sixfold
timeout above permits complete runs without changing sampling frequency.

## Results

| Workload | Before runs (ms) | After runs (ms) | Before median | After median | Speedup |
| --- | --- | --- | ---: | ---: | ---: |
| `ls /bin` | 3436.619, 3353.553, 3586.523 | 272.057, 284.521, 281.611 | 3436.619 ms | 281.611 ms | 12.2x |
| 2000 numbered lines | 103047.811, 105945.126, 116112.417 | 1889.659, 2243.283, 1867.326 | 105945.126 ms | 1889.659 ms | 56.1x |

Median combined `memcpy`/`memmove` share of **all CPU samples** fell from
47.45% to 20.37% for `ls`, and 49.45% to 9.82% for the stress command.
No `memmove` samples appeared in the optimized runs. These are statistical
RIP samples, not call-stack attribution; userspace symbol attribution in
PR #53's tool is approximate. The elapsed-time improvement is the primary
metric. Results are workload-dependent: dense, changing full-width text
still requires repainting most cells; this optimization particularly avoids
copying blank margins and unchanged cells.

## Cause and change

`kernel/devfs.c:write_console` dispatches guest console writes through
`kernel/output.c:output_write`/`output_putc_locked` into
`kernel/console.c:console_putc`. On reaching the bottom row, `scroll`
previously moved nearly the entire RAM pixel shadow and set `full_flush`,
causing `fb_sync` to call `kernel/fb.c:fb_flush` for the whole display.
Both copies used the byte loops in `kernel/string.c`. This happened for
every line, even when most of the screen was blank or unchanged.

As a countercheck, baseline `ls /bin > /tmp/ls.txt` took 1749.165 ms and
showed no `memmove` samples; it was dominated by ATA filesystem writes
instead. Redirection has its own substantial cost, so this is not a pure
estimate of `ls`'s non-console work, but it disproves directory traversal
alone accounting for the observed copy hotspot. `fb_init` switches the
console to the framebuffer backend; the captured QEMU image independently
demonstrates this backend is active. The framebuffer driver already writes
from a RAM shadow and never reads VRAM, so VRAM readback was not the cause.

Scrolling now updates cells top-to-bottom through `cell_put`, which marks
only changed character/attribute spans. Existing `fb_sync` cursor handling,
dirty-row painting and rectangular flushes update those spans without any
pixel move or scroll-triggered full-screen flush. Clear-screen full flushes
and the VGA text backend are unchanged. Compositor ownership still defers
all painting while accumulating dirty cells.

## Focused regression check

```sh
cc -O2 -Wall -Wextra -ffunction-sections -fdata-sections \
  -Ikernel/include -Iabi tools/test_console_scroll.c \
  -Wl,--gc-sections -o build/test_console_scroll
build/test_console_scroll
```

The host test checks cell contents against pre-scroll snapshots and device
output against the cell grid, including cursor inversion, colors, pending
dirty cells, compositor suppression, one-row and maximum-height grids.
It also asserts zero flushes for an unchanged empty screen, exactly two
cell flushes for a sparse shift, and no full framebuffer flush on scroll.

## Release and visual checks

`python3 tools/build.py all` completed successfully (host checks, 76/76
guest E2E checks, committed-journal boot recovery). The separate
`python3 tools/e2e.py --smoke --nic e1000` completed 4/4 checks with no skips.
The focused scroll check above also passed.

After the numbered-line stress returned to the prompt, QMP's
`human-monitor-command` executed
`screendump /home/ubuntu/repos/KestrelOS/build/console-scroll.ppm`.
`python3 tools/ppm2png.py build/console-scroll.ppm build/console-scroll.png`
produced a 2560x1440 image, visually checked for consecutive lines
1911 through 1999 and an intact shell prompt/cursor without ghost text.
The capture helper is retained locally at `build/capture-console.py`.

Real hardware, the legacy VGA fallback, non-default framebuffer geometries,
and dense full-width text performance were not measured. The host test
covers geometry boundaries and compositor suppression, but no interactive
desktop transition or ANSI color stress was visually checked.
