#!/usr/bin/env python3
"""Verify service graph startup, dependency loss, and reload in a guest."""

from pathlib import Path
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory

from e2e import Harness, qemu_command, t_boot, t_login, t_prompt, wait_prompt


def probe_image(work):
    root = work / "rootfs"
    shutil.copytree("build/rootfs", root)
    services = root / "etc/services"
    daemon = "exec=/bin/sleep\nargs=60\nrestart=never\n"
    leaf = ("exec=/bin/touch\nargs=/run/graph.ready\n"
            "ready=/run/graph.ready\nrestart=never\nenabled=no\n")
    (services / "graph-top.svc").write_text(daemon + "requires=graph-middle\n")
    (services / "graph-middle.svc").write_text(
        daemon + "requires=graph-leaf\nenabled=no\n")
    (services / "graph-leaf.svc").write_text(leaf)
    (root / "etc/graph-cycle.svc").write_text(leaf + "after=graph-top\n")
    (root / "etc/graph-switch.svc").write_text(
        daemon + "requires=logger\nenabled=no\n")
    with (root / "etc/inittab").open("a") as table:
        table.write("\nservice graph-top\nservice graph-middle\nservice graph-leaf\n")
    fs = work / "fs.img"
    subprocess.run([sys.executable, "tools/mkfs.py", "--mode", "/etc/shadow:0600",
                    "--mode", "/var/pkg/db:0700", "--mode", "/tmp:0777",
                    "--mode", "/run:0755", root, fs, "64"], check=True)
    image = work / "os.img"
    subprocess.run([sys.executable, "tools/mkimage.py", "build/stage1.bin",
                    "build/stage2.bin", "build/kernel.bin", image, fs], check=True)
    return image


def shell(h, command, output=None, regex=False):
    h.send(command)
    if output:
        h.expect(output, regex=regex)
    wait_prompt(h)


def state(h, unit, expected):
    shell(h, f"service status {unit}", rf"\n  state +{expected}\n", regex=True)


def check_graph(h):
    shell(h, "sleep 1")
    state(h, "graph-top", "running")
    state(h, "graph-middle", "running")
    state(h, "graph-leaf", "exited")

    shell(h, "cp /etc/graph-cycle.svc /etc/services/graph-leaf.svc")
    shell(h, "service reload graph-leaf", "err reload graph-leaf (invalid configuration)")
    state(h, "graph-top", "running")
    state(h, "graph-middle", "running")

    shell(h, "service stop graph-leaf", "ok stop graph-leaf")
    state(h, "graph-top", "failed")
    state(h, "graph-middle", "failed")
    shell(h, "sleep 1")
    shell(h, "service reset-failed graph-middle", "ok reset-failed graph-middle (stopped)")
    shell(h, "service start graph-top", "ok start graph-top (running)")
    state(h, "graph-middle", "running")

    shell(h, "service stop graph-leaf", "ok stop graph-leaf")
    state(h, "graph-top", "failed")
    shell(h, "sleep 1")
    shell(h, "cp /etc/graph-switch.svc /etc/services/graph-middle.svc")
    shell(h, "service reload graph-middle", "ok reload graph-middle (stopped)")
    shell(h, "service start graph-top", "ok start graph-top (running)")
    state(h, "graph-leaf", "stopped")
    print("PASS init service graph: transitive readiness and loss, rejected and committed reload")


def main():
    with TemporaryDirectory() as directory:
        image = probe_image(Path(directory))
        command = qemu_command("rtl8139", 2)
        command[command.index("-drive") + 1] = f"file={image},format=raw"
        h = Harness(command)
        try:
            h.start()
            t_boot(h)
            t_login(h)
            t_prompt(h)
            check_graph(h)
        except (OSError, RuntimeError, AssertionError) as exc:
            print(f"FAIL init service graph: {exc}", file=sys.stderr)
            h.print_tail()
            return 1
        finally:
            h.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
