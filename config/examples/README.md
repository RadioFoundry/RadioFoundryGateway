# Example settings

Settings files (version 3) to start from. The frequencies are amateur ones and the IDs placeholders: set your own,
with the color code. Shorter recipes and how to apply a file: [docs/settings.md](../../docs/settings.md). Every key:
[config-v3.md](../../docs/spec/fw/config-v3.md).

## Standalone: no network

Nothing in `networks`: no HBlink4 master, no MQTT broker. Two apps run:

- the parrot: a call to TG 9990 is played back to the radio;
- the IP gateway: IP packets as DMR data between radios and the box's own network, sent to ID 9991
  ([ipgw.md](../../docs/spec/fw/ipgw.md)). `"nat": true` lets radios reach IP hosts through the box; `routes` sends
  chosen datagrams to one host instead.

| File | Hardware | Mode |
|---|---|---|
| `standalone-mmdvm-simplex.json` | an MMDVM hotspot board (MMDVM_HS) | simplex, 438.5 MHz |
| `standalone-mmdvm-duplex.json` | a duplex MMDVM modem | a repeater, in 438.25 / out 438.75 MHz |
| `standalone-mmdvm-rtl-simplex.json` | an MMDVM hotspot board and an RTL-SDR | simplex; the RTL-SDR hears the frequency too |
| `standalone-mmdvm-rtl-duplex.json` | a duplex MMDVM modem and an RTL-SDR | a repeater; the RTL-SDR hears its input too |
| `standalone-pluto-simplex.json` | a PlutoSDR | simplex, half duplex |
| `standalone-pluto-duplex.json` | a PlutoSDR | a repeater, full duplex |

- A simplex box answers a call after it ends, on the frequency it was heard on. A repeater repeats what it hears and
  sends the apps' answers on its output.
- No talkgroups are needed: each app claims its own ID (the parrot 9990, the IP gateway 9991), and with no network
  nothing else is carried.

## BrandMeister

| File | Hardware | Mode |
|---|---|---|
| `brandmeister-mmdvm-duplex.json` | a duplex MMDVM modem | a repeater, in 438.25 / out 438.75 MHz, on BrandMeister |

One login. Set:

- `peer_ids`: the repeater's own ID, your DMR ID with two digits after it, registered on BrandMeister;
- `password`: the hotspot password set in BrandMeister's SelfCare;
- `callsign`, and `host`: your country's master.

A network carries no talkgroups unless its `tg` says so, so the repeater app has `"tg": "*"`: every talkgroup a radio
keys goes up, and BrandMeister decides what comes down (static and dynamic talkgroups, set in SelfCare or by keying
one). A private call to 9990 is BrandMeister's echo.

## Trunked: DMR Tier III

A `tier3` site ([tier3.md](../../docs/spec/fw/tier3.md)) set up the way every ham site should be, so a radio set up
once takes any of them:

| File | Hardware | Network |
|---|---|---|
| `ham-tier3-site-standalone.json` | a PlutoSDR, 1 carrier | none |
| `ham-tier3-site-linked.json` | a PlutoSDR, 2 carriers (`tx.carriers: 2`) | the MQTT network, to the operator's other sites |
| `ham-tier3-site-brandmeister.json` | a PlutoSDR, 2 carriers | BrandMeister: the site is one repeater there |
| `ham-tier3-hotspot-brandmeister.json` (with `ham-tier3-hotspot-box.json`) | a duplex MMDVM_HS hat and an RTL-SDR, 1 carrier | BrandMeister |

| Key | Value | Why |
|---|---|---|
| `system.model`, `system.net` | `huge`, `1` | One NET of 1024 site numbers for every ham site, so radios roam between regions |
| `system.site` | 0-1022, chosen with the neighbors | 1023 is left out so 4 x site + 4 stays within 4094 |
| `system.par` | `ab` | Radios are category A; any category is admitted |
| `defaults.cc` | `1` | Every carrier's color code |
| `carriers[].chan` | 4 x site + carrier (1-4) | No two sites announce one logical channel for different frequencies |
| `carriers[].tscc` | on carrier 1 | The control channel |
| `registration`, `auth` | `open`, `off` | Any licensed ham registers; no keys |
| `ms`, `private`, `tg` | `*` | Every radio, private call and talkgroup |
| `wide_tg` (linked) | `1-16776415` | Every BrandMeister talkgroup number is carried between the operator's sites |
| `wide_tg` (BrandMeister) | `1-8,10-3999,4001-16776415` | Every talkgroup but 9 (the repeater's own) and 4000 (the site keys it) |
| `static_tg` (linked) | `3100` | An example: a talkgroup the site always carries |

- Radios use their own DMR IDs and BrandMeister talkgroup numbers. The linked site links over the MQTT network
  (`calls`, `format: v3`; [mqtt-net.md](../../docs/spec/fw/mqtt-net.md)), so those talkgroups travel between one
  operator's sites. Set `host` to your broker.
- The BrandMeister site logs in as one repeater ([tier3.md](../../docs/spec/fw/tier3.md) §5.11): `peer_ids` names the
  `tier3` app. A talkgroup a radio registers to is keyed there so BrandMeister carries it, and TG 4000 drops it once
  no radio wants it; each talkgroup keeps one of the login's two slots (`ts1_tg` / `ts2_tg` pin some), so two
  calls at a time go through. A site can have both networks: a call that comes both ways plays once.
- The duplex hotspot is a one-carrier site on an MMDVM_HS dual hat ([tier3-hat.md](../../docs/spec/fw/tier3-hat.md)):
  the hat transmits, an RTL-SDR beside it hears the carrier's out and in (so keep them within about 1 MHz). It takes
  two files: `--config` the box (the hat, its calibration, the bench), `--tier3` the site.
- The site grants channels by logical number and broadcasts each one's frequencies (Chan_Freq): a radio that learns
  the plan from those needs none programmed. Grants with absolute frequencies aren't done yet.

## The clock and the bench

Each file ends with the same two sections:

- `ntp`: public servers that don't smear a leap second (NIST, USNO, Apple, Cloudflare; no pool). rdofradio keeps its
  own clock from them (netclock); `"clock": "os"` takes the system's instead.
- `bench`: the bench API ([bench-api.md](../../docs/spec/bench-api.md)) on `--bench PORT`. Its jobs may transmit
  anywhere in the US amateur bands from 6 m to 13 cm (`bench.tx.bands_hz`), each key-up 10 s at most, at the
  transmitter's least power. Add `"rf_level"` (0..255, an MMDVM modem's level) or `"max_dbm"` for more; narrow the
  bands, or remove `bench`, to transmit less. Without `--bench` the section does nothing.

A box receives for the bench only where it has an SDR: an MMDVM modem alone takes transmit jobs only, on its own band
(a hotspot board: 70 cm).

## Running one

With `rdofradio`; the device stays on the command line ([linux-macos.md](../../docs/linux-macos.md)):

```sh
rdofradio --allow-tx --mmdvm /dev/ttyAMA0 --config standalone-mmdvm-simplex.json --web 8080 --bench 7373
rdofradio --soapy driver=rtlsdr -r 288000 --allow-tx --mmdvm /dev/ttyAMA0 --config standalone-mmdvm-rtl-simplex.json --web 8080 --bench 7373
rdofradio --iio local: --allow-tx --config standalone-pluto-duplex.json --web 0.0.0.0:8080 --bench 7373
rdofradio --allow-tx --mmdvm /dev/ttyAMA0 --config brandmeister-mmdvm-duplex.json --web 8080
rdofradio --iio local: --allow-tx --config ham-tier3-site-linked.json --web 0.0.0.0:8080
rdofradio --soapy driver=rtlsdr -r 1536000 --allow-tx --mmdvm /dev/ttyAMA0 --config ham-tier3-hotspot-box.json --tier3 ham-tier3-hotspot-brandmeister.json --web 8080
```

- A Raspberry Pi with an MMDVM_HS hat, including the hat's calibration from MMDVM.ini and running as a service:
  [raspberry-pi.md](../../docs/raspberry-pi.md). A PlutoSDR: [plutosdr.md](../../docs/plutosdr.md).
- `--bench` listens on every interface unless given an address (`--bench 127.0.0.1:7373`): anyone who reaches the
  port can key the transmitter within `bench.tx`.
- With `"nat": true`, ping from a radio needs a raw socket: run as root or give the program `CAP_NET_RAW`; without it
  NAT carries UDP only.
- `host-tests/test_examples.py` runs each file as written on simulated hardware (ctest `rdofradio_e2e_examples`).
