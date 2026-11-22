#!/usr/bin/env python3
"""Run directory benchmarks: make -j8 && python3 tools/bench_kfs_dir.py."""

import statistics

from e2e import Harness, find_qemu, t_boot, t_login, t_prompt


def main():
    h = Harness([find_qemu(), "-drive", "file=build/os.img,format=raw",
                 "-snapshot", "-m", "512M", "-smp", "2", "-device",
                 "VGA,vgamem_mb=32", "-device", "e1000,netdev=n0",
                 "-netdev", "user,id=n0", "-display", "none",
                 "-serial", "stdio", "-no-reboot"])
    h.start()
    try:
        t_boot(h)
        t_login(h)
        t_prompt(h)
        for mode, path, count in [("stat", "/bin/yes", 1000),
                                  ("stat", "/bin/about", 1000),
                                  ("stat", "/bin/no-such-dirbench", 1000),
                                  ("create", "/tmp/dirbench", 200)]:
            times = []
            for run in range(3):
                target = path + str(run) if mode == "create" else path
                h.send("dirbench %s %s %d" % (mode, target, count))
                m = h.expect(r"dirbench: .* n=(\d+) ok=(\d+) errors=(\d+) ms=(\d+)\n",
                             regex=True, timeout=180)
                print(m.group(0), flush=True)
                expected = 0 if "no-such" in path else count
                assert int(m.group(2)) == expected
                assert int(m.group(3)) == count - expected
                times.append(int(m.group(4)))
                t_prompt(h)
            print("%s %s runs=%s median_ms=%s" %
                  (mode, path, times, statistics.median(times)), flush=True)
    finally:
        h.kill()


if __name__ == "__main__":
    main()
