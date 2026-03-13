# make-dos.ps1 — Build MS-DOS 4.0 using demu (DOS emulator)
#
# Expects pre-built demu.exe and dimg.exe in output/.
# Run .\make-tools.ps1 first if they are missing.
#
#   output/demu.exe    — the DOS emulator (pre-built)
#   output/dimg.exe    — the disk image tool (pre-built)
#   output/logs/       — per-component build logs
#   output/bin/        — final DOS 4.0 binaries
#
# Usage:
#   .\make-dos.ps1                     Build everything
#   .\make-dos.ps1 -Clean              Remove build artifacts and rebuild
#   .\make-dos.ps1 -Component bios     Build only a specific component
#   .\make-dos.ps1 -Verbose            Show full build output
#
# Components (in build order):
#   messages, mapper, boot, bios, dos, cmd, dev, select, memm

param(
    [switch]$Clean,
    [string]$Component = "",
    [switch]$Verbose
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

$components = @("messages", "mapper", "boot", "bios", "dos", "cmd", "dev", "select", "memm")

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
        default    { throw "Unknown component: $name" }
    }
    return Join-Path $srcDir $sub
}

# ── Step 1: Check for pre-built tools ───────────────────────────────

function Check-Tools {
    Write-Step "Checking for pre-built tools"
    if (-not (Test-Path $demuExe)) {
        throw "demu.exe not found in output/. Run .\make-tools.ps1 first."
    }
    if (-not (Test-Path $dimgExe)) {
        throw "dimg.exe not found in output/. Run .\make-tools.ps1 first."
    }
    Write-Ok "demu.exe and dimg.exe found"
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
        $output = & $demuExe (Join-Path $toolsDir "NMAKE.EXE") 2>&1 | Out-String
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
    # Build COMSUBS.LIB stub if needed before CMD
    if ($name -eq "cmd") {
        $comsubsLib = Join-Path $srcDir "INC\COMSUBS.LIB"
        $comsubsSrc = Join-Path $srcDir "INC\COMSUBS.C"
        if ((Test-Path $comsubsSrc) -and -not (Test-Path $comsubsLib)) {
            Push-Location (Join-Path $srcDir "INC")
            try {
                & $demuExe (Join-Path $toolsDir "CL.EXE") /c /AS /Gs COMSUBS.C 2>&1 | Out-Null
                & $demuExe (Join-Path $toolsDir "LIB.EXE") "COMSUBS.LIB+COMSUBS.OBJ;" 2>&1 | Out-Null
            } finally { Pop-Location }
        }
        return Build-CmdSubdirs
    }
    # Build CASSFAR stub if needed before SELECT
    # Also ensure messages are built (select needs usa-ms.idx)
    if ($name -eq "select") {
        $msgIdx = Join-Path $srcDir "MESSAGES\usa-ms.idx"
        if (-not (Test-Path $msgIdx)) {
            Invoke-Nmake (Get-ComponentDir "messages") "messages" | Out-Null
        }
        $selectDir = Get-ComponentDir "select"
        $cassfarAsm = Join-Path $selectDir "CASSFAR.ASM"
        if (Test-Path $cassfarAsm) {
            # Assemble stub
            Push-Location $selectDir
            try {
                & $demuExe (Join-Path $toolsDir "MASM.EXE") "-Mx -t CASSFAR.ASM,CASSFAR.OBJ;" 2>&1 | Out-Null
            } finally { Pop-Location }
        }
    }
    return Invoke-Nmake (Get-ComponentDir $name) $name
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
            $output = & $demuExe (Join-Path $toolsDir "NMAKE.EXE") 2>&1 | Out-String
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
    New-Item -ItemType Directory -Path $binDir -Force | Out-Null

    $outputs = @(
        "bios\io.sys",
        "dos\msdos.sys",
        "boot\MSBOOT.BIN",
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
    $missing = 0
    foreach ($rel in $outputs) {
        $src = Join-Path $srcDir $rel
        if (Test-Path $src) {
            Copy-Item $src (Join-Path $binDir (Split-Path $rel -Leaf)) -Force
            $copied++
        } else {
            $missing++
            if ($Verbose) { Write-Host "  Missing: $rel" -ForegroundColor DarkYellow }
        }
    }

    Write-Ok "$copied files copied to output/bin/ ($missing not found)"
}

# ── Step 5: Clean in-tree intermediates ─────────────────────────────

function Clean-IntreeArtifacts {
    Write-Step "Cleaning in-tree build intermediates from dos/"
    $cleanExts = @("*.obj","*.exe","*.com","*.sys","*.bin","*.lib","*.lst",
                   "*.cl1","*.cl2","*.cl3","*.cl4","*.cl5","*.clf","*.ctl",
                   "*.map","*.tmp")
    $removed = 0

    foreach ($comp in $components) {
        $dir = Get-ComponentDir $comp
        if (Test-Path $dir) {
            foreach ($ext in $cleanExts) {
                $files = Get-ChildItem $dir -Recurse -Filter $ext -File -ErrorAction SilentlyContinue
                $removed += $files.Count
                $files | Remove-Item -Force -ErrorAction SilentlyContinue
            }
        }
    }

    # Also clean INC generated objects
    $incDir = Join-Path $srcDir "INC"
    if (Test-Path $incDir) {
        foreach ($ext in $cleanExts) {
            $files = Get-ChildItem $incDir -Filter $ext -File -ErrorAction SilentlyContinue
            $removed += $files.Count
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
    Get-Process demu -ErrorAction SilentlyContinue | Stop-Process -Force
    Clean-IntreeArtifacts
    # Remove build artifacts but preserve pre-built tool binaries
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

    # 360KB disk — include essential utilities
    $img360 = Join-Path $outputDir "msdos4-360.img"
    $utils360 = @(
        "fdisk.exe", "format.com", "chkdsk.com", "sys.com",
        "debug.com", "edlin.com", "attrib.exe", "label.com",
        "mode.com", "more.com", "find.exe", "sort.exe", "mem.exe"
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
        "ramdrive.sys", "ega.cpi"
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

Check-Tools
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
