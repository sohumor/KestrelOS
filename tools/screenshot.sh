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
mkfifo "$FIFO"

# Feed serial input (shell commands) on one pipe, drive the QEMU monitor on
# stdio so we can issue screendump at the right moment.
{
    sleep 6
    for cmd in "$@"; do
        printf '%s\r' "$cmd"
        sleep 2
    done
    sleep 30
} > "$FIFO" &
FEEDER=$!

{
    sleep "$DELAY"
    for cmd in "$@"; do sleep 2; done
    printf 'screendump %s\n' "$PPM"
    sleep 1
    printf 'quit\n'
} | qemu-system-x86_64 \
      -drive file=build/os.img,format=raw -no-reboot -m 256M \
      -device rtl8139,netdev=n0 -netdev user,id=n0 \
      -display none -serial "file:/dev/stdout" -chardev pipe,id=in,path="$FIFO" \
      -monitor stdio >/dev/null 2>&1 || true

kill $FEEDER 2>/dev/null || true
rm -f "$FIFO"

python3 tools/ppm2png.py "$PPM" "$OUT"
rm -f "$PPM"
echo "screenshot: $OUT"
