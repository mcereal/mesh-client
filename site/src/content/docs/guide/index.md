---
title: Getting started
description: What MeshClient is, what you need to run it, and where to go next.
---

MeshClient is a mesh radio client for handhelds. It lets you read and send messages, see who
is on the mesh and change your radio's settings from a TrimUI Brick, without a phone. It works
with [Meshtastic](https://meshtastic.org) radios and also with [MeshCore](meshcore/)
companion radios.

The Brick is the main target. Builds for macOS, Windows and a Linux command line also exist,
running the same client.

## What you need

- **Something to run it on.** A TrimUI Brick running NextUI, a Mac or Windows PC, or a Linux
  machine for the command line. See [Installing](install/).
- **A radio.** A Meshtastic or MeshCore node, such as a Heltec, a RAK or a T-Echo. The client
  reaches it over Bluetooth LE, a USB cable, or the network. See
  [Connecting a radio](connecting/).

You don't need a phone at any point, including for setup. You can pair, set your region and
choose your channels from the client.

## The first five minutes

1. [Install](install/) MeshClient from the Pak Store, then open it from **Tools → MeshClient**.
2. Switch your radio on. If it's plugged in over USB, the client connects straight away. If
   it's a Bluetooth radio you've used before, the client finds it on its own.
3. For a new Bluetooth radio, go to the **Radio** tab, press **A** on **devices** and pick the
   radio. If the radio shows a PIN on its screen, type it in.
4. Go to **Settings → LoRa** and check that the **region** is set for your country. A radio
   that has never been set up transmits nothing until it has one.
5. Open **Messages**, choose a channel and press **Y** to write a message.

:::tip
Press **SELECT** on any screen for help with what's on it. For a settings section, help opens
at the row your cursor is on.
:::

## Where to go next

- [Controls](controls/) lists what every button does.
- [Messages](messages/), [Nodes](nodes/) and [Map](map/) cover the main tabs.
- [Settings](settings/) explains how changing your radio's configuration works, and why the
  radio reboots afterwards.
- [Troubleshooting](troubleshooting/) is the place to look when something doesn't connect.
