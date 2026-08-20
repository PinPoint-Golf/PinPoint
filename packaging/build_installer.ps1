<#
.SYNOPSIS
    Build a Release PinPoint Studio and produce the Windows installer (CPack + Inno Setup).

.DESCRIPTION
    1. Configures a Release build dir (NMake JOM generator, MSVC env from vcvars).
    2. Builds the PinPointStudio target.
    3. Runs CPack with the Inno Setup generator to produce
       PinPointStudioSetup-<version>.exe under the build dir.

    A fresh Release build dir triggers the usual FetchContent downloads
    (whisper ~148 MB, ViTPose ~330 MB, ONNX Runtime, etc.) and takes several minutes.

.PREREQUISITES
    - Visual Studio (vcvars64.bat), Qt 6.11 MSVC, CMake + jom (paths below).
    - Inno Setup 6 (ISCC.exe). Auto-detected in the usual install locations.
    - Build a RELEASE config: the debug CRT (vcruntime140d.dll, ...) is not
      redistributable, so a Debug installer only runs where Visual Studio is present.

.PARAMETER Components
    'both' (default) packages core + cuda in one installer. 'core' produces a small
    core-only installer (the eventual auto-update payload); 'cuda' a standalone GPU
    runtime package. (The two-artifact split is groundwork for in-app auto-update.)

.EXAMPLE
    pwsh -File packaging\build_installer.ps1
    pwsh -File packaging\build_installer.ps1 -Components core
#>
[CmdletBinding()]
param(
    [string]$BuildDir = 'build\Release-Installer',
    [string]$QtPrefix = 'C:\Qt\6.11.1\msvc2022_64',
    [ValidateSet('both','core','cuda')]
    [string]$Components = 'both'
)
# NOTE: deliberately 'Continue', not 'Stop'. vcvars64.bat and cmake/cpack write
# benign messages to stderr (e.g. the cosmetic "'vswhere.exe' is not recognized");
# under 'Stop', PowerShell 5.1 wraps native stderr as a terminating NativeCommandError
# and aborts before anything builds. Real failures are caught via $LASTEXITCODE below.
$ErrorActionPreference = 'Continue'

$repo   = Split-Path -Parent $PSScriptRoot
$cmake  = 'C:\Qt\Tools\CMake_64\bin\cmake.exe'
$cpack  = 'C:\Qt\Tools\CMake_64\bin\cpack.exe'
$vcvars = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$jom    = 'C:\Qt\Tools\QtCreator\bin\jom'

# Locate the Inno Setup compiler (CPack's INNOSETUP generator shells out to ISCC).
$iscc = (Get-Command ISCC -ErrorAction SilentlyContinue).Source
if (-not $iscc) {
    foreach ($p in @(
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
        'C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
        'C:\Program Files\Inno Setup 6\ISCC.exe')) {
        if (Test-Path $p) { $iscc = $p; break }
    }
}
if (-not $iscc) { throw "Inno Setup 6 (ISCC.exe) not found. Install it from https://jrsoftware.org/isdl.php" }

# cpack's preinstall step shells out to the generator's build tool (jom) and the
# Inno generator runs ISCC — so configure, build AND package all run inside the same
# vcvars + jom environment. cpack runs from the build dir so it finds CPackConfig.cmake
# and writes _CPack_Packages / the installer there.
# Distinct output name per variant so successive runs don't overwrite each other:
#   both -> PinPointStudioSetup-<ver>.exe
#   core -> PinPointStudioSetup-<ver>-core.exe   (no CUDA; smaller)
#   cuda -> PinPointStudioSetup-<ver>-cuda.exe   (GPU runtime only)
# The asset FILENAME reflects the version.h string / release tag (e.g. v0.1-alpha3),
# the single source of truth — NOT the internal monotonic build number. (The Inno
# AppVersion / CPACK_PACKAGE_VERSION stays numeric MAJOR.MINOR.BUILD, derived by CMake
# from the same version.h, for upgrade comparison.) Don't scrape CMakeLists.txt: the
# project() line is computed there, not a literal.
$verH = Get-Content (Join-Path $repo 'src\Core\version.h') -Raw
$maj = [regex]::Match($verH, '#define\s+PINPOINT_VERSION_MAJOR\s+(\d+)').Groups[1].Value
$min = [regex]::Match($verH, '#define\s+PINPOINT_VERSION_MINOR\s+(\d+)').Groups[1].Value
$pfx = [regex]::Match($verH, '#define\s+PINPOINT_VERSION_POSTFIX\s+"([^"]*)"').Groups[1].Value
if (-not $maj -or -not $min) { throw "Could not parse version from src/Core/version.h" }
$version = "v$maj.$min$pfx"   # e.g. v0.1-alpha3 — matches PINPOINT_VERSION_STRING and the tag
$pkgName = "PinPointStudioSetup-$version"
$cpackComponentArg = ''
switch ($Components) {
    'core' { $cpackComponentArg = '-D CPACK_COMPONENTS_ALL=core'; $pkgName += '-core' }
    'cuda' {
        $pkgName += '-cuda'
        # CUDA runtime as its OWN Inno product (design §4.3): a distinct, stable AppId so
        # a -core auto-update never lists or removes these files (separate uninstall log).
        # Same install dir as core (CPACK_PACKAGE_INSTALL_DIRECTORY is unchanged), so the
        # cuDNN/CUDA DLLs land next to the exe and need no DLL-search changes. No app
        # shortcut / run-on-finish — this component carries no executable.
        # NOTE: authored; validate on a clean VM (install core, install cuda, then run a
        # -core upgrade and confirm the cuda DLLs survive).
        $cudaAppId = '{{C8E2F1A4-7B3D-4E5F-9A1C-2D6B8F0E3A47}'
        $cpackComponentArg =
            '-D CPACK_COMPONENTS_ALL=cuda ' +
            "-D `"CPACK_INNOSETUP_SETUP_AppId=$cudaAppId`" " +
            '-D "CPACK_PACKAGE_NAME=PinPoint Studio GPU Runtime" ' +
            '-D "CPACK_PACKAGE_EXECUTABLES=" ' +
            '-D "CPACK_CREATE_DESKTOP_LINKS=" ' +
            '-D "CPACK_INNOSETUP_RUN_EXECUTABLES="'
    }
}
$bat = @"
@echo on
call "$vcvars"
set PATH=%PATH%;$jom
cd /d "$repo"
"$cmake" -S . -B "$BuildDir" -G "NMake Makefiles JOM" -DCMAKE_BUILD_TYPE=Release -DPP_SHIPPING_BUILD=ON -DCMAKE_PREFIX_PATH="$QtPrefix" || exit /b 1
"$cmake" --build "$BuildDir" --target PinPointStudio || exit /b 1
cd /d "$repo\$BuildDir"
"$cpack" -G INNOSETUP -D CPACK_INNOSETUP_EXECUTABLE="$iscc" -D CPACK_PACKAGE_FILE_NAME="$pkgName" $cpackComponentArg || exit /b 1
"@
$batPath = Join-Path $env:TEMP 'pp_build_installer.bat'
$bat | Out-File -FilePath $batPath -Encoding ascii
cmd /c "`"$batPath`""
if ($LASTEXITCODE -ne 0) { throw "Build/package failed (exit $LASTEXITCODE)" }

Write-Host ""
Write-Host "Installer written under $repo\$BuildDir" -ForegroundColor Green
Get-ChildItem (Join-Path $repo $BuildDir) -Filter '*.exe' | Where-Object { $_.Name -like 'PinPointStudioSetup*' } | ForEach-Object { Write-Host "  $($_.FullName)" }
