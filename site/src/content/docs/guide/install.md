---
title: Installing
description: Install MeshClient on a TrimUI Brick, a Miyoo Mini Plus, a Mac, a Windows PC or Linux, and keep it up to date.
---

The [download page](/download/) has a direct link to each file. This page covers the details
behind those links.

## TrimUI Brick

### From the Pak Store

1. On the Brick, open **Tools → Pak Store**.
2. Find **MeshClient** under **Miscellaneous Tools** and install it.
3. Start it from **Tools → MeshClient**.

The Pak Store also installs updates to the whole pak.

### By hand

Download `MeshClient.pak.zip` from the
[latest release](https://github.com/mcereal/mesh-client/releases/latest). The zip holds the
pak's *contents*, not the pak folder, so create the folder first and unzip into it:

```bash
mkdir -p /Volumes/SDCARD/Tools/tg5040/MeshClient.pak
unzip MeshClient.pak.zip -d /Volumes/SDCARD/Tools/tg5040/MeshClient.pak
```

:::caution
If you unzip without creating the folder first, you end up with
`MeshClient.pak/MeshClient.pak/launch.sh`, and the launcher won't run that.
:::

## Miyoo Mini Plus

:::note[Experimental]
The Miyoo build is new, and tested on a Mini Plus running [Onion OS](https://onionui.github.io).
:::

**It connects to radios over the network only.** The Miyoo has no Bluetooth, and its USB port
can't host a radio. It works with an ESP32 radio that has Wi-Fi turned on, including a MeshCore
Wi-Fi companion, or with `meshtasticd`. See [Network](../connecting/#network).

1. Download `MeshClient-miyoomini.zip` and unzip it at the root of the SD card. It holds
   `App/MeshClient/`:

   ```bash
   unzip MeshClient-miyoomini.zip -d /Volumes/SDCARD
   ```

2. In Onion, turn on network time under **Apps → Tweaks → Network**. Without it, the clock
   starts at 1970 and every download fails with *this device's clock is wrong*.
3. Start it from **Apps → MeshClient**, go to **Radio → devices**, and type the radio's address
   on the last row.

The address can also go in `App/MeshClient/env.sh`, one line,
`export MESHCLIENT_TCP_HOST=192.168.1.50:5000`. Unzipping a newer release doesn't replace that
file.

## macOS

:::note[Experimental]
The desktop builds are newer and less tested than the Brick build. If something goes wrong,
[open an issue](https://github.com/mcereal/mesh-client/issues).
:::

You need macOS 11 or later, on Apple silicon or Intel.

1. Download `MeshClient-macos.dmg`, open it, and drag **MeshClient** to **Applications**.
2. Open it from Applications. The first time it looks for a radio, macOS asks whether it can
   use Bluetooth. Allow it.

New releases are signed and notarized. If an older download won't open, allow it under
**System Settings → Privacy & Security → Open Anyway**.

## Windows

You need Windows 10 or later, on x64.

1. Download and run `MeshClient-windows-x86_64-setup.exe`. It installs for your user only and
   doesn't need an administrator.
2. The installer isn't code-signed yet, so SmartScreen may say "Windows protected your PC". If
   it does, choose **More info → Run anyway**.

## Linux (command line)

The Linux download is one static binary. It needs nothing installed on the machine: no Python,
no runtime and no matching glibc.

```bash
curl -LO https://github.com/mcereal/mesh-client/releases/latest/download/meshclient-linux-x86_64
curl -LO https://github.com/mcereal/mesh-client/releases/latest/download/meshclient-linux-x86_64.sha256
sha256sum -c meshclient-linux-x86_64.sha256
chmod +x meshclient-linux-x86_64 && ./meshclient-linux-x86_64 --status
```

The download is x86-64 only. On an ARM machine such as a Raspberry Pi, build the same binary
from source with `make linux-cli`. The `meshclient-tg5040-aarch64` file in each release is the
Brick's build, and a Pi won't run it. [The command line](../cli/) covers what the binary can do.

## Updating

MeshClient updates itself on every platform:

1. Open **Settings → About MeshClient**.
2. Choose **Check for updates**.
3. If there's a new version, press **A** on **Download and install**.
4. Quit and start MeshClient again.

The client downloads the new binary, checks it against the published checksum and swaps it in.
It's safe to do this while the client is running, because nothing changes until you relaunch.

- **You need Wi-Fi.** The client handles HTTPS itself, so it needs no other setup.
- **On a Mac, the app must be in Applications.** If you run it from the disk image or from
  Downloads, macOS runs a read-only copy that can't be updated.
- **Beta builds stay on the beta channel.** A stable build is only offered stable releases.
- **On the Brick, the updater replaces only the binary.** The pak's launch script is not part
  of a self-update. Updating through the Pak Store, or unzipping a new `MeshClient.pak.zip`
  over the old one, replaces the whole pak.
- **On the Miyoo, the same.** Unzipping a new `MeshClient-miyoomini.zip` replaces the whole app
  and keeps your `env.sh`.
