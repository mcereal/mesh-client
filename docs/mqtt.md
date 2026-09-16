# MQTT client proxy

A Meshtastic radio normally reaches an MQTT broker itself. One with `proxy_to_client_enabled`
set does not: it hands the connected client a `MqttClientProxyMessage` — a topic and a payload it
has already decided on — and expects the client to put that on a broker, and to hand back
anything arriving on the topics it is interested in.

The radio stays the origin. This client is only the box with a route to the internet, which on a
Brick is the whole point: the radio has LoRa and Bluetooth and no WiFi worth the name, and the
handheld it is paired with has WiFi.

**The connection and the protocol are both here.** What is still missing is the last wire:
nothing constructs a proxy or hands it to a session yet, and the "proxy via client" toggle in
settings is still read-only.

## The pieces

| File | What it is |
|---|---|
| `src/proto/mqtt_packet.c` | the MQTT 3.1.1 wire format, and nothing else |
| `src/core/mqtt_proxy.c` | one broker connection: resolve, connect, subscribe, publish, keepalive, backoff |
| `src/core/tls_client.c` | Mbed TLS over a non-blocking descriptor |
| `src/proto/mqtt_topic.c` | where a mesh lives on a broker, matched to the firmware |
| `src/core/session.c` | the two hooks: the radio's message out, the broker's message back |
| `third_party/mbedtls-config/mesh_mbedtls_config.h` | what this build of Mbed TLS is and is not |

The codec/client split is the same one `stream_framing.c` and `stream_link.c` already have: the
format is a pure function over bytes, testable against hand-written packets with no broker in
sight, and the client is the state machine that owns a socket.

## Publishing is a relay; subscribing is not

The two directions are not symmetric, and the asymmetry is the reason `src/proto/mqtt_topic.c`
exists at all.

**Outbound, the radio has already decided everything.** `MqttClientProxyMessage` carries the
topic the radio built, the payload it encoded and the retain flag it wants, and this client
copies all three onto the broker without reading any of them. That is the whole contract: the
client is a route to the internet, not a second opinion about what should be on it. A payload
here is a `ServiceEnvelope` that may well be encrypted with a key this client does not hold.

**Inbound, there is no message to follow.** The firmware's `MQTT::sendSubscriptions()` is
wrapped entirely in `#if HAS_NETWORKING`, so a radio proxying through a client never subscribes
to anything and never says what it *would* have subscribed to. The client has to derive the
topics itself, from the same configuration the radio used.

That makes topic derivation a compatibility surface rather than a design. Every string has to
match what the firmware would have produced, character for character:

```
<root>/2/e/<channel>/+        one per downlink-enabled channel
<root>/2/e/PKI/+              once, if any channel downlinks at all
```

`<root>` is `MQTTConfig.root` or `msh`. `<channel>` is the channel's name, or — for the unnamed
default primary, which is most of them — the name of the modem preset. `+` rather than `#`
because the one level left open is the gateway node id, which is what the firmware asks for.

The failure mode if any of this is wrong is the reason it is pinned by tests: a filter that is
merely *sensible* subscribes to a topic nobody publishes on, and the mesh publishes fine and
receives nothing. Nothing logs an error, because as far as MQTT is concerned everything worked.

### Two names that look like typos and are not

`mesh_mqtt_preset_name()` reproduces `DisplayFormatters::getModemPresetDisplayName()`, including
the two entries somebody will eventually try to correct:

- **`LONG_MODERATE` is `LongMod`**, while every other long name is spelled out.
- **`VERY_LONG_SLOW` has no case in the firmware's switch**, so it falls through to the default
  and comes out as **`Invalid`**. A radio still on that preset — deprecated in 2.5 — genuinely
  publishes to `.../2/e/Invalid/...`.

A radio that is not using a preset at all (`use_preset` false, bandwidth set by hand) is
`Custom`, which the firmware decides before it looks at the preset field.

The same rule covers the root topic, which is concatenated and **not normalised**: the firmware
writes `moduleConfig.mqtt.root + "/2/e/"` with no slash handling, so a radio configured with
`msh/` publishes to `msh//2/e/...`. Tidying that up here would subscribe to a topic this radio
is not using.

### Two gates worth knowing about

`mesh_session_send_mqtt_proxy()` refuses before the config sync finishes. That is the firmware's
own rule mirrored rather than guessed: `PhoneAPI` drops the variant outright while it is still
handshaking, so sending earlier spends a round trip to have the message discarded in silence.

It also refuses a payload over 435 bytes, which is what `MqttClientProxyMessage.data` holds. A
broker can send more, and an encrypted envelope cannot be truncated — half of one decodes to
nothing — so an oversized message is dropped whole and counted. This is the same bound the
[skip](#an-oversized-message-is-skipped-not-buffered) above exists to survive, seen from the
other end.

## 3.1.1, QoS 0, clean session

Three choices, all made to match what the radio's own client would have done.

**3.1.1 rather than 5.0.** The firmware speaks 3.1.1 and every broker still accepts it. 5.0 adds
a property system to every packet for something this never uses.

**QoS 0 in both directions.** The firmware publishes at QoS 0, so nothing downstream expects an
acknowledgement, and the mesh underneath offers no delivery guarantee for a QoS 1 ledger to
preserve. A PUBLISH that arrives at QoS 1 is still decoded correctly — the packet id sits between
the topic and the payload, so a reader that ignored QoS would hand on a payload two bytes off —
and then treated like any other. It is forwarded but not acknowledged.

**A clean session, always.** The alternative asks the broker to hold undelivered messages while
we are away, which for a proxy is precisely wrong: what it would hold is a queue of mesh traffic
to replay at a radio whose users walked out of range of whatever it was about. For the same
reason nothing is queued locally across a disconnect — a publish offered while the broker is
unreachable is dropped and counted. A partial *write* is different and is buffered; that is one
packet mid-flight, not a backlog.

Because the session is clean, the broker remembers no subscriptions, so the whole filter set goes
out again on every reconnection. Filters are sent one at a time, which is what makes a refusal
attributable: a broker answers a multi-filter SUBSCRIBE with one code per filter in order, and
the thing a refusal has to say is *which topic*, since that is the channel whose traffic will
silently never arrive.

## An oversized message is skipped, not buffered

`mesh_mqtt_decode_header()` decodes a packet's fixed header without its body, and this is the
reason. A broker may retain a message far larger than anything a radio could accept — a
`MqttClientProxyMessage` holds a 60-byte topic and a 435-byte payload — but the bytes still have
to come off the stream in order. MQTT has no resynchronisation: one packet read at the wrong
offset and every packet after it is plausible nonsense.

So a body past `MESH_MQTT_PACKET_MAX` is *counted* off the stream without ever being held, and
the inbound buffer stays sized for what the radio can take rather than for the 256 MB a remaining
length can describe. `mqtt_proxy_skips_an_oversized_message` sends a 6 KB message followed by an
ordinary one and checks that the ordinary one arrives intact — one byte out either way and it
would not.

The decoder also refuses a reserved packet type, an illegal flags nibble, and a fifth
continuation byte in a remaining length. Those checks are cheap and they are the *only* detection
a framed protocol gets that the reader has drifted.

## TLS

`src/core/tls_client.c` is the one place this process does TLS itself. Everything else it fetches
over HTTPS is a forked curl (`src/core/fetch.c`), because a fetch is a request and a reply and a
child process is a fine way to do one. A broker connection is not: it is long-lived,
bidirectional, and has to be readable and writable between UI frames on the same epoll loop as
everything else. There is nothing to fork and nowhere to block.

So Mbed TLS is driven through BIO callbacks over a non-blocking socket, reporting `-EAGAIN` all
the way up. Two things about that are easy to get wrong and are handled deliberately:

- **Which direction it is blocked on is not a property of what the caller asked for.** A
  handshake flight, and a write that triggers one, can block waiting to *read*. Arming the wrong
  epoll event parks the connection forever. `mesh_tls_client.wants_write` is set by the library's
  own answer, not guessed.
- **A reader must loop until `-EAGAIN`.** One TLS record can hold more plaintext than one call
  returns, and the leftovers live inside the session rather than in the socket — so the
  descriptor is empty, epoll has nothing to report, and a reader that stops after one call waits
  forever on data it already has. The proxy bounds its read loop so a busy broker cannot starve
  the UI, and sets `more_to_read` when it stops early so the next tick comes back.

### Certificates are always verified

There is no insecure mode and no argument that turns one on. A missing CA bundle is a refusal,
not a downgrade — `mqtt_proxy_will_not_do_tls_without_a_bundle` holds that line, and
`mqtt_proxy_refuses_an_unknown_certificate` holds the other half, because a client that trusts
everything passes a "TLS works" test just as well as one that does not.

The bundle is **not** resolved here. `mesh_fetch_resolve_ca_bundle()` already works it out — an
environment override, then the bundle shipped inside our own pak, then the system locations,
because the Brick has no `/etc/ssl` at all — and `mesh_mqtt_proxy_set_ca_bundle()` takes the path
somebody else arrived at. Two modules working it out separately is two answers that can disagree.

> **The pak's CA bundle does not ship through self-update.** Only the bare binary does, so a
> self-updated client keeps whatever bundle its original pak carried. A certificate rotation on
> the far end then starts failing with nothing on screen to connect it to, which is why the
> failure names the bundle rather than saying "TLS failed". Treat it as a compatibility boundary,
> the same as `launch.sh`.

### Mbed TLS is a submodule, and a trimmed one

`third_party/mbedtls`, pinned to a 3.6 LTS tag. `git submodule update --init --recursive` is
required — without it the build still works and says so, and `mesh_tls_available()` reports
false, which makes MQTT over TLS the one thing that build cannot do.

`third_party/mbedtls-config/mesh_mbedtls_config.h` is a `MBEDTLS_USER_CONFIG_FILE`: it is
included *after* the library's own config and **subtracts** from it. That direction is
deliberate. A hand-written replacement config starts from nothing and has to enumerate every
primitive a handshake might need, and the failure mode of getting that list wrong is not a build
error — it is a client that works against the broker the author tested and fails against somebody
else's with a cipher suite mismatch nobody can read.

Removed: DTLS, pre-shared-key and static key exchanges, the library's own blocking socket and
timer, its self-tests, 3DES/Camellia/ARIA, and the sub-256-bit, Koblitz and Brainpool curves.
Kept, against the instinct to trim: `MBEDTLS_ERROR_C`, because `mbedtls_strerror()` is what turns
a handshake failure into a sentence and `-0x2700` on a handheld is not an answer; and
`MBEDTLS_SSL_SRV_C`, because it is what lets `tests/suites/mqtt_proxy.c` stand a real broker on a
loopback socket and complete a real handshake against it, with no network and no `openssl` in the
container.

If a handshake ever fails on a named group, the curve list in that file is the first place to
look.

## Failure, and what it says

Every failure goes through one function, which closes the descriptor *and* arms a retry — a close
without a retry is a proxy that is silently off with the setting still on. Backoff doubles from
five seconds to a five-minute cap, slower than the radio link's: a radio that is out of range
comes back when the user walks back into the room, while a broker refusing us will still be
refusing us in a second, and the far end is somebody else's server.

The failure count is cleared by a CONNACK and by nothing earlier. Clearing it on a completed TCP
connect is the tempting bug, and it turns a broker that rejects the password into a five-second
retry loop against somebody else's machine, forever.

The refusals are kept apart rather than collapsed, because they are different things to do next:

| CONNACK | What it means | What the user does |
|---|---|---|
| 4 | bad username or password | fix a setting on the radio |
| 5 | not authorised | fix an account on the broker |
| 3 | broker unavailable | wait; it is somebody else's outage |
| other | protocol-level | nothing; the number is reported |

A broker that sends a packet only a client sends is fatal, and deliberately so: it means the
stream is being read at the wrong offset, and dropping the connection is the only recovery MQTT
has. A *subscription* the broker refuses is not fatal — ACLs permitting some topics and not
others is an ordinary configuration, and dropping the link would cost every filter that works.

## Testing

`tests/suites/mqtt_packet.c` checks the codec against byte arrays written out from the
specification, because a codec tested only by round-tripping its own output agrees with itself
and can still be wrong about the wire.

`tests/suites/mqtt_proxy.c` stands a real listening socket on loopback and speaks MQTT back at
the proxy by hand — a real non-blocking connect, real partial reads, a real EOF when the broker
hangs up. The clock is synthetic throughout, which is the only reason the five-second backoff and
the ninety-second silence timeout are testable at all. The TLS cases run a real Mbed TLS server
against a certificate generated for that file and valid until 2120.

`devtools/fuzz/fuzz_mqtt_packet.c` runs the proxy's own reader loop — decode, skip, take, advance
— over arbitrary bytes, because fuzzing the decoder without that loop would miss the case that
matters, which is a skip that miscounts.

```bash
./build/debug/tests/meshclient_core_tests --filter mqtt
./scripts/fuzz.sh mqtt_packet
```
