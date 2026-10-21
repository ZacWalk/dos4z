<#
.SYNOPSIS
    Boots the build in libdos's emulated IBM PC and checks that DOS works.

.DESCRIPTION
    Each test powers on an IBM PC 5150 (an 8088, 640 KB) with `demu --boot`,
    with the build's floppies or hard disk images made by dimg, then waits for
    text and types commands as a person would. A test fails if the text never
    comes, or if the processor meets an instruction an 8088 does not have.

    Run .\make-dos.ps1 first. The tests, their images and how to run one by
    hand are described in docs\LARGE-DISKS.md, "Testing with demu --boot".

.PARAMETER LibdosPath
    Use demu and dimg from a libdos checkout's exe\ instead of output\.

.PARAMETER Only
    Run just these tests.
#>
param(
    [string]$LibdosPath = "",
    [string[]]$Only = @()
)

$ErrorActionPreference = "Stop"
$doc = "docs\LARGE-DISKS.md, `"Testing with demu --boot`""
$out = Join-Path $PSScriptRoot "output"
$bin = Join-Path $out "bin"
$tools = if ($LibdosPath) { Join-Path (Resolve-Path $LibdosPath).Path "exe" } else { $out }
$demu = Join-Path $tools "demu.exe"
$dimg = Join-Path $tools "dimg.exe"
$floppy360 = Join-Path $out "msdos4-360.img"
$floppy1440 = Join-Path $out "msdos4-1440.img"
foreach ($file in $demu, $dimg, $floppy360, $floppy1440, (Join-Path $bin "io.sys")) {
    if (-not (Test-Path $file)) { throw "$file not found - build first: .\make-dos.ps1 (see $doc)" }
}
$work = Join-Path $out "test"
New-Item -ItemType Directory -Force -Path $work | Out-Null

# Runs demu --boot; the console's output goes to <name>.log in output\test.
function Invoke-Boot([string]$Name, [string[]]$Arguments) {
    $log = Join-Path $work "$Name.log"
    $output = & $demu --boot @Arguments --log $log 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "demu --boot exited $LASTEXITCODE (log: $log)`n$($output -join "`n")"
    }
}

function New-HardDisk([string]$Name, [string[]]$Arguments) {
    $image = Join-Path $work "$Name.img"
    $autoexec = Join-Path $work "AUTOEXEC.BAT"
    [IO.File]::WriteAllText($autoexec, "@ECHO OFF`r`nPROMPT `$P`$G`r`nVER`r`n")
    $files = @("-f", $autoexec)
    foreach ($f in "chkdsk.com", "mem.exe") { $files += @("-f", (Join-Path $bin $f)) }
    $output = & $dimg -o $image -b $bin -l DOS4Z @Arguments @files 2>&1
    if ($LASTEXITCODE -ne 0) { throw "dimg failed: $($output -join "`n")" }
    return $image
}

# KEYS.COM puts keystrokes in the BIOS keyboard buffer (40:1Eh, tail at 40:1Ch)
# for COMMAND.COM's line editor to read once KEYS has ended: the keys demu has
# no name for. Each character of its command line is a key, but ~X is the
# extended key with scan code X (~G Home, ~O End, ~R Ins, ~S Del, ~K Left, ~H
# Up) and ^X the control character (^M Enter, ^C). At most 15 keys fit.
function New-KeysCom {
    $path = Join-Path $work "KEYS.COM"
    [IO.File]::WriteAllBytes($path, [byte[]](
        0xBE, 0x81, 0x00,           #         mov  si,81h       ; the command line
        0xB8, 0x40, 0x00,           #         mov  ax,40h
        0x8E, 0xC0,                 #         mov  es,ax        ; the BIOS data area
        0xFC,                       #         cld
        0xAC,                       # skip:   lodsb
        0x3C, 0x20,                 #         cmp  al,' '
        0x74, 0xFB,                 #         je   skip
        0x3C, 0x0D,                 # next:   cmp  al,0Dh
        0x74, 0x2C,                 #         je   done
        0x32, 0xE4,                 #         xor  ah,ah        ; a character
        0x3C, 0x7E,                 #         cmp  al,'~'
        0x75, 0x07,                 #         jne  control
        0xAC,                       #         lodsb             ; ~X: scan code X
        0x8A, 0xE0,                 #         mov  ah,al
        0x32, 0xC0,                 #         xor  al,al
        0xEB, 0x07,                 #         jmp  short store
        0x3C, 0x5E,                 # control: cmp al,'^'
        0x75, 0x03,                 #         jne  store
        0xAC,                       #         lodsb             ; ^X: control X
        0x24, 0x1F,                 #         and  al,1Fh
        0xFA,                       # store:  cli
        0x26, 0x8B, 0x3E, 0x1C, 0x00, #       mov  di,es:[1Ch]  ; the buffer's tail
        0xAB,                       #         stosw
        0x83, 0xFF, 0x3E,           #         cmp  di,3Eh
        0x72, 0x03,                 #         jb   wrapped
        0xBF, 0x1E, 0x00,           #         mov  di,1Eh
        0x26, 0x89, 0x3E, 0x1C, 0x00, # wrapped: mov es:[1Ch],di
        0xFB,                       #         sti
        0xAC,                       #         lodsb
        0xEB, 0xD0,                 #         jmp  next
        0xCD, 0x20))                # done:   int  20h
    return $path
}

function Copy-Floppy {
    $copy = Join-Path $work "a.img"
    Copy-Item $floppy1440 $copy -Force
    return $copy
}

function Set-Le([byte[]]$Bytes, [int]$Offset, [long]$Value, [int]$Size) {
    for ($i = 0; $i -lt $Size; $i++) { $Bytes[$Offset + $i] = [byte](($Value -shr (8 * $i)) -band 0xFF) }
}

# Reads a sector of an image, lets $Change alter its bytes, and writes it back.
function Edit-Sector([string]$Path, [long]$Sector, [scriptblock]$Change) {
    $stream = [IO.File]::Open($Path, 'Open', 'ReadWrite')
    try {
        $bytes = New-Object byte[] 512
        $stream.Position = $Sector * 512
        [void]$stream.Read($bytes, 0, 512)
        & $Change $bytes
        $stream.Position = $Sector * 512
        $stream.Write($bytes, 0, 512)
    } finally {
        $stream.Close()
    }
}

# Where the EBRs of a dimg image are, in the order its extended partition (the
# MBR's second entry) links them.
function Get-EbrChain([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        $bytes = New-Object byte[] 512
        $read = { param([long]$at) $stream.Position = $at * 512; [void]$stream.Read($bytes, 0, 512) }
        & $read 0
        $base = [long][BitConverter]::ToUInt32($bytes, 0x1D6)
        $chain = @()
        for ($at = $base; $at -ne 0 -and $chain.Count -lt 64;) {
            $chain += $at
            & $read $at
            $linked = $bytes[0x1D2] -eq 0x05 -or $bytes[0x1D2] -eq 0x0F
            $at = if ($linked) { $base + [BitConverter]::ToUInt32($bytes, 0x1D6) } else { 0 }
        }
        return $chain
    } finally {
        $stream.Close()
    }
}

# A hard disk whose FAT16 volumes are laid out as no DOS FORMAT makes them, as
# Linux mkfs.fat can: C: with 8 reserved sectors, D: with 4 and a single FAT.
# D: has mkfs.fat's own name in its boot sector, so its BPB is trusted for
# describing a FAT volume, where C:'s "MSDOS5.0" is trusted for the name.
function New-OddFatDisk([string]$Path) {
    $spt = 63; $heads = 16; $cylinder = $spt * $heads
    $chs = {
        param([long]$lba)
        $c = [long][math]::Min([math]::Floor($lba / $cylinder), 1023); $r = $lba % $cylinder
        $h = [long][math]::Floor($r / $spt); $s = $r % $spt
        [byte[]]@($h, ((($c -shr 2) -band 0xC0) -bor ($s + 1)), ($c -band 0xFF))
    }
    $entry = {
        param([byte[]]$table, [int]$at, [int]$active, [int]$type, [long]$start, [long]$size)
        $table[$at] = $active
        [Array]::Copy((& $chs $start), 0, $table, $at + 1, 3)
        $table[$at + 4] = $type
        [Array]::Copy((& $chs ($start + $size - 1)), 0, $table, $at + 5, 3)
        Set-Le $table ($at + 8) $start 4
        Set-Le $table ($at + 12) $size 4
    }
    $stream = [IO.File]::Create($Path)
    try {
        $stream.SetLength(200L * $cylinder * 512)
        $put = { param([long]$sector, [byte[]]$bytes) $stream.Position = $sector * 512; $stream.Write($bytes, 0, $bytes.Length) }
        $volume = {
            param([long]$base, [long]$total, [int]$reserved, [int]$fats, [string]$name, [string]$text, [long]$serial,
                  [string]$oem)
            $spc = 4; $root = 512; $fat = 1
            while ($true) {
                $clusters = [math]::Floor(($total - $reserved - $fats * $fat - $root * 32 / 512) / $spc)
                $need = [math]::Floor((($clusters + 2) * 2 + 511) / 512)
                if ($need -le $fat) { break }
                $fat = $need
            }
            $boot = New-Object byte[] 512
            [Array]::Copy([byte[]](0xEB, 0x3C, 0x90), $boot, 3)
            [Array]::Copy([Text.Encoding]::ASCII.GetBytes($oem), 0, $boot, 3, 8)
            Set-Le $boot 11 512 2; $boot[13] = $spc; Set-Le $boot 14 $reserved 2; $boot[16] = $fats
            Set-Le $boot 17 $root 2; Set-Le $boot 19 $(if ($total -gt 0xFFFF) { 0 } else { $total }) 2
            $boot[21] = 0xF8; Set-Le $boot 22 $fat 2; Set-Le $boot 24 $spt 2; Set-Le $boot 26 $heads 2
            Set-Le $boot 28 $base 4; Set-Le $boot 32 $(if ($total -gt 0xFFFF) { $total } else { 0 }) 4
            $boot[36] = 0x80; $boot[38] = 0x29; Set-Le $boot 39 $serial 4
            [Array]::Copy([Text.Encoding]::ASCII.GetBytes("NO NAME    FAT16   "), 0, $boot, 43, 19)
            $boot[510] = 0x55; $boot[511] = 0xAA
            & $put $base $boot
            $table = New-Object byte[] ($fat * 512)
            [Array]::Copy([byte[]](0xF8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF), $table, 6)
            for ($i = 0; $i -lt $fats; $i++) { & $put ($base + $reserved + $i * $fat) $table }
            $rootSector = $base + $reserved + $fats * $fat
            $entryBytes = New-Object byte[] 32
            [Array]::Copy([Text.Encoding]::ASCII.GetBytes($name), 0, $entryBytes, 0, 11)
            $entryBytes[11] = 0x20; Set-Le $entryBytes 22 0x6000 2; Set-Le $entryBytes 24 0x5552 2
            Set-Le $entryBytes 26 2 2; Set-Le $entryBytes 28 $text.Length 4
            & $put $rootSector $entryBytes
            & $put ($rootSector + 32) ([Text.Encoding]::ASCII.GetBytes($text))
        }
        $mbr = New-Object byte[] 512
        & $entry $mbr 0x1BE 0x80 0x06 $spt (120 * $cylinder - $spt)
        & $entry $mbr 0x1CE 0x00 0x05 (120 * $cylinder) (80 * $cylinder)
        $mbr[510] = 0x55; $mbr[511] = 0xAA
        & $put 0 $mbr
        $ebr = New-Object byte[] 512
        & $entry $ebr 0x1BE 0x00 0x06 $spt (80 * $cylinder - $spt)
        $ebr[510] = 0x55; $ebr[511] = 0xAA
        & $put (120 * $cylinder) $ebr
        & $volume $spt (120 * $cylinder - $spt) 8 2 "HELLO   TXT" "HELLO FROM 8 RESERVED SECTORS`r`n" 0x11112222 "MSDOS5.0"
        & $volume (120 * $cylinder + $spt) (80 * $cylinder - $spt) 4 1 "WORLD   TXT" "WORLD FROM 1 FAT, 4 RESERVED`r`n" 0x33334444 "mkfs.fat"
    } finally {
        $stream.Close()
    }
}

# DOS asks for the date and time when a disk has no AUTOEXEC.BAT.
$dateAndTime = @("--expect", "new date", "--key", "enter", "--expect", "new time", "--key", "enter")

$tests = [ordered]@{
    "floppy-360" = {
        Invoke-Boot "floppy-360" (@("-a", $floppy360, "--snapshot") + $dateAndTime + @(
            "--expect", "A>", "--line", "ver", "--expect", "Version 4.0z",
            "--expect", "A>", "--line", "chkdsk", "--expect", "bytes available on disk",
            "--expect", "A>"))
    }
    "floppy-1440" = {
        Invoke-Boot "floppy-1440" (@("-a", $floppy1440, "--snapshot") + $dateAndTime + @(
            "--expect", "A>", "--line", "mem", "--expect", "largest executable program size",
            "--expect", "A>", "--line", "dir /w", "--expect", "bytes free", "--expect", "A>"))
    }
    "hard-disk-chs" = {
        # Four drives reached by cylinder, head and sector alone: no INT 13h
        # extensions. The last EBR links back to the first, as a damaged chain
        # can, and the drives do not get a second set of letters from it.
        $image = New-HardDisk "chs" @("-hd", "100", "-p", "32")
        $chain = @(Get-EbrChain $image)
        Edit-Sector $image $chain[-1] { param($b) $b[0x1D2] = 0x05; Set-Le $b 0x1D6 0 4; Set-Le $b 0x1DA 1008 4 }
        Invoke-Boot "hard-disk-chs" @("-c", $image, "--no-lba",
            "--expect", "Version 4.0z", "--expect", "C:\>", "--line", "dir f:", "--expect", "DRIVE F",
            "--expect", "C:\>", "--line", "dir g:", "--expect", "Invalid drive specification",
            "--expect", "C:\>", "--line", "chkdsk c:", "--expect", "bytes available on disk", "--expect", "C:\>")
        Remove-Item $image
    }
    "chs-256-heads" = {
        # A BIOS that reports 256 heads (DH=FFh), which MS-DOS 4.00 took for
        # none: C: boots by CHS alone, reading across tracks head by head.
        # dimg stops at 255 heads, but DOS finds the partitions by their LBA
        # fields, so only C:'s boot sector has to be told.
        $image = New-HardDisk "h256" @("-hd", "600", "-heads", "255", "-p", "300")
        Edit-Sector $image 63 { param($b) Set-Le $b 26 256 2 }
        Invoke-Boot "chs-256-heads" @("-c", $image, "--no-lba", "--chs", "256,63",
            "--expect", "Version 4.0z", "--expect", "C:\>", "--line", "chkdsk c:", "--expect", "bytes available on disk",
            "--expect", "C:\>", "--line", "dir d:", "--expect", "DRIVE D", "--expect", "C:\>")
        Remove-Item $image
    }
    "hard-disk-lba" = {
        # 24 drives, C: to Z:, on a 49 GB disk: everything past 8 GB by LBA.
        # The EBRs are linked with type 0Fh, as other partitioners write them
        # on an LBA disk, rather than dimg's 05h.
        $image = New-HardDisk "lba" @("-hd", "50000", "-p", "2047", "-lba")
        foreach ($ebr in @(Get-EbrChain $image)) {
            Edit-Sector $image $ebr { param($b) if ($b[0x1D2] -eq 0x05) { $b[0x1D2] = 0x0F } }
        }
        Invoke-Boot "hard-disk-lba" @("-c", $image,
            "--expect", "Version 4.0z", "--expect", "C:\>", "--line", "dir z:", "--expect", "DRIVE Z",
            "--expect", "C:\>", "--line", "chkdsk z:", "--expect", "bytes available on disk", "--expect", "C:\>")
        Remove-Item $image
    }
    "fdisk-format" = {
        # FDISK and FORMAT /S on a blank disk, then boot from it.
        $floppy = Copy-Floppy
        $disk = Join-Path $work "blank.img"
        $stream = [IO.File]::Create($disk); $stream.SetLength(406L * 16 * 63 * 512); $stream.Close()
        Invoke-Boot "fdisk-format" (@("-a", $floppy, "-c", $disk) + $dateAndTime + @(
            "--expect", "A>", "--line", "fdisk 1 /pri:60 /ext:140",
            "--expect", "A>", "--line", "fdisk 1 /log:70",
            "--expect", "A>", "--line", "fdisk 1 /log:70",
            "--expect", "A>", "--reboot") + $dateAndTime + @(
            "--expect", "A>", "--line", "format c: /s /v:fdisked", "--expect", "(Y/N)?", "--line", "y",
            "--expect", "System transferred", "--expect", "A>",
            "--line", "format d: /v:data", "--expect", "(Y/N)?", "--line", "y",
            "--expect", "Format complete", "--expect", "A>"))
        $listing = (& $dimg -dir $disk) -join "`n"
        foreach ($want in "Volume C: FDISKED, FAT16", "\IO.SYS", "\COMMAND.COM", "Volume D: DATA, FAT16") {
            if (-not $listing.Contains($want)) { throw "dimg -dir does not show '$want':`n$listing" }
        }
        Invoke-Boot "fdisk-format-boot" (@("-c", $disk) + $dateAndTime + @(
            "--expect", "C>", "--line", "dir", "--expect", "bytes free", "--expect", "C>"))
        Remove-Item $disk, $floppy
    }
    "odd-fat" = {
        # Reading and writing volumes with 8 reserved sectors and with a single FAT.
        $floppy = Copy-Floppy
        $disk = Join-Path $work "odd.img"
        New-OddFatDisk $disk
        Invoke-Boot "odd-fat" (@("-a", $floppy, "-c", $disk) + $dateAndTime + @(
            "--expect", "A>", "--line", "type c:\hello.txt", "--expect", "HELLO FROM 8 RESERVED SECTORS",
            "--expect", "A>", "--line", "type d:\world.txt", "--expect", "WORLD FROM 1 FAT, 4 RESERVED",
            "--expect", "A>", "--line", "copy c:\hello.txt d:\copy.txt", "--expect", "1 File(s) copied",
            "--expect", "A>", "--line", "chkdsk d:", "--expect", "bytes available on disk", "--expect", "A>"))
        $copy = Join-Path $work "copy.txt"
        & $dimg -extract $disk "D:\COPY.TXT" $copy
        if ($LASTEXITCODE -ne 0 -or (Get-Content $copy -Raw) -ne "HELLO FROM 8 RESERVED SECTORS`r`n") {
            throw "D:\COPY.TXT did not come back from the one-FAT volume as written"
        }
        Remove-Item $disk, $floppy, $copy
    }
    "diskinfo" = {
        # DISKINFO from the 360 KB floppy, asking a 49 GB disk's BIOS with the
        # extensions and then without them, as a CHS-only XT-IDE BIOS would.
        $image = New-HardDisk "diskinfo" @("-hd", "50000", "-p", "2047", "-lba")
        Invoke-Boot "diskinfo-lba" (@("-a", $floppy360, "-c", $image, "--snapshot") + $dateAndTime + @(
            "--expect", "A>", "--line", "diskinfo", "--expect", "LBA:  yes", "--expect", "Size: 49999 MB",
            "--expect", "A>"))
        Invoke-Boot "diskinfo-chs" (@("-a", $floppy360, "-c", $image, "--snapshot", "--no-lba", "--chs", "16,63") +
            $dateAndTime + @("--expect", "A>", "--line", "diskinfo", "--expect", "63 sectors per track: 504 MB",
            "--expect", "LBA:  no", "--expect", "A>"))
        Remove-Item $image
    }
    "fixes" = {
        # The bug fixes a program can see. TESTFIX (tests\testfix.c, compiled
        # here by libdos's C compiler) provokes each through INT 21h, with C: a
        # 2 GB volume, among them FCB I/O there, which needed SHARE in MS-DOS
        # 4.00: SHARE.EXE is on the floppy, but nothing loads it until the
        # SHARE line, so it would say "already installed" had IO.SYS loaded it.
        # CHKDSK then finds C: as TESTFIX left it, and DEBUG sends SHARE an
        # INT 2Fh call it does not know, which used to hang the machine.
        $testfix = Join-Path $work "TESTFIX.COM"
        $output = & $demu --cc (Join-Path $PSScriptRoot "tests\testfix.c") -o $testfix 2>&1
        if ($LASTEXITCODE -ne 0) { throw "tests\testfix.c does not compile: $($output -join "`n")" }
        $floppy = Join-Path $work "testfix.img"
        $files = @("-f", $testfix)
        foreach ($f in "share.exe", "debug.com", "chkdsk.com") { $files += @("-f", (Join-Path $bin $f)) }
        $output = & $dimg -o $floppy -b $bin -s 1440 @files 2>&1
        if ($LASTEXITCODE -ne 0) { throw "dimg failed: $($output -join "`n")" }
        $disk = New-HardDisk "fixes" @("-hd", "2100", "-p", "2047", "-nosys")
        Invoke-Boot "fixes" (@("-a", $floppy, "-c", $disk) + $dateAndTime + @(
            "--expect", "A>", "--line", "testfix", "--expect", "TESTFIX:", "--expect", " passed, 0 failed", "--expect", "A>",
            "--line", "chkdsk c:", "--expect", "bytes available on disk", "--expect", "A>",
            "--line", "share", "--expect", "A>",
            "--line", "debug", "--expect", "-", "--line", "a 100", "--line", "mov ax,1001", "--line", "int 2f",
            "--line", "int 3", "--key", "enter", "--line", "g=100", "--expect", "AX=", "--line", "q",
            "--expect", "A>"))
        $log = [IO.File]::ReadAllText((Join-Path $work "fixes.log"))
        foreach ($text in "already installed", "lost allocation units", "Errors found") {
            if ($log.Contains($text)) { throw "fixes.log says '$text'" }
        }
        Remove-Item $disk, $floppy, $testfix
    }
    "history" = {
        # COMMAND.COM's line editor and history. demu presses the keys it has
        # names for, and KEYS.COM the others. Each line run is VER unless an
        # edit went wrong, so the log must show "Version 4.0z" once for each
        # step that expects it and never "Bad command or file name". Between
        # them: DEBUG overwrites the transient, which COMMAND loads again from
        # the floppy, and a second COMMAND reads its commands from CMDS.TXT.
        # Last, twelve 24-character REM lines fill the history's 256 bytes, so
        # Up cannot reach a VER line any more.
        $keys = New-KeysCom
        $commands = Join-Path $work "CMDS.TXT"
        [IO.File]::WriteAllText($commands, "ver`r`nexit`r`n")
        $floppy = Join-Path $work "history.img"
        $output = & $dimg -o $floppy -b $bin -s 1440 -f $keys -f $commands -f (Join-Path $bin "debug.com") 2>&1
        if ($LASTEXITCODE -ne 0) { throw "dimg failed: $($output -join "`n")" }
        $ver = @("--expect", "Version 4.0z", "--expect", "A>")
        $vol = @("--expect", "Volume in drive A", "--expect", "A>")
        $steps = @("-a", $floppy) + $dateAndTime + @("--expect", "A>",
            "--line", "ver") + $ver + @("--line", "vol") + $vol + @(
            "--key", "up", "--key", "up", "--key", "enter") + $ver + @(
            "--key", "up", "--key", "up", "--key", "up", "--key", "down", "--key", "down", "--key", "enter") + $ver + @(
            "--key", "up", "--key", "down", "--line", "ver") + $ver + @(
            "--type", "verx", "--key", "backspace", "--key", "enter") + $ver + @(
            "--type", "vxr", "--key", "left", "--key", "left", "--type", "e", "--key", "enter") + $ver + @(
            "--type", "garbage", "--key", "esc", "--line", "ver") + $ver + @(
            "--key", "f3", "--key", "enter") + $ver + @(
            "--type", "v", "--key", "f1", "--key", "right", "--key", "enter") + $ver + @(
            "--line", "keys xver~G~S^M") + $ver + @(
            "--line", "keys vr~K~Re^M") + $ver + @(
            "--line", "keys ve~G~Or^M") + $ver + @(
            "--type", "vol", "--key", "tab", "--line", "a:") + $vol + @(
            "--line", "keys garbage^C", "--expect", "keys garbage^C", "--expect", "garbage^C", "--expect", "A>",
            "--line", "ver") + $ver + @(
            "--line", "vol") + $vol + @("--line", "vol") + $vol + @(
            "--key", "up", "--key", "up", "--key", "enter") + $ver + @(
            "--line", "debug", "--expect", "-", "--line", "f 9000:0 ffff 0", "--expect", "-", "--line", "q",
            "--expect", "A>", "--key", "up", "--key", "up", "--key", "enter") + $ver + @(
            "--line", "command < cmds.txt", "--expect", "Version 4.0z") + $ver
        for ($i = 10; $i -lt 22; $i++) { $steps += @("--line", "rem $i abcdefghijklmnopq", "--expect", "A>") }
        for ($i = 0; $i -lt 12; $i++) { $steps += @("--key", "up") }
        $steps += @("--key", "enter", "--expect", "A>", "--line", "vol") + $vol
        Invoke-Boot "history" $steps
        $log = [IO.File]::ReadAllText((Join-Path $work "history.log"))
        if ($log.Contains("Bad command or file name")) { throw "history.log says 'Bad command or file name'" }
        $expected = @($steps | Where-Object { $_ -eq "Version 4.0z" }).Count + 1
        $count = ([regex]::Matches($log, "Version 4\.0z")).Count
        if ($count -ne $expected) { throw "history.log says 'Version 4.0z' $count times, not $expected" }
        Remove-Item $floppy, $keys, $commands
    }
}

$failed = @()
foreach ($name in $tests.Keys) {
    if ($Only.Count -gt 0 -and $Only -notcontains $name) { continue }
    $timer = [Diagnostics.Stopwatch]::StartNew()
    try {
        & $tests[$name]
        Write-Host ("  PASS  {0,-14} {1,5:N1} s" -f $name, $timer.Elapsed.TotalSeconds) -ForegroundColor Green
    } catch {
        Write-Host ("  FAIL  {0,-14} {1}" -f $name, $_.Exception.Message) -ForegroundColor Red
        $failed += $name
    }
}
if ($failed.Count -gt 0) {
    Write-Host "$($failed.Count) test(s) failed: $($failed -join ', ') - the console logs are in $work" -ForegroundColor Red
    exit 1
}
Write-Host "All boot tests passed" -ForegroundColor Green
