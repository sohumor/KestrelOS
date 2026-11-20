#!/bin/sh
# run-qemu.sh - boot the KestrelOS disk image in QEMU (Linux / WSL2).
#
#   sh run-qemu.sh [--headless] [--image PATH] [--memory SIZE] [--] [extra qemu args...]
#
#   --headless      no VGA window; the serial console on stdio is the only UI
#   --image PATH    disk image to boot (default: build/os.img)
#   --memory SIZE   guest RAM, QEMU syntax (default: 256M)
#   --disk MODE     ata (default), ahci (q35), or virtio; also set via DISK
#   --nic MODE      rtl8139 (default), e1000, or virtio; also set via NIC
#
# Anything after "--" (or any unrecognised argument) is passed straight
# through to qemu-system-x86_64.
# KVM is preferred when /dev/kvm is accessible; otherwise QEMU uses TCG.
# To force software emulation, pass -- -accel tcg.
#
# Networking: an RTL8139 NIC on QEMU user-mode networking. The kernel
# configures itself statically for that setup (10.0.2.15/24, gw 10.0.2.2,
# dns 10.0.2.3) -- see kernel/net.c.

set -u

QEMU="${QEMU:-qemu-system-x86_64}"
IMAGE="build/os.img"
MEMORY="256M"
DISK="${DISK:-ata}"
NIC="${NIC:-rtl8139}"
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
    --disk)
        [ $# -ge 2 ] || { echo "run-qemu: --disk needs ata, ahci, or virtio" >&2; exit 2; }
        DISK="$2"
        shift 2
        ;;
    --nic)
        [ $# -ge 2 ] || { echo "run-qemu: --nic needs rtl8139, e1000, or virtio" >&2; exit 2; }
        NIC="$2"
        shift 2
        ;;
    -h|--help)
        sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
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

case "$DISK" in
ata) STORAGE="-drive file=$IMAGE,format=raw" ;;
ahci) STORAGE="-machine q35 -drive if=none,id=d0,file=$IMAGE,format=raw -device ide-hd,drive=d0,bus=ide.0" ;;
virtio) STORAGE="-drive if=none,id=d0,file=$IMAGE,format=raw -device virtio-blk-pci,drive=d0" ;;
*) echo "run-qemu: invalid disk mode: $DISK" >&2; exit 2 ;;
esac
CMD="$QEMU $STORAGE -m $MEMORY -no-reboot"
case "$EXTRA " in
    *" -accel "*|*" --accel "*) ;; # Explicit -accel cannot mix with -machine accel=.
    *)
        if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
            CMD="$CMD -machine accel=kvm:tcg"
        fi
        ;;
esac
case "$NIC" in
rtl8139|e1000) NIC_DEVICE="$NIC" ;;
virtio) NIC_DEVICE="virtio-net-pci" ;;
*) echo "run-qemu: invalid NIC mode: $NIC" >&2; exit 2 ;;
esac
CMD="$CMD -device $NIC_DEVICE,netdev=n0 -netdev user,id=n0"
CMD="$CMD -serial stdio$DISPLAY_ARGS$EXTRA"

echo "+ $CMD"
# Word splitting here is deliberate: CMD is a fully assembled command line.
exec $CMD
