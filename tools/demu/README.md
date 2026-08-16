# demu — DOS Executable eMUlator

A lightweight 8086 CPU emulator that runs DOS `.EXE` and `.COM` executables on modern
Windows. Similar in concept to Wine on Linux — it interprets 8086 instructions and maps
DOS `INT 21h` system calls onto Win32 equivalents rather than emulating a whole PC.

## Purpose

Run the 16-bit DOS build tools from `dos/TOOLS/` (MASM, CL, LINK, NMAKE, …) directly on
Windows 11, without a full PC emulator or virtual machine.

## Building

Requires Visual C++ (Visual Studio 2022 or later). From the repository root:

```powershell
.\make-tools.ps1
```

The result is `output/demu.exe`.

## Usage

```
demu [-v] [-d] [-shell] <program.exe|program.com> [args...]
```

| Option   | Meaning |
|----------|---------|
| `-v`     | Verbose — log load addresses, syscalls and EXEC activity to stderr |
| `-d`     | Dump the MZ header / load map of the program and exit (no execution) |
| `-shell` | Run a command shell (COMMAND.COM): reports DOS 4.00 and disables the stack guard |
| `--`     | End of options; everything after is passed to the guest |

Examples:

```powershell
.\output\demu.exe dos\TOOLS\MASM.EXE source.asm
.\output\demu.exe dos\TOOLS\CL.EXE /c file.c
.\output\demu.exe dos\TOOLS\NMAKE.EXE /f MAKEFILE
.\output\demu.exe -d dos\TOOLS\LINK.EXE
```

## Architecture

| File | Contents |
|------|----------|
| `main.c` | Entry point, option parsing, interrupt dispatch (including IVT-installed handlers) |
| `8086.c` / `8086.h` | 8086 CPU core, 1 MB address space, shadow-RAM access checking |
| `exe.h` | DOS MZ executable header structures and `struct exe` |
| `loader-dos.c` | `.EXE` / `.COM` loader: relocation, PSP and environment setup |
| `syscall-dos.c` | `INT 21h` / `INT 2Fh` / `INT 1Ah` handler, MCB memory arena, file handles |
| `exec-dos.c` / `exec-dos.h` | `INT 21h/4Bh` EXEC — in-process child loading and native command dispatch |
| `lib-omf.c` | Minimal OMF library manager used as the native `LIB.EXE` replacement |

### Shadow RAM

Every byte of the 1 MB address space has a companion `fRead` / `fWrite` flag byte. Reading
uninitialised memory or writing outside an allocated block aborts with a diagnostic instead
of silently corrupting the guest — this catches loader and allocator bugs early.

### In-process EXEC

`INT 21h/4Bh` does *not* spawn a Windows process. The child is loaded into the same 1 MB
image at a freshly allocated MCB block; the parent's CPU state, PSP, DTA, file-descriptor
table and working directory are saved on an explicit stack (`EXEC_MAX_DEPTH` = 8) and
restored via `longjmp` when the child terminates. This keeps nested `NMAKE → CL → LINK`
chains fast and avoids re-entering the emulator.

### Native command interception

Some original 16-bit tools cannot run under 8086 emulation — for example `LIB.EXE` is an
OS/2 bound NE executable. `runNativeCommand()` in `exec-dos.c` matches the program's base
name against a dispatch table and runs a native C implementation instead; the DOS binary is
never loaded. The same check runs at the top level in `main()`, so `demu LIB.EXE args` works
from the command line too.

The table covers `lib` plus the COMMAND.COM builtins the build system invokes via
`COMMAND.COM /c`: `copy`, `del`/`erase`, `ren`/`rename`, `type`, `echo`, `cd`/`chdir`,
`set`, `path`, `md`/`mkdir`, `rd`/`rmdir`, `attrib`, `cls`, `ver`, and no-op stubs for
batch-flow keywords (`if`, `for`, `goto`, `call`, `shift`, `rem`, `pause`, `break`,
`verify`, `ctty`, `vol`, `date`, `time`, `dir`).

To add a command: write `int myCmd(const char *args)` returning 0 on success, then add an
entry to `nativeCommands[]` in `exec-dos.c`.

## Credits

8086 emulator originally from
[Andrew Jenner's reenigne project](https://github.com/reenigne/reenigne).
DOS enhancements by TK Chia. ELKS support and rewrite by Greg Haerr.
Windows port and DOS build-tool support for demu.
