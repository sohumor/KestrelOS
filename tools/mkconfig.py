#!/usr/bin/env python3
"""Turn the repo-root `config` file into the build's configuration inputs.

`config` is a line-oriented KEY=y|m|n list (a stripped-down Kconfig, see
docs/config.md). This script validates it and writes two files:

  kernel/include/config.h   #define CONFIG_X 1         for y
                            #define CONFIG_X_MODULE 1  for m
                            nothing at all             for n

  build/config.mk           make variables listing the kernel objects and
                            the apps the build must leave out, so the
                            Makefile can `-include` it and filter its
                            wildcards.

Validation is the point of having a schema at all: an unknown key, a value
that is not y/m/n, a module value on an option with no module form, a
duplicated key and a dependency violation are all errors carrying the line
number of the offending line, and nothing is written when any of them
fires. A half-generated config.h that silently drops a driver is a much
worse failure than a build that stops.

Usage:
    python3 tools/mkconfig.py               # regenerate both outputs
    python3 tools/mkconfig.py --check       # validate only, write nothing
    python3 tools/mkconfig.py --list        # print the option table
    python3 tools/mkconfig.py --config c --header h.h --mk c.mk

Python 3 stdlib only.
"""

import argparse
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DEFAULT_CONFIG = os.path.join(REPO, "config")
DEFAULT_HEADER = os.path.join(REPO, "kernel", "include", "config.h")
DEFAULT_MK = os.path.join(REPO, "build", "config.mk")

VALUES = ("y", "m", "n")


class Option:
    """One configurable subsystem.

    name      CONFIG_* key as it appears in `config`
    default   value used (with a warning) when `config` does not mention it
    kind      "bool" for y/n only, "tristate" when m is meaningful
    kernel    kernel sources this option owns. They are compiled into the
              kernel for y, built as a module for m, and dropped for n.
    apps      apps/<name>.c programs that are pointless without it. Dropped
              from the image for n.
    requires  other options that must not be n when this one is enabled.
    help      one-line description, reused by --list and docs/config.md.
    """

    def __init__(self, name, default, kind="bool", kernel=(), apps=(),
                 requires=(), help=""):
        self.name = name
        self.default = default
        self.kind = kind
        self.kernel = list(kernel)
        self.apps = list(apps)
        self.requires = list(requires)
        self.help = help

    @property
    def tristate(self):
        return self.kind == "tristate"


# The schema. Order is the order the options are reported in; it follows
# the grouping in `config` itself.
OPTIONS = [
    Option("CONFIG_MODULES", "y",
           kernel=["kernel/module.c", "kernel/ksyms.c"],
           apps=["insmod", "rmmod", "lsmod"],
           help="Loadable kernel modules: export table, ET_REL loader, "
                "insmod/rmmod/lsmod."),
    Option("CONFIG_KLOG", "y",
           kernel=["kernel/klog.c"],
           apps=["dmesg"],
           help="Kernel log ring behind klog_printf, dmesg and /dev/klog."),
    Option("CONFIG_DEVFS", "y",
           kernel=["kernel/devfs.c"],
           requires=["CONFIG_KLOG"],
           help="/dev: null, zero, full, console, random, klog."),
    Option("CONFIG_KMON", "y",
           kernel=["kernel/kmon.c"],
           help="In-kernel rescue console for a machine that cannot start "
                "/bin/init."),

    Option("CONFIG_NET_RTL8139", "y", kind="tristate",
           kernel=["kernel/rtl8139.c"],
           help="RealTek RTL8139 ethernet driver (QEMU's default NIC)."),
    Option("CONFIG_NET_E1000", "y", kind="tristate",
           kernel=["kernel/e1000.c"],
           help="Intel 82540EM (e1000) ethernet driver (VirtualBox's "
                "default NIC)."),
    Option("CONFIG_TCP", "y",
           kernel=["kernel/tcp.c"],
           apps=["curl", "wget", "telnet"],
           help="TCP, and the userspace that needs it: curl, wget, telnet, "
                "http:// in the browser and kpkg."),
    Option("CONFIG_UDP", "y",
           kernel=["kernel/udp.c"],
           apps=["udp"],
           help="UDP datagram sockets and the `udp` tool."),
    Option("CONFIG_DNS", "y",
           kernel=["kernel/dns.c"],
           apps=["nslookup"],
           requires=["CONFIG_UDP"],
           help="DNS resolver and `nslookup`."),

    Option("CONFIG_GRAPHICS", "y",
           kernel=["kernel/fb.c", "kernel/font.c"],
           help="Linear framebuffer and the graphical console backend."),
    Option("CONFIG_WM", "y",
           kernel=["kernel/wm.c"],
           apps=["about", "browser", "desktop", "gcalc", "gclock",
                 "gfiles", "gpaint", "settings", "sysmon", "terminal"],
           requires=["CONFIG_GRAPHICS"],
           help="Compositor, window manager and the GUI apps."),
    Option("CONFIG_MOUSE", "y",
           kernel=["kernel/mouse.c"],
           help="PS/2 mouse driver."),

    Option("CONFIG_PACKAGES", "y",
           help="Stage the .kpkg repository into the disk image so "
                "`kpkg install` works offline."),
    Option("CONFIG_APPS_GAMES", "y",
           apps=["snake"],
           help="Games: apps/snake.c."),
]

BY_NAME = dict((o.name, o) for o in OPTIONS)


# --- reading -----------------------------------------------------------


def parse(path):
    """Read `path` into ({name: value}, {name: lineno}, [(lineno, msg)]).

    Syntax errors are collected rather than raised so that one run reports
    every bad line instead of only the first.
    """
    values = {}
    lines = {}
    errors = []

    try:
        with open(path, "r") as f:
            text = f.read()
    except IOError as e:
        return {}, {}, [(0, "%s: cannot read: %s" % (path, e))]

    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if "=" not in line:
            errors.append((lineno, "%s:%d: not a KEY=VALUE line: %s"
                           % (path, lineno, raw.strip())))
            continue
        key, val = line.split("=", 1)
        key = key.strip()
        val = val.strip()
        if key in values:
            errors.append((lineno, "%s:%d: %s set again (first set on "
                                   "line %d)" % (path, lineno, key,
                                                 lines[key])))
            continue
        values[key] = val
        lines[key] = lineno

    return values, lines, errors


def validate(path, values, lines):
    """Check the parsed file against the schema.

    Returns (resolved, errors, warnings). `resolved` maps every option name
    to a value in VALUES and is only meaningful when errors is empty.
    """
    errors = []
    warnings = []
    resolved = {}

    def at(name):
        return lines.get(name, 0)

    def where(name):
        return "%s:%d" % (path, lines[name]) if name in lines else path

    for key in values:
        if key not in BY_NAME:
            errors.append((lines[key], "%s:%d: unknown option %s"
                           % (path, lines[key], key)))

    for opt in OPTIONS:
        if opt.name not in values:
            warnings.append("%s: %s is not set, defaulting to %s"
                            % (path, opt.name, opt.default))
            resolved[opt.name] = opt.default
            continue
        val = values[opt.name]
        if val not in VALUES:
            errors.append((at(opt.name), "%s: %s=%s: expected y, m or n"
                           % (where(opt.name), opt.name, val)))
            continue
        if val == "m" and not opt.tristate:
            errors.append((at(opt.name),
                           "%s: %s=m: this option has no module form, "
                           "use y or n" % (where(opt.name), opt.name)))
            continue
        resolved[opt.name] = val

    if errors:
        return resolved, errors, warnings

    # Dependencies. An option is "enabled" at y or m; its dependencies must
    # then be built in, because a module cannot pull a subsystem that was
    # compiled out back into existence.
    for opt in OPTIONS:
        val = resolved[opt.name]
        if val == "n":
            continue
        for dep in opt.requires:
            if resolved.get(dep, "n") != "y":
                errors.append((at(opt.name),
                               "%s: %s=%s requires %s=y (it is %s)"
                               % (where(opt.name), opt.name, val, dep,
                                  resolved.get(dep, "n"))))
        if val == "m" and resolved.get("CONFIG_MODULES") != "y":
            errors.append((at(opt.name),
                           "%s: %s=m requires CONFIG_MODULES=y (it is %s)"
                           % (where(opt.name), opt.name,
                              resolved.get("CONFIG_MODULES", "n"))))

    return resolved, errors, warnings


# --- writing -----------------------------------------------------------


def header_text(resolved):
    out = []
    out.append("/* kernel/include/config.h - GENERATED by tools/mkconfig.py")
    out.append(" * from the repo-root `config` file. Do not edit: run")
    out.append(" * `python3 tools/mkconfig.py` instead.")
    out.append(" *")
    out.append(" * y -> #define CONFIG_X 1")
    out.append(" * m -> #define CONFIG_X_MODULE 1")
    out.append(" * n -> no #define at all, so #ifdef CONFIG_X is the test")
    out.append(" *      for \"present in the kernel image\" and")
    out.append(" *      #if defined(CONFIG_X) || defined(CONFIG_X_MODULE)")
    out.append(" *      is the test for \"present at all\".")
    out.append(" */")
    out.append("")
    out.append("#ifndef KESTREL_CONFIG_H")
    out.append("#define KESTREL_CONFIG_H")
    out.append("")
    for opt in OPTIONS:
        val = resolved[opt.name]
        out.append("/* %s */" % opt.help)
        if val == "y":
            out.append("#define %s 1" % opt.name)
        elif val == "m":
            out.append("#define %s_MODULE 1" % opt.name)
        else:
            out.append("/* %s is not set */" % opt.name)
        out.append("")
    out.append("#endif /* KESTREL_CONFIG_H */")
    return "\n".join(out) + "\n"


def mk_list(name, items, comment):
    return ["# %s" % comment,
            "%s :=%s" % (name, "".join(" " + i for i in items)),
            ""]


def mk_text(resolved):
    """The make fragment. Everything is a plain := list so the Makefile can
    $(filter-out ...) its wildcards without any GNU-make trickery."""
    kernel_drop = []      # compiled out of the kernel image (n and m)
    kernel_mod = []       # built as loadable modules (m)
    apps_drop = []

    for opt in OPTIONS:
        val = resolved[opt.name]
        if val != "y":
            kernel_drop.extend(opt.kernel)
        if val == "m":
            kernel_mod.extend(opt.kernel)
        if val == "n":
            apps_drop.extend(opt.apps)

    out = []
    out.append("# build/config.mk - GENERATED by tools/mkconfig.py from the")
    out.append("# repo-root `config` file. Do not edit.")
    out.append("#")
    out.append("# The Makefile includes this and filters its wildcards with")
    out.append("# it, so an option set to n really does stop being compiled.")
    out.append("")
    out.append("CONFIG_MK_GENERATED := 1")
    out.append("")
    out.extend(mk_list(
        "CONFIG_KERNEL_EXCLUDE", sorted(set(kernel_drop)),
        "kernel sources that must NOT be linked into kernel.elf"))
    out.extend(mk_list(
        "CONFIG_KERNEL_MODULES", sorted(set(kernel_mod)),
        "kernel sources built as loadable .kmod modules instead"))
    out.extend(mk_list(
        "CONFIG_APPS_EXCLUDE", sorted(set(apps_drop)),
        "apps/<name>.c programs that must NOT be built or staged"))

    out.append("# Every option, so a rule can test one directly:")
    out.append("#   ifeq ($(CONFIG_PACKAGES),y)")
    for opt in OPTIONS:
        out.append("%s := %s" % (opt.name, resolved[opt.name]))
    out.append("")
    return "\n".join(out) + "\n"


def write_if_changed(path, text):
    """Leave the file's mtime alone when nothing changed, so regenerating
    the configuration does not force a full kernel rebuild."""
    try:
        with open(path, "r") as f:
            if f.read() == text:
                return False
    except IOError:
        pass
    directory = os.path.dirname(path)
    if directory and not os.path.isdir(directory):
        os.makedirs(directory)
    with open(path, "w") as f:
        f.write(text)
    return True


# --- main --------------------------------------------------------------


def list_options():
    for opt in OPTIONS:
        form = "y/m/n" if opt.tristate else "y/n"
        print("%-22s %-6s default=%s" % (opt.name, form, opt.default))
        print("    %s" % opt.help)
        if opt.requires:
            print("    requires: %s" % ", ".join(opt.requires))
        if opt.kernel:
            print("    kernel:   %s" % " ".join(opt.kernel))
        if opt.apps:
            print("    apps:     %s" % " ".join(opt.apps))


def main():
    ap = argparse.ArgumentParser(
        description="generate kernel/include/config.h and build/config.mk")
    ap.add_argument("--config", default=DEFAULT_CONFIG,
                    help="input config file (default: %(default)s)")
    ap.add_argument("--header", default=DEFAULT_HEADER,
                    help="generated C header (default: %(default)s)")
    ap.add_argument("--mk", default=DEFAULT_MK,
                    help="generated make fragment (default: %(default)s)")
    ap.add_argument("--check", action="store_true",
                    help="validate only; write nothing")
    ap.add_argument("--list", action="store_true",
                    help="print the option table and exit")
    ap.add_argument("--quiet", action="store_true",
                    help="say nothing unless something changed or failed")
    args = ap.parse_args()

    if args.list:
        list_options()
        return 0

    values, lines, errors = parse(args.config)
    resolved, verrors, warnings = validate(args.config, values, lines)
    # Report in file order: a config file is read top to bottom, and so
    # should the complaints about it be.
    errors = sorted(errors + verrors, key=lambda e: e[0])

    for w in warnings:
        sys.stderr.write("mkconfig: warning: %s\n" % w)
    if errors:
        for _, e in errors:
            sys.stderr.write("mkconfig: error: %s\n" % e)
        sys.stderr.write("mkconfig: %d error(s); nothing written\n"
                         % len(errors))
        return 1

    if args.check:
        if not args.quiet:
            print("mkconfig: %s is valid (%d options)"
                  % (args.config, len(OPTIONS)))
        return 0

    changed = []
    if write_if_changed(args.header, header_text(resolved)):
        changed.append(args.header)
    if write_if_changed(args.mk, mk_text(resolved)):
        changed.append(args.mk)

    if changed:
        print("mkconfig: wrote %s" % ", ".join(changed))
    elif not args.quiet:
        print("mkconfig: %s and %s are up to date"
              % (args.header, args.mk))
    return 0


if __name__ == "__main__":
    sys.exit(main())
