# MS-DOS 4.0 — Copilot Guidelines

## Project Overview

MS-DOS 4.00 source code (~95% x86 assembly, ~5% K&R C). Targets 8086 real-mode DOS. The original 1988 toolchain in `dos/TOOLS/` is MASM 5.10, Microsoft C 5.10, LINK 3.65 and NMAKE 1.00.05; it is 16-bit, so it runs on modern Windows under **demu**, the command-line runner of [libdos](https://github.com/ZacWalk/libdos). `make-dos.ps1` downloads demu, and **dimg**, which makes the disk images, from a pinned libdos release.

## Layout

- `dos/` — DOS source tree (renamed from original `src/`)
  - `BOOT/` — boot sector → `MSBOOT.BIN`
  - `BIOS/` — IO.SYS (hardware abstraction, ~12 ASM files)
  - `DOS/` — MSDOS.SYS kernel (INT 21h API, FAT, memory, ~50 ASM files)
  - `CMD/` — external commands (COMMAND.COM, FORMAT, DEBUG, FDISK, etc.); `CMD/DISKINFO/` is the one built by libdos's C compiler (`demu --cc`) rather than NMAKE, as `make-dos.ps1`'s last component
  - `DEV/` — device drivers (ANSI.SYS, RAMDRIVE.SYS, etc.)
  - `SELECT/` — interactive installer
  - `MEMM/` — EMM386 expanded memory manager (386 protected mode)
  - `MAPPER/` — OS/2-to-DOS API mapping library
  - `MESSAGES/` — localized message database
  - `INC/`, `H/`, `LIB/` — shared assembly includes, C headers, prebuilt libraries
  - `TOOLS/` — original 16-bit build tools (MASM, CL, LINK, NMAKE, etc.)
- `docs/LARGE-DISKS.md` — how the LBA / large disk support works, IBM 5150 + XT-IDE notes, the boot tests and DOSBox testing
- `tests/` — test programs for `test-boot.ps1`, written in the C libdos's compiler takes (`testfix.c` provokes each bug fix a program can see through INT 21h)
- `make-dos.ps1` — build script for MS-DOS; gets demu and dimg from the pinned libdos release (or a checkout, `-LibdosPath`)
- `test-boot.ps1` — boots the build with `demu --boot` (libdos's emulated IBM PC 5150, an 8088) and checks floppies, CHS and LBA hard disks, FDISK/FORMAT, unusual FAT layouts, DISKINFO, the bug fixes (`tests/testfix.c`, compiled by libdos's C compiler) and COMMAND.COM's line editor and history (`KEYS.COM`, which the script writes from a byte array, queues the keys `demu --key` has no name for: Home, End, Ins, Del, ^C). Documented in `docs/LARGE-DISKS.md` ("Testing with demu --boot"); CI runs it after the build
- `test-dosbox.ps1` — boots the build in DOSBox with a `dimg -hd` disk image, for trying things by hand. Its parameters and behaviour are documented only in `docs/LARGE-DISKS.md` ("Testing in DOSBox"), which its help points to; update both together
- `output/` — `demu.exe` and `dimg.exe` (copied there by each build), `tools/` (the libdos download), `bin/`, `logs/`, `tmp/`, disk images; all of it untracked
- `.github/workflows/build.yml` — CI: builds the whole OS on `windows-latest`, then runs `test-boot.ps1`

## Building

```powershell
.\make-dos.ps1                # Build everything
.\make-dos.ps1 -Component bios   # Build one component
.\make-dos.ps1 -Clean         # Clean rebuild
.\make-dos.ps1 -Verbose       # Echo full tool output as well as writing logs
.\make-dos.ps1 -LibdosPath ..\libdos   # Build with a libdos checkout's demu and dimg
.\test-boot.ps1               # Boot the build in an emulated 5150 and check it works (seconds)
```

The first build downloads the libdos release pinned in `make-dos.ps1` (`$libdosVersion`, with the zip's SHA-256 in `$libdosSha256`) into `output/tools/`, and every build copies its `demu.exe` and `dimg.exe` to `output/`. Nothing needs installing, and no compiler: demu and dimg are developed in libdos.

Build order: messages → mapper → boot → bios → dos → cmd → dev → select → memm → diskinfo.

A component failure does not stop the run — the script continues and lists failures at the end. Check `output/logs/<component>.log` first when diagnosing a failure.

Known gaps and gotchas:

- MS C 5.10 was seen to die now and then with `fatal error C1001: Internal Compiler Error` (sometimes followed by `run-time error R6001`) in a *different* file on each run under the old demu. It has not happened under libdos; if it does, re-run the build before investigating — a failure that moves between components is the compiler, not your change.
- `-Component <name>` only builds that one directory. It does **not** build prerequisites, and a full build ends by deleting in-tree intermediates, so e.g. `-Component cmd` on its own fails looking for `boot/boot.cl1` and `INC/MSDOSME.OBJ`. Use it for iterating on a component you have just built, not from a clean tree.
- `dos/MESSAGES/usa-ms.IDX` is committed. NMAKE regenerates it from `usa-ms.msg`, and bumps the `level` line in the `.msg`, whenever the `.msg` is newer — commit both files together after changing messages.
- Original release binaries the build needs (`*.LIB`, the printer `*.CPI` files, `INDEXMAE.SYS`) are tracked through `.gitignore` exceptions and listed in `$keep` in `make-dos.ps1` so the in-tree clean leaves them alone. Do not replace them with stubs.
- Batch files and `CONFIG.SYS` written for a DOS guest need CRLF line endings: COMMAND.COM reads an LF-only batch file as one long line (symptoms such as "File creation error"). PowerShell here-strings are LF-only.
- DOSBox-X can drop the last write to a disk image before its window closes; write one more file before closing when a test reads results back from the image.
- COMMAND.COM's memory layout: data its transient part changes belongs in TRANSPACE (`TSPC.ASM`), because the checksum that decides whether the transient must be loaded again from disk covers TRANCODE and TRANDATA; resident data belongs in `RDATA.ASM`'s DATARES before `DATARESEND`, because init frees the rest. The prompt's line editor (`TEDIT.ASM`) follows this: its state is in `TSPC.ASM` and the command history, `HistBuf`, in `RDATA.ASM`. Objects contributing to TRANSPACE link before `TSPC.OBJ`, which ends it with `TRANSPACEEND`.
- The 360 KB image (`$utils360` in `make-dos.ps1`) has about 13 KB to spare. A bigger COMMAND.COM or another utility can overflow it, and the build then reports "360KB disk image creation failed".

## Build System

- **NMAKE** with rules in `TOOLS/TOOLS.INI`
- Key makefile variables: `extasw` (asm switches), `extcsw` (C switches), `inc`, `dos`, `hinc`, `msg` (relative paths to shared dirs)
- Message system: `.SKL` skeletons → `NOSRVBLD` → `.CL*` files → included via `SYSMSG.INC`/`MSGSERV.ASM`
- Common patterns: `link` → `exe2bin`/`convert` for .COM/.SYS; C+ASM utilities use `_PARSE.ASM` + `_MSGRET.ASM` stubs
- Linker response files (`.LNK`) use `+` separators in MS LINK format
- `make-dos.ps1` builds each `CMD/` subdirectory directly rather than via `CMD/MAKEFILE`, because the original `cd`+`nmake` pairs nest EXEC 3–4 deep, which exhausted guest memory under the old demu; it has not been re-tried under libdos's

## Code Conventions

**Assembly:** Manual `SEGMENT`/`ENDS`/`GROUP` (no `.MODEL`). `ASSUME` for segment tracking. `procedure`/`EndProc` macros from `DOSMAC.INC`. Custom `.IF`/`.ELSE`/`.ENDIF` macros in `STRUC.INC` (not MASM 6+). `IF1`/`IF2` pass-dependent conditionals. Everything outside MEMM, XMAEM, `BIOS/MSHARD.ASM` (an IBM AT ROM workaround installed only on ATs) and the extended-memory paths of RAMDRIVE/SMARTDRV must stay 8086-only so it runs on an IBM PC 5150 (MASM 5.10 defaults to 8086; never add `.186`/`.286`). `test-boot.ps1` checks this for what it runs: `demu --boot` stops at any instruction an 8088 does not have. Conditional jumps and `JCXZ` are short (±127 bytes) — use an inverted jump around a `JMP` when a target is out of range.

**C (DOS sources):** K&R declarations, `far`/`near` pointers, `int86()`/`intdos()` for interrupts, small memory model default.

**C for libdos's compiler** (`dos/CMD/DISKINFO/`, `tests/`): built with `demu --cc` into a tiny-model `.COM`, so ANSI definitions only, no `far` pointers, no struct assignment and no run-time floating point (libdos's `docs/todo.md` lists the subset). There is no C library unless a program includes one: these talk to DOS and the BIOS through the compiler's thunks (`__int21`, `__int21si`, `__int13`, `__int13si`, with `__cf`, `__bx`, `__cx` and `__dx` holding what the call left), and must stay 8086-only like the rest.

## demu and dimg (libdos)

Both live in [libdos](https://github.com/ZacWalk/libdos); a change to either is made there, where this repository's build is the gate - every binary in `output/bin` byte for byte the same - and released, then pinned here by changing `$libdosVersion` and `$libdosSha256` together. Build here with a libdos checkout's `exe/` through `-LibdosPath` to try a change first.

- `make-dos.ps1` runs `demu -r <repo> dos\TOOLS\NMAKE.EXE` in each component directory: the repository is drive C:, so the guest sees `C:\DOS\BIOS` wherever it is checked out, and nothing outside it. PATH, INCLUDE, LIB, INIT, TMP and TEMP are passed in moved onto that drive. The CPU is a strict 8086, the date is fixed at 15 June 1988 and DOS reports 3.20.
- NMAKE's `COMMAND.COM /C` lines are carried out natively by demu - built-in commands (`copy`, `del`, `echo`, `if exist`, `for`, ...), redirection and PATH search - and `LIB.EXE`, an OS/2 bound executable that cannot run, is replaced by a native OMF librarian.
- Creating, opening or deleting a file cuts the last path component to 8.3 the way DOS does. `RAMDRIVE.LNK` names its map `ramdrive.map -map`; LINK creates that file and then reopens it, so both calls must agree on `RAMDRIVE.MAP`.
- `demu -v` reports every DOS call and file open on stderr: the first thing to look at when a tool fails in a way the log does not explain. Run it by hand from the component directory with the environment `make-dos.ps1` sets.
- `dimg -dir <image>` lists the volumes and files on an image and `dimg -extract <image> C:\PATH <file>` copies one out, which is how a test reads back what DOS wrote to a disk.
