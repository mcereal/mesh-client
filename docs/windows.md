# Native Windows port

The Windows target is under active development. The first supported slice is an x64 UCRT
executable with the SDL UI, the Bluetooth, USB serial and TCP transports and plaintext MQTT.
Bluetooth bonding, firmware installation, self-update, TLS and the UI control socket are
follow-up platform backends rather than promises of the first build.

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
device is not required to see the window. A radio in Bluetooth range or plugged in over USB is
found and connected to on its own. Set `$env:MESHCLIENT_UI_BACKEND = 'cli'` when a terminal-only
run is intended.

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

The Windows event loop, TCP connector and stream handoff carry native pointer-sized Winsock
sockets without passing them through `int` descriptors. Hostnames use overlapped
`GetAddrInfoExW`, whose completion event is watched by the same loop, so both names and numeric
addresses can use that path without blocking the UI. The MQTT client is on the same native
sockets, so a plaintext broker (port 1883) works; one with TLS turned on is refused by name
until TLS is ported. HTTPS fetch still uses an explicit unavailable backend.

USB serial is inkwell's SetupAPI scan and overlapped COM I/O: a port is found by its USB vendor
and product, named by the product string the device reports, and opened as `COM4` or
`\\.\COM10`. A port another program holds (a serial terminal, a flasher) is exclusive on Windows
and fails to open with `EBUSY` until that program lets go.

Closing the port drops RTS before DTR, because an ESP32 takes RTS raised while DTR is low as a
reset. A client that is killed outright rather than closed leaves the order to Windows, which can
reset the radio - into its ROM download mode, where the screen is dark and the port is silent.
Close the window, or press the radio's reset button to recover.

Bluetooth is inkwell's Windows Runtime backend. **Windows cannot bond with a Meshtastic ESP32
yet**, in either pairing mode: Settings > Add device takes the PIN, the radio reports the
passkey authenticated, and Windows fails the pairing a moment later. A node set to **No PIN**
connects and works normally; one set to Fixed PIN or Random PIN refuses the first write, which
the client reports as needing to be paired. Use USB serial for those until bonding works.

Inkcell input and the client updater compile on Windows; the updater offers no install action
because releases contain Linux binaries only.

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

1. Port TLS and HTTPS fetch to native Windows sockets.
2. Replace the explicit unavailable device backends with native implementations.
3. Bond with a PIN-protected node over Bluetooth, once Windows and the firmware agree on it.
4. Give the UI-control protocol a native IPC backend and settle Windows user-data paths.

The build and CI smoke check are the regression driver for this first slice. A successful link
does not yet imply every transport or service is available.
