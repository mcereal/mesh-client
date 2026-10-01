---
title: Troubleshooting
description: Fixes for the most common connection, pairing and update problems, and where to find the log.
---

## The log

When something goes wrong, the log usually says why.

- **On the Brick:** `/.userdata/tg5040/logs/MeshClient.txt` on the SD card.
- **On the command line:** the log goes to the terminal. Add `--log-level debug` for more.

The client's own data, such as settings, the node list and messages, lives in
`~/.meshclient/`.

## Connecting

**The radio doesn't appear in the device list.**
Check that the radio is on and close by, and that it isn't connected to a phone. A Meshtastic
radio talks to one client at a time over Bluetooth. On the Brick, check that Bluetooth is on in
NextUI.

**Pairing fails, or worked once and now doesn't.**
The radio's PIN probably changed, or the radio was reflashed, and the old pairing no longer
matches. In the device list, press **Y** twice on the radio to forget it, then pair again.

**On a Mac, it stays on "Bluetooth is starting".**
MeshClient hasn't been given Bluetooth access. Allow it under **System Settings → Privacy &
Security → Bluetooth**. If you run the client from a terminal, allow the terminal app.

**A USB radio isn't found.**
Try another cable. Many USB-C cables carry power only. If the radio runs MeshCore firmware built
for Bluetooth, it says nothing over USB at all. See [MeshCore radios](../meshcore/).

**The link drops whenever I download maps.**
That's expected on the Brick: Wi-Fi and Bluetooth share an antenna. The radio reconnects when
the download ends. Use USB to stay connected during a download.

**The radio reboots after I save a setting.**
That's normal. Most settings need a restart to take effect, and the client reconnects on its
own.

## Updates

**Check for updates says there's nothing new, but there is.**
A build you made yourself reports a `-dev` version and is never offered updates. Install a
release build from the [download page](/download/).

**On a Mac, the update fails.**
Move MeshClient into **Applications** first. Run from the disk image or from Downloads, macOS
runs a read-only copy that can't be replaced.

## Still stuck?

[Open an issue](https://github.com/mcereal/mesh-client/issues) with what you tried, your
device, and the end of the log.
