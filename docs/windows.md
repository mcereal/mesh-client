# Native Windows port

The Windows target is under active development. The first supported slice is an x64 UCRT
executable with the SDL UI, the Bluetooth, USB serial and TCP transports, MQTT and HTTPS, with
TLS through Mbed TLS, radio firmware installation for ESP32 boards over Bluetooth, and
self-update from the installer's own release asset. Bluetooth bonding, firmware installation over
USB and the UI control socket are follow-up platform backends rather than promises of the first
build.

## The installer

Releases carry `MeshClient-windows-x86_64-setup.exe`, built by
[`scripts/package-windows.ps1`](../scripts/package-windows.ps1) from
[`packaging/windows/MeshClient.iss`](../packaging/windows/MeshClient.iss) with Inno Setup 6. It
installs per user into `%LOCALAPPDATA%\Programs\MeshClient` and never asks for elevation, which
is what lets the client replace its own `meshclient.exe` from Settings > About, as the Brick does.
A Program Files install would need an administrator for that.

- The Start menu shortcut passes `--foreground` and starts the client in
  `%LOCALAPPDATA%\MeshClient`, which is where its settings and history land, since Windows has no
  `HOME`. An uninstall leaves that directory alone.
- A `--foreground` run releases a console that no other process shares, so a launch from the
  Start menu shows only the window. Run from a terminal, the console is kept.
- An update renames the running `meshclient.exe` to `meshclient.exe.old`, moves the download into
  its place and deletes the `.old` on the next launch: Windows refuses to overwrite a running
  executable but allows one to be renamed. Only the executable is updated; `SDL2.dll` and the
  other bundled DLLs stay as the installer left them, so a change to those needs the installer.
- Neither the installer nor the executable is code-signed, so SmartScreen shows "Windows
  protected your PC" on first run until the download has a reputation. **More info > Run
  anyway** installs it.

`scripts/package-windows.ps1 [-Version x.y.z]` builds the same thing locally, and needs Inno
Setup (`winget install JRSoftware.InnoSetup`).

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

Mbed TLS builds with the UCRT64 compiler like the rest of the tree, and needs the `jinja2` and
`jsonschema` Python packages the setup script installs. The setup script initialises it under
inkwell with Git's long paths turned on, because its nested post-quantum sources run past
`MAX_PATH` in a worktree; it does not recurse into inkcell's or inkstand's own inkwell pins,
which this build never reads.

## Porting boundary

Inkcell's SDL renderer is already platform-neutral outside its guarded Cocoa title-bar code. The
merged Inkwell runtime now provides waitable Windows handles, timers, wake events, console
shutdown and durable-file operations. The client uses host file wrappers for binary map packs
and store directories. The Unix UI-control socket reports `ENOTSUP` on Windows until a
native IPC backend is added.

The Windows event loop, TCP connector and stream handoff carry native pointer-sized Winsock
sockets without passing them through `int` descriptors. Hostnames use overlapped
`GetAddrInfoExW`, whose completion event is watched by the same loop, so both names and numeric
addresses can use that path without blocking the UI. The MQTT client and HTTPS fetch are on the
same native sockets, and a TLS session runs over them as it does over a POSIX descriptor - so a
broker with TLS turned on connects, and `--fetch-firmware` downloads a radio image. Certificates
are checked against the roots compiled into the client, exactly as on the Brick, and
`SSL_CERT_FILE` still names a bundle to use instead.

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

An ESP32 radio's firmware installs over Bluetooth as it does on the Brick: `--install-firmware
heltec-v4 -p ADDRESS`. The radio is asked into its OTA loader over the ordinary link, so a
PIN-protected radio has to be switched to No PIN first - or be sent into the loader from
elsewhere, after which the same command resumes it. Windows does not take the 7.5 ms connection
interval the install asks for, and the image goes at about 13 KB/s: a Heltec V4's 2.1 MB takes
under three minutes. The image stages in `%TEMP%` unless `--staging`
or `MESHCLIENT_FIRMWARE_STAGING` names another directory. An nRF52 or RP2040 installs by writing
its UF2 to the bootloader's drive, and there is no Windows backend for that yet.

Inkcell input and the client updater build on Windows. An installed release updates from
`meshclient-windows-x86_64.exe`; a local build checks but does not install, as on every other
platform.

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

1. Replace the explicit unavailable device backends with native implementations.
2. Bond with a PIN-protected node over Bluetooth, once Windows and the firmware agree on it.
3. Give the UI-control protocol a native IPC backend and settle Windows user-data paths.

The build and CI smoke check are the regression driver for this first slice. A successful link
does not yet imply every transport or service is available.
