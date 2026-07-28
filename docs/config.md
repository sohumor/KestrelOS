# Build configuration

KestrelOS is configured by a single file at the repo root called `config`.
It is a line-oriented `KEY=y|m|n` list — a stripped-down Kconfig — and it is
the mechanism section 5 of [MODULARITY.md](MODULARITY.md) calls for: if a
driver can be set to `n` and the kernel still builds and boots, its coupling
is genuinely gone. Modularity that cannot be checked is just an assertion.

```
$ python3 tools/mkconfig.py --check      # validate, write nothing
$ python3 tools/mkconfig.py              # regenerate both outputs
$ python3 tools/mkconfig.py --list       # what every option controls
```

The build runs `mkconfig.py` for you; you only need to run it by hand when
you want to see the errors without waiting for a compile.

## The file format

```
# comments start with '#', at the start of a line or after a value
CONFIG_NET_RTL8139=y
CONFIG_NET_E1000=m
CONFIG_APPS_GAMES=n
```

Whitespace around the key and the value is ignored. Blank lines are ignored.
The three values are:

| value | meaning |
| ----- | ------- |
| `y`   | build the subsystem into the kernel image (or ship the component) |
| `m`   | build it as a loadable kernel module, to be loaded with `insmod` |
| `n`   | leave it out entirely |

Not every option has a module form. An option that is not a driver, or that
has to exist before the module loader does, is `y/n` only and setting it to
`m` is an error rather than a silent downgrade.

## What it generates

`tools/mkconfig.py` writes two files, and nothing else in the tree needs to
know that `config` exists:

**`kernel/include/config.h`** — checked in, because a header the compiler
needs should never be missing from a fresh clone:

```c
#define CONFIG_NET_RTL8139 1        /* from CONFIG_NET_RTL8139=y */
#define CONFIG_NET_E1000_MODULE 1   /* from CONFIG_NET_E1000=m   */
/* CONFIG_APPS_GAMES is not set */  /* from CONFIG_APPS_GAMES=n  */
```

So `#ifdef CONFIG_X` means "compiled into the kernel image" and
`#if defined(CONFIG_X) || defined(CONFIG_X_MODULE)` means "present at all,
one way or the other". That is the distinction driver code cares about: a
NIC driver built as a module must still compile, it just must not be called
by name from `kmain()`.

**`build/config.mk`** — a make fragment, regenerated whenever `config`
changes:

```make
CONFIG_KERNEL_EXCLUDE := kernel/wm.c
CONFIG_KERNEL_MODULES := kernel/e1000.c
CONFIG_APPS_EXCLUDE   := desktop gclock gfiles gpaint snake terminal
CONFIG_NET_RTL8139 := y
...
```

The Makefile includes it and filters its wildcards with it, so a source
that is switched off is not compiled, not linked and not staged into the
image. `CONFIG_KERNEL_MODULES` lists the sources that move out of
`kernel.elf` and into `.kmod` files instead.

## Validation

Nothing is written unless the whole file is valid, and every complaint
carries the line number of the line that caused it:

```
$ python3 tools/mkconfig.py --check
mkconfig: error: config:3: unknown option CONFIG_FOO
mkconfig: error: config:4: CONFIG_MOUSE=x: expected y, m or n
mkconfig: error: config:5: CONFIG_KMON=m: this option has no module form, use y or n
mkconfig: error: config:6: CONFIG_MOUSE set again (first set on line 4)
mkconfig: error: config:7: not a KEY=VALUE line: broken line
mkconfig: 5 error(s); nothing written
```

Dependency violations are reported the same way, against the option that
asked for something that is not there:

```
mkconfig: error: config:1: CONFIG_WM=y requires CONFIG_GRAPHICS=y (it is n)
mkconfig: error: config:3: CONFIG_NET_E1000=m requires CONFIG_MODULES=y (it is n)
mkconfig: error: config:5: CONFIG_DNS=y requires CONFIG_UDP=y (it is n)
```

A half-generated `config.h` that quietly drops a driver is a far worse
failure than a build that stops, so the script writes nothing at all when
any error fires. An option the file does not mention is a warning, not an
error — it takes its schema default — so that adding an option to
`mkconfig.py` does not break every existing `config` in one commit.

Outputs are written only when their content changes, so regenerating the
configuration does not force a full kernel rebuild.

## The options

### Core infrastructure

| option | form | what it controls |
| ------ | ---- | ---------------- |
| `CONFIG_MODULES` | y/n | The exported symbol table, the `ET_REL` relocating loader and `insmod`/`rmmod`/`lsmod`. Any option set to `m` needs this at `y`. |
| `CONFIG_KLOG`    | y/n | The kernel log ring behind `klog_printf`, `dmesg` and `/dev/klog`. |
| `CONFIG_DEVFS`   | y/n | `/dev`: `null`, `zero`, `full`, `console`, `random`, `urandom`, `klog`, and registry views. Needs `CONFIG_KLOG`, because `/dev/klog` reads the ring directly. |
| `CONFIG_KMON`    | y/n | The in-kernel rescue console. With it off, a machine that cannot start `/bin/init` halts instead of dropping to a prompt. |

### Network

| option | form | what it controls |
| ------ | ---- | ---------------- |
| `CONFIG_NET_RTL8139` | y/m/n | RealTek RTL8139 driver. QEMU's default NIC and the one `make run` and the test suite attach — turning it off costs the network tests. |
| `CONFIG_NET_E1000`   | y/m/n | Intel 82540EM driver. VirtualBox's default NIC. |
| `CONFIG_TCP`         | y/n   | TCP, and with it `curl`, `wget`, `telnet`, `http://` in the browser and `kpkg` over the network. |
| `CONFIG_UDP`         | y/n   | UDP datagram sockets and the `udp` tool. |
| `CONFIG_DNS`         | y/n   | The resolver and `nslookup`. Needs `CONFIG_UDP`. |

The two NIC drivers are the acceptance test for the whole modularity
exercise: they are interchangeable, they are bound by PCI ID rather than by
name, and each can be `y`, `m` or `n` independently of the other.

### Graphics

| option | form | what it controls |
| ------ | ---- | ---------------- |
| `CONFIG_GRAPHICS` | y/n | Linear framebuffer and the graphical console backend. Off means VGA text mode only. |
| `CONFIG_WM`       | y/n | Compositor, window manager, and the GUI apps (`desktop`, `terminal`, `gclock`, `gfiles`, `gpaint`). Needs `CONFIG_GRAPHICS`. |
| `CONFIG_MOUSE`    | y/n | PS/2 mouse driver. Independent of the compositor: the rescue console can use a pointer too. |

### Userspace

| option | form | what it controls |
| ------ | ---- | ---------------- |
| `CONFIG_PACKAGES`   | y/n | Stage the `.kpkg` repository and its index into the disk image, so `kpkg install <name>` works with no network. |
| `CONFIG_APPS_GAMES` | y/n | Games: `apps/snake.c`. |

## Known-good combinations

Everything below has been checked against the option schema; the first two
are the ones the project actually builds and boots.

| configuration | status |
| ------------- | ------ |
| everything `y` (the shipped `config`) | the default build, exactly the system that existed before this file did; boots on QEMU and VirtualBox and passes the end-to-end suite |
| `CONFIG_APPS_GAMES=n` | boots identically, with `snake` absent from `/bin` — the worked example that the mechanism really removes things |
| `CONFIG_NET_E1000=m`, `CONFIG_MODULES=y` | e1000 leaves the kernel image and becomes `/lib/modules/e1000.kmod`; QEMU's RTL8139 still works, and `insmod e1000` is needed on VirtualBox |
| `CONFIG_NET_RTL8139=m`, `CONFIG_NET_E1000=m` | a kernel with no NIC driver linked in at all; the network comes up only after `insmod` |
| `CONFIG_WM=n` | no compositor and no GUI apps; the console and every text tool are unaffected |
| `CONFIG_GRAPHICS=n`, `CONFIG_WM=n`, `CONFIG_MOUSE=n` | text-mode-only machine |
| `CONFIG_TCP=n`, `CONFIG_DNS=n`, `CONFIG_UDP=n` | no network stack above the NIC; `ping` still works, `curl`/`wget`/`telnet`/`nslookup`/`udp` are not built |
| `CONFIG_PACKAGES=n` | `kpkg` is still there but the image ships no repository, so only `kpkg install ./some.kpkg` works |

Combinations that are rejected rather than merely unwise:

| configuration | why |
| ------------- | --- |
| `CONFIG_WM=y`, `CONFIG_GRAPHICS=n` | the compositor has no framebuffer to composite onto |
| `CONFIG_DNS=y`, `CONFIG_UDP=n` | the resolver is a UDP client |
| `CONFIG_DEVFS=y`, `CONFIG_KLOG=n` | `/dev/klog` reads the log ring |
| anything `=m` with `CONFIG_MODULES=n` | there is no loader to load it with |
| `CONFIG_KMON=m`, and any other `y/n` option set to `m` | no module form; a rescue console that has to be loaded from disk is not a rescue console |

## Adding an option

1. Add an `Option(...)` to `OPTIONS` in `tools/mkconfig.py`, naming the
   kernel sources it owns, the apps that are pointless without it, and the
   options it requires.
2. Add the key to `config` with a comment saying what it does.
3. Guard the call sites with `#ifdef CONFIG_X` — the header is what makes
   `n` mean "no code", the make fragment is what makes it mean "no object".
4. Run `python3 tools/mkconfig.py`, commit the regenerated
   `kernel/include/config.h`, and add the combination to the table above
   once you have booted it.
