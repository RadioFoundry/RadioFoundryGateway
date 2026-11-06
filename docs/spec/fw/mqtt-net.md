# MQTT network

Covers: calls between boxes, apps and a private-call router over one MQTT broker, with no master: topics, the binary
call message, node state and events, arbitration, private-call routing, transports, security, and what phase 1 builds.
Status: phase 1 built, 2026-11-02 (§1.1): `common/src/mqttnet.c`, run by rdofradio and the board
(`esp_idf/main/mqttnet_task.c`). Run on the Pluto by rdofradio, 2026-11-05: a full duplex repeater's calls up to the
network and back out from another node on both slots, speech, every voice burst in order; the board's task is built,
not yet run on hardware. Today's MQTT calls (`{root}/calls/...`, DMRD packets, [mqtt.md](mqtt.md) §10) stay the
default; this replaces them later.
Trunked sites (Tier III, [trunk.md](trunk.md)) use the same calls and topics, plus a call setup item (§9.4),
registration records (§15), node status (§16) and provisioning from a manager (§17); a Tier II box needs only the
setup answer (§9.4). The format stays 3: it is a draft until released.
Related: [config-v3.md](config-v3.md) (talk paths, policies, networks), [dmr-core.md](dmr-core.md) (PDU layouts),
[mqtt.md](mqtt.md) (today's topics), [ipgw.md](ipgw.md) (data reassembly, confirmed ACK).

## 1. Phase 1: what to build now

| In | Out (later phases, §14) |
|---|---|
| The call message (§4): build and parse every item, in `common/` C, with host tests against the vectors (§13) | MQTT-SN, SCTP, QUIC, WebSocket (§5) |
| The topics (§3) and node JSON (§6, §7) | `bundle` above 1 when sending (receiving 1–3 is in) |
| Arbitration, TG subscriptions, the jitter buffer (§8) | The voter |
| Delivery of `dl` streams on the path byte (§9.3) | Retries of confirmed data on delivery (§4.4) |
| Confirmed data answered on RF by the hearing box (§4.4) | The router, the Python library, the legacy adapter: separate programs |
| `mqtt/tcp` and `mqtt/tls` with the existing `mqtt` keys | Roaming transceivers on this network: they stay on HBlink (§14) |
| `format: "v3"` beside today's DMRD calls (§10) | |

Order: rdofradio first (it already carries MQTT calls, [mqtt.md](mqtt.md) §10), then the board.

### 1.1 What's built

| Part | Where |
|---|---|
| The message, its items, the topics, `info`, `activity`, the TG subscriptions | `common/src/mqttnet.c`, `rdof/mqttnet.h`; host tests in `host-tests/test_mqttnet.c` |
| Uplink: a receiver's tracker frames to messages (late entry held, CSBKs counted, data calls whole or in parts, `quality`) | `rdof_mn_up_t` |
| Downlink: messages to bursts for the router's transmitters, the jitter buffer, arbitration, a header made when voice comes first | `rdof_mn_down_t`, then `rdof_siteroute_burst_on` |
| `data_result` for data this box delivered | `rdof_mn_results_*` |
| rdofradio: `format: "v3"`, `--file RX[,TX]` to run it with no radio | `sdr/cli/rdofradio.c` |
| The board: one connection, every receiver's tracker, the router's transmitters, `/api/status` `mqtt_net` | `esp_idf/main/mqttnet_task.c` |
| The vectors (§13) and seven RF scenes, for the router and the apps to test against | `host-tests/fixtures/mqtt-net/vectors.json` |
| For Tier III (2026-11-06): the `setup` item and the Tier II answer (§9.4), registration records, `where`, `status`, the trunk object of `info`, the trunk events, provisioning with its cache, commands | `rdof/mqttnet.h`, `common/src/mnctl.c` (`rdof/mnctl.h`); the Tier II answer and its `setup` events in `rdof_mn_setup_tier2`, run by the board (`mqttnet_task.c`) and rdofradio |

Decisions taken while building it:

- **Connection.** Its own, client ID `rdof-net-{node}`, will `{root}/node/{node}/online` `"0"` (retained, QoS 1),
  `"1"` and `info` on connect. MQTT 5 with No Local on every subscription; TCP_NODELAY (the board sets it on
  esp-mqtt's socket once connected).
  The node is the box's `name`.
- **Subscriptions.** `dl/{node}` and `tg/{tg}/+` for each TG a transmitting path carries (or `tg/+/+` when one
  carries `*`), refreshed every 5 s: held `ptt` TGs come and go. A subscribe or unsubscribe the client refuses is
  tried again a second later.
- **`dl` path 0xFF.** The box picks: the router's last-heard table for a private call, the paths whose policy carries
  the TG for a group call. It does not pick by slot.
- **`activity` `path`** is `null` when no path applies (an app, a flood). The box reads no other node's activity.
- **Late entry.** Voice held until an embedded LC gives the addresses (at most 6 bursts), then sent with the late
  flag. A receiver of voice with no `lc_header` makes the header itself; the embedded LC applies one superframe late.
- **Late messages.** The jitter buffer drops a message whose seq is 128 or more behind (a wrap, not a reorder).
- **CSBK preambles** fold into one `csbk` with a count. **DPF data** drops its CRC-32 and pad.
- **Confirmed data.** The hearing box answers ACK only (no selective NAK yet). The delivering box waits 1 s for the
  radio's response after its transmission ends and 5 s for it to go on air; `tries` is 1.
- **Priority** (flag bit 3) is carried, not acted on. **`info.paths`** lists the paths on this network only.
- **paths_rev** is CRC-16/CCITT-FALSE (init 0xFFFF, poly 0x1021), as the router computes it.
- **A message with no items** is dropped. `origin` source IDs are little endian, as the header. `seq` counts every
  item, `quality` included; `quality` is last.
- **Settings.** `format`, `bundle` and `raw_tx` need `calls`; `transport: "mqtt/tls"` is `tls: true`.

## 2. The idea

Every RadioFoundry Gateway box, and every app (parrot, ARS server, transcriber, ...), connects to one MQTT broker.
There is no master.

- A box publishes what it hears on RF: group calls on `{root}/tg/{tg}/{node}`, private calls on
  `{root}/pc/{dst}/{node}`.
- A box subscribes to the talkgroups its paths carry (`{root}/tg/{tg}/+`) and to its own downlink (`{root}/dl/{node}`).
- A private-call router knows where each radio was last heard. It reads `{root}/pc/#` and forwards each private call
  down the right box's `{root}/dl/{node}`.
- Boxes publish their state and a decoded event feed under `{root}/node/{node}/...`. Most apps need only that feed.
- Apps are nodes without RF. An app can claim DMR IDs (the parrot claims 9990) and gets their private calls directly.
- Calls cross as decoded content (AMBE frames, LCs, data PDUs), never as coded bursts: the box that hears a call
  strips the FEC, the box that transmits adds it back. One message can hold several items.

```
           RF                      broker                                cloud / apps
 radio ──► box A ── DMR/tg/91/A ───────────────► box B, box C (sub DMR/tg/91/+) ──► RF
 radio ──► box A ── DMR/pc/9801234/A ──────────► router ── DMR/dl/C ──► box C ──► RF
 radio ──► box A ── DMR/pc/9990/A ─────────────► parrot (claims 9990, sub DMR/pc/9990/+)
                                                 parrot ── DMR/pc/{caller}/parrot ──► router ──► DMR/dl/A
           box A ── DMR/node/A/activity ───────► router (last heard), dashboards, location map
```

`{root}` is the network's `root` (default `DMR`). The examples write it as `DMR`. A test box uses a test root such as
`test/DMR`.

## 3. Topics

### 3.1 Names

| Name | What it is | Rules |
|---|---|---|
| `{node}` | a box (its config `name`) or an app | unique on the root; letters, digits, `.`, `_`, `-`; at most 39 characters. A box whose name breaks the rule doesn't start the network (logged) |
| `{tg}` | a talkgroup | decimal, no leading zeros; all-call is 16777215 |
| `{dst}` | the called radio (or app) ID | decimal |
| stream ID | one transmission | u32, random, made by whoever publishes the stream |

### 3.2 Calls

| Topic | Who publishes | Who subscribes | Payload | QoS | Retain |
|---|---|---|---|---|---|
| `DMR/tg/{tg}/{node}` | a box (RF-heard group traffic), an app | boxes carrying the TG, apps (transcriber, recorder) | call message (§4) | 0 | no |
| `DMR/pc/{dst}/{node}` | a box (RF-heard individual traffic), an app | the router; an app claiming `{dst}` | call message | 0 | no |
| `DMR/dl/{node}` | the router only | that node | the `pc` message, with the path byte set to the destination's path (§9.1) | 0 | no |

- "Individual traffic" is everything addressed to one ID: voice, CSBKs (radio check, call alert) and data.
- A node drops its own messages (topic ends in `/{node}`, or MQTT 5 No Local).
- `{node}` is in the topic, not the source ID: a publisher's node never changes, so topic aliases still work, and the
  broker's ACL can check who publishes what (§11). The source ID is in the payload, at a fixed offset.
- The talk path is in the payload (the header's path byte), not the topic. In the topic it would multiply each node's
  topics by its paths, using up topic aliases, and every subscriber's filter would grow a level. Nothing subscribes
  by path.

### 3.3 Node state (`DMR/node/{node}/...`)

| Topic | Payload | QoS | Retain | When |
|---|---|---|---|---|
| `online` | `1`, or `0` (the will) | 1 | yes | `1` right after connecting; `0` from the broker when the connection drops |
| `info` | JSON (§6) | 1 | yes | at connect and on any config change |
| `activity` | JSON events (§7) | 1 | no | each call start / end, CSBK, completed data, talker alias, GPS |
| `status` | JSON (§16) | 1 | yes | every 10 s and on change; today's status ([mqtt.md](mqtt.md) §6) stays where it is too |
| `cmd` | JSON (§17.3) | 1 | no | from the manager |
| `cmd/res` | JSON (§17.3) | 1 | no | the node's answer to a `cmd` |
| `ms/{id}` | JSON (§15); an empty payload deletes | 1 | yes | a trunked site: each radio it serves, on every change and at most once a minute for a refresh |

`online` stays separate from `status` so the will can't overwrite a full status with an empty one.

### 3.4 Router

| Topic | Payload | QoS | Retain | When |
|---|---|---|---|---|
| `DMR/router/{id}/online` | `1` / `0` (will) | 1 | yes | as a node's |
| `DMR/router/{id}/status` | JSON: radios known, calls routed, floods, drops | 1 | yes | every 60 s |
| `DMR/where/{src}` | JSON: `node`, `path` (`{i, name}`), `at`, `rssi`, `source` (`reg`: a registration record, §15; `heard`: traffic) | 1 | yes | when a radio's location changes (a lookup for apps; a trunked site drops a record that a newer one elsewhere replaced) |

### 3.5 Subscriptions, by participant

| Participant | Subscribes to | Publishes to |
|---|---|---|
| Box | `DMR/tg/{tg}/+` for each TG it carries (§8.3), `DMR/dl/{node}` | `DMR/tg/+/{node}`, `DMR/pc/+/{node}`, `DMR/node/{node}/#` |
| Trunked site | a box's, plus `DMR/cfg/net`, `DMR/cfg/node/{node}`, `DMR/cfg/tg/+`, `DMR/cfg/ms/+`, `DMR/node/{node}/ms/+` (its own records, read back at start), `DMR/node/{node}/cmd`, `DMR/where/+` | a box's |
| Router | `DMR/pc/#`, `DMR/node/+/activity`, `DMR/node/+/info`, `DMR/node/+/online`, `DMR/node/+/ms/+`, `DMR/cfg/ms/+` | `DMR/dl/+`, `DMR/where/+`, `DMR/router/{id}/#` |
| Manager | `DMR/node/+/#`, `DMR/where/#`, `DMR/router/+/#` | `DMR/cfg/#`, `DMR/node/+/cmd`, `DMR/manager/{id}/online` |
| Listen-only app (transcriber) | `DMR/tg/{tg}/+` | `DMR/node/{app}/online`, `info` |
| App with an ID (parrot, ARS) | `DMR/pc/{claimed}/+` | `DMR/pc/+/{app}` (and `DMR/tg/+/{app}` if allowed), `DMR/node/{app}/#` |
| Dashboard, location map | `DMR/node/+/#`, `DMR/where/#` | nothing |

### 3.6 Provisioning and the manager (§17)

| Topic | Payload | QoS | Retain | Publisher |
|---|---|---|---|---|
| `DMR/cfg/net` | the network's document | 1 | yes | the manager |
| `DMR/cfg/node/{node}` | one node's document | 1 | yes | the manager |
| `DMR/cfg/tg/{tg}` | one talkgroup's | 1 | yes | the manager |
| `DMR/cfg/ms/{id}` | one radio's | 1 | yes | the manager |
| `DMR/manager/{id}/online` | `1` / `0` (will) | 1 | yes | the manager |

## 4. The call message

Every message on `tg`, `pc` and `dl` is one of these: a 14-byte header, then one or more items. The version byte is 3,
after the raw burst's 1 and 2 ([mqtt.md](mqtt.md) §5), so a reader of either can tell them apart.

**Decoded, not coded.** The box that hears a call removes the FEC (BPTC, trellis, Reed-Solomon, the CRCs) and checks
it. The box that transmits rebuilds the bursts: FEC, CRCs, sync, slot type, its own color code, EMB. None of that
crosses the network, so apps never need FEC code, and bit errors don't pile up from hop to hop.

**Several items, one message.** A voice burst and the embedded LC that completed with it go together; a whole data
call goes as one item. A reader skips item types it doesn't know, so new types can be added later.

**Byte order.** The header's multi-byte fields and the `gps` item's are little endian. Everything taken from a PDU
(LCs, CSBKs, data headers, AMBE frames, user data) keeps its on-air order: octets as [dmr-core.md](dmr-core.md) §6
numbers them, bits MSB first ([dmr-core.md](dmr-core.md) §1). IDs inside an LC are big endian, as on air.

### 4.1 Header (14 bytes)

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 1 | version = 3 | a reader drops other versions |
| 1 | 1 | flags | bit 0 heard on slot 2 (RF only; information); bit 1 group (else individual); bit 2 emergency; bit 3 priority; bit 4 privacy (PI header seen); bit 5 late entry (no voice LC header; addresses from the embedded LC); bits 6–7 zero |
| 2 | 1 | path | the talk path index (§6): on `tg` / `pc`, the path it was heard on; on `dl`, the path to send it on. 0xFF: none (apps, bridges, floods) |
| 3 | 1 | seq | the number of this message's first item; every item counts one; per stream, from 0, wraps at 256 |
| 4 | 4 | stream ID | u32 |
| 8 | 3 | src | u24, always set |
| 11 | 3 | dst | u24, always set |

A reader expects the next message's seq to be this one's plus its item count. A gap is loss; a lower number (on a
datagram transport) is a late message, put back in order by the jitter buffer (§8.4).

### 4.2 Items

Each item is `[type u8][length u8][value]`. A length of 255 means a u16 length (little endian) follows, for values of
255 bytes or more. Types 0x80–0xFE are for experiments and vendors; 0xFF is reserved.

| Type | Item | Value | Item size | Comes from |
|---|---|---|---|---|
| 0x01 | `voice` | voice index u8 (A–F = 0–5), then 27 bytes: the three 72-bit AMBE+2 channel frames, as on air (vocoder FEC included). The 27 bytes are burst bits 0–107 then 156–263, packed MSB first: `rdof_ambe_frames` order ([dmr-core.md](dmr-core.md) §7) | 30 | a voice burst |
| 0x02 | `lc_header` | the 9-byte full LC ([dmr-core.md](dmr-core.md) §6.1); RS(12,9) checked and removed | 11 | a voice LC header (sent once, however many times it was on air; again every 240 ms while only headers come, §4.3) |
| 0x03 | `terminator` | the 9-byte full LC | 11 | a terminator with LC (once); ends the stream |
| 0x04 | `embedded_lc` | the 9-byte LC from the superframe just completed; checksum checked and removed. Any FLCO: voice LC, talker alias, GPS, vendor | 11 | bursts B–E, sent in burst F's message |
| 0x05 | `pi_header` | 10 bytes; CRC checked and removed | 12 | a privacy indicator header |
| 0x06 | `csbk` | octets 0–9 ([dmr-core.md](dmr-core.md) §6.2; CRC removed), then a count u8 (how many were heard: preambles) | 13 | one or more identical CSBKs |
| 0x07 | `data_pdu` | part u8 (bits 0–6 the part number, bit 7 more parts follow); rate u8 (0 ½, 1 ¾, 2 rate 1); header count u8; the data headers, octets 0–9 each ([dmr-core.md](dmr-core.md) §6.3; CRC removed); the user data | 5 + 10 × headers + data | a whole data call (§4.4) |
| 0x08 | `talker_alias` | format u8 (as the DMR alias header), then the alias in UTF-8 (converted by the box) | 3 + text | the alias, once complete; information only |
| 0x09 | `gps` | lat i32, lon i32 (1e-7 degree), position error code u8 | 11 | an embedded GPS LC; information only |
| 0x0A | `quality` | bits corrected u8 (sum over this message's bursts, saturating), RSSI u8 (−dBm, average; 0 not measured) | 4 | added to RF-heard messages only |
| 0x0B | `origin` | source network ID u32, source repeater ID u32, hop count u8 | 11 | a stream that came in through a bridge to another network, in its first message; bridges pass it on with one more hop |
| 0x0C | `setup` | op u8, reason u8, flags u8, service options u8, ref u32 (§9.4) | 10 | a private call's setup across the network; the only item of its message |
| 0x7F | `raw` | data type u8, then the 33-byte burst as heard | 36 | a burst the box can't decode (reserved data types, single-burst reverse channel) |

### 4.3 Rules

- **One stream per message.** Items are in on-air order.
- **src and dst are always set.** A box that comes in late (no voice LC header) holds the voice until the embedded LC
  gives the addresses (at most 6 bursts, 360 ms), then sends the held items with the late entry flag. Without an LC
  after a superframe it drops the stream.
- **The transmitting box rebuilds from the LC items,** never from `talker_alias` or `gps`. Those two are for apps; a
  box may leave them out in phase 1 (the `embedded_lc` items carry the same LCs).
- **Repeats are the transmitter's choice.** One `lc_header`, one `terminator`, one `csbk` with a count. The box that
  transmits sends as many repeats and preambles as its own config says.
  The one exception: a run of headers longer than a stream's 0.5 s with nothing (§8.4; a modem or radio may send
  them for half a second or more before the voice) would end the stream at the far end before its voice. While only
  headers are heard, the `lc_header` item goes again when nothing has gone for 240 ms; a receiver takes it as the same
  stream's and sends no second header for it.
- **Voice can be bundled.** A sender may put up to `bundle` voice items in one message (1–3). Every receiver takes 1–3.
  Everything else goes at once and never waits: a header or terminator ends the bundle early.
- **`raw`** is passed through so nothing is lost silently. A box transmits it as-is (with its own color code) or
  drops it (`raw_tx`, default drop).
- **The path byte.** On `tg` and `pc`, a box sets it to the path the call was heard on; the router learns private-call
  locations from it (§9.1). A box transmitting a `tg` stream picks its own paths (those whose policy carries the TG,
  config-v3.md §9.2). On `dl`, the router sets it to the destination's path, and the box transmits there.
- **Priority** (flag bit 3) marks a stream that should take a path from a stream in progress: emergency calls, an
  announcement app. Who may set it is in §11.

### 4.4 Data calls

The box that hears a data call collects the whole call, checks every CRC, and sends it as one `data_pdu` item: the
headers and the user data, without block serial numbers, CRC-9s, the CRC-32 or pad (the reassembly
[ipgw.md](ipgw.md) already does). The box that transmits rebuilds blocks to follow, pad, serial numbers and CRCs for
the rate in the item, and recomputes the header fields that depend on them (blocks to follow, pad octets).

- **Large calls.** A data call can be up to about 3 KB. On a datagram transport (§5), a message stays under 1200
  bytes, so a larger call goes in parts: the same stream ID, `part` 0, 1, 2, ... with bit 7 set on all but the last.
  The headers are in part 0 only. A receiver that misses a part drops the call and counts it.
- **Confirmed data is answered hop by hop.** The box that hears a confirmed data call answers the radio on RF itself
  (ACK, or a selective NAK for missing blocks) as soon as it has the call, then publishes it. It answers the way
  [ipgw.md](ipgw.md) §10 does. The box that delivers it sends it to the destination radio as confirmed data and
  reports the result in `activity` (`data_result`, §7). Phase 1 sends it once; retries come later.
  - Why: a radio waits `T_RspnsWait` for the response, 180 ms recommended (2 s minimum on simulcast; TS 102 361-3
    annex A.1), too short for a trip through the broker. ETSI leaves relaying between base stations out of scope
    (TS 102 361-3 §5).
  - Exception: if the destination was last heard on the same path, the radios exchange the confirmation themselves
    (a repeater repeats it), and the box only publishes.
  - The sending radio's ACK means "the network has it" (Tier III's `Message_Accepted`), not "the radio has it"
    (`MS_Accepted`, TS 102 361-4 §6.6.4.1.5); Tier II has no way to send the second later. End-to-end delivery is the
    application's job (TS 102 361-3 §5.2): text apps carry their own acknowledgment, which travels back as an
    ordinary data call, or watch `data_result`.
- **Response packets** a radio sends (its ACK or NAK of data a box delivered) are used by that box and not published.

### 4.5 Sizes

| Message | Bytes |
|---|---|
| One voice burst (+ `quality`) | 14 + 30 + 4 = **48** |
| Burst F with its embedded LC | 14 + 30 + 11 + 4 = **59** |
| Three voice bursts, bundled | 14 + 3 × 30 + 4 = **108** (36 a burst) |
| Call start (`lc_header`) or end (`terminator`) | 14 + 11 + 4 = **29** |
| An ARS registration (30 bytes of IP/UDP), with its preambles | 14 + 13 + (5 + 10 + 30 + 2) + 4 = **78**, one message. As coded bursts, the same call is about 14 publishes |

## 5. Transports

The topics, the payloads and every rule here are the same on every transport. Phase 1 builds `mqtt/tcp` and
`mqtt/tls`; the rest is the plan.

| `transport` | What it is | Wire bytes, one voice burst (48-byte message, IPv4) | Good for | Where |
|---|---|---|---|---|
| `mqtt/tcp` (default) | MQTT 3.1.1 or 5 over TCP | about 107, plus TCP ACKs back | LAN, wired links | everywhere |
| `mqtt/tls` | the same, TLS 1.2/1.3 | about 129 | wired links over the internet | everywhere |
| `mqtt/wss` | MQTT over WebSocket over TLS | about 135 | browser apps, dashboards | apps |
| `mqtt/quic` | MQTT over QUIC | about 114 | Linux boxes on changing links | Linux boxes, apps; brokers that support it (EMQX, NanoMQ) |
| `mqtt-sn/udp` | MQTT-SN 1.2 over UDP, through an MQTT-SN gateway next to the broker | about 82 | cellular and metered links | ESP32, Linux, apps |
| `mqtt-sn/dtls` | the same, DTLS 1.2 | about 119 | the same, over the internet | ESP32, Linux, apps |
| `mqtt-sn/sctp` | MQTT-SN over SCTP over UDP (RFC 6951), on `tinysctp` | about 112 | lossy links: PR-SCTP lets voice expire instead of arriving late | ESP32, Linux |

- On TCP and TLS, one lost packet holds up everything behind it until it's resent, often 200 ms or more. UDP, QUIC and
  SCTP let the jitter buffer deal with the gap instead.
- With `mqtt/tcp` and `mqtt/tls`, TCP_NODELAY is on, so a message never waits in the socket for the next one.
- MQTT 5 where the broker has it, with topic aliases for the node's own `tg` / `pc` topics; 3.1.1 otherwise (the
  fallback in [mqtt.md](mqtt.md) §1).
- MQTT-SN, when built: each topic registered once per connection, `PINGREQ` every 20 s (under UDP NAT timeouts),
  messages under 1200 bytes so IP never fragments them.

## 6. `info` (retained)

What a node is and what it carries. The router uses `paths` and `claims`; dashboards use the rest.

`paths` lists the box's talk paths, as config v3 resolves them ([config-v3.md](config-v3.md) §6.1, §11): each is one
place a call comes in or goes out, so the slot is part of the path where the slots are separate.

| What the box has | Talk paths |
|---|---|
| A repeater (an input / output pair) | 2: `<name>/1`, `<name>/2` |
| A DCDM frequency | 2: `<name>/1`, `<name>/2` |
| A simplex frequency | 1: transmits on its `ts`, hears both slots |
| An SFR frequency | 1 (the radio times the slots) |
| A frequency with `tx: false` | 1, both slots heard, `tx_ok: false` |

```json
{
  "schema": 1,
  "node": "w12-lab",
  "kind": "box",
  "name": "RadioFoundry Gateway W12, lab",
  "firmware": "0.9.0+3df8112",
  "dmr_id": 9801001,
  "transport": "mqtt/tls",
  "bundle": 1,
  "location": {"lat": 35.12345, "lon": -80.12345, "source": "gnss"},
  "paths_rev": 2754,
  "paths": [
    {"i": 0, "name": "rpt1/1", "app": "repeater", "rx": 438.25, "tx": 438.75, "cc": 1, "ts": 1,
     "tx_ok": true, "tg": "91,3100", "ptt": "3101-3199", "pc": "*"},
    {"i": 1, "name": "rpt1/2", "app": "repeater", "rx": 438.25, "tx": 438.75, "cc": 1, "ts": 2,
     "tx_ok": true, "tg": "31672", "ptt": "", "pc": "*"},
    {"i": 2, "name": "438.1", "rx": 438.1, "cc": 1, "ts": null, "tx_ok": false, "tg": "*", "ptt": "", "pc": "*"}
  ],
  "claims": []
}
```

| Field | Meaning |
|---|---|
| `schema` | this JSON's version |
| `kind` | `box`, `app`, `bridge` (an outside network through an adapter) or `router` |
| `transport`, `bundle` | how this node connects (§5) |
| `paths_rev` | u16, CRC-16/CCITT-FALSE over, for each path in order, its index byte, its name in UTF-8 and a zero byte (65535 for none); changes whenever the list does |
| `paths[].i` | the index in the header's path byte: the path's position in the resolved view, from 0; at most 63 |
| `paths[].name` | the path's name in config v3 |
| `paths[].app` | the app that offers the path (`repeater`), or absent for a frequency |
| `paths[].rx`, `tx`, `cc`, `ts` | the frequencies (MHz), color code and slot; `ts` null for a path that hears both slots and doesn't transmit |
| `paths[].tx_ok` | the path can transmit; only these can deliver a private call or answer confirmed data |
| `paths[].tg`, `ptt`, `pc` | the path's resolved policy, in config v3's ID-set syntax ([config-v3.md](config-v3.md) §3, §8); only paths whose `net` is this network are listed |
| `claims` | IDs this node answers for (apps, and a box's own `parrot` / `ipgw` apps); §9.2 |
| `paths[].trunk` | `true` on a trunked site's path ([trunk.md](trunk.md)): the network addresses the site, never a slot |
| `trunk` | a trunked site only: its system code, control channel and carriers, as below |

```json
"trunk": {"system": {"model": "small", "net": 5, "site": 3, "par": "ab", "area_bits": 5},
          "tscc": [{"chan": 1, "tx_hz": 438750000, "rx_hz": 438250000, "cc": 1}],
          "carriers": [{"chan": 1, "tx_hz": 438750000, "rx_hz": 438250000},
                       {"chan": 2, "tx_hz": 438600000, "rx_hz": 438500000}],
          "services": ["reg", "group", "private", "allcall"], "reg": true}
```

A trunked site builds its neighbor list (Adjacent_Site broadcasts) from the `trunk` objects of the other online
nodes with the same `model` and `net`, unless its `cfg/node` document lists `neighbors`.

## 7. `activity` events

One JSON object per message. Every event has `ev`, `at` (UTC ISO 8601, ms), `node`, `path` (the index, §6; `null`
when none applies), `slot`
(heard on), `source` (`rf` heard here, `net` sent here from the network) and `stream_id`. Apps that read several boxes
merge duplicates by `call_key`.

| `ev` | When | Fields beyond the common ones |
|---|---|---|
| `call_start` | voice LC header, or the late-entry LC | `src, dst, group, emergency, privacy, call_key, rssi, tx_ok` (this path can answer) |
| `call_end` | terminator, or 0.5 s with no burst | `reason` (`end`, `lost`), `duration`, `ber`, `rssi {min, max, avg}`, `bursts`, `lost_bursts` |
| `talker_alias` | the alias is complete | `src, alias` |
| `gps` | embedded-LC GPS (FLCO 8) | `src, lat, lon, error_m` |
| `csbk` | a CSBK (preamble repeats folded) | `src, dst, group, opcode, desc` (as [mqtt.md](mqtt.md) §3.3) |
| `data` | a data call that passed its CRCs | `src, dst, group, sap` (`ip`, `short`, `proprietary`, ...), `confirmed`, `bytes`, `payload` (base64, up to 512 bytes), and for IP: `ip {src, dst, proto, sport, dport}` |
| `data_fail` | a data call that didn't | `src, dst, reason` |
| `data_result` | this box finished delivering a data call on RF | `src, dst, from` (the publisher's `{node}`), `from_stream`, `result` (`delivered`, `failed`, `no_response`, `unconfirmed`), `tries` |
| `tx` | this box starts / stops transmitting a network stream | `action`, `src, dst, from` |
| `reg` | a trunked site: a radio registers, de-registers, expires, or is denied or refused | `src, action` (`register`, `deregister`, `expire`, `deny`, `refuse`), `tg` (array) |
| `grant` | a trunked site grants a payload channel | `src, dst, group, chan` (logical channel), `lcn` (0 or 1), `late` (a late entry grant); `source` `rf` or `net` (who asked) |
| `queue` | a trunked site queues a request, serves it, or drops it | `src, dst, group, action` (`in`, `out`, `drop`), `why` (on a drop) |
| `setup` | a call setup sent or received (§9.4) | `src, dst, op, reason, peer` (the other node) |

On a trunked site, `call_start` and `call_end` carry `chan` and `lcn`; `path` is the site's one path and `slot` the
payload channel's slot.

An RSSI the receiver didn't measure is `null` in `call_start` (never 0, which would read as 0 dBm), and `call_end`
leaves its `rssi` out. A reader takes `null`, and 0 from an older sender, as unknown: an unknown RSSI never wins a
best-RSSI comparison (§9.1).

`call_key` = `"{src}-{dst}-{group?'g':'p'}-{t}"`, where `t` is the start time in UTC seconds rounded down to a
multiple of 2. Two boxes hearing one call make the same key (or keys 2 s apart near a boundary: a reader checks both).

The router learns locations only from `call_start`, `csbk` and `data` with `source: "rf"`.

## 8. Calls without a master

Each receiving box decides for its own paths, with the same rules everywhere, so the boxes mostly agree.

### 8.1 Arbitration, per path

Config v3's rules decide which calls a path carries and which wins a busy path ([config-v3.md](config-v3.md) §9.2):
a listed TG beats one matched only by `*`, the call already on the path keeps it, an emergency beats both. In
addition:

1. A stream holds the path until its terminator, or 0.5 s with no message, plus the path's hang time (a repeater's;
   0 on other paths). During the hang time only the same TG, or a reply to the last talker, may take the path.
2. A priority stream (flag bit 3, from a node allowed to set it, §11) takes the path as an emergency does. The box
   ends the one on air with a terminator.
3. RF heard on the path beats network traffic for it.

The broker delivers each publisher's messages in order but doesn't promise one order across publishers. Two key-ups
within a few ms can win on different boxes. That's rare, and the same happens across HBlink4 peers.

### 8.2 Duplicate uplinks

Two boxes that hear one radio both publish the call, with different stream IDs. A receiving box treats a second
stream with the same src, dst and type that starts within 1 s of the first as the same call and ignores it (rule
"the call already on the path keeps it" does this already). Apps merge by `call_key`.

### 8.3 Talkgroups a box subscribes to

- A path's `tg` set: the box subscribes to `DMR/tg/{tg}/+` for each TG at connect. A set with `*`, or with more than
  16 TGs after expanding its ranges, subscribes to `DMR/tg/+/+` instead and filters locally.
- A path's `ptt` set: the box subscribes to a TG when an RF user keys it, and unsubscribes when the hold
  (`ptt_hold_s`) ends with no other path holding it.

### 8.4 Jitter buffer

A box transmitting network traffic holds 2 bursts (120 ms) before the first, plus 60 ms for each extra burst the
publisher bundles, then plays one burst per 60 ms. It puts messages back in seq order. A burst that hasn't arrived
when it's due is replaced by silence (voice); a stream with nothing for 0.5 s ends.

## 9. Private calls and apps

### 9.1 The router

A separate program; the only participant with global state: where each radio was last heard. It is the "shared
last-heard table" of [config-v3.md](config-v3.md) §14.

**Where a radio is.** Each radio's entry is `{node, path, at, rssi}`. The router learns it from the header of each `pc`
stream (the topic gives the node, the path byte the path) and from `activity` (§7). It looks the index up in the
node's `info` (`paths`) for `tx_ok`. A sighting counts only if the path can transmit. Sightings within 300 ms of each
other (several boxes hearing one key-up) are compared, and the best RSSI wins. An entry expires after `pc_hold_s`
(default 900 s, as a box's own table); so does every entry on a node whose `paths_rev` changes or whose `online`
goes to 0.

**Routing one stream.** At the first message of a new `pc` stream it decides, and caches the decision by
`{publisher, stream ID}` for the rest of the stream:

| Destination | Action |
|---|---|
| Claimed by a node (`info.claims`) | nothing: the claimant subscribes to its own `pc` topic |
| Known, on another node | forward each message to `DMR/dl/{that node}`, with the path byte set to the destination's path |
| Known, on the node that published it | nothing: the box delivered it locally (§9.3) |
| Unknown or expired | the flood policy: `drop` (default), send to every box with a `tx_ok` path (path byte 0xFF), or `bridge:{node}`: send to an outside network's bridge |

Every message of the stream carries the path, so a lost first message (on a datagram transport) doesn't lose the
route.

**Redundancy.** Run two routers. Each subscribes to `activity`, `info` and `online` normally, so both keep the full
table. They take `DMR/pc/#` as a shared subscription (`$share/router/DMR/pc/#`, MQTT 5), so each message goes to one
of them. If one goes down, the broker sends everything to the other.

### 9.2 Apps

An app is a node without RF. It connects with its own `{node}`, publishes `online` (with a will) and `info`
(`kind: "app"`), and has its own ACL. An app with a DMR ID (parrot 9990, an ARS or location server) lists it in
`info.claims` and subscribes to `DMR/pc/{id}/+`. To answer a radio, it publishes `DMR/pc/{radio}/{app}` with src = its
ID; the router sends it down to the box where the radio is.

| App | Reads | Sends | What it handles itself |
|---|---|---|---|
| Last heard, dashboard | `DMR/node/+/activity`, `info` | — | JSON only |
| Location map | `activity` `gps` and `data` (LRRP), `DMR/where/#` | location requests | the LRRP payload |
| TG transcriber, recorder | `DMR/tg/{tg}/+` | — | `voice` items → its AMBE decoder |
| Parrot | `DMR/pc/9990/+` | `DMR/pc/{caller}/parrot` | the same `voice` items, with a new header and new LC items |
| Announcement, TTS | — | `DMR/tg/{tg}/{app}` with priority | AMBE frames from its encoder, LC items, 60 ms timing |
| ARS server | `DMR/pc/{ars id}/+` | ACKs, queries | `data_pdu` items: the IP/UDP payload |
| Text message / IP data server | `DMR/pc/{id}/+` | messages to radios or TGs | `data_pdu` items; `data_result` for delivery |

None of them needs FEC code. Confirmed data is answered by the boxes (§4.4), so an app's response time doesn't
matter to the radio. A client library (Python first) does the header, items, LCs, data headers, stream IDs and
60 ms timing; it has no vocoder.

### 9.3 On the box

- **A call heard on RF** follows config v3 ([config-v3.md](config-v3.md) §9.1). One sent to this network goes out on
  `tg` or `pc` with the path byte set.
- **A `dl` stream** goes out on the path in its path byte, if that path's policy and `dir` allow it. With 0xFF (a
  flood), it uses its own last-heard table (config-v3.md §9.3), else drops the stream. The path then follows the
  arbitration (§8.1).
- **Local delivery.** A box that hears a private call for a radio in its own last-heard table delivers it there on RF
  right away, as it does today, and still publishes it so the router learns the caller's location.
- **A box's own apps** (`parrot`, `ipgw`) take calls to their IDs as today and are listed in `claims`.

### 9.4 Call setup (the `setup` item)

A trunked site checks that a called radio is there before it grants the caller a channel (Tier III's availability
check). When the called radio isn't registered at the site, the check goes over the network as a setup message: a
call message on `pc/{dst}/{node}` with no voice, its header individual (src the caller, dst the called radio) and one
`setup` item. The router routes it like any private message.

| Offset | Field | Values |
|---|---|---|
| 0 | op | 1 `request`, 2 `accept`, 3 `reject`, 4 `alerting`, 5 `cancel`, 6 `answered` |
| 1 | reason | 0 ok, 1 `not_found`, 2 `busy`, 3 `no_answer`, 4 `refused` (by the radio or policy), 5 `not_supported`, 6 `congestion` (no channel), 7 `timeout` |
| 2 | flags | bit 0 `foacsu` (the called radio is alerted first), bit 1 `unconfirmed` (the answerer couldn't ask the radio: a Tier II box); bits 2–7 zero |
| 3 | service options | as the trunking random access's: bit 7 emergency, bit 6 privacy, bits 1–0 priority |
| 4 | ref | u32 little endian: the stream ID of the `request` this message answers; 0 in a request |

A setup item whose length isn't 8, or whose op is 0 or above 6, makes the message invalid: a reader drops it whole,
and a box never takes it for a stream. Other reasons are kept as they come (a reader names one it doesn't know `?`).

An answer goes back on `pc/{caller}/{node}`: src the called radio, dst the caller, its own stream ID, ref the
request's. The path byte is the path the answerer will use (0xFF none).

```
site A: pc/3100456/A   request, stream S, src 3100123, dst 3100456
router: dl/B           (3100456 registered at B, §15)
site B: asks the radio, grants it a channel
site B: pc/3100123/B   accept, ref S
router: dl/A
site A: grants the caller; the voice then goes as an ordinary private call both ways
```

- The caller's site waits up to 5 s for an answer, then gives up and sends `cancel`. `alerting` keeps it waiting for
  `answered` (or the site's own timer).
- **Every box with a transmitting path:** a `request` on its `dl` is answered at once: `accept` with `unconfirmed`
  when the called radio was last heard on a path of this network that can transmit, else `reject not_found`. A Tier
  III radio can call a radio on a Tier II repeater this way. A box never answers a setup it got on `tg` or `pc`.
  Each setup it gets on `dl` goes in `activity` as a `setup` event, and so does its answer (`path` the answer's
  path, `peer` `router`). Code: `rdof_mn_setup_tier2`.
- **The router:** a `request` for a radio it can't place gets `reject not_found` from the router on
  `dl/{publisher}`, unless its flood policy sends the request on; then the first `accept` wins and the caller's site
  sends `cancel` to any later acceptor.
- A setup message never becomes bursts: the downlink hands it to the box's handler (`rdof_mn_down_t.setup`).
- A private call from a Tier II radio to a Tier III radio needs no setup: the trunked site does the check itself
  when the voice arrives ([trunk.md](trunk.md) §3.6).

## 10. Config

The `mqtt` network entry ([config-v3.md](config-v3.md) §7.2) gains:

| Key | Default | Meaning |
|---|---|---|
| `format` | `dmrd` | with `calls`: `dmrd` is today's DMRD calls ([mqtt.md](mqtt.md) §10); `v3` is this document |
| `bundle` | 1 | voice items per message when sending (1–3; phase 1: 1) |
| `raw_tx` | false | transmit `raw` items as heard (§4.3) |
| `transport` | `mqtt/tcp`, or `mqtt/tls` when `tls` is set | later, the other transports (§5) |

```json
{"type": "mqtt", "host": "broker.example.net", "root": "DMR", "tls": true, "calls": true, "format": "v3"}
```

The router's settings (`pc_hold_s`, the flood policy, the claims and priority allowlists) are its own config.

## 11. Security

- Encryption on the wire: TLS on anything crossing the internet. A username / password or client certificate per node.
- ACLs, by participant:

| Participant | Publish | Subscribe |
|---|---|---|
| Box `X` | `DMR/tg/+/X`, `DMR/pc/+/X`, `DMR/node/X/#` | `DMR/tg/#`, `DMR/dl/X`, `DMR/node/X/cmd`, `DMR/node/X/ms/+`, `DMR/cfg/net`, `DMR/cfg/node/X`, `DMR/cfg/tg/+`, `DMR/cfg/ms/+`, `DMR/where/#` |
| Router | `DMR/dl/#`, `DMR/where/#`, `DMR/router/{id}/#` | `DMR/pc/#`, `DMR/node/+/#`, `DMR/cfg/ms/+` |
| Manager | `DMR/cfg/#`, `DMR/node/+/cmd`, `DMR/manager/{id}/#` | `DMR/node/+/#`, `DMR/where/#`, `DMR/router/+/#` |
| App `P` | `DMR/node/P/#`, plus what it's allowed: `DMR/pc/+/P`, `DMR/tg/{list}/P` | as needed |
| Read-only (dashboard) | none | `DMR/node/+/#`, `DMR/where/#` |

- Only the router publishes on `dl` and `where`. Only the manager publishes `cfg` and `cmd`. No one but `X`
  publishes as `X` (its `ms` records and `cmd/res` included).
- A node trusts `cfg` because of the ACL. Signed documents (a manager key the node checks) are a later step: a
  `sig` key is reserved and ignored.
- An app's src and claims are checked by the router against a list (its config), so an app can't take over a radio's
  ID.
- The priority bit is honored only from boxes (emergency) and from nodes in an allowlist (box and router config).
- Per-client rate limits on the broker (for example 40 messages/s per node, about 2 slots) cap a misbehaving node.

## 12. What happens when something fails

| Failure | Result |
|---|---|
| A box loses the broker | the broker publishes its `online` = 0. RF on that box keeps working locally. The router drops its entries |
| The router is down | group calls are unaffected. Private calls between boxes stop; private calls inside one box still work (local delivery). With two routers, the other takes over |
| The broker is down | everything is local only. Boxes reconnect every 2 s and resubscribe |
| Messages lost | seq shows the gap; the transmitting box fills voice with silence and ends after 0.5 s with nothing. A data call that misses parts is dropped and counted |
| Clock off | doesn't affect calls (no timestamps in messages). `call_key` and `activity` times need NTP or GNSS time |

## 13. Test vectors

The C reproduces these byte for byte (host tests). The header vectors below are normative. They and the rest, made
from real bursts, are in `host-tests/fixtures/mqtt-net/vectors.json` (format in `host-tests/fixtures/README.md`).

Common: stream 0x12345678, src 9801234 (0x958E12), heard on path 1, `quality` 3 bits corrected at −87 dBm.

Call start, group call to TG 91, seq 0 (next message seq 2). The LC is `rdof_group_lc(9801234, 91, 1, 0)`:

```
03 02 01 00 78 56 34 12 12 8E 95 5B 00 00   header: v3, group, path 1, seq 0, stream, src, dst 91
02 09 00 00 00 00 00 5B 95 8E 12            lc_header: FLCO 0, FID 0, SO 0, dst 00005B, src 958E12 (big endian)
0A 02 03 57                                 quality
```

Voice burst A of that call, AMBE+2 silence (`AMBE_SILENCE`, [dmr-core.md](dmr-core.md) §10.2) in all three frames,
seq 2:

```
03 02 01 02 78 56 34 12 12 8E 95 5B 00 00
01 1C 00 B9 E8 81 52 61 73 00 2A 6B B9 E8 81 52 61 73 00 2A 6B B9 E8 81 52 61 73 00 2A 6B
0A 02 03 57
```

Private call to 9990 (0x2706), call start, seq 0:

```
03 00 01 00 78 56 34 12 12 8E 95 06 27 00
02 09 03 00 00 00 27 06 95 8E 12
0A 02 03 57
```

The same call's first message as the router forwards it on `dl` to a box where 9990's path is 0: identical but byte 2
= `00`.

A setup request (§9.4) from 3100123 (0x2F4DDB) to 3100456 (0x2F4F28), stream 0x12345678, no path:

```
03 00 FF 00 78 56 34 12 DB 4D 2F 28 4F 2F   header: v3, individual, path 0xFF, seq 0
0C 08 01 00 00 00 00 00 00 00               setup: request, ok, no flags, no options, ref 0
```

A Tier II box's answer, the radio heard on its path 1, stream 0x0BADCAFE:

```
03 00 01 00 FE CA AD 0B 28 4F 2F DB 4D 2F   header: path 1, src 3100456, dst 3100123
0C 08 02 00 02 00 78 56 34 12               setup: accept, ok, unconfirmed, ref 0x12345678
```

The fixture adds: three bundled voice items, a `raw` item, an unknown item type skipped and an `origin`; and scenes
(every message and event of a call heard on RF): a group call (burst F with `embedded_lc`, the talker alias, the
terminator), a late-entry stream, a `pi_header` call, a radio check (CSBK with count), an unconfirmed data call and
two confirmed ones, one in parts (with the u16 length).

## 14. Later

| Item | Notes |
|---|---|
| Other transports | MQTT-SN (UDP, DTLS; a gateway next to the broker: EMQX's built-in or Paho's), then SCTP over `tinysctp`, QUIC on Linux boxes |
| `bundle` 2–3 when sending | for cellular links |
| Confirmed data retries | on delivery, with `tries` in `data_result` |
| More in `status` (§16) | per path `on_air` and held `ptt` TGs, and `net` counters (messages, seq gaps, reorders, jitter underruns) |
| Roaming transceivers | HBlink4 tunes a roamer per stream (`DMRT` / `DMRK`). On this network a box tunes its own transmitters from the path byte ([transmitters.md](transmitters.md) §4), so no `tune` message is needed |
| The voter | best copy of each burst from several boxes, republished as one stream |
| An early "undeliverable" for confirmed data | answering NACK "undeliverable" for an unknown destination needs a lookup that fits in the radio's 180 ms |
| More `cmd`s (§17.3) | reload config, reboot; for Tier II boxes as for trunked sites |
| Signed provisioning | a manager key the node checks against `cfg` documents' `sig` |
| A legacy adapter | next to the broker: Homebrew / MMDVM, today's DMRC peers, OpenBridge and BrandMeister's SEA as nodes on this network; designed separately |
| Cross-region | one broker, or bridged brokers per region with a router in each |
| Encrypted calls | passed through as now |

## 15. Registration records (`node/{node}/ms/{id}`)

A trunked site publishes one retained record per radio it serves ([trunk.md](trunk.md) §3.4):

```json
{"id": 3100123, "at": "2026-11-05T21:04:11.250Z", "exp": "2026-11-06T01:04:11.000Z",
 "how": "register", "tg": [91, 3100], "sub": 91, "area": 3}
```

| Field | Meaning |
|---|---|
| `id` | the radio |
| `at`, `exp` | registered or last refreshed; expires (UTC) |
| `how` | `register` (the radio registered), `call` (implicit: from a call request) |
| `tg` | the talkgroups the site carries for it (the subscribed one first, then its recent calls, then provisioning), at most 8 |
| `sub` | the talkgroup the radio subscribed to when it registered (absent: none) |
| `area` | the registration area (absent: not given) |

- Written on every change and at most once a minute for a refresh. Deleted (an empty retained message) on
  de-registration, expiry, a refusal after a policy change, or when `where/{id}` names another node with a newer
  time.
- On a restart the site reads its own records back from the broker and serves those radios at once. A standby
  controller does the same ([trunk.md](trunk.md) §6.2).
- The router reads them as sightings with `source: "reg"` at the record's `at`, valid until `exp`. A newer sighting
  anywhere (a record or heard traffic) wins; at the same time the record wins. It publishes `where/{id}` with
  `source`.
- The site subscribes to the talkgroups in its records, so a group call reaches exactly the sites with members.

Code: `rdof_mn_record_json`, `rdof_mn_record_parse`, `rdof_mn_ms_topic` (`rdof/mnctl.h`).

## 16. `status` (retained)

Any node, every 10 s and on change:

```json
{"at": "2026-11-05T21:04:11.250Z", "node": "site-a", "firmware": "0.9.0", "uptime_s": 3600,
 "trunk": {"connected": true, "regs": 41, "queue": 0,
           "channels": [{"chan": 1, "lcn": 0, "use": "tscc"},
                        {"chan": 1, "lcn": 1, "use": "call", "src": 3100123, "dst": 91, "group": true,
                         "since": "2026-11-05T21:04:01.000Z"},
                        {"chan": 2, "lcn": 0, "use": "idle"}, {"chan": 2, "lcn": 1, "use": "hang", "dst": 3100,
                         "group": true}],
           "counters": {"aloha": 0, "rand": 0, "collisions": 0, "grants": 0, "refused": 0, "queued": 0}}}
```

`trunk` is a trunked site's only. `use`: `tscc`, `call`, `hang`, `idle`, `off` (out of service). `connected`: the
backbone link (the site's Active_Connection). Code: `rdof_mn_status_json`.

## 17. Provisioning (`cfg/...`) and commands

### 17.1 Documents

The manager publishes them retained; each has `rev` (an integer the manager raises on every change) and `at`. A node
applies a document only when its `rev` is newer than the one it holds (after a deletion, any rev), keeps it, and
caches it (rdofradio: a file in its state directory; a board: NVS) so it starts with the last documents when the
network is down. An empty retained message deletes a document. A key the node doesn't know, or one a document can't
change (frequencies, devices, carriers), is ignored and reported (in the log, or in `cmd/res` for a command).

`cfg/net`, the whole network:

```json
{"rev": 12, "at": "2026-11-05T21:00:00Z", "system": {"model": "small", "net": 5},
 "registration": "listed", "ms": "3100000-3199999", "refuse_unlisted": "denied",
 "private": "*", "wide_tg": "1-99999", "timers": {"hang_s": 3}}
```

`cfg/node/{node}`, one site: `rev`, `at`, `name`, `site`, `par`, `area_bits`, `neighbors` (site numbers), `tg`,
`static_tg`, `timers`, `broadcast`, `enabled` (false takes the site off the air: it withdraws its control channel).
`site`, `par` and `area_bits` go at the document's top level (as the manager writes them) or in `system`. Both
site-level documents take the same keys as the settings file's `trunk` app ([trunk.md](trunk.md) §4): `system`
(`model`, `net`, `site`, `par`, `area_bits`), `registration` (`open`, `listed`), `ms`, `refuse_unlisted` (`denied`,
`refused`), `private`, `tg`, `wide_tg`, `static_tg` (ID sets), `require_registration`, `foacsu`, `enabled`,
`neighbors`, `timers` (names to numbers), `broadcast` (the control channel's broadcast intervals, [trunk.md](trunk.md)
§3.2: `adjacent_s`, `chan_freq_s`, `local_time_s`, seconds, 0 off). Both also take `name`: a name for people (the
manager and dashboards show it); a site keeps it and doesn't act on it.

`cfg/tg/{tg}`: `{"rev", "at", "name", "wide", "sites", "ms", "priority", "hang_s"}`. `sites` (site numbers) and `ms`
(radios) limit where and by whom the talkgroup may be used, `*` when absent; `wide` carries it on the network;
`priority` 0-3.

`cfg/ms/{id}`: `{"rev", "at", "name", "enabled", "private", "wide_private", "priority", "emergency", "tg"}`. The
booleans are true when absent. `enabled: false` denies the radio's registration at every site and removes a
registration it has. `tg` (at most 8) are talkgroups the radio always gets.

### 17.2 Precedence

`cfg/ms` over `cfg/tg` over `cfg/node` over `cfg/net` over the settings file, key by key (a timer by its name).
Without any of them a site runs on its settings file alone: a site must work standalone.

### 17.3 Commands

`node/{node}/cmd`, from the manager: `{"id": "c-123", "cmd": "...", ...}`. The node answers on `node/{node}/cmd/res`
with `{"id": "c-123", "ok": true}` or `{"id": "c-123", "ok": false, "error": "..."}`.

| `cmd` | Arguments | Does |
|---|---|---|
| `deregister` | `ms` | removes the radio's registration; it registers again on its next access |
| `clear` | `chan` (1-4094), `lcn` (0, 1) | ends the call on that payload channel |
| `withdraw`, `restore` | | takes the control channel off the air, and back |
| `resend_records` | | publishes every registration record again |
| `status` | | publishes `status` now |

An unknown command or a missing argument is answered `ok: false`.

Code: `rdof_mn_cfg_t` and `rdof_mn_cfg_message`, `rdof_mn_cfg_site`, the cache (`rdof_mn_cfg_cache`,
`rdof_mn_cfg_cache_line`: one line per document, its topic under the root, a tab, the document), `rdof_mn_cmd_parse`,
`rdof_mn_cmd_result_json` (`rdof/mnctl.h`).
