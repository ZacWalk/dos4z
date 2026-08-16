# MS-DOS 4.0 *modified*

[![Build](https://github.com/ZacWalk/dos4z/actions/workflows/build.yml/badge.svg)](https://github.com/ZacWalk/dos4z/actions/workflows/build.yml)

<p align="center">
  <img src="msdos-logo.png" alt="MS-DOS Logo" width="250">
</p>

This is a fork of the MS-DOS 4.00 source code as released by Microsoft. It has been modified to build on a modern Windows 11 machine using the *original* DOS build tools. Those tools are 16-bit, so they run under a small shim I created called **demu**. I actively use this version of DOS on my retro PCs and I am slowly adding features I need — `VER` reports **4.0z** so you can tell it apart.

`make-dos.ps1` produces two bootable floppy images in `output/`, which you can boot in a PC emulator (86Box, PCem, DOSBox-X, VirtualBox, …) or write to real media.

## Requirements

- Windows 10/11 with Windows PowerShell 5.1 or PowerShell 7+
- Visual Studio 2022 with the C++ workload — **only** needed to rebuild `demu.exe` / `dimg.exe`

## Building

Pre-built `demu.exe` and `dimg.exe` are committed to `output/`. Most contributors only need:

```powershell
.\make-dos.ps1                   # Build everything
.\make-dos.ps1 -Clean            # Clean rebuild
.\make-dos.ps1 -Component bios   # Build one component
.\make-dos.ps1 -Verbose          # Show full tool output
```

This uses demu to run the original DOS toolchain (NMAKE, MASM, LINK, CL, …) to compile the OS.
Components build in this order: `messages`, `mapper`, `boot`, `bios`, `dos`, `cmd`, `dev`,
`select`, `memm`. A full build takes a few minutes.

To rebuild the native tools after modifying sources in `tools/`:

```powershell
.\make-tools.ps1            # Rebuild demu.exe and dimg.exe (requires MSVC)
```

### Build output

| Path | Contents |
|------|----------|
| `output/bin/` | Final DOS binaries (`io.sys`, `msdos.sys`, `command.com`, utilities, drivers) |
| `output/logs/` | Per-component build logs — first place to look when something fails |
| `output/msdos4-360.img` | 360 KB bootable floppy image with the essential utilities |
| `output/msdos4-1440.img` | 1.44 MB bootable floppy image with the full utility set |

`SELECT.HLP` is the one product file that never gets built. `ASC2HLP.EXE` runs but rejects
`USA.TXT` with "Source File Error" before reading any topics, so the build reports one
missing file and carries on. `SELECT.EXE` itself builds and runs fine without it.

MS C 5.10 occasionally aborts with an internal compiler error in a random file. This is a
quirk of the original 1988 compiler — just run the build again.

## Repository Layout

| Directory | Contents |
|-----------|----------|
| `dos/` | MS-DOS 4.0 source tree (renamed from the original `src/`) |
| `dos/BOOT/` | Boot sector |
| `dos/BIOS/` | IO.SYS — hardware abstraction layer |
| `dos/DOS/` | MSDOS.SYS — kernel (INT 21h API, FAT, memory, processes) |
| `dos/CMD/` | External commands (COMMAND.COM, FORMAT, DEBUG, FDISK, …) |
| `dos/DEV/` | Device drivers (ANSI.SYS, RAMDRIVE.SYS, …) |
| `dos/SELECT/` | Interactive installer |
| `dos/MEMM/` | EMM386 expanded memory manager (386 protected mode) |
| `dos/MAPPER/` | OS/2-to-DOS API mapping library |
| `dos/MESSAGES/` | Localised message database |
| `dos/INC/`, `dos/H/`, `dos/LIB/` | Shared includes, headers and libraries |
| `dos/TOOLS/` | Original 16-bit build tools (MASM, CL, LINK, NMAKE, …) |
| `tools/demu/` | DOS emulator — runs the 16-bit tools on modern Windows ([details](tools/demu/README.md)) |
| `tools/dimg/` | FAT12 bootable disk image creator |
| `output/` | Pre-built tool binaries and build artifacts |

## Continuous Integration

[`.github/workflows/build.yml`](.github/workflows/build.yml) builds the native tools and then
the whole OS on `windows-latest` for every push and pull request against `master`.

## License

[MIT](LICENSE) — Copyright IBM and Microsoft Corporation.
