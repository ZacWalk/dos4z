# MS-DOS 4.0 — Copilot Guidelines

## Project Overview

MS-DOS 4.00 source code (~95% x86 assembly, ~5% K&R C). Targets 8086 real-mode DOS. The original 1988 toolchain in `dos/TOOLS/` is MASM 5.10, Microsoft C 5.10, LINK 3.65 and NMAKE 1.00.05; it is 16-bit, so it runs on modern Windows under **demu**, a lightweight 8086 emulator in this repo.

## Layout

- `dos/` — DOS source tree (renamed from original `src/`)
  - `BOOT/` — boot sector → `MSBOOT.BIN`
  - `BIOS/` — IO.SYS (hardware abstraction, ~12 ASM files)
  - `DOS/` — MSDOS.SYS kernel (INT 21h API, FAT, memory, ~50 ASM files)
  - `CMD/` — external commands (COMMAND.COM, FORMAT, DEBUG, FDISK, etc.)
  - `DEV/` — device drivers (ANSI.SYS, RAMDRIVE.SYS, etc.)
  - `SELECT/` — interactive installer
  - `MEMM/` — EMM386 expanded memory manager (386 protected mode)
  - `MAPPER/` — OS/2-to-DOS API mapping library
  - `MESSAGES/` — localized message database
  - `INC/`, `H/`, `LIB/` — shared assembly includes, C headers, prebuilt libraries
  - `TOOLS/` — original 16-bit build tools (MASM, CL, LINK, NMAKE, etc.)
- `tools/demu/` — DOS emulator (runs 16-bit tools on modern Windows)
- `tools/dimg/` — FAT12 bootable disk image creator
- `make-dos.ps1` — build script for MS-DOS (uses pre-built demu/dimg from `output/`)
- `make-tools.ps1` — build script for demu and dimg (requires MSVC)
- `output/` — pre-built tool binaries (`demu.exe`, `dimg.exe`), `bin/`, `logs/`, `tmp/`, disk images
- `.github/workflows/build.yml` — CI: builds tools then the whole OS on `windows-latest`

## Building

```powershell
.\make-tools.ps1              # Rebuild demu.exe and dimg.exe (only needed after tool changes)
.\make-dos.ps1                # Build everything (requires demu.exe in output/)
.\make-dos.ps1 -Component bios   # Build one component
.\make-dos.ps1 -Clean         # Clean rebuild
.\make-dos.ps1 -Verbose       # Echo full tool output as well as writing logs
```

Pre-built `demu.exe` and `dimg.exe` are committed to `output/` (force-added; `output/` is otherwise gitignored). Most contributors only need `.\make-dos.ps1`. Run `.\make-tools.ps1` only after modifying tool sources in `tools/`.

Build order: messages → mapper → boot → bios → dos → cmd → dev → select → memm.

A component failure does not stop the run — the script continues and lists failures at the end. Check `output/logs/<component>.log` first when diagnosing a failure.

Known gaps and gotchas:

- `SELECT.HLP` never gets built, so output collection always reports one missing file. `SELECT/MAKEFILE` runs `asc2hlp USA.TXT select.hlp`; `ASC2HLP.EXE` is present in `dos/TOOLS` and does execute, but prints "Source File Error" and exits 0 without writing topics — it fails the same way with no arguments at all, and `USA.TXT` is valid CRLF text ending in 0x1A. Under `-v` the failing call is an `INT 21h/3Dh` open of a garbage path at `DS:DX = PSP:0181`. Unresolved; unrelated to any recent change.
- MS C 5.10 occasionally dies with `fatal error C1001: Internal Compiler Error` (sometimes followed by `run-time error R6001`) in a *different* file on each run. Re-run the build before investigating — a failure that moves between components is the compiler, not your change.
- `-Component <name>` only builds that one directory. It does **not** build prerequisites, and a full build ends by deleting in-tree intermediates, so e.g. `-Component cmd` on its own fails looking for `boot/boot.cl1` and `INC/MSDOSME.OBJ`. Use it for iterating on a component you have just built, not from a clean tree.

## Build System

- **NMAKE** with rules in `TOOLS/TOOLS.INI`
- Key makefile variables: `extasw` (asm switches), `extcsw` (C switches), `inc`, `dos`, `hinc`, `msg` (relative paths to shared dirs)
- Message system: `.SKL` skeletons → `NOSRVBLD` → `.CL*` files → included via `SYSMSG.INC`/`MSGSERV.ASM`
- Common patterns: `link` → `exe2bin`/`convert` for .COM/.SYS; C+ASM utilities use `_PARSE.ASM` + `_MSGRET.ASM` stubs
- Linker response files (`.LNK`) use `+` separators in MS LINK format
- `make-dos.ps1` builds each `CMD/` subdirectory directly rather than via `CMD/MAKEFILE`, because the original `cd`+`nmake` pairs nest EXEC 3–4 deep in demu and exhaust guest memory

## Code Conventions

**Assembly:** Manual `SEGMENT`/`ENDS`/`GROUP` (no `.MODEL`). `ASSUME` for segment tracking. `procedure`/`EndProc` macros from `DOSMAC.INC`. Custom `.IF`/`.ELSE`/`.ENDIF` macros in `STRUC.INC` (not MASM 6+). `IF1`/`IF2` pass-dependent conditionals.

**C (DOS sources):** K&R declarations, `far`/`near` pointers, `int86()`/`intdos()` for interrupts, small memory model default.

**C (tools/):** C99, MSVC, `/W4`-clean apart from benign `int`→`Word` narrowing in the emulator core. Keep it lightweight: no third-party dependencies, no dead code, and shared globals declared once in `8086.h` / `exe.h` rather than re-`extern`'d in each translation unit.

## demu Notes

- Source: `tools/demu/` (build with `.\make-tools.ps1`, requires MSVC) — see `tools/demu/README.md`
- Usage: `demu [-v] [-d] [-shell] <program.exe|program.com> [args...]`
  - `-v` verbose syscall/load tracing, `-d` dump MZ header and exit, `-shell` COMMAND.COM mode
- If locked: `taskkill /f /im demu.exe` before rebuilding
- Shadow RAM (`shadowRam[]`) tracks per-byte read/write permission; reading uninitialized or writing unallocated memory aborts with a diagnostic. When a loader/allocator change causes "Reading uninitialized address", the fix is almost always a missing `setShadowFlags` on a newly mapped region, not a real guest bug.
- EXEC is in-process: `execInProcess()` saves CPU state, PSP, DTA, fd table and cwd, loads the child into a fresh MCB block, and returns via `longjmp`. Max nesting is `EXEC_MAX_DEPTH` (8).

### Native Command Interception

Some original 16-bit tools cannot run under 8086 emulation (e.g. `LIB.EXE` is an OS/2 bound NE executable). demu intercepts these by name and runs native C reimplementations instead.

When a program running inside demu calls `EXEC` (INT 21h/4Bh), `execInProcess()` checks `runNativeCommand()` **before** loading the child binary. If the basename matches a known native command, the C implementation runs directly and the DOS binary is never loaded. The same check also runs at the top level in `main()`, so `demu LIB.EXE args` works from the command line too.

**Dispatch table** (`exec-dos.c` → `nativeCommands[]`):
- Shell builtins: `copy`, `del`/`erase`, `ren`/`rename`, `type`, `echo`, `cd`/`chdir`, `set`, `path`, `md`/`mkdir`, `rd`/`rmdir`, `attrib`, `cls`, `ver`
- Batch-flow no-ops: `if`, `for`, `goto`, `call`, `shift`, `rem`, `pause`, `break`, `verify`, `ctty`, `vol`, `date`, `time`, `dir`
- `lib` → `nativeLib()` in `lib-omf.c` (OMF library manager — reads/writes `.LIB` archives)

**Adding a new native command:**
1. Write the implementation as `int myCmd(const char *args)` (return 0=success)
2. Add an entry to the `nativeCommands[]` table in `exec-dos.c`
