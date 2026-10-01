---
title: Connecting a radio
description: Pair over Bluetooth, plug in over USB, or reach a radio over the network, and how auto-connect chooses.
---

MeshClient connects to a radio in one of three ways:

| Link | Good for | What it needs |
|---|---|---|
| **USB** | the most reliable link; no pairing | a data cable |
| **Bluetooth LE** | a radio in your pocket or on your pack | pairing once, if the radio uses a PIN |
| **Network** | a radio on your Wi-Fi, or `meshtasticd` on a Linux box | the radio's address |

All three run through the **device list**. To open it, go to the **Radio** tab and press
**A** on the Link card's **devices** button.

## Bluetooth

1. Switch the radio on and keep it close.
2. Open the device list. Bluetooth radios are listed below any USB ports.
3. Move to your radio and press **A**.
4. If the radio is in PIN mode, it shows six digits on its screen and MeshClient asks for them.
   Type them in and confirm.

MeshClient handles pairing itself, so you don't need a phone or a system Bluetooth menu.
Pairing only happens when you choose a radio from the list, so auto-connect never pops up a PIN
prompt while you're doing something else.

:::tip[The PIN changed?]
If you changed the radio's PIN, or reflashed it, the old pairing no longer works. In the device
list, press **Y** twice on that radio to forget the pairing, then pair again.
:::

:::caution[Windows and PIN-protected radios]
Windows can't pair with a Meshtastic radio that uses a PIN yet, in either Fixed PIN or Random
PIN mode. A radio set to **No PIN** connects and works normally over Bluetooth. For a radio with
a PIN, connect over USB instead.
:::

On a Mac, the first connection asks for Bluetooth permission. Until you allow it, the client
shows "Bluetooth is starting". If you run the client from a terminal, macOS asks on behalf of
the terminal app.

## USB

Plug the radio in with a cable that carries data. The client connects on its own, with no
pairing and no device list needed. USB ports always sort to the top of the device list.

This works on the Brick even with radios that use their own USB chip, such as the Heltec T114
or a RAK. The Brick doesn't recognise those ports by default, and the client sets them up
itself on every connect.

## Network

A network connection works with an ESP32 radio that has Wi-Fi enabled, or with `meshtasticd`
running on a Linux machine. Both listen on port **4403**.

You can't scan for a network radio, so the last row of the device list holds its address:

- **A** connects to the saved address. If no address is saved yet, it opens the keyboard.
- **Y** opens the keyboard with the current address filled in, so you can change it.
- To stop trying that address, clear the field and press **Done**.

You can type an IP address, such as `192.168.1.50`, or a hostname. A port is optional and
defaults to 4403.

## Auto-connect

Once you've connected to a radio, MeshClient reconnects on its own the next time you start
it. It tries the links in this order:

1. **A radio on USB.** A radio on a cable always wins.
2. **The saved network address**, if there is one. It tries at most once every 30 seconds.
3. **Bluetooth**, from the radios that answered the last scan: your preferred radio, then the
   one you used most recently, then the strongest Meshtastic signal.

Only a radio that is actually advertising nearby counts. A radio you paired with and then left
in another building isn't a candidate, even though the system still lists it.

To stop auto-connect for a while, press **X** twice in the device list to disconnect. The
client stays disconnected until you pick a radio again.

## Disconnects and reboots

- **A reboot after a settings change is normal.** Most changes need the radio to restart. The
  link drops and comes back by itself.
- **Messages queued during a drop are marked as failed.** They stay in the conversation, and
  you can resend them with **START**.
- **Your node list survives a disconnect.** The nodes you've heard stay listed while the radio
  is away.
