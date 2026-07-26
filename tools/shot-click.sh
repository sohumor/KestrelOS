#!/bin/sh
# Log in, start the desktop, click somewhere with the emulated mouse, and
# capture the screen. Proves the compositor's input routing end to end.
#   tools/shot-click.sh out.png X Y [more X Y pairs...]
OUT="$1"
shift

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
    printf 'desktop\r'
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

python3 - "$MON" "$PPM" "$@" <<'PY' || true
import os, socket, sys, time

mon, ppm = sys.argv[1], sys.argv[2]
coords = [int(v) for v in sys.argv[3:]]

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

def cmd(text):
    s.sendall(text.encode() + b"\n")
    time.sleep(0.35)

# Wait for login + desktop startup.
time.sleep(24)

# A PS/2 packet carries a 9-bit signed delta, so anything past +/-255 sets
# the overflow bit and the driver (rightly) throws the packet away. Move in
# small steps instead.
STEP = 100

def move_to(x, y):
    cmd("mouse_move -200 -200")          # park at the origin; the driver clamps
    for _ in range(20):
        cmd("mouse_move -200 -200")
    cx = cy = 0
    while cx != x or cy != y:
        dx = max(-STEP, min(STEP, x - cx))
        dy = max(-STEP, min(STEP, y - cy))
        cmd("mouse_move %d %d" % (dx, dy))
        cx += dx
        cy += dy

for i in range(0, len(coords), 2):
    move_to(coords[i], coords[i + 1])
    cmd("mouse_button 1")
    cmd("mouse_button 0")
    time.sleep(3)

time.sleep(4)
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
