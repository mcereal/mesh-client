<#
.SYNOPSIS
Provision the native Windows development toolchain for mesh-client.

.DESCRIPTION
The produced programs are ordinary UCRT Windows executables. MSYS2 supplies GCC, CMake,
Ninja, SDL2 and Python packages; the application does not link to the MSYS runtime.

Use -Check to report missing prerequisites without installing anything.
#>
[CmdletBinding()]
param([switch]$Check)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$msysRoot = if ($env:MSYS2_ROOT) { $env:MSYS2_ROOT } else { 'C:\msys64' }
$bash = Join-Path $msysRoot 'usr\bin\bash.exe'
$pacman = Join-Path $msysRoot 'usr\bin\pacman.exe'
$ucrtBin = Join-Path $msysRoot 'ucrt64\bin'

if (-not (Test-Path -LiteralPath $bash) -or -not (Test-Path -LiteralPath $pacman)) {
    throw "MSYS2 was not found at $msysRoot. Install it from https://www.msys2.org/ or set MSYS2_ROOT."
}

$packages = @(
    'mingw-w64-ucrt-x86_64-gcc',
    'mingw-w64-ucrt-x86_64-cmake',
    'mingw-w64-ucrt-x86_64-ninja',
    'mingw-w64-ucrt-x86_64-pkgconf',
    'mingw-w64-ucrt-x86_64-SDL2',
    'mingw-w64-ucrt-x86_64-python',
    'mingw-w64-ucrt-x86_64-python-protobuf',
    'mingw-w64-ucrt-x86_64-python-grpcio-tools',
    'mingw-w64-ucrt-x86_64-python-jinja',
    'mingw-w64-ucrt-x86_64-python-jsonschema'
)

$missing = @()
foreach ($package in $packages) {
    & cmd.exe /d /c "`"$pacman`" -Q $package >nul 2>nul"
    if ($LASTEXITCODE -ne 0) {
        $missing += $package
    }
}

Write-Host 'MSYS2 UCRT64 packages:'
if ($missing.Count -eq 0) {
    Write-Host '  ready'
} elseif ($Check) {
    $missing | ForEach-Object { Write-Host "  missing: $_" }
} else {
    # Package install hooks expect the MSYS utilities on PATH even when pacman was launched
    # from PowerShell. Running through bash supplies that environment.
    $quoted = ($missing | ForEach-Object { "'$_'" }) -join ' '
    & $bash -lc "MSYSTEM=UCRT64 pacman -S --needed --noconfirm $quoted"
    if ($LASTEXITCODE -ne 0) {
        throw 'MSYS2 package installation failed.'
    }
}

Push-Location $repoRoot
try {
    $outOfSync = git submodule status 2>$null | Select-String '^[+-]'
    $submodulesOutOfSync = $null -ne $outOfSync
    if ($outOfSync) {
        if ($Check) {
            Write-Host 'Submodules:'
            Write-Host '  missing or out of sync'
        } else {
            Write-Host 'Synchronizing direct submodules...'
            # Do not recurse into Mbed TLS here. Its optional PQ dependencies create paths long
            # enough to exceed Git-for-Windows limits in worktrees. The initial Windows build
            # disables TLS; a later TLS-enabled build can use a short standalone checkout.
            git -c core.longpaths=true submodule update --init proto/meshtastic third_party/inkwell third_party/inkcell third_party/nanopb
            if ($LASTEXITCODE -ne 0) {
                throw 'Submodule synchronization failed.'
            }
        }
    } else {
        Write-Host 'Submodules:'
        Write-Host '  direct dependencies are in sync'
    }
} finally {
    Pop-Location
}

if ($Check -and ($missing.Count -ne 0 -or $submodulesOutOfSync)) {
    exit 1
}

Write-Host "Ready. Run: powershell -File scripts/build-windows.ps1"
Write-Host "Toolchain: $ucrtBin"
