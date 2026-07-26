#!/bin/sh
# Boot the image with different amounts of video memory and report which
# graphics mode the bootloader ends up selecting.
for v in 8 16 32 64; do
    printf -- '--- vgamem %sM ---\n' "$v"
    timeout 16 qemu-system-x86_64 -m 512M \
        -device VGA,vgamem_mb="$v" \
        -drive file=build/os.img,format=raw \
        -no-reboot -display none -serial stdio </dev/null 2>&1 \
        | grep -E '^fb:|no VBE|text mode' || echo "(no fb line)"
done
