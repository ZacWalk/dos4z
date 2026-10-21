<#
.SYNOPSIS
    Boot MS-DOS 4.0z in DOSBox with a partitioned hard disk image.

.DESCRIPTION
    Makes a hard disk image with dimg and starts DOSBox or DOSBox-X with it. The
    parameters, the image it makes, which DOSBox tests what, and examples are described
    in docs\LARGE-DISKS.md, section "Testing in DOSBox". Run .\make-dos.ps1 first.

.LINK
    docs\LARGE-DISKS.md
#>
param(
    [int]$SizeMB = 2000,
    [int]$PartitionMB = 1000,
    [switch]$Lba,
    [switch]$Floppy,
    [switch]$New,
    [string]$DosBox
)

$ErrorActionPreference = "Stop"
$doc = "docs\LARGE-DISKS.md, `"Testing in DOSBox`""
$out = Join-Path $PSScriptRoot "output"
$dimg = Join-Path $out "dimg.exe"
$bin = Join-Path $out "bin"
$floppyImg = Join-Path $out "msdos4-1440.img"
if (-not (Test-Path (Join-Path $bin "io.sys")) -or -not (Test-Path $floppyImg)) {
    throw "Build first: .\make-dos.ps1"
}

if (-not $DosBox) {
    $found = @("${env:ProgramFiles(x86)}\DOSBox-0.74-3\DOSBox.exe", "$env:ProgramFiles\DOSBox-X\dosbox-x.exe") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $found) {
        $cmd = Get-Command dosbox-x.exe, dosbox.exe -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($cmd) { $found = $cmd.Source }
    }
    if (-not $found) { throw "DOSBox not found; pass -DosBox <path to DOSBox.exe or dosbox-x.exe> (see $doc)" }
    $DosBox = $found
}
$isX = (Split-Path -Leaf $DosBox) -match 'dosbox-x'
if (-not $isX -and $SizeMB -gt 2000) { Write-Warning "DOSBox 0.74-3 cannot use images much larger than 2 GB; use DOSBox-X (see $doc)." }
if (-not $isX -and $Lba) { Write-Warning "DOSBox 0.74-3 has no INT 13h extensions, so DOS will not see 0Eh/0Fh partitions (see $doc)." }

# Same geometry as dimg -hd: 63 sectors/track, 16 heads up to 504 MB, else 255
$heads = if ($SizeMB -le 504) { 16 } else { 255 }
$cylinders = [math]::Floor($SizeMB * 2048 / ($heads * 63))
$bytes = [long]$cylinders * $heads * 63 * 512
$name = if ($Lba) { "hd-${SizeMB}mb-lba.img" } else { "hd-${SizeMB}mb.img" }
$img = Join-Path $out $name

if ($New -or -not (Test-Path $img) -or (Get-Item $img).Length -ne $bytes) {
    $tmp = New-Item -ItemType Directory -Force (Join-Path $out "tmp")
    $autoexec = Join-Path $tmp "AUTOEXEC.BAT"
    [IO.File]::WriteAllText($autoexec, "@ECHO OFF`r`nPROMPT `$P`$G`r`nVER`r`n")
    $dimgArgs = @("-o", $img, "-b", $bin, "-hd", $SizeMB, "-p", $PartitionMB, "-l", "DOS4Z", "-f", $autoexec)
    if ($Lba) { $dimgArgs += "-lba" }
    foreach ($f in "chkdsk.com", "fdisk.exe", "format.com", "mem.exe") {
        $dimgArgs += @("-f", (Join-Path $bin $f))
    }
    & $dimg @dimgArgs
    if ($LASTEXITCODE -ne 0) { throw "dimg failed" }
}

$boot = if ($Floppy) { "boot `"$floppyImg`" -l a" } else { "boot -l c" }
$conf = Join-Path $out "dosbox-dos4z.conf"
@"
[sdl]
autolock=false
[autoexec]
imgmount 2 "$img" -size 512,63,$heads,$cylinders -fs none
$boot
"@ | Set-Content -Path $conf -Encoding ASCII

Write-Host "Starting $(Split-Path -Leaf $DosBox) with $name ($cylinders cylinders, $heads heads, 63 sectors)"
Start-Process -FilePath $DosBox -ArgumentList @("-conf", "`"$conf`"") -WorkingDirectory $out
