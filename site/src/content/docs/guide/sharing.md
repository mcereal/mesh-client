---
title: Sharing channels and contacts
description: Share your channels or your contact details as a QR code, and add one from a link.
---

MeshClient shows channels and contacts as QR codes that the Meshtastic app on a phone can
scan. The Brick has no camera, so to bring a link *in*, you type it.

## Sharing your channels

In **Settings → Channels**, choose **Share these channels**, or **Share this channel** for just
one. The client shows a QR code with the link underneath it. Point a phone's camera or the Meshtastic app at the code to join.

The code carries the primary channel, every secondary channel, and the radio's LoRa settings,
such as the region and preset. A radio that joins with it gets all of that, and its own
channels are replaced.

:::danger[Treat the code like a password]
Anyone who sees this code can read everything on those channels. Show it only to the people
who are joining, and never in front of a camera that's recording.
:::

## Joining channels from a link

On the channel list, choose **Add from a link** and type the link. You only need the part after
the `#`.

## Sharing your contact

Open **Settings → User** and choose **Show my contact code**. The code carries your radio's name, number and public key. A phone that scans it can
send your radio encrypted direct messages straight away, without waiting to hear it on the mesh.

A public key is meant to be shared, so this code is safe to show to anyone. It doesn't contain
your private key, so nobody who scans it can pretend to be your radio.

To add someone else's contact, choose **Add from a link** at the foot of **Settings → User** and
type their link, again only the part after the `#`.

:::note
A contact added from a link starts as **not verified**. A code proves who sent it no more than
a name does. To trust it, do the [spoken key check](../nodes/#verifying-a-key) together.
:::

On a [MeshCore](../meshcore/) radio, the codes are the MeshCore app's: one channel per code, and
`meshcore://contact/add` links for contacts.
