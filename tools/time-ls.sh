#!/bin/sh
# Measure how long `ls /bin` takes inside the VM. The three commands go on
# one line so the shell runs them back to back and the uptime delta is the
# real cost, with no harness sleep inside the window.
{
    sleep 10
    printf 'root\r'
    sleep 1
    printf 'root\r'
    sleep 3
    printf 'uptime; ls /bin > /tmp/l.txt; uptime\r'
    sleep 50
    printf 'wc -l /tmp/l.txt\r'
    sleep 3
} | timeout 100 qemu-system-x86_64 \
      -drive file=build/os.img,format=raw -no-reboot -display none \
      -serial stdio -device rtl8139,netdev=n0 -netdev user,id=n0 2>&1 \
  | grep -E 'up [0-9]|l\.txt'
