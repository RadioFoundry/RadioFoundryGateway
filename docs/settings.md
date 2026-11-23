# Settings

One JSON file, version 3, runs every box: an ESP32 board, `rdofradio` on a Linux or macOS host, a PlutoSDR. This page
is the short version. Every key, default and check is in [config-v3.md](spec/fw/config-v3.md).

## The file's shape

```jsonc
{
  "version": 3,                 // required
  "name": "site1",              // the box's name: MQTT topics, the page, mDNS
  "id": 9800100,                // your DMR ID: the HBlink4 peer ID
  "callsign": "N0CALL",
  "defaults": {"cc": 1},        // keys every frequency takes unless it sets its own
  "devices": [],                // the radios: SDRs and modems
  "frequencies": [],            // what is heard and sent
  "apps": [],                   // repeater, parrot, IP gateway, Tier III site
  "networks": []                // HBlink4, BrandMeister, another Homebrew master, MQTT
}
```

| Section | What goes in it | Spec |
|---|---|---|
| `devices` | an `sdr` (`driver`: `rtlsdr`, `lr2021`, `soapy`, `iio`) or a `modem` (an MMDVM board or a radio in hotspot mode) and its `port` | [§4](spec/fw/config-v3.md#4-devices) |
| `frequencies` | a number (MHz below 1e5, else Hz) or an object with `freq`, `name`, `cc`, `ts`, `tx` | [§5](spec/fw/config-v3.md#5-frequencies) |
| `apps` | `repeater` (takes an `in` and an `out` frequency), `parrot`, `ipgw`, `tier3` (a trunked site; not on a board) | [§6](spec/fw/config-v3.md#6-apps), [tier3.md](spec/fw/tier3.md) §4 |
| `networks` | `hblink`, `brandmeister`, `homebrew`, `mqtt`: at most one of each, and one master | [§7](spec/fw/config-v3.md#7-networks) |
| `policies` and the policy keys (`tg`, `ptt`, `pc`, `net`) | which calls go where | [§8](spec/fw/config-v3.md#8-policies) |
| `rules` | calls kept off networks, for the whole box (TG 9 on the repeater only, say) | [§8.1](spec/fw/config-v3.md#81-rules) |
| `bench`, `ntp`, `wifi`, `oled`, `location`, `slot_timing` | the rest | [§2](spec/fw/config-v3.md#2-document), [§16](spec/fw/config-v3.md#16-on-the-board) |

Rules that catch people:

- **A network carries no talkgroups unless a `tg` says so.** Put `"tg": "*"` in `defaults` (or on a repeater's
  `slots`) to send everything up. Private calls (`pc`) are all carried by default.
- A frequency's `cc` is required unless it has `"tx": false`.
- Comments (`//`, `/* */`) are fine.
- A file without `"version": 3` is refused.

## Recipes

Each one passes `tools/config_check.py`. The addresses, IDs and passwords are placeholders: use your own and
frequencies you're licensed for.

### An SDR monitor to HBlink4 and MQTT

An ESP32-S3 board with an RTL-SDR on its USB port hears five frequencies. Each frequency is its own HBlink4 peer
(`per_path`: IDs 9800101 to 9800105), and every call goes to MQTT.

```json
{
  "version": 3,
  "name": "monitor",
  "id": 9800100,
  "callsign": "N0CALL",
  "defaults": {"tx": false, "tg": "*"},
  "devices": [
    {"name": "rtl", "kind": "sdr", "driver": "rtlsdr", "rx": {"rate": 288000, "gain_db": 30}}
  ],
  "frequencies": [
    {"name": "rpt A", "freq": 438.0625, "cc": 1},
    {"name": "rpt B", "freq": 438.1125, "cc": 3},
    438.1625, 438.2125, 438.2625
  ],
  "networks": [
    {"type": "hblink", "host": "hb.example.net", "port": 62031, "identity": "per_path"},
    {"type": "mqtt", "host": "broker.example.net", "root": "DMR"}
  ]
}
```

- `"tx": false` in `defaults`: nothing transmits, and a frequency without `cc` takes any color code.
- 288 kS/s holds about 288 kHz of channels; on an ESP32 the RTL-SDR's `rx.rate` is 288000 or 240000.
- Board only: `rdofradio --config` runs a file with something that transmits, and refuses this one ("nothing
  transmits"). On a host, `rdofsdr` and `rdofradio --listen` receive without a settings file and print or serve the
  bursts, but don't report to HBlink4 or MQTT ([linux-macos.md](linux-macos.md)).
- Eight frequencies to MQTT only: [config-v3.md §10.4](spec/fw/config-v3.md#104-monitoring-only-eight-frequencies-to-mqtt).

### A W12 as a transceiver

A Meshnology W12 hears three simplex frequencies on its LR2021 and answers on whichever a call came in on, with the
parrot on TG 9990.

```json
{
  "version": 3,
  "name": "w12",
  "id": 9800100,
  "callsign": "N0CALL",
  "defaults": {"cc": 1, "tg": "*"},
  "devices": [
    {"name": "w12", "kind": "sdr", "driver": "lr2021",
     "rx": {"rate": 240000, "center_hz": 438.5},
     "tx": {"power_dbm": 10}}
  ],
  "frequencies": [438.475, 438.5, 438.525],
  "apps": [
    {"type": "parrot", "tg": 9990}
  ],
  "networks": [
    {"type": "hblink", "host": "hb.example.net", "port": 62031},
    {"type": "mqtt", "host": "broker.example.net"}
  ]
}
```

- `"driver": "lr2021"` is required: the default `rtlsdr` can't transmit.
- The LR2021 is half duplex: no duplex repeater on a W12.
- The W12's filter is for 902–928 MHz. At UHF it costs about 46 dB, so it hears only nearby radios
  ([reference.md](reference.md), "LR2021 and the Meshnology W12").
- More (location, the IP gateway, the bench): [config-v3.md §10.5](spec/fw/config-v3.md#105-a-w12).

### A hotspot on BrandMeister

An MMDVM_HS hat on a Raspberry Pi, simplex on 438.5 MHz, logged in to BrandMeister as a hotspot.

```json
{
  "version": 3,
  "name": "hotspot",
  "callsign": "N0CALL",
  "defaults": {"cc": 1, "tg": "*"},
  "devices": [
    {"name": "hat", "kind": "modem", "type": "mmdvm_hs", "port": {"path": "/dev/ttyAMA0"}}
  ],
  "frequencies": [
    {"name": "hs", "freq": 438.5, "ts": 2}
  ],
  "networks": [
    {"type": "brandmeister", "host": "3102.master.brandmeister.network",
     "password": "your-hotspot-password", "peer_ids": {"hs": 980010001}}
  ]
}
```

- `peer_ids`: the hotspot's ID, your DMR ID with two digits after it, registered on BrandMeister. `password` is the
  hotspot security password from SelfCare. `host` is your country's master.
- `callsign` is required; the top-level `id` isn't.
- `"tg": "*"` sends up whatever a radio keys; BrandMeister decides what comes down (static and dynamic talkgroups).
- On an ESP32 board, the port is `{"usb": "auto"}` or `{"uart": 1, "tx": 5, "rx": 6}` in place of `path`
  ([§4.3](spec/fw/config-v3.md#43-port)).
- A repeater on BrandMeister: `config/examples/brandmeister-mmdvm-duplex.json`. Other Homebrew masters (TGIF, a
  stock HBlink): `"type": "homebrew"` with the same keys ([§7.4](spec/fw/config-v3.md#74-homebrew)).

### A repeater

A duplex MMDVM modem as a repeater (in 438.25, out 438.75): TS1 carries every talkgroup, TS2 carries TG 3100 always
and 3101 to 3199 once a radio keys one.

```json
{
  "version": 3,
  "name": "repeater",
  "id": 9800100,
  "callsign": "N0CALL",
  "defaults": {"cc": 1},
  "devices": [
    {"name": "hat", "kind": "modem", "type": "mmdvm_hs", "port": {"path": "/dev/ttyAMA0"}}
  ],
  "frequencies": [
    {"name": "in", "freq": 438.25},
    {"name": "out", "freq": 438.75}
  ],
  "apps": [
    {"name": "R1", "type": "repeater", "in": "in", "out": "out",
     "slots": {"1": {"tg": "*"}, "2": {"tg": 3100, "ptt": "3101-3199"}}},
    {"type": "parrot", "tg": 9990}
  ],
  "networks": [
    {"type": "hblink", "host": "hb.example.net", "port": 62031},
    {"type": "mqtt", "host": "broker.example.net"}
  ]
}
```

- `in` is what the modem receives, `out` what it sends. A repeater slot doesn't take `defaults`' policy keys: give
  each slot its own.
- A simplex hat can't be a repeater; a radio in hotspot mode (`radio_hotspot`) can't either, except as half of a
  split (two radios, [§6.2](spec/fw/config-v3.md#62-repeater)).
- On a PlutoSDR the device is `{"name": "pluto", "kind": "sdr", "driver": "iio"}`: see
  `config/examples/standalone-pluto-duplex.json`.

### An IP gateway

Radios' DMR packet data (ARS, text, location, any UDP app) to the IP network, on a simplex hotspot with no master:
datagrams to private ID 9990 go to one server, and anything else through NAT.

```json
{
  "version": 3,
  "name": "hotspot",
  "id": 9800100,
  "callsign": "N0CALL",
  "defaults": {"cc": 1},
  "devices": [
    {"name": "hat", "kind": "modem", "type": "mmdvm_hs", "port": {"path": "/dev/ttyAMA0"}}
  ],
  "frequencies": [438.5],
  "apps": [
    {"type": "ipgw", "tg": 9991,
     "routes": [{"name": "ars", "private": 9990, "to": "192.0.2.20:4005"}],
     "nat": true}
  ]
}
```

- Radios send to the gateway's ID, 9991 here.
- NAT denies private ranges by default; `"nat": {"allow": [...], "deny": [...]}` changes that
  ([§6.3](spec/fw/config-v3.md#63-ipgw)).
- On a host, ping through NAT needs a raw socket: run as root or give `rdofradio` `CAP_NET_RAW`.
- How it works and what's left: [ipgw.md](spec/fw/ipgw.md).

### A Tier III site

A DMR Tier III trunked site is a `tier3` app: a control channel and payload channels on one to four carriers. It runs
in `rdofradio` on a host or a PlutoSDR, not on an ESP32 board. Start from the `ham-tier3-*.json` files in
[config/examples/](../config/examples/README.md): a site on a PlutoSDR alone, linked over MQTT or on BrandMeister, and
a one-carrier site on a duplex MMDVM_HS hat with an RTL-SDR ([raspberry-pi.md](raspberry-pi.md), "A Tier III site").
Every key: [tier3.md](spec/fw/tier3.md) §4.

More complete files, with the parrot, the IP gateway, `ntp` and `bench`, are in
[config/examples/](../config/examples/README.md).

## Check a file

```sh
python3 tools/config_check.py my.json
```

- It checks keys, types and ranges against `config/rdof-config.schema.json`, with the `jsonschema` Python package or,
  without it, Node.
- Names, references and what fits the platform are checked when the file is applied: a board refuses it with the
  reason (and keeps the old one); `rdofradio` says why and exits.

## Apply it

| Box | How |
|---|---|
| ESP32 board | the page's Settings tab: edit, Save and restart, or Import a file |
| ESP32 board, from a script | `curl -X POST --data-binary @my.json http://<board>/api/config`: stored, then a restart 1 s later; `400` with the reason when refused |
| Linux / macOS | `rdofradio <device flags> --allow-tx --config my.json`; with `--web`, the page's Settings tab rewrites the file and restarts |
| PlutoSDR | `./install.sh --config my.json`, or the page's Settings tab ([plutosdr.md](plutosdr.md)) |

On a board:

- `GET /api/config` returns the stored file (`?download` as a file). `GET /api/resolved` shows it as it runs: each
  frequency with its defaults filled in, each path's policy and peer, and which talkgroups go where
  ([§11](spec/fw/config-v3.md#11-the-resolved-view)). Check it after a change to `tg`.
- The board's own sections ride along: `wifi`, `ntp`, `oled`, `usb_hub`. Wi-Fi passwords are never exported; a
  network without `password` keeps the stored one, so GET, edit and POST is safe.
- A body is 16384 bytes at most.
- Endpoints: [interfaces-http.md](spec/fw/interfaces-http.md).
