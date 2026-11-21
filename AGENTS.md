# KestrelOS Workspace Agent Guide

## C / x86-64 Code Navigation (`clangd` MCP Server)
- Use the `clangd` MCP server (`find_definition`, `find_references`, `get_hover`, `search_symbols`, `get_diagnostics`) backed by `compile_commands.json` in the repo root.
- Run `C:\Python315\python.exe tools/build.py compdb` (also updated automatically on `tools/build.py build`) whenever files or flags change in `kernel/`, `modules/`, `libc/`, `libgui/`, `libweb/`, `libjs/`, or `apps/`.
- Note Windows clang target flags differ between freestanding kernel/userspace translation units (`--target=x86_64-unknown-none-elf -ffreestanding -nostdlib`) and host tests (`-DJS_HOST`); keep all of them indexed via `tools/build.py compdb`.

## Live VM & Kernel Debugging (`qemu` + `gdbstub` MCP Servers)
- On Linux/WSL, no MCP servers are needed: run `make debug` (headless QEMU, paused at reset with `-s -S`), then `make gdb` in another terminal. Alternatively use `python3 tools/build.py debug` and `python3 tools/build.py gdb`. Both load `tools/.gdbinit` explicitly with `-x`, load `build/kernel.elf`, and connect to `localhost:1234`. Use `hbreak kmain`, `continue`, then `bt`; hardware breakpoints work before the higher-half kernel is mapped. See `docs/RUNNING.md#debugging-on-linuxwsl` for batch commands and shutdown instructions.
- Boot/inspect KestrelOS interactively via `qemu_boot(disk="build/os.img", accel="whpx:tcg", smp=2, extra_args="-device rtl8139,netdev=n0 -netdev user,id=n0")` (or `e1000`, `smp=4`), capture COM1 with `qemu_wait_serial` / `qemu_serial`, and inspect VGA framebuffer output with `qemu_screenshot`.
- For page faults, triple faults, or deadlocks, boot QEMU with `extra_args="-s -S -d int,cpu_reset -D int.log -no-reboot"` and attach `gdbstub` (`debug_connect`, symbols from `build/kernel.elf`, `debug_registers`, `debug_backtrace`, `debug_disassemble`, `debug_explain_fault`).

## Cross-Session Memory (`mem0`)
- On Windows PowerShell, invoke `C:\Users\aiari\AppData\Roaming\npm\mem0.cmd` (since `.ps1` scripts are blocked by ExecutionPolicy).
- Store major milestones, subtle hardware/QEMU quirks, and bug root causes with `mem0.cmd add "..."` and retrieve context with `mem0.cmd search "..."`.

## Build, Verification & Budget Invariants
- Keep `Makefile` and `tools/build.py` in lockstep when adding host tests or special link rules; new public `.PHONY` targets or build.py subcommands must also be accounted for in `tools/check_build_parity.py` `COMMANDS`/`MAKE_ONLY` (CI runs it first).
- Budgets: `build/kernel.bin < 900 KiB` (hard limit 992 KiB); every file under `build/rootfs/` `< 3.7 MiB` (KFS per-file cap ~4.04 MiB); browser C call stacks under the 64 KiB guarded stack limit (heap-allocate large state structs via `calloc`, mark deep pipelines `BROWSER_NOINLINE`).
- E2E invariants: do NOT alter `/compat` fixture behavior in `tools/e2e.py`; `rootfs/doc/home.html` rendered text must preserve `KestrelOS`, `HTML parser`, `Local pages`, and `TLS 1.3` in exact order.
- PowerShell 5.1 does not support `&&`; chain commands with `; if ($LASTEXITCODE -eq 0) { ... }`.
