#!/usr/bin/env python3
"""Verify that mount attachments hold and release a block provider reference."""

from e2e import Harness, qemu_command, t_boot, t_login, t_prompt, wait_prompt


def check(h, command, status, output=None):
    h.send(command + "; echo LEASE-STATUS-$?")
    if output:
        h.expect(output)
    h.expect(f"\nLEASE-STATUS-{status}\n")
    wait_prompt(h)


def main():
    h = Harness(qemu_command("rtl8139", 2))
    try:
        h.start()
        t_boot(h)
        t_login(h)
        t_prompt(h)
        check(h, "mount -t kfs ram0 /lease", 1, "no available block device 'ram0'")
        check(h, "insmod ramdisk", 0, "ram0 registered")
        check(h, "mount -t kfs ram0 /lease", 1, "cannot mount ram0 on /lease")
        check(h, "rmmod ramdisk", 0)

        check(h, "insmod ramdisk", 0, "ram0 registered")
        check(h, "mount -t devfs ram0 /lease", 0)
        check(h, "mount -t devfs ram0 /lease", 1, "/lease is already a mount point")
        check(h, "rmmod ramdisk", 1, "is in use (1 references)")
        check(h, "mount -u /lease", 0)
        check(h, "rmmod ramdisk", 0)
        check(h, "mount -t devfs ram0 /lease", 1, "no available block device 'ram0'")
        print("PASS mount block leases: failed attachment releases the provider; live attachment pins it")
    except (OSError, RuntimeError, AssertionError) as exc:
        print(f"FAIL mount block leases: {exc}")
        h.print_tail()
        return 1
    finally:
        h.kill()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
