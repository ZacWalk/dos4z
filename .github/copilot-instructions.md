# MS-DOS 4.0 — Copilot Guidelines

## Project Overview

MS-DOS 4.00 source code (~95% x86 assembly / MASM 5.x, ~5% K&R C / Microsoft C 4.0). Targets 8086 real-mode DOS. Built on modern Windows via **demu** (a lightweight DOS emulator that runs the original 16-bit tools).

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
  - `INC/`, `H/` — shared assembly includes and C headers
  - `TOOLS/` — original 16-bit build tools (MASM, CL, LINK, NMAKE, etc.)
  - `MESSAGES/` — localized message database
- `tools/demu/` — DOS emulator (runs 16-bit tools on modern Windows)
- `tools/dimg/` — disk image creator
- `make-dos.ps1` — build script for MS-DOS (uses pre-built demu/dimg from output/)
- `make-tools.ps1` — build script for demu and dimg (requires MSVC)
- `output/` — pre-built tool binaries (demu.exe, dimg.exe) and build artifacts

## Building

```powershell
.\make-tools.ps1              # Rebuild demu.exe and dimg.exe (only needed after tool changes)
.\make-dos.ps1                # Build everything (requires demu.exe in output/)
.\make-dos.ps1 -Component bios   # Build one component
.\make-dos.ps1 -Clean         # Clean rebuild
```

Pre-built `demu.exe` and `dimg.exe` are checked into `output/`. Most contributors only need `.\make-dos.ps1`. Run `.\make-tools.ps1` only after modifying tool sources in `tools/`.

Build order: messages → mapper → boot → bios → dos → cmd → dev → select → memm.

## Build System

- **NMAKE** with rules in `TOOLS/TOOLS.INI`
- Key makefile variables: `extasw` (asm switches), `extcsw` (C switches), `inc`, `dos`, `hinc`, `msg` (relative paths to shared dirs)
- Message system: `.SKL` skeletons → `NOSRVBLD` → `.CL*` files → included via `SYSMSG.INC`/`MSGSERV.ASM`
- Common patterns: `link` → `exe2bin`/`convert` for .COM/.SYS; C+ASM utilities use `_PARSE.ASM` + `_MSGRET.ASM` stubs
- Linker response files (`.LNK`) use `+` separators in MS LINK format

## Code Conventions

**Assembly:** Manual `SEGMENT`/`ENDS`/`GROUP` (no `.MODEL`). `ASSUME` for segment tracking. `procedure`/`EndProc` macros from `DOSMAC.INC`. Custom `.IF`/`.ELSE`/`.ENDIF` macros in `STRUC.INC` (not MASM 6+). `IF1`/`IF2` pass-dependent conditionals.

**C:** K&R declarations, `far`/`near` pointers, `int86()`/`intdos()` for interrupts, small memory model default.

## demu Notes

- Source: `tools/demu/` (build with `.\make-tools.ps1`, requires MSVC)
- Usage: `demu [-v] <program.exe> [args...]`
- If locked: `taskkill /f /im demu.exe` before rebuilding

### Native Command Interception

Some original 16-bit tools cannot run under 8086 emulation (e.g. `LIB.EXE` is an OS/2 bound NE executable). demu intercepts these by name and runs native C reimplementations instead.

When a program running inside demu calls `EXEC` (INT 21h/4Bh), `execInProcess()` checks `runNativeCommand()` **before** loading the child binary. If the basename matches a known native command, the C implementation runs directly and the DOS binary is never loaded. The same check also runs at the top level in `main()`, so `demu LIB.EXE args` works from the command line too.

**Dispatch table** (`exec-dos.c` → `nativeCommands[]`):
- Shell builtins: `cd`, `copy`, `del`, `ren`, `set`, `md`, `rd`, `echo`, `type`, `attrib`, `ver`, `cls`
- `lib` → `nativeLib()` in `lib-omf.c` (OMF library manager — reads/writes `.LIB` archives)

**Adding a new native command:**
1. Write the implementation as `int myCmd(const char *args)` (return 0=success)
2. Add an entry to the `nativeCommands[]` table in `exec-dos.c`
