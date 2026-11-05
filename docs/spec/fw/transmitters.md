# Receivers and transmitters

Covers: a device that receives and transmits run as two parts, a receiver of every frequency it hears and a
transmitter tuned per stream to any frequency it may send on; the transmitter pool the router draws from; apps that
answer through it; half duplex; several transmissions at once.
Status: 2026-11-01. Built: the pool (§4, `rdof/txpool.h`), the transmitter as the roamer the router tunes (§3, §12),
apps on the router (§5), the blind span (§6), the status (§8). Not yet: clear to send across paths (§3), slot timing
across a blind span (§6), the MQTT network's requests (step 5), several at once (§9).
Related: [config-v3.md](config-v3.md) (devices, paths, §9.4, §12), [mqtt-net.md](mqtt-net.md) (paths and streams on
the network), [sdr-tx.md](sdr-tx.md) (the modulator, the SDR modem), [sdr-rx.md](sdr-rx.md), [roamer.md](roamer.md).

## 1. Why

A W12 should hear 3 to 5 frequencies at once and, when a call has to go out, key its one transmitter on that call's
frequency. Today it can't, quite:

- The settings made a transmitting SDR a `roamer` over its simplex frequencies plus a `monitor` of all of them
  (config-v3.md §12). The roamer is a modem's state machine: one receive channel and the transmitter, tuned together.
- The router already tunes that roamer to a network call's frequency (config-v3.md §9.4, `txvr_for`). Nothing else
  uses that: the parrot and the IP gateway are options of a role and answer on the roamer's channel only, and they
  hear only that channel (`sdrmodem.c` feeds the roamer's modem from its tuned channel).
- An SDR transceiver that scans gets near this, but it hears one channel at a time.

## 2. The model

Two words, as [mqtt-net.md](mqtt-net.md) uses them:

| Word | What it is |
|---|---|
| path | where a call comes in or goes out: a frequency and, where the slots are separate, a slot (config-v3.md §6.1). It has an index (`paths[].i`, the call message's path byte) |
| stream | one transmission: a stream ID, src, dst, its bursts until the terminator |

Everything that transmits asks for the same thing: **send stream S on path P.** A network's group call, a `dl`
stream, the parrot's reply and the IP gateway's data are each that request. Where it was heard is a path too: every
call the receiver hears carries the path it was heard on.

A device that receives and transmits is two parts:

| Part | What it does | Runs as |
|---|---|---|
| the receiver | hears every frequency in its window at once, both slots; each call with its path | the `monitor` role `<device>-rx`, as today |
| the transmitter | one carrier at a time, tuned per stream to any frequency in the device's `transmits` (its `cc`, `power` and the path's slot); no receiver of its own | a `transmitter` role named after the device (§3), in place of the roamer |

They share hardware. On a half-duplex device the receiver is blind while the transmitter is keyed (§6).

A modem or a radio stays a roamer: its receiver is the same tuned channel as its transmitter, so there is nothing to
split. The split is for a device whose receiver covers more than its transmitter: an SDR (the LR2021, a PlutoSDR),
or an SDR receiving beside a separate transmitter.

## 3. The transmitter

A role with no receive path: the SDR modem's transmit half, fed bursts by the router.

| State | Meaning |
|---|---|
| free | not keyed, not held: any request may take it |
| sending | keyed for stream S on path P |
| holding | S ended (its terminator, or the router's idle time with no burst, `RDOF_ROUTER_IDLE_S` 1.5 s): it stays on P for `answer_s` (the device's, 0.75 s by default), so a reply goes out where the call was |

**Tuning.** At a stream's first burst the transmitter takes the path's frequency, `cc`, `power` and slot (the
frequency's `ts` on a simplex path, the path's slot on a two-slot one), then sends as the roamer does: its preambles
(`csbk_preambles`), the jitter buffer, `cwid`. These keys stay the device's (config-v3.md §4). A retune between two
streams costs the LR2021 a PLL relock, well under the 120 ms jitter buffer.

**Clear to send.** Before keying on a path, it asks the receiver whether that path is busy (a carrier in the slot, or
a call heard within `clear_s`), as the roamer checks its own channel. The receiver hears every path, so the check
needs no retune.

## 4. The pool

The router (`siteroute.c`) holds every transmitter, with the frequencies each may reach. For a request (S, P) it takes,
in order:

1. the transmitter already sending S on P;
2. one holding P, when S answers the stream it held for (its dst is that stream's src, or the same talkgroup);
3. a free one that reaches P's frequency;
4. one holding another path, when S is an emergency or priority stream, a network's stream, or a confirmed-data
   answer. So the hold keeps a transmitter for replies against the box's own apps; a network stream never waits
   on one. A hold that's over is free (rule 3).

An emergency or priority stream takes a sending transmitter as config-v3.md §9.2 and mqtt-net.md §8.1 say: the stream
on air ends with a terminator, and the new one starts.

None found:

| Who asked | Then |
|---|---|
| a network (HBlink, MQTT `tg` or `dl`) | not sent on that path, counted (`no_txvr`), as today |
| an app on the box (§5) | it waits for one, up to the app's limit (the parrot: 10 s), then gives up, counted |
| a confirmed-data answer | may take a transmitter that is only holding; otherwise not sent (the radio retries) |

`tx_ok` on a path (mqtt-net.md §6) means some transmitter reaches it, busy or not.

## 5. Apps answer through the router

A box's own apps (`parrot`, `ipgw`) become clients of the router, not options of a role:

- **They hear every path.** The receiver's calls go to each app whose IDs a path's `apps` lets it reach
  (config-v3.md §6), whichever frequency they came on. One parrot per box, as now.
- **They answer with a request.** The parrot plays back on the path the call was heard on. The IP gateway sends to a
  radio on the path it was last heard on (the box's last-heard table, config-v3.md §9.3), and answers confirmed data
  on the path it was heard on.
- **The same request as the network's.** An app on the box sends exactly what an app on the MQTT network would put on
  `{root}/pc/{radio}/{app}` and what the router would send down `dl` with a path byte (mqtt-net.md §9). An app can
  move off the box later with nothing changed but the transport.

## 6. Half duplex

While a half-duplex device's transmitter is keyed, its receiver hears nothing on any frequency (the LR2021 is one
chip; [lr2021-iq.md](../lr2021-iq.md)).

- The front end marks the blind span on the receive stream. Each path's tracker treats it as no signal rather than a
  lost call: a call in progress on another path is ended as lost only after the blind span plus the usual 0.5 s, and
  the span is counted per path.
- Our own transmission isn't heard on a half-duplex device. On a full-duplex one the receiver drops it (config-v3.md
  §9.4), so it never goes back to a network.
- The receiver's slot timing for a path (a repeater heard, its TS1 / TS2) carries across the blind span from its last
  sync.

A full-duplex device (a PlutoSDR, or an RTL-SDR with a separate transmitter) keeps hearing every path while it
transmits.

## 7. Config

No new keys. A version 3 file already says what a device hears and where it may transmit (`hears`, `transmits`,
config-v3.md §4). What runs (the units, config-v3.md §12) changes for an SDR that transmits on simplex frequencies:

| Version 3 | Runs as today | Runs as |
|---|---|---|
| an SDR that transmits on simplex frequencies | a transceiver unit named after the device, plus `<device>-rx` | a `transmitter` named after the device, plus `<device>-rx` |
| a modem or radio that transmits | a transceiver unit | the same |
| `parrot`, `ipgw` apps | the parrot of each unit, the IP gateway on its `tx` unit | apps on the router (§5); `ipgw.tx` is not needed |

The W12 with three frequencies:

```json
{
  "version": 3,
  "name": "w12",
  "id": 9990501,
  "defaults": {"cc": 1},
  "devices": [{"name": "w12", "kind": "sdr", "driver": "lr2021"}],
  "frequencies": [
    {"freq": 438.075, "power_dbm": 20}, {"freq": 438.1, "power_dbm": 20}, {"freq": 438.125, "power_dbm": 20}
  ],
  "apps": [{"type": "parrot", "pc": "9990"}, {"type": "ipgw", "pc": "9991", "nat": true}]
}
```

It runs as `w12-rx` hearing all three, and `w12` transmitting on whichever of the three a stream needs.

## 8. Status

Per transmitter: `state` (§3), `path`, `stream_id`, `since`, and counters `sent`, `preempted`, `waited`, `gave_up`,
`not_clear`. Per path: `blind_s` (§6). On MQTT the `activity` `tx` event already carries the path (mqtt-net.md §7).
The OLED's TX row shows the transmitter's path while it sends or holds.

## 9. Several at once

- **Two transmitters** (two devices) are already the pool: the router keys both.
- **Two slots of one carrier.** The LR2021 has one synthesizer: one carrier. It can still carry two streams at once
  when they are on the same frequency in opposite slots: repeater-style transmission, timed as dual capacity direct
  mode ([sdr-tx.md](sdr-tx.md) §2). The transmitter then holds per (frequency, slot), not per device.
- **Two carriers.** A wideband transmitter (a PlutoSDR) can sum two modulators inside its band: a transmitter with
  `carriers: 2`, each carrier its own (frequency, slot) pair. The pool counts carriers, not devices.

## 10. Order of work

| Step | What | Test |
|---|---|---|
| 1 | The pool in `common/` (`txpool.c`: requests, holding, preemption), used by `siteroute.c` in place of `txvr_for`. Done | `test_site.c:test_txpool`, every rule in §4 |
| 2 | The transmitter: the SDR modem's transmit half, tuned per stream. Done as the roamer (§12) | host test of the units (`test_plan.c`); the W12 on the §7 file, a network call on each frequency |
| 3 | Apps on the router: calls carry their path; the parrot and IP gateway hear every path and answer by request. Done; the W12's parrot answered on each of its three frequencies (2026-11-01) | `test_site.c` (app bursts), `test_parrot.c:test_frequencies`; the parrot called on each of the W12's three frequencies |
| 4 | Half duplex: the blind span (§6). Done, but not the slot timing | `test_sdr.c`; the W12 with calls on two frequencies while it transmits |
| 5 | The MQTT network (mqtt-net.md phase 1): `dl` and `tg` streams make the same requests (`rdof_siteroute_app_burst` with the path byte, or the network path with `take_hold`) | mqtt-net.md §13 |
| later | Two slots of one carrier, then carriers (§9) | |

## 11. Open

- How long a network stream may wait for a transmitter. Today it doesn't wait. A short wait (one jitter buffer,
  120 ms) would catch a transmitter finishing a terminator.
- HBlink roaming (`DMRT`, the master tuning a peer's roamer) for a pool transmitter. With `identity: per_path` each
  path has its own peer, so the master needs no tuning: proposed, `DMRT` ignored for a transmitter.
- Whether the 0.5 s end and `answer_s` should be the path's (a repeater's hang time is) rather than the device's.

## 12. How it's built

- **The transmitter is the roamer.** An SDR that transmits on simplex frequencies is a transceiver unit named after
  the device, run as a roamer, plus `<device>-rx`. The roamer's transceiver (`rdof_txvr` over the SDR modem) is
  in the router's pool with those frequencies, and the router tunes it per stream (`rdof_txvr_on_tune`). The roamer
  reports nothing it hears (the receiver reports it), has no parrot of its own, and still hears its tuned
  channel to listen before talking (`rx_path.c`, `radio_task.c`).
- **Apps** call `rdof_siteroute_app_burst(path, burst, take_hold, power_max)`: the pool's transceiver, tuned at the
  stream's first burst (the path's power, lowered to `power_max`; 255 keeps it), the burst on the path's slot. `rdof_siteroute_app_ready` says whether one would take a new stream now.
  - The parrot is one per box (`site_run.c`), made when a roamer with the parrot joins the pool. It is fed from the
    SDR receiver's configured channels (`sdr_task.c`, skipping our own transmission) and from a modem roamer's
    channel, keeps one transmission at a time (`rdof_parrot_heard_at`: the first frequency heard wins), waits for a
    transmitter (`ready`, up to `RDOF_PARROT_WAIT_S`), and plays back on the path it heard. A repeater keeps its own
    parrot, and the box parrot takes nothing heard on a repeater's paths.
  - The IP gateway sends on the path the radio was last heard on (its frequency and slot), with `take_hold`. Its
    settings are the ipgw app's (`rdof_plan_ipgw`, config-v3.md §6.3). Tested on a W12 (2026-11-01) with a DM-32UV on 438.075: a UDP datagram through NAT to an echo server
    and its reply back on air (a header and five blocks), and a confirmed one whose ACK went out the same way.
  - A bench job (`WS /tx via=modem`, or the line protocol's) goes through the pool when the roamer has the SDR modem
    (bench-api.md section 7): `rdof_benchroute` answers it as an MMDVM modem would, each call a stream
    (`0x4245nnnn`) on the path at the job's frequency, without `take_hold`. It sends at the job's power
    (`power_dbm`, or the older `power` 0..255 on the transmitter's range) under `bench.tx`'s `max_dbm` (or
    `rf_level`) at most: `rdof_siteroute_app_burst`'s `cap_dbm` lowers the tune's RF level on the transmitter's map
    (a modem without one sends at its least under a cap), and the SDR roamer is held to the job's dBm while it runs
    (`sdr_modem_tx_cap`), streams other than the job's included. Its BS family is refused there (the pool frames MS
    sync), before anything keys.
  - A bench `outbound` job (a repeater's idle outbound, BS sync on both slots) doesn't go through the pool: it
    borrows the raw SDR modem from the roamer (`sdr_modem_bench_open` with `borrow`; the roamer's modem is parked
    and its writes go to it), keys it duplex in the modem's own TX half for the job's length, and gives it back
    re-tuned to the roamer's frequency and power. It waits while the roamer is transmitting.
- **Power:** in dBm at the antenna (config-v3.md §4.7). The roamer's SDR modem sends at the device's
  `tx.power_dbm` (default 10 dBm, the roamer's `tx_power_dbm`) at most. A stream's tune carries its frequency's
  `power_dbm` as an RF level on the device's map (255 the device's power, 0.2 dB a level under it); the SDR modem
  hands `SET_FREQ`'s level to the host (`rdof_sdrmodem_io_t.tx_level`), which sends at the lower of the two. On a W12
  the front end picks the PA or the bypass per transmission (`rdof_txpower_path`): the PA (+12 dB) in its band for
  more than −1 dBm, else the bypass (−22 dB), the chip at the request less the path's gain, −9.5..21 dBm (at 22 its
  PA setup sends noise, lr2021-iq.md R3.49). The log says the chip's setting and the path.
- **The blind span** is the receiver's, not a path's: the LR2021 front end sets `blind_when_muted`, and while the SDR
  modem's transmission covers the stream every channel's tracker stops aging (`rdof_tracker_blind` moves its calls'
  last burst on by the span). `/api/status` `sdr.blinds` (a span per burst transmitted, as the SDR modem mutes each),
  `sdr.blind_s` (their total).
- **Tested** on a W12 (2026-11-01): a line protocol voice job on 438.075 (8 headers, 12 voice, 3 terminators) went
  out through the pool as one stream, at 10 dBm: 11 bursts on air (one header, one terminator, and the jitter
  buffer's trim of the silent voice).
- **Status:** `/api/status` `router`: `no_txvr`, `app_no_txvr`, `preempted`, `held_taken`, and per transmitter `state`,
  `path`, `stream_id`; `router.parrot`: `state`, `hz`, `gave_up`.

