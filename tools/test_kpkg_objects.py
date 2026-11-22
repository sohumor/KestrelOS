#!/usr/bin/env python3
"""Check digest-addressed package installs without their original archive."""

from pathlib import Path

from e2e import Harness, PKG_TIMEOUT, qemu_command, t_boot, t_login, t_prompt, wait_prompt


def check(h, command, status=0, output=None):
    h.send(command + "; echo OBJECT-STATUS-$?")
    if output:
        h.expect(output, timeout=PKG_TIMEOUT)
    h.expect(f"\nOBJECT-STATUS-{status}\n", timeout=PKG_TIMEOUT)
    wait_prompt(h, timeout=PKG_TIMEOUT)


def main():
    sha = Path("build/repo/hello.kpkg").read_bytes()[-32:].hex()
    path = f"/var/pkg/objects/{sha[:8]}/{sha[8:]}"
    h = Harness(qemu_command("rtl8139", 2))
    try:
        h.start()
        t_boot(h)
        t_login(h)
        t_prompt(h)

        check(h, "kpkg install hello", output=f"kpkg: object {sha}")
        check(h, "kpkg remove hello")
        check(h, "rm /var/pkg/repo/hello.kpkg")
        check(h, "kpkg install hello", output=f"kpkg: object {sha}")
        check(h, "kpkg remove hello")
        check(h, "mv /var/pkg/repo/index.kpi /var/pkg/repo/index.saved")
        check(h, f"kpkg install-object {sha}", output=f"kpkg: object {sha}")
        check(h, "hello object", output="Hello, object!")
        check(h, "kpkg verify hello", output="kpkg: everything matches")

        check(h, "kpkg remove hello")
        check(h, "kpkg remove kestrel-extras")
        check(h, f"kpkg install-object {sha}", 1,
              "offline object install requires installed kestrel-extras")
        check(h, f"rm {path}")
        check(h, f"cp /var/pkg/repo/kestrel-extras.kpkg {path}")
        check(h, f"kpkg install-object {sha}", 1, "does not match requested object")
        check(h, "kpkg install-object ../../bad", 1, "usage: kpkg install-object")
        print("PASS kpkg objects: indexed reuse, offline replay, missing dependency and wrong digest rejection")
    except (OSError, RuntimeError, AssertionError) as exc:
        print(f"FAIL kpkg objects: {exc}")
        h.print_tail()
        return 1
    finally:
        h.kill()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
