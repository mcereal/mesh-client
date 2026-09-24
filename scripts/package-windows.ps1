<#
The Windows download: an installer, and the bare executable the in-app updater swaps in.

    scripts/package-windows.ps1                  # a development build, for looking at
    scripts/package-windows.ps1 -Version 2.72.0  # a release: stamped, allowed to update itself

Writes to dist\:
    MeshClient-windows-x86_64-setup.exe   what a person downloads and runs
    meshclient-windows-x86_64.exe         the installed binary alone, which the updater
                                          downloads and renames into the install directory
    *.sha256                              in sha256sum's format, as the other assets are

The executable is MSYS2 UCRT64's, so it needs SDL2.dll and whatever else from ucrt64\bin it
imports. Those are found by walking the import tables rather than listed by hand, so a new
dependency is picked up rather than missed. An update replaces only the executable; the DLLs stay
as the installer left them.

Needs MSYS2 (scripts/setup-windows.ps1) and Inno Setup 6 (winget install JRSoftware.InnoSetup,
or ISCC on PATH).
#>
[CmdletBinding()]
param(
    [string]$Version = ''
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$msysRoot = if ($env:MSYS2_ROOT) { $env:MSYS2_ROOT } else { 'C:\msys64' }
$ucrtBin = Join-Path $msysRoot 'ucrt64\bin'
$objdump = Join-Path $ucrtBin 'objdump.exe'
$assetName = 'meshclient-windows-x86_64.exe'
$installerName = 'MeshClient-windows-x86_64-setup.exe'
$distDir = Join-Path $repoRoot 'dist'
$stageDir = Join-Path $repoRoot 'build\windows-package\MeshClient'

# The installer's version wants numbers; a prerelease tag keeps its suffix in the binary.
if ($Version) {
    $numeric = ($Version -split '[-+]')[0]
} else {
    $line = Select-String -Path (Join-Path $repoRoot 'CMakeLists.txt') `
        -Pattern '^project\(meshclient VERSION ([0-9]+\.[0-9]+\.[0-9]+)'
    $numeric = $line.Matches[0].Groups[1].Value
}
if ($numeric -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
    throw "Cannot derive a numeric installer version from '$Version'."
}

$iscc = (Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source
if (-not $iscc) {
    foreach ($candidate in @(
            (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
            (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe'),
            (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'))) {
        if (Test-Path -LiteralPath $candidate) {
            $iscc = $candidate
            break
        }
    }
}
if (-not $iscc) {
    throw 'Inno Setup 6 is not installed (winget install JRSoftware.InnoSetup).'
}

# ---------------------------------------------------------------------------------------------
# The client. Both release options are passed either way: the build tree is reused, and a cached
# RELEASE_BUILD=ON left by a release would otherwise stamp the next development build as one.
$cmakeArgs = @("-DMESHCLIENT_UPDATE_ASSET=$assetName")
if ($Version) {
    $cmakeArgs += @("-DMESHCLIENT_VERSION_OVERRIDE=$Version", '-DMESHCLIENT_RELEASE_BUILD=ON')
} else {
    $cmakeArgs += @('-DMESHCLIENT_VERSION_OVERRIDE=', '-DMESHCLIENT_RELEASE_BUILD=OFF')
}
& (Join-Path $PSScriptRoot 'build-windows.ps1') -Configuration Release -CMakeArgs $cmakeArgs
$exe = Join-Path $repoRoot 'build\windows-release\meshclient.exe'

# ---------------------------------------------------------------------------------------------
# The staged install: the executable and every DLL it reaches that ucrt64\bin supplies. Anything
# not found there is the system's (KERNEL32, the UCRT's api-ms-win-crt-*) and is not shipped.
if (Test-Path -LiteralPath $stageDir) {
    Remove-Item -LiteralPath $stageDir -Recurse -Force
}
New-Item -ItemType Directory -Path $stageDir | Out-Null
Copy-Item -LiteralPath $exe -Destination (Join-Path $stageDir 'meshclient.exe')

$seen = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
$queue = New-Object 'System.Collections.Generic.Queue[string]'
$queue.Enqueue($exe)
while ($queue.Count -gt 0) {
    $file = $queue.Dequeue()
    $imports = & $objdump -p $file | Select-String -Pattern 'DLL Name:\s*(\S+)' |
        ForEach-Object { $_.Matches[0].Groups[1].Value }
    if ($LASTEXITCODE -ne 0) {
        throw "objdump could not read $file."
    }
    foreach ($dll in $imports) {
        if (-not $seen.Add($dll)) {
            continue
        }
        $candidate = Join-Path $ucrtBin $dll
        if (Test-Path -LiteralPath $candidate) {
            Copy-Item -LiteralPath $candidate -Destination $stageDir
            $queue.Enqueue($candidate)
            Write-Host "Bundling $dll"
        }
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $stageDir 'SDL2.dll'))) {
    throw 'meshclient.exe does not import SDL2.dll: the window backend did not build.'
}

# The licences for what the binary carries (see scripts/package.sh), plus SDL2's, which the
# installer carries as a file.
$licenses = Join-Path $stageDir 'licenses'
New-Item -ItemType Directory -Path $licenses | Out-Null
Copy-Item -LiteralPath (Join-Path $repoRoot 'LICENSE') -Destination (Join-Path $licenses 'LICENSE-MeshClient.txt')
Copy-Item -Path (Join-Path $repoRoot 'licenses\*.txt') -Destination $licenses
$sdlLicense = Join-Path $msysRoot 'ucrt64\share\licenses\SDL2\LICENSE.txt'
if (Test-Path -LiteralPath $sdlLicense) {
    Copy-Item -LiteralPath $sdlLicense -Destination (Join-Path $licenses 'Zlib-SDL2.txt')
}
$pthreadLicense = Join-Path $msysRoot 'ucrt64\share\licenses\libwinpthread\COPYING'
if ((Test-Path -LiteralPath (Join-Path $stageDir 'libwinpthread-1.dll')) -and
    (Test-Path -LiteralPath $pthreadLicense)) {
    Copy-Item -LiteralPath $pthreadLicense -Destination (Join-Path $licenses 'winpthreads.txt')
}

# The staged install as a user's machine would run it: with MSYS2 off PATH, so a DLL the walk
# above missed fails here rather than on somebody's desktop.
$savedPath = $env:Path
$env:Path = ($env:Path -split ';' | Where-Object { $_ -and ($_ -notlike "$msysRoot*") }) -join ';'
try {
    $reported = & (Join-Path $stageDir 'meshclient.exe') --version 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "The staged meshclient.exe did not start (exit $LASTEXITCODE)."
    }
} finally {
    $env:Path = $savedPath
}
Write-Host $reported
if ($Version -and -not ($reported -match [regex]::Escape($Version))) {
    throw "The staged meshclient.exe reports '$reported', not $Version."
}

# ---------------------------------------------------------------------------------------------
# The downloads.
New-Item -ItemType Directory -Force -Path $distDir | Out-Null
& $iscc /Q "/DAppVersion=$numeric" "/DSourceDir=$stageDir" "/DOutputDir=$distDir" `
    (Join-Path $repoRoot 'packaging\windows\MeshClient.iss')
if ($LASTEXITCODE -ne 0) {
    throw 'Inno Setup could not build the installer.'
}
Copy-Item -LiteralPath (Join-Path $stageDir 'meshclient.exe') -Destination (Join-Path $distDir $assetName)

foreach ($name in @($installerName, $assetName)) {
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $distDir $name)).Hash.ToLowerInvariant()
    # sha256sum's format, with LF, so `sha256sum -c` reads it on any system.
    [System.IO.File]::WriteAllText((Join-Path $distDir "$name.sha256"), "$hash  $name`n")
}

Write-Host "Windows $(if ($Version) { $Version } else { 'development' }) build:"
Get-ChildItem -LiteralPath $distDir -Filter '*windows*' | Format-Table Name, Length
