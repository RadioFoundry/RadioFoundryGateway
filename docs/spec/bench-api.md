# Bench API: one HTTP and WebSocket interface for SDR benches

How a host tool talks to a receive / transmit bench: an ESP32 board running the gateway (`esp_idf`), or the bench
daemon (the authors' own, which owns an RTL-SDR and an MMDVM modem on a Linux host; it isn't published, and every tool
here works against a board without it). Host tools speak it through the client `tools/bench/benchapi.py` (the HIL
tests in `hil-tests/` use it), so the same tool works against either bench. The board will serve it natively (section
9). The bench daemon can serve it through an adapter now and natively later (section 10).

Version 1. Section 11 says what works where; section 9 what the board has.

## 0. Goals and limits

- One interface for discovery, tuning, spectrum, I/Q, decoded DMR bursts, transmit and radio control.
- Control and snapshots are HTTP + JSON. Streams are WebSocket: JSON text frames for control and records, binary
  frames for samples.
- One sample clock (`sc`, section 2) ties bursts, I/Q, spectrum rows and transmit times together. Timing tests
  depend on it, and the board already schedules TX against its RX sample counter.
- The tools must keep working on a board with 8 HTTP sockets, about 7 MB of free PSRAM, USB Full Speed (the
  RTL-SDR at 288 kS/s at most) and Wi-Fi. So channel taps (48 kS/s) are the normal I/Q stream. The wideband
  stream is opt-in, and drops are reported, never hidden.
- Nothing transmits unless every gate in section 7 is open. A closed or silent socket stops a transmission.

## 1. Conventions

- Base path `/api/bench/v1`. Frequencies are in Hz (`freq_hz`, floats), rates in samples/s, levels in dBFS, times
  in UTC seconds (`t`) or microseconds (`t_us`).
- Errors: HTTP status plus `{"error": "text", "code": "..."}`. On a WebSocket, a text frame
  `{"type": "error", "code", "error"}` is followed by a close. Codes and the tools' exit codes (the same as
  the bench daemon's client):

| code | HTTP | exit | Meaning |
|---|---|---|---|
| `bad_request` | 400 | 1 | malformed or missing parameter |
| `refused` | 403 | 77 | not allowed: frequency not listed, cap missing, TX gate closed |
| `busy` | 409 | 75 | held by another session; the body carries `queue` (`eta_s`, `worst_s`, `position`, `blockers`) |
| `timeout` | 504 | 75 | waited longer than `wait_max` |
| `no_signal` | 200 + `done.no_signal` | 3 | a trigger or burst wait ended with nothing heard |
| `unavailable` | 503 | 69 | the device (SDR, modem, radio) isn't working |
| `unsupported` | 404 / 501 | 69 | this bench can't do it |
| `internal` | 500 | 1 | anything else |

- Auth: none. Benches sit on a private test network, as the board's web UI does. TX still needs a lease and the
  gates in section 7.

## 2. The sample clock

`sc` is the count of wideband RX samples since the receiver started (u64), in the front end's own rate
(`info.sdr.rate`). Every stream says which run it belongs to (`run`, changed on a restart or retune) and how `sc`
maps to UTC:

```json
"clock": {"run": 7, "rate": 288000.0, "sc0": 0, "t0_us": 1792584000123456, "ppm": 0.42, "source": "ntp"}
```

`t_us = t0_us + (sc - sc0) * 1e6 / rate * (1 - ppm * 1e-6)` (the board learns `ppm` from its disciplined clock; it is
null until then: use 0). A channel tap's samples have their own index. Each I/Q frame carries the `sc` of its first
sample, rounded to a wideband sample, so a tool can line up a channel tap, a burst record and a TX time to one
wideband sample (3.5 us at 288 kS/s). On the daemon, `sc` is the capture's sample index and `run` is the capture.

## 3. Discovery

`GET /info`: what the bench is, which changes only with settings.

```json
{"api": "v1", "device": {"kind": "esp32s3-gateway", "name": "...", "version": "0.3.0"},
 "sdr": {"frontend": "rtlsdr", "tuner": "R820T", "rate": 288000.0, "rates": [240000, 288000],
         "center_hz": 461122500.0, "gain_db": 29.7, "gains_db": [0, 0.9, ...], "ppm": 0.42,
         "window": {"lo_hz": 461007300.0, "hi_hz": 461237700.0}, "tx": false},
 "channels": [{"id": "SDR06", "name": "Operations 3", "freq_hz": 461137500.0, "cc": 2, "slot": 1,
               "caps": ["rx", "dmr", "nfm", "iq"], "in_band": true}],
 "radios": [{"name": "radio1", "kind": "odr", "caps": ["tune", "ptt", "mmdvm"], "port": "usb"}],
 "tx": {"freqs_hz": [], "max_key_s": 0, "power": null},
 "caps": {"spectrum": true, "bursts": true, "calls": false, "iq": ["cs16", "cf32", "cs8"],
          "iq_wideband": true, "tune": true, "tx": false, "radio_control": false},
 "limits": {"streams": 3, "iq_wideband_max": 1, "preroll_s": 0.4}}
```

Channel caps, as the bench daemon names them where they overlap: `rx` (in the receive window), `dmr` (a DMR demodulator
runs on it), `nfm` / `wfm` (FM demod available), `iq` (a channel tap can be opened), `mmdvm_tx` (the bench's modem
may transmit there), `station_tx` / `radio_tx` (a bench radio may key there), `sdr_tx` (the SDR front end may).

`GET /status`: what it is doing now. Load, drops and per-channel activity (`bursts`, `rssi`, `afc`, `idle`), the
sessions and leases, open streams, the TX state (`idle`, `armed`, `keyed`, with the lease) and the clock.

## 4. Sessions and leases

Several tools share a bench. A session holds leases on frequencies (as the daemon's `session` + `claim`) and, for an
exclusive retune or a transmitter, the device.

- `POST /session {"label": "hil-fm", "ttl_s": 30}` → `{"session": "s-3f2a", "ttl_s": 30}`. Every request with
  `X-Bench-Session: s-3f2a` renews it. It ends on `DELETE /session/s-3f2a`, at its TTL, or when its control
  WebSocket closes (`/session/s-3f2a/ws`: a client that keeps one open never has to renew).
- `POST /session/{id}/lease {"freq_hz": [...], "rx": true, "tx": false, "exclusive": false, "wait_max": 300}`
  returns a lease or `busy` with a queue position. A WebSocket form streams `queued` / `granted` the way the daemon
  does.
- `GET /session/{id}` renews the session and returns it:
  `{"session", "label", "ttl_s", "expires_in_s", "lease": null | {"freq_hz": [...], "rx", "tx", "exclusive"}}`.
  `DELETE /session/{id}/lease` drops the lease and keeps the session. Both `DELETE`s return `{"ok": true}`. An
  unknown or expired id is 404 `bad_request`.
- Receive needs no lease on a shared channel. A retune (section 5), a wideband stream when the bench limits them,
  and every TX need one.

## 5. Tuning

`GET /sdr`, and `PUT /sdr {"center_hz", "rate", "gain_db", "ppm"}` with an exclusive lease. A tune applies at run
time only: settings stay as they are, and the bench goes back to them when the lease ends. A tune bumps
`clock.run` and sets the `retuned` flag on open streams. `"center_hz": "auto"` asks for the bench's channel plan.
The board refuses a rate above what USB Full Speed carries (`refused`, with the limit in the text).

On the board: `PUT /sdr?session=ID {"center_hz", "gain_db", "taps_hz"}`, after `POST /session/ID/lease
{"exclusive": true}` (one exclusive lease at a time; another gets `busy`). `center_hz` and `gain_db` take a number
or `"auto"` (the settings'); a fixed gain turns the AGC law off while it lasts. `taps_hz` lists up to 4
receiver channels at any frequency in the window (ids `T1`...): they decode like a configured channel (bursts,
calls, a channel's I/Q) but never go to MQTT or HBlink4. A center with no configured channel in the window gets one
tap of its own, an eighth of the rate above the center. Fields left out stay as they are. The rate is the
settings' and the ppm is learned, so those are refused. The receiver restarts at the new tune (about 0.3 s; the
reply comes once it runs) and `clock.run` bumps. A stream bound to one channel ends with `done.why: "retuned"`;
all-channel and wideband streams go on, wideband I/Q frames with the `retuned` flag. A retune the board can't do
(a tap outside the window, too many channels) is refused and the tune before it stays. `GET /sdr` shows the retune
in force (`retune`, or null). `benchapi`'s `retune()` does this, and its `capture()` at a frequency with no
channel makes a tap for the capture and puts the settings back after it.

## 6. Streams

All streams are WebSockets under the base path. Query parameters select. The first text frame is
`{"type": "start", ...}` with the clock, and the last is `{"type": "done", ...}` with counts. A frame with
`"type": "drop"` reports what the bench couldn't send (`samples`, `bursts`, `rows`) as it happens.

### 6.1 Spectrum

`GET /spectrum?since=SEQ` stays compatible with the board's current `/api/spectrum`: `{seq, bins, row_ms, rate,
center, rows}`, where each row is base64 with one byte per bin, value = -2 x dBFS, lowest frequency first.
`WS /spectrum/ws?bins=512&row_ms=200` pushes the same rows as binary frames: a 16-byte header (`RDSP`, u16 bins,
u16 row_ms, u64 sc), then the bytes. On the board, `sc` is the row's last sample, and the size is fixed (512 bins about
every 200 ms, as the start frame says, whatever the query asks); `seconds=` ends it, rows it fell behind on are a
`drop` frame with `rows`. Client: `Esp32Bench.spectrum_stream()`.

### 6.2 I/Q

`WS /iq?freq_hz=461137500&fmt=cs16&seconds=5&pre_s=0.3` gives a channel tap at 48 kS/s, centered on `freq_hz`
(any frequency in the window, not only a configured channel). `bw_hz` sets the tap filter. `rate=wide` gives the
front end's wideband samples instead (`fmt=cs8`, the RTL's own format; one such stream at a time). Other parameters:
`seconds` (or until close), `pre_s` (pre-roll from the bench's ring, up to `limits.preroll_s`) and
`trigger=level&threshold_db=10&min_ms=10&post_s=2`, which waits for the channel to rise above its own floor, as
the daemon's `capture-trigger` does. With a trigger, `seconds` is how long to wait: the stream is `pre_s` before the
rise, then `post_s` after it, or a `done` with `why: "no_signal"` if nothing rose. A trigger is on a channel tap,
not `rate=wide`.

Pre-roll frames come first, flagged 32 (pre-roll), with a `seq` of their own from 0; the live frames follow with
no gap between. The board keeps its history only while I/Q is in use (an I/Q stream opened in the last 10
minutes), so the first stream after a quiet spell gets what it has, possibly none: count the flagged samples
(`benchapi` writes `preroll_samples`).

Start frame:

```json
{"type": "start", "run": 7, "fmt": "cs16", "rate": 48000.0, "freq_hz": 461137500.0, "center_hz": 461137500.0,
 "scale": 32767, "sc_rate": 288000.0, "clock": {...}, "gain_db": 29.7}
```

Binary frames: a 32-byte little-endian header, then `n` samples.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `RDIQ` |
| 4 | 1 | version (1) |
| 5 | 1 | format: 1 cs8, 2 cu8, 3 cs16, 4 cf32 |
| 6 | 2 | flags: 1 gap (samples were lost before this frame), 2 clipped, 4 retuned, 8 tx active, 16 last, 32 pre-roll |
| 8 | 4 | seq (u32, per stream) |
| 12 | 4 | n (complex samples in this frame) |
| 16 | 8 | sc of the first sample (u64, wideband samples) |
| 24 | 8 | t_us of the first sample (i64) |

A client writes the samples in order, filling a gap with zeros so file position equals sample index
(`benchapi.iq_to_file` does this and writes `FILE.json`). The done frame carries `why` (`seconds`, `no_signal`,
`retuned`), `samples`, `gaps`, `clip_pct`, and for a trigger `trigger_sc`, `level_db`, `floor_db` (the channel's
power when it fired and its floor before, in dB full scale).

### 6.3 Bursts

`WS /bursts?channel=SDR06` (or `freq_hz=`; no selector for every channel) with optional `seconds`, `max_bursts`,
`until_end=1`, `soft=1` (adds `dibits`) and `fec=1`. Each frame is `{"type": "burst", "burst": {...}}`. Records use
The bench daemon's `dmr-bursts` field names:

| Field | |
|---|---|
| `t`, `sc` | UTC seconds and sample clock at the burst's sync center |
| `channel`, `freq_hz`, `slot` | where; `slot` from the CACH when there is one |
| `sync` | `BS_VOICE`, `BS_DATA`, `MS_VOICE`, `MS_DATA`, `TS1_*`, `TS2_*`, or `EMB` for voice B-F |
| `kind`, `dt`, `n` | `CSBK`, `VLC_HDR`, `VOICE_A`..`VOICE_F`, ...; data type; voice frame number |
| `cc` | color code from slot type or EMB |
| `cfo_hz`, `level_dbfs`, `dev_hz`, `sym_err` | demodulator measurements |
| `grid_err_ms` | offset from the 30 ms TDMA grid of this run |
| `fec` | `{"bit_err_est", "fec_ok"}` |
| `lc` | decoded link control when the burst completes one: `src`, `dst`, `group`, `flco` |
| `raw_hex` | the 33-byte burst |

### 6.4 Calls

`WS /calls?channel=` sends one record per call (start, end, src, dst, slot, encrypted, burst count, lost) and,
with `audio=ambe`, the AMBE frames. Later: `audio=pcm` once the board decodes voice itself. On the board, records come from the
SDR's trackers: `{"type": "call", "event": "start" | "late_entry" | "end" | "lost", "channel", "freq_hz", "t",
"slot", "src", "dst", "group", "cc"}`, an end also with `start`, `duration`, `ber`, `rssi`, `voice_bursts` and `lost`;
the trackers' other events (CSBKs, data) as `{"type": "event", "event", "dmr": {...}}`; with `audio=ambe`, `{"type":
"ambe", "channel", "slot", "n", "t", "frames": [three 72-bit frames as hex]}` a voice burst. Encryption isn't marked
yet. Client: `Esp32Bench.calls()`.

## 7. Transmit

`WS /tx?session=s-3f2a&lease=L&freq_hz=461462500&via=sdr|modem|radio:NAME`. Gates, all of which must be open, or
the socket closes with `refused`:

1. The bench was built with TX (`CONFIG_RDOF_BENCH_TX` on the board) and its front end can transmit (`info.sdr.tx`).
2. `freq_hz` is in the bench's TX list (`info.tx.freqs_hz`), which only the settings file or the daemon's config
   changes, never this API.
3. The session holds a lease on it with `tx: true`. A lease has no `allow_rf` default: the client asks for it.
4. Key time per transmission ≤ `info.tx.max_key_s`, power ≤ `info.tx.power` (the bench clamps; lowest by default).
5. Optional: a hardware arm input (a GPIO jumper on the board) must be closed.

Client frames:

- `{"type": "bursts", "items": [{"raw_hex": "...", "slot": 1, "at_sc": 123456789}, ...]}`: DMR bursts, each
  scheduled at a sample-clock time, or back to back on the TDMA grid when `at_sc` is left out. Through the modem
  or a radio, `at_sc` can't be honored (`timed: false` in the ack).
- `{"type": "iq", ...}` followed by `RDIQ` binary frames: samples to the SDR front end at `info.sdr.tx_rate`.
- `{"type": "fm", "tone_hz": 1000, "ctcss_hz": 100, "seconds": 2}`: a test tone, built by the bench.
- `{"type": "unkey"}`: stop now and drop what's queued (an abort; sent before the keying starts, nothing goes out).
  A transmission ends by itself once its queue is on the air (`unkeyed` `end`), so a client doesn't send `unkey`
  after its last burst.

The bench answers `{"type": "ack", "queued_s", "timed"}`, then `keyed` (with `sc`), `unkeyed`, and `underrun` if the
queue ran dry mid-transmission. Dead-man rule: TX stops when the socket closes, when no frame or ping arrives for
2 s, when the key time runs out, or when the lease ends. Every keying is logged with the session label, frequency,
start, length and peak power, in `/status` and in the bench's log.

On the board (`bench_tx.c`, the job in `common/src/benchtx.c`, host-tested in `host-tests/test_benchtx.c`):

- `via=modem` is the board's SDR modem (sdrmodem), when its front end can transmit (not an RTL-SDR) and no roamer
  has it; `via=radio:NAME` is a bench radio with `"proto": "mmdvm"`, through its link's guard (lowest power, TX
  data only on the frequency), and not while it's linked. `via=sdr` (I/Q, `fm`) is `unsupported`. Both paths drive
  an MMDVM modem in simplex: `SET_CONFIG`, `SET_FREQ` (RX = TX), `SET_MODE` DMR, one `DMR_DATA2` a burst, paced at
  60 ms a burst, three ahead of air time. A burst's control byte comes from its sync (voice sync, data sync with its
  slot type's data type) or, for voice B-F, from `n` or its place after the last voice sync.
- What may go out is `bench.tx` in the settings: `{"freqs_hz": [whole Hz, up to 8], "max_key_s": 10 (60 at most),
  "rf_level": 0 (SET_FREQ's RF level, 0-255), "arm_gpio": -1}`. Without it nothing transmits. `arm_gpio` is a
  jumper to ground, read with the pull-up on; pulling it stops a keying.
- One job at a time: a second socket is refused at the handshake. A gate that's closed (or a bad query) is an
  `error` frame and `done` once the socket is up, with the codes of section 1.
- Frames: after `start`, `ready` (the modem answered), `ack` (`bursts`, `queued`, `queued_s`, `timed: false`),
  `keyed` (`t`, UTC), `unkeyed` (`why`: `end` (a terminator), `unkey`, `underrun`, `max_key`, `deadman`, `stopped`,
  `refused`; `seconds`, `bursts`), `underrun`, `error`, and `done` (`bursts`, `keyings`, `underruns`). A bursts
  frame that would run past `max_key_s` is refused whole, and the job stops; a client streaming a longer call has
  already keyed by then, so it checks the whole call against `info.tx.max_key_s` before it opens the job. A ping is either a WebSocket ping or `{"type": "ping"}`.
- `info.tx`: the policy, `arm` (its GPIO and whether it's in), `built`, `via` (the paths, `modem` when the front end
  can transmit, `radio:NAME` for each MMDVM radio), `modem` ("ready" or why not). `caps.tx` is true when the
  settings allow some TX. `status.tx`: `state` (`idle`, `setup`, `armed`, `keyed`), the open `job`, and `log`, the
  last eight keyings.

The client is `benchapi.TxStream` (`Esp32Bench.tx()`; it pings every half second and keeps about 1.5 s queued).
Tested on air 2026-10-21 through a DM32 on RadioFoundry (its MMDVM hotspot over an FTDI cable, `via=radio:dm32`,
lowest power): a radio check, a 2 s call (silence with its embedded LC), a text message, both refusals, and a client
frozen mid-call (stopped by the underrun 1.8 s later, before the 2 s dead-man).

## 8. Radios

A bench radio is a radio wired to the bench: a RadioFoundry radio on USB, or an MMDVM modem or hotspot. The daemon calls these
stations.

- `GET /radios`, `GET /radios/{name}`: model, firmware, port, the channel it's on, held by.
- `POST /radios/{name}/tune?session=ID {"freq_hz", "mode": "dmr|fm", "cc", "slot", "tg", "private"}` needs a
  lease on that frequency (receive is enough: a tune doesn't key). Only a RadioFoundry radio is tuned this way, over
  its debug link; `&proto=odr-debug` for a RadioFoundry radio set up as `mmdvm`. An MMDVM modem is tuned by each
  transmit job.
  The bench leaves hotspot mode, turns the VFO on, sets the frequency, then mode, color code and slot (for DMR),
  and the call target when `tg` is given. It answers once the guard has checked the radio at the new frequency:
  `{"radio", "freq_hz", "mode", "rx_hz", "power"}` (power forced to the lowest step). Fields left out stay as they
  are. Refused (409 `busy`) while the radio is linked or transmitting; 503 `unavailable` when the radio refuses a
  step or ends up elsewhere.
- `WS /radios/{name}/link?proto=odr-debug|mmdvm`: the radio's serial port, raw, behind a guard. The guard parses the
  protocol (RadioFoundry debug link: `A5 5A` frames, CRC-16/CCITT-FALSE; MMDVM: `E0` frames). It drops a write that would key
  on a frequency the lease doesn't cover or raise power, and forces the lowest power. This is the daemon's station
  bridge, over a WebSocket.
- Burst TX through a radio is section 7 with `via=radio:NAME`.

Where a radio is attached comes from the bench's config, one entry per radio (on the board, `bench.radios` in the
JSON settings):

```json
{"name": "rf1",  "proto": "odr-debug", "port": {"uart": 1, "tx": 17, "rx": 18, "baud": 921600}}
{"name": "hs1",  "proto": "mmdvm",     "port": {"usb": "cdc-acm", "vid": "0483", "pid": "5740"}}
{"name": "rf2",  "proto": "odr-debug", "port": {"usb": "uart", "serial": "FT1A2B3C", "baud": 921600}}
```

- `uart`: one of the MCU's UARTs, with its pins (and optional `rts` / `cts`).
- `usb`: a device on the OTG host port, alone or behind a hub next to the RTL-SDR: `cdc-acm` (`vid` and `pid`
  required, so it can't take the RTL-SDR), or `uart` for the common USB-serial bridges (FTDI, CP210x, CH34x; `vid`
  / `pid` optional). The first matching device is opened; `serial` and `usb_port` are accepted for telling two
  identical devices apart, but the firmware doesn't use them yet. A radio that isn't there is `unavailable`, and
  comes back when it's plugged in.

On the link, binary frames are the radio's serial bytes both ways. Text frames from the bench are the guard's
events: `{"type": "guard", "event": "refused" | "tx_violation" | "power_step" | "aprs_power_lowered" |
"check_failed", "detail"}`. `?session=ID` takes the TX frequencies from that session's lease (a lease with
`"tx": true` and its `freq_hz`); without one the link is receive only. For a RadioFoundry radio the guard checks the radio
(power to the lowest) as the link opens. The link follows its session's lease: when the TX frequencies change the
radio is checked again, and when the session ends the link becomes receive only (a RadioFoundry radio's transmission found then is
a violation and is released). When the tool's socket closes, the guard unkeys the radio (RadioFoundry: PTT release; MMDVM:
the modem to idle). On MMDVM, `SET_FREQ` goes to the radio with its power byte at 0, and TX
data is refused (a NAK) until a `SET_FREQ` names a declared TX frequency.

## 9. The board's side

| File | What |
|---|---|
| `esp_idf/main/bench_api.c` | info, status, sdr, spectrum, sessions and leases, `GET /radios`; the bursts and I/Q WebSockets, each sent from a task of its own on core 0 |
| `esp_idf/main/bench_radio.c` | the radios (section 8): their ports (MCU UART; USB CDC-ACM; FTDI, CP210x and CH34x through Espressif's drivers) and `WS /radios/{name}/link` |
| `common/src/benchguard.c` | the link's guard, the bench daemon's as a state machine (host tests in `host-tests/test_guard.c`) |
| `esp_idf/main/bench_tx.c` | `WS /tx` (section 7): opens the path, runs the job from a task of its own, checks the lease and the arm jumper, keeps the keying log |
| `common/src/benchtx.c` | the transmit job: gates, queue, pacing, key time, dead-man rule (host tests in `host-tests/test_benchtx.c`) |
| `common/src/benchcfg.c` | `bench.radios` and `bench.tx` from the settings |

How it hooks in:

- `sdr_task.c`: the receiver's burst hook is `bench_on_burst` (it calls the SDR transceiver's `sdr_tx_heard`, then
  queues a copy while a bursts stream is open), and `bench_on_chunk` runs after each DSP chunk (channel taps and
  the wideband samples, copied only while an I/Q stream is open). A full queue or ring is a `drop` frame or a `gap`
  flag; the DSP never waits on a socket.
- `web.c`: `bench_api_register(server)`, 48 URI handlers, and a URI match that takes a `*` for one path segment.
  `CONFIG_HTTPD_WS_SUPPORT=y` in the sdkconfig defaults.
- The I/Q ring is 768 kB of PSRAM (about 1.3 s of the wideband stream). One wideband stream at a time.
- The bench API's buffers, queues and task stacks are in PSRAM (`EXT_RAM_BSS_ATTR`,
  `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`): in internal RAM they left the DSP without its buffers.
- Measured on the N16R8 (2026-10-21, 5 channels at 288 kS/s, DSP load 0.76 at rest): a cs16 channel tap raises it to
  about 0.88 with no drops. The wideband stream takes it to 0.96 (cu8, cs8) or 1.0 (cs16), and the receiver loses
  chunks (`dropped_chunks` in status) while it runs. The stream itself has no gaps. The cost grows with the bytes
  sent, not the conversion, so it is most likely PSRAM cache contention between the network path on core 0 and the
  DSP. Don't stream wideband while the decoder's output matters.

- Kconfig: `CONFIG_RDOF_BENCH` (the API; off, the hooks are the receiver's own) and `CONFIG_RDOF_BENCH_TX` (`WS /tx`).
- Query values are percent-decoded.

- Pre-roll: `bench_on_chunk` keeps 2 s of every channel (cs16) and of the wideband samples (cs8) in PSRAM rings,
  about 4.5 MB with five channels, and skips the channels' ring if that would leave less than 1.5 MB free. Writing
  them costs about 0.11 of a core (`history_load` in the v1 status), so it runs only within 10 minutes of an I/Q
  stream being opened. A stream's pre-roll goes out from the stream task, read from the rings; it doesn't hold up
  the DSP. The level trigger is measured in `bench_on_chunk` on each chunk of the channel's output.

I/Q at a frequency that isn't a configured channel's needs a tap (`PUT /sdr`, section 5); `benchapi`'s
`capture()` makes one.
A WebSocket the board can't
serve (no such channel or radio, all streams in use, the radio already linked) is refused at the handshake
rather than with an error frame; the log says why.

## 10. The bench daemon

The mapping, for an adapter (a small HTTP/WS server on the bench host that talks to the daemon's socket) and later
for the daemon itself:

| v1 | Bench daemon |
|---|---|
| `GET /info` | `info` (window → `sdr`, channels and caps as they are, stations → `radios`) |
| `GET /status` | `status`, `activity` |
| `POST /session`, lease | `session` (connection-scoped) + `claim` / `release` |
| `PUT /sdr` | an exclusive `capture` with its own center and gain (the daemon has no standing retune) |
| `WS /iq` | `capture` (cs8 to a file, at 1.536 MS/s) or `trigger` (a channelized cf32 / cs16 file); the adapter streams the file |
| `WS /bursts` | `bursts` (`dmr-bursts` records), else `listen` |
| `WS /spectrum/ws` | none yet: an FFT over the shared receiver's samples |
| `WS /tx via=modem` | `run --modem` with a burst file; `timed: false` |
| `WS /radios/{n}/link` | `bridge` (the station guard) |

`benchapi.DaemonBench` does the read side of this mapping on the client today (info, status, bursts, capture), over
ssh with a small relay that keeps stdin open for the request's lifetime, because the daemon cancels a request on EOF.

## 11. What works where today

| Operation | Board, legacy endpoints | Board, v1 | Bench daemon via `benchapi` |
|---|---|---|---|
| info, status | yes (`/api/status`, `/api/resolved`) | yes | yes |
| spectrum | yes (`/api/spectrum`) | yes | no (`unsupported`) |
| bursts | over MQTT (`{root}/{channel}/{app}/raw`), credentials from the environment, no `sc` / level / cfo | yes | yes |
| I/Q capture | no | channel taps (cs16, cf32, cs8, cu8) and the wideband stream (cs8, cu8, cs16); pre-roll up to 1 s; level trigger on a channel | yes (cs8 file, fetched and removed) |
| tune | no | `PUT /sdr`: center, gain and taps, under an exclusive lease | through an exclusive capture |
| TX | no | DMR bursts through the SDR modem (a front end that transmits) or an MMDVM bench radio; no I/Q or FM | modem jobs (`run`), not through `benchapi` yet |
| radios | no | list, the guarded link (`benchapi.link_pty` puts it on a pty), and tune for a RadioFoundry radio | `bridge`, not through `benchapi` yet |

The MQTT fallback has one catch: several receivers publish the same channel ids under the same root, so a burst on
`DMR/SDR06/sdr/raw` may come from another receiver. Point the board at its own root or app name while testing.

## 12. Decided

- No auth (private test network).
- Radios on a board UART (number and pins in the config) or on the USB host port (CDC-ACM or a USB-serial
  bridge, directly or behind a hub). Section 8.
- Replace the daemon with the board first; the daemon speaks v1 natively later (section 10's mapping is the guide).
- The first TX path is DMR bursts through the board's MMDVM modem emulation (sdrmodem), before an SDR TX front
  end exists.

## 13. Open questions

1. MQTT login for the fallback: from `RDOF_BENCH_MQTT_USER` / `RDOF_BENCH_MQTT_PASSWORD` only. The board's
   `/api/config` has it, but tools must not scrape credentials. The v1 bursts stream removes the need.
2. Wi-Fi throughput with three cs16 taps and the spectrum push, while MQTT and HBlink run: measure before fixing
   the defaults.
3. Burst analysis and SDR bench helpers for the HIL tests: the existing ones import modules from the bench daemon's
   package, whose origin is GPL or unclear. Write them on this project's own code rather than copy
   them.
