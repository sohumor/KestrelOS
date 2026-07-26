#!/bin/sh
# Drive a login session over the serial console and run a few commands.
# usage: tools/try-login.sh [user] [password] [command ...]
USER_NAME="${1:-kestrel}"
PASS="${2:-kestrel}"
shift 2 2>/dev/null || true

{
    sleep 10
    printf '%s\r' "$USER_NAME"
    sleep 1
    printf '%s\r' "$PASS"
    sleep 3
    for c in "$@"; do
        printf '%s\r' "$c"
        sleep 2
    done
    sleep 2
} | timeout 45 qemu-system-x86_64 -m 512M \
      -drive file=build/os.img,format=raw -no-reboot -display none \
      -serial stdio -device e1000,netdev=n0 -netdev user,id=n0 2>&1 | tail -45
