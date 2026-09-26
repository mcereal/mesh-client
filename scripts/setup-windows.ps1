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
    'mingw-w64-ucrt-x86_64-protobuf',
    'mingw-w64-ucrt-x86_64-python-protobuf',
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
    # The direct submodules, and Mbed TLS under inkwell with everything nested in it. Not a bare
    # --recursive: inkcell and inkstand each pin an inkwell of their own, which this build never
    # reads because this repository's pin is the one the tree builds.
    $outOfSync = @(git submodule status 2>$null) +
        @(git -C third_party/inkwell submodule status --recursive 2>$null) |
        Select-String '^[+-]'
    $submodulesOutOfSync = $null -ne $outOfSync
    if ($outOfSync) {
        if ($Check) {
            Write-Host 'Submodules:'
            Write-Host '  missing or out of sync'
        } else {
            Write-Host 'Synchronizing submodules...'
            # Long paths: Mbed TLS's nested post-quantum sources run past MAX_PATH in a worktree.
            git -c core.longpaths=true submodule update --init proto/meshtastic third_party/inkwell third_party/inkcell third_party/inkstand third_party/nanopb
            if ($LASTEXITCODE -eq 0) {
                git -c core.longpaths=true -C third_party/inkwell submodule update --init --recursive
            }
            if ($LASTEXITCODE -ne 0) {
                throw 'Submodule synchronization failed.'
            }
        }
    } else {
        Write-Host 'Submodules:'
        Write-Host '  in sync, with Mbed TLS'
    }
} finally {
    Pop-Location
}

# Only scripts/package-windows.ps1 needs Inno Setup, so it is reported and not installed: a build
# is ready without it. The places looked in are the ones that script looks in.
$iscc = (Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source
foreach ($candidate in @(
        (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
        (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe'),
        (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'))) {
    if (-not $iscc -and (Test-Path -LiteralPath $candidate)) {
        $iscc = $candidate
    }
}
Write-Host 'Inno Setup 6 (the installer only):'
if ($iscc) {
    Write-Host "  $iscc"
} else {
    Write-Host '  not installed: winget install JRSoftware.InnoSetup'
}

if ($Check -and ($missing.Count -ne 0 -or $submodulesOutOfSync)) {
    exit 1
}

Write-Host "Ready. Run: powershell -File scripts/build-windows.ps1"
Write-Host "Toolchain: $ucrtBin"
