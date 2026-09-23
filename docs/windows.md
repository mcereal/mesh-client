# Native Windows port

The Windows target is under active development. The first supported slice is an x64 UCRT
executable with the SDL UI and TCP transport. Bluetooth, USB serial, firmware installation,
self-update, TLS and the UI control socket are follow-up platform backends rather than promises
of the first build.

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

TLS is temporarily disabled in this target. Recursive Mbed TLS initialization reaches deeply
nested post-quantum submodules and can exceed Git for Windows' path limit when this repository is
a worktree. This is independent of the runtime port and should not obscure its compiler errors.

## Porting boundary

Inkcell's SDL renderer is already platform-neutral outside its guarded Cocoa title-bar code. The
merged Inkwell runtime now provides waitable Windows handles, timers, wake events, console
shutdown and durable-file operations. The client uses host file wrappers for binary map packs
and store directories. The Unix UI-control socket reports `ENOTSUP` on Windows until a
native IPC backend is added.

The Windows event loop, TCP connector and stream handoff now carry native pointer-sized Winsock
sockets without passing them through `int` descriptors. Numeric TCP addresses can use that path;
asynchronous hostname lookup still needs a Windows resolver. The full executable build still
stops in POSIX-only Inkwell serial, USB, fetch and MQTT sources, before all client sources can be
compiled.

The remaining work is primarily in platform backends:

1. Port asynchronous name resolution and compile or stub the POSIX-only Inkwell facilities so
   the native executable can link.
2. Compile device-only facilities to explicit unavailable backends until their Windows versions
   arrive.
3. Add SetupAPI/overlapped COM serial, then a Windows Runtime BLE backend.
4. Give the UI-control protocol a native IPC backend and settle Windows user-data paths.

The build script intentionally stops at the first real unsupported API. It is the regression
driver for this work: each platform slice moves that boundary forward without weakening the
Linux or macOS builds.
