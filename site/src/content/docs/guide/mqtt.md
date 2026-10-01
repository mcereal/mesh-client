---
title: MQTT
description: Let your radio reach an MQTT broker through the handheld's Wi-Fi.
---

Meshtastic radios can link the mesh to the internet through an MQTT broker. A radio without
Wi-Fi can't do that by itself, but the handheld it's paired with has Wi-Fi. With **Proxy via
client** on, the radio hands its MQTT traffic to MeshClient, and MeshClient carries it to the
broker and back.

## Turning it on

1. Connect the Brick to Wi-Fi.
2. Open **Settings → Modules → MQTT**.
3. Turn MQTT on, then turn on **Proxy via client**, the second row.
4. Set the broker address, user name and password if you use your own broker. Leave the address
   empty to use the public Meshtastic broker.
5. Save. The radio reboots, and the client starts relaying once it reconnects.

The radio stays in charge of what's sent where. The client only carries the messages.

:::caution
The proxy works only while MeshClient is running and connected to the radio. If you take the
handheld away, the radio loses its route to the broker.
:::

## Checking it works

The radio can't tell whether its messages reached the broker, so the **Radio** tab gets a
**Broker** card while the proxy is on. It shows:

- **Status**: connected, or a reason it isn't, such as a wrong password or an unreachable
  broker.
- **Server**: the broker actually in use, which shows the public broker's name when the address
  setting is empty.
- **Topics**: what the client subscribed to. "none" means no channel has downlink turned on, so
  messages go out but none come back.
- **Dropped**: messages that couldn't be relayed, counted separately for each direction.

A sign-in count that keeps climbing means the connection keeps dropping, even when the card
says connected.

The connection always checks the broker's TLS certificate.
