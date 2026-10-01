---
title: MeshCore radios
description: Using MeshClient with a MeshCore companion radio, and what's different from Meshtastic.
---

[MeshCore](https://meshcore.co.uk) is a different mesh firmware for the same LoRa radios.
MeshClient talks to a radio running MeshCore's **companion** firmware as well as to Meshtastic
radios. The tabs, the messages and the node list all work the same way.

## Connecting

No setup is needed.

- **Bluetooth**: a MeshCore radio announces itself as one, and the client speaks MeshCore to
  it.
- **USB and network**: nothing on the port says which firmware is behind it, so the client
  asks in one protocol and, if there's no answer, the other. It remembers which one answered.

:::note
A MeshCore firmware build serves either Bluetooth *or* USB, never both. A Bluetooth build says
nothing on its USB port, so connect to it over Bluetooth.
:::

The PIN for Bluetooth pairing is set under **User**. It takes effect the next time the radio
restarts, so if you set it over USB, set it before you move the radio to Bluetooth.

## What's different

MeshCore and Meshtastic don't do all the same things, so the client only offers what the radio
on the link can do.

**Only on MeshCore:**

- **Log in** to a repeater or room server.
- **Ask for status** or **Ask for neighbours** from a repeater.
- A repeater's conversation works as its console: its commands appear where quick replies
  would be.
- **Forget route**, to clear a contact's stored path.
- **Share with nearby**, to send a contact's advert again.
- **Add as contact**, for a node you've heard but haven't added.

**Only on Meshtastic:** waypoints, reactions, key verification, configuring a node over the
mesh, modules, and the full set of radio settings.

## Settings on MeshCore

Settings shows what MeshCore has:

- **User**: the name, location sharing, who may ask for telemetry, location and sensor
  readings, and which heard nodes are added as contacts.
- **LoRa**: frequency, bandwidth, spreading factor, coding rate and power. A MeshCore change
  applies without a reboot.
- **Channels**: each channel's name and key. A hashtag channel's key comes from its name, so
  anyone who knows the name can join.
