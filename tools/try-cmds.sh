#!/bin/sh
# Log in as root and run commands with a generous pause between each.
#   tools/try-cmds.sh <seconds-per-command> <command>...
GAP="${1:-4}"
shift

{
    sleep 10
    printf 'root\r'
    sleep 1
    printf 'root\r'
    sleep 3
    for c in "$@"; do
        printf '%s\r' "$c"
        sleep "$GAP"
    done
    sleep 2
} | timeout 180 qemu-system-x86_64 -m 512M \
      -drive file=build/os.img,format=raw -no-reboot -display none \
      -serial stdio -device e1000,netdev=n0 -netdev user,id=n0 2>&1 \
  | sed -n '/welcome, root/,$p'
