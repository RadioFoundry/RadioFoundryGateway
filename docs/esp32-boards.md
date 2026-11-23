# ESP32-S3 boards

The firmware runs on an ESP32-S3 with PSRAM. Three boards have profiles:

| Board | Profile | What it runs |
|---|---|---|
| Meshnology W12 | `16mb` + `w12` | its built-in LR2021 as a multi-channel SDR that also transmits (half duplex); the OLED, a GNSS header, a battery |
| ESP32-S3 N16R8 (16 MB flash, 8 MB PSRAM) | `16mb` | an RTL-SDR, modems and radios on USB or UART, a USB hub |
| ESP32-S3 SuperMini (4 MB flash, 2 MB PSRAM) | `4mb` | a roamer or repeater on a modem or radio; no SDR |

The W12 is the one tested most. `16mb` and `4mb` aren't built in CI for now and may not build. What each runs, and
the limits: [reference.md](reference.md), "Firmware builds" and "Boards".

## 1. Install the tools

**ESP-IDF v5.5** (built with v5.5.4):

```sh
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.4 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3
```

Then in every shell that builds: `. ~/esp/esp-idf/export.sh`.

**Rust** (the core modules are Rust by default):

```sh
curl -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal && . ~/.cargo/env
cargo install espup --locked
espup install --targets esp32s3 --std
```

- Without Rust, add the `.c` overlay (below): the same modules, in C. A build that can't find the Rust toolchain stops,
  lists what's missing and names that overlay.
- The ESP-IDF components the build needs (USB host CDC-ACM, esp-dsp, mDNS) are fetched on the first build.

## 2. Build

From `esp_idf/`. Each profile has its own build directory:

```sh
cd esp_idf
# W12
idf.py -B ../build/w12 -D SDKCONFIG=../build/w12/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.16mb;sdkconfig.defaults.w12" build
# N16R8
idf.py -B ../build/16mb -D SDKCONFIG=../build/16mb/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.16mb" build
# SuperMini
idf.py -B ../build/4mb -D SDKCONFIG=../build/4mb/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.4mb" build
```

The image is `build/<dir>/radiofoundry_gateway.bin`, its ELF `radiofoundry_gateway.elf`.

Overlays go at the end of `SDKCONFIG_DEFAULTS`, in a new build directory:

| Overlay | What |
|---|---|
| `sdkconfig.defaults.c` | the C modules; no Rust toolchain needed |
| `sdkconfig.defaults.roamer`, `.sdr` | one role only |
| `sdkconfig.defaults.hub` | USB hub support on `4mb` (`16mb` has it) |
| `sdkconfig.defaults.dspfast` | `16mb` experiment: the receive path's hot code in IRAM |

For example the W12 without Rust:
`-D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.16mb;sdkconfig.defaults.w12;sdkconfig.defaults.c"`.

## 3. First flash, over USB

The first flash writes the bootloader, the partition table and the app. After that, update over Wi-Fi (section 6).

**W12.** One USB-C, the S3's own USB.

1. Plug it into the computer. Hold the top button (USER, GPIO0), tap the bottom one (RST), release USER: download
   mode. The port is `/dev/cu.usbmodem*` on macOS, `/dev/ttyACM*` on Linux.
2. To keep the factory firmware, read it out first:
   `esptool.py --chip esp32s3 -p <port> read_flash 0 0x1000000 w12-factory.bin`.
3. Flash:

   ```sh
   idf.py -B ../build/w12 -D SDKCONFIG=../build/w12/sdkconfig -p <port> flash
   ```
4. Tap RST to start it.

The firmware's console is on UART0, not the USB port: read the log on the web page. To flash over USB again, use the
same USER + RST steps.

**N16R8.** Two USB-C ports. The USB-UART one flashes and carries the console: the bridge resets the
board into download mode by itself. `idf.py ... -p <port> flash monitor`. The other port (OTG) is for the RTL-SDR or
the radio; many boards power it only when a solder jumper (often `USB-OTG`) is bridged.

**SuperMini.** One USB-C, the S3's own USB.

1. Plug it into the computer. Hold BOOT, tap RST, release BOOT.
2. `idf.py -B ../build/4mb -D SDKCONFIG=../build/4mb/sdkconfig -p <port> flash`, then tap RST.
3. Put an OTG adapter and the radio on the USB-C. The S3 doesn't switch VBUS: the 5 V must come from the adapter.
4. The console is on UART0 (TX GPIO43, RX GPIO44, 115200), through a 3.3 V USB-serial adapter.

## 4. First boot: Wi-Fi

With no Wi-Fi networks stored, or none joined within 20 s, the board opens an access point:

| Setting | Value |
|---|---|
| Network | `radiofoundry-XXXX` (`XXXX` from its MAC) |
| Password | `radiofoundry` |
| Page | <http://192.168.4.1/> |

1. Join it and open the page.
2. Network tab: add your Wi-Fi network. Up to 8, each with a priority; the board joins the best one in sight.
3. The board joins it, and the access point goes down. It comes back whenever no network is joined for 20 s.

Or from a script, while on its access point:

```sh
curl -X POST -d '{"ssid": "my-network", "password": "my-password"}' http://192.168.4.1/api/wifi/networks
```

On your network it answers as `<name>-xxxx.local` (mDNS): the settings' `name` and the last two bytes of its MAC.
A new board's name is `esp32`, so `esp32-xxxx.local`. Or find its address in your router's DHCP list.

The status LED:

| Color | Means |
|---|---|
| red | no Wi-Fi (also while it boots) |
| orange | on Wi-Fi, but a radio or SDR in the settings isn't working |
| green | on Wi-Fi, everything in the settings running |

## 5. Settings

A new board runs placeholder settings: a radio in hotspot mode on USB, and an RTL-SDR on five 438 MHz channels,
reporting to a sample address that goes nowhere. A W12 shows orange until it has its own.

1. Open the page's Settings tab.
2. Paste a file from [settings.md](settings.md) (the W12 example for a W12), with your ID, call sign, frequencies and
   servers.
3. Save and restart. A file the board refuses isn't stored; the page says why.

Or `curl -X POST --data-binary @my.json http://<board>/api/config`. Export (or `GET /api/config`) gives the stored
file back, without Wi-Fi passwords.

The page's tabs:

| Tab | What |
|---|---|
| Overview | what's wrong, the radios at a glance |
| Last heard | the calls, newest first |
| Radios | each radio and the SDR in full, the spectrum and waterfall |
| Network | Wi-Fi, the clock, HBlink4 and MQTT links |
| Settings | the settings file: edit, save, export, import |
| Logs & maintenance | the log, firmware update, restart, the crash report and core dump, the self-test |

The HTTP API has no login and no TLS: keep the board on a network you trust. Every endpoint:
[interfaces-http.md](spec/fw/interfaces-http.md).

## 6. Updates over Wi-Fi

The page's Logs & maintenance tab, Firmware update: pick `radiofoundry_gateway.bin`. Or:

```sh
curl -X POST --data-binary @../build/w12/radiofoundry_gateway.bin http://<board>/api/ota
```

- It answers, then restarts on the new image. It's done when `/api/status` `uptime_s` starts over.
- The settings and Wi-Fi networks are kept.
- There's no rollback: an image that doesn't start needs a USB flash.
- An update over Wi-Fi keeps the partition table; only a USB flash changes it.
- Keep each image's `.elf` (`build/<dir>/radiofoundry_gateway.elf`): a crash report needs the one that crashed.

## 7. Crashes

A crash (an exception, an abort, a task stuck for 30 s) saves a core dump and restarts. The Overview tab says so, and
`/api/status` `crash` names the image by the start of its ELF's SHA-256 (`elf`); `this_image` says whether it's the
one running.

```sh
curl -o dump.bin http://<board>/api/coredump
esp-coredump --chip esp32s3 info_corefile -t raw -c dump.bin radiofoundry_gateway.elf   # dbg_corefile: GDB
curl -X DELETE http://<board>/api/coredump
```

- `shasum -a 256 *.elf` finds the ELF whose hash starts with `elf`.
- Erase the dump once read: it stays until the next crash, so an old one looks like a new crash.
- More: [reference.md](reference.md), "Crashes".

## Next

- More than one radio, USB hubs, UART pins: [reference.md](reference.md), "More than one radio".
- The SDR's settings and CPU limits: [reference.md](reference.md), "The SDR role".
- The W12's LR2021 and its front end: [reference.md](reference.md), "LR2021 and the Meshnology W12".
- Hardware-in-the-loop tests against a running board: [reference.md](reference.md), "Hardware-in-the-loop tests".
