# Native Windows port

The Windows target is under active development. The first supported slice is an x64 UCRT
executable with the SDL UI and TCP transport. Bluetooth, USB serial, firmware installation,
self-update, TLS and the UI control socket are follow-up platform backends rather than promises
of the first build.

## See the UI

After building, launch the native window from PowerShell:

```powershell
$env:Path = "C:\msys64\ucrt64\bin;$env:Path"
.\build\windows-debug\meshclient.exe --foreground
```

Set `MSYS2_ROOT` and substitute its `ucrt64\bin` directory if MSYS2 is installed elsewhere.
Windows now selects SDL by default; `--foreground` keeps the event loop and window open until
you close it or press Escape. Without `--foreground`, the default single poll exits almost
immediately. The SDL2 DLLs must remain on `PATH` until they are bundled with a release. A
device is not required to see the window, though Bluetooth and USB serial are not connected
yet. Set `$env:MESHCLIENT_UI_BACKEND = 'cli'` when a terminal-only run is intended.

## Toolchain

Install [MSYS2](https://www.msys2.org/) in `C:\msys64`, then run from PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/setup-windows.ps1
powershell -ExecutionPolicy Bypass -File scripts/build-windows.ps1
```

Set `MSYS2_ROOT` before either command when MSYS2 is installed elsewhere. `-Check` makes the
setup script report only, and `build-windows.ps1 -Configuration Release` selects another build
type. Build trees are kept in `build/windows-<configuration>` so they cannot share a CMake cache
with Linux, macOS or container builds.

MSYS2 is the build and package environment only. The UCRT64 compiler produces an ordinary
Windows executable and does not link `msys-2.0.dll`.

TLS is unavailable on Windows until Inkwell has a native Winsock TLS backend. The build script
disables it, and Inkwell refuses TLS even if Mbed TLS is present. The Windows setup script also
avoids recursive Mbed TLS initialization, whose deeply nested post-quantum submodules can exceed
Git for Windows' path limit in a worktree.

## Porting boundary

Inkcell's SDL renderer is already platform-neutral outside its guarded Cocoa title-bar code. The
merged Inkwell runtime now provides waitable Windows handles, timers, wake events, console
shutdown and durable-file operations. The client uses host file wrappers for binary map packs
and store directories. The Unix UI-control socket reports `ENOTSUP` on Windows until a
native IPC backend is added.

The Windows event loop, TCP connector and stream handoff now carry native pointer-sized Winsock
sockets without passing them through `int` descriptors. Numeric TCP addresses can use that path;
asynchronous hostname lookup still needs a Windows resolver. The full executable build still
links: HTTPS fetch and MQTT use explicit unavailable backends until their native socket ports
arrive. Inkcell input and the client updater compile on Windows; the updater offers no install
action because releases contain Linux binaries only.

For a display-free smoke run after building, use PowerShell with the UCRT64 DLL directory on
`PATH`:

```powershell
$env:Path = "C:\msys64\ucrt64\bin;$env:Path"
$env:SDL_VIDEODRIVER = 'dummy'
$client = (Resolve-Path .\build\windows-debug\meshclient.exe).Path
Push-Location $env:TEMP
try {
    $smoke = & $client --disable-ble --disable-serial --disable-tcp -t 1 2>&1
    $smoke | Out-Host
    if ($LASTEXITCODE -ne 0 -or
        -not ($smoke | Where-Object { $_.ToString().Contains('SDL UI backend active') })) {
        throw 'SDL did not start.'
    }
} finally { Pop-Location }
```

The temporary working directory keeps the current fallback `.meshclient` preferences out of
the checkout. The Windows CI job runs this build and smoke check on every PR.

The remaining work is primarily in platform backends:

1. Port asynchronous name resolution, HTTPS fetch, MQTT and TLS to native Windows sockets.
2. Replace the explicit unavailable device backends with native implementations.
3. Add SetupAPI/overlapped COM serial, then a Windows Runtime BLE backend.
4. Give the UI-control protocol a native IPC backend and settle Windows user-data paths.

The build and CI smoke check are the regression driver for this first slice. A successful link
does not yet imply every transport or service is available.
