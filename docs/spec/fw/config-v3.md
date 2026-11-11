# Settings, version 3

Covers: the settings, the one format the firmware and rdofradio take: devices, frequencies, apps, networks and the
policies that say which calls move between a talk path and a network; every key with its type, default and rule;
how a box runs a file (§12); its limits and checks, and the schema (§13); where the board keeps it (§16).
Code: the parser and the resolved view `common/src/site.c` ([site.h](../../../common/include/rdof/site.h)); what
runs, the devices' own keys and the checks `common/src/siteplan.c`
([siteplan.h](../../../common/include/rdof/siteplan.h)); each part's settings from the plan: `common/src/config.c`
(a repeater or transceiver), `sdr/src/config.c` (the SDR), `common/src/ipgwcfg.c` (the IP gateway),
`common/src/benchcfg.c` (the bench), `common/src/oled_sdr.c` (the screen); ID sets `idset.c`, the router `router.c`
and, on a running box, `siteroute.c`. rdofradio (`sdr/cli/rdofradio_cfg.c`) and the firmware
(`esp_idf/main/settings.c`, `site_run.c`) run it. The schema is `config/rdof-config.schema.json`
(`tools/config_check.py`).
Related: [rxtx-combinations.md](rxtx-combinations.md) (devices and resources), [transmitters.md](transmitters.md),
[hblink.md](hblink.md), [mqtt.md](mqtt.md), [ipgw.md](ipgw.md), [../repeater.md](../repeater.md) (split,
`slot_timing`), [../slot-timing.md](../slot-timing.md), [../bench-api.md](../bench-api.md),
[interfaces-http.md](interfaces-http.md) (`/api/config`, `/api/resolved`).

## 1. Why a version 3

Version 2 named roles: a roamer, a repeater, a monitor, an SFR, a DCDM. Each role mixed three things: the hardware
it used, the frequencies it worked on, and where its calls went. That made simple sites wordy (a monitor role to
receive, a roamer to transmit simplex on the same frequencies) and left no place for per-path rules (which
talkgroups, which private calls, which apps).

Version 3 splits them:

| Section | What it is | Says nothing about |
|---|---|---|
| `devices` | hardware and what it can do | frequencies' use, calls |
| `frequencies` | RF: frequency, color code, mode, whether anything may transmit there, and its call policy | hardware |
| `apps` | things that take frequencies (a repeater) or answer calls (parrot, IP gateway) | hardware |
| `networks` | connections that carry calls: HBlink, MQTT, later others | RF |
| `policies` | named call policies, shared by several paths | anything else |

Nothing names a role and nothing says what runs: what runs follows from the sections (§12). Which device serves
which frequency follows from each device's `hears` and `transmits`; a file only pins a device (a frequency's
`devices`) when it must.

## 2. Document

```jsonc
{
  "version": 3,
  "name": "site1",
  "id": 9800100,
  "defaults": {"cc": 2, "ts": 1},
  "devices": [ ... ],
  "frequencies": [ ... ],
  "apps": [ ... ],
  "networks": [ ... ],
  "policies": [ ... ]
}
```

- One JSON object. JSONC is taken: `//` and `/* */` comments, trailing commas.
- At most 2000 JSON nodes and 16 KB of strings (`RDOF_SITE_MAX_NODES`, `RDOF_SITE_STR_BUF`). Larger is refused.
- A key a place doesn't take is refused (`devices.w12.rx.sample_rate: not a key here`). Errors name the place:
  a list entry by its name once it has one (`devices.w12`, `frequencies.438.0625`, `apps.R1`), else by index
  (`frequencies[3]`).
- Frequencies: a number below 1e5 is MHz, else Hz (`438.1` and `438100000` are the same). The bench's `freqs_hz`
  and `bands_hz` are whole Hz.
- A string is taken as written: `${VAR}` isn't read from the environment.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `version` | 3 | | required; anything else is refused (`version: 3`) |
| `name` | str | `"radiofoundry"` | the box's name: the Wi-Fi host name, the MQTT and status name |
| `id` | int, 1..16777215 | | the box's DMR ID: its HBlink peer ID, and the base for per-path peer IDs (§7.1); required with an `hblink` network (`rdof_plan_radio` refuses a unit that transmits without it there); without one, a unit's peer ID is 0 |
| `callsign` | str | a unit's name, first 8 characters; the SDR: `SDR` | the callsign on every HBlink peer the box runs (with `per_path`, each peer's own name, §7.1) |
| `pc_hold_s` | number, 1..86400 | 900 | how long a radio heard on a path takes the private calls to it (§9.3) |
| `defaults` | object | `{}` | frequency keys (§5: `cc`, `mode`, `ts`, `tx`, `power_dbm`, `policy` and the policy keys) for every frequency that doesn't set them |
| `devices` | list | `[]` | §4 |
| `frequencies` | list | `[]` | §5 |
| `apps` | list | `[]` | §6 |
| `networks` | list | `[]` | §7 |
| `policies` | list | `[]` | §8 |
| `location` | object | none | §2.2 |
| `oled` | object, `true`, `false` or `null` | §2.2 | the screen |
| `slot_timing` | object | `free` | the UTC slot grid ([../slot-timing.md](../slot-timing.md)); inherited by a repeater on a split or the SDR |
| `wifi`, `ntp` | object | | §2.2; the board keeps them apart (§16) |
| `usb_hub` | bool | false | a hub on the board's USB port: every USB device at once (a build with hub support: `16mb`, `w12`, the `hub` overlay) |
| `selftest_load` | object | `{"channels": 8, "rates": [288000]}` | the SDR self-test's load runs: `channels` 1..16, `rates` up to 4 sample rates |
| `bench` | object | none | the bench API's devices and TX policy (§4.6) |

### 2.1 Lists and names

Every section is a list. An entry's `name` is optional and defaults as below. Names are unique within a section,
under 40 characters, without `/`, `"` or `\`. A device's name is letters, digits, `-`, `_` and `.`.

| Section | Default name | A name is required when |
|---|---|---|
| `frequencies` | the frequency in MHz (`"438.4875"`) | the same frequency appears twice |
| `devices` | the kind and its index among devices of that kind (`sdr0`, `modem1`) | never |
| `networks` | the type (`hblink`, `mqtt`) | two networks of one type |
| `apps` | the type (`parrot`, `repeater`) | two apps of one type |
| `policies` | | always |

A reference to a frequency matches its name first, then its frequency as a number: `439`, `"439"` and `"439.0"` all
find the entry at 439 MHz. A name that is another entry's frequency is refused.

A bare number in `frequencies` is an entry with only `freq`: `438.1125` is `{"freq": 438.1125}`.

What runs takes its name from the entries (§12): a repeater app's name, an SFR or DCDM frequency's (with anything but
letters, digits, `-`, `_` and `.` turned into `_`), a device's, `<device>-rx`. Two that come out the same are
refused ("rename one").

### 2.2 The board's sections

`location`:

| Key | Type | Default | Rule |
|---|---|---|---|
| `latitude` (or `lat`) | number | | required with `location`; −90..90 |
| `longitude` (or `lon`) | number | | required; −180..180 |
| `height` | number | 0 | meters, rounded, clamped 0..999 |

The location goes in every HBlink peer's DMRC and the status.

`oled`: the screen (the SDR's when one runs, else the first radio's). Absent: on in a build with
`CONFIG_RDOF_OLED_BUILTIN` (`w12`), else off. `true` or an object: on. `false` or `null`: off. Read by
`oled_sdr.c:rdof_oled_settings_parse` whatever runs ([device.md](device.md) §2).

| Key | Type | Default | Rule |
|---|---|---|---|
| `address` | int | 0x3C | 0x03..0x77; also `i2c.address` |
| `sh1106` | bool | false | an SH1106 panel (else SSD1306); also `i2c.sh1106` |
| `rotate180` | bool | false | also `i2c.rotate180` |
| `refresh_s` | number | 0.5 | |
| `sda`, `scl` | int | 8, 9 (`CONFIG_RDOF_OLED_SDA` / `_SCL`; 17, 18 in `w12`) | GPIOs 0..48, not the same |

`slot_timing`: `mode` (`free` or `utc`), `offset_us`, `max_err_us` (a number, or `{pps, ntp}`), `on_loss`,
`holdover_s`. Meanings and defaults: [../slot-timing.md](../slot-timing.md). The top level's reaches the SDR and a
duplex repeater on a split or the SDR; a repeater app's own `slot_timing` wins over it. `utc` is refused where the
radio keeps its own slot clock: a repeater on one modem, an SFR or DCDM frequency.

`wifi`: `{"networks": [{"ssid", "password", "priority", "hidden"}]}`, up to 8 networks (wifilist's list,
[components/wifilist/README.md](../../../components/wifilist/README.md)).

`ntp`: the servers as a string, or `{"servers", "poll_s", "clock"}`. `servers` default `pool.ntp.org`, `poll_s` 300
(16..3600). `clock` (`netclock` or `os`) is the host tools': with servers they run netclock unless it's `os`. The
board always runs netclock ([components/netclock/README.md](../../../components/netclock/README.md)).

## 3. ID sets

Talkgroups and private-call IDs use one syntax everywhere:

| Form | Meaning |
|---|---|
| `202` | one ID |
| `"3100-3199"` | a range, both ends included |
| `"*"` | any ID |
| `"!350"`, `"!4000-4099"` | not these (in a list) |
| `[202, "300-399", "!350"]` | a list: the union of the plain items, less the `!` items |
| `"202,300-399,!350"` | the same list as one string |
| `false` | none |

A list of only `!` items starts from `*`: `"!9"` is every ID but 9. Order doesn't matter. IDs are 1..16777215. The
resolved view (§11) shows each set expanded to ranges.

## 4. Devices

A device is one piece of hardware the box opens: an SDR, a modem (anything that speaks MMDVM on a serial port), or a
radio (a RadioFoundry radio on its debug link, for the bench). Every device takes:

| Key | Type | Default | Meaning |
|---|---|---|---|
| `kind` | `sdr` \| `modem` \| `radio` | | required |
| `name` | str | §2.1 | |
| `hears` | object or `false` | an SDR: every frequency in its window. A modem: the first frequency it transmits on, as its idle frequency; when that is a repeater's `out`, the repeater's `in` (below) | where a one-frequency device listens (§4.4); `false`: it never receives |
| `transmits` | list or `false` | every frequency with `tx` true that it can reach, except a repeater's input | the frequencies it may transmit on (names or frequencies); `false`: it never transmits |
| the talker keys | | §4.5 | how a device that transmits on several frequencies waits and answers |

Every power in the settings, the API, the logs and the page is dBm at the antenna connector.

- A frequency's `devices` pins it: only those devices transmit there, and only they hear it.
- An SDR has no `hears` object: it hears every frequency inside its receive window, and the rate and center are
  chosen to cover them. `"hears": false` turns its receiver off. `hears.scan` on an SDR is for later (§14).
- A modem that doesn't transmit needs `hears` (its frequency).
- A modem with no `hears` that sends a repeater's `out` hears its `in`: one duplex modem is the whole repeater,
  with nothing more to say. A `radio_hotspot` doesn't (it is half duplex: a split's transmit half), and a modem of
  type `auto` only when no other device hears `in` (else it is a split's transmit half too).
- A radio takes neither `hears` nor `transmits`: it is a bench device only.
- An SDR transmits when its driver can (`lr2021`, `sx1255`, `soapy`, `iio`, `sim`) or it has `tx`. `transmits` on an
  SDR that can't (`rtlsdr`) is refused.
- A modem or a radio serves one thing: two units that both need it are refused ("a modem serves one"; pin a
  frequency's devices, or set `transmits`). A modem in `bench.devices` with neither `hears` nor `transmits` is the
  bench's alone: it makes no unit; a frequency whose `devices` pins it makes one, and that is refused.

What version 2 called a roamer is a modem with `hears` and `transmits`. HBlink4 roaming (DMRT, the master moving the
device to a frequency) is a network option (§7.1), not a role.

### 4.1 `sdr`

| Key | Type | Default | Rule / meaning |
|---|---|---|---|
| `driver` | `rtlsdr` \| `lr2021` \| `sx1255` \| `sim` \| `soapy` \| `iio` | `rtlsdr` | the front end; the build must have it. `soapy` and `iio` are the host tool's (refused on a board) |
| `match` | `{serial, usb_port, uri, args, index}` | | which device, when several match. Taken, not read yet: rdofradio takes its device from the command line (`--soapy`, `--iio`) |
| `rx` | object | | below |
| `tx` | object | | below; its presence makes the SDR a transmitter |
| `bench` | object | | what the bench may transmit on it (§4.6); `tx.bench` is the same (one or the other) |
| `usb` | `{transfers, transfer_kb}` | 8, 8 | an RTL-SDR's USB bulk transfers: how many, and their size in KB |
| `dsp_cores` | 1 \| 2 | 1 | 2 or more runs the demodulators on both cores |
| `spike` | bool | false | the USB throughput test at start |
| `selftest` | bool | false | the CPU self-test at boot ([device.md](device.md)) |
| `sim` | bool | false | forces the `sim` front end |
| `sim_ppm` | number | 0 | the simulated tuner's error, with `sim` |
| `idle_gate` | bool | true | skip demodulating a channel with nothing on it |
| `learn_ppm` | bool | true | learn the tuner's error from what's heard ([sdr.md](sdr.md)) |
| `afc` | bool | true | follow each call's frequency error |
| `rssi_offset` | dB | by driver: `lr2021` 5 − the step's gain (−48.5 at the default step); the others −gain − 25 (gain 30 when auto) | added to the measured level for the reported RSSI |
| `status_seconds` | number | 30 | the SDR's MQTT status period |

`rx`:

| Key | Type | Default | Rule / meaning |
|---|---|---|---|
| `rate` | S/s | 1536000 | a multiple of 48000; at most the driver's (`rtlsdr` 3.2 MS/s; the LR2021 576 kS/s, checked by its front end). An S3 takes 288000 or 240000 for an RTL-SDR |
| `center_hz` | frequency or `"auto"` | auto | the window's center; auto puts it over every frequency the SDR hears. Each frequency must be inside the window (`rate` wide) |
| `gain_db` | dB or `"auto"` | auto; `lr2021` 53.5 (step 13) | `lr2021`: the nearest of its 13 steps' dB (−8 to 53.5), fixed so every burst's level is measured at a known gain; `"auto"` runs the chip's AGC, which can change the gain between slots, so its RSSI is a guess |
| `agc` | bool or `{min, clip, headroom_db, hold_s, relax_s, settle_s}` | off | the gain loop; needs a fixed `gain_db` (the most it may use). Defaults 0, 1e-4, 6, 10, 300, 0.3 |
| `ppm` | number | 0 | the tuner's error, before learning |
| `biastee` | bool | false | |
| `iq_bits` | 8 \| 16 | the front end's choice | LR2021 only: 16 takes int16 samples in the wide mode (about 50 dB more headroom than int8's 6 bits), up to 240000 S/s; else refused |

`tx`:

| Key | Type | Default | Rule / meaning |
|---|---|---|---|
| `carriers` | int, 1..4 | 1 | carriers it sends at once; at most the driver's (1 for every driver for now, §13.1) |
| `power_dbm` | number, −60..40 | 10 | transmit power, dBm at the antenna. A frequency's `power_dbm` is taken under it: the SDR's RF level 255 is this, each level 0.2 dB under it (§4.7) |
| `center_hz` | frequency or `"auto"` | auto | taken, not read yet |
| `bench` | object | | §4.6 |

### 4.2 `modem`

| Key | Type | Default | Rule / meaning |
|---|---|---|---|
| `type` | `auto` \| `radio_hotspot` \| `mmdvm_hs` \| `mmdvm` | `auto` | the modem's firmware. `radio_hotspot`: a radio in hotspot mode (RadioFoundry, OpenGD77); `mmdvm_hs`: an MMDVM_HS board; `mmdvm`: an MMDVM board; `auto`: found from its description |
| `port` | object | | required (§4.3) |
| `needs` | list of `duplex`, `split`, `dcdm`, `sfr` | | what its description must say. Taken; not checked at open yet |
| `mmdvm` | object | | the MMDVM settings, below |
| `tx` | `{min_dbm, max_dbm}` | none | the modem's power at RF level 0 and at 255, dBm at the antenna (§4.7) |
| `bench` | object | | §4.6 |

`mmdvm` (a split repeater takes its TX half's, else its RX half's):

| Key | Type | Default | Rule / meaning |
|---|---|---|---|
| `rx_invert`, `tx_invert`, `ptt_invert` | bool | false | |
| `tx_delay_ms` | int | 100 | |
| `rx_level`, `tx_level`, `cwid_level` | number | 50 | |
| `rx_dc_offset`, `tx_dc_offset`, `dmr_delay` | int | 0 | |
| `rx_offset_hz`, `tx_offset_hz` | int | 0 | |
| `headers`, `terminators` | int | the kind's (hotspot 1 / 1, else 8 / 3) | |
| `tuner` | `"modem"`, `"fixed"` or `{type, freq, settle_ms}` | the kind's | `rigctld` and `command` are refused (a host's) |
| `rssi` | `[[raw, dBm], ...]` | none | up to 16 points |
| `protocol` | `uart` \| `udp` | `uart` | `udp` (with `modem_address`) is refused: not on this board |
| `modem_address` | str | | with `protocol: udp` |

### 4.3 `port`

A modem's or a radio's port: exactly one of `uart`, `usb` or `path`.

| Key | Type | Default | Rule / meaning |
|---|---|---|---|
| `uart` | 1 \| 2 | | one of the board's UARTs (UART0 is the console) |
| `tx`, `rx` | GPIO 0..48 | | required with `uart` |
| `rts`, `cts` | GPIO −1..48 | −1 | none |
| `baud` | int, 1200..6000000 | 115200 | 8N1 |
| `usb` | `auto` \| `cdc-acm` \| `uart` | | a USB device: `auto` (a modem, found by its kind), `cdc-acm`, or `uart` (a USB-serial bridge: FTDI, CP210x, CH34x) |
| `vid`, `pid` | four hex digits, or a number | | a `cdc-acm` bench device needs both; VID 0BDA (the RTL-SDR's) is refused |
| `serial` | str, < 32 | | the USB serial number |
| `usb_port` | str, < 16 | | the hub port path (`"2"`, `"1.3"`) |
| `path` | str | | a serial device (`/dev/ttyACM0`): a host's only |

The board's UART keys aren't taken on a USB port, and the other way round. Two USB devices of one box each need
`serial` or `usb_port`, and they must not match the same device.

### 4.4 `hears`

A modem's: `{"idle": "3"}`, `{"scan": ["3", "4", "6"]}`, or `false`.

| Key | Type | Default | Rule / meaning |
|---|---|---|---|
| `idle` | frequency reference | with `scan`: the first in the list | where it listens between calls |
| `scan` | list of frequency references | | it hops these, one at a time (`CONFIG_RDOF_SCAN`) |
| `dwell_s` | number | 0.45 | with `scan`; above 0: time on each |
| `hang_s` | number | 3.0 | with `scan`; at least 0: time held on a frequency after a call |
| `carrier_s` | number | 0 | with `scan`; 0 (off) or under `dwell_s`: hold on a carrier this long before the sync |

### 4.5 The talker keys

A device that transmits on simplex frequencies (a transceiver, §12) takes these; a device that doesn't is refused
them. An SFR or DCDM frequency takes `prebuffer`, the jitter buffer and the CW ID keys too (§5.1).

| Key | Type | Default | Meaning |
|---|---|---|---|
| `listen_s` | number | 0.3 | listen before transmitting |
| `clear_s` | number | 2.0 | how long the frequency must be clear |
| `idle_after_s` | number | 0 | back to the idle frequency after this long |
| `csbk_preambles` | int | 2 | CSBK preambles before data |
| `answer_s` | number | 0.75 | the answer window after a call heard |
| `answer_max_s` | number | 5.0 | the longest answer window |
| `jitter_buffer` | `"auto"` or ms 0..3000 | `"auto"` | the network jitter buffer: auto sizes the prebuffer per call from the network's lateness, between `jitter_min_ms` and `jitter_max_ms` ([roamer.md](roamer.md), "Jitter buffer"); ms is a fixed size, rounded up to whole 60 ms bursts. Not with `prebuffer` |
| `jitter_min_ms`, `jitter_max_ms` | ms | 180, 480 | auto's bounds: 60 ≤ min ≤ max ≤ 3000, each rounded up to whole bursts |
| `prebuffer` | int | 6 | the older fixed size, in bursts; set, it turns auto off. Not with `jitter_buffer` |
| `cwid` | str, ≤ 40 | `""` | the CW ID; empty: none |
| `cwid_every_s` | number | 900 | |

`jitter_buffer: 300` is a fixed 5 bursts (300 / 60); `310` is 6, since milliseconds round up. `prebuffer: 4` is the
same as `jitter_buffer: 240`.

### 4.6 The bench

`bench` at the top: the bench API ([../bench-api.md](../bench-api.md)).

| Key | Type | Default | Rule / meaning |
|---|---|---|---|
| `devices` | list of device names | `[]` | the modems and radios wired to the bench, up to 4 (`RDOF_BENCH_MAX_RADIOS`). A radio speaks `odr-debug`, a modem `mmdvm`. Each on a UART or a USB device with `cdc-acm` (with `vid` and `pid`) or `uart`, not `auto` or a path; two on one UART are refused; a name of letters, digits, `-` and `_`, under 24 characters. A bench modem serves nothing else: listed here, it needs no `hears` or `transmits` |
| `tx` | object | none: no bench TX | below |

`bench.tx`:

| Key | Type | Default | Rule / meaning |
|---|---|---|---|
| `freqs_hz` | list of whole Hz, 30e6..3e9 | `[]` | where the bench may transmit, up to 8 (with the devices' own) |
| `bands_hz` | list of `[low, high]`, whole Hz, 30e6..3e9, low below high | `[]` | where else it may: any frequency in one, up to 8 |
| `max_key_s` | number, 0..60 | 10 | the longest key-up; 0: no bench TX |
| `max_dbm` | number, −60..60 | | the most a bench job sends at, dBm at the antenna. Not with `rf_level` |
| `rf_level` | int, 0..255 | 0 | without `max_dbm`: the most as an RF level on the transmitter's range (0: its least) |
| `arm_gpio` | GPIO −1..48 | −1 | a jumper to ground that must be in for TX; −1: none |

A device's `bench` (on an SDR, `bench` or `tx.bench`):

| Key | Type | Default | Rule / meaning |
|---|---|---|---|
| `freqs_hz` | list of whole Hz, 30e6..3e9 | `[]` | up to 8; added to `bench.tx.freqs_hz` |
| `max_key_s` | number, 0..60 | `bench.tx`'s | taken, not used yet: the box's `bench.tx.max_key_s` applies |

A bench radio's guard sends at its lowest power step whatever the job asks.

### 4.7 Transmit power

A frequency's `power_dbm` is the power at the antenna connector. A modem's `SET_FREQ` carries an RF level, 0..255;
the transmitter's map turns dBm into it: `level = 255 (dBm − min) / (max − min)`, rounded and held to 0..255.

- A modem or a radio: its `tx.min_dbm` and `tx.max_dbm`, measured at its antenna connector. Without them it sends at
  level 255 (its own power setting), and `power_dbm` caps nothing there.
- An SDR: its `tx.power_dbm` is level 255, and the map spans 51 dB under it (0.2 dB a level). A `power_dbm` over the
  device's is held to it.
- A W12 (the LR2021 with its GC1109 PA) picks the path per transmission: the PA (+12 dB) inside its band
  (`CONFIG_RDOF_LR2021_PA_LO_MHZ`..`_HI_MHZ`) for a request over `CONFIG_RDOF_LR2021_PA_ABOVE_DBM` (−1), else
  the bypass (−22 dB; `_PA_GAIN_DB`, `_BYPASS_GAIN_DB`). The chip goes to the request less the path's gain, −9.5..21 dBm in 0.5 dB steps, a step down when
  between. So the W12 sends −31.5 to −1 dBm through the bypass and up to about +27 through the PA (it saturates
  there). The gains are R3.44's at 920 MHz; 446 and 439 MHz aren't calibrated.

## 5. Frequencies

| Key | Type | Default | Meaning |
|---|---|---|---|
| `freq` | number | | required; MHz below 1e5, else Hz |
| `name` | str | the frequency | §2.1 |
| `cc` | 0..15 | `defaults.cc` | color code; required unless `tx: false` (then any color code is taken) |
| `mode` | `simplex` \| `sfr` \| `dcdm` | `simplex` | what the frequency carries when no app takes it (§6.1). `sfr` and `dcdm` make a unit (§12) |
| `ts` | 1 \| 2 | 1 | a simplex call's slot: plain direct mode carries none on the air, so a call heard here goes to the master, last heard and MQTT as `ts`, and the answer comes back on it |
| `tx` | bool | true | false: nothing ever transmits here; its paths are receive only |
| `power_dbm` | number, −60..60 | the device's | transmit power here, dBm at the antenna (§4.7); a repeater takes its `out`'s. `power` (the older 0..255 level) is refused |
| `devices` | list of device names | any | pin the devices that may serve it: only they transmit and hear it |
| `slots` | object | | per-slot policy, for a frequency heard as two slots: a DCDM frequency, or one with `tx: false` (a monitored repeater output). Refused elsewhere |
| `policy` | policy name | | §8 |
| policy keys | | | §8, for the frequency's path |
| SFR / DCDM keys | | | §5.1, with `mode: sfr` or `dcdm` only |

A frequency no app takes is a talk path by itself (§6.1). A frequency an app takes is no longer a path; the app
offers its own paths. Any frequency inside an SDR's window is heard either way, so a repeater's input can also be
reported.

### 5.1 SFR and DCDM

An `sfr` frequency (single-frequency repeater) or a `dcdm` one (dual-capacity direct mode) runs a repeater in that
mode on the device that transmits there: a hotspot-mode modem (`type` `radio_hotspot` or `auto`). An SDR is refused
for SFR. The keys:

| Key | Type | Default | Meaning |
|---|---|---|---|
| `inbound_slot` | 1 \| 2 | 1 | `sfr` only: the slot the radios send on |
| `duplex` | `half` \| `full` | `half` | `dcdm` only: `full` needs an SDR that is full duplex |
| `active_slots` | `[1]`, `[2]`, `[1, 2]` | `[1, 2]` | `dcdm` only: the slots it repeats |
| `hang_s` | number, > 0 | 5.0 | the repeater's hang |
| `call_hang_s` | number, ≥ 0 | 3.0 | a call's hang |
| `short_lc_ts1_low` | bool | false | |
| `slot_timing` | object | | `utc` refused: the radio keeps its own slot clock |
| `cwid`, `cwid_every_s`, `prebuffer`, `jitter_buffer`, `jitter_min_ms`, `jitter_max_ms` | | §4.5 | |

## 6. Apps

| Type | Takes | Offers | Keys |
|---|---|---|---|
| `repeater` | `in` and `out` frequencies | paths `<name>/1`, `<name>/2` | §6.2 |
| `parrot` | | answers the IDs in its `tg` or `pc` | `name`, `type`, and `tg` or `pc` (ID sets) |
| `ipgw` | | answers its ID | §6.3 |

Later, each as an app type, with no new section: `voting` (the best copy of each burst from several receivers),
`simulcast` (one path, several transmitters), `trunk` (a control frequency and a pool; it assigns its own paths),
`recorder`.

An app that answers IDs (parrot, IP gateway) claims them: a call to a claimed ID on a path whose `apps` allows the app
goes to the app and not to the network. Two apps may not claim the same ID.

The parrot is not a unit of its own: each repeater whose paths may reach it runs one, and with a transceiver one box
parrot hears every frequency and answers through the router ([transmitters.md](transmitters.md) §5).

### 6.1 Talk paths

A talk path is a place a call comes in or goes out. Policies (§8) are set on paths.

| Path | Name | Slots |
|---|---|---|
| a simplex frequency | the frequency's name | one path; transmits on `ts`, hears both |
| an SFR frequency | the frequency's name | one path (the radio times the slots) |
| a DCDM frequency | `<name>/1`, `<name>/2` | a path per slot; policy in `slots` |
| a frequency with `tx: false` | the frequency's name | one path, both slots heard; `slots` makes it a path per slot |
| a repeater | `<app>/1`, `<app>/2` | a path per slot; policy in `slots` |

### 6.2 `repeater`

A repeater on one device that sends `out` and hears `in` (an SDR or a modem), or split: the device that hears `in`
and the one that sends `out` ([../repeater.md](../repeater.md)).

| Key | Type | Default | Meaning |
|---|---|---|---|
| `in`, `out` | frequency reference | | required; `cc` comes from `out`, and `power` (else 255) |
| `slots` | object | | a policy per slot (§8) |
| `policy`, policy keys | | | §8, for both slots |
| `hang_s` | number, > 0 | 5.0 | the repeater's hang |
| `call_hang_s` | number, ≥ 0 | 3.0 | a call's hang |
| `cwid`, `cwid_every_s` | | `""`, 900 | the CW ID |
| `short_lc_ts1_low` | bool | false | |
| `active_slots` | `[1]`, `[2]`, `[1, 2]` | `[1, 2]` | the slots it repeats |
| `slot_timing` | object | the top level's, on a split or the SDR | `utc` only on a split or the SDR |
| `prebuffer`, `jitter_buffer`, `jitter_min_ms`, `jitter_max_ms` | | §4.5 | the network jitter buffer |
| `split` | `{power_cap, prebuffer, lease_s, cooldown_s, max_key_s}` | | the split's own settings ([../repeater.md](../repeater.md)): the first four 0..255, `max_key_s` 0..600 |

The repeater runs on the device that sends `out` when that device hears `in` too and can be the whole repeater
(an SDR, or a modem that isn't a `radio_hotspot`), whatever else hears `in`: an SDR beside a duplex modem only
monitors. Else it is split, with the first device that hears `in`.
On one modem the repeater needs a duplex modem: a `radio_hotspot` is refused. A split's halves are hotspot-mode
modems (`type` `auto` or `radio_hotspot`); its transmit half can't be an SDR. On the SDR it needs a full-duplex
front end.

### 6.3 `ipgw`

The IP gateway: IP packets as DMR data between radios and the network ([ipgw.md](ipgw.md)). One at most.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `tg` or `pc` | one ID | | required: the gateway's ID, the one radios send to |
| `radio_net` | IPv4 CIDR, prefix 8..30 | `12.0.0.0/8` | the radios' addresses, from their DMR IDs |
| `tx` | unit name | the first transmitting unit | where its answers go when no pool picks one. With the transmitter pool, the path the radio was last heard on |
| `ack` | bool | true | answer a confirmed message with an ACK ([ipgw.md](ipgw.md) §10) |
| `max_bytes` | int, 68..1520 | 1500 | the longest IP packet |
| `up_pps`, `down_pps` | int, 0..1000 | 10, 2 | datagrams a second each way (a token bucket of twice that) |
| `idle_s` | int, 5..86400 | 120 | a flow's idle time |
| `max_flows` | int, 1..16 | 8 | NAT flows; cut to the build's `CONFIG_RDOF_IPGW_MAX_SOCKETS` |
| `routes` | list, up to 8 | `[]` | below |
| `nat` | bool or `{enabled, allow, deny}` | off | radios reach the internet through the box. `enabled` (default true), `allow` (empty: any) and `deny` are CIDR lists, up to 8; a `deny` replaces the default 10/8, 172.16/12, 192.168/16, 100.64/10 and 13/8 ([ipgw.md](ipgw.md) §4) |

It needs `routes` or `nat` ("nothing to do"). A route:

| Key | Type | Default | Meaning |
|---|---|---|---|
| `name` | str | `route<n>` | |
| `all` | bool | false | every datagram; not with the three below |
| `private`, `group` | ID or range | | datagrams to these IDs (one of the two) |
| `from` | ID or range | | datagrams from these radios |
| `to` | `"host:port"` or `"host"` | | required: where they go (`host` keeps each datagram's port) |
| `payload` | `udp` \| `ip` | `udp` | the UDP payload, or the whole IP packet |

A firmware build without `CONFIG_RDOF_IPGW` refuses an `ipgw` app, and so does rdofradio (it runs none).

## 7. Networks

| Key | Type | Default | Meaning |
|---|---|---|---|
| `type` | `hblink` \| `brandmeister` \| `homebrew` \| `mqtt` | | required. Later: `openbridge`, others |
| `name` | str | the type | §2.1 |

At most one of each, and one of `hblink`, `brandmeister` and `homebrew`. The first network in the list is the default: a path with no `net` uses it.

### 7.1 `hblink`

The HBlink4 master ([hblink.md](hblink.md)). With it, the top-level `id` is required.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `host` | str | | required |
| `port` | int | 62031 | |
| `transport` | `udp` \| `sctp` | `udp` | the SDR's peers are UDP whatever this says |
| `sctp_encap_port` | int, 1..65535 | 9899 | SCTP over UDP |
| `sctp_heartbeat_ms` | int, 0..600000 | 10000 | |
| `sctp_max_retransmit` | int, 1..20 | 3 | |
| `sctp_ttl_ms` | int, 0..60000 | 0 | |
| `identity` | `single` \| `per_path` | `single` | `single`: the box is one peer (`id`), and its policies decide what goes where. `per_path`: each frequency or repeater is its own peer (`id` + 1, + 2, ... in list order, or `peer_ids`), and the master decides where a call goes: a stream sent to a peer goes only to that peer's paths |
| `peer_ids` | object | | `{"<frequency or repeater>": id}`, with `per_path` only; a repeater's two slots share its peer. Each such peer gives the master its frequency's or repeater's name (the first 8 characters) as its callsign |
| `roaming` | bool | false | let the master move a device that `transmits` on several frequencies (DMRT / DMRK) |

Which units are HBlink peers: with `single`, the box's one peer carries every path. With `per_path` or a
BrandMeister network, a unit is a peer only when its own path is on that network: a repeater on MQTT, or a path with
`net: false`, opens no peer and sends the master nothing (it used to log in beside the others with the box's `id`).
A box with no HBlink network runs no peer at all. The status then has no `hblink` for that unit.

### 7.2 `mqtt`

The MQTT broker ([mqtt.md](mqtt.md)).

| Key | Type | Default | Meaning |
|---|---|---|---|
| `host` | str | | required |
| `port` | int | 1883 | |
| `username`, `password` | str | `""` | |
| `root` | str | `DMR` | the topic root; slashes at either end are dropped |
| `tls` | bool | false | needs `CONFIG_RDOF_MQTT_TLS`; a build without it runs plain MQTT, with a warning |
| `transport` | str | `mqtt/tcp` | `mqtt/tcp` or `mqtt/tls` ([mqtt-net.md](mqtt-net.md) §5); `mqtt/tls` is `tls: true`. `mqtt/tcp` with `tls: true` is refused. The other transports aren't built yet |
| `report` | bool | true | status, the call JSON and the raw bursts, on today's topics ([mqtt.md](mqtt.md)) |
| `calls` | bool | false | carry calls: bursts in and out on topics per path and talkgroup. With it, MQTT can be the only network (no HBlink) |
| `format` | str | `dmrd` | with `calls`: `dmrd` (DMRD packets, [mqtt.md](mqtt.md) §10) or `v3` (the MQTT network's messages, [mqtt-net.md](mqtt-net.md)) |
| `bundle` | int, 1..3 | 1 | `format: "v3"`: voice items per message when sending. Messages with 1 to 3 are taken; 1 is sent for now |
| `raw_tx` | bool | false | `format: "v3"`: transmit `raw` items as heard ([mqtt-net.md](mqtt-net.md) §4.3); off, they are dropped |

Both `report` and `calls` false: the network is off. A box with `mqtt` and `report` only, and no other network,
reports but carries no calls. With `calls`, a path whose `net` is the MQTT network sends what it carries as DMRD
packets on `{root}/calls/{tg|pc}/{dst}/{box}` and takes the other boxes' from `{root}/calls/#`
([mqtt.md](mqtt.md) §10). With `format: "v3"` the calls go as the MQTT network's messages instead: on
`{root}/tg/{tg}/{box}`, `{root}/pc/{id}/{box}` and `{root}/dl/{box}`, with the box's `info` and `activity`
([mqtt-net.md](mqtt-net.md)). `bundle` and `raw_tx` need `format: "v3"`, and `format` needs `calls`. The errors:
`format: dmrd or v3`, `format: with calls true`, `bundle: 1 to 3`, `bundle: with format v3`, `raw_tx: with format
v3`, `transport: mqtt/tcp or mqtt/tls (the others aren't built yet)`. The v3 calls run on a connection of their own
(client ID `rdof-net-{name}`, the box's `name` as the node, so it must be letters, digits, `.`, `_` or `-`), in
rdofradio and on the board; one network in a file takes `format: "v3"` (the first, if several do).

### 7.3 `brandmeister`

A BrandMeister master ([brandmeister.md](brandmeister.md)). Each simplex frequency or repeater is its own login
(`RPTL`, `RPTK`, `RPTC`), a hotspot or a repeater: one connection per frequency or pair, no shared identity.
The top-level `id` isn't needed with it; BrandMeister IDs are their own 32-bit keys. The top-level `callsign` is
required: each login's `RPTC` carries it, and BrandMeister checks it against the login's ID.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `host` | str | | required: the master, `<country code>.master.brandmeister.network` |
| `port` | int | 62031 | |
| `password` | str, 1..63 | | required: the hotspot security password (SelfCare), or a repeater's device password |
| `peer_ids` | object | | required: `{"<frequency or repeater>": id}`, a registered ID for each path on the network (a hotspot: the DMR ID and two digits). No two paths share one, and none is derived |
| `location` | str, ≤ 20 | `""` | `RPTC` location text |
| `description` | str, ≤ 19 | `""` | `RPTC` description |
| `url` | str, ≤ 124 | `""` | `RPTC` URL |

`RPTC` takes the rest from the path: its frequencies, color code, slots (`'4'` simplex, `'3'` a repeater), the
transmitter's power in dBm off the device's power map, and the top-level `location`'s coordinates and height. A
simplex login sends its calls up on slot 2. A device that transmits on a frequency of the network transmits on that
one only (no roaming transceiver). `GET /api/config` shows the password, as it does MQTT's.

### 7.4 `homebrew`

Any other master that speaks the Homebrew repeater protocol: a stock HBlink (one without this project's `DMRC`
peer), TGIF, a FreeDMR or DMR+ style server. The keys, the login and the rules are §7.3's: a login per frequency
or repeater with its ID in `peer_ids`, the top-level `callsign` in each `RPTC`. `password` is the one the master
has for that login (TGIF: the hotspot security key of its self care page), `host` the master's. Use `hblink` only
for an HBlink4 that knows `DMRC` (§7.1).

Status (2026-11-11): the settings and the login are the ones tested against the simulated master; not yet run
against a stock HBlink or TGIF. `RPTO` (an options string, which some masters take static talkgroups from) isn't
sent.

## 8. Policies

A policy says which calls a path carries. It is a set of these keys, on a frequency (for its path), in a `slots` entry
(for a slot's path), or in a named policy:

| Key | Type | Default | Meaning |
|---|---|---|---|
| `tg` | ID set | none | talkgroups carried, always on |
| `ptt` | ID set | none | talkgroups carried once an RF user keys one, for `ptt_hold_s` after the last call |
| `ptt_hold_s` | int, 1..86400 | 600 | |
| `pc` | ID set | `"*"` | private calls carried, by the ID called (rf→net) or the ID calling (net→rf) |
| `apps` | `"*"`, a list of app names, or `false` | `"*"` | the apps this path may reach |
| `net` | network name or `false` | the first network | where this path's calls go and come from; `false`: none |
| `dir` | `both` \| `rf>net` \| `net>rf` | `both`; `rf>net` when the frequency has `tx: false` | |

Where a path gets a key, first match wins:

1. the path's own keys (the frequency, or the `slots` entry);
2. the named policy it uses (a `slots` value or `"policy": "<name>"` on a frequency);
3. `defaults` (frequencies only; a repeater slot doesn't take `defaults`);
4. the table above.

A `slots` value is a policy name or an inline policy: `{"1": "open", "2": {"tg": 3100}}`.

A named policy:

```jsonc
"policies": [
  {"name": "open",   "tg": "*,!9,!4000-4099", "apps": false},
  {"name": "closed", "tg": "3100,3120,3177"}
]
```

## 9. How calls move

### 9.1 RF to network

A call heard on a path:

1. A group call to a TG claimed by an app the path may reach goes to that app. Same for a private call to a claimed ID.
2. Else, a group call whose TG is in the path's `tg`, or in its `ptt` (which starts or renews the hold), goes to the
   path's network, if `dir` allows.
3. Else, a private call whose called ID is in `pc` goes to the network, if `dir` allows.
4. Else it is not sent. It still shows in last heard and the call JSON (`report`).

A repeater repeats locally first, whatever its policy.

### 9.2 Network to RF, group calls

A group call from a network goes to every path on that network whose `tg`, or held `ptt`, holds the TG, and whose
`dir` allows. That fan-out is by design: a TG on two paths is sent on both.

When one path is wanted by two calls:

- A TG listed by ID or range beats one matched only by `*`.
- Between equals, the call already on the path keeps it; the newer one is refused (and counted).
- An emergency call beats both.

### 9.3 Network to RF, private calls

A private call from a network goes to the path where the called radio was last heard (within `pc_hold_s`, default
900 s), if that path's `pc` holds the calling ID and its `dir` allows. A radio not heard within the hold: the call is
dropped and counted. The last-heard table is per box.

With `identity: per_path` the master picks the peer, so a private call sent to a peer goes to that peer's path whether
or not the box heard the radio there.

### 9.4 Transmitting

A path's transmission goes to a device that `transmits` on its frequency and is free, preferring one already there:
a repeater app's repeater, an SFR or DCDM frequency's repeater, or a transceiver tuned to the frequency (its `cc` and
`power`) for the call. The transmitter pool picks the transceiver ([transmitters.md](transmitters.md) §4): an
emergency takes one from a call that isn't one. None free: the call isn't sent on that path (counted). The receiver
drops our own transmission heard back, so it never goes back to the network. An SDR's transmitter is in the pool,
tuned per stream; the parrot, the IP gateway and the bench send through it too ([transmitters.md](transmitters.md)
§12).

## 10. Scenarios

Each passes the schema and the open (§13). §10.1 and §10.3 use the `iio` driver, so they are host files (rdofradio);
a board takes them with a board's SDR (`lr2021`, or `rtlsdr` to receive only) and its rate. rdofradio runs one
repeater or one transceiver on the SDR; it has no IP gateway.

### 10.1 Commercial: 16 simplex frequencies

- 16 frequencies, all simplex; one TG each, all slot 1.
- Each TG both ways; every private call both ways.
- Parrot on 9990 and the IP gateway on 9991, from any frequency.

```jsonc
{
  "version": 3,
  "name": "plant",
  "id": 9800100,
  "defaults": {"cc": 2, "ts": 1},
  "devices": [
    {"kind": "sdr", "driver": "iio", "match": {"uri": "local:"}}
  ],
  "frequencies": [
    {"freq": 438.0625, "tg": 201},
    {"freq": 438.0875, "tg": 202},
    {"freq": 438.1125, "tg": 203},
    {"freq": 438.1375, "tg": 204},
    {"freq": 438.1625, "tg": 205},
    {"freq": 438.1875, "tg": 206},
    {"freq": 438.2125, "tg": 207},
    {"freq": 438.2375, "tg": 208},
    {"freq": 438.2625, "tg": 209},
    {"freq": 438.2875, "tg": 210},
    {"freq": 438.3125, "tg": 211},
    {"freq": 438.3375, "tg": 212},
    {"freq": 438.3625, "tg": 213},
    {"freq": 438.3875, "tg": 214},
    {"freq": 438.4875, "tg": 215},
    {"freq": 438.7125, "tg": 216}
  ],
  "apps": [
    {"type": "parrot", "tg": 9990},
    {"type": "ipgw",   "tg": 9991, "nat": true}
  ],
  "networks": [
    {"type": "hblink", "host": "hb.example.net", "port": 62031}
  ]
}
```

- No policies: `pc` and `apps` default to all, and `dir` to both.
- The SDR hears all 16 at once (438.0625 to 438.7125 fits the default 1.536 MS/s window) and transmits on whichever a
  network call needs, one at a time. It runs as two units: `sdr0` (the transceiver) and `sdr0-rx` (the receiver).

### 10.2 Two repeaters on split radios

- Two repeaters, each on its own pair of radios: one receives, one transmits.
- TS1: every TG both ways except a deny list; every private call.
- TS2: only a few TGs; every private call; parrot and the IP gateway.

```jsonc
{
  "version": 3,
  "name": "hilltop",
  "id": 9800200,
  "defaults": {"cc": 1},
  "usb_hub": true,
  "devices": [
    {"name": "r1rx", "kind": "modem", "port": {"uart": 1, "tx": 5, "rx": 6}, "hears": {"idle": "r1 in"}, "transmits": false},
    {"name": "r1tx", "kind": "modem", "port": {"usb": "auto", "serial": "R1TX"}, "hears": false, "transmits": ["r1 out"]},
    {"name": "r2rx", "kind": "modem", "port": {"uart": 2, "tx": 15, "rx": 16}, "hears": {"idle": "r2 in"}, "transmits": false},
    {"name": "r2tx", "kind": "modem", "port": {"usb": "auto", "serial": "R2TX"}, "hears": false, "transmits": ["r2 out"]}
  ],
  "frequencies": [
    {"name": "r1 in",  "freq": 438.25},
    {"name": "r1 out", "freq": 438.75},
    {"name": "r2 in",  "freq": 438.3},
    {"name": "r2 out", "freq": 438.8}
  ],
  "policies": [
    {"name": "open",   "tg": "*,!9,!4000-4099", "apps": false},
    {"name": "closed", "tg": "3100,3120,3177"}
  ],
  "apps": [
    {"name": "R1", "type": "repeater", "in": "r1 in", "out": "r1 out", "slots": {"1": "open", "2": "closed"}},
    {"name": "R2", "type": "repeater", "in": "r2 in", "out": "r2 out", "slots": {"1": "open", "2": "closed"}},
    {"type": "parrot", "tg": 9990},
    {"type": "ipgw",   "tg": 9991, "nat": true}
  ],
  "networks": [
    {"type": "hblink", "host": "hb.example.net", "port": 62031}
  ]
}
```

- A board file: each split's receive half on a UART, its transmit half on USB through a hub (`usb_hub`), told apart
  by serial number.
- `open` has `apps: false`, so 9990 keyed on TS1 is an ordinary TG and goes to the network.
- A network call on TG 3100 goes to R1/2 and R2/2 (listed) and to R1/1 and R2/1 (`*`). Where it meets another call,
  the listed slots win (§9.2).
- `pc` is the default (all) on both slots.

### 10.3 One full-duplex SDR: a repeater and four forwarders

- One full-duplex SDR: five receive frequencies, one transmit frequency.
- A repeater using the SDR for its input and output.
- Four forwarders: one per remaining frequency, receive only, sending everything to a network.

```jsonc
{
  "version": 3,
  "name": "tower",
  "id": 9800300,
  "defaults": {"cc": 1},
  "devices": [
    {"kind": "sdr", "driver": "iio", "match": {"uri": "local:"}}
  ],
  "frequencies": [
    {"name": "rpt in",  "freq": 438.25},
    {"name": "rpt out", "freq": 438.75},
    {"name": "fwd A", "freq": 438.9375, "tx": false, "tg": "*", "net": "mqtt"},
    {"name": "fwd B", "freq": 438.9625, "tx": false, "tg": "*", "net": "mqtt"},
    {"name": "fwd C", "freq": 438.9875, "tx": false, "tg": "*", "net": "mqtt"},
    {"name": "fwd D", "freq": 438.6, "tx": false, "tg": "*", "net": "mqtt"}
  ],
  "apps": [
    {"name": "R1", "type": "repeater", "in": "rpt in", "out": "rpt out",
     "slots": {"1": {"tg": 3100}, "2": {"tg": "3101-3199", "ptt": "3200-3299", "ptt_hold_s": 600}}}
  ],
  "networks": [
    {"type": "hblink", "host": "hb.example.net", "port": 62031},
    {"type": "mqtt", "host": "broker.example.net", "root": "DMR", "calls": true}
  ]
}
```

- The SDR transmits only on "rpt out": the forwarders have `tx: false`, and a repeater's input isn't transmitted on.
- The forwarders' `dir` is `rf>net` (from `tx: false`) and `pc` is all (the default), so they pass every call, both
  slots.
- The repeater uses HBlink, the first network. Units: `R1` and `sdr0-rx` (the forwarders).

### 10.4 Monitoring only: eight frequencies to MQTT

- Eight frequencies: four repeater outputs and four simplex frequencies.
- Receive only; every call on every frequency goes to MQTT; no apps.

```jsonc
{
  "version": 3,
  "name": "listener",
  "id": 9800400,
  "defaults": {"tx": false, "tg": "*"},
  "devices": [
    {"kind": "sdr", "driver": "rtlsdr", "match": {"serial": "00000001"}, "rx": {"rate": 288000, "gain_db": 30}}
  ],
  "frequencies": [
    {"name": "city rpt",   "freq": 438.1625, "cc": 1},
    {"name": "county rpt", "freq": 438.1875, "cc": 3},
    {"name": "fire rpt",   "freq": 438.2125, "cc": 1},
    {"name": "water rpt",  "freq": 438.2375, "cc": 7},
    {"name": "yard 1",     "freq": 438.0625},
    {"name": "yard 2",     "freq": 438.0875},
    {"name": "yard 3",     "freq": 438.1125},
    {"name": "yard 4",     "freq": 438.1375}
  ],
  "networks": [
    {"type": "mqtt", "host": "broker.example.net", "root": "DMR", "calls": true}
  ]
}
```

- A board file: an RTL-SDR on the board's USB port at 288 kS/s, so the eight fit 288 kHz.
- `defaults` makes every frequency receive only, carrying every TG; `pc` is all by default. So every call goes to
  MQTT.
- Nothing transmits anywhere: every frequency has `tx: false`, and the RTL-SDR can't transmit anyway. The one unit is
  `sdr0-rx`.
- The simplex frequencies leave out `cc`, so any color code is taken.
- With `calls: false`, the same file still sends every burst on the report topics (raw and call JSON).

### 10.5 A W12

A Meshnology W12 (the `w12` profile): its LR2021 hears three simplex frequencies and transmits on any of them, with
the parrot, the IP gateway, HBlink, MQTT and the bench. The address is a placeholder; the board adds `wifi` and
`ntp` from its own store (§16).

```jsonc
{
  "version": 3,
  "name": "w12",
  "id": 9990501,
  "callsign": "N0CALL",
  "location": {"latitude": 39.7392, "longitude": -104.9903, "height": 10},
  "defaults": {"cc": 1},
  "devices": [
    {"name": "w12", "kind": "sdr", "driver": "lr2021",
     "rx": {"rate": 240000, "center_hz": 438.1},
     "tx": {"power_dbm": 10}}
  ],
  "frequencies": [438.075, 438.1, 438.125],
  "apps": [
    {"type": "parrot", "tg": 9990},
    {"type": "ipgw", "tg": 9991, "nat": {"allow": ["0.0.0.0/0"]}}
  ],
  "networks": [
    {"type": "hblink", "host": "192.0.2.10", "port": 62031},
    {"type": "mqtt", "host": "192.0.2.10", "port": 1883, "root": "DMR"}
  ],
  "bench": {"tx": {"freqs_hz": [438100000], "max_key_s": 10, "max_dbm": -20}}
}
```

- `driver: lr2021` is required: the default `rtlsdr` can't transmit, and the open refuses an SDR that transmits on
  a driver that can't.
- Units: `w12` (the transceiver: the router tunes the LR2021 per stream) and `w12-rx` (the receiver, which reports
  all three). The LR2021 is half duplex, so it can't run a duplex repeater.
- 240 kS/s centered on 438.1 MHz holds 438.075 to 438.125.
- The parrot answers on the frequency the call was heard on; the IP gateway answers on the path the radio was last
  heard on.
- No policies: every TG and private call both ways, on HBlink (the first network); MQTT reports.
- The bench may key the LR2021 on 438.1 MHz for at most 10 s ([../bench-api.md](../bench-api.md)).

## 11. The resolved view

`GET /api/resolved` ends with the settings as they run (`rdof_site_resolved_text`, JSON with a line per entry):

- `frequencies`: each with every key filled in from `defaults`, and the devices that hear it and may send on it;
- `paths`: each with its policy expanded (ID sets in their canonical form), its peer, and the source of each key
  (`own`, `policy <name>`, `defaults`, `default`);
- `talkgroups`: the reverse view, each TG or range with the path that carries it and how (`static`, `ptt`, `*`,
  `ptt *`);
- `apps`, `networks` and `devices`.

A file the open refuses doesn't run; the page's settings save says why (the field's path and the rule).

## 12. How a box runs it

Version 3 is the only settings format: a file without `"version": 3` is refused (`version: 3`). Versions 1 and 2 were
prototypes and are gone, with no upgrade.

A file is opened once (`rdof_plan_open`, `common/src/siteplan.c`). The open parses the document
(`rdof_site_ctx_parse`), checks the top level's keys, the devices' own keys (port, SDR, modem, bench), the units'
keys, makes the run units, checks them (§13.1), and adds the SDR's extra channels. Each part of the box then reads
its own settings from the plan, straight from the version 3 places; no other format is written in between.

The plan:

| Function | Gives |
|---|---|
| `rdof_plan_open`, `rdof_plan_close` | the plan, or NULL with `"<path>: <rule>"` |
| `rdof_plan_site`, `rdof_plan_doc`, `rdof_plan_root` | the site (`rdof_site_t`), the JSON document and its root |
| `rdof_plan_dev_node`, `_freq_node`, `_app_node`, `_net_node` | an entry's JSON node |
| `rdof_plan_net_of` | the network of a type, or −1 |
| `rdof_plan_dev` | a device's own keys, checked (`rdof_plan_dev_t`: port, `needs`, TX, carriers, power, bench) |
| `rdof_plan_nunits`, `rdof_plan_unit`, `rdof_plan_unit_find` | the run units (`rdof_unit_t`) |
| `rdof_plan_nextra`, `rdof_plan_extra` | the SDR's extra channels: a frequency a unit on the SDR hears that its receiver doesn't |
| `rdof_plan_nbench`, `rdof_plan_bench` | the bench devices |
| `rdof_plan_receiver`, `rdof_plan_sdr_used`, `rdof_plan_sdr_name` | an SDR's receiver unit, whether anything uses it, its name |
| `rdof_plan_check` | what a platform opens (§13.1): `rdof_plan_board` or `rdof_plan_host` |
| `rdof_plan_roles` | what a board runs (`rdof_roles_t`): its units, at most 2 that transmit and one SDR |

Each part's settings:

| Function | Fills | From |
|---|---|---|
| `rdof_plan_radio` (`config.c`) | `rdof_config_t` of one repeater, SFR, DCDM or transceiver unit | the unit's device (port, `type`, `mmdvm`, talker keys, `tx.power_dbm`), its frequencies (`freq`, `cc`, `power`, `ts`, the SFR / DCDM keys), its repeater app, the networks, the top level (`name`, `id`, `callsign`, `location`, `slot_timing`) |
| `rdof_plan_sdr` (`sdr/src/config.c`) | `rdof_sdr_config_t` of an SDR and its receiver unit | the SDR device (`driver`, `rx`, `tx`, ...), the frequencies it hears, the networks, the top level |
| `rdof_plan_ipgw` (`ipgwcfg.c`) | `rdof_ipgw_config_t` | the `ipgw` app |
| `rdof_plan_bench_radios`, `rdof_plan_bench_tx` (`benchcfg.c`) | the bench's devices and TX policy | `bench`, and the devices' `bench` |
| `rdof_oled_settings_parse` (`oled_sdr.c`) | the screen | `oled`, from the text |

Each has a text form that opens a plan of its own, for a caller with only the text: `rdof_box_roles`,
`rdof_config_parse_radio`, `rdof_config_parse`, `rdof_sdr_config_parse`, `rdof_sdr_config_parse_device`,
`rdof_ipgw_settings_parse`, `rdof_bench_radios_parse`, `rdof_bench_tx_parse`.

The run units are internal; nothing in a file names them:

| Unit | Made from | Runs |
|---|---|---|
| repeater | a `repeater` app | a repeater on its `in` and `out`: one device that sends `out` and hears `in` (an SDR or a modem), or the device that hears `in` and the one that sends `out` (split) |
| SFR, DCDM | an `sfr` or `dcdm` frequency | a repeater in that mode on the device that sends there |
| transceiver | a device that transmits on simplex frequencies no app takes | a transceiver over those frequencies, named after the device: `hears.idle` (else the first) its idle frequency, `hears.scan` its scan list. For an SDR it is the box's transmitter: the router tunes it per stream, and the receiver unit reports what's heard ([transmitters.md](transmitters.md) §12) |
| receiver | each SDR that hears a frequency no repeater on it takes | those frequencies, named `<device>-rx`; with `per_path`, each its frequency's peer |

The parrot is not a unit: it runs in each repeater whose paths may reach it, or, with a transceiver in the router,
as one box parrot that hears every frequency and answers through the router ([transmitters.md](transmitters.md)
§5).

The firmware (`settings.c:resolve_plan`) opens the stored text, checks it for the board (`rdof_plan_check`), takes
the roles, the screen and the IP gateway, then each radio unit's settings (`rdof_plan_radio`, then
`rdof_config_check_ports` over them all) and the SDR's (`rdof_plan_sdr`), and checks them against the build (§13.1).
A stored file that fails runs `DEFAULT_JSON` (§16). rdofradio (`rdofradio_cfg.c`) opens `--config` with
`rdof_plan_host`, runs the one repeater or transceiver on the SDR, and takes the SDR's other frequencies as its
monitor channels; without `--config` it builds a version 3 file from its flags.

The router (`rdof_siteroute_*`) is asked at each stream's start, by every sink that would send it (the HBlink peer,
MQTT calls), and once for the stream: the network it goes to, or none. A network's burst goes to the transmitters the
router picks (§9.4). The parrot's admit is the path's `apps`.

With `identity: single` the first transceiver or repeater owns the box's HBlink peer and every other unit and
channel sends through it. HBlink4 credits a stream to the peer's last DMRC (its frequency), so a stream another unit
sends shows under the owner's frequency on the master.

### 12.1 What versions 1 and 2 had that version 3 doesn't

Added to version 3: the talker keys (`cwid`, `cwid_every_s`, `prebuffer`, `jitter_buffer`, `jitter_min_ms`,
`jitter_max_ms`) on an SFR or DCDM frequency, and `active_slots` (`[1]`, `[2]` or `[1, 2]`, the default) on a
repeater app and a DCDM frequency: the slots it repeats.

Dropped: `run` and `enabled` (every unit the file makes runs; take a device or app out to stop it), a role's own
`location`, a monitor's `receiver_ids` and `peer_ids` formats and its own `slot_timing`, a transceiver, SFR or DCDM on
split devices (only a repeater splits), a receiver on a device that isn't an SDR, the plain SDR config (`device` at
the top), the SDR-only firmware's stored settings and the broker (its checks are the open's now, §13.1).

## 13. Limits

| What | At most | Where |
|---|---|---|
| frequencies | 48 | `RDOF_SITE_MAX_FREQS` |
| devices | 16 | `RDOF_SITE_MAX_DEVICES` |
| apps | 16 | `RDOF_SITE_MAX_APPS` |
| networks | one `hblink`, one `mqtt` | |
| named policies | 16 | `RDOF_SITE_MAX_POLICIES` |
| talk paths | 64 | `RDOF_SITE_MAX_PATHS` |
| ranges in an ID set (plain and `!` together, after merging) | 12 | `RDOF_IDSET_MAX` |
| radios in the last-heard table | 256 | `RDOF_ROUTER_HEARD` |
| talkgroups held by `ptt` on a path at once | 4 | `RDOF_ROUTER_PTT` |
| transceivers the router drives | 4 | `RDOF_TXPOOL_MAX` |
| a name | 39 characters, no `/`, `"` or `\` | `RDOF_SITE_NAME` |
| run units | 16 | `RDOF_PLAN_MAX_UNITS` |
| frequencies a transceiver transmits on | 32 | `RDOF_CFG_CHANNELS` |
| an SDR's receive channels (receiver and extras) | 16 | `RDOF_SDR_MAX_CHANNELS` |
| bench devices | 4 | `RDOF_BENCH_MAX_RADIOS` |
| bench TX frequencies | 32 | `RDOF_BENCHTX_MAX_FREQS` |
| bench TX bands | 8 | `RDOF_BENCHTX_MAX_BANDS` |
| IP gateway routes; networks in `allow` or `deny`; flows | 8; 8 each; 16 | `RDOF_IPGW_MAX_*` |
| the document | 2000 JSON nodes, 16 KB of strings | `RDOF_SITE_MAX_NODES`, `RDOF_SITE_STR_BUF` |

A board runs at most 2 units that transmit (`RDOF_MAX_RADIOS`; a second needs PSRAM) and one SDR.

### 13.1 Checks

The open (`rdof_plan_open`) refuses, with the place and the rule:

| Check | Refused |
|---|---|
| units | an SFR on an SDR; an SFR or DCDM without a hotspot-mode modem (`radio_hotspot` or `auto`); `duplex: full` on a modem; a split whose transmit half is an SDR, or whose halves aren't hotspot-mode modems; a duplex repeater on a `radio_hotspot`; one modem or radio for two units ("a modem serves one"); two units with one name |
| an SDR's driver (`check_caps`) | a rate over the driver's (`rtlsdr` 3.2 MS/s); a heard frequency outside its tuning range (`rtlsdr` 24 to 1766 MHz) or outside the window when `center_hz` is set; more than 16 receive channels |
| an SDR that transmits | a driver that can't (`rtlsdr`); `tx.carriers` over the driver's (1 for every driver for now); more transmitting units than carriers; a duplex repeater, or a DCDM with `duplex: full`, on a half-duplex SDR (the LR2021) |
| the bench | a bench modem that a frequency's `devices` pins to a unit (listed in `bench.devices`, it makes no unit otherwise); an SDR with both `bench` and `tx.bench` |
| a transceiver | more than 32 frequencies to transmit on (`RDOF_PLAN_MAX_FREQS`, `RDOF_CFG_CHANNELS`) |

`rdof_plan_check` (`siteplan.c`) checks the devices against a platform:

| Platform | Refused |
|---|---|
| `rdof_plan_board` | `soapy` or `iio` ("the host tool's, not this board's"); a `path` port ("a serial path is for a host") |
| `rdof_plan_host` | a `uart` port ("this host has no UARTs of its own"); a split may have both halves on `path` ports (a path isn't the board's one USB port) |

`rdof_plan_roles` refuses more than 2 units that transmit ("a board runs 2 units that transmit at most"), a second
SDR ("a board runs one SDR"; rdofradio: "a host runs one SDR"), and a file that makes no unit ("nothing to run"). `rdof_plan_roles_on` takes the
platform's limit: `rdof_plan_host` allows 8 (`RDOF_ROLES_MAX`). rdofradio runs one unit that transmits, or up to 8
split repeaters whose RX half is the SDR, each TX half a modem at a `path` port (the first may take `--split-tx`'s);
anything else beside a second unit is refused ("rdofradio runs one unit that transmits, or split repeaters on the
SDR").

The firmware then checks against the build and the board (`settings.c`): an `ipgw` app without
`CONFIG_RDOF_IPGW`; ports (`rdof_config_check_ports`: the SDR modem shared, a UART's number and pins, two USB devices
told apart by `serial` or `usb_port`); a second unit without PSRAM; the SDR's front end in the build, its rate and
channels (`rdof_fe_check`); and a unit on the SDR: the front end transmits, is full duplex where asked, hears the
unit's frequencies, and tunes its transmit frequency.

### 13.2 The schema

`config/rdof-config.schema.json` is the version 3 settings as JSON Schema: each place's keys, types and ranges. It
doesn't do the checks that need the whole file (names, references, devices' reach); the open does.

- `tools/config_check.py <file>` checks a file against it, with Python's `jsonschema`, or with node and
  `config/schema-check.js` when that's missing.
- `host-tests/test_schema.js` (ctest `rdof_schema_tests`, when node is installed) checks the fixtures in
  `host-tests/fixtures/config/v3/`: the `good-*` files pass, the `bad-*` files fail, and the `bad-semantic-*` files
  pass the schema but fail the open. It also checks that the schema's keys match the key lists in `site.c` and
  `siteplan.c`.
- The C has its own tests: `rdof_site_tests` (`test_site.c`) and `rdof_plan_tests` (`test_plan.c`).

Not built: the schema served by the board (`GET /api/config/schema`), a check-only save (`POST /api/config?check=1`),
and the page checking against the schema before a save.

## 14. Later

- **Bridges:** for what policies on paths don't do: TG rewrites (TG 9 on RF is TG 3100 on the network), one call on
  several paths as one (simulcast), and network-to-network links. A `bridges` list of members (path, network or app),
  each with its own TG and activation. Added beside policies, not instead.
- **A system file:** one file for several boxes, with `site` on each device; each box runs what's local to it. Remote
  receivers (`kind: remote`, `via`, `site`) and voting need it.
- **A shared last-heard table** (over MQTT), so private calls find a radio heard at another box: the MQTT network's
  router ([mqtt-net.md](mqtt-net.md) §9.1).
- **`hears.scan` on an SDR:** scanning frequencies outside its window.
- **`pc_home`:** a path for private calls to a radio never heard here (today: dropped, with `identity: single`).
- **SFR as an app:** the box itself as the SFR repeater (its SDR, or its SDR with another transmitter or a modem),
  beside today's SFR frequency served by a connected radio.
- **`apps` on SFR / DCDM paths** and the parrot on several transmitters at once ([transmitters.md](transmitters.md) §5).
- **Scheduled talkgroups** (`mode: scheduled`).

## 15. Decisions taken

- Policy on the path, not a route table. The common sites are allow lists, deny lists and private calls on or off per
  path; those are cleanest as a property of the path. The reverse view (§11) answers "where does this TG go".
- Lists with optional names for every section; a frequency's name defaults to its frequency.
- One ID-set syntax for talkgroups and private-call IDs.
- A group call from a network goes to every path that carries it (fan-out). A listed TG beats `*`.
- A private call from a network goes where the radio was last heard; never heard here: dropped. With `per_path`,
  where the master sent it.
- `tx: false` makes a path receive only, with `dir: rf>net`.
- HBlink `identity` defaults to `single`.
- Version 3 is the only format (2026-11-01): versions 1 and 2 were prototypes, dropped with no upgrade. A file is
  parsed straight into what runs (§12).
- Apps claim IDs; `apps` on a path says which it may reach.
- MQTT `format` (2026-11-02): `dmrd` stays the default; `v3` is the MQTT network ([mqtt-net.md](mqtt-net.md)).
  `transport` takes `mqtt/tcp` and `mqtt/tls` for now; `tls: true` stays as the short form of `mqtt/tls`.

## 16. On the board

### 16.1 NVS

| Namespace | Key | Type | Content | Written by / when |
|---|---|---|---|---|
| `rd` | `config` | blob | the settings text, `wifi` and `ntp` removed | `settings.c:settings_save` on POST `/api/config` (then a restart) |
| `rd` | `selftest` | u8 | a one-shot flag | set by POST `/api/selftest`; taken (erased) at boot by `sdr.c:sdr_boot_selftest` |
| `rd` | `spike` | u8 | a one-shot flag | set by POST `/api/spike`; taken at `sdr_role_start` |
| `rdof_ppm` | `l<hash>` | blob `{char serial[64]; float ppm}` | a tuner's coarse value (the sample clock's coarse move); `<hash>` is 8 hex digits of FNV-1a over the serial | `sdr_task.c`, when the table has nothing for the die's temperature |
| `rdof_ppm` | `learned`, `cal` | as `l<hash>`, `c<hash>` | one tuner, from before the per-tuner keys | read only, when a tuner has no key of its own |
| `rdof_ppm` | `c<hash>` | blob `{char serial[64]; nc_tcal_t cal}` | a dongle's ppm by die temperature | `sdr_task.c` when settled; a bin again at most every `CAL_AGAIN_S` 7200 s; sampled every `PPM_EVERY_US` 5 s; follows ppm within `CAL_FOLLOW_PPM` 0.25 |
| `netclock` | `servers` | str | ≤ `NETCLOCK_SERVERS_LEN` 128 | the settings' `ntp`, `/api/clock/settings` |
| `netclock` | `poll_s` | u32 | 16..3600 | same |
| `netclock` | `rate_ppb` | i32 | the board crystal's rate | after ≥ `NC_RATE_SAVE_MIN_N` 8 refs, moved ≥ 0.3 ppm, ≤ once / 3600 s |
| `netclock` | `tcal` | blob `nc_tcal_t` | `NC_TCAL_N` 24 bins of 2 °C | when a bin is learned, ≤ every `NC_TCAL_AGAIN_S` 7200 s |
| `wifilist` | `nets` | blob | byte 0 the format (1); byte 1 the count; then `rec_t {char ssid[33]; char password[65]; int8 priority; uint8 flags (bit0 hidden)}` × count | the settings' `wifi`, `/api/wifi/networks` |

An NVS partition with no free pages or a new version is erased at boot (`main.c`), losing every key above.

### 16.2 Precedence

| Layer | What it sets | Wins over |
|---|---|---|
| Build (Kconfig, the profile; [overview.md](overview.md) §4) | features, pins, sizes, USB VID / PID, the components' defaults | nothing at run time; a setting that needs a missing feature is refused (`mqtt.tls`: plain MQTT, with a warning) |
| `DEFAULT_JSON` (`settings.c`) | the settings when NVS has none or the stored ones fail | |
| NVS `rd` / `config` | the settings | `DEFAULT_JSON` |
| The components' NVS | the Wi-Fi list, NTP, the clock's rate, calibrations | `wifi` and `ntp` in old stored settings (moved over once, only when the component has nothing stored) |
| A settings save (POST `/api/config`) | the whole settings; its `wifi` and `ntp` replace the components' | everything stored; opened and checked first (§13.1), refused whole on an error, a restart after |
| The components' pages (`/api/wifi/*`, `/api/clock/settings`) | their component's NVS | the last save, for that component |
| One-shot flags | the self-test or spike on the next boot | the SDR's `selftest` / `spike` keys (either turns it on) |

`DEFAULT_JSON`: name `esp32`, id 9990301, callsign `RT01`, `defaults.cc` 2; in a build with the roamer, a modem `ROAM1` (`radio_hotspot` on USB
`auto`) on "Ch 1" 438.5 MHz (pinned with `devices`), its idle frequency; in a build with the SDR, an RTL-SDR `s3-sdr` (`rx.rate` 288000,
`gain_db` 30) hearing Area 4 438.0625 (TG 213), Service Bravo 438.1125 (TG 204), Operations 3 438.1625 (TG 206),
Service Desk 438.2125 (TG 208) and Area 1 438.2625 (TG 210); HBlink at 192.0.2.115:62031, MQTT at
192.0.2.115:1883 (user `radios`).

GET `/api/config` is the stored text with `wifi` (no passwords) and `ntp` added back (`rdof_jsonc_add`).
