#!/bin/sh
# run-qemu.sh - boot the KestrelOS disk image in QEMU (Linux / WSL2).
#
#   sh run-qemu.sh [--headless] [--image PATH] [--memory SIZE] [--] [extra qemu args...]
#
#   --headless      no VGA window; the serial console on stdio is the only UI
#   --image PATH    disk image to boot (default: build/os.img)
#   --memory SIZE   guest RAM, QEMU syntax (default: 256M)
#
# Anything after "--" (or any unrecognised argument) is passed straight
# through to qemu-system-x86_64.
#
# Networking: an RTL8139 NIC on QEMU user-mode networking. The kernel
# configures itself statically for that setup (10.0.2.15/24, gw 10.0.2.2,
# dns 10.0.2.3) -- see kernel/net.c.

set -u

QEMU="${QEMU:-qemu-system-x86_64}"
IMAGE="build/os.img"
MEMORY="256M"
HEADLESS=0
EXTRA=""

while [ $# -gt 0 ]; do
    case "$1" in
    --headless|-headless)
        HEADLESS=1
        shift
        ;;
    --image)
        [ $# -ge 2 ] || { echo "run-qemu: --image needs a path" >&2; exit 2; }
        IMAGE="$2"
        shift 2
        ;;
    --memory)
        [ $# -ge 2 ] || { echo "run-qemu: --memory needs a size" >&2; exit 2; }
        MEMORY="$2"
        shift 2
        ;;
    -h|--help)
        sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    --)
        shift
        while [ $# -gt 0 ]; do
            EXTRA="$EXTRA $1"
            shift
        done
        ;;
    *)
        EXTRA="$EXTRA $1"
        shift
        ;;
    esac
done

if ! command -v "$QEMU" >/dev/null 2>&1; then
    echo "run-qemu: $QEMU not found on PATH." >&2
    echo "  Debian/Ubuntu: sudo apt install qemu-system-x86" >&2
    echo "  Fedora:        sudo dnf install qemu-system-x86" >&2
    echo "  Arch:          sudo pacman -S qemu-system-x86" >&2
    exit 1
fi

if [ ! -f "$IMAGE" ]; then
    echo "run-qemu: $IMAGE not found -- run 'make' first." >&2
    exit 1
fi

DISPLAY_ARGS=""
if [ "$HEADLESS" -eq 1 ]; then
    DISPLAY_ARGS=" -display none"
fi

CMD="$QEMU -drive file=$IMAGE,format=raw -m $MEMORY -no-reboot"
CMD="$CMD -device rtl8139,netdev=n0 -netdev user,id=n0"
CMD="$CMD -serial stdio$DISPLAY_ARGS$EXTRA"

echo "+ $CMD"
# Word splitting here is deliberate: CMD is a fully assembled command line.
exec $CMD
