# MQTT client proxy

A Meshtastic radio normally reaches an MQTT broker itself. One with `proxy_to_client_enabled`
set does not: it hands the connected client a `MqttClientProxyMessage` — a topic and a payload it
has already decided on — and expects the client to put that on a broker, and to hand back
anything arriving on the topics it is interested in.

The radio stays the origin. This client is only the box with a route to the internet, which on a
Brick is the whole point: the radio has LoRa and Bluetooth and no WiFi worth the name, and the
handheld it is paired with has WiFi.

**This is finished.** A radio that asks for it gets a broker connection, the right subscriptions,
and both directions relayed; the Status tab grows a Broker card that says whether it worked; and
Settings → Modules → MQTT → **Proxy via client** is the press that asks for it in the first
place. Nothing about it needs a phone any more.

## The pieces

| File | What it is |
|---|---|
| inkwell's `src/codec/mqtt.c` | the MQTT 3.1.1 wire format, and nothing else - it was never about a mesh, so it is the platform layer's |
| `src/core/net/mqtt_proxy.c` | one broker connection: resolve, connect, subscribe, publish, keepalive, backoff |
| `src/core/net/tls_client.c` | Mbed TLS over a non-blocking descriptor |
| `src/proto/mqtt_topic.c` | where a mesh lives on a broker, matched to the firmware |
| `src/core/session/session.c` | the two hooks: the radio's message out, the broker's message back |
| `src/app/app_mqtt.c` | the decision: whether to be connected, to what, with which subscriptions |
| `include/mesh/ui/store_mqtt.h` | what the Status screen is told about it |
| `third_party/mbedtls-config/mesh_mbedtls_config.h` | what this build of Mbed TLS is and is not (TLS and X.509) |
| `third_party/mbedtls-config/mesh_psa_crypto_config.h` | the same, for the crypto half 4.x split out |

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
<root>/2/e/<channel>/+        one per downlink-enabled channel, deduplicated
<root>/2/e/PKI/+              once, if any channel downlinks at all
```

`<root>` is `MQTTConfig.root` or `msh`. `<channel>` is the channel's name, or — for the unnamed
default primary, which is most of them — the name of the modem preset. `+` rather than `#`
because the one level left open is the gateway node id, which is what the firmware asks for.

**Two channels can be one topic.** Because an unnamed channel derives the preset's name, a radio
carrying the usual unnamed primary *and* an unnamed secondary derives `LongFast` twice, as does
one with a channel named after its own preset. The second is not a second subscription, so
`mesh_session_mqtt_filter()` enumerates distinct ids and skips the repeat without counting it.
Deduplicating at the far end instead is what the client used to do by accident: the proxy
refused the repeat with `-EEXIST` and the log went on claiming one more subscription than had
ever been made. `mqtt_session_subscribes_once_to_a_repeated_channel` and the two cases beside it
hold the rule, including that the channel *behind* a repeat is still reached.

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

## The client decides nothing it can read off the radio

`src/app/app_mqtt.c` re-derives the whole arrangement from `MQTTConfig` on every loop turn:
whether to be connected, to what, and with which subscriptions. There is no client-side broker
setting and there should not be one — the radio is the origin, it decides where its mesh is
published, and a client with its own idea of the broker would be publishing this mesh somewhere
its own radio is not listening. The only say the client has is `MESHCLIENT_MQTT_PROXY=0`, which
turns the whole thing off from a shell.

Deriving it every turn rather than reacting to a change is what removes the class of bug where a
setting moves by a path nobody wired up. The cost is a few hundred bytes of comparison per turn.

### The credentials are all-or-nothing

This is the rule most likely to be "fixed" into a bug. The firmware's `PubSubConfig` reads the
username and password *only* inside `if (*config.address)`:

| `MQTTConfig.address` | Connects to | As |
|---|---|---|
| empty | `mqtt.meshtastic.org` | `meshdev` / `large4cats`, whatever the config's own username says |
| set | that address | that username and password, **including empty ones** |

Substituting only the address sends `meshdev` to somebody's private broker. Substituting only
when empty sends a blank username to the public one, which refuses it — and a refusal at that
point looks from the Brick exactly like a broker that is down. The Android proxy carries the same
rule for the same reason.

### The client id is not the radio's

The firmware presents a bare `!abcd1234` when it dials a broker itself. This client presents
`meshclient-!abcd1234`, and the prefix is the point: a broker does not report a duplicate client
id, it disconnects the older holder. Without the prefix, the moment somebody turned proxying off
this client and that radio would both be live under one id, taking turns evicting each other —
during exactly the window in which the user is watching to see whether the change worked.

### A changed configuration is a reconnect

Any of the five things a CONNECT carries moving — address, username, password, client id,
TLS — drops the connection and remakes it, as does any change to the filter set.

A new *filter* could be added to a live connection, but the set can also **shrink**, and
`mesh_mqtt_proxy_clear_filters()` deliberately does not unsubscribe on the wire: a channel whose
downlink was just turned off would keep delivering until the connection went away by itself.
Reconnecting is also nearly free in practice, since every way this set can change is a settings
write, and a settings write reboots the radio and takes the link with it.

The link dropping stands the broker connection down too. `handshake.config_complete` is the gate
for starting at all — the module config and the channel table arrive as separate fragments, so
anything started earlier would connect with an address about to change — and it is also the one
field a link reset clears. With no radio there is nothing to publish and nothing that could take
a delivery; the only thing an open socket would still be doing is holding this client's id at
the broker.

## Nothing else on the client can tell you it is broken

The radio cannot see this connection. It hands over a `MqttClientProxyMessage` and is told
nothing about what happened to it — `publishQueuedMessages()` runs every 200 ms whether or not a
broker is reachable — so a proxy that is failing looks, from the radio and from every other
screen here, exactly like one that is working. That is what the **Broker card** on the Status tab
is for.

It is drawn only when the radio has asked to be proxied for. Almost no radio has the setting on,
and the Status column is the one place in this client that runs out of room, so a card saying
"Off" on every Brick in the world would have been four rows spent on nothing.

Four situations it exists to tell apart, none of which any other screen distinguishes:

| What the card shows | What is actually wrong |
|---|---|
| `Status` red, a reason under it | the broker refused or is unreachable; the reason names it |
| `Connected, 9 sign-ins` | the link is flapping — every single frame of that says "Connected" |
| `Topics  none - no channel downlinks` | nothing is wrong with MQTT; no channel has downlink on, so traffic goes out and none comes back |
| `The radio is asking… not holding one` | `MESHCLIENT_MQTT_PROXY=0` on this client, which the radio cannot know |

Two rows are worth their space for less obvious reasons. **Server** is the broker the *plan*
resolved, which the radio's own Settings screen cannot draw: an empty `MQTTConfig.address` means
the public broker, that substitution happens here, and the Settings field a reader has already
looked at is blank. **Dropped** keeps outbound and inbound apart — outbound is this client
refusing a publish, which on a dead link is every position report the radio makes; inbound is a
broker message that reached the radio's doorstep and no further. One total would have averaged
the two.

The card carries no verb. Nothing on the Brick can reach a broker to retry it, and the retry is
already on a backoff of its own.

## Turning it on

**Settings → Modules → MQTT → Proxy via client**, the second row, under the toggle that turns
MQTT on at all. It writes `proxy_to_client_enabled` and nothing else on this side: the proxy is
re-derived from `MQTTConfig` every loop turn, so what starts it is the radio's own reply — the
config sync that follows the reboot a settings write causes.

It was a read-only row for three phases, and the argument for that was real: turning it on takes
the radio's MQTT off its own WiFi and hands it to the attached client, and until this client
spoke the protocol the client was dropping every message. What is still true is that the client
has to be *running and connected*, which is a handheld somebody can walk away with — and that is
what the Broker card is for.

```bash
make ui-capture ARGS="devtools/ui_capture/scenes/broker.scene -o broker.gif"
```

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

`src/core/net/tls_client.c` is where this process does TLS: the broker connection here, and every
HTTPS request through `src/core/net/fetch.c`. A broker connection is long-lived and
bidirectional, and a download is megabytes arriving while the screen still has to draw, so both
have to be readable and writable between UI frames on the same epoll loop as everything else.
There is nowhere to block.

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
- **A session ticket is not a failure and is not `-EAGAIN` either.** TLS 1.3 sends its tickets
  *after* the handshake, so they arrive on the application stream and `mbedtls_ssl_read()`
  reports one by returning `MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET`. Treating that as an
  error drops every healthy connection — which is what happened against `mqtt.meshtastic.org`
  once Mbed TLS moved to 4.1, since 4.x enables TLS 1.3 and session tickets by default.
  Answering it with `-EAGAIN` is the other tempting fix and fails the same way as the bullet
  above: the broker's CONNACK usually shares the flight with the ticket, so it is already inside
  the session with an empty socket underneath. `mesh_tls_client_read()` retries the read instead.
  `mqtt_proxy_reads_past_a_session_ticket` holds this, and the fixture broker issues real tickets
  so the handshake tests exercise the post-handshake path at all — an Mbed TLS server with no
  ticket callback configured sends none, which is why a real handshake under test still missed it.
  That retry is bounded by `MESH_TLS_TICKETS_PER_READ`, because `MQTT_READS_PER_TURN` counts
  calls *into* `tls_client.c` and cannot bound work that never returns from one; hitting the cap
  hands back `-EAGAIN` with `more_to_read` set, and the proxy resumes on its own next turn, in
  `GREETING` as well as `READY`. Mbed TLS reports at most one ticket per read in practice, so the
  cap is defensive rather than exercised — `mqtt_proxy_survives_a_run_of_tickets` says so.

### Certificates are always verified

There is no insecure mode and no argument that turns one on, and no state with nothing to verify
against: Mozilla's roots are compiled into the binary (`include/mesh/core/ca_roots.h`), so the
Brick having no `/etc/ssl` does not matter. `mqtt_proxy_verifies_against_built_in_roots_without_a_bundle`
holds that line, and `mqtt_proxy_refuses_an_unknown_certificate` holds the other half, because a
client that trusts everything passes a "TLS works" test just as well as one that does not.

**The roots ship with the binary**, which is the point of compiling them in: a file in the pak
does not ship through self-update, so an install updated in place would keep its first pak's roots
until a rotation on the far end broke the connection. The roots are parsed once, on the first TLS
connection, and shared by every session after it.

`SSL_CERT_FILE` names a bundle to use *instead* — for a broker behind a private CA. A path that
is set and unusable fails naming the path rather than falling back
(`mqtt_proxy_names_a_missing_bundle`).

Refreshing the roots is re-downloading `third_party/mozilla-ca/cacert.pem` from
[curl.se/ca](https://curl.se/ca/cacert.pem) and running `scripts/gen-ca-roots.py`; `make test`
fails while the two disagree.

### Mbed TLS is a submodule, and a trimmed one

`third_party/mbedtls`, pinned to a 4.1 LTS tag. `git submodule update --init --recursive` is
required — without it the build still works and says so, and `mesh_tls_available()` reports
false, which makes MQTT over TLS the one thing that build cannot do.

`.gitmodules` tracks the `mbedtls-4.1` branch, which is a statement to dependabot rather than to
git: a submodule with no branch configured is followed on the remote's *default* branch, and Mbed
TLS writes 4.2 and later on `development`. That is how a 3.6.7 pin came back as a 4.2.0 bump that
could not build. It changes nothing about what checks out — the pinned SHA does that, and only
`git submodule update --remote` follows a branch.

**Mbed TLS 4.x is two projects.** TLS and X.509 are the outer one; every primitive, the PSA API
and the RNG live in the `tf-psa-crypto` submodule nested inside it. That is why the config is a
pair — `mesh_mbedtls_config.h` is the `MBEDTLS_USER_CONFIG_FILE`, `mesh_psa_crypto_config.h` is
the `TF_PSA_CRYPTO_USER_CONFIG_FILE` — and why the link line names `tfpsacrypto` where 3.x named
`mbedcrypto`. Both files are included *after* the library's own config and **subtract** from it.
That direction is deliberate. A hand-written replacement config starts from nothing and has to
enumerate every primitive a handshake might need, and the failure mode of getting that list wrong
is not a build error — it is a client that works against the broker the author tested and fails
against somebody else's with a cipher suite mismatch nobody can read.

Removed: DTLS, the pre-shared-key exchanges, the library's own blocking socket and timer, its
self-tests, Camellia/ARIA, and the Koblitz and Brainpool curves. Several things this used to
remove by hand are simply gone from 4.x's default and no longer appear in either file — 3DES, the
key exchanges without forward secrecy, and the sub-256-bit NIST curves.

Kept, against the instinct to trim: `MBEDTLS_ERROR_C`, because `mbedtls_strerror()` is what turns
a handshake failure into a sentence and `-0x2700` on a handheld is not an answer; and
`MBEDTLS_SSL_SRV_C`, because it is what lets `tests/suites/mqtt_proxy.c` stand a real broker on a
loopback socket and complete a real handshake against it, with no network and no `openssl` in the
container.

**4.x generates sources at build time.** `error.c`, the SSL debug helpers and the PSA driver
wrappers are not committed to the submodule the way 3.6's were, so the build runs upstream's
generators and needs Python with `jinja2` and `jsonschema` to configure at all. `make setup`,
`docker/Dockerfile` and CI all install them; a checkout that predates this will fail at the
generate step rather than the compile.

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

inkwell's `tests/suites/codec_mqtt.c` checks the codec against byte arrays written out from the
specification, because a codec tested only by round-tripping its own output agrees with itself
and can still be wrong about the wire.

`tests/suites/mqtt_proxy.c` stands a real listening socket on loopback and speaks MQTT back at
the proxy by hand — a real non-blocking connect, real partial reads, a real EOF when the broker
hangs up. The clock is synthetic throughout, which is the only reason the five-second backoff and
the ninety-second silence timeout are testable at all. The TLS cases run a real Mbed TLS server
against a certificate generated for that file and valid until 2120.

`tests/suites/mqtt_app.c` covers the decisions above without opening a socket — the plan, the
credential rule, the client id and the filter set are all pure functions of a session, which is
the reason they were written as pure functions of a session.

`devtools/fuzz/fuzz_mqtt_packet.c` runs the proxy's own reader loop — decode, skip, take, advance
— over arbitrary bytes, because fuzzing the decoder without that loop would miss the case that
matters, which is a skip that miscounts.

```bash
./build/debug/tests/meshclient_core_tests --filter mqtt
./scripts/fuzz.sh mqtt_packet
```
