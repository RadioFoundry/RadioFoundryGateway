# Linux and macOS

Two host tools run the firmware's code on a PC or a Raspberry Pi:

| Tool | What it does | Needs |
|---|---|---|
| `rdofsdr` | receives: many channels from one SDR, or from a recording; prints each burst and call as JSON | SoapySDR for a live device; nothing for a file |
| `rdofradio` | a radio: a repeater or transceiver on an SDR that transmits, or on an MMDVM modem, or a DMR Tier III trunked site; the web page, the bench line, HBlink4, BrandMeister, MQTT | SoapySDR or libiio |

A Raspberry Pi with an MMDVM_HS HAT is its own guide: [raspberry-pi.md](raspberry-pi.md). A PlutoSDR running
`rdofradio` on its own ARM: [plutosdr.md](plutosdr.md).

## 1. Dependencies

**Debian, Ubuntu, Raspberry Pi OS:**

```sh
sudo apt install build-essential cmake pkg-config git libsoapysdr-dev soapysdr-module-rtlsdr libmosquitto-dev
sudo apt install libiio-dev libad9361-dev      # optional: rdofradio --iio, a PlutoSDR or AD9361 without SoapySDR
```

**macOS** (Homebrew):

```sh
brew install cmake soapysdr soapyrtlsdr mosquitto
```

**Rust**, both (the core modules are Rust by default):

```sh
curl -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal && . ~/.cargo/env
```

| Library | Without it |
|---|---|
| SoapySDR (and its module for your device: `soapysdr-module-rtlsdr`, Homebrew `soapyrtlsdr`) | `rdofsdr` reads files only; `rdofradio` isn't built unless libiio is there |
| libmosquitto | no MQTT in `rdofradio` |
| libiio, libad9361 | no `--iio`; a Pluto is still reached through SoapySDR (SoapyPlutoSDR; not in Homebrew) |
| cargo | the build stops and says so; `-DRDOF_RUST=OFF` builds the C modules instead |

## 2. Build

```sh
git clone https://github.com/RadioFoundry/RadioFoundryGateway && cd RadioFoundryGateway
cmake -S . -B build/rel -DRDOF_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release -DRDOF_HOST_WEB=ON
cmake --build build/rel -j4
```

- The configure step says which libraries it found ("SoapySDR found: ...").
- The tools are `build/rel/rdofsdr`, `build/rel/rdofradio` and `build/rel/rdofchz` (the Pluto's FPGA bring-up).
  `cmake --install build/rel --prefix ~/.local` copies them to `~/.local/bin`.
- `-DRDOF_HOST_WEB=ON` adds the web page (`--web`).
- A Pi with 1 GB: `-j2`. Cross-building for arm64 on an x86_64 machine with Docker: `tools/linux-arm64/build.sh`.
- The host tests: `cmake -S host-tests -B build/host && cmake --build build/host && ctest --test-dir build/host`
  ([reference.md](reference.md), "Host tests").

## 3. rdofsdr: receive many channels

From the recording in the repo (five channels at 288 kS/s around 438.1475 MHz, color code 2):

```sh
build/rel/rdofsdr -i esp_idf/main/selftest/clip.cu8 -r 288000 --center 438147500 \
    --ch A:438.0625 --ch E:438.2625:2
```

From an RTL-SDR:

```sh
build/rel/rdofsdr --soapy driver=rtlsdr -r 1536000 --gain 30 \
    --ch rptA:438.0625 --ch rptB:438.1125:1 --ch hs:438.5
```

- `--ch ID:MHZ[:CC[:SLOT]]`, repeated: a channel, its name, and optionally the color code and slot it takes.
- Each line starts with the channel's ID: `json` lines are the call JSON MQTT carries, `raw` lines each burst.
  `--bursts` adds a `burst` line per burst with its level and timing. A summary per channel (bursts, AFC) goes to
  stderr at the end.
- `-r` must be a multiple of 48000 the device runs at exactly. The window is `-r` wide; every channel must fit.
  `--center HZ` (in Hz) sets its center, else it's planned over the channels. A recording needs its own center
  given.
- `--ppm` corrects the dongle's crystal; `--setting biastee=true` powers an active antenna.
- `--seconds S` stops after S seconds; `rdofsdr --bench` times each stage on this machine.

| Device | Soapy module | `-r` | Status |
|---|---|---|---|
| RTL-SDR | SoapyRTLSDR | 1536000 | tested |
| PlutoSDR | SoapyPlutoSDR | 2400000 (2.4 MS/s or more) | tested |
| LimeSDR, HackRF, B200, bladeRF, SDRplay, Airspy Mini | their own | [reference.md](reference.md), "rdofsdr" | untested |

`rdofsdr` doesn't log in to HBlink4 or publish to MQTT. A receive-only site that does is an ESP32 board
([settings.md](settings.md), "An SDR monitor").

## 4. rdofradio

The SDR is `--soapy ARGS`, `--iio URI` or `--file RX[,TX]` (a recording, for runs with no radio); only `--mmdvm`
runs without one. Anything that transmits needs `--allow-tx`.

### Receive only

```sh
build/rel/rdofradio --soapy driver=rtlsdr -r 1536000 --gain 30 --listen --rx 438.25 --tx 438.75 --cc 1 \
    --web 0.0.0.0:8080 --bench 0.0.0.0:7373
```

The receiver hears `--rx` and `--tx` as channels, with the page (spectrum, last heard) and the bench line. Nothing
transmits: `--allow-tx`, `--mmdvm`, `--config` and the transmit flags are refused with `--listen`.

### An SDR that transmits

A PlutoSDR on USB, through SoapySDR, as a full-duplex repeater with the parrot on 9990:

```sh
build/rel/rdofradio --soapy driver=plutosdr -r 2400000 --allow-tx --rx 438.25 --tx 438.75 \
    --duplex full --cc 1 --gain 20 --tx-gain 65 --tx-offset 0.063 --parrot 9990 --web 8080
```

- `--duplex full` is the repeater, `half` the transceiver (it answers on the frequency it heard).
- Power: `--tx-power DBM` (at the antenna, from `--tx-max-dbm`, +7 dBm by default: a Pluto's nominal) or the raw
  `--tx-gain DB`. Without either, TX gain 0 (a Pluto's least).
- `--tx-offset 0.063` is the Pluto's transmit delay over USB at the default buffer size. `--monitor` measures it by
  hearing the outbound back.
- `--iio ip:<pluto address>` or `--iio usb:` in place of `--soapy` uses libiio directly.
- Timing, the rate, the transmit guard: [reference.md](reference.md), "rdofradio".

### With a settings file

The file has the radio (frequencies, color code, apps, networks); the command line has the device:

```sh
build/rel/rdofradio --soapy driver=plutosdr -r 2400000 --gain 20 --tx-gain 65 --tx-offset 0.063 \
    --allow-tx --config config/examples/standalone-pluto-duplex.json --web 0.0.0.0:8080
```

- `--rx`, `--tx`, `--cc`, `--duplex` and `--parrot` are refused with `--config`: they're in the file.
- It runs the file's one unit that transmits: a repeater, a transceiver on the SDR or the modem, or a `tier3` site.
  A file where nothing transmits is refused.
- With `--web`, the page's Settings tab rewrites the file and restarts `rdofradio` with the same command line.
- `--clock-state FILE` keeps the clock's learned rate across restarts; `--set-os-clock` (root) sets the system's.
- Writing the file: [settings.md](settings.md).

### An MMDVM modem

A hotspot or repeater board on a serial port, with an optional SDR beside it (receive only):

```sh
# a USB hotspot, simplex on 438.5
build/rel/rdofradio --allow-tx --mmdvm /dev/ttyACM0 --rx 438.5 --duplex half --cc 1 --web 8080
# a duplex board with its settings file, and an RTL-SDR hearing its input
build/rel/rdofradio --soapy driver=rtlsdr -r 1536000 --gain 30 --ppm 0 \
    --allow-tx --mmdvm /dev/ttyAMA0 --rf-level 128 --config my.json --web 0.0.0.0:8080
```

- The port: `/dev/ttyAMA0` or `/dev/ttyS0` (a HAT), `/dev/ttyACM0` or `/dev/ttyUSB0` (USB) on Linux,
  `/dev/cu.usbmodem*` on macOS. `--baud` if it isn't 115200.
- `--rf-level N` caps the modem's power, 0..255. `--rx-offset-hz`, `--tx-offset-hz`, `--rx-level`, `--tx-level` are
  its calibration (with `--config`: the device's `mmdvm` keys).
- `--duplex full` (the default) needs a duplex board and runs the repeater; `half` is the transceiver.
- The whole Pi setup, with the MMDVM.ini mapping: [raspberry-pi.md](raspberry-pi.md).

### A radio transmits, the SDR receives

`--split-tx PORT` makes a split-duplex repeater: a RadioFoundry radio in hotspot mode on PORT transmits, the SDR
receives. Host-tested, not yet run with a real radio: [reference.md](reference.md), "A radio transmits, the SDR
receives".

### A Tier III site

A `tier3` app in the settings file makes `rdofradio` a DMR Tier III trunked site ([tier3.md](spec/fw/tier3.md)).

- On an SDR that transmits: `--config` with the site, as any settings file. Start from
  `config/examples/ham-tier3-site-*.json` ([config/examples/README.md](../config/examples/README.md)).
- On a duplex MMDVM modem with an SDR hearing it: `--config` the host's settings, `--tier3` the site
  ([raspberry-pi.md](raspberry-pi.md), "A Tier III site").
- `--state-dir DIR` keeps the site's cache of settings from the network's manager.
- `--config FILE --sim-air N` runs the site with no radio: N simulated radios on a simulated air. `--air-ms N` adds
  N simulated radios beside the real ones on a device.
- It needs cargo: a build with `-DRDOF_TIER3=OFF`, or without cargo, has no trunked site.

## 5. The web page

`--web [ADDR:]PORT` serves the board's page: status, last heard, the spectrum and waterfall, settings, the log, an
update. It listens on 127.0.0.1 unless ADDR is given (`--web 0.0.0.0:8080` for the network). There's no login: open it
only on a network you trust. Needs `-DRDOF_HOST_WEB=ON`.

## 6. The bench line

`--bench [ADDR:]PORT` serves the bench API's line protocol to `tools/bench/rfgbench.py`: bursts, captures, triggers,
and transmit jobs. A modem and an RTL-SDR as a bench line:

```sh
build/rel/rdofradio --soapy driver=rtlsdr -r 2.4e6 --gain 30 --ppm 0 --allow-tx \
    --mmdvm /dev/ttyAMA0 --rx 438.5 --tx 438.5 --duplex half --cc 1 \
    --bench 0.0.0.0:7373 --bench-tx 438.5,430-440
```

- Besides DMR, `tx` and `rx` jobs take signal modes: analog FM and AM, raw 4FSK (several carriers in one job) and,
  to transmit only, the client's complex baseband (`iq`). `info` lists what the line runs. For example
  `rfgbench.py --bench <host> tx fm --freq 438.5 --pcm audio.wav` ([bench-api.md](spec/bench-api.md) §14.6).
- `--bench-tx` lists every frequency and band (MHz) a job may key on; without it (or a settings file's `bench.tx`),
  no transmit job runs.
- Anyone who reaches the port can key the transmitter within `--bench-tx`. `--bench 127.0.0.1:7373` keeps it local.
- Check it from another machine: `tools/bench/rfgbench.py --bench <host> info`. `rfgbench.py find` lists bench lines
  on the network (DNS-SD, through `avahi-publish-service` or macOS's `dns-sd`).
- `--ppm`: rdofradio doesn't measure the dongle's error, so the bench's frequency reports are off by it unless given.
- What each job does: [bench-api.md](spec/bench-api.md) §14; the flags: [reference.md](reference.md), "The bench's
  line protocol".

## 7. Run it as a service

A lost device (the modem's port gone, 5 s with no samples) ends `rdofradio` with exit status 1, so run it under a
supervisor. `tools/linux-arm64/run.sh` and `rdofradio.service` restart it 5 s after any exit and take updates from
the web page: [raspberry-pi.md](raspberry-pi.md), "Run it as a service".
