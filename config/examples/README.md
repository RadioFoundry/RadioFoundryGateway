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
```

- A Raspberry Pi with an MMDVM_HS hat, including the hat's calibration from MMDVM.ini and running as a service:
  [raspberry-pi.md](../../docs/raspberry-pi.md). A PlutoSDR: [plutosdr.md](../../docs/plutosdr.md).
- `--bench` listens on every interface unless given an address (`--bench 127.0.0.1:7373`): anyone who reaches the
  port can key the transmitter within `bench.tx`.
- With `"nat": true`, ping from a radio needs a raw socket: run as root or give the program `CAP_NET_RAW`; without it
  NAT carries UDP only.
- `host-tests/test_examples.py` runs each file as written on simulated hardware (ctest `rdofradio_e2e_examples`).
