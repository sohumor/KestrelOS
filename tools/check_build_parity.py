#!/usr/bin/env python3
"""Check GNU make's expanded rules against build.py's recorded build commands.

Requires GNU make and the normal Linux build toolchain, but executes no compiler
or linker. Uses the active config. Command aliases below intentionally describe
the asymmetric CLIs: build.py groups host tests and exposes compdb; make offers
individual tests, VM helpers and configuration helpers. Host test *recipes*,
filesystem staging/copy operations, boot-sector size expressions and incremental
dependency tracking are not compared. Native compile, ELF assembly, archive and
link commands are compared including ordered flags, inputs and output paths.
Archive member order alone is ignored (the two runners enumerate libc's C/ASM
members differently); link order is never ignored.
"""

import argparse
import contextlib
import difflib
import io
import shlex
import subprocess
import sys
import tempfile
from unittest.mock import patch

import build


# Every public command must be accounted for, even where no direct alias exists.
COMMANDS = {
    "build": {"all"},
    "clean": {"clean"},
    "debug": {"debug"},
    "gdb": {"gdb"},
    "test-host": {"test-tcp", "test-checksum", "test-net", "test-random",
                  "test-jpeg-prog", "test-img-formats", "test-browser-host",
                  "test-gui-host", "test-kfs-journal", "test-kfs-readdir"},
    "fsck": {"fsck"},
    "e2e": {"test", "test-e1000", "test-kfs-boot-recovery", "test-kfs"},
    "smoke": {"smoke", "smoke-e1000"},
    "compdb": set(),  # Python-only clangd integration.
    "all": set(),  # Python-only build + host tests + E2E aggregate.
}
MAKE_ONLY = {"run", "run-headless", "screenshot", "vm-images", "help",
             "reconfig", "checkconfig"}


def command_output(args):
    return subprocess.check_output(args, cwd=build.ROOT, text=True,
                                   stderr=subprocess.STDOUT)


def compare(label, left, right):
    if left == right:
        return True
    print("Build parity drift: " + label)
    print("\n".join(difflib.unified_diff(
        sorted(left), sorted(right), fromfile="Makefile", tofile="tools/build.py",
        lineterm="")))
    return False


def python_actions():
    # Inspect the real argparse choices without running dispatch/tool detection.
    class Parsed(Exception):
        pass

    choices = set()

    def capture(parser, *args, **kwargs):
        for action in parser._actions:
            if action.dest == "action":
                choices.update(action.choices)
        raise Parsed

    with patch.object(argparse.ArgumentParser, "parse_args", capture):
        try:
            build.main()
        except Parsed:
            pass
    return choices


def native_commands(commands, out_dir):
    result = set()
    for cmd in commands:
        cmd = [str(arg).replace(out_dir + "/", "build/")
               .replace(build.ROOT + "/", "") for arg in cmd]
        if not cmd or cmd[0] not in {"gcc", "ld", "ar", "nasm"}:
            continue
        if cmd[0] == "nasm" and "elf64" not in cmd:
            continue
        # -Lpath and -L path are equivalent.
        if "-L" in cmd:
            i = cmd.index("-L")
            cmd[i:i + 2] = ["-L" + cmd[i + 1]]
        if cmd[:2] == ["ar", "rcs"]:
            cmd[3:] = sorted(cmd[3:])
        result.add(shlex.join(cmd))
    return result


def main():
    with tempfile.TemporaryDirectory(prefix="kestrel-parity-") as out_dir:
        # Use a separate output tree, independent of existing incremental builds.
        command_output([sys.executable, "tools/mkconfig.py", "--config", "config",
                        "--header", out_dir + "/config.h", "--mk",
                        out_dir + "/config.mk", "--quiet"])
        make_args = ["make", "--no-print-directory", "BUILD=" + out_dir,
                     "CC=gcc", "LD=ld", "AR=ar", "NASM=nasm", "OBJCOPY=objcopy"]
        database = command_output(make_args + ["-np", "all"])
        phony = set()
        for line in database.splitlines():
            if line.startswith(".PHONY:"):
                phony.update(line.split()[1:])
        ok = compare("public Make targets", phony,
                     set().union(*COMMANDS.values(), MAKE_ONLY))
        ok &= compare("Python subcommands", set(COMMANDS), python_actions())
        dry_run = command_output(make_args + ["-Bn", "all"])
        make_commands = [shlex.split(line) for line in dry_run.replace("\\\n", " ").splitlines()
                         if line.startswith(("gcc ", "ld ", "ar ", "nasm "))]
        recorded = []
        tc = {"cc": "gcc", "ld": "ld", "ar": "ar", "nasm": "nasm",
              "objcopy": "objcopy", "is_clang": False,
              "has_nowarn_rwx": "--no-warn-rwx-segments" in command_output(["ld", "--help"])}
        # Execute Python's real rule construction, replacing only effects/probes.
        with contextlib.ExitStack() as stack:
            stack.enter_context(patch.object(build, "BUILD", out_dir))
            stack.enter_context(patch.object(build, "run_cmd", lambda cmd, **kw: recorded.append(cmd)))
            stack.enter_context(patch.object(build, "compile_jobs", lambda jobs: recorded.extend(jobs)))
            stack.enter_context(patch.object(build, "needs_rebuild", return_value=True))
            stack.enter_context(patch.object(build.os.path, "getsize", return_value=512))
            for name in ("copyfile", "copytree", "rmtree"):
                stack.enter_context(patch.object(build.shutil, name))
            with contextlib.redirect_stdout(io.StringIO()):
                build.build_all(tc)
        left = native_commands(make_commands, out_dir)
        right = native_commands(recorded, out_dir)
        if not left or not right:
            raise RuntimeError("No native build commands captured")
        ok &= compare("native build commands", left, right)
        if ok:
            print("Build parity OK: %d Make targets, %d Python subcommands, %d native commands"
                  % (len(phony), len(COMMANDS), len(left)))
        return 0 if ok else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print("Build parity check failed: %s" % exc, file=sys.stderr)
        sys.exit(1)
