# make-tools.ps1 — Build demu and dimg (native build tools)
#
# Compiles the native Windows tools used by the DOS build system.
# The resulting binaries are placed in output/ and should be checked in.
#
# Usage:
#   .\make-tools.ps1            Build demu.exe and dimg.exe
#   .\make-tools.ps1 -Clean     Force rebuild

param(
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$outputDir = Join-Path $root "output"
$demuSrcDir = Join-Path $root "tools\demu"
$dimgSrcDir = Join-Path $root "tools\dimg"
$demuExe = Join-Path $outputDir "demu.exe"
$dimgExe = Join-Path $outputDir "dimg.exe"

# ── Find Visual Studio ──────────────────────────────────────────────

function Find-VcVarsAll {
    $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vsWhere)) {
        $vsWhere = "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe"
    }
    if (-not (Test-Path $vsWhere)) { throw "Could not find Visual Studio (vswhere.exe)" }
    $vsPath = & $vsWhere -latest -property installationPath
    return Join-Path $vsPath "VC\Auxiliary\Build\vcvarsall.bat"
}

# ── Build demu ──────────────────────────────────────────────────────

function Build-Demu {
    Write-Host "`n=== Building demu.exe ===" -ForegroundColor Cyan
    New-Item -ItemType Directory -Path $outputDir -Force | Out-Null

    if ((Test-Path $demuExe) -and -not $Clean) {
        $exeTime = (Get-Item $demuExe).LastWriteTime
        $srcFiles = Get-ChildItem $demuSrcDir -Include *.c,*.h -File -Recurse
        $needsRebuild = $false
        foreach ($f in $srcFiles) {
            if ($f.LastWriteTime -gt $exeTime) { $needsRebuild = $true; break }
        }
        if (-not $needsRebuild) {
            Write-Host "  OK: demu.exe is up to date" -ForegroundColor Green
            return
        }
    }

    Get-Process demu -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500

    $vcvarsall = Find-VcVarsAll
    $sources = @("main.c","8086.c","loader-dos.c","syscall-dos.c","exec-dos.c","lib-omf.c") |
        ForEach-Object { "`"$(Join-Path $demuSrcDir $_)`"" }
    $sourceList = $sources -join " "

    $cmd = "`"$vcvarsall`" x64 >nul 2>&1 && cl.exe /nologo /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /Fe:`"$demuExe`" $sourceList /link user32.lib"
    cmd.exe /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "demu build failed" }

    # Clean stray .obj files
    Get-ChildItem $demuSrcDir -Filter *.obj -File -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue
    Get-ChildItem $root -Filter *.obj -File -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue
    Get-ChildItem $outputDir -Filter *.obj -File -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue

    Write-Host "  OK: demu.exe built" -ForegroundColor Green
}

# ── Build dimg ──────────────────────────────────────────────────────

function Build-Dimg {
    Write-Host "`n=== Building dimg.exe ===" -ForegroundColor Cyan
    New-Item -ItemType Directory -Path $outputDir -Force | Out-Null

    if ((Test-Path $dimgExe) -and -not $Clean) {
        $exeTime = (Get-Item $dimgExe).LastWriteTime
        $srcFiles = Get-ChildItem $dimgSrcDir -Include *.c,*.h -File -Recurse
        $needsRebuild = $false
        foreach ($f in $srcFiles) {
            if ($f.LastWriteTime -gt $exeTime) { $needsRebuild = $true; break }
        }
        if (-not $needsRebuild) {
            Write-Host "  OK: dimg.exe is up to date" -ForegroundColor Green
            return
        }
    }

    $vcvarsall = Find-VcVarsAll
    $src = "`"$(Join-Path $dimgSrcDir 'main.c')`""
    $cmd = "`"$vcvarsall`" x64 >nul 2>&1 && cl.exe /nologo /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /Fe:`"$dimgExe`" $src"
    cmd.exe /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "dimg build failed" }

    # Clean stray .obj files
    Get-ChildItem $dimgSrcDir -Filter *.obj -File -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue
    Get-ChildItem $outputDir -Filter *.obj -File -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue

    Write-Host "  OK: dimg.exe built" -ForegroundColor Green
}

# ── Main ────────────────────────────────────────────────────────────

Write-Host "MS-DOS 4.0 — Build Tools" -ForegroundColor White
Build-Demu
Build-Dimg
Write-Host "`nDone. Binaries are in output/" -ForegroundColor Green
