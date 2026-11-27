# RadioFoundry Gateway

**English** · [简体中文](README.zh-CN.md)

**Open-source DMR Tier II and Tier III infrastructure.**

![The W12's Radios page: six channels on its LR2021, with DMR calls in the waterfall](docs/img/w12-spectrum.png)

## What it does

| Function | Description |
|---|---|
| **Tier III trunked site** | Open-source DMR Tier III site controller (ETSI TS 102 361-4) |
| **Tier II repeater and hotspot** | Duplex, simplex, split duplex, single-frequency repeater |
| **Multi-channel receiver** | Up to 16 channels at once |
| **Test bench** | Radio test and measurement over the network |
| **DMR-IP gateway** | Radio data to IP networks |
| **Networking** | BrandMeister, HBlink4, MQTT, site linking |
| **Timing** | Locked to UTC by GNSS or NTP |

## Runs on

- Raspberry Pi, Linux or macOS
- PlutoSDR or Pluto+
- ESP32-S3 boards, including the Meshnology W12

## Architecture

![Architecture: radios and time sources on the left, the gateway's interfaces and applications in the center, networks and clients on the right](docs/img/architecture.svg)

## Supported radios

| Radio | Receive | Transmit |
|---|---|---|
| RadioFoundry radio | 1 channel per radio | 1 channel per radio |
| MMDVM modem (hotspot or repeater) | 1 channel per modem | 1 channel per modem |
| OpenGD77 radio in hotspot mode | 1 channel per radio | 1 channel per radio |
| RTL-SDR | Up to 16 channels | No |
| LR2021 (W12) | Up to 6 channels | 1 channel |
| PlutoSDR or Pluto+ | Up to 15 channels | Up to 8 channels |
| SoapySDR devices | Up to 16 channels | Varies |

Use several radios or modems for several channels (up to 2 on an ESP32-S3, 16 on a computer), alongside SDRs.

## Tier III

- Dedicated or composite control channel
- Up to 4 carriers
- Group, individual, broadcast and emergency calls
- Status, short data and packet data
- Stun, revive, kill and ambient listening
- Multi-site roaming with a standby controller
- BrandMeister connection

Tested on air with RadioFoundry radios; other manufacturers' radios are untested. [Specification](docs/spec/fw/tier3.md)

## Test bench

- **Receive:** I/Q capture, decoded bursts, bit error rate, frequency, power ramps
- **Transmit:** FM, AM, DMR, P25, NXDN, dPMR, M17, D-STAR, I/Q
- **Calibration:** GNSS or NTP referenced

[Protocol](docs/spec/bench-api.md) · client: `tools/bench/rfgbench.py`

## Getting started

### Install the package

For a Raspberry Pi (any, Pi Zero included), Pi-Star 4.x, Debian 10 or later, or Ubuntu 20.04 or later, on 32-bit
ARM, 64-bit ARM or x86-64. This takes the latest release's package for this machine:

```sh
sudo apt install -y curl ca-certificates
URL=$(curl -fsSL https://api.github.com/repos/RadioFoundry/RadioFoundryGateway/releases/latest | grep -o "https://[^\"]*_$(dpkg --print-architecture)\.deb")
curl -fsSL -o /tmp/rdofradio.deb "$URL" && sudo apt install -y /tmp/rdofradio.deb
```

The same lines update it to a newer release.

It installs set up as a BrandMeister hotspot on an MMDVM hotspot board. Fill in the lines marked `SET` in
`/etc/rdofradio/config.json` (callsign, frequency, BrandMeister password and hotspot ID), then start it:

```sh
sudo systemctl enable --now rdofradio
```

The web page is on port 8080. On Pi-Star, run `rpi-rw` first, and stop MMDVMHost, which holds the modem.

### Other ways

| Hardware | Guide |
|---|---|
| Raspberry Pi with an MMDVM HAT | [raspberry-pi.md](docs/raspberry-pi.md) |
| PlutoSDR or Pluto+ | [plutosdr.md](docs/plutosdr.md) |
| ESP32-S3 board | [esp32-boards.md](docs/esp32-boards.md) |
| Linux or macOS | [linux-macos.md](docs/linux-macos.md) |

Configuration: [settings.md](docs/settings.md) and [examples](config/examples/README.md).

## Status

| Area | State |
|---|---|
| Tier III | On air, two linked sites and BrandMeister |
| Tier II repeater and hotspot | On air |
| SDR receive and transmit | On air |
| Test bench | Used by RadioFoundry's hardware-in-the-loop test suite |

Details: [reference.md](docs/reference.md).

## Documentation

- [Documentation index](docs/README.md)
- [Reference](docs/reference.md)
- [Specifications](docs/spec/fw/overview.md)

## Contributing

[Contributing](CONTRIBUTING.md) · [Security](SECURITY.md) · [Code of conduct](CODE_OF_CONDUCT.md)

## License

[MIT](LICENSE). Third-party code: [PROVENANCE.md](PROVENANCE.md).

## Acknowledgements

- ETSI: the DMR standards (TS 102 361)
- Semtech: the LR2021 datasheet
- Jonathan Naylor (G4KLX): MMDVM and its modem protocol
- HBlink4 and OpenGD77 projects
- RadioFoundry: DMR FEC, CACH, LC and CRC code
- [DSD](https://github.com/szechyjs/dsd) and [mbelib](https://github.com/szechyjs/mbelib): AMBE+2 tables
- [KISS FFT](https://github.com/mborgerding/kissfft), [Adafruit GFX](https://github.com/adafruit/Adafruit-GFX-Library) font
- Howard Hinnant: `days_from_civil`
- chrony and David L. Mills: NTP
- Realtek, Rafael Micro and [webrtlsdr](https://github.com/jtarrio/webrtlsdr): RTL-SDR register details
- Meshtastic and MeshCore: W12 board details
- Espressif: ESP-IDF and esp-dsp
- lr2021-sdr: initial discovery of the LR2021's raw I/Q mode
