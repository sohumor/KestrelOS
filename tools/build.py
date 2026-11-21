#!/usr/bin/env python3
"""Cross-platform parallel build, host-test, and verification runner for KestrelOS.

Works identically on Windows (Clang/LLD/NASM) and Linux/WSL (GCC/Clang + GNU ld/LLD),
replicating all rules in the top-level Makefile with `.d` dependency tracking.
"""

import argparse
import concurrent.futures
import glob
import json
import os
import re
import shlex
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")

BROWSER_LIBS = ["libz", "libtls", "libimg", "libweb", "libjs"]
HTTP_TLS_APPS = {"curl", "wget", "kpkg"}


def find_tool(names, win_candidates=()):
    for n in names:
        p = shutil.which(n)
        if p:
            return p
    if sys.platform == "win32":
        for c in win_candidates:
            if c and os.path.isfile(c):
                return c
    raise RuntimeError("Required tool not found: %s" % ", ".join(names))


def detect_toolchain():
    cc = find_tool(
        [os.environ.get("CC", "")] if os.environ.get("CC") else ["clang", "gcc"],
        [r"C:\Program Files\LLVM\bin\clang.exe"],
    )
    ld = find_tool(
        [os.environ.get("LD", "")] if os.environ.get("LD") else ["ld.lld", "ld"],
        [r"C:\Program Files\LLVM\bin\ld.lld.exe"],
    )
    ar = find_tool(
        [os.environ.get("AR", "")] if os.environ.get("AR") else ["llvm-ar", "ar"],
        [r"C:\Program Files\LLVM\bin\llvm-ar.exe"],
    )
    objcopy = find_tool(
        [os.environ.get("OBJCOPY", "")] if os.environ.get("OBJCOPY") else ["llvm-objcopy", "objcopy"],
        [r"C:\Program Files\LLVM\bin\llvm-objcopy.exe"],
    )
    nasm = find_tool(
        [os.environ.get("NASM", "")] if os.environ.get("NASM") else ["nasm"],
        [r"C:\Program Files\NASM\nasm.exe"],
    )
    ver = subprocess.run([cc, "--version"], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True).stdout
    is_clang = "clang" in ver.lower()
    ld_help = subprocess.run([ld, "--help"], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True).stdout
    has_nowarn_rwx = "--no-warn-rwx-segments" in ld_help
    return {
        "cc": cc,
        "ld": ld,
        "ar": ar,
        "objcopy": objcopy,
        "nasm": nasm,
        "is_clang": is_clang,
        "has_nowarn_rwx": has_nowarn_rwx,
    }


def shell_join(cmd):
    """Render a command list as a copy-pasteable shell line."""
    try:
        return shlex.join([str(c) for c in cmd])
    except AttributeError:  # Python < 3.8
        return " ".join(str(c) for c in cmd)


def run_cmd(cmd, cwd=ROOT, env=None):
    p = subprocess.run(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True)
    if p.returncode != 0:
        raise RuntimeError("command failed (exit %d): %s%s%s" % (
            p.returncode, shell_join(cmd),
            "\n" if p.stdout.strip() else "", p.stdout))
    if p.stdout.strip():
        sys.stdout.write(p.stdout)


def parse_depfile(dep_path):
    if not os.path.isfile(dep_path):
        return []
    with open(dep_path, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    # Normalize line endings (clang writes CRLF on Windows) and unwrap
    # '\' line continuations so the whole first rule sits on one line.
    text = text.replace("\r\n", "\n").replace("\\\n", " ")
    # Keep only the first logical rule. With -MP the file then lists phony
    # targets ('kernel/include/foo.h:'); reading those as dependencies
    # would invent files that never exist and force a rebuild every time.
    first_rule = text.split("\n", 1)[0]
    # The target/dependency separator is the first ':' followed by
    # whitespace or EOL. This skips the drive colon in absolute Windows
    # targets like 'C:\repo\build\kernel/main.o: deps...'.
    m = re.search(r":(?=\s|$)", first_rule)
    if not m:
        return []
    rhs = first_rule[m.end():]
    deps = []
    cur = []
    i = 0
    while i < len(rhs):
        ch = rhs[i]
        if ch == "\\" and i + 1 < len(rhs) and rhs[i + 1] == " ":
            cur.append(" ")
            i += 2
            continue
        if ch in " \t\r\n":
            if cur:
                deps.append("".join(cur))
                cur = []
        else:
            cur.append(ch)
        i += 1
    if cur:
        deps.append("".join(cur))
    return deps


def needs_rebuild(out_path, inputs, dep_path=None):
    if not os.path.isfile(out_path):
        return True
    out_m = os.path.getmtime(out_path)
    all_inputs = list(inputs)
    if dep_path:
        all_inputs.extend(parse_depfile(dep_path))
    for p in all_inputs:
        if not os.path.exists(p) or os.path.getmtime(p) > out_m:
            return True
    return False


# Overridable via `build.py --jobs N` (set by main); default = all cores.
MAX_WORKERS = os.cpu_count() or 4


def compile_jobs(jobs):
    if not jobs:
        return
    workers = min(len(jobs), MAX_WORKERS)
    # Run every job to completion (like `make -k`) so one bad translation
    # unit does not hide failures in the others, then report them all.
    failed = []  # (job index, exception)
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as ex:
        futs = {ex.submit(run_cmd, cmd): i for i, cmd in enumerate(jobs)}
        for fut in concurrent.futures.as_completed(futs):
            try:
                fut.result()
            except Exception as exc:
                failed.append((futs[fut], exc))
    if failed:
        reports = []
        for i, exc in sorted(failed):
            # exc already carries the failing command line + captured output.
            reports.append("----- job %d/%d -----\n%s" % (i + 1, len(jobs), exc))
        raise RuntimeError("%d of %d build jobs failed:\n\n%s" % (
            len(failed), len(jobs), "\n\n".join(reports)))


def parse_config_mk(path):
    cfg = {"CONFIG_KERNEL_EXCLUDE": [], "CONFIG_APPS_EXCLUDE": []}
    with open(path, "r", encoding="utf-8") as f:
        lines = f.read().splitlines()
    i = 0
    while i < len(lines):
        line = lines[i].strip()
        while line.endswith("\\") and i + 1 < len(lines):
            i += 1
            line = line[:-1].rstrip() + " " + lines[i].strip()
        if line and not line.startswith("#") and ":=" in line:
            k, v = [x.strip() for x in line.split(":=", 1)]
            if k in ("CONFIG_KERNEL_EXCLUDE", "CONFIG_APPS_EXCLUDE"):
                cfg[k] = v.split()
            else:
                cfg[k] = v
        i += 1
    return cfg


def rel_glob(pattern):
    return sorted(p.replace("\\", "/") for p in glob.glob(os.path.join(ROOT, pattern)))


def r(rel):
    return os.path.join(ROOT, rel)


def b(rel):
    return os.path.join(BUILD, rel)


def build_all(tc):
    os.makedirs(BUILD, exist_ok=True)
    # 1. Config generation
    run_cmd([
        sys.executable, r("tools/mkconfig.py"),
        "--config", r("config"),
        "--header", r("kernel/include/config.h"),
        "--mk", b("config.mk"),
        "--quiet",
    ])
    cfg = parse_config_mk(b("config.mk"))

    target_flags = (["--target=x86_64-unknown-none-elf"] if tc["is_clang"] else []) + [
        "-march=x86-64", "-mtune=generic",
    ]
    kreg_flags = ["-mgeneral-regs-only"] if tc["is_clang"] else ["-mno-mmx", "-mno-sse", "-mno-sse2"]
    ldflags = ["-nostdlib", "-z", "max-page-size=0x1000"]
    if tc["has_nowarn_rwx"]:
        ldflags.append("--no-warn-rwx-segments")

    cflags = target_flags + [
        "-m64", "-ffreestanding", "-nostdlib", "-fno-stack-protector",
        "-fno-pic", "-fno-pie", "-fno-builtin", "-mno-red-zone",
    ] + kreg_flags + [
        "-mcmodel=kernel", "-fno-omit-frame-pointer", "-Wall", "-Wextra",
        "-O2", "-g", "-Ikernel/include", "-Iabi", "-MMD", "-MP",
    ]
    modcflags = target_flags + [
        "-m64", "-ffreestanding", "-nostdlib", "-fno-stack-protector",
        "-fno-pic", "-fno-pie", "-fno-builtin", "-mno-red-zone",
    ] + kreg_flags + [
        "-mcmodel=kernel", "-fno-common", "-fno-asynchronous-unwind-tables",
        "-Wall", "-Wextra", "-O2", "-Ikernel/include", "-Iabi", "-MMD", "-MP",
    ]
    ucflags = target_flags + [
        "-m64", "-ffreestanding", "-nostdlib", "-fno-stack-protector",
        "-fno-pic", "-fno-pie", "-fno-builtin", "-mno-red-zone",
        "-Wall", "-Wextra", "-O2", "-g",
        "-Ilibc/include", "-Iabi", "-Ilibgui",
        "-Ilibz", "-Ilibtls", "-Ilibimg", "-Ilibweb", "-Ilibjs", "-MMD", "-MP",
    ]
    dyn_cflags = target_flags + [
        "-m64", "-ffreestanding", "-nostdlib", "-fno-stack-protector",
        "-fno-builtin", "-mno-red-zone", "-Wall", "-Wextra", "-O2", "-g",
    ]

    jobs = []

    def queue_c(src_rel, obj_rel, flags):
        src = r(src_rel)
        obj = b(obj_rel)
        dep = os.path.splitext(obj)[0] + ".d"
        os.makedirs(os.path.dirname(obj), exist_ok=True)
        if needs_rebuild(obj, [src], dep):
            jobs.append([tc["cc"]] + flags + ["-c", src_rel, "-o", obj])
        return obj

    def queue_asm(src_rel, obj_rel):
        src = r(src_rel)
        obj = b(obj_rel)
        os.makedirs(os.path.dirname(obj), exist_ok=True)
        if needs_rebuild(obj, [src]):
            jobs.append([tc["nasm"], "-f", "elf64", src_rel, "-o", obj])
        return obj

    # Kernel objects
    k_excl = set(cfg["CONFIG_KERNEL_EXCLUDE"])
    k_csrcs = [os.path.relpath(p, ROOT).replace("\\", "/")
               for p in rel_glob("kernel/*.c")
               if os.path.relpath(p, ROOT).replace("\\", "/") not in k_excl]
    k_asrcs = [os.path.relpath(p, ROOT).replace("\\", "/")
               for p in rel_glob("kernel/*.asm")]
    k_entry = "kernel/entry.asm"
    k_asrcs_rest = [s for s in k_asrcs if s != k_entry]
    k_objs = [queue_asm(k_entry, "kernel/entry.o")]
    for s in k_asrcs_rest:
        base = os.path.splitext(os.path.basename(s))[0]
        k_objs.append(queue_asm(s, "kernel/%s.o" % base))
    for s in k_csrcs:
        base = os.path.splitext(os.path.basename(s))[0]
        k_objs.append(queue_c(s, "kernel/%s.o" % base, cflags))

    # Modules
    mod_bins = []
    for s in rel_glob("modules/*.c"):
        rel = os.path.relpath(s, ROOT).replace("\\", "/")
        name = os.path.splitext(os.path.basename(rel))[0]
        kmod = b("modules/%s.kmod" % name)
        dep = b("modules/%s.d" % name)
        os.makedirs(os.path.dirname(kmod), exist_ok=True)
        if needs_rebuild(kmod, [s], dep):
            jobs.append([tc["cc"]] + modcflags + ["-c", rel, "-o", kmod])
        mod_bins.append(kmod)

    # Libc
    crt0 = queue_asm("libc/crt0.asm", "libc/crt0.o")
    libc_objs = []
    for s in rel_glob("libc/*.asm"):
        rel = os.path.relpath(s, ROOT).replace("\\", "/")
        if rel == "libc/crt0.asm":
            continue
        base = os.path.splitext(os.path.basename(rel))[0]
        libc_objs.append(queue_asm(rel, "libc/%s.o" % base))
    for s in rel_glob("libc/*.c"):
        rel = os.path.relpath(s, ROOT).replace("\\", "/")
        base = os.path.splitext(os.path.basename(rel))[0]
        libc_objs.append(queue_c(rel, "libc/%s.o" % base, ucflags))

    # libgui + browser libs
    libgui_objs = []
    for s in rel_glob("libgui/*.c"):
        rel = os.path.relpath(s, ROOT).replace("\\", "/")
        base = os.path.splitext(os.path.basename(rel))[0]
        libgui_objs.append(queue_c(rel, "libgui/%s.o" % base, ucflags))

    browser_lib_objs = {}
    for libname in BROWSER_LIBS:
        objs = []
        for s in rel_glob("%s/*.c" % libname):
            rel = os.path.relpath(s, ROOT).replace("\\", "/")
            base = os.path.splitext(os.path.basename(rel))[0]
            extra = ["-DCSS_WITH_DOM"] if rel == "libweb/style.c" else []
            objs.append(queue_c(rel, "%s/%s.o" % (libname, base), ucflags + extra))
        browser_lib_objs[libname] = objs

    # Apps
    app_excl = set(cfg["CONFIG_APPS_EXCLUDE"])
    app_objs = {}
    for s in rel_glob("apps/*.c"):
        rel = os.path.relpath(s, ROOT).replace("\\", "/")
        name = os.path.splitext(os.path.basename(rel))[0]
        if name in app_excl:
            continue
        app_objs[name] = queue_c(rel, "apps/%s.o" % name, ucflags)

    # Dynamic fixture
    libgreet_o = queue_c("dynamic/libgreet.c", "dynamic/libgreet.o", dyn_cflags + ["-fPIC"])
    dynhello_o = queue_c("dynamic/dynhello.c", "dynamic/dynhello.o", dyn_cflags + ["-fPIE"])

    # Packages C sources
    pkg_names = sorted(
        os.path.basename(p) for p in glob.glob(r("packages/*")) if os.path.isdir(p)
    )
    pkg_prog_objs = {}
    for p in pkg_names:
        for s in rel_glob("packages/%s/src/*.c" % p):
            rel = os.path.relpath(s, ROOT).replace("\\", "/")
            prog = os.path.splitext(os.path.basename(rel))[0]
            pkg_prog_objs[(p, prog)] = queue_c(rel, "pkgobj/%s/%s.o" % (p, prog), ucflags)

    print("Compiling changed translation units (%d jobs)..." % len(jobs))
    compile_jobs(jobs)

    # Archive static libraries
    def make_archive(ar_path, objs):
        if needs_rebuild(ar_path, objs):
            if os.path.exists(ar_path):
                os.remove(ar_path)
            run_cmd([tc["ar"], "rcs", ar_path] + objs)

    libc_a = b("libc.a")
    libgui_a = b("libgui.a")
    make_archive(libc_a, libc_objs)
    make_archive(libgui_a, libgui_objs)
    lib_archives = {}
    for libname in BROWSER_LIBS:
        lib_a = b("%s.a" % libname)
        make_archive(lib_a, browser_lib_objs[libname])
        lib_archives[libname] = lib_a

    # Link kernel
    kernel_elf = b("kernel.elf")
    kernel_bin = b("kernel.bin")
    k_ld = r("kernel/linker.ld")
    if needs_rebuild(kernel_elf, k_objs + [k_ld]):
        run_cmd([tc["ld"]] + ldflags + ["-T", k_ld, "-o", kernel_elf] + k_objs)
    if needs_rebuild(kernel_bin, [kernel_elf]):
        run_cmd([tc["objcopy"], "-O", "binary", kernel_elf, kernel_bin])
    ksize = os.path.getsize(kernel_bin)
    print("kernel.bin: %d bytes (%.1f KiB / 992 KiB limit)" % (ksize, ksize / 1024.0))

    # Link dynamic fixture
    dyn_lib = b("lib/libgreet.so")
    dyn_app = b("apps/dynhello")
    os.makedirs(os.path.dirname(dyn_lib), exist_ok=True)
    os.makedirs(os.path.dirname(dyn_app), exist_ok=True)
    if needs_rebuild(dyn_lib, [libgreet_o]):
        run_cmd([tc["ld"], "-nostdlib", "-z", "max-page-size=0x1000",
                 "-shared", "--hash-style=sysv", "-soname", "libgreet.so",
                 "-o", dyn_lib, libgreet_o])
    if needs_rebuild(dyn_app, [dynhello_o, dyn_lib]):
        run_cmd([tc["ld"], "-nostdlib", "-z", "max-page-size=0x1000",
                 "-pie", "--hash-style=sysv", "-e", "_start",
                 "-L", b("lib"), "-o", dyn_app, dynhello_o, "-lgreet"])

    # Link apps
    user_ld = r("apps/user.ld")
    app_bins = [dyn_app]
    link_jobs = []
    for name, obj in sorted(app_objs.items()):
        out = b("apps/%s" % name)
        app_bins.append(out)
        if name == "browser":
            deps = [obj, crt0, libc_a, libgui_a, user_ld] + [lib_archives[l] for l in BROWSER_LIBS]
            cmd = [tc["ld"], "-nostdlib", "-z", "max-page-size=0x1000", "-T", user_ld, "-o", out,
                   crt0, obj, lib_archives["libweb"], lib_archives["libjs"],
                   lib_archives["libimg"], lib_archives["libtls"], lib_archives["libz"],
                   libgui_a, libc_a]
        elif name == "tlstest":
            deps = [obj, crt0, libc_a, libgui_a, lib_archives["libtls"], user_ld]
            cmd = [tc["ld"], "-nostdlib", "-z", "max-page-size=0x1000", "-T", user_ld, "-o", out,
                   crt0, obj, libgui_a, lib_archives["libtls"], libc_a]
        elif name == "kjs":
            deps = [obj, crt0, libc_a, libgui_a, lib_archives["libjs"], user_ld]
            cmd = [tc["ld"], "-nostdlib", "-z", "max-page-size=0x1000", "-T", user_ld, "-o", out,
                   crt0, obj, lib_archives["libjs"], libgui_a, libc_a]
        elif name in HTTP_TLS_APPS:
            deps = [obj, crt0, libc_a, libgui_a, lib_archives["libtls"], user_ld]
            cmd = [tc["ld"], "-nostdlib", "-z", "max-page-size=0x1000", "-T", user_ld, "-o", out,
                   crt0, obj, libgui_a, "--start-group", libc_a, lib_archives["libtls"], "--end-group"]
        else:
            deps = [obj, crt0, libc_a, libgui_a, user_ld]
            cmd = [tc["ld"], "-nostdlib", "-z", "max-page-size=0x1000", "-T", user_ld, "-o", out,
                   crt0, obj, libgui_a, libc_a]
        if needs_rebuild(out, deps):
            link_jobs.append(cmd)

    # Link package programs
    pkg_progs = []
    for (p, prog), obj in sorted(pkg_prog_objs.items()):
        out = b("pkg/%s/root/bin/%s" % (p, prog))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        pkg_progs.append(out)
        if needs_rebuild(out, [obj, crt0, libc_a, libgui_a, user_ld]):
            run_cmd([tc["ld"], "-nostdlib", "-z", "max-page-size=0x1000",
                     "-T", user_ld, "-o", out, crt0, obj, libgui_a, libc_a])
            run_cmd([tc["objcopy"], "--strip-debug", out])

    compile_jobs(link_jobs)

    # Build .kpkg & index.kpi
    os.makedirs(b("repo"), exist_ok=True)
    kpkgs = []
    for p in pkg_names:
        manifest = r("packages/%s/manifest" % p)
        kpkg = b("repo/%s.kpkg" % p)
        pkg_root_stage = b("pkg/%s/root" % p)
        os.makedirs(pkg_root_stage, exist_ok=True)
        src_root = r("packages/%s/root" % p)
        pkg_data = []
        if os.path.isdir(src_root):
            shutil.copytree(src_root, pkg_root_stage, dirs_exist_ok=True)
            for dr, _, fs in os.walk(src_root):
                for fn in fs:
                    pkg_data.append(os.path.join(dr, fn))
        deps = [manifest, r("tools/mkpkg.py")] + pkg_data + [
            o for o in pkg_progs if ("%s%s" % ("pkg/", p)) in o.replace("\\", "/")
        ]
        if needs_rebuild(kpkg, deps):
            run_cmd([sys.executable, r("tools/mkpkg.py"),
                     "--manifest", manifest, "--root", pkg_root_stage, "--out", kpkg])
        kpkgs.append(kpkg)

    index_kpi = b("repo/index.kpi")
    if needs_rebuild(index_kpi, kpkgs + [r("tools/mkrepo.py")]):
        run_cmd([sys.executable, r("tools/mkrepo.py"),
                 "--repo", b("repo"), "--out", index_kpi])

    # Stage rootfs & build fs.img
    rootfs_stage = b("rootfs")
    if os.path.exists(rootfs_stage):
        shutil.rmtree(rootfs_stage)
    for d in ("bin", "dev", "run", "tmp", "media", "var/log", "lib/modules",
              "var/pkg/repo", "var/pkg/db", "var/pkg/cache"):
        os.makedirs(os.path.join(rootfs_stage, d), exist_ok=True)
    if os.path.isdir(r("rootfs")):
        shutil.copytree(r("rootfs"), rootfs_stage, dirs_exist_ok=True)
    run_cmd([sys.executable, r("tools/mkaudio.py"),
             os.path.join(rootfs_stage, "doc", "tone.pcm")])
    run_cmd([sys.executable, r("tools/mkchime.py"),
             os.path.join(rootfs_stage, "media", "chime.wav")])
    for app in app_bins:
        dst = os.path.join(rootfs_stage, "bin", os.path.basename(app))
        shutil.copyfile(app, dst)
        run_cmd([tc["objcopy"], "--strip-debug", dst])
    dst_so = os.path.join(rootfs_stage, "lib", "libgreet.so")
    shutil.copyfile(dyn_lib, dst_so)
    run_cmd([tc["objcopy"], "--strip-debug", dst_so])
    for km in mod_bins:
        shutil.copyfile(km, os.path.join(rootfs_stage, "lib/modules", os.path.basename(km)))
    for kp in kpkgs + [index_kpi]:
        shutil.copyfile(kp, os.path.join(rootfs_stage, "var/pkg/repo", os.path.basename(kp)))

    fs_img = b("fs.img")
    run_cmd([
        sys.executable, r("tools/mkfs.py"),
        "--mode", "/etc/shadow:0600",
        "--mode", "/var/pkg/db:0700",
        "--mode", "/tmp:0777",
        "--mode", "/run:0755",
        rootfs_stage, fs_img, "64",
    ])

    # Stage1 & Stage2 + disk image
    stage1_bin = b("stage1.bin")
    stage2_bin = b("stage2.bin")
    if needs_rebuild(stage1_bin, [r("boot/stage1.asm")]):
        run_cmd([tc["nasm"], "-f", "bin", r("boot/stage1.asm"), "-o", stage1_bin])
    ksectors = (ksize + 511) // 512
    # Always rebuild stage2 if kernel.bin changed or stage2.asm changed
    if needs_rebuild(stage2_bin, [r("boot/stage2.asm"), kernel_bin]):
        run_cmd([tc["nasm"], "-f", "bin", "-DKERNEL_SECTORS=%d" % ksectors,
                 r("boot/stage2.asm"), "-o", stage2_bin])

    os_img = b("os.img")
    run_cmd([sys.executable, r("tools/mkimage.py"),
             stage1_bin, stage2_bin, kernel_bin, os_img, fs_img])


def run_host_tests(tc):
    out_dir = b("host-tests")
    os.makedirs(out_dir, exist_ok=True)
    exe_ext = ".exe" if sys.platform == "win32" else ""
    common = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", "-g"]
    if sys.platform == "win32":
        common += ["-D_CRT_SECURE_NO_WARNINGS", "-Wno-deprecated-declarations"]
    else:
        common += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]

    suites = [
        ("test_console_scroll", [r("tools/test_console_scroll.c")],
         ["-Ikernel/include", "-Iabi", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections"]),
        ("test_mission", [r("kernel/mission_core.c"), r("tools/test_mission.c")],
         ["-Ikernel/include", "-Iabi"]),
        ("test_mission_manifest", [r("libc/mission.c"), r("tools/test_mission_manifest.c")],
         ["-Iabi", "-idirafter", "libc/include"]),
        ("test_usb_hid", [r("kernel/usb_hid_core.c"), r("tools/test_usb_hid.c")],
         ["-Ikernel/include"]),
        ("test_virtio_ring", [r("kernel/virtio_ring.c"), r("kernel/virtio_net_core.c"), r("tools/test_virtio_ring.c")],
         ["-Ikernel/include"]),
        ("test_play", [r("tools/test_play.c")], []),
        ("test_random_core", [r("kernel/random_core.c"), r("tools/test_random_core.c")],
         ["-Ikernel/include"]),
        ("test_tcp_reassembly", [r("kernel/tcp_reassembly.c"), r("tools/test_tcp_reassembly.c")],
         ["-Ikernel/include"]),
        ("test_hangar", [r("kernel/hangar_fmt.c"), r("tools/test_hangar.c")],
         ["-Ikernel/include"]),
        ("test_tcp_rst", [r("kernel/tcp_reassembly.c"), r("kernel/net_checksum.c"), r("tools/test_tcp_rst.c")],
         ["-Ikernel/include", "-Iabi", "-ffunction-sections", "-fdata-sections",
          "-Wl,/opt:ref" if sys.platform == "win32" else "-Wl,--gc-sections"]),
        ("test_net_checksum", [r("kernel/net_checksum.c"), r("tools/test_net_checksum.c")],
         ["-Ikernel/include", "-Iabi"]),
        ("test_dns", [r("kernel/dns.c"), r("tools/test_dns.c")],
         ["-Ikernel/include", "-Iabi"]),
        ("test_gui_primitives", [r("tools/test_gui_primitives.c"), r("libgui/draw.c"), r("libgui/font_data.c")],
         ["-Iabi", "-Ilibgui", "-idirafter", "libc/include"]),
        ("test_cryptosym", [r("tools/test_cryptosym.c"), r("libtls/hash.c"), r("libtls/aead.c")],
         ["-Ilibtls"]),
        ("test_jpeg_prog", [
            r("tools/test_jpeg_prog.c"),
            r("libimg/img.c"), r("libimg/bmp.c"), r("libimg/gif.c"), r("libimg/jpeg.c"), r("libimg/png.c"),
            r("libimg/ico.c"), r("libimg/svg_img.c"), r("libimg/qoi_pnm.c"),
            r("libz/inflate.c"), r("libz/adler32.c"), r("libz/crc32.c"),
        ], ["-Ilibimg", "-Ilibz"]),
        ("test_img_formats", [
            r("tools/test_img_formats.c"),
            r("libimg/img.c"), r("libimg/bmp.c"), r("libimg/gif.c"), r("libimg/jpeg.c"), r("libimg/png.c"),
            r("libimg/ico.c"), r("libimg/svg_img.c"), r("libimg/qoi_pnm.c"),
            r("libz/inflate.c"), r("libz/adler32.c"), r("libz/crc32.c"),
        ], ["-Ilibimg", "-Ilibz"]),
        ("test_js", [r("tools/test_js.c")] + rel_glob("libjs/*.c"),
         ["-DJS_HOST", "-Ilibjs"]),
        ("test_dom", [r("tools/test_dom.c"), r("libweb/dom.c"), r("libweb/entities.c"), r("libweb/html.c")],
         ["-Ilibweb"]),
        ("test_css", [r("tools/test_css.c"), r("libweb/css.c"), r("libweb/style.c"), r("libweb/ua_style.c")],
         ["-Ilibweb", "-Ilibgui"]),
        ("test_font", [r("tools/test_font.c"), r("libgui/font.c"), r("libgui/font_data.c")],
         ["-Ilibgui", "-Iabi"]),
        ("test_layout", [
            r("tools/test_layout.c"), r("libweb/layout.c"), r("libweb/paint.c"),
            r("libweb/dom.c"), r("libweb/entities.c"), r("libweb/html.c"),
            r("libimg/img.c"), r("libimg/bmp.c"), r("libimg/gif.c"), r("libimg/jpeg.c"), r("libimg/png.c"),
            r("libimg/ico.c"), r("libimg/svg_img.c"), r("libimg/qoi_pnm.c"),
            r("libz/inflate.c"), r("libz/adler32.c"), r("libz/crc32.c"),
            r("libgui/font.c"), r("libgui/font_data.c"),
        ], ["-DPAINT_WITH_STDIO", "-Ilibweb", "-Ilibimg", "-Ilibz", "-Ilibgui", "-Iabi"]),
        ("test_jsdom", [
            r("tools/test_jsdom.c"), r("libweb/dom.c"), r("libweb/entities.c"),
            r("libweb/html.c"), r("libweb/css.c"), r("libweb/url.c"),
            r("libweb/storage.c"), r("libweb/jsdom.c"), r("libtls/hash.c"),
        ] + rel_glob("libjs/*.c"), ["-DJS_HOST", "-DCSS_WITH_DOM", "-Ilibweb", "-Ilibjs", "-Ilibtls"]),
        ("test_libc_string", [r("tools/test_libc_string.c")],
         ["-fno-builtin", "-Ilibc/include", "-Iabi"]),
    ]

    for name, srcs, flags in suites:
        exe = os.path.join(out_dir, name + exe_ext)
        print("=== Building & running %s ===" % name)
        run_cmd([tc["cc"]] + common + flags + srcs + ["-o", exe])
        run_cmd([exe])

    if os.path.isfile(b("fs.img")):
        print("=== Running kfsck.py ===")
        run_cmd([sys.executable, r("tools/kfsck.py"), b("fs.img")])
    print("=== Running test_kfs_journal.py ===")
    run_cmd([sys.executable, r("tools/test_kfs_journal.py")])
    print("=== Running test_kfs_readdir.py ===")
    run_cmd([sys.executable, r("tools/test_kfs_readdir.py"), "--cc", tc["cc"]])


def generate_compile_commands(tc):
    os.makedirs(BUILD, exist_ok=True)
    run_cmd([
        sys.executable, r("tools/mkconfig.py"),
        "--config", r("config"),
        "--header", r("kernel/include/config.h"),
        "--mk", b("config.mk"),
        "--quiet",
    ])
    cfg = parse_config_mk(b("config.mk"))

    target_flags = (["--target=x86_64-unknown-none-elf"] if tc["is_clang"] else []) + [
        "-march=x86-64", "-mtune=generic",
    ]
    kreg_flags = ["-mgeneral-regs-only"] if tc["is_clang"] else ["-mno-mmx", "-mno-sse", "-mno-sse2"]
    cflags = target_flags + [
        "-m64", "-ffreestanding", "-nostdlib", "-fno-stack-protector",
        "-fno-pic", "-fno-pie", "-fno-builtin", "-mno-red-zone",
    ] + kreg_flags + [
        "-mcmodel=kernel", "-fno-omit-frame-pointer", "-Wall", "-Wextra",
        "-O2", "-g", "-Ikernel/include", "-Iabi",
    ]
    modcflags = target_flags + [
        "-m64", "-ffreestanding", "-nostdlib", "-fno-stack-protector",
        "-fno-pic", "-fno-pie", "-fno-builtin", "-mno-red-zone",
    ] + kreg_flags + [
        "-mcmodel=kernel", "-fno-common", "-fno-asynchronous-unwind-tables",
        "-Wall", "-Wextra", "-O2", "-Ikernel/include", "-Iabi",
    ]
    ucflags = target_flags + [
        "-m64", "-ffreestanding", "-nostdlib", "-fno-stack-protector",
        "-fno-pic", "-fno-pie", "-fno-builtin", "-mno-red-zone",
        "-Wall", "-Wextra", "-O2", "-g",
        "-Ilibc/include", "-Iabi", "-Ilibgui",
        "-Ilibz", "-Ilibtls", "-Ilibimg", "-Ilibweb", "-Ilibjs",
    ]
    dyn_cflags = target_flags + [
        "-m64", "-ffreestanding", "-nostdlib", "-fno-stack-protector",
        "-fno-builtin", "-mno-red-zone", "-Wall", "-Wextra", "-O2", "-g",
    ]
    host_common = ["-std=c11", "-Wall", "-Wextra", "-O2", "-g"]
    if sys.platform == "win32":
        host_common += ["-D_CRT_SECURE_NO_WARNINGS", "-Wno-deprecated-declarations"]

    entries = []
    seen = set()

    def add_entry(src_rel, obj_rel, flags):
        norm_rel = src_rel.replace("\\", "/")
        if norm_rel in seen:
            return
        seen.add(norm_rel)
        abs_file = os.path.abspath(r(norm_rel))
        entries.append({
            "directory": ROOT,
            "arguments": [tc["cc"]] + flags + ["-c", norm_rel, "-o", b(obj_rel)],
            "file": abs_file,
        })

    k_excl = set(cfg["CONFIG_KERNEL_EXCLUDE"])
    for s in rel_glob("kernel/*.c"):
        rel = os.path.relpath(s, ROOT).replace("\\", "/")
        if rel not in k_excl:
            add_entry(rel, "kernel/%s.o" % os.path.splitext(os.path.basename(rel))[0], cflags)
    for s in rel_glob("modules/*.c"):
        rel = os.path.relpath(s, ROOT).replace("\\", "/")
        add_entry(rel, "modules/%s.kmod" % os.path.splitext(os.path.basename(rel))[0], modcflags)
    for s in rel_glob("libc/*.c"):
        rel = os.path.relpath(s, ROOT).replace("\\", "/")
        add_entry(rel, "libc/%s.o" % os.path.splitext(os.path.basename(rel))[0], ucflags)
    for s in rel_glob("libgui/*.c"):
        rel = os.path.relpath(s, ROOT).replace("\\", "/")
        add_entry(rel, "libgui/%s.o" % os.path.splitext(os.path.basename(rel))[0], ucflags)
    for libname in BROWSER_LIBS:
        for s in rel_glob("%s/*.c" % libname):
            rel = os.path.relpath(s, ROOT).replace("\\", "/")
            extra = ["-DCSS_WITH_DOM"] if rel == "libweb/style.c" else []
            add_entry(rel, "%s/%s.o" % (libname, os.path.splitext(os.path.basename(rel))[0]), ucflags + extra)
    app_excl = set(cfg["CONFIG_APPS_EXCLUDE"])
    for s in rel_glob("apps/*.c"):
        rel = os.path.relpath(s, ROOT).replace("\\", "/")
        name = os.path.splitext(os.path.basename(rel))[0]
        if name not in app_excl:
            add_entry(rel, "apps/%s.o" % name, ucflags)
    add_entry("dynamic/libgreet.c", "dynamic/libgreet.o", dyn_cflags + ["-fPIC"])
    add_entry("dynamic/dynhello.c", "dynamic/dynhello.o", dyn_cflags + ["-fPIE"])
    for pkg in sorted(os.path.basename(p) for p in glob.glob(r("packages/*")) if os.path.isdir(p)):
        for s in rel_glob("packages/%s/src/*.c" % pkg):
            rel = os.path.relpath(s, ROOT).replace("\\", "/")
            prog = os.path.splitext(os.path.basename(rel))[0]
            add_entry(rel, "pkgobj/%s/%s.o" % (pkg, prog), ucflags)

    host_suites = [
        ("tools/test_console_scroll.c", ["-Ikernel/include", "-Iabi", "-ffunction-sections", "-fdata-sections"]),
        ("tools/test_mission.c", ["-Ikernel/include", "-Iabi"]),
        ("tools/test_mission_manifest.c", ["-Iabi", "-idirafter", "libc/include"]),
        ("tools/test_usb_hid.c", ["-Ikernel/include"]),
        ("tools/test_virtio_ring.c", ["-Ikernel/include"]),
        ("tools/test_random_core.c", ["-Ikernel/include"]),
        ("tools/test_tcp_reassembly.c", ["-Ikernel/include"]),
        ("tools/test_hangar.c", ["-Ikernel/include"]),
        ("tools/test_tcp_rst.c", ["-Ikernel/include", "-Iabi"]),
        ("tools/test_net_checksum.c", ["-Ikernel/include", "-Iabi"]),
        ("tools/test_dns.c", ["-Ikernel/include", "-Iabi"]),
        ("tools/test_gui_primitives.c", ["-Iabi", "-Ilibgui", "-idirafter", "libc/include"]),
        ("tools/test_cryptosym.c", ["-Ilibtls"]),
        ("tools/test_jpeg_prog.c", ["-Ilibimg", "-Ilibz"]),
        ("tools/test_img_formats.c", ["-Ilibimg", "-Ilibz"]),
        ("tools/test_js.c", ["-DJS_HOST", "-Ilibjs"]),
        ("tools/test_dom.c", ["-Ilibweb"]),
        ("tools/test_css.c", ["-DCSS_WITH_DOM", "-Ilibweb", "-Ilibgui"]),
        ("tools/test_font.c", ["-Ilibgui", "-Iabi"]),
        ("tools/test_layout.c", ["-DPAINT_WITH_STDIO", "-Ilibweb", "-Ilibimg", "-Ilibz", "-Ilibgui", "-Iabi"]),
        ("tools/test_jsdom.c", ["-DJS_HOST", "-DCSS_WITH_DOM", "-Ilibweb", "-Ilibjs", "-Ilibtls"]),
        ("tools/test_browser_stack.c", ["-DJS_HOST", "-DCSS_WITH_DOM", "-Itools/browser-host", "-Ilibgui", "-Ilibz", "-Ilibtls", "-Ilibimg", "-Ilibweb", "-Ilibjs"]),
        ("tools/test_libc_string.c", ["-fno-builtin", "-Ilibc/include", "-Iabi"]),
    ]
    for rel, extra in host_suites:
        if os.path.isfile(r(rel)):
            add_entry(rel, "host-tests/%s.o" % os.path.splitext(os.path.basename(rel))[0], host_common + extra)

    out_path = r("compile_commands.json")
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(entries, f, indent=2)
    print("compile_commands.json: %d translation units -> %s" % (len(entries), out_path))


def main():
    ap = argparse.ArgumentParser(description="KestrelOS cross-platform build runner")
    ap.add_argument("action", nargs="?", default="build",
                    choices=["build", "clean", "debug", "gdb", "profile", "test-host", "fsck", "e2e", "smoke", "compdb", "all"])
    ap.add_argument("--profile-flags", default=os.environ.get("PROFILE_FLAGS", ""),
                    help="extra profiler arguments (default: PROFILE_FLAGS)")
    ap.add_argument("--qemu-flags", default=os.environ.get("QEMU_FLAGS", ""),
                    help="extra QEMU arguments for debug (default: QEMU_FLAGS)")
    ap.add_argument("--gdb-flags", default=os.environ.get("GDB_FLAGS", ""),
                    help="extra GDB arguments, e.g. batch commands (default: GDB_FLAGS)")
    ap.add_argument("--nic", default="rtl8139")
    ap.add_argument("--cpus", type=int, default=2)
    ap.add_argument("-j", "--jobs", type=int, default=0, metavar="N",
                    help="parallel compile/link workers (default: all cores)")
    args = ap.parse_args()

    if args.jobs > 0:
        global MAX_WORKERS
        MAX_WORKERS = args.jobs

    if args.action == "gdb":
        gdb = find_tool([os.environ.get("GDB", "gdb")])
        return subprocess.call([gdb, "-nx", "-x", r("tools/.gdbinit")]
                               + shlex.split(args.gdb_flags), cwd=ROOT)

    if args.action == "clean":
        if os.path.exists(BUILD):
            shutil.rmtree(BUILD)
        print("Cleaned build/")
        return 0

    tc = detect_toolchain()
    if args.action in ("build", "debug", "profile", "compdb", "all"):
        generate_compile_commands(tc)
    if args.action in ("build", "debug", "profile", "all"):
        build_all(tc)
    if args.action == "profile":
        return subprocess.call([sys.executable, r("tools/profile.py")]
                               + shlex.split(args.profile_flags), cwd=ROOT)
    if args.action == "debug":
        qemu = find_tool([os.environ.get("QEMU", "qemu-system-x86_64")])
        return subprocess.call([
            qemu, "-drive", "file=%s,format=raw" % b("os.img"),
            "-no-reboot", "-smp", "2", "-device", "rtl8139,netdev=n0",
            "-netdev", "user,id=n0", "-display", "none", "-serial", "stdio",
            "-s", "-S",
        ] + shlex.split(args.qemu_flags), cwd=ROOT)
    if args.action in ("test-host", "all"):
        run_host_tests(tc)
    if args.action == "fsck":
        run_cmd([sys.executable, r("tools/kfsck.py"), "-l", b("fs.img")])
    if args.action == "smoke":
        run_cmd([sys.executable, r("tools/e2e.py"), "--smoke",
                 "--nic", args.nic, "--cpus", str(args.cpus)])
    if args.action in ("e2e", "all"):
        run_cmd([sys.executable, r("tools/e2e.py"),
                 "--nic", args.nic, "--cpus", str(args.cpus)])
        run_cmd([sys.executable, r("tools/test_kfs_boot_recovery.py")])
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except RuntimeError as exc:
        # Build/tooling errors are expected failures: report them without a
        # Python traceback so the actual compiler output is what stands out.
        print("build.py: error: %s" % exc, file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("build.py: interrupted", file=sys.stderr)
        sys.exit(130)
