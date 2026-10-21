# Large disk support

MS-DOS 4.0z can use hard disks of any size up to 2 TB. That includes CompactFlash cards on an
XT-IDE / XT-CF adapter in an IBM PC 5150. Each DOS partition is still FAT16, so it holds at
most 2 GB, and a big disk shows up as several drive letters. The letters run out at Z:, so DOS
can use about 48 GB of any one disk (see [How big a card?](#how-big-a-card)).

## What changed compared with MS-DOS 4.00

| | MS-DOS 4.00 | MS-DOS 4.0z |
|---|---|---|
| Disk access | INT 13h CHS calls only | INT 13h extensions (LBA) when the BIOS has them, CHS otherwise |
| Usable part of a disk | First 1024 cylinders: 504 MB with a 16-head BIOS geometry, about 8 GB with 255 heads | The whole disk with LBA, the first ~8 GB with CHS |
| Partition types | 01h, 04h, 06h, extended 05h | Also 0Eh and extended 0Fh on disks reached through LBA |
| Logical drives | Located from the CHS fields; misplaced when an EBR is not on a cylinder boundary or lies past cylinder 1023 | Located from the 32-bit LBA fields of the partition tables; the EBR chain is followed only forward |
| A request for cylinder 1024 | Wrapped round to cylinder 0 | Rejected |
| BIOS geometry with 256 heads | Stored as 0 heads | Works |
| Partitions too big for FAT16 | Could be mounted with nonsense parameters | Get a drive letter but are refused |
| Volumes formatted by other systems | Trusted only if the boot sector names MS-DOS, PC DOS or OS/2; others (Linux `mkfs.fat`, Windows 95, cameras) were assumed to have 1 reserved sector and 2 FATs, were read in the wrong place when they had not, and writes corrupted them | A boot sector that describes a FAT12/16 volume is used whatever system wrote it, with its reserved sectors and number of FATs |
| FDISK | 16-bit size arithmetic (wrong above 4 GB); sizes on the command line limited to 4000 MB; creates partitions too big for FAT16 | 32-bit arithmetic; DOS partitions capped at 2047 MB; sizes up to 65535 MB |
| FORMAT | "Divide overflow" when a partition has more than 65,535 tracks (2 GB with 255 heads) or starts more than ~2 GB into the disk | 32-bit track counts |
| SHARE.EXE | Loaded at start-up, taking 6 KB of memory, whenever a drive is larger than 32 MB; without it, programs that open files with FCBs could not use such a drive | Never needed: FCBs work on drives of up to 2 GB without it ([FCBs without SHARE](#fcbs-without-share)) |

## How it works

### Asking the BIOS

At start-up IO.SYS calls INT 13h function 41h (`BX=55AAh`) for each hard disk. If the call
succeeds, returns `BX=AA55h` and sets bit 0 of `CX` (the "fixed disk access" subset), the disk
is treated as an LBA disk. All its reads, writes and verifies then use functions 42h, 43h and
44h with a 32-bit sector number. Other disks keep using the classic CHS functions 02h, 03h and
04h.

IO.SYS still issues `INT 13h`, so programs that hook the interrupt see every request. A disk
cache that only knows the CHS functions, such as SMARTDRV.SYS, does not cache the extended
calls.

### Transfers

- A request covers at most 127 sectors, the limit in the EDD specification.
- The buffer address is normalised, and requests are split so that none crosses a 64 KB
  physical boundary. A sector that straddles a boundary goes through IO.SYS's own sector
  buffer. This matters for DMA controllers and costs nothing on PIO ones such as the XT-IDE.
- Errors are retried and reported exactly like CHS errors. With `VERIFY ON`, every write is
  followed by a function 44h verify.

### Finding the partitions

For each hard disk, at start-up:

1. The master boot record is read. Its first partition of type 01h, 04h or 06h (or 0Eh on an
   LBA disk) becomes the disk's primary DOS drive: C: on the first disk, D: on the second.
2. The first extended partition (05h, or 0Fh on an LBA disk) is followed one extended boot
   record (EBR) at a time. Each EBR's DOS entry becomes the next logical drive, and its link
   entry points to the next EBR. Positions come only from the LBA fields: a logical drive
   starts relative to its EBR, the next EBR relative to the start of the extended partition.
   The CHS fields are not used.
3. Each drive's "hidden sectors" value is its absolute starting sector, as in MS-DOS 5 and
   later. FORMAT copies it into the boot sector of the drive it formats.
4. Safety limits: a link is followed only to an EBR further on and inside the extended
   partition, so a damaged chain that points back cannot give the same drives more letters;
   at most 64 EBRs are followed per disk, at most 23 logical drives in total, and never more
   than the 26 drive letters.

Types 0Eh and 0Fh say "the CHS fields are not valid", so they are only used on LBA disks. On a
disk without LBA, a partition or EBR that starts past cylinder 1023 cannot be reached and is
skipped.

### Choosing the FAT layout

A formatted partition is described by the BPB in its boot sector. Sectors per cluster, reserved
sectors, number of FATs, FAT size and root directory size all come from there, so volumes
formatted by other systems are read correctly. MS-DOS 4.00 believed a BPB only when the boot
sector named MS-DOS, PC DOS or OS/2 (3.1 or later); 4.0z also believes one with any other name,
such as `mkfs.fat`'s, when it describes a FAT12/16 volume: a jump instruction first, 512-byte
sectors, a power of two sectors per cluster, reserved sectors, one or two FATs, root directory
entries, a FAT size, a media byte and a size. A volume with more than 65,524 clusters (the most
FAT16 can number) is refused rather than mounted wrongly.

An unformatted partition (for example straight after FDISK) gets its layout from this table.
FORMAT uses the same table, always with 1 reserved sector and 2 FATs:

| Partition size | FAT | Cluster size |
|---|---|---|
| up to 16 MB | FAT12 | 4 KB |
| up to 128 MB | FAT16 | 2 KB |
| up to 256 MB | FAT16 | 4 KB |
| up to 512 MB | FAT16 | 8 KB |
| up to 1 GB | FAT16 | 16 KB |
| up to 2047 MB | FAT16 | 32 KB |
| up to 4 GB | FAT16 | 64 KB, from Microsoft's original table (avoid, see below) |
| larger | | not usable |

FDISK and `dimg` never create a DOS partition bigger than 2047 MB. FAT16 volumes with 64 KB
clusters, as Windows NT can make, are not supported by many programs or by MS-DOS 5 and 6, so
do not use them.

### What still uses CHS

- **Booting.** The master boot record and C:'s boot sector read the disk with CHS calls, using
  the sectors per track and heads recorded on the disk. C: must lie in the first 1024
  cylinders, which it does as the first partition, and the recorded sectors per track must
  match the BIOS's. The number of heads does not matter as long as the BIOS reports at least
  16: everything read while booting lies within the first 1,008 sectors. FDISK and FORMAT on
  the machine that boots from the disk always match; `dimg` uses 63 sectors per track, as the
  XT-IDE Universal BIOS does for cards larger than 504 MB.
- **FDISK.** It works in cylinders using the geometry from INT 13h function 08h, so it sees at
  most 1024 cylinders (about 8 GB) of any disk. Bigger disks are set up on a PC instead (see
  [Setting up a card larger than 8 GB](#setting-up-a-card-larger-than-8-gb)). Do not use
  4.0z FDISK on a disk that already has partitions beyond 8 GB or of type 0Eh/0Fh: it does
  not understand them.
- **FORMAT's surface check.** FORMAT verifies a partition track by track through generic
  IOCTL calls that take cylinder, head and sector. On an LBA disk IO.SYS converts these back
  to LBA with the same geometry, so this works anywhere below cylinder 65,536 (about 500 GB
  with 255 heads).

### FCBs without SHARE

Programs written for DOS 1, and some ported from CP/M, open files with file control blocks
(FCBs) rather than handles. DOS keeps a system file table entry for only a few FCBs at a time
(`FCBS=`, 4 by default) and, when a program has more open, rebuilds an entry from what it
stored in the FCB's reserved bytes, among them the number of the sector holding the file's
directory entry. MS-DOS 4.00 stored only the low 16 bits of that number, enough for 32 MB, and
refused to open an FCB further into a drive unless SHARE was loaded. So it loaded SHARE.EXE
itself at start-up whenever a drive was bigger than 32 MB, at a cost of 6 KB of memory, and
warned "SHARE should be loaded for large media" if it could not find it.

4.0z keeps six more bits of the sector number in another of the FCB's reserved bytes, which
covers the 2 GB of a FAT16 drive, so FCBs work on every drive without SHARE and IO.SYS no
longer loads it. SHARE.EXE is still there for file sharing and locking (`INSTALL=` in
CONFIG.SYS, or AUTOEXEC.BAT). With SHARE loaded, DOS cannot rebuild an FCB's entry, so a
program with more FCBs open than `FCBS=` allows gets "FCB unavailable"; raise `FCBS=` then.

## IBM PC 5150 with an XT-IDE or XT-CF card

### Requirements

- **BIOS dated 10/27/82** (the third 5150 BIOS). Earlier versions do not run option ROMs, so
  the card's BIOS never starts. `DEBUG` shows the date with `D F000:FFF5 L 8`.
- **XT-IDE Universal BIOS (XUB)** on the card. Using more than about 8 GB of a card needs a
  build with LBA support (the EBIOS module). The standard XT builds have it; the "tiny" build
  does not. [Checking the card's BIOS](#checking-the-cards-bios) shows how to find out.
  Without LBA, 4.0z falls back to CHS: everything still works, but only the first ~8 GB of
  the card is usable.
- **Memory:** 256 KB at least. 640 KB is recommended, because CHKDSK needs a lot of memory
  on large FAT16 drives.
- **Boot floppy:** `output/msdos4-360.img` for a 360 KB 5¼" drive. It has FDISK, FORMAT,
  SYS, CHKDSK, MEM, DEBUG, DISKINFO and the other essentials. SHARE.EXE is not needed.

### How big a card?

Each DOS drive is FAT16 and holds at most 2 GB, and one disk can have at most 24 drives
(C: to Z:), so DOS can use about 48 GB of any card. Every build is tested with all 24 drives on
a 49 GB disk image ([Testing with demu --boot](#testing-with-demu---boot)), and they were tried
on a 60 GB one in DOSBox-X. A bigger card works, but the space past the 24th drive is not used.

| Card | Usable space | What it needs |
|---|---|---|
| up to 8 GB | all of it, as up to 4 drives (C:–F:) | Nothing extra: FDISK and FORMAT on the 5150 |
| 16–32 GB | all of it, as up to 15 drives (C:–Q:) | LBA in the card's BIOS; the card set up on a PC with `dimg` |
| 64 GB | about 48 GB, using every letter from C: to Z: | The same, and no letters are left for a RAM disk |
| bigger | still about 48 GB | No benefit |

- Past about 8 GB the card's BIOS must support LBA, and 4.0z FDISK cannot reach that part of
  the card, so bigger cards are set up on a PC
  ([Setting up a card larger than 8 GB](#setting-up-a-card-larger-than-8-gb)).
- The 24-drive limit holds whatever the drive size: with 1 GB drives at most 24 GB is usable.
- Partitions after the 24th are ignored, so a card partitioned elsewhere with more of them is
  still safe to use.

For a 5150, a 4–8 GB card split into drives of a few hundred MB to 1 GB is the best fit. Any
FAT16 drive bigger than 128 MB has a FAT of 64 to 128 KB. CHKDSK, and the first DIR after
start-up (which counts free clusters), read all of it, which takes a moment on a 4.77 MHz 8088.
Cluster size grows with the drive (see [Choosing the FAT layout](#choosing-the-fat-layout)):
on a 2 GB drive every file takes at least 32 KB, so it fills up quickly with small files.

### Checking the card's BIOS

Start DOS on the 5150 from the floppy and run `DISKINFO`, which is on both floppy images. It
asks the BIOS about each hard disk - the geometry (INT 13h AH=08h), whether the extensions are
there (AH=41h) and the size by LBA (AH=48h):

```
A>diskinfo
DISKINFO - what the BIOS says about the hard disks

Drive 80h
  CHS:  1024 cylinders, 255 heads, 63 sectors per track: 8032 MB
  LBA:  yes, extensions version 21h - DOS 4.0z uses them for the whole disk
  Size: 30520 MB (62504960 sectors)
```

- **LBA: yes** - the card's BIOS has the INT 13h extensions, and 4.0z uses the whole card, up to
  about 48 GB.
- **LBA: no** - the BIOS has CHS only, and 4.0z reaches what the CHS line says, at most about
  8 GB: treat the card as a card of that size.
- **63 sectors per track** is the geometry `dimg` uses, so a card set up on a PC with `dimg`
  boots on this machine whatever the number of heads. DISKINFO says so when the BIOS reports
  anything else; then set the card up on the 5150 itself, with FDISK and FORMAT.

DISKINFO is not built by the 1988 toolchain: it is a small C program, `dos/CMD/DISKINFO`,
compiled by libdos's C compiler during the build.

### Which cards work

XT-CF adapters such as the XT-CF Lite talk to the card in 8-bit mode. The CompactFlash
standard requires every card to support that, but a few cards do not do it properly, and the
adapter then does not detect them. Fast UDMA "Extreme" and "Pro" camera cards, which most large
cards are, are the ones most often reported not to work. Industrial cards and plain older
cards are the safest choice. Try a card before you trust data to it.

### Setting up a card of up to 8 GB

1. Write `msdos4-360.img` to a 360 KB floppy and boot from it.
2. Run `FDISK`. Create the primary DOS partition, then an extended partition and logical
   drives in it. Answering "Y" to "use the maximum size" gives at most 2047 MB. FDISK can
   also work unattended, with sizes in MB:

   ```
   FDISK 1 /PRI:512 /EXT:65535 /LOG:512 /Q
   FDISK 1 /LOG:512 /Q
   ```

   The first line creates C:, an extended partition covering the rest of the disk and the
   first logical drive. Repeat the second line for more logical drives. A size larger than
   the free space takes all of it.
3. Restart from the floppy, then run `FORMAT C: /S` and `FORMAT D:` and so on.
4. Remove the floppy and start from the card.

### Setting up a card larger than 8 GB

4.0z FDISK cannot reach past about 8 GB, so the whole card is prepared on a PC with `dimg`
and written to the card in one go. This erases everything on the card.

1. Check that the card's BIOS supports LBA and uses 63 sectors per track
   ([Checking the card's BIOS](#checking-the-cards-bios)).
2. Put the card in a card reader on the PC and find its size in MB. The card is the entry
   whose media type is "Removable Media":

   ```powershell
   Get-CimInstance Win32_DiskDrive | Format-Table Index, Model, MediaType, @{n='MB'; e={[math]::Floor($_.Size / 1MB)}}
   ```

   The size shown is slightly less than the card really holds, which is what you want here.
3. Build DOS with `.\make-dos.ps1`, then make the card image. For example, for a 32 GB card
   that shows 30,520 MB:

   ```powershell
   output\dimg.exe -o card.img -b output\bin -hd 30500 -p 2047 -lba -l CARD -f output\bin\format.com -f output\bin\chkdsk.com -f output\bin\sys.com -f output\bin\mem.exe
   ```

   - `-hd` is the image size in MB and must not be more than the card's. For a card bigger
     than 48 GB use `-hd 49000`: that already holds all 24 drives, and the smaller image is
     quicker to write.
   - `-p` is the drive size in MB (at most 2047).
   - `-lba` gives the partitions types 0Eh and 0Fh, so DOS versions without LBA support skip
     them instead of misreading them.
   - Every drive is formatted. C: gets the system files and the `-f` files.
4. Optionally, check the image: `output\dimg.exe -dir card.img` lists every drive with its
   size and free space, and the files on C:. Or boot it in libdos's emulated 5150, with the
   INT 13h extensions an XT-IDE BIOS with LBA gives, and look at the last drive:

   ```powershell
   output\demu.exe --boot -c card.img --snapshot --expect "new date" --key enter --expect "new time" --key enter --expect "C>" --line "dir q:" --expect "C>" --screen
   ```

   Or try it in DOSBox-X: mount it with the `imgmount` line that `dimg` prints, then type
   `boot -l c`.
5. Write `card.img` to the card with a raw disk image writer such as Win32 Disk Imager. Make
   sure you select the card, because the whole device is overwritten. The full image is
   written, so a 30 GB image takes a while.
6. Put the card in the 5150 and start it. Copy any other files onto the card from Windows,
   which can read and write all these drives, or from the floppy.

C: boots whatever number of heads the card's BIOS reports, as long as it uses 63 sectors per
track (see [What still uses CHS](#what-still-uses-chs)). If it does not boot, start from the
floppy; all the drives still work.

### Cards prepared on another computer

- The card needs an MBR partition table with FAT12/FAT16 partitions of at most 2 GB (types
  01h, 04h, 06h, 0Eh; extended 05h, 0Fh). DOS 4.0z does not see FAT32 (0Bh/0Ch), exFAT or
  NTFS partitions. It also does not see cards formatted without a partition table
  ("superfloppy"), which some cameras and card readers produce.
- Data partitions may be anywhere on the card and may be formatted elsewhere, as long as
  their clusters are 32 KB or smaller. Layouts with extra reserved sectors or a single FAT,
  which Linux `mkfs.fat` can produce, are read correctly.
- To *boot* from the card, set C: up on the 5150 (FDISK and `FORMAT /S`) or with `dimg`. The
  boot code on a card prepared elsewhere often needs a newer CPU than the 8088, or expects a
  different geometry.
- Windows reads and writes these FAT16 partitions directly, which is a handy way to copy
  files onto the card.

## Testing with demu --boot

`test-boot.ps1` boots the build in libdos's emulated IBM PC 5150 (`demu --boot`): an 8088 at
4.77 MHz with 640 KB, whose BIOS serves disk images through INT 13h, with or without the
extensions, and which stops at any instruction an 8088 does not have. Each test waits for text
on the console and types commands, as a person at the keyboard would. The tests take seconds,
need no window, and CI runs them after every build. Build DOS with `.\make-dos.ps1` first.

| Test | What it checks |
|---|---|
| `floppy-360` | Boots `msdos4-360.img`; VER and CHKDSK |
| `floppy-1440` | Boots `msdos4-1440.img`; MEM and DIR |
| `hard-disk-chs` | A 100 MB disk, C: to F:, reached by CHS alone: no INT 13h extensions. Its last EBR links back to the first, and no G: appears |
| `chs-256-heads` | A BIOS that reports 256 heads: C: boots by CHS alone and CHKDSK reads it |
| `hard-disk-lba` | A 49 GB disk with 24 drives, C: to Z:, all past 8 GB reached by LBA, the EBRs linked with type 0Fh; CHKDSK Z: |
| `fdisk-format` | `FDISK 1 /PRI /EXT /LOG` on a blank disk, a restart, `FORMAT C: /S` and `FORMAT D:`, the result read back with `dimg -dir`, then booting from C: |
| `odd-fat` | Volumes laid out as Linux `mkfs.fat` makes them: C: with 8 reserved sectors, D: with 4 and a single FAT and `mkfs.fat`'s name in its boot sector. TYPE, COPY onto D: and CHKDSK, then the copy read back with `dimg -extract` |
| `diskinfo` | `DISKINFO` from the 360 KB floppy, asking a 49 GB disk's BIOS with the INT 13h extensions and without them |
| `fixes` | The bug fixes a program can see: `tests\testfix.c`, compiled by libdos's C compiler, provokes each through INT 21h on a 2 GB volume (AH=47h, a label search for CON, 5D00h, IOCTL 0863h, a write past cluster 65535, and FCB I/O on six files at once in a directory past sector 65535, without SHARE). CHKDSK checks the volume after, then SHARE is loaded - it must not say "already installed" - and DEBUG sends it an INT 2Fh call it does not know |
| `history` | COMMAND.COM's line editor and history: recalling with Up and Down, editing with the cursor keys, Backspace, Esc, F1 and F3, and, through `KEYS.COM` (which the script makes, to put the keys demu has no name for in the BIOS keyboard buffer), Home, End, Ins, Del and ^C. Then the history after DEBUG overwrites the transient, a second COMMAND reading its commands from a file, and the oldest lines going when the history is full |

```powershell
.\test-boot.ps1                        # all of them
.\test-boot.ps1 -Only hard-disk-lba    # just one
.\test-boot.ps1 -LibdosPath ..\libdos  # with a libdos checkout's demu and dimg
```

The images, and the console output of each test, are in `output\test`. A test is a `demu --boot`
command line, so one can be run by hand too:

```powershell
output\demu.exe --boot -a output\msdos4-360.img --snapshot --expect "new date" --key enter --expect "new time" --key enter --expect "A>" --line "dir" --expect "bytes free" --screen
```

- `-a` and `-b` are floppies, `-c` and `-d` hard disks. A hard disk has the geometry its
  partition table was made for (dimg's, or FDISK's), unless `--chs <heads>,<sectors>` gives
  another (up to 256 heads), and `--no-lba` takes the INT 13h extensions away.
- `--expect` waits for text on the console after what the last one matched. `--line` types a
  command and presses Enter, `--type` types without Enter, `--key` presses Enter, Esc, Tab,
  Backspace, an arrow or a function key, and `--reboot` restarts the machine with the disks as
  they now are.
- `--snapshot` leaves the images as they were; without it, what DOS writes goes into them.
- `--screen` prints the screen at the end and `--log <file>` writes everything the console
  showed.

## Testing in DOSBox

For trying things out by hand, `test-dosbox.ps1` makes a hard disk image with `dimg`, writes
`output\dosbox-dos4z.conf` and starts DOSBox with the image as the first hard disk. Build DOS
with `.\make-dos.ps1` first.

| Parameter | Default | Meaning |
|---|---|---|
| `-SizeMB` | 2000 | Disk size in MB |
| `-PartitionMB` | 1000 | Largest drive in MB (`dimg` caps it at 2047) |
| `-Lba` | off | Partition types 0Eh/0Fh, which need INT 13h extensions |
| `-Floppy` | off | Boot from `output\msdos4-1440.img` instead of from C: |
| `-New` | off | Make the disk image again even if it already exists |
| `-DosBox` | search | DOSBox or DOSBox-X to run. Without it, the usual install folders of DOSBox 0.74-3 and DOSBox-X are tried, then `PATH` |

The image is `output\hd-<size>mb.img` (`hd-<size>mb-lba.img` with `-Lba`). Its C: holds the
system files, an `AUTOEXEC.BAT`, CHKDSK, FDISK, FORMAT and MEM. The image is reused as
long as its size matches, so pass `-New` after rebuilding DOS to pick up the new files.

```powershell
.\test-dosbox.ps1                  # 2000 MB disk (C: and D:), boot from C:
.\test-dosbox.ps1 -Floppy          # boot from msdos4-1440.img, disk as C: and D:
.\test-dosbox.ps1 -SizeMB 500 -PartitionMB 100 -New   # 500 MB disk in 100 MB drives
```

**DOSBox 0.74-3** has no INT 13h extensions, so it tests the CHS path. It cannot use images
much larger than 2 GB, and it never shows 0Eh/0Fh partitions.

**DOSBox-X** has the extensions, so it tests the LBA path with disks of any size:

```powershell
.\test-dosbox.ps1 -SizeMB 12000 -Lba -DosBox C:\path\to\dosbox-x.exe
```

Setting `cputype=8086` in the `[cpu]` section of DOSBox-X emulates the 8086 instruction set,
a good check that nothing needs a newer CPU than the 5150's 8088. DOSBox-X can drop the last
write made to a disk image before its window closes. After changing an image, write one more
small file to it (for example `ECHO X>C:\FLUSH.TMP`) before closing.

To set things up by hand, `dimg` prints the matching `imgmount` line:

```
output\dimg.exe -o output\hd.img -b output\bin -hd 1000 -p 500
```

then in DOSBox:

```
imgmount 2 "C:\path\to\output\hd.img" -size 512,63,255,127 -fs none
boot -l c
```

## dimg hard disk images

dimg is part of [libdos](https://github.com/ZacWalk/libdos); every build of DOS puts the copy it
used in `output\dimg.exe`.

```
dimg -o <file> -b <bindir> -hd <MB> [-p <MB>] [-lba] [-heads <n>] [-nosys] [-l <label>] [-f <file> ...]
dimg -dir <image>
dimg -extract <image> <path> <file>
```

- C: is the active primary partition. The rest of the disk is an extended partition with
  logical drives of at most `-p` MB each (default and maximum 2047). Partitions end on
  cylinder boundaries, as FDISK makes them. At most 24 drives (C: to Z:) are made; any space
  after them stays unused inside the extended partition.
- Volumes are laid out exactly as DOS 4 FORMAT would lay them out, using the table above.
  C: gets IO.SYS, MSDOS.SYS and COMMAND.COM (unless `-nosys`) plus any `-f` files. The
  master boot record is FDISK's and the boot sectors are DOS 4's.
- The geometry is 63 sectors per track, with 16 heads up to 504 MB and 255 heads above that
  (`-heads` overrides it). An image written to a card boots on a machine whose BIOS also uses
  63 sectors per track (see [What still uses CHS](#what-still-uses-chs)).
- `-lba` uses types 0Eh/0Fh, which only LBA-capable BIOSes (and DOSBox-X) can use.
- The image file is sparse, so a 12 GB image takes very little real disk space.
- Files are dated with the time they are added, unless `-t <seconds since 1970>` or the
  `SOURCE_DATE_EPOCH` environment variable fixes it, which makes the same image every time.
- `-dir` lists the drives on any FAT12/FAT16 image - one dimg made, or one read off a card or
  formatted by DOS - and the files on each, and `-extract` copies a file out, for example
  `dimg -extract card.img D:\REPORT.TXT report.txt`.

## Where the code is

| File | What |
|---|---|
| `dos/BIOS/MSDISK.ASM` | `Is_LBA_Drive`, `LBA_RW`, `LBA_Xfer`: resident LBA read/write/verify; LBA conversion for IOCTL track calls |
| `dos/BIOS/MSINIT.ASM` | `Detect_LBA`, `Read_Abs_Sector`, `SETHARD`, `FIND_MINI_PARTITION`, `DiskTable2`: start-up detection and partition scanning; `COPYBPB`: layout from the boot sector; `SETDRVPARMS`: the BPB FORMAT starts from |
| `dos/CMD/FDISK/` | `CONVERT.C` size arithmetic, `SPACE.C` 2047 MB cap, `FDPARSE.C` switch ranges |
| `dos/CMD/FORMAT/FORMAT.ASM` | 32-bit track counts (`Phase2Initialisation` and the format loop) |
| `dos/CMD/DISKINFO/DISKINFO.C` | What the BIOS says about each hard disk; built with libdos's C compiler |
| `dos/DOS/FCBIO.ASM`, `FCBIO2.ASM` | `NslBits`, `SaveFCBInfo`, `FCBRegen`, `CheckFCB`, `$FCB_Open`: the directory sector's high bits in an FCB, for FCBs without SHARE |
| `disk/disk-image.cpp` in [libdos](https://github.com/ZacWalk/libdos) | `disk_build_hard_disk`: dimg's partitions and volumes |
