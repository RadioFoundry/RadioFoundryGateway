# A Raspberry Pi with an MMDVM_HS hat

`rdofradio` runs a Pi with an MMDVM_HS hat as a hotspot or a repeater, with or without an RTL-SDR beside it. It
takes the place of MMDVMHost: the two can't share the hat.

| Hardware | What it runs | Start from |
|---|---|---|
| Simplex hat (MMDVM_HS_Hat, ZUMspot) | a hotspot | `config/examples/standalone-mmdvm-simplex.json` |
| Duplex hat (MMDVM_HS_Dual_Hat) | a repeater | `config/examples/standalone-mmdvm-duplex.json` |
| Either hat + an RTL-SDR | the same, and the RTL-SDR hears the input: levels, BER, the spectrum and the bench's receive jobs | `standalone-mmdvm-rtl-simplex.json`, `standalone-mmdvm-rtl-duplex.json` |
| Duplex hat + an RTL-SDR | a one-carrier DMR Tier III trunked site | `ham-tier3-hotspot-box.json` with `ham-tier3-hotspot-brandmeister.json` (section 8) |

Each also gets the web page (`--web`) and the bench API (`--bench`), and can log in to BrandMeister
(`brandmeister-mmdvm-duplex.json`) or an HBlink4 master ([config-v3.md](spec/fw/config-v3.md) §7).

Tested: a duplex MMDVM_HS hat and an RTL-SDR on a Debian 13 arm64 board, as a repeater, a Tier III site and a bench
line (transmit jobs, bursts and captures on air), running as the systemd service below. A Pi runs the same build. On a
Pi 3 or Zero 2 W, check the load with `top`; a lower SDR rate (`-r 960000`) costs less.

## 1. The OS

Use **Raspberry Pi OS Lite, 64-bit** (Debian 13). 32-bit images (Pi-Star, most WPSD installs) haven't been tried. If the
Pi runs one of those now, keep that card and start on a new one.

## 2. The hat's serial port

The hat is on the header's UART. On a Pi 3, 4 or Zero 2 W, Bluetooth has that UART until you move it:

```sh
echo dtoverlay=disable-bt | sudo tee -a /boot/firmware/config.txt
sudo raspi-config nonint do_serial_cons 1     # no login console on the port
sudo raspi-config nonint do_serial_hw 0       # the port itself on
sudo usermod -aG dialout $USER
sudo reboot
```

The hat is then `/dev/ttyAMA0`. On a Pi 5, add `dtparam=uart0=on` in place of `disable-bt`; it's `/dev/ttyAMA0`
too. MMDVM_HS hats run at 115200 baud, the default.

Stop anything else that opens the port: `sudo systemctl disable --now mmdvmhost` (and its gateways, if installed).

## 3. The RTL-SDR (if there is one)

```sh
sudo apt install rtl-sdr        # keeps the kernel's TV driver off the dongle; replug it after
rtl_test -t                     # finds it
rtl_test -p                     # a few minutes: the crystal's error in ppm, roughly
```

`rdofradio` doesn't measure the dongle's ppm: give it with `--ppm`. A dongle with a TCXO is close to 0. A known carrier
measures it better than `rtl_test`.

## 4. Build rdofradio

On the Pi:

```sh
sudo apt install build-essential cmake pkg-config git libsoapysdr-dev soapysdr-module-rtlsdr libmosquitto-dev
curl -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal && . ~/.cargo/env
git clone https://github.com/RadioFoundry/RadioFoundryGateway && cd RadioFoundryGateway
cmake -S . -B build/rel -DRDOF_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release -DRDOF_HOST_WEB=ON
cmake --build build/rel --target rdofradio -j2
```

- `-j2` fits a 1 GB Pi. Each step's needs: SoapySDR (`rdofradio` isn't built without it, even for a hat alone),
  libmosquitto (MQTT; optional), cargo (the Rust modules, the default; `-DRDOF_RUST=OFF` builds the C instead).
- The Rust is in when `find build/rel -name librdof_rust.a` finds it. With cargo missing, the configure step stops
  and lists what's missing.
- Or cross-build on an x86_64 machine with Docker: `tools/linux-arm64/build.sh` writes
  `build/linux-arm64/out/rdofradio`. The Pi then needs only `libsoapysdr0.8 soapysdr-module-rtlsdr libmosquitto1`.

## 5. The settings file

```sh
mkdir -p ~/rfg/bin && cp build/rel/rdofradio ~/rfg/bin/
cp config/examples/standalone-mmdvm-rtl-duplex.json ~/rfg/config.json     # or the one that fits, from the table
```

Edit `~/rfg/config.json` (`//` comments are fine):

- `id`, `callsign`, `defaults.cc`: yours.
- `frequencies`: the hotspot's frequency, or the repeater's `in` (what the hat receives, MMDVM.ini's
  `RXFrequency`) and `out` (`TXFrequency`).
- The modem device: `"type": "mmdvm_hs"` for any HS hat (the duplex example says `mmdvm`), and its calibration in
  an `mmdvm` block, from MMDVM.ini's `[Modem]` section:

  | MMDVM.ini | `mmdvm` key |
  |---|---|
  | `RXOffset`, `TXOffset` | `rx_offset_hz`, `tx_offset_hz` |
  | `RXLevel`, `TXLevel` | `rx_level`, `tx_level` |
  | `RXInvert`, `TXInvert`, `PTTInvert` | `rx_invert`, `tx_invert`, `ptt_invert` |
  | `TXDelay` | `tx_delay_ms` |
  | `RXDCOffset`, `TXDCOffset`, `DMRDelay` | `rx_dc_offset`, `tx_dc_offset`, `dmr_delay` |
  | `RFLevel` (percent) | `--rf-level` on the command line, 0..255 (50 % is about 128) |

  ```json
  {"name": "hat", "kind": "modem", "type": "mmdvm_hs", "port": {"path": "/dev/ttyAMA0"},
   "mmdvm": {"rx_offset_hz": -150, "tx_offset_hz": -175, "rx_level": 100, "tx_level": 50, "tx_delay_ms": 100}}
  ```
- `bench.tx`: where bench jobs may transmit. The examples allow every US amateur band at the hat's least power.
  Narrow it to your frequencies (`"freqs_hz": [438500000]`) and set `"rf_level"`. Remove `bench` to allow none.
- With an RTL-SDR: the repeater's `in` and `out` are both heard only if they fit in the SDR's window (`-r` wide,
  1.536 MHz at `-r 1536000`).

The file's other sections (networks, MQTT, the apps) are in [config-v3.md](spec/fw/config-v3.md), and
`python3 tools/config_check.py ~/rfg/config.json` checks a file.

## 6. Run it

First in the foreground, to see it start:

```sh
# the hat alone
~/rfg/bin/rdofradio --config ~/rfg/config.json --allow-tx --mmdvm /dev/ttyAMA0 --rf-level 128 \
    --web 0.0.0.0:8080 --bench 0.0.0.0:7373

# the hat and an RTL-SDR
~/rfg/bin/rdofradio --config ~/rfg/config.json --soapy driver=rtlsdr -r 1536000 --gain 30 --ppm 0 \
    --allow-tx --mmdvm /dev/ttyAMA0 --rf-level 128 --web 0.0.0.0:8080 --bench 0.0.0.0:7373
```

- The file has the radio (frequencies, color code, duplex); the command line has the devices. `--rx`, `--tx`, `--cc`,
  `--duplex` and `--parrot` are refused with `--config`. The RTL-SDR's rate, gain and ppm are taken from the command
  line, not the file.
- `--setting biastee=true` powers an active antenna from the RTL-SDR.
- The web page is at `http://<pi>:8080`. Saving settings there rewrites the file and `rdofradio` starts again with
  it.
- `--bench 0.0.0.0:7373` lets anyone on the network key the hat within `bench.tx`. `--bench 127.0.0.1:7373` keeps
  it to the Pi. Check the bench from another machine with `tools/bench/rfgbench.py --bench <pi> info`.
- On a duplex hat, a bench job stops the repeater for its length and brings it back. The bench's repeater jobs run
  DMR, P25 or NXDN on a duplex hat ([bench-api.md](spec/bench-api.md) 14.3).

## 7. Run it as a service

`tools/linux-arm64/` has a supervisor script and a systemd unit:

```sh
cp tools/linux-arm64/run.sh ~/rfg/
sudo cp tools/linux-arm64/rdofradio.service /etc/systemd/system/
sudo nano /etc/systemd/system/rdofradio.service     # User, Group and the /home/... path: yours
```

Put the device flags in `~/rfg/flags`, without `--config` (`run.sh` adds it). Lines starting with `#` are skipped:

```
# RTL-SDR: rate, gain, the dongle's ppm
--soapy driver=rtlsdr -r 1536000 --gain 30 --ppm 0
--allow-tx --mmdvm /dev/ttyAMA0 --rf-level 128
--bench 0.0.0.0:7373 --web 0.0.0.0:8080
```

```sh
sudo systemctl daemon-reload && sudo systemctl enable --now rdofradio
journalctl -u rdofradio -f
```

- It restarts 5 s after any exit, so a dongle that drops off USB comes back.
- Updates: the web page's Firmware update takes a new `rdofradio` (or its `.gz`). `run.sh` checks that it runs, keeps
  the old one as `rdofradio.prev`, and puts it back after 3 quick failures in a row.
- `touch ~/rfg/disabled` keeps it from starting, for example while MMDVMHost has the hat back.

## Without a settings file

The command line alone runs a hat too, with `--rx`, `--tx`, `--duplex half|full`, `--cc` and `--bench-tx` for the
bench's frequencies: [reference.md](reference.md), "An MMDVM modem on a serial port".

## 8. A Tier III site

A duplex hat and an RTL-SDR beside it run a one-carrier DMR Tier III trunked site
([tier3-hat.md](spec/fw/tier3-hat.md)): the hat transmits the carrier, and the RTL-SDR hears its out and its in, so
keep the two within about 1 MHz. It takes two files:

- `--config`: the box (the hat, its calibration from section 5, the clock, the bench). Start from
  `config/examples/ham-tier3-hotspot-box.json`.
- `--tier3`: the site, with its BrandMeister login. Start from `config/examples/ham-tier3-hotspot-brandmeister.json`;
  what each key is for is in [config/examples/README.md](../config/examples/README.md).

```sh
~/rfg/bin/rdofradio --soapy driver=rtlsdr -r 1536000 --gain 30 --ppm 0 --allow-tx --mmdvm /dev/ttyAMA0 \
    --config ~/rfg/config.json --tier3 ~/rfg/site.json --web 0.0.0.0:8080
```

As a service (section 7), put `--tier3 /home/<you>/rfg/site.json` in `~/rfg/flags`, with the full path: `run.sh`
doesn't expand `~`. The design and what's built: [tier3.md](spec/fw/tier3.md).
