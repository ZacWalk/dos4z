# demu - DOS Executable Emulator

A lightweight 8086 CPU emulator that runs DOS .EXE and .COM executables on modern Windows.
Similar in concept to Wine on Linux — maps DOS INT 21h system calls to Windows equivalents.

## Purpose

Run the 16-bit DOS build tools from `dos/TOOLS/` (MASM, CL, LINK, NMAKE, etc.) directly
on Windows 11 without needing a full DOS emulator or virtual machine.

## Building

Requires Visual C++ (Visual Studio 2022 or later):

```powershell
.\make-tools.ps1
```

## Usage

```
demu [-v] <program.exe|program.com> [args...]
```

Options:
- `-v` — Verbose output (show load addresses and syscall info)

Examples:
```powershell
# Run MASM
.\output\demu.exe dos\TOOLS\MASM.EXE source.asm

# Run the C compiler
.\output\demu.exe dos\TOOLS\CL.EXE /c file.c

# Run NMAKE
.\output\demu.exe dos\TOOLS\NMAKE.EXE /f MAKEFILE
```

## Architecture

- **main.c** — Main entry point, interrupt dispatch
- **8086.c / 8086.h** — Full 8086 CPU emulator (1MB address space)
- **exe.h** — DOS MZ executable header structures
- **loader-dos.c** — .EXE and .COM file loader with relocation support
- **syscall-dos.c** — DOS INT 21h system call handler (file I/O, console, memory)

## Credits

8086 emulator originally from [Andrew Jenner's reenigne project](https://github.com/reenigne/reenigne).
DOS enhancements by TK Chia. ELKS support and rewrite by Greg Haerr.
Windows port for demu project.
