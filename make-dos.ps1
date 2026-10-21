# make-dos.ps1 — Build MS-DOS 4.0 with its original 1988 toolchain, run by demu
#
# demu, which runs the 16-bit tools, and dimg, which makes the disk images,
# come from libdos (https://github.com/ZacWalk/libdos): the release pinned
# below is downloaded once into output/tools/ and checked against its SHA-256,
# or -LibdosPath takes them from a libdos checkout's exe/ instead.
#
#   output/demu.exe    — the DOS emulator (from libdos)
#   output/dimg.exe    — the disk image tool (from libdos)
#   output/tools/      — the downloaded libdos release
#   output/logs/       — per-component build logs
#   output/bin/        — final DOS 4.0 binaries
#
# Usage:
#   .\make-dos.ps1                     Build everything
#   .\make-dos.ps1 -Clean              Remove build artifacts and rebuild
#   .\make-dos.ps1 -Component bios     Build only a specific component
#   .\make-dos.ps1 -Verbose            Show full build output
#   .\make-dos.ps1 -LibdosPath ..\libdos   Use a libdos checkout's demu and dimg
#
# Components (in build order):
#   messages, mapper, boot, bios, dos, cmd, dev, select, memm, diskinfo

param(
    [switch]$Clean,
    [string]$Component = "",
    [switch]$Verbose,
    [string]$LibdosPath = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$srcDir = Join-Path $root "dos"
$toolsDir = Join-Path $srcDir "TOOLS"
$outputDir = Join-Path $root "output"
$demuExe = Join-Path $outputDir "demu.exe"
$dimgExe = Join-Path $outputDir "dimg.exe"
$logDir = Join-Path $outputDir "logs"
$binDir = Join-Path $outputDir "bin"

# The libdos release that provides demu and dimg, and the SHA-256 of its zip.
# Change both together.
$libdosVersion = "v1.1.1"
$libdosSha256 = "AC61F11CCC3A6C66E301D8F5D90CA8F4B23F961839EA9785EBE3EE1A7062752A"

$components = @("messages", "mapper", "boot", "bios", "dos", "cmd", "dev", "select", "memm", "diskinfo")

# ── Helpers ──────────────────────────────────────────────────────────

function Write-Step($msg) {
    Write-Host "`n=== $msg ===" -ForegroundColor Cyan
}

function Write-Ok($msg) {
    Write-Host "  OK: $msg" -ForegroundColor Green
}

function Write-Fail($msg) {
    Write-Host "  FAIL: $msg" -ForegroundColor Red
}

function Get-ComponentDir($name) {
    $sub = switch ($name) {
        "messages" { "MESSAGES" }
        "mapper"   { "MAPPER" }
        "boot"     { "BOOT" }
        "bios"     { "BIOS" }
        "dos"      { "DOS" }
        "cmd"      { "CMD" }
        "dev"      { "DEV" }
        "select"   { "SELECT" }
        "memm"     { "MEMM" }
        "diskinfo" { "CMD\DISKINFO" }
        default    { throw "Unknown component: $name" }
    }
    return Join-Path $srcDir $sub
}

# ── Step 1: Get demu and dimg from libdos ───────────────────────────

function Get-LibdosTools {
    Write-Step "Getting demu and dimg (libdos)"
    if ($LibdosPath) {
        $from = Join-Path (Resolve-Path $LibdosPath).Path "exe"
        $what = "the libdos checkout in $LibdosPath"
    } else {
        $from = Join-Path $outputDir "tools\libdos-$libdosVersion"
        $what = "libdos $libdosVersion"
        if (-not (Test-Path (Join-Path $from "demu.exe")) -or -not (Test-Path (Join-Path $from "dimg.exe"))) {
            $zipName = "libdos-$libdosVersion-win-x64.zip"
            $zip = Join-Path $outputDir "tools\$zipName"
            New-Item -ItemType Directory -Path (Split-Path $zip) -Force | Out-Null
            [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
            $url = "https://github.com/ZacWalk/libdos/releases/download/$libdosVersion/$zipName"
            Write-Host "  Downloading $url"
            Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
            $hash = (Get-FileHash $zip -Algorithm SHA256).Hash
            if ($hash -ne $libdosSha256) {
                Remove-Item $zip -Force
                throw "$zipName has SHA-256 $hash, not the pinned $libdosSha256"
            }
            Expand-Archive -Path $zip -DestinationPath $from -Force
            Remove-Item $zip -Force
        }
    }
    foreach ($exe in "demu.exe", "dimg.exe") {
        $path = Join-Path $from $exe
        if (-not (Test-Path $path)) {
            $hint = if ($LibdosPath) { " - build libdos first: .\dd.ps1 build release" } else { "" }
            throw "$exe not found in $from$hint"
        }
        Copy-Item $path $outputDir -Force
    }
    Write-Ok "demu.exe and dimg.exe from $what"
}

# ── Step 2: Set up environment ──────────────────────────────────────

function Set-BuildEnvironment {
    Write-Step "Setting build environment"
    $env:CL = ""
    $env:LINK = ""
    $env:MASM = ""
    $env:COUNTRY = "usa-ms"
    $env:INIT = $toolsDir
    $env:LIB = Join-Path $toolsDir "bld\lib"
    $env:INCLUDE = Join-Path $toolsDir "bld\inc"
    $env:PATH = "$outputDir;$toolsDir;$([Environment]::GetFolderPath('System'))"
    # Use a short project-local temp directory so MS C 5.10's fixed-size
    # path buffers don't overflow, and clean it to avoid stale temp files
    # from previous builds colliding with new compilations.
    $tmpDir = Join-Path $outputDir "tmp"
    New-Item -ItemType Directory -Path $tmpDir -Force | Out-Null
    Remove-Item (Join-Path $tmpDir "*") -Force -ErrorAction SilentlyContinue
    $env:TMP = $tmpDir
    $env:TEMP = $tmpDir
    Write-Ok "Environment configured"
}

# ── Step 3: Build components ────────────────────────────────────────

function Invoke-Nmake($componentDir, $componentName) {
    Write-Step "Building $componentName"

    if (-not (Test-Path $componentDir)) {
        Write-Fail "Directory not found: $componentDir"
        return $false
    }

    $logFile = Join-Path $logDir "$componentName.log"
    New-Item -ItemType Directory -Path $logDir -Force | Out-Null

    Push-Location $componentDir
    try {
        # -r: the repository is the emulated drive C:, wherever it is checked out
        $output = & $demuExe -r $root (Join-Path $toolsDir "NMAKE.EXE") 2>&1 | Out-String
        $exitCode = $LASTEXITCODE

        $output | Out-File -FilePath $logFile -Encoding ASCII

        if ($Verbose) {
            Write-Host $output
        }

        if ($exitCode -ne 0) {
            $lines = $output -split "`n" | Select-Object -Last 20
            foreach ($line in $lines) { Write-Host "  $line" -ForegroundColor Yellow }
            Write-Fail "$componentName build failed (exit code $exitCode) — see $logFile"
            return $false
        }

        Write-Ok "$componentName built successfully"
        return $true
    } finally {
        Pop-Location
    }
}

function Build-Component($name) {
    if ($name -eq "cmd") {
        return Build-CmdSubdirs
    }
    if ($name -eq "diskinfo") {
        return Build-DiskInfo
    }
    # SELECT needs usa-ms.idx from the messages component
    if ($name -eq "select") {
        $msgIdx = Join-Path $srcDir "MESSAGES\usa-ms.idx"
        if (-not (Test-Path $msgIdx)) {
            Invoke-Nmake (Get-ComponentDir "messages") "messages" | Out-Null
        }
    }
    return Invoke-Nmake (Get-ComponentDir $name) $name
}

# DISKINFO.COM is new in 4.0z and is not built by the 1988 toolchain:
# libdos's C compiler (demu --cc) turns its one C file into a .COM.
function Build-DiskInfo {
    Write-Step "Building diskinfo"
    $dir = Get-ComponentDir "diskinfo"
    $logFile = Join-Path $logDir "diskinfo.log"
    New-Item -ItemType Directory -Path $logDir -Force | Out-Null
    $output = & $demuExe --cc (Join-Path $dir "DISKINFO.C") -o (Join-Path $dir "DISKINFO.COM") 2>&1 | Out-String
    $exitCode = $LASTEXITCODE
    $output | Out-File -FilePath $logFile -Encoding ASCII
    if ($Verbose) { Write-Host $output }
    if ($exitCode -ne 0) {
        Write-Host "  $output" -ForegroundColor Yellow
        Write-Fail "diskinfo build failed (exit code $exitCode) — see $logFile"
        return $false
    }
    Write-Ok "diskinfo built successfully"
    return $true
}

# Build each CMD subdirectory individually to avoid deep exec nesting in demu.
# The original CMD/MAKEFILE uses cd+nmake pairs which causes 3-4 levels of
# nested EXEC in demu, leading to memory pressure and C compiler ICEs.
function Build-CmdSubdirs {
    Write-Step "Building cmd"

    $cmdDir = Get-ComponentDir "cmd"
    $logFile = Join-Path $logDir "cmd.log"
    New-Item -ItemType Directory -Path $logDir -Force | Out-Null
    "" | Out-File -FilePath $logFile -Encoding ASCII

    # Build order matches CMD/MAKEFILE
    $cmdSubs = @(
        "COMMAND", "ATTRIB", "ASSIGN", "CHKDSK", "DEBUG", "EDLIN",
        "EXE2BIN", "FIND", "FC", "FORMAT", "JOIN", "MORE", "PRINT",
        "RECOVER", "SORT", "SUBST", "SYS", "SHARE", "NLSFUNC",
        "IFSFUNC", "MEM", "FILESYS", "BACKUP", "COMP", "DISKCOMP",
        "DISKCOPY", "FDISK", "LABEL", "MODE", "RESTORE", "TREE",
        "REPLACE", "XCOPY", "GRAFTABL", "FASTOPEN", "APPEND",
        "GRAPHICS", "KEYB"
    )

    $anyFail = $false
    foreach ($sub in $cmdSubs) {
        $subDir = Join-Path $cmdDir $sub
        if (-not (Test-Path $subDir)) { continue }
        if (-not (Test-Path (Join-Path $subDir "MAKEFILE"))) { continue }

        Push-Location $subDir
        try {
            $output = & $demuExe -r $root (Join-Path $toolsDir "NMAKE.EXE") 2>&1 | Out-String
            $exitCode = $LASTEXITCODE

            "=== $sub ===" | Out-File -FilePath $logFile -Append -Encoding ASCII
            $output | Out-File -FilePath $logFile -Append -Encoding ASCII

            if ($Verbose) { Write-Host $output }

            if ($exitCode -ne 0) {
                Write-Host "  warn: cmd/$sub failed (exit code $exitCode)" -ForegroundColor Yellow
                $anyFail = $true
            }
        } finally {
            Pop-Location
        }
    }

    if ($anyFail) {
        Write-Fail "cmd build had failures — see $logFile"
        return $false
    }
    Write-Ok "cmd built successfully"
    return $true
}

# ── Step 4: Collect outputs ─────────────────────────────────────────

function Copy-Outputs {
    Write-Step "Collecting build outputs to output/bin/"
    # A full build starts from an empty bin/ so a component that fails to
    # build can't hide behind a stale binary from an earlier run.
    if (-not $Component -and (Test-Path $binDir)) {
        Remove-Item (Join-Path $binDir "*") -Force -ErrorAction SilentlyContinue
    }
    New-Item -ItemType Directory -Path $binDir -Force | Out-Null

    $outputs = @(
        "bios\io.sys",
        "dos\msdos.sys",
        "boot\MSBOOT.BIN",
        "cmd\fdisk\fdboot.bin",
        "cmd\append\append.exe",
        "cmd\assign\assign.com",
        "cmd\attrib\attrib.exe",
        "cmd\backup\backup.com",
        "cmd\chkdsk\chkdsk.com",
        "cmd\command\command.com",
        "cmd\comp\comp.com",
        "cmd\debug\debug.com",
        "cmd\diskcomp\diskcomp.com",
        "cmd\diskcopy\diskcopy.com",
        "cmd\diskinfo\diskinfo.com",
        "cmd\edlin\edlin.com",
        "cmd\exe2bin\exe2bin.exe",
        "cmd\fastopen\fastopen.exe",
        "cmd\filesys\filesys.exe",
        "cmd\fdisk\fdisk.exe",
        "cmd\find\find.exe",
        "cmd\fc\fc.exe",
        "cmd\format\format.com",
        "cmd\graftabl\graftabl.com",
        "cmd\graphics\graphics.com",
        "cmd\graphics\graphics.pro",
        "cmd\ifsfunc\ifsfunc.exe",
        "cmd\join\join.exe",
        "cmd\label\label.com",
        "cmd\mem\mem.exe",
        "cmd\mode\mode.com",
        "cmd\more\more.com",
        "cmd\nlsfunc\nlsfunc.exe",
        "cmd\print\print.com",
        "cmd\recover\recover.com",
        "cmd\replace\replace.exe",
        "cmd\restore\restore.com",
        "select\select.exe",
        "select\select.com",
        "select\select.dat",
        "select\select.prt",
        "select\select.hlp",
        "cmd\share\share.exe",
        "cmd\sort\sort.exe",
        "cmd\sys\sys.com",
        "cmd\subst\subst.exe",
        "cmd\tree\tree.com",
        "cmd\xcopy\xcopy.exe",
        "cmd\keyb\keyb.com",
        "dev\ansi\ansi.sys",
        "dev\country\country.sys",
        "dev\display\display.sys",
        "dev\display\ega\ega.cpi",
        "dev\display\lcd\lcd.cpi",
        "dev\keyboard\keyboard.sys",
        "dev\printer\printer.sys",
        "dev\printer\4201\4201.cpi",
        "dev\printer\4208\4208.cpi",
        "dev\printer\5202\5202.cpi",
        "dev\smartdrv\smartdrv.sys",
        "dev\ramdrive\ramdrive.sys",
        "dev\driver\driver.sys",
        "dev\xma2ems\xma2ems.sys",
        "dev\xmaem\xmaem.sys",
        "memm\memm\emm386.sys"
    )

    $copied = 0
    $missing = @()
    foreach ($rel in $outputs) {
        $src = Join-Path $srcDir $rel
        if (Test-Path $src) {
            Copy-Item $src (Join-Path $binDir (Split-Path $rel -Leaf)) -Force
            $copied++
        } else {
            $missing += $rel
        }
    }

    Write-Ok "$copied files copied to output/bin/ ($($missing.Count) not found)"
    # After a single-component build the other components' outputs are
    # expected to be missing (bin/ keeps them from the last full build).
    if (-not $Component) {
        foreach ($rel in $missing) { Write-Host "  Missing: $rel" -ForegroundColor DarkYellow }
    }
}

# ── Step 5: Clean in-tree intermediates ─────────────────────────────

function Clean-IntreeArtifacts {
    Write-Step "Cleaning in-tree build intermediates from dos/"
    $cleanExts = @("*.obj","*.exe","*.com","*.sys","*.bin","*.lib","*.lst",
                   "*.cl1","*.cl2","*.cl3","*.cl4","*.cl5","*.clf","*.ctl",
                   "*.map","*.tmp")
    # Binaries shipped with the original source release; never delete them.
    $keep = @("CMD\APPEND\SYSMAC.LIB", "DEV\XMAEM\INDEXMAE.SYS", "INC\COMSUBS.LIB",
              "LIB\MEM.LIB", "SELECT\CASSFAR.LIB") |
        ForEach-Object { (Join-Path $srcDir $_).ToUpperInvariant() }
    $removed = 0

    $dirs = @($components | ForEach-Object { Get-ComponentDir $_ })
    foreach ($dir in $dirs + (Join-Path $srcDir "INC")) {
        if (-not (Test-Path $dir)) { continue }
        $recurse = $dir -ne (Join-Path $srcDir "INC")
        foreach ($ext in $cleanExts) {
            $files = Get-ChildItem $dir -Recurse:$recurse -Filter $ext -File -ErrorAction SilentlyContinue |
                Where-Object { $keep -notcontains $_.FullName.ToUpperInvariant() }
            $removed += @($files).Count
            $files | Remove-Item -Force -ErrorAction SilentlyContinue
        }
    }

    $bootInc = Join-Path $srcDir "INC\BOOT.INC"
    if (Test-Path $bootInc) { Remove-Item $bootInc -Force; $removed++ }

    Write-Ok "Removed $removed intermediate files from dos/"
}

# ── Clean (full reset) ──────────────────────────────────────────────

function Clean-All {
    Write-Step "Cleaning all build artifacts"
    # A demu an interrupted build left running holds its files open. Only this
    # build's copy is stopped: demu is libdos's tool and may be in use elsewhere.
    Get-Process demu -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq $demuExe } | Stop-Process -Force
    Clean-IntreeArtifacts
    # Remove build artifacts; output/tools, the libdos download, stays
    foreach ($sub in @("logs", "bin", "tmp")) {
        $p = Join-Path $outputDir $sub
        if (Test-Path $p) { Remove-Item $p -Recurse -Force }
    }
    # Remove disk images
    Get-ChildItem $outputDir -Filter *.img -File -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue
    Write-Ok "Clean complete"
}

# ── Step 6: Create disk images ──────────────────────────────────────

function New-DiskImage {
    Write-Step "Creating boot disk images"

    if (-not (Test-Path $dimgExe)) {
        Write-Fail "dimg.exe not found — skipping disk images"
        return
    }

    # Check required files exist
    $required = @("MSBOOT.BIN", "io.sys", "msdos.sys", "command.com")
    $missing = @()
    foreach ($f in $required) {
        if (-not (Test-Path (Join-Path $binDir $f))) { $missing += $f }
    }
    if ($missing.Count -gt 0) {
        Write-Fail "Missing files for disk image: $($missing -join ', ')"
        return
    }

    # 360KB disk — the essential utilities. SHARE.EXE is left for the
    # 1.44MB disk: DOS no longer needs it for partitions over 32 MB.
    $img360 = Join-Path $outputDir "msdos4-360.img"
    $utils360 = @(
        "fdisk.exe", "format.com", "chkdsk.com", "sys.com",
        "debug.com", "edlin.com", "attrib.exe", "label.com",
        "mode.com", "more.com", "find.exe", "sort.exe", "mem.exe",
        "diskinfo.com"
    )
    $fArgs360 = @()
    foreach ($u in $utils360) {
        $p = Join-Path $binDir $u
        if (Test-Path $p) { $fArgs360 += @("-f", $p) }
    }
    & $dimgExe -o $img360 -b $binDir -s 360 -l "MSDOS4" -v @fArgs360
    if ($LASTEXITCODE -eq 0) {
        Write-Ok "Disk image: $img360"
    } else {
        Write-Fail "360KB disk image creation failed"
    }

    # 1.44MB disk — include common utilities
    $img1440 = Join-Path $outputDir "msdos4-1440.img"
    $utils = @(
        "fdisk.exe", "format.com", "chkdsk.com", "sys.com",
        "debug.com", "edlin.com", "attrib.exe", "label.com",
        "mode.com", "more.com", "find.exe", "sort.exe",
        "tree.com", "comp.com", "diskcomp.com", "diskcopy.com",
        "fc.exe", "xcopy.exe", "backup.com", "restore.com",
        "recover.com", "replace.exe", "print.com", "assign.com",
        "join.exe", "subst.exe", "mem.exe", "append.exe",
        "fastopen.exe", "graphics.com", "graftabl.com", "keyb.com",
        "nlsfunc.exe", "share.exe", "country.sys", "ansi.sys",
        "display.sys", "driver.sys", "keyboard.sys", "printer.sys",
        "ramdrive.sys", "ega.cpi", "diskinfo.com"
    )
    $fArgs = @()
    foreach ($u in $utils) {
        $p = Join-Path $binDir $u
        if (Test-Path $p) { $fArgs += @("-f", $p) }
    }
    & $dimgExe -o $img1440 -b $binDir -s 1440 -l "MSDOS4" -v @fArgs
    if ($LASTEXITCODE -eq 0) {
        Write-Ok "Disk image: $img1440"
    } else {
        Write-Fail "1.44MB disk image creation failed"
    }
}

# ── Main ────────────────────────────────────────────────────────────

$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()

Write-Host "MS-DOS 4.0 Build System" -ForegroundColor White
Write-Host "=======================" -ForegroundColor White

if ($Clean) {
    Clean-All
}

Get-LibdosTools
Set-BuildEnvironment

$failed = @()

if ($Component) {
    $name = $Component.ToLower()
    if ($name -notin $components) {
        Write-Fail "Unknown component '$Component'. Valid: $($components -join ', ')"
        exit 1
    }
    if (-not (Build-Component $name)) {
        $failed += $name
    }
} else {
    foreach ($comp in $components) {
        if (-not (Build-Component $comp)) {
            $failed += $comp
            Write-Host "`n  Continuing despite $comp failure..." -ForegroundColor Yellow
        }
    }
}

Copy-Outputs
New-DiskImage
Clean-IntreeArtifacts

$stopwatch.Stop()
$elapsed = $stopwatch.Elapsed

Write-Host "`n=======================================" -ForegroundColor White
if ($failed.Count -eq 0) {
    Write-Host "BUILD COMPLETE" -ForegroundColor Green
} else {
    Write-Host "BUILD FINISHED WITH FAILURES:" -ForegroundColor Red
    foreach ($f in $failed) {
        Write-Host "  - $f (see output/logs/$f.log)" -ForegroundColor Red
    }
}
Write-Host "Elapsed: $($elapsed.ToString('hh\:mm\:ss'))" -ForegroundColor White
Write-Host "Output:  $outputDir" -ForegroundColor White
if ($failed.Count -ne 0) { exit 1 }
