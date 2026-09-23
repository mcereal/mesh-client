<# Build the native Windows development target with the MSYS2 UCRT64 toolchain. #>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Configuration = 'Debug',
    [switch]$ConfigureOnly
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$msysRoot = if ($env:MSYS2_ROOT) { $env:MSYS2_ROOT } else { 'C:\msys64' }
$ucrtBin = Join-Path $msysRoot 'ucrt64\bin'
$cmake = Join-Path $ucrtBin 'cmake.exe'
$ninja = Join-Path $ucrtBin 'ninja.exe'
$compiler = Join-Path $ucrtBin 'gcc.exe'
$python = Join-Path $ucrtBin 'python.exe'

foreach ($tool in @($cmake, $ninja, $compiler, $python)) {
    if (-not (Test-Path -LiteralPath $tool)) {
        throw "Missing $tool. Run scripts/setup-windows.ps1 first."
    }
}

$env:Path = "$ucrtBin;$(Join-Path $msysRoot 'usr\bin');$env:Path"
$buildDir = Join-Path $repoRoot "build\windows-$($Configuration.ToLowerInvariant())"

& $cmake -S $repoRoot -B $buildDir -G Ninja `
    "-DCMAKE_BUILD_TYPE=$Configuration" `
    "-DCMAKE_C_COMPILER=$($compiler.Replace('\', '/'))" `
    "-DCMAKE_MAKE_PROGRAM=$($ninja.Replace('\', '/'))" `
    "-DPython3_EXECUTABLE=$($python.Replace('\', '/'))" `
    -DINKWELL_WITH_TLS=OFF `
    -DBUILD_TESTING=OFF `
    -DMESHCLIENT_BUILD_DEVTOOLS=OFF
if ($LASTEXITCODE -ne 0) {
    throw 'Windows configure failed.'
}

if (-not $ConfigureOnly) {
    & $cmake --build $buildDir --target meshclient
    if ($LASTEXITCODE -ne 0) {
        throw 'Windows build failed.'
    }
}
