#!/bin/sh
# Log in, launch a GUI program, and capture the screen.
#   tools/shot-gui.sh out.png "command" [extra seconds]
OUT="${1:-build/shots/gui.png}"
CMD="${2:-desktop}"
EXTRA="${3:-8}"

mkdir -p "$(dirname "$OUT")"
PPM="${OUT%.png}.ppm"
MON="$(mktemp -u)"
FIFO="$(mktemp -u)"
mkfifo "$FIFO"
rm -f "$PPM"

{
    sleep 10
    printf 'root\r'
    sleep 1
    printf 'root\r'
    sleep 3
    printf '%s\r' "$CMD"
    sleep 600
} > "$FIFO" &
FEEDER=$!

qemu-system-x86_64 -m 512M \
    -drive file=build/os.img,format=raw -no-reboot \
    -device e1000,netdev=n0 -netdev user,id=n0 \
    -display none -serial stdio \
    -monitor "unix:$MON,server=on,wait=off" \
    < "$FIFO" > /dev/null 2>&1 &
QEMU=$!

WAIT=$((14 + EXTRA))
python3 - "$MON" "$PPM" "$WAIT" <<'PY' || true
import os, socket, sys, time
mon, ppm, wait = sys.argv[1], sys.argv[2], float(sys.argv[3])
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
deadline = time.time() + 30
while True:
    try:
        s.connect(mon)
        break
    except OSError:
        if time.time() > deadline:
            sys.exit("no monitor")
        time.sleep(0.2)
time.sleep(wait)
s.sendall(b"screendump " + ppm.encode() + b"\n")
size, stable, deadline = -1, 0, time.time() + 25
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

[ -s "$PPM" ] || { echo "no framebuffer captured" >&2; exit 1; }
python3 tools/ppm2png.py "$PPM" "$OUT"
rm -f "$PPM"
