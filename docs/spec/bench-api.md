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
wideband sample (3.5 us at 288 kS/s). `t0_us` is the time when the stream started; a board may steer its stream time after
that by microseconds (a W12 follows its hardware time marks, R3.57 of the LR2021 notes), and a burst's `t` follows. On the daemon, `sc` is the capture's sample index and `run` is the capture.

## 3. Discovery

`GET /info`: what the bench is, which changes only with settings.

```json
{"api": "v1", "device": {"kind": "esp32s3-gateway", "name": "...", "version": "0.3.0"},
 "sdr": {"frontend": "rtlsdr", "tuner": "R820T", "rate": 288000.0, "rates": [240000, 288000],
         "center_hz": 438147500.0, "gain_db": 29.7, "gains_db": [0, 0.9, ...], "ppm": 0.42,
         "window": {"lo_hz": 438032300.0, "hi_hz": 438262700.0}, "tx": false},
 "channels": [{"id": "SDR06", "name": "Operations 3", "freq_hz": 438162500.0, "cc": 2, "slot": 1,
               "caps": ["rx", "dmr", "nfm", "iq"], "in_band": true}],
 "radios": [{"name": "radio1", "kind": "odr", "caps": ["tune", "ptt", "mmdvm"], "port": "usb"}],
 "tx": {"freqs_hz": [], "bands_hz": [], "max_key_s": 0, "power": null, "max_dbm": null},
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
channel makes a tap for the capture and puts the settings back after it. The line protocol's `tune` (section 14.2)
is the same retune: while one is in force an exclusive lease is `busy`, and while an exclusive lease is held a
`tune` is.

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

`WS /iq?freq_hz=438162500&fmt=cs16&seconds=5&pre_s=0.3` gives a channel tap at 48 kS/s, centered on `freq_hz`
(any frequency in the window, not only a configured channel). `bw_hz` sets the tap filter. `rate=wide` gives the
front end's wideband samples instead (`fmt=cs8`, the RTL's own format; one such stream at a time). Other parameters:
`seconds` (or until close), `pre_s` (pre-roll from the bench's ring, up to `limits.preroll_s`) and
`trigger=level&threshold_db=10&min_ms=10&post_s=2`, which waits for the channel to rise above its own floor, as
the daemon's `capture-trigger` does. With a trigger, `seconds` is how long to wait: the stream is `pre_s` before the
rise, then `post_s` after it, or a `done` with `why: "no_signal"` if nothing rose. A trigger is on a channel tap,
not `rate=wide`.

Pre-roll frames come first, flagged 32 (pre-roll), with a `seq` of their own from 0; the live frames follow with
no gap between. The board keeps its history only while it is in use (a stream that reads it opened in the last 10
minutes), so the first stream after a quiet spell gets what it has, possibly none: count the flagged samples
(`benchapi` writes `preroll_samples`). A wideband stream, or a line protocol capture or trigger (section 14), keeps
only the wideband history warm; a channel's stream keeps the channels' histories warm only with `pre_s` or a
trigger. The channels' histories cost the DSP more.

Where the front end lost samples (its ring was full), the board puts zeros in their place so the samples keep real
time. A frame with such filler is flagged 1 (gap), and the done frame's `filled` counts it, in the stream's own
samples (a channel's share of the wideband filler); `benchapi` adds it to `gap_samples`.

cs8 and cu8 have the wideband 8-bit gain for a channel tap too: the front end's full scale is `scale_db` under
cs8's (42.14 dB on the LR2021, whose channel noise is about 1.4 of cs16's 32767: at the ADC's full scale cs8 was all
zeros; 0 on the RTL-SDR). The start and done frames give `scale_db` for every format (0 for cs16 and cf32): a level
in the samples less `scale_db` is dBFS.

Start frame:

```json
{"type": "start", "run": 7, "fmt": "cs16", "rate": 48000.0, "freq_hz": 438162500.0, "center_hz": 438162500.0,
 "scale": 32767, "sc_rate": 288000.0, "scale_db": 0.0, "gain_db": 29.7, "clock": {...}}
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
`retuned`), `samples`, `gaps` (frames flagged gap), `filled` (filler samples, above), `scale_db`, `clip_pct`, and
for a trigger `trigger_sc`, `level_db`, `floor_db` (the channel's power when it fired and its floor before, in dB
full scale).

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
| `pi`, `lcss` | voice B-F's EMB; `null` otherwise |
| `fec` | what each code corrected: `slot_type_corrected`, `bptc_ok` and `bptc_corrected` (of 196 bits), `rs129_ok` (voice LC), `crc_ok` (CSBK, data and PI headers); voice: `emb_corrected` (B-F), `ambe_corrected` (each frame's Golay data bits), `ambe_bad` (a frame past 3); then `bit_err_est` (their sum, a floor) and `fec_ok` |
| `lc` | decoded link control when the burst completes one, else `null`: the voice LC on headers and terminators, voice E's embedded LC (with `emb_flco`), a CSBK's `csbko`, `fid`, `crc_ok`, `src`, `dst`. An LC has `type` (`grp_voice`, `priv_voice`, `talker_alias_hdr`, `talker_alias_blk1`-`3`, `gps_info`), `flco`, `fid`, `raw`, and by type `src`, `dst`, `group`, `so` and its flags, `lat`/`lon`, or the alias `format`/`length` |
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

`WS /tx?session=s-3f2a&lease=L&freq_hz=438487500&via=sdr|modem|radio:NAME`. Gates, all of which must be open, or
the socket closes with `refused`:

1. The bench was built with TX (`CONFIG_RDOF_BENCH_TX` on the board) and its front end can transmit (`info.sdr.tx`).
2. `freq_hz` is in the bench's TX list (`info.tx.freqs_hz`) or one of its bands (`info.tx.bands_hz`, low and high
   Hz), which only the settings file or the daemon's config changes, never this API.
3. The session holds a lease on it with `tx: true`. A lease has no `allow_rf` default: the client asks for it.
4. Key time per transmission ≤ `info.tx.max_key_s`, power ≤ `info.tx.max_dbm` (dBm at the antenna), or without it
   `info.tx.power` (an RF level on the transmitter's range, −1 when `max_dbm` is set). The bench clamps; lowest by
   default.
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

- `via=modem` is the board's SDR modem (sdrmodem), when its front end can transmit (not an RTL-SDR). A bench job
  comes first: when a roamer has the modem, the job takes it as `outbound` does (the roamer's modem parked, a call it
  was sending cut, given back after), and its bursts go out as built. (It used to go through the router's
  transmitter pool, whose transceiver could refuse the stream and framed the call itself; it doesn't now, and
  `info.tx.router` is false.) The transmitter sends at the job's power (`--power-dbm`, or the older `--power`
  0-255), under `bench.tx`'s `max_dbm` (or `rf_level`) at most: a modem's tune lowered to it on the modem's `tx` map
  (its least under a cap without one). `via=radio:NAME` is a bench radio with `"proto": "mmdvm"`, through its link's guard (lowest power, TX
  data only on the frequency), and not while it's linked. `via=sdr` (I/Q, `fm`) is `unsupported`. Both paths drive
  an MMDVM modem in simplex: `SET_CONFIG`, `SET_FREQ` (RX = TX), `SET_MODE` DMR, one `DMR_DATA2` a burst, paced at
  60 ms a burst, three ahead of air time. A burst's control byte comes from its sync (voice sync, data sync with its
  slot type's data type) or, for voice B-F, from `n`, its EMB (LCSS: first B, last E, single F; continuation C, or D
  after C) or its place after the last voice sync. So a late entry may start at B-F with no voice sync before it (one
  starting at D is taken as C unless `n` says so); a B-F after F with no A between is refused.
- What may go out is `bench.tx` in the settings: `{"freqs_hz": [whole Hz, up to 8], "bands_hz": [[low, high] in
  whole Hz, up to 8], "max_key_s": 10 (60 at most),
  "max_dbm": -20 (dBm at the antenna) or "rf_level": 0 (an RF level, 0-255, on the transmitter's range), "arm_gpio":
  -1}`. Without it nothing transmits. `arm_gpio` is a
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

Where a radio is attached comes from the bench's config. On the board each is a device in the settings (a `radio`
speaks `odr-debug`, a `modem` speaks `mmdvm`), named in `bench.devices` (fw/config-v3.md §4.6):

```json
{"name": "rf1", "kind": "radio", "port": {"uart": 1, "tx": 17, "rx": 18, "baud": 921600}}
{"name": "hs1", "kind": "modem", "port": {"usb": "cdc-acm", "vid": "0483", "pid": "5740"}, "hears": false, "transmits": false}
{"name": "rf2", "kind": "radio", "port": {"usb": "uart", "serial": "FT1A2B3C", "baud": 921600}}
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
| `common/src/benchcfg.c` | `bench.devices` and `bench.tx` from the settings |

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
  them costs about 0.11 of a core (`history_load` in the v1 status; the wideband ring alone 0.04 on a W12), so it
  runs only within 10 minutes of a stream that reads it being opened. The wideband ring rounds as the streams do, so
  a wide cs8 or cu8 stream's frames go from the DSP as references to the ring, and the stream task copies the bytes in
  (`stream_load`: the DSP's share for the I/Q streams' frames). A reference the ring has moved past (the stream more
  than 1.8 s behind) is dropped: the client sees the samples missing. A stream past its `seconds` ends 2 s after its
  last frame (or 20 s late), not while frames still come. A stream's pre-roll goes out from the stream task, read from the rings; it doesn't hold up
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

## 14. The line protocol (TCP)

The bench daemon's own protocol, served by the gateway over TCP, so that a tool written for the daemon's client works
against a board or rdofradio with a client of this project in place of the daemon's: `tools/bench/rfgbench.py`, with
the daemon client's subcommands, options, output and exit codes (section 14.5). It sits beside v1 (sections 3-8) and
shares its parts: the sessions, the burst feed, the I/Q history and the transmit job.

### 14.1 Framing

- TCP, port 7373 by default: `CONFIG_RDOF_BENCH_LINE_PORT` on the board (0: off), `--bench [ADDR:]PORT` on rdofradio.
- Found by DNS-SD: a gateway lists the line as `_rfgbench._tcp` (instance: its info's device name), TXT `kind`
  (`esp32s3-gateway`, `rdofradio`), `ver` and, on a board, `mac` (the Wi-Fi station's).
- One request a connection: a JSON object on one line. The replies are JSON objects, one a line. A reply line with
  `"bytes": N` is followed by N bytes of payload (I/Q samples) before the next line.
- Closing the connection cancels the request: a capture stops, a transmission unkeys, a session ends.
- Errors are `{"error": "text", "code": "..."}`, the request's last line, with the codes of section 1 plus the
  daemon's `deadlock`. A wait is reported as the daemon's: `{"state": "queued", "kind", "eta_s", "worst_s",
  "ahead", "blockers"}`, then `{"state": "granted", "kind", "waited_s"}`. `{"note": "text"}` is for the user.
- At most 8 connections at a time on a board, 32 on rdofradio; one more is answered with `busy` and closed.

### 14.2 Ops

Field names are the daemon's (frequencies in MHz, unlike v1).

| op | request | replies |
|---|---|---|
| `info` | | `window` (`enabled`, `lo`, `hi`, `center` MHz, `fs`, `gain`, `dc_guard_khz`, `ppm`, `ppm_source`, `ppm_tol`, `ppm_ref`, `lo_ppm`, `fs_ppm` (section 14.7), `tx_lo`, `tx_hi` MHz: where the transmitter tunes now, each platform's own limit (a W12: 1 MHz either side of `center`, the LR2021's one synthesizer; rdofradio in full duplex: the modulator's reach around its TX LO); both 0: anywhere it can), `tune` (null, or `session`, `label`, `for` `"session"` or `"request"`, `state` `"tuning"` or `"tuned"`), `sdr` (`"shared"` when the receiver runs), `channels` (one per receiver channel and TX frequency: `freq`, `ch`, `in_window`, `listed`, `caps`, `claimed_by`, `keyed`), `tx_bands` (`[low, high]` MHz each: where else it may transmit, `bench.tx.bands_hz` or rdofradio's `--bench-tx LOW-HIGH`; a frequency in one is a transmit frequency as a listed one with `tx` is), `stations` (`modem`: the gateway's own modem, `kind` `sdrmodem`, or `mmdvm` with the modem's description in `model` and `mmdvm_tx` in place of `sdr_tx` in the channels' caps; `tx`, `held_by`, `waiting`), `modem` (`held_by`, `waiting`, `next_job_eta`), `default_caps`, `device` (kind, name, version), `signal_modes` (section 14.6), `jobs` (`bench_tx`, `bench_repeater`, `outbound`, `mssim`, `signal_tx`: null when this bench can run it, else the reason it can't, section 14.9) |
| `status` | | `sdr`, `receiver` (`seconds`, `overflows`, `clip_pct`, `dsp_skipped`, `decode_dropped`, `captures`), `held`, `waiting`, `sessions`, `tx` (section 7's) |
| `session` | `label`, `expect_s`, `max_s` | `{"session": ID, "label"}`, then nothing until the connection closes (the session ends with it) or the session's `max_s` runs out (`{"ended": "max_s"}`) |
| `claim` | `session`, `freqs`, `caps`, `wait_max` | queued / granted, `{"claimed": [MHz], "window"}` |
| `release` | `session`, `freqs` (none: all), `hold`, `tune` | `{"released": [MHz], "hold", "tune"}` |
| `tune` | `session`, `center` (MHz or `"auto"`), `gain` (dB or `"auto"`), `taps` ([MHz]) | `{"tuned": {"session", "window", "channels": [{"freq", "ch", "id"}]}}` once the receiver runs at it |
| `hold` | `session`, `expect_s`, `max_s`, `wait_max` | queued / granted, `{"held": ID}`: the modem is the session's until `release --hold ID` or the session ends |
| `run` | `session`, `job` (section 14.3), `argv`, `kind`, `expect_s`, `max_s`, `wait_max` | queued / granted, `{"started": ID}`, `{"out": line}` and `{"err": line}`, `{"exit": code}` |
| `bursts` | `session`, `freq`, `seconds`, `max_bursts`, `until_end`, `cc`, `soft` | `{"ready": t0}`, `{"burst": record}`, `{"done": summary}`, or `no_signal` |
| `listen` | `freq`, `seconds` | `{"burst": record}` |
| `capture` | `session`, `freq`, `seconds`, `pre_s`, `gain`, `mode`, `offset_khz` | `{"ready": t0, "shared": true, "center": MHz, "pre_s", "fs", "sc0"}` (sc0: the first sample's, so a client places every frame, gaps as zeros), `{"iq": {"seq", "n", "sc", "flags"}, "bytes": N}` + cs8 samples, `{"done": meta}` |
| `trigger` | `session`, `freq`, `bw`, `pre_s`, `post_s`, `threshold_db`, `min_ms`, `timeout_s`, `fmt`, `until_end`, `max_s`, `floor_db`, `floor_s` | `{"armed": {"floor_db", "rate"}}`, `{"iq": ...}` + samples (`sc` the channel sample's index), `{"done": meta}`, or `no_signal` |
| `track` | `session`, `freq`, `mode` (`fm`, `4fsk`), `bw`, `interval_ms`, `line_ms`, `threshold_db`, `min_ms`, `timeout_s`, `max_s`, `post_s`, `drift_s`, `floor_db`, `floor_s` | `{"armed": {"floor_db", "rate"}}`, `{"track": line}` an interval, `{"done": summary}`, or `no_signal` (section 14.8) |
| `activity`, `burstlog`, `bridge` | | `unsupported` |
| `mssim` | `session`, `action` (`arm`, `disarm`, `status`), arm: `freq`, `cc`, `power`, `seconds` (300, at most 1800) and the profile's fields; `since` | `{"mssim": {"armed", "arming", "answered", "ended", "log": [entry], "next"}}` (arm: `{"mssim": {"armed": true, "freq", "cc", "note"}}`) |

- `mssim` is a timed confirmed-data responder (rdofradio and a board with an SDR modem; `rdof/mssim.h`): it plays a radio that answers
  confirmed data packets N slots after they end, for testing the far end's retries and turnaround. `arm` takes the
  transmitter as a transmit job does (a transmit frequency with a receiver channel on it, the transceiver paused or a
  duplex repeater stopped and its modem simplex; on an MMDVM modem its timing is the serial port's and a key-up's, learned from what's heard back) and holds it until `disarm`, `seconds` or the profile's `count`. The profile: `answers` (1-8
  steps, packet k gets step k, the last one after, or k mod n with `"repeat": true`), each `answer` (`ack`, `nack`,
  `sack`, `none`, `ack2`: an ACK and the same burst 60 ms later), `slots` (1-200: the answer's first burst starts
  this many 30 ms slots after the end burst's start), `nack_type` (1), `sack_type` (0), `resend` (SACK: the block
  numbers asked for again); one step may be given flat. `end`: `block` (the last block), `terminator` (the
  terminator after it; where one was expected when none came) or `learn` (a terminator when the sender's previous
  packet had one). `to`, `from`: answer packets between these IDs only. Answers are to individual confirmed packets
  (DPF 3, A=1); the response header swaps the IDs and keeps the SAP and N(S); the sync is MS, or in TDMA direct mode
  the other timeslot's when `slots` is odd; `sync` (`ms`, `bs`, `ts1`, `ts2`; `auto` the default) forces one (no
  CACH with `bs`). A log `entry` per packet: `seq`, `src`, `dst`, `sap`, `ns`, `fsn`, `blocks`, `heard`, `dbsn`
  (hex: serial numbers with good CRC-9s), `rate_dt`, `terminator`, `end`, `end_heard`, `step`, `family` (the
  packet's sync), `sync` (the answer's), `answer`, `slots`, the positions `header48`, `end48`, `asked48`,
  `placed48`, `air48` (48 kS/s receiver samples; -1 none), `sent`, `failed`, then `turnaround_us` (start to start)
  and `gap_us` (from the end burst's 27.5 ms) when sent, and `air_turnaround_us`, `air_gap_us`, `air_late_us` where
  the answer was heard back. `status` gives the entries after `since`, as many as fit; `next` is the `since` for the
  next read. `rfgbench.py mssim arm|disarm|status|watch`.
  An `arm` where no channel decodes DMR (or past the transmitter's reach) tunes the receiver there first, as `trigger`
  does; that tune is held while the responder is armed (until `disarm` or its `seconds`, or its session's end), and
  other requests outside it are `busy` meanwhile.
- `tune` moves the receiver's window for the session, as `PUT /sdr` under an exclusive lease (section 5; the same
  code on a board): fields left out stay as they are, `"auto"` is the settings', `taps` (up to 4 on a board) are
  receiver channels at any frequency in the window (ids `T1`...) that decode like configured ones, `[]` drops them.
  A center with no channel in the window gets a tap an eighth of the rate above it. The reply comes once the
  receiver runs at the new tune (about 0.3 s); a tune the bench can't do is `refused` (or `bad_request`) and the
  one before it stays. The window is the session's until `release` with `"tune": true` (or a release of
  everything) or the session ends, its connection dropped included: then the settings again. Meanwhile another
  session's `tune` is `busy`, and so is its request for a frequency outside the window (`bursts`, `listen`,
  `capture`, `trigger`, `rx`); one inside the window runs. On a board an HTTP exclusive lease makes `tune` `busy`
  and the reverse. A tune or its end ends the streams cut from the window (`bursts` and `listen` with `done.reason`
  `"retuned"`, `capture`, `trigger` and `rx` with `busy`). A request that comes while a tune or its end is being
  applied waits for it and then runs (or is `busy` if the tune it finds is another session's).
- rdofradio's tune points channels at the taps (each on a channel the site's repeater or transceiver isn't using, from
  the last one down; one of theirs when none is spare) and moves the RX LO only when the taps don't fit the window
  as it is (within 0.4 of the rate of the center, 25 kHz clear of the DC): a Pluto's 19.872 MS/s window holds a whole
  plan, so a job inside it costs the site one channel. The LO goes where asked when the taps fit there, else an
  eighth of the rate under the first tap. While it's moved, the other channels hear nothing (their bursts dropped)
  and `freqs` lists only the taps. A tap's bursts go to the bench, the modem and the responder, never to the network.
  The I/Q history (or, with the FPGA channelizer, the channels' history) starts again where the LO moved. A `gain`
  sets the SDR's receive gain (`--gain` is the settings'). The settings come back with the untune.
- `trigger`, `rx`, `capture` and `bursts` at a frequency outside the window (for `bursts`, any frequency no
  channel decodes DMR on), with or without a session, tune for themselves when nobody holds the window and no other
  request is using it: `{"note": "tuning the receiver to F MHz for this request"}`, the center an eighth of the rate
  under the frequency (clear of the DC) with a tap on it (a board's tap decodes DMR), then the op as usual; the
  settings come back when the request ends. `"tune": false` refuses instead; while another request uses the window,
  or another session holds the tune, it is `busy`.
- So do transmit jobs (`run`'s bench_tx jobs, `outbound`, `tx`) on a frequency past the transmitter's reach around
  the center (`tx_lo_hz`-`tx_hi_hz` in the window: a W12 transmits within 1 MHz of where it receives), and a `run`
  job that hears (`listen`, `listen_after`) where no channel decodes DMR: the receiver tuned there for the job, back
  after it. A frequency `bench.tx` doesn't list is still `refused`.
- `claim` refuses a frequency the bench doesn't list (no receiver channel, not in `bench.tx`). Receiving needs no
  claim; the HIL tools claim anyway, and a claim keeps another session's job off the frequency.
- `capture` sends the receiver's wideband samples, cs8 (int8 I, Q; full scale 127), at the front end's rate, with up
  to `pre_s` seconds from before the request (the I/Q history, section 9), at most half of what the history holds
  (the other half is room for the client to be behind; about 0.9 s on a board). The receiver isn't retuned: `gain` and
  `offset_khz` are noted in the metadata and otherwise ignored, and a frequency outside the window is `refused`.
  `rfgbench.py capture --quiet` holds the modem in a session of its own for the capture, so no other session's job
  transmits during it (a `hold`, section 14.4).
  `meta` is what `FILE.json` holds: `freq` and `center` (Hz), `fs`, `fmt` `"cs8"`, `gain`, `ppm`, `ppm_source`, `ppm_tol`, `ppm_ref`, `lo_ppm`, `fs_ppm` (what's left in the samples), `freq_tol_hz` (section 14.7), `t0` (UTC of
  sample 0), `seconds`, `overflows`, `gap_samples`, `clip_pct`, `pre_s`, `scale_db`. `gap_samples` counts the
  samples the capture doesn't have: those the history overwrote before they went (sent as nothing: the client
  fills them from `sc`) and the board's filler where the front end lost samples (zeros, kept in the history with
  where they are; their frames are flagged 1). `overflows` is 1 when there are any. A front end with a wider ADC gives its low
  bits, where a bench signal is: `scale_db` is how far cs8's full scale is under the ADC's (24.08 on a PlutoSDR:
  ×16, clipping over -24 dBFS; 42.14 on the LR2021: ×128, clipping over -42 dBFS, its noise about 2 counts rms at
  53.5 dB gain; a level in the capture less `scale_db` is the ADC's dBFS). The board's `WS /iq?rate=wide` cs8 and cu8
  have the same gain, and their `start` message gives `scale_db`.
- `trigger` cuts the channel at `freq` from the wideband history: mixed to 0 Hz, low-pass filtered to `bw` (a
  Hamming-windowed sinc, 8 taps a decimated sample), decimated to about 48 kS/s (`fs / round(fs / 48000)`, the
  metadata's `rate`). It learns the floor as the median 1 ms block over the first `floor_s` (1 s; from the history
  when it is warm, else live; blocks of exact zeros left out, as a channelizer's output quantizes a quiet band to
  zero, and -100 dB when every block is), unless `floor_db` is given, and says `armed`. It fires when `min_ms` blocks in a row
  are `threshold_db` over the floor, then sends from `pre_s` before the rise (as far as the history goes) to
  `post_s` after it; with `until_end`, to `post_s` after the last block over half the threshold, at most `max_s`
  after the rise. Samples are cf32 or cs16, full scale 1.0 = cs8's 127. Nothing in `timeout_s` (60) is
  `no_signal`; its message gives the floor, the loudest 1 ms block since it armed, and how many times it fell behind
  the history (restarted at its oldest sample, the run so far lost). `meta` is the daemon's: `freq`, `center`, `fs` and `rate` (the channel's), `wide_fs`, `fmt`, `bw`,
  `t0`, `trigger_time`, `signal_start_sample`, `trigger_sc`, `level_db`, `floor_db`, `snr_db`, `threshold_db`,
  `seconds`, `samples`, `pre_s`, `ppm`, `ppm_source`, `ppm_tol`, `ppm_ref`, `lo_ppm`, `fs_ppm`, `freq_tol_hz`, `corr_hz` (where the channel was mixed from, past the nominal offset, for the crystal; section 14.7), `gap_samples` (output samples lost, and the wideband filler in what it sent, in
  output samples), `clip_pct`, `scale_db`. The engine filters on its own thread
  (up to twice the input since the last tick), about 20 multiplies a wideband sample for one trigger.
  With no wideband history but a channelizer's own outputs (rdofradio `--fpga-chz`; `info`'s window says
  `"iq": "channels"`, else `"wideband"`, or `"none"`), the receiver keeps each channel's outputs for 2.5 s instead,
  each with the NCO offset it was mixed from. `trigger`, `track`, `capture` and `rx` on one of its channels (within
  3.25 kHz of one) take that channel's I/Q from it, pre-roll and floor as from the wideband history: no filter, `bw`
  the channel's, the AFC's NCO steps taken out (a carrier stays where it is through a step). A `capture` there is the
  channel's: cs16 (or `cf32` with `fmt`) at about 48 kS/s, centered on `freq`, the ready line and meta with `fmt`,
  `"channel": true`, `bw`, `wide_fs` and `wide_center`. Another frequency is `unsupported`.
- `bursts` and `listen` send the receiver channel's bursts at `freq` as `dmr-bursts` records (section 6.3's fields
  with the daemon's names: `t`, `dt_prev_ms`, `grid_err_ms`, `slot_ab`, `sync`, `kind`, `cc`, `cfo_hz` (the burst's carrier
  from its true frequency, the crystal taken out: section 14.7), `cfo_tol_hz`, `level_dbfs`, `dev_hz`, `pi`, `lcss`, `fec`, `lc`, `raw_hex`; no `sym_err` yet), the summary the daemon's (`bursts`, `kinds`,
  `first_t`, `last_t`, `seconds`, `cfo_hz`, `level_dbfs`, `ended`). Beyond the daemon's: `sync_errors` (the sync
  field's bit errors against the pattern it matched; an EMB burst's: its EMB's), and `cach`, the 24 bits heard
  before a BS-sync burst as 6 hex digits in air order (the first bit the top bit), with its TACT decoded through
  Hamming(7,4): `cach_at`, `cach_tc`, `cach_lcss`, `cach_tact_errors` (0 or 1: the code always decodes, so 1 may be
  a worse error read as one); `"cach": null` on other bursts. The CACH is sliced with the burst's own levels, so a
  key-up's first CACH may be the ramp. The summary adds `lost` (`overflows`, `dsp_skipped`, `decode_dropped`,
  `queue_drops`: what the whole receiver lost while the stream ran, counts, not this channel's alone) and
  `complete` (true: nothing lost and no record dropped, so a burst missing was not heard on the air). `info`'s
  `features` names both (`bursts_cach`, `bursts_lost`). `bursts` includes idle bursts (a repeater's hang
  is all idle); `listen` leaves them out. The bench's own transmission is in them: the receiver hears it as any
  other signal. BS-sync bursts (a repeater's outbound) carry the slot from their CACH; `t` and `sc` are at the
  sync's center, the same point as an outbound job's records. Several streams can run at once, one a channel.
  With `seconds`, the client ends the stream itself 5 s past it if the gateway hasn't, and writes the summary, so
  `--out` is always complete.
- `bursts` with `"envelope": true` also measures the channel's carrier envelope, as the bench analyzer does on a
  capture, so a test needs no I/Q for a transmitter's keying. The channel is cut from the history as `trigger`'s is
  (`env_bw`, 12500), its floor the median 1 ms block over the `floor_s` (1) before the request unless `floor_db` is
  given (blocks of exact zeros left out: a channelizer quantizes quiet to zero, and the floor would sit under any
  residual carrier). A measured floor follows a close carrier: it is at least 40 dB (30 + `on_db`) under the strongest 1 ms
  level seen in the last second or coming in the next 15 ms, so the carrier keys 30 dB under its own level and a receiver's
  quantization at low gain doesn't. A `floor_db` the tool gives is used as given. The 0.1 ms power envelope (a centered mean) is keyed at `on_db` (10) over the floor and unkeyed under
  `off_db` (`on_db` - 3); an edge is interpolated between two outputs. A ramp runs from the envelope leaving the noise
  (`on_db` - 4 over the floor) to 3 dB under the plateau (the median 1-3 ms in from the edge), and back at the
  key-down; a step is the 1 ms envelope moving over `step_db` (3) in 1 ms, from 2 ms in to 2 ms before the key-down.
  Segments under 0.5 ms are left out. Each keyed segment is a record once its bursts are in (up to 0.5 s after its
  key-down): `{"segment": {"t", "up_ms", "down_ms", "t_down", "on_ms", "ramp_up_ms", "ramp_down_ms", "level_dbfs",
  "end_dbfs", "peak_dbfs", "min_dbfs", "clip_pct", "power_steps", "steps", "bursts", "burst_sc", "burst_t",
  "unexplained_ms", "up_sc", "open_start", "open_end", "gap"}}`. `t` and `t_down` are UTC at the edges, `up_ms` and
  `down_ms` from the stream's start (the summary's `envelope.t0`), `level_dbfs` and `end_dbfs` the plateaus after
  the key-up and before the key-down, `peak_dbfs` and `min_dbfs` the 1 ms envelope's extremes 2 ms in from either
  edge, `clip_pct` the I/Q components at the ADC's rails over it, `steps` `[ms, dB]` (at most 8; `power_steps`
  counts all). `bursts`, `burst_sc` and `burst_t` are the stream's bursts whose sync center is inside it (the same
  `sc` and `t` as their records); `unexplained_ms` the keyed time no burst's 27.5 ms covers: a PA left up, a carrier
  where per-burst keying is expected. A segment keyed when the stream began, or cut by lost samples, is
  `open_start` (no ramp up), `open_end` (no key-down, `null` figures) or `gap`. A ramp is `null` where the carrier
  didn't come up from the noise or go back to it (a repeater's hang). The summary adds `envelope`: `t0`, `bw`,
  `rate`, `gain`, `floor_db`, `floor_used_db` (the highest floor it keyed against), `noise_dbfs` (the 1 ms envelope's 10th percentile, the analyzer's noise), `dbm_offset`
  (dBFS to dBm, rough: the floor taken as thermal noise in `bw` with a 3 dB noise figure; `dbm_source` "noise"),
  `clip_pct`, `segments`, `segments_dropped`, `power_steps`, `overlaps` (bursts starting under 27.5 ms after the one
  before) and `overlap_ms` (at most 16), `unexplained_carrier` (`[up_ms, unexplained_ms]` of each segment with over
  5 ms, at most 16) and `unexplained_ms`, `gap_samples`. `info`'s `features` says `bursts_envelope`. The cut costs
  what a `trigger`'s does (about 20 multiplies a wideband sample); `listen` takes no envelope.
- `capture` with `"raw": true` sends the ADC's own samples, cs16 (int16 I, Q; 12 bits, LSB aligned, full scale 2048), at the front
  end's rate, nothing filtered: `rfgbench.py capture --raw`. `seconds` long, from when it starts: no pre-roll, and no history
  behind it. `rate` (520834-61440000 S/s, `--rate`) gives it a rate of its own: the chip changes for it and back after (the
  receiver is deaf through it anyway), and nothing transmits meanwhile, since the transmitter shares the rate. After it the
  transmitter goes on where its stream would have been without the pause (a transmission under way has a gap the capture's
  length), the receiver makes up what its silence came short of or leaves out what it ran over, and both calibrate again;
  any move the calibration still finds, it takes up. `gain` (dB) is the RX gain for the capture, the receiver's own
  again after (`--gain`; without it, the receiver's). The front end
  is tuned 10 % of the rate above `freq` when `freq` isn't within 35 % of the rate of where it is, so any frequency the chip
  tunes works; `ready`'s and `meta`'s `center` say where. `info`'s `features` says `capture_raw`; a receiver without it answers `unsupported` (today a PlutoSDR running
  `--fpga-chz` has it). Its DMA blocks (up to 32 MB each) are all queued before the first sample, so the DMA fills them back to
  back and its samples are in a row; if the ADC core's DMA overflow flag says otherwise, it fails (`unavailable`) rather than
  hand over a gap. They're copied to memory before the first goes out, so its length is what the DMA memory holds: about 2.5
  s at 19.872 MS/s on a PlutoSDR with 256 MB of CMA (`unavailable` otherwise, saying why). The receiver is deaf for about
  0.8 s more than the capture (the blocks are set up and copied), and the log says how long in one line. One at a time; a
  second waits its turn. **While it's made the receiver hears nothing**: the channelizer is off, and each bursts stream gets
  filler and counts the samples lost (`complete: false`). The loop goes on turning on that silence, so the transmitter and every
  count keep real time: a transmission keeps going, and a capture can catch its own board's. No reboot, and the bitstream stays.
  `meta` is as `capture`'s, with `fmt` `"cs16"`, `raw` `true`, `bits` 12, `full_scale` 2048, `clip_pct` (the rails, I and Q),
  `scale_db` -24.08 (a level from cs16 scaled by 32768, less this, is the ADC's dBFS). `t0` comes from when the DMA block ended.
- `bursts` with `"ramp": true` (it implies `envelope`) also measures each burst's PA ramp on the gateway, with no I/Q streamed.
  The figures are those of 14.8.2, against the burst's own settled level and payload; the burst's decoded symbols place it, so
  voice B-F are placed as well as a burst with a sync. Each is a record once what follows the burst is in (about 24 ms after its
  sync): `{"ramp": {"t", "sc", "slot", "kind", "first", "align_ms", "settled_db", "ripple_db", "rise_ms", "rise_30_3_ms",
  "overshoot_db", "overshoot_t_ms", "settle_1db_ms", "settle_0p5db_ms", "sym1_db", "err5_mean_db", "err5_abs_db", "err12_mean_db",
  "err12_abs_db", "fall_ms", "fall_3_30_ms", "pre_db", "empty_slot_db", "f_first1ms_hz", "f_first1ms_unreliable", "f_next2ms_hz",
  "f_next2ms_unreliable", "dev3_hz", "clipped"}}`. `sc` and `t` are the burst's sync center, as in its burst record. `align_ms` is
  how far the symbols moved it from there. `first`: over 90 ms after the burst before. `clipped` counts inputs at the rails over
  the payload, `null` on a channel from the FPGA, where nothing counts them. The summary's `envelope` gets `"ramp": {"measured",
  "lost", "dropped", "edge_res_us"}`; `lost` bursts' samples had gone, `dropped` ones went unmeasured because too many were
  waiting. `edge_res_us` is what the channel's bandwidth allows: set `env_bw` wider to see a faster edge. `info`'s `features` says
  `bursts_ramp`. It keeps the channel's last 0.68 s of outputs (256 KB, PSRAM on a board).
- `bursts` with `"raw": true` does the same from a raw capture of its own (as `capture`'s, above) instead of the receiver's
  channel: `rfgbench.py dmr-bursts --raw [--ramp] [--rate R]`. Anywhere the front end tunes, at `rate` (a multiple of 48000 in
  528000-61440000 S/s, default 2400000), `seconds` long (default 5). Once the samples are in, the gateway mixes the channel to
  0 Hz, filters it to `env_bw` and takes it to 48 kS/s (as the envelope's cut), runs its own DMR decoder on that, and sends the
  bursts, the envelope's segments and, with `ramp`, the ramps, as a live stream's: only the results go, no I/Q. `ready` adds
  `raw`, `fs` and `center` (MHz); `{"captured": {"t0", "samples", "fs", "center", "clip_pct", "bits"}}` comes when the samples
  are in (`clip_pct`: the ADC's own rails, I and Q), then the records, then `done` (`reason` `"time"`; `complete` is about the
  decoding alone: the receiver's own losses during the capture aren't its). The burst's `sc` counts the capture's samples from
  0, `t` from its `t0`; the floor is learned over the capture's first `floor_s` (give `floor_db` when it starts keyed).
  `cfo_hz` is from the channel's center, the crystal taken out as the receiver's. The receiver is deaf for the capture, as a
  raw `capture` makes it; the processing after runs beside it (a quarter million samples a tick). `info`'s `features` says
  `bursts_raw`.

### 14.3 Jobs

`run` doesn't run a program: the client reads the bench tool's command line (`bench_tx.py` and `bench_repeater.py`,
the tools the HIL tests run on the daemon's modem) and sends the job it describes; the gateway does it with its own
DMR stack and transmitter and answers in the tool's output (its JSON lines on `out`). `job` is the tool's options,
by their long names with `_` for `-`:

```json
{"tool": "bench_tx", "freq": 438.1125, "cc": 2, "power_dbm": -10, "cmd": "voice", "listen_after": 0,
 "src": 9990301, "dst": 9990199, "private": true, "superframes": 6, "family": "ms", ...}
```

- `bench_tx` `voice`, `csbk`, `data` go out through the bench's TX path (section 7: the SDR modem, `bench.tx`'s
  frequencies, key time and power) in DMO timing, one burst every 60 ms; `listen` (and `listen_after`) reports what
  the receiver channel at `freq` hears, as the tool's `--json` lines and per-call summaries.
- `bench_tx` `outbound` sends a repeater's idle outbound on `freq`: a BS-sync idle burst every 30 ms on both
  slots, each with the CACH as an MMDVM sends it (AT 0, TS1/TS2 alternating) and `cc`, keyed from the first slot
  for `seconds` (0.5-60) with no handshake. `slots` (`1,2`, `1` or `2`) picks which slots' records are written;
  both go out. The slot clock is the modem's own and never steered: a jump is counted in `skips`. Records: one
  `start` (`freq`, `cc`, `slots`, `first_slot`, `rate`, `period_samples`, `period_ms`, `sync_off`, `sc0`, `t0`,
  `ppm`), then one per burst with `slot`, `sc` and `t` at the sync's center (the TX sample clock: the slot's start
  plus `sync_off`, 775 at 48 kS/s), then `summary` (`slots`, `ts1`, `ts2`, `skips`, `seconds`). It doesn't go
  through the router: on a roamer it takes the raw SDR modem (the roamer pauses, a call it was sending cut) and gives
  it back after. On the W12 a key-up this long is one streamed transmission in 1.35 s
  runs; their seams are in lr2021-iq.md R3.49.
- An `outbound` with `voice` also carries voice calls in its slots, as a repeater's downlink does: a list of up to
  8 calls, each `{"slot": 1|2, "src", "dst", "group" (true), "start_s" (0), "superframes" (3), "headers" (3),
  "late" (-1), "terminators" (3), "pi", "ambe_hex"}`. A call is BS bursts on its slot, one every 60 ms: its voice
  LC headers, then voice superframes (voice sync on A, the EMB with the job's `cc` and the call's embedded LC on
  B-E), then its terminators with LC. `late` 0-5 is a late entry: no headers, and the first superframe starts at
  voice burst A-F (`headers` 0 alone is `late` 0). `ambe_hex` is the call's own AMBE frames (9 bytes each), else
  silence. A call starts `start_s` after the key-up, rounded to a slot of its number, and no earlier than the
  first slot still to be queued. Each slot's calls can't overlap, and every call must end before `seconds`; the
  slots of the other number keep their own calls and idle bursts, so one slot can be mid-call while the other's
  call starts, or goes on after it ends. Refused before anything keys, with the option named: a bad field, an
  overlap, a call past `seconds`, `late` with headers, and a bench whose platform can't push voice (`unsupported`).
  The bursts go to the duplex modem as its host's `DMR_DATA1` / `DMR_DATA2`, in the slot queue of their number
  (prebuffer one). Slot k's burst goes when the slot clock's next slot to queue is k - 1, or earlier behind the
  call's burst for k - 2, so it lands in k; one whose slot is already queued (the engine was late) is dropped
  and its slot is idle (`"late": true` in its record). A jump of the slot clock (`skip`) can put a burst queued
  across it one slot of its number late; the log says so. Records: a voice slot's record names its burst (`sync`
  `BS_DATA`, `BS_VOICE` or `EMB`; `kind` `VLC_HDR`, `VOICE_A`-`VOICE_F` or `TERM_LC`; `call` (1-based), `src`,
  `dst`, `burst` (from 0)) in place of `IDLE`; a `call` record follows each call's last slot (`index`, `slot`,
  `src`, `dst`, `group`, `first_n`, `last_n`, `bursts`, `late`, `late_entry`); `summary` adds `voice` (`calls`,
  `ts1`, `ts2`: voice bursts sent; `late`). The log has a line a call (its slot, from, to, bursts, first slot).
- `family` on `voice`, `csbk` and `data` (`ms`, `bs`, `ts1`, `ts2`), a voice job's `headers`, `terminators` and
  `late` go out as asked: every job takes the raw SDR modem (a roamer's parked, given back after). `outbound` is BS
  only.
- A carrier offset, for a receiver's bit errors against frequency error: `offset_hz` (`--offset-hz`, ±2500 Hz at
  most, so a 12.5 kHz channel's signal stays inside it) sends a `voice`, `csbk` or `data` job's bursts that far
  from `freq`; the claim, the lease and the plan check stay on `freq`. `offset_sweep` (`--offset-sweep FROM:TO:STEP`,
  Hz, STEP toward TO, 200 steps at most, every one within ±2500) steps one voice call through the offsets, each step
  `superframes_per_step` (`--superframes-per-step`, 1) superframes long; the call has steps × that many superframes
  (`superframes` left out, or equal), an `ambe_hex` looped to fill them. The headers go with the first step, the
  terminators with the last. Refused, before anything keys: a sweep with `calls` above 1, with `offset_hz`, on
  another command or past the limits, and either one on a bench whose transmitter can't move its carrier
  (`unsupported`; rdofradio with an MMDVM modem). A job with either takes the raw SDR modem, as `family` does. Each
  step starts with a record before its first burst:
  `{"type": "step", "step": k, "steps": n, "offset_hz", "t", "superframes": [first, last], "bursts": [first, last]}`
  (superframes from 0 counted from the call's first, bursts from 0 counted with the headers; `t` the first burst's
  hand-over plus 60 ms a burst, the step's modeled start on air). A constant `offset_hz` logs one line. The W12 moves
  its synthesizer per transmission rather than spending the samples' ±4064 Hz (a DMR burst's frequency peaks near
  3.7 kHz): a DMO burst is a transmission of its own, and a continuous one (a BS family's) ends where the step
  changes and keys again at the new carrier, so a sample or two at the seam may go. The Pluto turns its I/Q.
- `bench_repeater` runs a duplex repeater with no network on the job's `rx` (a receiver channel that decodes DMR;
  where none does, the receiver tunes there for the job, as for any job that hears; on rdofradio that channel decodes the job's `cc` while it runs) and `tx` (a TX frequency) for `seconds`: `slots`, `hang` (T_MSInactiv), `call_hang`, and idle beacons every
  `beacon` seconds from `beacon_phase` on (a local BS_Dwn_Act into its receive path, the hang `beacon_len`; skipped
  while keyed). Its records are the tool's: `wakeup` (a BS_Dwn_Act heard: `src`, `slot`, `while_down`), `rx` (each
  burst heard: `slot`, `control`, `rssi`, `dt`, and the CSBK opcode, LC or voice frame), `lost`, `key`, `beacon`,
  then `summary` per slot (wakeups, voice frames, data types). `rx_offset`, `tx_offset` and `port` are
  `unsupported`: the gateway's own receiver and transmitter.
- An option the gateway can't honor is `unsupported` with the option named, before anything keys. `power_dbm`
  (dBm at the antenna; `power`, 0-255, the older form) is clamped to `bench.tx.max_dbm` or `rf_level`, and the
  refusal or note says the dBm (rdofradio: see 14.4).
- The tool's log lines (Python logging's `asctime message`, UTC) come as `err`, its records as `out`; the exit code
  is 0, 1 when the transmitter failed, or -9 with `killed` when the job ran past `max_s`. A TX job's `started`
  comes when it has the modem; `listen` starts at once.
- Files a job writes (`--ambe-out`) are the client's: the job sends their bytes as `{"file": name, "bytes": N}`.

### 14.4 Arbitration

One SDR modem: a `run` with a TX job (and a `hold`) waits for it, in order; jobs of one session holding it go
straight on. A claimed frequency is the claiming session's: another session's `run` there waits. Receive ops
(`bursts`, `listen`, `capture`, `trigger`) never wait: the receiver is shared, and retuned only by a `tune` (or a
`trigger` or `rx` of its own, section 14.2), under which a request outside the window is `busy`. `wait_max` ends a
wait with `timeout`; a session that would wait on itself is `deadlock`. The modem's queue estimates come from each
job's `expect_s` (else its length as the gateway works it out).

While a request of a session waits on another session (or a request with no session), the session's running
`capture`, `bursts`, `listen` and `rx` grow by that time, and an armed `trigger` or `track` gets that much more
`timeout_s`: a test that starts a capture and then transmits still captures its transmission when its job queued.
A trigger that already fired keeps its length. `granted` carries `extended`, how many streams grew; waiting on the
session's own job extends nothing.

On rdofradio, a TX job borrows the SDR modem from the roaming transceiver for its length: a bench job comes first,
so the transceiver pauses at once (the network's calls wait; a call on the air is cut, what it queued dropped), the
job drives the modem in DMO, and the modem is set up again for the transceiver after the job. Only on the transceiver of `--mmdvm` (a
modem whose queue isn't the program's) does a call on the air end first, up to 10 s. A `bench_repeater` job does the same with a repeater in the transceiver's place:
the modem goes duplex, and the job's first key-up, heard back in the receiver's band, sets the inbound slot timing
(the transmit path's delay; the transceiver's own is restored after). An instance started as a duplex repeater
(`--duplex full`, or a config's duplex repeater) gives the job its transmitter the same way: the repeater stops (its
transmission cut), the modem goes simplex on the repeater's RX channel, the TX LO moves if the job's frequency needs
it, and after the job the LO goes back and the repeater starts again. On `--mmdvm` the repeater stops too and starts again after the job. A `bench_tx` `outbound` takes the modem the same way, as a
repeater's TX half keyed with no traffic; after it the modem is unkeyed, its role dropped, and set up again for the
transceiver. Its records' times are the modem's slot clock on the receiver's timeline plus the transmit path's
delay. On `--mmdvm` a duplex modem runs it, with `DMR_START` and `DMR_DATA1` / `DMR_DATA2`: the modem makes the CACH and its own
idle downlink, and the records' slot times come from a burst of it the SDR hears (or from the time it keyed, until
one is heard). A simplex hotspot has one transceiver and no CACH: an `outbound` or a `bench_repeater` there is
`unsupported`, naming that. The frequencies are `--bench-tx`'s. On an SDR the job transmits at `--tx-power`, or
lower when it asks a `power_dbm` under that (and under `bench.tx.max_dbm`): the SDR's gain turns down by the
difference for the job, down to the device's least gain, and the log says the dBm. A `power` 0-255 isn't applied on
an SDR (a job naming no power asks 0), and the note says so. The ESP32 boards run no repeater jobs (`unsupported`).

### 14.5 The client

`tools/bench/rfgbench.py --bench HOST[:PORT] SUBCOMMAND ...` (or `$RDOF_BENCH_LINE`; a name that isn't a host is a
gateway's DNS-SD name, any case or a unique prefix). `rfgbench.py find [--json] [--no-scan]` lists every gateway on
the host's networks with its address, device, version, receive window, frequencies by capability, TX bands and the
modem's state: the ones DNS-SD lists (a legacy unicast query out of each interface) and, for a gateway that doesn't
list itself, each address of the host's networks (a /22 at most) that takes a connection on 7373, asked for `info`.
A gateway DNS-SD lists on two networks is listed once (by its MAC, else its instance name; boards may share a device
name, and DNS-SD gives a second one's instance a suffix: `w12-rx-2`). Then `info`, `status`, `session`,
`claim`, `release`, `hold`, `run`, `capture`, `capture-trigger`, `dmr-bursts`, `listen`, with the daemon client's
options, and `tx fm`, `rx fm`, `tx am`, `rx am` (section 14.6), `tune --session ID [--center MHz|auto] [--gain dB|auto] [--taps
MHz...]` and `release --tune` of its own. Its stdout and stderr are the daemon client's (the
`READY` and `wrote` lines, the `rfoundry-bench:` notes, the JSON lines), and so are its exit codes: 0, 1, 3 nothing
heard, 69 the gateway can't be reached or can't do it, 75 busy or timed out, 77 refused. `run -- python bench_tx.py
ARGS` (any interpreter, any directory) sends the job; `--file` and `--out` files are written where the client runs.

### 14.6 Signal modes: tx and rx

A signal mode is a modulation other than DMR, as one table (`rdof/sigmode.h`; the list in `rdof/sdr/modes.h`): analog
FM (`fm`) and analog AM (`am`). `info`'s `signal_modes` lists them: `{"tx": [names], "rx": [names]}`. The fields
after the op's own are the mode's.

| op | request | replies |
|---|---|---|
| `tx` | `session`, `mode`, `freq`, `power_dbm` (or `power`), `max_s`, `wait_max`, the mode's fields; input bytes after the line | `{"tx": {"mode", "freq", "seconds", "input_bytes"}}`, queued / granted, `{"started": ID, "t"}`, `{"done": {"mode", "freq", "on_air_s", "input_bytes", "cut", ...}}` |
| `rx` | `session`, `mode`, `freq`, `seconds`, `bw`, `squelch_dbfs`, `squelch_over_db`, the mode's fields | `{"rx": header}`, `{"rec": record}`, `{"pcm": {"stream", "seq", "n"}, "bytes": 2n}` + int16 LE, `{"gap": ...}`, `{"done": {"mode", "freq", "seconds", "samples", "gap_samples", "summary"}}` |

- `tx` is a transmit job: it waits for the modem as `run` does (`expect_s` its length plus 3 s), on a frequency of
  `bench.tx`, at `power_dbm` (or `power`) capped by its `max_dbm` or `rf_level`, for at most `max_key_s`. On a W12, while the gateway's own roamer is
  transmitting it notes `waiting to transmit` and tries again each tick, for up to 10 s (then `busy`); rdofradio cuts
  its transceiver's or repeater's transmission instead (above). The source renders on the transmitting
  thread, so nothing on the network can make it stutter, except PCM that comes too slowly (counted in `underruns`).
  `done` comes once the last sample has gone out, or at `max_s` (`cut` true). `seconds` is the length the mode
  works out (FM: lead, program, repeats, the subaudio's end, tail).
- Input: a mode that takes audio (FM's `pcm`) reads the bytes the client sends after the request line, int16 LE.
  The gateway reads only as its buffer empties, so TCP's flow control paces the client: send it all at once.
  `input_bytes` in the header is what the job expects; `done` gives what came.
- `rx` cuts the channel from the wideband history as `trigger` does (`bw` 16000 by default, 1000 to 0.9 of the
  rate; about 48 kS/s) and feeds the mode's receiver. Without `squelch_dbfs` it first learns the floor (the median
  1 ms block over 0.3 s of history, exact zeros left out as a trigger's) and sets the squelch `squelch_over_db` (10) over it. The header gives `rate`,
  `bw`, `t0` and `sc0` (the first channel sample's UTC and wideband index), `seconds`, `floor_dbfs`,
  `squelch_dbfs`, `scale_db` and the streams asked for (`name`, `unit`, `rate`, `full`: the value at int16 full
  scale). A gap is `{"gap": {"sc", "samples"}}` (channel samples the history overwrote before they went: the
  client fills them) or `{"gap": {"sc", "filler"}}` (the front end's zeros, already in the stream). `seconds` is
  at most 3600.
- A half-duplex front end can't hear its own `tx`: the receiver stops while it transmits. Use a second board.
- The source renders at the platform's rate for it (`sig_rate`): on rdofradio about 48 kS/s (the stream's rate
  divided, 864 kS/s by 18), each sample spread over the stream's linearly (`rdof_sdrmodem_signal_rate`); a 1 kHz
  tone's deviation is then within 5.4 Hz of the curve. Rendered at 864 kS/s it took most of a Pluto core, and the
  receiver lost samples. A PlutoSDR hears its own `tx` (full duplex): `rx` on the same frequency works there.

FM `tx` fields: `dev_hz` (2500: the program at level 1), `ctcss` (Hz) or `dcs` (`"023N"`, `"023I"`: octal, normal
or inverted), `sub_dev_hz` (0.2 of `dev_hz`), `sub_end_s` (the CTCSS reverse burst, 0.15; the DCS turn-off code,
0.18), `preemph_us`, `lead_s` and `tail_s` (carrier and subaudio before and after the program, 0.25), `repeat`,
`seed` (noise), `pcm` (`{"rate", "samples"}`: the input), and `program`, a list of segments, each with `kind`, `s`
(seconds) and `level` (0-1 of `dev_hz`):

| kind | fields |
|---|---|
| `tone` | `hz`: a number, or a list (up to 4 tones summed) |
| `sweep` | `hz`: `[from, to]`, `log` |
| `dtmf` | `digits`, `s` a digit (0.1), `gap_s` (0.05) |
| `fsk` | `text` or `hex`, `bits` (all), `baud` (1200), `hz` `[mark, space]` (Bell 202's 1200, 2200), `nrzi`, `lsb`: AFSK, audio on the FM |
| `noise`, `silence` | |
| `pcm` | where the input goes (with `pcm` and no program, the program is the input) |

`done` adds `dev_hz`, `peak_hz`, `seconds`, `ctcss` or `dcs` with `sub_dev_hz`, `pcm_samples` and `underruns`. On
the W12 the LR2021 sends deviation as int8 samples at 32 Hz a count: a source that peaks over 4064 Hz is refused.

FM `rx` fields: `streams` (`"audio"`, `"disc"` or both: the filtered audio, about 16 kS/s, 300 Hz to 3 kHz with the
CTCSS tone notched; the raw discriminator at the channel's rate), `audio_full_hz` (5000), `block_s` (0.1, a record
each), `deemph_us` (0; 750 for TIA-603), `nominal_dev_hz` (for ratios), `spectrum_hz` (0: no spectrum bins in the
summary). A record is `{"block": {"t", "carrier", "power_dbfs", "env_min", "env_max", "cfo_hz", "dev_pos_hz",
"dev_neg_hz", "audio_rms_hz", "sub_rms_hz"}}`. The summary: `carrier_s`, `power_dbfs` (mean, min, max),
`noise_dbfs`, `envelope` (mean, `am_pct`), `cfo_hz` (mean, min, max), `deviation` (`pos_hz`, `neg_hz`, `rms_hz`),
`audio` (`rms_hz`, peaks, `rms_db`), `tone` (the strongest: `hz`, `dev_hz`, `sinad_db`, `thd_pct`), `peaks`, and
`sub`: `{"kind": "ctcss", "hz", "nominal_hz", "dev_hz", "reverse_burst_t"}`, `{"kind": "dcs", "code", "aliases",
"words", "dev_hz", "turnoff"}` or `{"kind": "none"}`. The CTCSS tone is the standard tone most quarter-second windows
were nearest; `hz` and `dev_hz` are those windows' mean only, so a turn-off tone after it (55 Hz on some radios) or a
window across the change doesn't pull them.

With `"dtmf": true` FM `rx` decodes DTMF digits in the filtered audio, as the bench analyzer does: 40 ms Hann
windows every 5 ms, the eight Q.23 tones by Goertzel; a window holds a digit when its row and column tones stand 10
dB over the other six and hold 60 % of its power, and a run of 30 ms or more is a digit. Each digit is a record as
it ends, `{"dtmf": {"digit", "t0", "t1", "s", "dev_hz", "twist_db"}}`: its span in s from the stream's start (the
blocks' `t`), its peak deviation and its twist (the column tone over the row's, dB, on the carrier: with no
`deemph_us`), each the median over the run. The summary adds `dtmf`: `{"digits": "159#", "runs": [...]}` (64 digits
at most). About 1 M multiplies a second at the audio's rate.

With `"tones": true` FM `rx` times steady tones (tone bursts, roger beeps, CW) in the filtered audio, 300-3000 Hz: 20 ms
Hann windows every 5 ms at 8-16 kS/s, a window holding a tone when one peak has half the band's energy. Windows of one
pitch (within 40 Hz) are a piece, its edges where the peak's power crosses a quarter of the piece's median. Pieces of one
pitch less than 50 ms apart are one run; a piece of another pitch ends it if it lasts 15 ms or more. A run of 40 ms or
more is a record as it ends, `{"tone_run": {"hz", "t0", "t1", "s", "dev_hz"}}`: its pitch, its span in s from the stream's
start (the blocks' `t`, as a digit's) and its peak deviation. The summary adds `tones`: `{"count": N, "runs": [...]}`
(about 60 runs at most; `count` is all of them). Speech can make runs too. Float only.

With `"aprs": true` FM `rx` decodes Bell 202 AFSK (1200 baud, mark 1200 Hz, space 2200 Hz) and AX.25 frames in the
filtered audio, as the bench analyzer does: each tone mixed to 0 Hz and averaged over a bit, the decision mark less g
times space for seven weights g (for twist), each with its bit clock, NRZI, HDLC flags and unstuffing, the X.25 FCS;
a frame two weights decode counts once. Each frame with a good FCS is a record, `{"aprs": {"tnc2", "src", "dst",
"digis", "control", "pid", "info", "fcs_ok", "t0", "t1", "txdelay_flags", "txdelay_ms", "decision_gain",
"peak_dev_hz", "max_dev_hz", "rms_dev_hz", "mark_dev_hz", "space_dev_hz", "twist_db", "mark_hz", "space_hz"}}`:
`t0`, `t1` its first and last bit (s, as the blocks' `t`), the flags back to back before it, the deviation over the
flags and the frame (peak the 99.9th percentile of the audio), each tone's level (the median at the centers of bits
inside a run of one tone) and frequency (its phase's drift along those runs), the twist mark over space (dB, on the
carrier: with no `deemph_us`). The summary adds `aprs`: `{"frames", "fcs_bad"}` (`fcs_bad`: frames between flags,
whole octets, whose FCS failed, counted by the weight-1 decoder only). About 300 KB of buffers (4 s of audio and
each decoder's bits).

AM is double sideband with a full carrier (airband voice; ACARS's MSK is an `fsk` segment, `baud` 2400, `hz` [1200,
2400]). The source renders complex baseband, so a front end that sends frequency must also take polar I/Q
(`rdof_fe_caps_t.tx_polar`, the W12: lr2021-iq.md R3.22-R3.27), else `tx` is refused; an I/Q front end (a PlutoSDR)
takes it as it takes DMR.

AM `tx` fields: `depth` (0.3: the modulation at the program's level 1, so 1 kHz at 30%, the receiver sensitivity
test's signal), `offset_hz` (0: the carrier from `freq`, -10000 to 10000, for an offset-carrier test), `lead_s` and
`tail_s` (bare carrier before and after the program, 0.25), `repeat`, `seed`, `pcm` and `program`, FM's segments with
`level` 0-1 of `depth`. The envelope is `(1 + depth a) / (1 + depth)`, `a` the program clamped to ±1 (never past
100%): the peak at the transmitter's full power, the carrier `20 log10(1 + depth)` dB under it. With no program it
sends the lead and tail alone, half a second of carrier. `done` adds `depth`, `carrier_db` (the carrier against the
peak), `offset_hz`, `seconds`, `pcm_samples` and `underruns`.

AM `rx` fields: `streams` (`"audio"`, `"env"` or both: the audio, about 16 kS/s, band-passed `audio_lo_hz` to
`audio_hi_hz` (300 and 3000; 50 to 6000), in units of modulation, 1.0 being 100% and `audio_full` (1.0) int16's full
scale; the raw envelope `|x|` at the channel's rate, full scale 1.0), `block_s` (0.1), `nominal_depth` (for
`rms_db`), `spectrum_hz`. The modulation is the envelope over the carrier's level less 1, the level tracked over 0.2
s once the first 30 ms of carrier have set it; the first 60 ms after a key-up aren't measured. A record is
`{"block": {"t", "carrier", "power_dbfs", "carrier_dbfs", "cfo_hz", "depth_pos_pct", "depth_neg_pct",
"audio_rms_pct", "fm_rms_hz"}}`. The summary: `carrier_s`, `power_dbfs` (mean, min, max), `noise_dbfs`,
`carrier_dbfs` (the carrier unmodulated), `cfo_hz` (mean, min, max), `depth` (`pos_pct`, `neg_pct`, `rms_pct`: the
5 kHz band's peaks each way and rms), `audio` (`rms_pct`, `pos_pct`, `neg_pct`, `rms_db`, `tone`: `{"hz",
"depth_pct", "sinad_db", "thd_pct"}` or null, `peaks`: `[{"hz", "depth_pct"}]`), `fm` (`rms_hz`, `pos_hz`, `neg_hz`:
the residual FM on the carrier, a transmitter's AM-to-PM) and `spectrum` when asked. Lead and tail carrier count in
the averages (the rms and the tone's depth and SINAD): for a tone's figures send `lead_s` and `tail_s` 0, or read the
blocks.

### 14.7 Frequency correction

The bench's measurements find radios whose frequency is off, so the gateway takes its own crystal's error out of
everything it reads and sends, to a fraction of a ppm, not to the chip's whole-ppm step. `info`'s window says how:

| field | meaning |
|---|---|
| `ppm` | the crystal's error the gateway uses (positive: fast) |
| `ppm_source` | where it came from: `clock` (the sample clock timed against GNSS or NTP; the best), `afc` (the AFC on received traffic, only with no NTP or GNSS reference: the radios' own errors are in it), `table` (the board's temperature table), `kept` (saved by an earlier run), `setting` (`device.ppm`, rdofradio `--ppm`); absent or `none`: not known |
| `lo_ppm` | what's left of it in the receiver's LO once the chip's correction (whole ppm on the LR2021 and the RTL) took its share |
| `fs_ppm` | what's left in the sample clock: the LR2021 doesn't correct its sample rate (all of `ppm`); an RTL does, to 1/16.8 ppm |
| `ppm_tol` | how far `ppm` may be off, ppm; `null`: not known |
| `ppm_ref` | what it was timed against: netclock's source for `clock` (`pps`, `gnss`, `ntp`, `browser`), `radios` for `afc`; `null`: none |
| `ppm_trusted` | `ppm` is the sample clock's, timed long enough to be used as measured |
| `ppm_held` | held for a measurement: changes wait until it ends (below) |
| `ppm_settled` | the board has warmed: the die temperature its crystal follows has settled. rdofradio: always false (not known) |

While any `capture`, `trigger`, `track`, `rx`, `bursts`, `listen`, `tx` or `run` is open, a board holds its
correction where it is: no new estimate (the clock fit, the table) and no chip move, so a frequency read over
seconds shows no step that isn't the signal's. The clock fit goes on; what it says applies when the last one ends.
A hold lapses after 2 minutes of asking (a stream left open doesn't keep the correction stale for good). rdofradio's
correction is a setting and never moves.

Each report that gives a frequency gives the tolerance at it, `ppm_tol` times the frequency: `freq_tol_hz` in the
`rx`, `trigger` and `capture` headers and `tx`'s `done`, `cfo_tol_hz` on each burst and `cfo_hz.tol` in the bursts'
summary (`null`: not known). It is the gateway's crystal only: a measurement's own noise (a burst's cfo scatters by
tens of Hz) comes on top, and the transmitter's error is what's being measured. A board's `ppm_tol`:

| `ppm_source` | `ppm_tol` |
|---|---|
| `clock`, the fit trusted | twice the fit's standard error (the board's own clock rate's error in it: a PPS's is hundredths of a ppm, NTP's can be tenths) plus what the sample rate moved by over the last 10 minutes (a crystal still warming) |
| `clock`, held while it times afresh | 0.5, or twice the new fit's error if more |
| `afc` | 1.0: the channels agree within it; the radios' own errors are in it. Only on a board with no time reference: with NTP or GNSS the crystal is learned from the sample clock alone |
| `table` | 0.5 |
| `kept` | 1.0 |
| `setting` | not known |

rdofradio's is `--ppm-tol` (not known without it). Its netclock disciplines its time from NTP, not the device's
crystal: the frequency is only as good as `--ppm`.

A carrier truly at f shows at `(f - center (1 + lo_ppm 1e-6)) / (1 + fs_ppm 1e-6)` Hz from the center in the
samples (`rdof_ppm_seen_hz`). What each op does with it:

- `bursts`, `listen`, the HTTP bursts stream: corrected. Each receiver channel is mixed from where a carrier on its
  frequency shows, and `cfo_hz` is the burst's carrier from its true frequency (the AFC's correction less the
  crystal's share), so a radio 150 Hz high says +150 at any crystal error.
- `trigger` and `rx`: corrected the same way (`corr_hz`, `ppm` and `ppm_source` in their headers).
- `track`: corrected, and if the correction changes during the keying, each line follows it (section 14.8).
- `capture`: not corrected. It sends the receiver's samples as they are; the header's `lo_ppm` and `fs_ppm` are
  what's left in them, for the client to take out.
- `tx`, `run`, `bench_repeater` on a W12: the LR2021's carrier is set for the fractional `ppm`. On rdofradio the
  modulator's carrier (DMR and the signal modes) is offset by `f ppm / (1 + ppm)` in the samples.
- rdofradio doesn't measure its crystal: give it `--ppm` (from a reference signal, or the PlutoSDR's measured XO).
  Without it `ppm_source` is absent and nothing is corrected. The AD9361's own `xo_correction` reaches only about
  0.5 ppm, so rdofradio corrects in its samples.
- MMDVM mode (an MMDVM modem as the station): the modem's hardware sets its frequencies; the gateway corrects
  nothing there. Set its offsets (`--rx-offset-hz`, `--tx-offset-hz`) from a measurement.
- The board's receive LO keeps the whole-ppm step; the channels take the fraction (and the sample clock's share),
  so a wideband `WS /iq` stream is off by `lo_ppm` and `fs_ppm` as a capture is.

A source of `afc` means the correction came from radios, so a radio's error measured against it is relative to the
others it heard; `clock` is absolute. `rfgbench.py info` shows the correction, and the client warns on stderr
when the gateway's crystal error isn't known.

### 14.8 Track: a keying's carrier over time

`track` follows a transmitter's carrier through a keying, so a radio's drift (a key-up's settling, a crystal
warming) can be told from the gateway's own. It cuts the channel and arms as `trigger` does (bw 25 kHz by default),
starts at the rise and ends `post_s` (0.5 s, longer than a DMR slot's gap) after the last 1 ms block over half the
threshold, at most `max_s` (30) after the rise. It sends no I/Q: each `interval_ms` (100; 20-2000) of the keying is
one line, from each output's instantaneous frequency (its phase step from the one before) in the blocks over half
the threshold. A line needs `line_ms` on the air: by default a fifth of the interval for `fm` (an interval with
less has no line) and 80 ms for `4fsk` (about 380 symbols), where an interval short of it pools into the next
until it has it, for up to 1 s. One TDMA slot is on the air 27.5 ms in 60, so its 100 ms intervals hold 30-60 ms
each, too few symbols to place four levels: pooled, a line spans about 200 ms.

- `fm` (the default): the mean frequency. The modulation averages out over an interval; a voice's or a tone's
  doesn't always quite, so an interval of a few hundred ms is steadier.
- `4fsk`: the center of the four levels, from a k-means on the interval's histogram (25 Hz bins, +-8 kHz) started from
  its 1st and 99th percentiles, the later rounds using only the outputs near a level (a symbol's transition pulls
  it). The four are weighted alike, so a burst with more of one symbol isn't moved; `dev_hz` is half the outer
  pair's spacing (DMR: 1944 Hz).

A line: `t` (its first interval's start, s from the rise), `span_ms` (the intervals it covers), `hz` (the carrier from `freq`, the gateway's crystal taken out),
`dev_hz` and `symbols` (4fsk: the on-air time in symbols at 4800/s), `level_db`, `ms` (how much of its span was on
the air), `ppm`, `ppm_source`, `trusted` (the
correction is the sample clock's, trusted as measured), `tol_hz` (`ppm_tol` at `freq`), `die_c` (the die
temperature the board's crystal follows; `null` where not known), `transient` (`t` under 0.3 s: the key-up's
settling, left out of the summary's figures).

The channel is mixed with the correction as it was at the start; each 1 ms block is corrected again for the
correction its samples were written under (the engine reads the history behind the board's state, by up to its 2 s
on a busy W12). The engine notes the window at each tick; a block written between the tick before a change and
the one that saw it could be under either and is left out (about 10 ms a change). A chip move (a real step in the
samples, the window's `lo_ppm` with it) cancels out; a new estimate of the crystal (the clock fit, the table) moves
`hz` by the estimate's change, and `ppm` and the summary's `ppm_moved` say so.

The summary: `freq`, `mode`, `bw`, `interval_ms`, `t0`, `trigger_time`, `seconds`, `lines`, `level_db`,
`floor_db`, `snr_db`; `settled_hz` (the mean of the last 2 s), `start_hz` (0.3 to 1.3 s), `drift_hz` (the mean of
the second before `drift_s` (6) less `start_hz`; `null` for a shorter keying), `mean_hz`, `std_hz`, `min_hz`,
`max_hz` (the lines after the transient, weighted by `ms`), `ppm_moved` (the correction's range over the lines),
`trusted_throughout`, `settled_throughout` (`ppm_settled` on every line), `gap_ms` (samples the history overwrote before the engine got to them, or the front end's
filler), `warnings`, and the correction's fields with `freq_tol_hz` at the end (section 14.7). `std_hz` is the
radio's wander plus the measurement's noise. `warnings` lists what makes the figures less than the gateway's best,
empty when nothing does: `not_trusted` (a line's correction wasn't the trusted sample clock: the table, the AFC, a
fit timing afresh), `not_settled` (the board was still warming, its crystal moving), `ppm_moved` (the correction
changed mid-keying: a hold that lapsed), `gaps` (samples lost). The track runs either way.

`rfgbench.py track --freq MHz [--mode 4fsk]` writes the lines (and `{"summary": ...}`) as JSON to `--out` or
stdout, and the summary's figures to stderr, with a warning line for each of `warnings`.

### 14.8.1 What a transmit job measures

A `bench_tx` job's `{"tx_event": frame}` lines say what really happened, one frame format on every front end (the
`/tx` socket gets the same frames). `sent N bursts` in a job's log, `unkeyed`'s `seconds` and `done`'s `bursts` are the
job's own model: bursts handed to the modem, not bursts on the air (`unkeyed` says `"estimated": true`).

| Frame | Fields | Meaning |
|---|---|---|
| `modem` | `t`, `tx`, `space`, `model_left` | the modem's own TX bit changed (GET_STATUS, every 100 ms); its buffer space; the bursts the model says are still to go |
| `modem_flag` | `flag`: `quiet` (`outstanding`), `late` (`late_s`), `held` (`space`, `idle_space`) | the modem stopped transmitting with bursts due; still transmitting 0.5 s past the model's end; stopped with its buffer not empty |
| `tx_check` | `handed`, `timing`, `modem_tx_s`, `quiet`, `late`, `held`; with `timing` `heard`: `compared`, `heard`, `never`, `first_s`, `latest_s`; with `estimate`: `why` | a keying's account, once the modem has said it's done (1.5 s at most) |

`timing` is `heard` when a separate receiver decodes the job's frequency: an MMDVM modem with an SDR channel there
(a `tune` with a tap makes one). Each burst handed over is then matched against the bursts heard, up to 24 bit errors
apart. `first_s` and `latest_s` are seconds from the burst's modeled air time to the receiver's report, so `first_s`
is the path's own delay and a stall shows as a much larger `latest_s`; `never` counts bursts with no match.
`timing` is `estimate` otherwise, with `why`. On a W12 or a Pluto the SDR is the transmitter, and its receiver isn't a
separate witness: a W12 can't hear while it transmits and a Pluto's own leakage doesn't show what reached the air, so
those figures stay estimates, and `modem`'s `tx` is the SDR modem's own state.

### 14.8.2 A transmitter's PA ramps, from a capture

`tools/bench/burstramp.py FILE... [--bursts] [--json OUT]` measures each burst's power ramp in a `capture` or `capture-trigger`
file (with its `.json`). It needs numpy and runs where the file is. A `bursts` stream with `"ramp": true` (14.2) measures the same
figures on the gateway, burst by burst, with no I/Q streamed: edges agree with this tool to about 0.01 ms and levels to a few
hundredths of a dB on the same capture, `dev3_hz` (a least-squares fit there) to a few percent. A rise time alone misses a burst
that starts 8 dB over its settled level; these figures catch it.

Every figure is against the burst's own settled level (0 dB) and payload. t = 0 is the leading edge of the first symbol, and for
the falling edge the trailing edge of the last. Negative is before. Symbols come from the sync; voice B-F, with no sync, from the
30 ms grid of the nearest burst that has one. Rising and falling figures are given only where the burst opens or closes a
key-up, and only for levels at least 6 dB over the floor; otherwise `null`.

| Field | |
|---|---|
| `t_s`, `t`, `kind`, `sync`, `corr`, `anchored`, `first` | the payload's start (from the capture's start, UTC); `VOICE_A`, `VOICE_B_F` or the data type; the sync, or `GRID`; `first`: no burst in the 90 ms before |
| `settled_db`, `ripple_db` | the median level from 4 ms in to 4 ms before the end; the 0.1 ms envelope's spread there |
| `rise_ms["-40"…"-1"]`, `rise_30_3_ms` | the last upward crossing of each level before the 0.1 ms envelope holds there for 1 ms (a sag counts, a dip later doesn't), searched 14 ms before to 10 ms after t = 0 |
| `overshoot_db`, `overshoot_t_ms` | the 0.1 ms envelope's peak in the 4 ms from the -3 dB point, and when |
| `settle_1db_ms`, `settle_0p5db_ms` | when the envelope comes within the band and stays for 2 ms; `null` where `ripple_db` is over 0.4 or 0.2, as the band is then no bigger than the signal's own ripple |
| `sym1_db`, `err5_*_db`, `err12_*_db` | the level at the first symbol's center (half a symbol's mean, no lower than the floor); the mean and absolute mean error over the first 5 (1 ms) and 12 symbols |
| `fall_ms["-3","-10","-30"]`, `fall_3_30_ms` | the first downward crossings after t = 0 of the falling edge |
| `pre_db`, `empty_slot_db` | dB over the quiet floor in 0.5 ms bins: the 5 ms before t = 0, and 5 ms around the middle of the slot before (`null` if a burst is there) |
| `f_first1ms_hz`, `f_next2ms_hz`, `*_unreliable` | the mean residual frequency from the sync's, over the first 5 symbols with carrier (over -30 dB) and the next 10; unreliable when a symbol there is more than 20 dB down |

`--json` writes a line a burst and a last `{"summary": ...}`: the rate, `edge_res_us`, floor, SNR, the capture's own `clip_pct`,
`overflows` and `gap_samples`, and in `by_kind`, per kind, each figure's `median`, `min`, `max` and `n` (`first:KIND` for a
transmission's first burst, which has no burst before it).

`edge_res_us` is what the capture can claim on an edge: the larger of a sample and 0.7 / (one-sided bandwidth), about 112 us for
a 12.5 kHz channel at 48 kS/s. A rise under that is a limit, not a measurement. A wideband capture (over 150 kS/s) is cut to
120 kHz here, so it can claim about 12 us, if it doesn't clip: clipping makes the overshoot read low, and the text output warns.

### 14.9 What each front end does

`info` says what this bench can do: `stations[0].kind` is the transmitter, `signal_modes` the signal modes, `jobs`
each job's reason when it can't run. A client skips on those; a refused job's error gives the same reason, with the code `unsupported`.

| | W12 (LR2021) | PlutoSDR (AD9361) | MMDVM modem + RTL-SDR | MMDVM modem alone |
|---|---|---|---|---|
| `info`, `status`, sessions, `claim`, `hold` | yes | yes | yes | yes |
| `bursts`, `listen` (with the envelope option) | yes, on the LR2021 stream | yes | yes, on the SDR | no: no receiver |
| `capture`, `trigger`, `track`: cs8 wideband | yes, the LR2021's rate | yes | yes, cs8 is the RTL's own 8 bits | no |
| `capture`, `trigger`: channel taps, cs16 | yes | yes | yes | no |
| `rx`: FM, AM | yes | yes | yes, on the SDR | no |
| `tune`: center, taps | yes | yes | yes | no |
| `tune`: gain | yes | yes | yes | no |
| `run` `bench_tx`: `voice`, `csbk`, `data` | yes | yes | yes, the modem in simplex | yes, on the modem's own band (a hotspot board: 70 cm) |
| `run` `bench_tx`: `outbound` | yes, the slot clock its own | yes, its own | duplex modem only: the modem's idle downlink and CACH; its slot times from a burst the SDR hears | no: the slot clock is counted in an SDR's samples |
| `run` `bench_repeater` | no (`unsupported`) | yes | duplex modem only: the modem's own receiver hears the input | duplex modem only, as beside an SDR |
| `mssim` (confirmed-data responder) | yes | yes | yes: timing the serial port's and a key-up's, learned from what's heard back | no: it hears through an SDR's channel |
| `tx`: FM, AM (signal modes) | yes | yes | no: the MMDVM serial protocol carries DMR bursts only, and an RTL-SDR doesn't transmit | no |

What the last two columns can't do, and why:

- `tx` in FM or AM: `info.signal_modes.tx` is empty and `jobs.signal_tx` says why. The modem takes DMR bursts over its
  serial port: there's no way to give it audio or I/Q. The SDR beside it only receives.
- `outbound` and `bench_repeater` on a simplex hotspot (one transceiver): `jobs` names them. A repeater or an
  outbound needs the modem to send a CACH and hear another frequency while it transmits. A duplex board (two
  transceivers) runs them.

A running duplex modem and the SDR are independent. Both of the modem's frequencies are channels on the SDR
(`modem-rx`, `modem-tx`) when one window holds them, about 2 MHz apart on an RTL-SDR, so `bursts`, `capture` (cs8
wideband or a channel's cs16), `trigger` and `rx` work on either side while the modem runs. A wider split can't be
in one window: the SDR keeps the modem's RX, and a request for the TX side tunes the receiver for itself.

The third column needs only the modem and an RTL-SDR on a Linux box; [the reference](../reference.md) has the setup.
The last needs only the modem: its jobs transmit, and nothing on the box hears them (another receiver does). A hotspot board tunes its own band only: a job outside
it ends with the modem refusing `SET_FREQ`, whatever `tx_bands` allows.
