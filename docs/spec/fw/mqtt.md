# esp_idf MQTT

Covers: the board's MQTT connections, every topic it publishes (it subscribes to none), payloads, QoS, retain, rates, the will, and what a subscriber needs to know.
Sources: `esp_idf/main/net.c`, `roam_mqtt.c`, `sdr_mqtt.c`, `radio_task.c`, `roam_hblink.c`; `common/src/tracker.c`, `netcalls.c`, `status.c`, `rawburst.c`, `config.c`; `sdr/src/status.c`, `config.c`.
Related: [interfaces-http.md](interfaces-http.md), [hblink.md](hblink.md), [config.md](config.md) (the `mqtt` settings), [dmr-core.md](dmr-core.md) (the call tracker).

## 1. Connections

Each role has its own connection: the SDR one, and one per roamer or repeater (up to `RDOF_MAX_RADIOS`). Each has its
own client id, will and status topic.

| Item | SDR | Roamer | Repeater | Source |
|---|---|---|---|---|
| Client id | `rdof-sdr-{name}` | `rdof-roaming-{name}` | `rdof-repeater-{name}` | `sdr_mqtt.c:sdr_mqtt_start`, `roam_mqtt.c:roam_mqtt_start` |
| `{name}` | `sdr.name` (the box's SDR name) | the radio's name | the radio's name | |
| `{app}` | `mqtt.app`, default `sdr`; a box forces `sdr` | always `roaming` | always `repeater` | `sdr/src/config.c:mqtt`, `common/src/config.c` (`cfg->mqtt_app`) |
| `{root}` | `mqtt.root`, default `DMR`, `/` stripped from both ends | same | same | `sdr/src/config.c:mqtt`, `common/src/config.c` |
| Status / will topic | `{root}/_sdr/{name}/status` | same | same | |
| Will payload | `{"state": "offline"}` (`RDOF_STATUS_OFFLINE`, with a space after the colon) | same | same | |
| Will QoS / retain | 1 / yes | 1 / yes | 1 / yes | `net.c:net_mqtt_start` |
| esp-mqtt outbox | 64 KB | 32 KB | 32 KB | `outbox_kb` |
| Post queue (PSRAM) | 32 KB | 16 KB | 16 KB | `queue_kb` |
| Topic aliases, in order | every channel's `raw`, then every channel's `json` | `raw`, `json` | `raw`, `json` | `alias_topics` |
| Off | no `mqtt` in the settings, or empty `host` | same | same | |

Common settings (`net.c:net_mqtt_start`):

| Item | Value |
|---|---|
| URI | `mqtt://{host}:{port}` (port default 1883); `mqtts://` with the ESP CA bundle when `CONFIG_RDOF_MQTT_TLS` (default n) and `mqtt.tls` |
| Username / password | `mqtt.username`, `mqtt.password` (empty: none) |
| Keepalive | 30 s |
| Clean session | yes (esp-mqtt default) |
| Reconnect | esp-mqtt automatic, every `reconnect_timeout_ms` = 2000 ms, no backoff |
| Protocol | MQTT 5 when built with `CONFIG_MQTT_PROTOCOL_5` and `queue_kb` > 0; on CONNACK refusal code 1 (3.1.1 "unacceptable protocol version") or 0x84 (5 "unsupported protocol version") it sets `fall_back` and reconnects as 3.1.1, for good |
| Refused (bad credentials) | logged "check the username / password"; esp-mqtt keeps retrying |
| On connect | `on_connected`: republish the retained messages (Receiver, status) |

### 1.1 The post queue

Every publish goes through `net_mqtt_post` (`net.c`) onto a queue that one task per connection hands to esp-mqtt.
The DSP and radio loops never wait on the broker.

| Rule | Value / behavior |
|---|---|
| Message size | at most `POST_MAX` = 4096 bytes (topic + payload); larger is dropped |
| Queue full | dropped |
| QoS 0 while disconnected | dropped |
| QoS 1 while disconnected | queued; esp-mqtt's outbox keeps it until the connection is back |
| Count | every drop increments `net_mqtt_t.dropped` (`/api/status` `sdr.mqtt_dropped`) |

### 1.2 Topic aliases (MQTT 5)

| Rule | Value |
|---|---|
| Count | `min(n_alias_topics, broker Topic Alias Maximum, MAX_ALIASES = 32)`; the maximum is learned at each connect (esp-mqtt doesn't expose CONNACK's, so `net.c` probes it by binary search) |
| Assignment | alias k + 1 → `alias_topics[k]` |
| Used by | QoS 0 posts only (the `raw` topics in practice). QoS 1 always carries the full topic. |
| First use on a connection | full topic + alias; later posts: empty topic + alias |
| Reset | at every connect and disconnect (`gen` counter) |

## 2. Topics

| Topic | Payload | QoS | Retain | When | Source |
|---|---|---|---|---|---|
| `{root}/{channel id}/{app}/json` (SDR) | call JSON (§3) | 1 | no | each call event, per channel | `sdr_mqtt.c:on_json` |
| `{root}/{channel id}/{app}/json` (SDR) | `Receiver` (§4) | 1 | yes | on every connect, one per configured channel | `sdr_mqtt.c:sdr_publish_receivers` |
| `{root}/{channel id}/{app}/raw` (SDR) | raw burst v1 (§5) | 0 | no | each burst, `CONFIG_RDOF_MQTT_RAW` (default y) | `sdr_mqtt.c:on_raw_burst` |
| `{root}/{name}/{app}/json` (roamer, repeater) | call JSON: RF (`source: "rf"`) and network (`source: "network"`) (§3) | 1 | no | each call event | `roam_mqtt.c:roam_publish`, `roam_tx_event` |
| `{root}/{name}/{app}/json` (roamer, repeater) | `Receiver` (§4) | 1 | yes | on every connect | `roam_mqtt.c:publish_receiver` |
| `{root}/{name}/{app}/raw` (roamer, repeater) | raw burst v1 | 0 | no | each burst heard, and each network burst sent (`rdof_netcalls_burst_raw`), `CONFIG_RDOF_MQTT_RAW` | `roam_mqtt.c:roam_publish_raw`, `roam_tx_burst` |
| `{root}/{name}/{app}/tx` (roamer, repeater) | `TX` (§3.4) | 1 | no | start and end of each transmission | `roam_mqtt.c:roam_tx_event` |
| `{root}/_sdr/{name}/status` | status (§6) | 1 | yes | see §6 | `sdr_mqtt.c:sdr_publish_status`, `roam_mqtt.c:roam_publish_status` |

Bench taps (`sdr_is_tap`, a bench API retune's extra channels) are never published.

Test traffic: a board under test should publish under a test root (`mqtt.root` such as `test/DMR`), so that
subscribers on the production root don't take it for real calls.

### Rates

| Message | Rate |
|---|---|
| Call start / end | one each per call per slot |
| `BER`, `RSSI` | every `RDOF_METRIC_SECONDS` = 1.0 s per slot during RF voice (`tracker.c`) |
| `MMDVM` | at each mode change; `idle` after `RDOF_IDLE_HANG_SECONDS` = 3.0 s |
| `raw` | every burst: 16.7 per second per active slot (60 ms per burst) |
| status | §6 |

## 3. Call JSON (`json` topics)

One JSON object, compact (no spaces), at most `RDOF_JSON_MAX` = 640 bytes (`tracker.c`). Times:

| Format | Example | Source |
|---|---|---|
| `timestamp` | `"2026-09-27T15:00:01.123Z"` (UTC, ms) | `tracker.c` |
| float3 | `"%.3f"`, trailing zeros stripped, one digit after the point kept (`1.5`, `0.0`) | `tracker.c` |
| `frequency` (network calls) | `"%.5f"` MHz | `netcalls.c` |

### 3.1 RF calls (`source: "rf"`, `tracker.c`)

| Key | Fields | When |
|---|---|---|
| `DMR` start | `timestamp, source:"rf", action:"start"\|"late_entry", slot (1\|2), src_id, dst_id, group ("yes"\|"no"), src_info, frequency, colorCode` (if the channel is known), `frames` (data headers), `emergency: true` (if set) | voice LC header or data header; `late_entry` for voice heard without a header |
| `DMR` end | `timestamp, action, slot`; voice adds `duration` (float3, s), `ber` (float3, %), `rssi: {min, max, ave}` (dBm) | `end` on a terminator; `lost` after `RDOF_VOICE_LOST_SECONDS` = 0.5 s (voice) or `RDOF_DATA_LOST_SECONDS` = 2.0 s (data) with no burst |
| `DMR` CSBK | `timestamp, source, action:"csbk", slot, src_id, dst_id, group, src_info, csbk_desc` | a CSBK; repeated preambles within `RDOF_PREAMBLE_REPEAT_SECONDS` = 1.0 s are not repeated |
| `BER` | `{"BER": {timestamp, mode:"DMR", slot, value}}` | every 1.0 s of voice |
| `RSSI` | `{"RSSI": {timestamp, mode:"DMR", slot, value}}` | every 1.0 s of voice |
| `MMDVM` | `{"MMDVM": {timestamp, mode:"DMR"\|"idle"}}` | mode change |

A repeated terminator within `RDOF_REPEAT_TERMINATOR_SECONDS` = 1.0 s doesn't end a second call.

### 3.2 Network calls (`source: "network"`, `netcalls.c`)

What a roamer or repeater sends on air for HBlink4. `slot` is the call's slot if 1 or 2, else 2.

| Event | Fields |
|---|---|
| start | `timestamp, source:"network", action, slot, src_id, dst_id, group, src_info, frequency ("%.5f" MHz), colorCode, frames` (if ≥ 0) |
| end | `timestamp, source, action, slot, duration` |
| CSBK | `timestamp, source, action:"csbk", slot, src_id, dst_id, group, src_info, csbk_desc, frequency, colorCode` (no end) |

Network raw bursts: a data-sync burst repeated on the same slot (same control byte and bytes) is sent once.

### 3.3 `csbk_desc` (`netcalls.c`, `tracker.c`)

| CSBK opcode | Text |
|---|---|
| 0x04 | `Unit to Unit Voice Service Request` |
| 0x05 | `Unit to Unit Voice Answer Response` |
| 0x1F | `Call Alert` |
| 0x20 | `Call Alert Ack` |
| 0x24 | `Radio Check` (byte 3 == 0x80) or `Radio Check Ack` |
| 0x26 | `Negative Acknowledgment Response` |
| 0x27 | `Call Emergency` |
| 0x3D | `Preamble` |
| other | `CSBK 0x%02X` |

### 3.4 `TX` (`tx` topic, `rdof_tx_json`)

`{"TX": {timestamp, action ("start"|"end"|…), slot, src_id, dst_id, group, frequency, colorCode, duration (end only)}}`.
It is for monitoring; a call log needs only the `json` topic.

## 4. `Receiver` (retained on `json`)

| Role | Payload | Source |
|---|---|---|
| SDR (per channel) | `{"Receiver": {name (channel name, else id), frequency (Hz), colorCode (or null), source:"sdr", device (the SDR's name), tg (or null), latitude?, longitude?, height?}}` | `sdr/src/status.c:rdof_sdr_receiver_json` |
| Roamer | `{"Receiver": {name, frequency (the idle channel, Hz), colorCode, source:"roaming", device (callsign), latitude?, longitude?, height?}}` | `common/src/status.c:rdof_roamer_receiver_json` |
| Repeater | `{"Receiver": {name, frequency (RX), txFrequency, colorCode, source:"repeater", device, latitude?, longitude?, height?, sfr?: {inboundSlot, outboundSlot}}}` | `rdof_repeater_receiver_json` |

Location numbers are written in the shortest form that reads back as the same double, with `.0` on a whole number (`rdof_format_repr`): `40.5`, `40.0`.

## 5. Raw bursts (`raw` topics)

Binary, version 1, 49 bytes, little endian (`rdof/rawburst.h`, `RDOF_RAWBURST_BYTES`).

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | version = 1 |
| 1 | 1 | flags: 0x01 slot 2, 0x02 network (sent, not heard), 0x04 addressed (src / dst valid), 0x08 group (only with addressed) |
| 2 | 1 | data type: the slot type's data type 0–10 (voice_pi_header … usbd), 0xF0 voice sync, 0xF1 voice |
| 3 | 1 | n: the voice burst's index (A–F = 0–5), else 0 |
| 4 | 6 | ms: UTC ms, u48 |
| 10 | 3 | src, u24 (0 unless addressed) |
| 13 | 3 | dst, u24 (0 unless addressed) |
| 16 | 33 | the burst |

Type names (`TYPE_NAMES` in `rawburst.c`): `voice_pi_header, voice_lc_header,
terminator, csbk, mbc_header, mbc_continuation, data_header, rate_1_2, rate_3_4, rate_1, usbd`, then `voice_sync`
(0xF0) and `voice` (0xF1).

Version 2 (62 bytes, `RDOF_RAWBURST_BYTES_UTC`: v1 + `utc_us` i64 at 49, UTC grid slot n u40 at 57) is what an SDR
receiver with `slot_timing` utc sends for a BS or MS burst while its clock is on the grid
(`rdof_sdr_rx_utc_map`, fed by `slot_timing_rx`). Other bursts, and every burst off the grid, are v1. A subscriber
tells them apart by the version byte and the length.

## 6. Status (`{root}/_sdr/{name}/status`, retained, QoS 1)

### 6.1 SDR (`sdr/src/status.c:rdof_sdr_status_json`)

Sent first 5 s after start, then every `status_seconds` (default 30; ≤ 1 means 30), and on every connect, once the
receiver is up (`g_rx_ready`).

| Field | Type | Meaning |
|---|---|---|
| `state` | `"online"` | |
| `name` | string | the SDR's name |
| `at` | number | UTC s |
| `centerHz`, `sampleRate` | number | |
| `cpuLoad` | number/null | DSP load, 3 decimals |
| `workers` | 0 | always 0 (kept for status readers that expect it) |
| `droppedChunks`, `overflows` | int | |
| `channels[]` | | `{id, name, freqHz, bursts, lastBurstAt (UTC s or null), rssi (dBm or null), afcHz (1 decimal)}`; bench taps left out |
| `clipPct`, `peakDbfs` | number | input level, when measured |
| `gainDb` | number | when a fixed gain is known |
| `agc` | object | `{ceilingDb, minDb, changes}` when the gain control runs |
| `hblink` | object | `{"<channel id>": bool}` per channel peer, when any |
| `gnss` | object | where the board is: `{fix ("none", "fix", "2d", "3d"), sats (used), lat, lon (7 decimals), alt_m, hdop}`, the position only with a fix; when the board has a GNSS receiver that's sending (`components/gnss`). The roamer's and repeater's status carry it too |

### 6.2 Roamer (`common/src/status.c:rdof_roamer_status_json`)

`{"state":"online", "at", "frequency" (Hz, now), "colorCode", "listening", "scanning", "radio" (ready), "hblink"
(link up), "onAir"}`.

### 6.3 Repeater (`rdof_repeater_status_json`)

`{"state":"online", "at", "frequency" (RX Hz), "txFrequency", "colorCode", "radio", "hblink", "onAir", "slots":
{"1": busy, "2": busy}}`, with only the configured slots in `slots`.

### 6.4 When a roamer or repeater publishes status

| Trigger | Source |
|---|---|
| connect | `roam_mqtt.c:on_connected` |
| every 10 s | `roam_mqtt.c:heartbeat_task` |
| radio ready, radio lost (each retry) | `radio_task.c` |
| retune, scan hold / resume (roamer) | `radio_task.c:retune`, `scan` |
| keyed / unkeyed | `radio_task.c:on_key` |
| HBlink4 link up / down | `roam_hblink.c` |
| each TX start / end (roamer) | `roam_mqtt.c:roam_tx_event` |

## 7. Connection sequence

```mermaid
sequenceDiagram
    participant B as Board role
    participant M as Broker
    B->>M: CONNECT (v5, client id, keepalive 30, will {root}/_sdr/{name}/status offline q1 retained)
    alt broker refuses v5 (0x84 / 1)
        B->>M: CONNECT (3.1.1), from then on
    end
    M-->>B: CONNACK
    B->>M: PUBLISH Receiver (q1, retained) per channel or radio
    B->>M: PUBLISH status online (q1, retained)
    loop running
        B->>M: json (q1), raw (q0, alias), tx (q1), status (q1, retained)
    end
    Note over B,M: link lost: broker publishes the will; esp-mqtt reconnects every 2 s; QoS 0 dropped meanwhile
```

## 8. Subscribing

A subscriber takes calls from `{root}/+/+/json` and `{root}/+/+/raw`, and status from `{root}/_sdr/+/status`. Every
data topic is exactly `{root}` plus three levels: the channel id or radio name, the `app`, and `json` / `raw` / `tx`.

| Message | Read by |
|---|---|
| `json` | top key `DMR` (by `action`: start, late_entry, end, lost, csbk), `BER` / `RSSI` (mode DMR), `MMDVM`, `Receiver` |
| `raw` | the version byte, then v1 (49 bytes) or v2 (62 bytes) |
| status | `state, at, centerHz, sampleRate, cpuLoad, workers, droppedChunks, overflows, gainDb, peakDbfs, clipPct, agc, channels, hblink` (SDR); `frequency, txFrequency, colorCode, slots, listening, scanning, radio, hblink, onAir` (roamer, repeater); `gnss` (all, with a GNSS receiver) |

## 9. Notes

| Item | Behavior |
|---|---|
| Roamer / repeater `app` | always `roaming` / `repeater` |
| Reconnect | fixed 2 s |
| Retained on reconnect | `on_connected` republishes Receiver and status |
| Clean stop | never stops; offline comes only from the will |
| SDR status `slotTiming` | absent; the slot timing state is in `/api/status` |
| Network CSBK not decoded | `CSBK 0x%02X` |
| Aliases | the raw and json topics, learned per connect |
