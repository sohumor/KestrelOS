#!/bin/sh
# Boot the image headless, optionally type some shell commands, and capture
# the VGA framebuffer to a PNG.
#
#   tools/screenshot.sh out.png [seconds_before_capture] [command ...]
#
# Commands are typed into the guest over the serial console before the shot.

set -e

OUT="${1:-build/shots/vga.png}"
DELAY="${2:-7}"
shift 2 2>/dev/null || true

mkdir -p "$(dirname "$OUT")"
PPM="${OUT%.png}.ppm"
FIFO="$(mktemp -u)"
MON="$(mktemp -u)"
mkfifo "$FIFO"
rm -f "$PPM"

# Feed serial input (shell commands) on a fifo.
{
    sleep 6
    for cmd in "$@"; do
        printf '%s\r' "$cmd"
        sleep 2
    done
    sleep 30
} > "$FIFO" &
FEEDER=$!

# The fifo is QEMU's stdin and the serial port is `stdio`, so the keystrokes
# reach the guest: a `-chardev` that no device references is created and then
# silently ignored, which is why nothing used to be typed. The monitor moves
# to a unix socket so we can still drive screendump.
qemu-system-x86_64 \
      -drive file=build/os.img,format=raw -no-reboot -m 256M \
      -device rtl8139,netdev=n0 -netdev user,id=n0 \
      -display none -serial stdio \
      -monitor "unix:$MON,server=on,wait=off" \
      < "$FIFO" >/dev/null 2>&1 &
QEMU=$!

# Wait out the boot plus the typing, screendump, and -- because screendump
# returns before the file is complete -- wait for the ppm to settle on disk
# before quitting. That race is what produced the odd "no such file" run.
WAIT="$DELAY"
for cmd in "$@"; do WAIT=$((WAIT + 2)); done

python3 - "$MON" "$PPM" "$WAIT" <<'PY' || true
import os
import socket
import sys
import time

mon, ppm, wait = sys.argv[1], sys.argv[2], float(sys.argv[3])

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
deadline = time.time() + 30
while True:
    try:
        s.connect(mon)
        break
    except OSError:
        if time.time() > deadline:
            sys.exit("screenshot: QEMU monitor never came up")
        time.sleep(0.2)

time.sleep(wait)
s.sendall(b"screendump " + ppm.encode() + b"\n")

size, stable, deadline = -1, 0, time.time() + 20
while time.time() < deadline:
    time.sleep(0.25)
    try:
        now = os.path.getsize(ppm)
    except OSError:
        continue
    if now > 0 and now == size:
        stable += 1
        if stable >= 2:
            break
    else:
        stable = 0
    size = now

s.sendall(b"quit\n")
s.close()
PY

wait "$QEMU" 2>/dev/null || true
kill $FEEDER 2>/dev/null || true
rm -f "$FIFO" "$MON"

if [ ! -s "$PPM" ]; then
    echo "screenshot: no framebuffer captured from build/os.img" >&2
    rm -f "$PPM"
    exit 1
fi

python3 tools/ppm2png.py "$PPM" "$OUT"
rm -f "$PPM"
echo "screenshot: $OUT"
