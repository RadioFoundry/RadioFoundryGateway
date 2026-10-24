# RadioFoundry Gateway

DMR gateway firmware for the ESP32-S3: a roaming transceiver, a repeater and a multi-channel SDR receiver, linked
to an HBlink4 network and reporting calls over MQTT. One firmware (`esp_idf`) runs what a box config's `radios`
name for the board: an SDR receiver (an RTL-SDR on USB, 4 or 5 channels), a roamer or repeater (a radio in hotspot
mode over USB, `radio_hotspot`: RadioFoundry or OpenGD77; or an MMDVM_HS HAT on a UART), or one of each.
Ten-channel SDRs come later on an ESP32-P4 (High Speed USB), and eventually everything on an ESP32-S31. The core
is portable C11, so a Linux build can come later.

What the gateway puts out:

- **MQTT**: MMDVMHost-compatible call JSON (call start and end, network calls, `tx`), each burst as a 49-byte
  binary raw burst, and a retained status and `Receiver` message per radio ([docs/spec/fw/mqtt.md](spec/fw/mqtt.md)).
- **HBlink4**: a peer per radio (UDP, or SCTP over UDP for the roamer), the DMRC protocol, DMRD bursts both ways,
  DMRT / DMRK to route a call to a roamer ([docs/spec/fw/hblink.md](spec/fw/hblink.md)).
- **HTTP**: a web page for settings, status, Wi-Fi, the clock and OTA, a JSON API, and the bench API
  ([docs/spec/fw/interfaces-http.md](spec/fw/interfaces-http.md), [docs/spec/bench-api.md](spec/bench-api.md)).

The C is checked against golden fixtures committed in `host-tests/`: vectors for the pieces, and scenarios that
drive the transceiver and repeater on fake radios and record every byte to the modem, every packet to HBlink4 and
every JSON message, which the C must reproduce exactly.

The specs (what each part does: interfaces, wire formats, algorithms) are in [docs/spec/](spec/); start with
[overview.md](spec/fw/overview.md).

Status (2026-10-21): tested on the host; the firmware builds with ESP-IDF v5.5.4 for both profiles. The SDR role
runs on a SuperMini and an N16R8, with the simulated input and with an RTL-SDR; on 2026-10-21 a real radio's call
went from the dongle on the SuperMini end to end to a server subscribing to its MQTT topics ("The SDR role"
below). The roamer and repeater have no board run recorded here yet.

## Quick start

Host tests (CMake and a C compiler; cargo too, for the Rust build of the tests; Node for the setup pages' scripts):

```sh
cmake -S host-tests -B build/host && cmake --build build/host && ctest --test-dir build/host
```

The same from the top, with the system tests (`RADIOFOUNDRY_DIR` optional) and, with `-DRDOF_BUILD_TESTS=OFF`,
just the libraries and `rdofsdr` (`cmake --install` puts it in `bin/`):

```sh
cmake -S . -B build/linux -DRADIOFOUNDRY_DIR=/path/to/radiofoundry && cmake --build build/linux && ctest --test-dir build/linux
```

With SoapySDR installed (Debian / Ubuntu `libsoapysdr-dev`, Homebrew `soapysdr`) the build finds it and `rdofsdr`
also runs live from any SoapySDR device: `rdofsdr --soapy driver=rtlsdr -r 1536000 --ch C1:461.4625 ...
[--gain DB] [--setting biastee=true] [--seconds S]`. The device streams CF32 at the rate, tuned to the planned
center; `--ppm` stays the receiver's software correction. Checked against SoapySDR 0.8 and SoapyRTLSDR on Debian
aarch64 (2026-10-24). The device API plan, [docs/spec/fw/sdr-device-api.md](spec/fw/sdr-device-api.md), takes the
same model into the firmware's front ends.

Checked on macOS (arm64; Apple clang, and GCC 15 with `-DRDOF_SANITIZE=OFF -DTINYSCTP_SANITIZE=OFF`, as GCC there
has no sanitizer runtime) and Linux (Ubuntu 24.04 x86_64, GCC 13, LeakSanitizer included; Debian 13
aarch64 on a Raspberry Pi, GCC 14, with `-DRDOF_SANITIZE=OFF -DTINYSCTP_SANITIZE=OFF`: GCC's AddressSanitizer
won't start under the Pi kernel's 39-bit address space).

Firmware (ESP-IDF v5.5), from `esp_idf/`:

```sh
idf.py -B ../build/16mb -D SDKCONFIG=../build/16mb/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.16mb" build
idf.py -B ../build/16mb -D SDKCONFIG=../build/16mb/sdkconfig -p /dev/cu.usbserial-XXXX flash monitor
```

The `4mb` profile is the same with `4mb` in place of `16mb`; add `;sdkconfig.defaults.rust` to the defaults for
the Rust build. Against a running board (needs pytest and websockets; paho-mqtt for the MQTT burst fallback):

```sh
RDOF_BOARD=<board ip> python -m pytest -q hil-tests
```

Details: "Host tests", "Firmware: esp_idf" and "Flashing (SuperMini)" below.

## Layout

| Path | What |
|---|---|
| `common/` | `rdof_common`: portable C11, libc and libm only (no ESP-IDF, no POSIX). One `CMakeLists.txt` that is an ESP-IDF component when `ESP_PLATFORM` is set and a plain static library otherwise. Headers in `include/rdof/`. `src/radiofoundry/` holds the FEC, CACH, LC and CRC code from RadioFoundry (MIT). |
| `sdr/` | `rdof_sdr`: the SDR receive path in portable C11 on top of `common/` ("SDR core" below); `cli/rdofsdr.c`, a host tool. |
| `rtlsdr/` | the RTL2832U + R820T / R828D driver: portable register logic, and `esp/rtl_usb.c`, the ESP-IDF USB host glue. |
| `components/` | ESP-IDF components with nothing of the gateway in them, for any ESP-IDF project: `netclock` (a UTC clock disciplined by NTP or a browser) and `wifilist` (a prioritized list of Wi-Fi networks with a setup access point), each with its own README, Kconfig menu, NVS namespace, HTTP handlers and page section; `tinysctp` (SCTP over UDP, with PR-SCTP: the HBlink4 link's transport; its own README and Kconfig menu, no settings of its own); and `lr2021` (a Semtech LR2021 on SPI, from the datasheet; its own README). Each keeps its logic in plain C that the host tests build. |
| `rust/` | `rdof_rust`: modules of `common/` ported to Rust, linked in place of their C behind a Kconfig switch, `CONFIG_RDOF_RUST`; [rust/README.md](../rust/README.md). |
| `esp_idf/` | The ESP-IDF project for the S3 (`radiofoundry_gateway`): the SDR and the roamer / repeater as two roles of one app, each behind a Kconfig gate. `common/`, `sdr/`, `rtlsdr/` and `components/` come in through `EXTRA_COMPONENT_DIRS`. |
| `host-tests/` | Host build of `common/`, `sdr/` and `rtlsdr/` and their tests: the golden fixtures (`vectors.h`, `scenarios.h`, `fixtures/sdr-vectors/`, `fixtures/mod-vectors/`), fake radios (`fake_radio.c`); and the components' portable parts with their tests. |
| `systest/` | the system tests ([docs/spec/system-tests.md](spec/system-tests.md)): `radiosim` (a RadioFoundry radio as a process, built from a RadioFoundry checkout), `radiolink.c` (driving it in virtual time) and `test_radiosim.c` (the gateway's modem client against it). |
| `hil-tests/` | hardware-in-the-loop tests against a running board (`test_board.py`, pytest). |
| `tools/` | `bench/benchapi.py` (a bench API client, used by the HIL tests) and `hblink4_sctp_check.sh` (a roamer's SCTP link against a real HBlink4). |
| `docs/spec/` | the specs: the firmware's parts (`fw/`), the repeater, UTC slot timing, the bench API, the LR2021's raw I/Q. |
| `LICENSES/`, `PROVENANCE.md` | third-party license texts, and where each piece of copied or ported code came from. |

Why this way: every later target (P4, S31, and a Linux or host build) takes `common/` and `sdr/` as they are, and the host tests run
in seconds with no board. Platform code (USB, UART, I2C, Wi-Fi, sockets, NVS, HTTP, MQTT) stays in each firmware
project. Nothing in `common/` blocks or threads: the platform gives the modem client a port (write, wait for a
reply, a clock) and calls the transceiver's or repeater's `poll` every 20 ms.

`common/` modules:

| Module | What |
|---|---|
| `bits`, `fec`, `ambe` | Hamming, BPTC(196,96), embedded LC, trellis 3/4, rate 1, CRCs, RS(12,9), Golay(23,12), AMBE frames; the FEC and CACH codes are RadioFoundry's C (MIT) in `common/src/radiofoundry/` (`PROVENANCE.md`) |
| `layer2`, `pdu` | sync, slot type, EMB, CACH, burst builders; LC, CSBK, data headers and blocks |
| `mmdvm` | MMDVM protocol framing and the frame reader, config v1 / v2, replies, radio detection, SFR detection, radio_hotspot RSSI, regeneration, CSBK preambles |
| `modem` | the modem client (requests, callbacks), the radio: modem and fixed tuners, RSSI maps, profiles |
| `txvr` | the roaming transceiver |
| `repeater`, `duplex` | the duplex and single frequency repeater, burst regeneration, the CACH short LC |
| `split` | split duplex: two modems (a TX half and an RX half, RadioFoundry's 0xD6 extension) as one duplex modem, its clock filter and epoch |
| `hblink` | DMRC, DMRD, DMRT, DMRK, and the HBlink sink |
| `tracker` | the call tracker: MMDVMHost-style JSON, raw bursts, frames for HBlink4 |
| `netcalls`, `status` | network calls, `tx`, status and Receiver messages |
| `config`, `jsonp` | the settings: a box config in JSON resolved for one roamer or repeater; which radios a board runs (`run`) |
| `slotgrid` | the UTC slot grid, `slot_timing`, the gate, the sample clock |
| `rawburst` | the binary raw burst: version 1 (49 bytes), version 2 (62, with the UTC start and grid slot) |
| `usbmatch` | which radio a USB device goes to (VID / PID, serial, hub port) |
| `oled` | the frame, font and panel commands; the roamer's status screen |
| `oled_sdr` | the SDR's status screen and the `oled` settings (C only, no Rust port) |
| `ipgw`, `ipgwcfg` | the IP gateway: DMR packet data put together, relayed to UDP by route or NAT, replies sent back as packet data; the `ip_gateway` settings (C only, no Rust port) |
| `json` | a compact JSON writer (no spaces after `,` and `:`), byte for byte stable for the golden tests |

## Host tests

```sh
cmake -S host-tests -B build/host && cmake --build build/host && ctest --test-dir build/host
```

Offline, in seconds, with no Python: the golden vectors and scenarios are committed fixtures (`host-tests/vectors.h`,
`scenarios.h`, `fixtures/`). Six runs, with AddressSanitizer and UBSan (`-DRDOF_SANITIZE=OFF` to drop them): `rdof_tests` with every feature on
(11691 checks) and `rdof_tests_min` with every `RDOF_FEATURE_*` off (1903), then `test_netclock` (138),
`test_wifilist` (29), `lr2021` (37, the LR2021's command encodings) and `tinysctp_tests` (11808, mostly one a
message sent over its simulated links) for the components (their READMEs say what those cover). With cargo
installed, `rdof_tests_rust` too: `rdof_tests` with `rust/`'s modules in place of their C. `rdof_tests` covers:

- FEC, layer 2, PDUs, AMBE, the MMDVM protocol, HBlink4 packets and the OLED frame against golden vectors, with bit
  errors where there's correction;
- the call tracker and HBlink sink: 6 scenarios, every JSON, raw message, frame and DMRD compared;
- the roaming transceiver: 38 scenarios (`scenarios.h`): the
  call cycle, refusals, orphan bursts, a radio that refuses a frequency, CW ID, the idle-channel receiver,
  preemption, missed status replies at key-up and lost for good, the transmit tail, listen-before-talk and the
  reply to the call just heard, the radio check lead-in and answer window, TMS and confirmed data asking for a
  reply, scan (hop, hold, carrier first, interrupted by a call), a stream that times out without a terminator, a
  voice gap and underrun, an MMDVM_HS (header / terminator repeats, headers per call) and an MMDVM board on a fixed
  radio;
- the repeater: 35 scenarios, duplex, SFR and DCDM (two network calls at once, each with its slot's sync; RF on a
  slot heard and not repeated, the network kept off it; no DMR_START): wakeups, RF repeat with
  regeneration, late entry, CSBKs and data, the network per slot, RF priority, echo drop, a lone CSBK, hang and
  rekeying, DMR_LOST and timeouts, the short LC, CW ID, back to back calls, gap fill and a late underrun, new and
  repeated headers, the network mid-superframe, an MMDVM board, one slot only, lost status; SFR: the inbound
  slot, the outbound slot, refusal while RF is heard, RF mid-stream, RF busy NAKs, an old busy flag, TS1 outbound;
- split duplex (`test_split.c`): bring-up against two fake RadioFoundry halves, the epoch in the RX half's
  clock to the microsecond, refusals, the role matrix (any half offering RX with any offering TX; a modem without
  the extension as neither), a half going away, the clock filter; and the duplex repeater scenarios run again on a
  split, every frame each half gets compared with the golden scenario's;
- the jitter buffer (`test_jitter.c`): its measure, targets, memory, grow and shrink and the re-framing of moved
  bursts, then network calls through the transceiver and the repeater on a modeled air (one burst per 60 ms):
  steady, jittered, bursty after a stall, a late storm and lost bursts, auto against fixed, each checked for gaps,
  superframe positions, EMB and embedded LC, missing bursts and delay;
- the settings: 37 box configs (15 that run, 22 that must be refused) resolved as the fixtures say, SFR descriptions, the status
  and Receiver messages; the `wifi` and `ntp` keys moved out of and back into the settings text with its comments;
  the `hblink.sctp_*` keys; which radios a board runs (`run`, and without it), and the roamer `run` picks;
- two radios a board: each parsed by name with its own port, ports that clash refused (a split's halves each
  counted), and which radio a USB device goes to (`rdof/usbmatch.h`).

The SDR's tests (`rdof_sdr_tests` and the rest) are under "SDR core" below; `ctest` runs them all.

`tinysctp` against usrsctp and a real HBlink4 (on macOS: `brew install libusrsctp`, and an HBlink4 checkout with
its `.venv`): `sh components/tinysctp/test/interop.sh` (an echo server on HBlink4's usrsctp transport: a clean
link, then 15% loss with duplicates, reordering and a 3 s outage, then the same with a 300 ms lifetime) and
`sh tools/hblink4_sctp_check.sh [--lossy]` (HBlink4 with SCTP on, a roamer's DMRC in, DMRP back).

The system tests (`systest/`, [docs/spec/system-tests.md](spec/system-tests.md)) run the gateway's code against
RadioFoundry's own: `radiosim` is RadioFoundry's MMDVM and hotspot core on its simulated modem, built from a
[RadioFoundry](https://github.com/RadioFoundry/RadioFoundry) checkout at the commit in `systest/radiofoundry.ref`.
Give the root build one and `ctest` runs them with the rest:

```sh
cmake -S . -B build/linux -DRADIOFOUNDRY_DIR=/path/to/radiofoundry && cmake --build build/linux && ctest --test-dir build/linux
```

Without `RADIOFOUNDRY_DIR` (a CMake option, or the environment) they're left out.

Coverage: `sh host-tests/coverage.sh [build dir] [file regex]` (clang; default `build/cov`) prints lines and
branches per file and writes an HTML report to `<build dir>/html`.

The ESP glue the host can't run (`web.c`, the bench API, `sdr_task.c`, netclock's and wifilist's HTTP handlers) has
hardware-in-the-loop checks against a running board, against the specs in
[docs/spec/fw/interfaces-http.md](spec/fw/interfaces-http.md) and [docs/spec/bench-api.md](spec/bench-api.md)
(pytest and websockets; paho-mqtt for the MQTT burst fallback):

```sh
RDOF_BOARD=<board ip> python -m pytest -q hil-tests
```

They read status, settings, the clock, Wi-Fi and the spectrum, open and end bench sessions and a receive lease,
and run short spectrum, burst and I/Q streams. Nothing transmits or changes settings. `RDOF_BOARD_MUTATE=1` adds
bad requests to the endpoints that store settings (each must be refused, with nothing stored and no restart).
They are skipped without `RDOF_BOARD`.

## Firmware: esp_idf

One ESP-IDF app for the S3 with two roles, chosen at run time from the settings: the **SDR** (an RTL-SDR on the
USB host, `sdr/` on 4 or 5 channels at 288 kS/s; "The SDR role" below) and the **roamer or repeater** (an MMDVM
modem: an OpenGD77 on USB, an MMDVM_HS on UART1; `common/`). One image to build, flash and update over the air, one
settings format (a box config), one page.

### One app, two roles

- **What runs** is what the box config's `radios` name for this board: the firmware key `run` (a name or a list,
  two roamers or repeaters and one SDR at most; "More than one radio" below), else the first roamer or repeater, else the first SDR. A plain
  SDR config on its own (no `radios`: "Settings" under "The SDR role") runs the SDR.
- **Both at once** works with the modem on a UART (`"modem": "uart"`, an MMDVM_HS HAT or a RadioFoundry radio's
  programming cable) and the RTL-SDR on the USB port, or with both on a powered USB hub (`"usb_hub": true` and a build with hub
  support: `16mb`, or the `hub` overlay). Without `usb_hub`, with both roles set to use USB, whichever device is
  plugged in runs (a USB client looks at its VID / PID and starts that role), and plugging in the other kind later
  restarts the board, which then runs that one.
- **CPU.** The roamer is light (a 20 ms poll, a few tasks at higher priority than the DSP helper); the SDR takes core
  1, and with `"dsp_cores": 2` some of core 0 for whole channels (their channelizer share and demod). Five busy
  channels with both roles has not been measured: run the self-test, and watch "DSP load" and the roamer's
  "without a reply" count with a call on each.
- **RAM.** Nothing of a role is allocated until it starts: no sample ring (256 KB, PSRAM), DSP buffers or esp-dsp
  tables, no transceiver or repeater (PSRAM). Static RAM for both is in the image (about 16 KB of `.bss` more than
  one role). The DSP's buffers go in internal RAM while `CONFIG_RDOF_SDR_INTERNAL_RESERVE_KB` (40) stays free for
  Wi-Fi, lwIP, TLS, the web server and the roamer, then in PSRAM; with both roles the roamer starts first.
- **Shared:** settings (NVS, the `wifi` / `ntp` keys), Wi-Fi (`wifilist`), the clock (`netclock`), the web server
  and its page (each role's section shown when it runs; the SDR's tests whenever the build has the SDR), OTA, the
  log ring, one USB host (`usb_host.c`; each class driver is its own client and takes only its own VID / PID), and
  the MQTT connection and UDP socket setup (`net.c`). Each role keeps its own MQTT connection, since each has its
  own status topic as the last will; and its own HBlink4 code (the roamer's one peer, UDP or
  SCTP; the SDR's receive-only peer a channel, UDP). Calls and bursts go out through `net_mqtt_post`, a queue in
  PSRAM with a task of its own: esp-mqtt holds its lock through a whole connect attempt (up to 10 s), and a
  publish straight from the DSP stalled it that long each attempt while the broker was unreachable (the overnight
  soak lost 80 s of samples that way). QoS 0 while not connected is dropped; the SDR's status counts
  what was dropped (`mqtt_dropped`).

| File | What |
|---|---|
| `main.c`, `settings.c`, `wifi.c`, `web.c`, `net.c`, `usb_host.c`, `logring.c`, `crash.c` (the core dump: "Crashes" below), `status_led.c`, `oled_task.c` (`CONFIG_RDOF_OLED`) | the board |
| `roamer.c`, `radio_task.c`, `modem_port.c`, `usb_modem.c`, `uart_modem.c`, `roam_hblink.c`, `rx_path.c`, `roam_mqtt.c` | the roamers or repeaters (`CONFIG_RDOF_ROAMER`), a `roam_t` each |
| `sdr.c`, `sdr_task.c`, `dsp_esp.c`, `kbench.c`, `fir_s3.S`, `fe_rtlsdr.c`, `fe_sim.c`, `fe_lr2021.c` (`CONFIG_RDOF_SDR_LR2021`), `sdr_mqtt.c`, `sdr_hblink.c`, `selftest.c`, `selftest_core.c`, `selftest/` | the SDR (`CONFIG_RDOF_SDR`) |
| `modem_sdr.c` | an `sdr_transceiver`'s modem port, the SDR (with either role) |

### Toolchain

ESP-IDF v5.5 (built with v5.5.4). In each shell:

```sh
. $IDF_PATH/export.sh        # IDF_PATH: your ESP-IDF v5.5 checkout
cd esp_idf
```

The build directories below are under the repository's `build/`.

The USB host CDC-ACM driver (`espressif/usb_host_cdc_acm` 2.4.1) and esp-dsp (`espressif/esp-dsp` 1.8.2) are
fetched on the first build; `dependencies.lock` pins them. Both are fetched for every build (the component
manager's Kconfig rules need options a role that's off doesn't have); a role that's off leaves its library built
but not linked.

### Profiles

Each profile is `sdkconfig.defaults` plus an overlay, with its own partition table and build directory:

| Profile | Board | Partitions | Features |
|---|---|---|---|
| `4mb` | ESP32-S3 SuperMini (ESP32-S3FH4R2: 4 MB flash, 2 MB quad PSRAM) | `partitions-4mb.csv`: two 1.875 MB OTA slots, a 128 KB core dump | everything but MQTT TLS; 16 KB log ring; Wi-Fi at 10 dBm at most (at 15 or more the SuperMini can brown out) |
| `16mb` | ESP32-S3 N16R8 boards (16 MB flash, 8 MB octal PSRAM) with a USB-UART bridge and an OTG port; a quad PSRAM part needs `CONFIG_SPIRAM_MODE_QUAD`. RGB LED on GPIO48. Internal RAM is the tight resource: mbedTLS buffers go in PSRAM and the DSP leaves 64 KB internal free (`CONFIG_RDOF_SDR_INTERNAL_RESERVE_KB`). Measured on five busy channels: the first 40 KB of DSP buffers (FFT plans, channelizer state) in internal RAM save 5% of a core; the rest (channelizer output, trackers, demod history) cost nothing in PSRAM | `partitions-16mb.csv`: two 4 MB OTA slots, a 128 KB core dump, 7.75 MB data | everything, MQTT TLS, 256 KB log ring, USB hubs |

A role overlay on top makes a one-role image: `sdkconfig.defaults.roamer` (no SDR) or `sdkconfig.defaults.sdr` (no
roamer).

Board and option overlays go on top of a profile: `sdkconfig.defaults.w12` on `16mb` (the Meshnology W12: the
LR2021 front end, its pins, the RGB LED on GPIO46; "Front ends" below) and `sdkconfig.defaults.hub` on `4mb` (USB
hubs, which `16mb` has already; "More than one radio" above). For example
`idf.py -B ../build/w12 -D SDKCONFIG=../build/w12/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.16mb;sdkconfig.defaults.w12" build`.

`sdkconfig.defaults.rust` (`CONFIG_RDOF_RUST=y`) on top of a profile builds the Rust modules in place of their C
([rust/README.md](../rust/README.md)).

```sh
idf.py -B ../build/4mb -D SDKCONFIG=../build/4mb/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.4mb" build
idf.py -B ../build/16mb -D SDKCONFIG=../build/16mb/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.16mb" build
idf.py -B ../build/16mb-rust -D SDKCONFIG=../build/16mb-rust/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.16mb;sdkconfig.defaults.rust" build
idf.py -B ../build/4mb-roamer -D SDKCONFIG=../build/4mb-roamer/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.4mb;sdkconfig.defaults.roamer" build
idf.py -B ../build/4mb -D SDKCONFIG=../build/4mb/sdkconfig size
idf.py -B ../build/4mb -D SDKCONFIG=../build/4mb/sdkconfig menuconfig      # "RadioFoundry Gateway", "RadioFoundry Gateway common (common)", "RadioFoundry Gateway SDR (sdr)"
```

The image builds `-Os`; with `CONFIG_RDOF_SDR_FAST`, `sdr/`, esp-dsp and the DSP task's files build `-O2`
(per component in `CMakeLists.txt`, per file in `main/CMakeLists.txt`).

### Feature gates

Firmware options (`main/Kconfig.projbuild`, menu "RadioFoundry Gateway"):

| Option | Default | What it costs or saves |
|---|---|---|
| `CONFIG_RDOF_ROAMER` | y | the roamer and repeater: modem drivers, transceiver, repeater, SCTP (132 KB) |
| `CONFIG_RDOF_SDR` | y | the SDR: `sdr/`, the RTL-SDR driver, esp-dsp, the self-test (436 KB, the clip 338 KB of it) |
| `CONFIG_RDOF_MQTT`, `_RAW`, `_TLS` | y, y, n (y in `16mb`) | esp-mqtt; raw bursts on `.../raw`; TLS with the CA bundle |
| `CONFIG_MQTT_PROTOCOL_5` (esp-mqtt's) | y | MQTT 5 and its topic aliases, falling back to 3.1.1 (13 KB of flash; no static RAM) |
| `CONFIG_RDOF_WEB_FULL` | y | the full page (status, settings editor, export / import, log, OTA, the SDR's tests; 3.9 KB gzipped); off: a settings form only. Both pages carry the Wi-Fi and clock sections, which the components serve |
| `CONFIG_RDOF_OTA` | y | upload a new image from the web page; needs two OTA slots |
| `CONFIG_RDOF_SOFTAP` | y | the setup access point |
| `CONFIG_RDOF_GNSS_EN_GPIO`, `_LEVEL` | -1 (48, 0 in `w12`) | a GNSS receiver's power switch, driven on at boot; its pulse per second goes to netclock (`CONFIG_NETCLOCK_PPS_GPIO`, 41 in `w12`) |
| `CONFIG_GNSS_UART_RX_GPIO`, `_TX_GPIO`, `_NUM`, `CONFIG_GNSS_BAUD` | -1 (39, 38, 1, 9600 in `w12`) | the receiver's NMEA ([components/gnss](../components/gnss/README.md)): position and fix for the status, each second's time to netclock. Not started when a radio's modem uses that UART |
| `CONFIG_RDOF_STATUS_LED_GPIO` | -1 (48 in `4mb` and `16mb`, 46 in `w12`) | the status RGB LED (WS2812): red without Wi-Fi; with it, orange while a configured device isn't working (the SDR not open or not receiving, a radio's modem not ready; with both roles on one USB port, nothing plugged in), else green; -1 = none |
| `CONFIG_RDOF_LOG_RING_KB` | 16 (256 in `16mb`) | the log kept for the web page, in PSRAM when there is some; 0 = off |
| `CONFIG_RDOF_CRASH_TEST` | n | `POST /api/crash` crashes the board on purpose, to test the core dump ("Crashes" below); debug builds only |
| `CONFIG_RDOF_MODEM_USB` | y | the CDC-ACM driver for an OpenGD77 (VID / PID / interface settable) |
| `CONFIG_RDOF_MODEM_UART` | y | an MMDVM_HS or MMDVM board on a UART (UART1, or UART2 for a second radio) |
| `CONFIG_RDOF_HBLINK_SCTP` | y | the roamer's HBlink4 link over SCTP in UDP (`hblink.transport: sctp`, tinysctp): 16 KB of flash, 2 KB of RAM, and about 5 KB of heap when in use; off: UDP only |
| `CONFIG_RDOF_SCAN` | y | a roamer's scanning between calls |
| `CONFIG_RDOF_IPGW`, `_MAX_SOCKETS` | y, 8 | the IP gateway ("The IP gateway" below), and the flows (UDP sockets) it may hold; off unless the settings have `ip_gateway` |
| `CONFIG_RDOF_OLED` | y | the OLED status screen (I2C): the SDR's channels and calls, else the roamer's or repeater's state |
| `CONFIG_RDOF_OLED_BUILTIN`, `_SDA`, `_SCL`, `_POWER_GPIO`, `_POWER_LEVEL` | n, 8, 9, -1, 0 (y, 17, 18, 45, 0 in `w12`) | a board's own display: on with no `oled` key, its pins, its power switch |
| `CONFIG_RDOF_SDR_*` | | the SDR's: see "The SDR role" below |

The components have their own menus, "netclock", "wifilist" and "tinysctp (SCTP over UDP)" (their READMEs); the
app uses their defaults.

Core options (`common/Kconfig`, menu "RadioFoundry Gateway common (common)"), passed to `common/` as plain `RDOF_FEATURE_*` defines
(`include/rdof/features.h`; the host build tests all on and all off):

| Option | What |
|---|---|
| `CONFIG_RDOF_FEATURE_TX` | transmitting: the roaming transceiver, regeneration, CSBK preambles (off: a receive-only board that refuses tunes) |
| `CONFIG_RDOF_FEATURE_REPEATER` | the duplex and SFR repeater and the DCDM gateway (needs TX) |
| `CONFIG_RDOF_FEATURE_DATA_BLOCKS` | data block decode (trellis 3/4, rate 1, CRC-9, CRC-32) |
| `CONFIG_RDOF_FEATURE_TRACKER` | the call tracker and the HBlink sink (the SDR needs it) |

### Size

`idf.py size` after each build, against the partition plan. Keep this current per feature.

| Build (2026-10-09, ESP-IDF v5.5.4) | App .bin | App slot | Free in slot | DIRAM used (of 334 KB) | of it .bss |
|---|---|---|---|---|---|
| `4mb`, both roles (everything but MQTT TLS; the self-test clip) | 1515 KB (0x17AC60) | 1.875 MB | 405 KB (21%) | 155 KB (45.4%) | 55 KB |
| `16mb`, both roles (everything, MQTT TLS, 256 KB log) | 1608 KB (0x191F60) | 4 MB | 2.43 MB (61%) | 159 KB (46.5%) | 57 KB |
| `4mb` roamer only (`sdkconfig.defaults.roamer`) | 1079 KB (0x10DBB0) | 1.875 MB | 841 KB (44%) | 140 KB (41.0%) | 41 KB |
| `4mb` SDR only (`sdkconfig.defaults.sdr`) | 1383 KB (0x159C80) | 1.875 MB | 537 KB (28%) | 138 KB (40.5%) | 43 KB |
On 2026-10-20, after the front ends, the resampler, the LR2021 driver, the modulator and two radios a board
(.bin and DIRAM, against the same day's build before them):

| Build | App .bin | DIRAM used |
|---|---|---|
| `4mb` | 1583 KB (0x18BC90), +12.5 KB | 148 KB (44.3%), -7.7 KB |
| `16mb` | 1679 KB (0x1A3C90), +12.2 KB | 155 KB (46.6%), -7.7 KB |
| `16mb` + `w12` (the LR2021) | 1709 KB (0x1AB520) | 166 KB (49.7%) |
| `16mb` + `hub` | 1694 KB (0x1A7740) | 155 KB (46.6%) |

On 2026-10-24 the `16mb` + `w12` build is 1988 KB (0x1F1020) with 177 KB DIRAM used (53.1%), after the LR2021's
documented-modem API, the packet calls, the DMR receive setup, both patch RAM images (Semtech's 2.2 KB, ours 1.1 KB)
and the work of other sessions since 2026-10-20; the other builds weren't measured that day.

DIRAM went down because the roamer's state moved from `.bss` to the heap when it became a `roam_t`: the first
radio's is still internal (about 5 KB, allocated when it starts), its settings in PSRAM. The `w12` build's 11 KB
more DIRAM is the SPI master driver and the LR2021's state.

The roamer role costs 132 KB of flash and the SDR 436 KB (338 KB of it the self-test clip), against about 945 KB for
the board both share. The SDR only image is 64 KB smaller than with everything built `-O2`.

Everything, the self-test clip too, fits the 4 MB part with two OTA slots and about 400 KB to spare, so nothing is
gated off in `4mb`. If that changes, drop the self-test clip (`CONFIG_RDOF_SDR_SELFTEST`, 338 KB) first, then the
repeater, OLED or the full page, before giving up a slot or a role. IRAM shows 100% in `idf.py size`: that's
the fixed 16 KB vector / cache-miss region filling as usual. The transceiver (about 20 KB), the repeater (about 55
KB), the settings parser (under 65 KB, while parsing) and the log ring are allocated in PSRAM.

The components cost 55 KB of flash in all: netclock 20 KB (5 KB of it its page section), wifilist 12 KB (4 KB),
cJSON 4 KB, the rest Wi-Fi scanning and the temperature sensor driver; and 9 KB of internal RAM (netclock's clock
and NTP state, the scan results). Their page sections go with `CONFIG_NETCLOCK_HTTP` / `CONFIG_WIFILIST_HTTP`.
SCTP costs 16 KB of flash (tinysctp 14 KB, the rest the roamer's side and the settings) and 2 KB of static RAM (the
status JSON); an association takes about 5 KB of heap, and its task a 6 KB stack.

### The ESP32-S3 SuperMini

- **One USB-C, on the native USB (GPIO19 / 20), no UART bridge.** The firmware runs that port as the USB host to
  the radio or the RTL-SDR, so the console is on UART0: **TX GPIO43, RX GPIO44** (marked TX / RX), 115200. To see it, wire a
  3.3 V USB-serial adapter (adapter RX to TX, TX to RX, GND to GND). Without one, the web page shows the log.
- **Power.** Feed the board from its 5V pin. The S3 doesn't switch VBUS: the radio's or the dongle's 5 V on VBUS
  has to come from the OTG adapter. Check with a meter that VBUS at the device end reads 5 V before plugging in.
- **PSRAM** is quad SPI in the package; the boot log should say `Found 2MB PSRAM device` and `SPI SRAM memory
  test OK`.
- **Pins** the firmware uses by default: an MMDVM_HS on UART1 at TX GPIO5 / RX GPIO6 (`uart` setting), the OLED on
  SDA GPIO8 / SCL GPIO9 (`oled.sda` / `oled.scl`).

### Flashing (SuperMini)

1. Plug the board's USB-C into the computer (no OTG adapter).
2. Hold **BOOT**, tap **RST**, release BOOT: the ROM's download mode on the native USB. On macOS it shows up as
   `/dev/cu.usbmodem*` (`ls /dev/cu.usbmodem*`; on Linux, `/dev/ttyACM*`).
3. Flash:
   ```sh
   idf.py -B ../build/4mb -D SDKCONFIG=../build/4mb/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.4mb" \
       -p /dev/cu.usbmodem1101 flash
   ```
   (with the port `ls` showed). The first flash also writes the partition table and OTA data.
4. Tap **RST** if it stays in download mode. Unplug the computer and put the OTG adapter and the radio or dongle on
   the USB-C.
5. Console, through the serial adapter: `idf.py -p /dev/cu.usbserial-XXXX monitor` (Ctrl-] to quit).

Later updates can go over Wi-Fi: build, then upload `build/4mb/radiofoundry_gateway.bin` on the web page. The
settings carry over.

### More than one radio

`run` may name two roamers or repeaters (`RDOF_MAX_RADIOS`), with or without an SDR. Each is a `roam_t` (`app.h`):
its own modem and port, transceiver or repeater, tracker, HBlink4 peer and MQTT connection. The OLED shows the
first (or, with an SDR, the SDR's screen with the first one's TX row). The page shows each; `/api/status` has `roamer` (the first) and, with two, `roamers`.

**Ports.** Each radio takes its port from its own keys, else the box's: `modem` (`usb` or `uart`), `uart {tx, rx,
num}` and `speed` (the serial speed, 8N1, 115200 by default). UART1 is the first UART (`num` 1) and UART2 the
second; UART0 is the console. A RadioFoundry radio without USB runs its hotspot on the programming cable's UART at
115200 (3.3 V levels: check the cable). Radios can't share a UART or a pin (`rdof_config_check_ports`).

**A split** (a repeater with `radio.type: split`; [docs/spec/repeater.md](spec/repeater.md) and
[docs/spec/fw/radio.md](spec/fw/radio.md)) is one radio with two ports,
`radio.tx` and `radio.rx`, each with these keys. `split_port.c` runs each half on its usual transport and
`rdof_split_t` between them; the repeater sees one modem. At most one half on USB (it takes the radio's USB device
slot); the other on a UART. It adds a 4 KB task and the second half's transport task. Not run on a board yet.

**USB.** Two radios on USB need a hub, and each needs telling apart: `usb {serial}` (its USB serial number, as the
board's log prints it when it's plugged in) or `usb {port}` (where it's plugged in: the hub ports from the board
down, `"2"` or `"1.3"`). A watcher client in `usb_host.c` looks at each device as it arrives and gives it to the
radio it matches (`rdof/usbmatch.h`: VID / PID, then the serial and port where they're set, the radio that named
more winning; host-tested), and each radio's CDC-ACM driver opens only that address. A replug lands on the same
radio. One radio that names neither opens the first device of its VID / PID, as before. Settings refuse two USB
radios that could match the same device.

- **Hub support** is ESP-IDF's (`CONFIG_USB_HOST_HUBS_SUPPORTED`, off by default; on in `16mb`, and the `hub`
  overlay turns it on elsewhere; 15 KB of flash). In v5.5.4 it has no overcurrent handling, no port debounce and no
  remote wakeup, and the S3 supplies no VBUS: use a **powered hub**, and expect a replug rather than a recovery
  after a fault. A hub build runs with the RTL-SDR straight on the port as before; not run with a hub yet.
- **Host channels.** The S3 has 8; enumeration takes one, each open endpoint one. A hub takes 1, a RadioFoundry radio 3
  (CDC-ACM: notify, bulk in, bulk out), an RTL-SDR 1 or 2. Two radios and a hub fit (7); two radios, a hub and an
  RTL-SDR don't. Put the second radio on a UART instead.
- **Bandwidth.** Full Speed is about 1.2 MB/s for everything on the hub. The RTL-SDR at 288 kS/s takes 576 KB/s;
  a radio a few KB/s. A radio and an RTL-SDR on one hub fit; the RTL's "USB samples" line shows any shortfall.

**RAM.** Internal RAM is what's short: about 11 KB is free on the N16R8 with the SDR running. A radio needs about
45 to 50 KB of it as the first one is built: task stacks (radio 8 KB, rx 8 KB, modem_rx 6 KB, HBlink4 4 KB and
SCTP 6 KB, status 3 KB, usb_open 4 KB or uart_rx 3 KB), esp-mqtt's task (about 6 KB), and its buffers. So the
second radio's stacks, tracker, MQTT state and receive buffer go in PSRAM (`xTaskCreatePinnedToCoreWithCaps`,
`CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY`; a task with its stack in PSRAM mustn't write flash, and none of these
do), which leaves about 20 KB of internal RAM it still needs: its
esp-mqtt task, the UART driver or CDC-ACM buffers, lwIP's sockets. The first radio's are internal, as before.
Settings with two radios on a board without PSRAM are refused. The memory plan places each radio's state and
stacks at boot ([memory.md](spec/fw/memory.md)): the second radio's stacks go internal while the spare above
`CONFIG_RDOF_MEM_MARGIN_KB` holds them, else PSRAM; a radio the plan refuses doesn't start and says why on the page
(`radio_error`). Unplanned ("either" mode), a second radio that finds less than 20 KB of internal RAM free when it
starts doesn't. Two radios and an SDR on the N16R8 is not measured on a board yet.

### Settings

The settings are one box config in JSON (comments allowed), stored in NVS and edited on the web page (Settings:
edit, Save and restart, Export, Import). They're checked in full before they're saved; the page also shows them as
resolved. Every key is in [docs/spec/fw/config.md](spec/fw/config.md). The box keys: `name`, `location`,
`mqtt`, `hblink`, `cc`, `channels` (the plan), `radios`, `oled`. Then:

- `run`: the radios this board runs, a name or a list of names (two roamers or repeaters and one SDR at most). Without
  it, the first roamer or repeater runs, else the first SDR. SDR sections that don't run still tell a roamer which
  channels are `covered`.
- on any radio, `"enabled": false` (the default is true): the radio is left out as if it weren't in `radios`, and
  isn't checked. `run` may still name it. So two SDR sections, say a `sim` and an `rtlsdr` both in `run`, switch by
  flipping their `enabled`. A disabled SDR covers no channels.
- for the roamer or repeater, on the box or on each radio: `modem` (`usb` or `uart`; `usb` for an OpenGD77),
  `uart {tx, rx, num}` (`num` 1 or 2), `speed`; on a radio, `usb {serial, port}`;
- for the screen, whatever runs: `oled {sda, scl}`;
- `usb_hub`: `true` when the USB port has a hub, so a radio and an RTL-SDR on it both run ("More than one radio");
- in the SDR's section: `usb {transfers, transfer_kb}`, `dsp_cores`, `selftest`, `spike` ("The SDR role" below);
- and:

- `wifi`: `{"networks": [{"ssid", "priority", "hidden", "password", "was"}]}`, the Wi-Fi list (wifilist's format;
  `{"ssid", "password"}` is a list of one).
- `ntp`: `{"servers": "pool.ntp.org", "poll_s": 300}` (netclock's; a plain string is the servers).

Those two live in their components' NVS namespaces (`wifilist`, `netclock`), not in the stored box config: the page
and Export get them added to the JSON, and Save or Import hands them to the components (replacing the Wi-Fi list
and the NTP settings) and stores the rest. **Passwords never leave the board:** the exported `wifi` has none, and a
network without `password` keeps the one stored for its name (or for `was`, its name before a rename). So
exporting and importing on the same board keeps the passwords; on another board, add `"password"` to each
protected network (or set them in the Wi-Fi section). Stored settings in the older form, with `wifi {ssid,
password}` and `ntp` in the text, move into the components on the first boot (only if those have nothing stored
yet) and come out of the stored text.

The page's Wi-Fi and Clock sections edit the same things without a restart. A network list change applies at once;
an NTP server change on the next poll.

`hblink` takes `transport: "sctp"` (`CONFIG_RDOF_HBLINK_SCTP`): HBlink4 over SCTP in UDP, with `sctp_encap_port` (9899,
HBlink4's), `sctp_heartbeat_ms` (10000), `sctp_max_retransmit` (3) and `sctp_ttl_ms` (0), MMDVMHost's
`SCTPHeartbeat`, `SCTPMaxRetransmit` and `SCTPTTL` with their defaults. `sctp_ttl_ms` is a DMRD's lifetime: a burst
not acknowledged by then is given up (PR-SCTP) instead of holding up the ones behind it; 0 keeps every burst. DMRC
and DMRK are always reliable.

To set up a board, start from its default settings (Export on the page), or write the box config by hand, name the
radios it runs in `run`, and Import or paste it. An S3's SDR takes a `sample_rate` of 288000 or 240000.

With no Wi-Fi networks, or none reached within 20 s (at boot or after losing the connection), the board opens an
access point `radiofoundry-XXXX` (password `radiofoundry`) with the page at <http://192.168.4.1/>, while the station keeps
trying. A new board starts with a placeholder roamer (an OpenGD77 on USB, peer 9990301) and an SDR on five 461 MHz
channels at 288 kS/s, both on USB: whichever is plugged in runs. A one-role build starts with its one.

### The IP gateway

Optional, off unless the settings have `ip_gateway`: radios' IPv4 datagrams, sent as DMR packet data (ARS,
text messages, location, any app on UDP), relayed to the network, and the replies sent back to the radio. The
board puts each message back together from the bursts any of its receivers hear (the SDR's channels, each
roamer's or repeater's radio), checks it, and then:

- **routes** send it to a set address: a private call to an id or range, a talkgroup, a source radio, or all,
  to `host:port` (or `host`, keeping the datagram's port), as the UDP payload or the whole datagram;
- **NAT** sends any other UDP datagram to the address in its own header, from the board's address, with a local
  port per flow, if the destination is allowed (private ranges, the radios' 12.0.0.0/8 and 13.0.0.0/8, loopback,
  multicast and broadcast are denied by default).

It takes the full IPv4 header (SAP 4) and the compressed UDP/IPv4 header (SAP 3, the addresses rebuilt from the
DMR ids), packets chained in one transmission, and ping through NAT (echo, and ICMP errors for a flow rewritten
back to the radio); it routes TTL as a router does. Replies on a flow come back to the radio as unconfirmed packet
data, from the address it sent to, through a roamer (a hotspot, or an `sdr_transceiver` half duplex) or a repeater
(duplex on either slot, SFR, DCDM, or an `sdr_transceiver` full duplex) (`ip_gateway.tx`, else the first), on the
radio's frequency and slot, waiting for the slot when the repeater is busy; an SDR-only board relays but can't
answer. Confirmed data the gateway takes is ACKed. Radios that send off-net traffic to an IP gateway id take
`ip_gateway.id` there. Rate limits each way, a flow cap, an idle timeout and a size cap bound it; the counters
are in `GET /api/status` under `ip_gateway`.

```jsonc
"ip_gateway": {
  "routes": [{"name": "ars", "private": 9990, "to": "192.0.2.20:4005"}],
  "nat": true
}
```

Keys: [docs/spec/fw/config.md](spec/fw/config.md) §2.13; how it works and what it doesn't do yet:
[docs/spec/fw/ipgw.md](spec/fw/ipgw.md).

### Wi-Fi and the clock

`wifilist` ([components/wifilist/README.md](../components/wifilist/README.md)) keeps up to 8 networks with priorities. The board joins the highest
priority one in sight (the stronger signal among equals), falls back down the list when one fails, and scans again
when the connection drops. The page's Wi-Fi section shows the connection (network, RSSI, channel, IP), the list with
each network's signal, and lets you add, edit, delete and reorder; under "In sight and recent failures", the last
scan and the last 8 failures with the reason.

`netclock` ([components/netclock/README.md](../components/netclock/README.md)) keeps UTC from up to 6 NTP servers, chrony style: it learns the
crystal's rate (stored across restarts), slews offsets in, and drops outliers and falsetickers. Until NTP answers,
the page sends the browser's clock. Every timestamp the board writes comes from it: the `timestamp` and `at` fields
of the call, network call, `tx` and status JSON (Unix seconds), the tracker's call times, and the
OLED's clock. The page's Clock section shows the tracking summary, the servers table (offset, delay, jitter,
stratum, reach, root distance, which is selected), the servers and poll interval, "Set from this browser", and the
last 32 references with a residual plot. `/api/status` has `clock`: `ntp`, `browser` or `none`.

MQTT ([docs/spec/fw/mqtt.md](spec/fw/mqtt.md)): the roamer or repeater publishes `{root}/{name}/{app}/json`
(calls, network calls, the retained Receiver), `.../raw`, `.../tx`, and the retained `{root}/_sdr/{name}/status`
with `{"state": "offline"}` as the last will; `app` is `roaming` or `repeater`. The SDR's are under "The SDR role".
Each role, and each radio, has its own connection and last will.

`.../raw` carries each burst as 49 bytes of binary (`rdof/rawburst.h`; the layout is in
[docs/spec/fw/mqtt.md](spec/fw/mqtt.md), "Raw bursts"), as MMDVMHost's MQTTRaw sends too. An SDR with
`slot_timing` utc sends version 2 (62 bytes, the burst's UTC start and grid slot appended) while its clock is on the grid. The tracker hands each
sink the burst as fields (`on_raw_burst`) and builds the JSON only for a sink with `on_raw` (the host tests and the
self-test, which compare it with the golden output). `net_mqtt_post` takes a length, so binary goes through its queue
like the rest: the DSP and the radio's loop never wait on the network. Everything a role publishes goes through
that queue, status and the retained `Receiver` too, so its task is the only one publishing on the connection.

**MQTT 5 and topic aliases** (`net.c`, `CONFIG_MQTT_PROTOCOL_5`). A connection asks for MQTT 5; refused with
"unsupported protocol version", the queue's task stops the client and starts it again on 3.1.1. On each connect
the aliases go, in order, to every channel's `raw` topic, then their `json` (`net_mqtt_t.alias_topics`), up to the
broker's Topic Alias Maximum. esp-mqtt doesn't say what that maximum is, but refuses a publish property with an
alias over it, so the task finds it by trying (a binary search, the failures kept out of the log). A topic goes
once in full with its alias, then with an empty topic and the alias. Only QoS 0 uses them: esp-mqtt resends a
QoS 1 message from its outbox as first built, on whatever connection is up then, and has no way to take an alias
off it, so the `json` topics (QoS 1) don't use theirs. QoS 0 is never stored: sent at once or dropped. Aliases
start over at every connect and disconnect (`net_mqtt_t.gen`). esp-mqtt keeps a pointer to the publish property
until the next publish, so one that wasn't sent is cleared before anything else goes out. The one gap: a publish
built just as the connection drops and comes back can reach the new connection with an alias it doesn't know;
the broker then disconnects (protocol error), and the next connection starts over. On the wire, with the SDR's
17-byte topic, a burst is 70 bytes, 57 with its alias; it was 219 as JSON.

`/api/status` has the board (`name`, the box's; `version`, ESP-IDF's `git describe --always --tags --dirty` as of the
build's last CMake configure (a new commit reconfigures; `idf.py reconfigure` refreshes `-dirty`), and `idf`; Wi-Fi, clock, memory, `usb_either`, the SDR's test results), then a
`roamer` and an `sdr` object for the radios it runs. `/api/resolved` has each one's settings as resolved and the
SDR's tuner plan.

### Crashes

A panic (an exception, abort(), the interrupt watchdog) saves a core dump to the `coredump` partition (128 KB in
both profiles; ELF, every task) and reboots. The dump stays until the next crash or until it's erased, so it may
be from an earlier image. At boot the board logs it in one line (`/api/log`):

```
W (1018) crash: core dump in flash (44004 bytes, image 00390efb7): task dsp_helper, PC 0x4038c4b4, CacheError (71), EXCVADDR 0x938b8d90; GET /api/coredump
```

`/api/status` has the same as `crash` (`elf`, the start of the crashed image's ELF SHA-256; `this_image`; `task`,
`pc`, `exccause` and `cause`, `excvaddr`, `backtrace`, and abort()'s message as `details`), and the page shows it
with a link to the dump. `GET /api/coredump` downloads the dump, `DELETE /api/coredump` erases it (404 `no core
dump` after). The MQTT status doesn't carry it.

To decode one, find the ELF whose SHA-256 starts with `elf`. Keep each flashed or OTA'd build's ELF (say as
`build/ota-elfs/<commit>-<profile>.elf`; `shasum -a 256 build/ota-elfs/*.elf` finds it), then:

```sh
curl -o dump.bin http://<board>/api/coredump
esp-coredump --chip esp32s3 info_corefile -t raw -c dump.bin build/ota-elfs/<commit>-16mb.elf
curl -X DELETE http://<board>/api/coredump
```

That's every task's state and backtrace, with source lines (`dbg_corefile` instead of `info_corefile` opens GDB on
it). Measured on the 16mb board with both roles and the bench API: 25 tasks, 46 KB; each task is its TCB, the used
part of its stack and a register note, about 1.8 KB, so 128 KB holds about 70. A dump that doesn't fit isn't
written and the old one stays (the console says `Not enough space to save core dump!`): check `this_image`.
On `4mb` it costs about 19 KB of flash (the image 1,806,000 bytes on 2026-10-22, 160 KB free in its slot); the
partition is the 128 KB after `ota_1`, the end of the flash.

To test it end to end, build with `CONFIG_RDOF_CRASH_TEST=y` (never for a board in service): `POST /api/crash` stores to address
0 from a task `crash_test`, and after the reboot `crash` says `StoreProhibited` at `0x00000000`. The HIL suite does
that round trip with `RDOF_BOARD_MUTATE=1`. A board updated over the air keeps its old partition table (64 KB on
`16mb` before 2026-10-22, none on `4mb`) until it's flashed over USB.

### What to test first

The roamer or repeater here; the SDR under "The SDR role", then both:

1. Power up with the serial adapter on: PSRAM found, `USB host up`, `waiting for the radio (1FC9:0094)`.
2. Set Wi-Fi through the access point, then paste a box config (a roamer, `modem: usb`).
3. Plug in the OpenGD77 in hotspot mode (with 5 V on VBUS): `radio connected`, the CDC descriptor dump,
   `modem: OpenGD77_HS ...`, `tuned to ...` on the idle channel. If it connects but never answers, try
   `CONFIG_RDOF_USB_INTERFACE`.
4. Key a radio on the idle channel: start / end JSON in the log and on MQTT under the roamer's name, and HBlink4
   gets the stream from this peer (DMRC first).
5. Have HBlink4 route a call to the roamer (a private call from another peer to a radio last heard on another
   channel): DMRT, retune, 0.3 s listen, DMRK "on air", the call out, `tx` and network-call messages, back to the
   idle channel. Watch "without a reply" in the status (missed status replies at key-up) and the on-air time.
6. A radio check from another peer: two preamble CSBKs, the check, and the 0.75 s wait for the answer.
7. Later: scan, CW ID, and the repeater modes on an MMDVM_HS dual hat or an SFR OpenGD77.
8. SCTP (`hblink.transport: "sctp"`): HBlink4 needs `sctp_enabled` and an SCTP backend that takes UDP
   encapsulation on `sctp_encap_port` (see "Scope"). The log says `SCTP association with ... up`, then the
   registration; the status shows the association, its RTT and what was resent or given up. Walk the board to the
   edge of Wi-Fi during a call: bursts should arrive late rather than not at all, or be given up after
   `sctp_ttl_ms`; a long dropout ends in `peer lost` and a reconnect within 10 s of Wi-Fi coming back.
9. Wi-Fi and the clock: an upgrade from older firmware moves the stored network and NTP server over (log:
   `migrated the single network`). Add a second network, switch the first off, and watch the board fall back and
   the failure show on the page; switch it on and force a reconnect. The Clock section should show the servers
   with reach 377 after 8 polls, the rate learned after about 10 minutes, and the stored rate after a restart.
10. Both roles. With the default settings (or a roamer on USB and an SDR in `run`): plug in the OpenGD77, and the
    log says `1FC9:0094: running the roamer / repeater`; plug in the dongle instead, and the board restarts and runs
    the SDR. Then an MMDVM_HS HAT on the UART with the dongle on USB (both
    names in `run`, `"modem": "uart"`): both run; key the HAT's idle channel and an SDR channel at once, and check the SDR's DSP load, its
    `droppedChunks`, the roamer's "without a reply" count, and internal RAM free on the page.

## Scope

What the roamer, repeater and their settings do, and the limits:

| Part | Status |
|---|---|
| DMR FEC, PDUs, AMBE frames, layer 2 | in; talker alias text, LRRP / GPS and packet data decoding aren't (a server subscribing to the raw bursts can do those) |
| MMDVM protocol: Modem requests, config v1 / v2, SET_FREQ, CW ID, duplex commands | in |
| MMDVM over UDP (an OpenGD77 as a USB network adapter) | not supported: needs USB CDC-NCM host or a second network path; refused in the settings |
| Radios: detection, SFR detection, profiles, radio_hotspot RSSI, RSSI maps, modem and fixed tuners, settle time | in |
| rigctld and command tuners | not supported: they need a computer (CAT control through hamlib, a shell); refused in the settings |
| Bursts, receive, duplex regeneration | in |
| The roaming transceiver: DMRT / DMRK rules, listen-before-talk (`clear_s`, the reply to the call just heard), the jitter buffer (auto or a fixed size, [roamer.md](spec/fw/roamer.md) 3.9) and pacing by `space`, header / terminator repeats, orphans, CSBK lead-in, the answer window (`answer_s`, `answer_max_s`, `covered`), TMS / confirmed data replies, `idle_after_s`, CW ID, the transmit tail, missed status replies at key-up, the 2 s idle status (hotspot keepalive) | in; queues are bounded (256 bursts a call, 4 early streams of 16 bursts) |
| Scan (channels, dwell, hang, carrier first) | in, `CONFIG_RDOF_SCAN` |
| The repeater: duplex repeat, network per slot, RF priority, echo drop, hang, CW ID, short LC; SFR; dual capacity direct mode (`mode: dcdm`, two simplex calls at once on one frequency, on the SDR modem; no stock MMDVM / OpenGD77 firmware does DCDM yet) | in, `CONFIG_RDOF_FEATURE_REPEATER`; queues bounded (192 bursts a slot, 6 streams) |
| UTC slot timing: the grid, `slot_timing`, the gate, the sample clock | in (`common/src/slotgrid.c`); the gate on the board from netclock |
| Split duplex (two modems as one) | in (`common/src/split.c`; on the board `split_port.c` over two USB / UART halves). The board's SDR modem as a half: the core and host tests, not yet on the board |
| Network calls, `tx` messages, status every 10 s and on changes, Receiver, restart with backoff | in |
| Settings, defaults, validation | for a board's roamers or repeaters and its SDR, as JSON (comments allowed). Not: a standalone roaming JSON layout (use a box config), `${VAR}` from the environment (write the values in), checking SDR sections that don't run, ids unique across a box |
| HBlink4: DMRC (roamer and repeater), DMRD, DMRT, DMRK, the peer link over UDP, the HBlink sink | in |
| HBlink4 over SCTP | SCTP over UDP (RFC 6951; `components/tinysctp`, `CONFIG_RDOF_HBLINK_SCTP`), not kernel SCTP over IP: lwIP has no SCTP. One stream, PPID 0, ordered, as MMDVMHost. HBlink4 takes it with its usrsctp backend (`sctp_encap: udp`, on Linux too since HBlink4 cd6846b). The roamer runs it too |
| The call tracker | in |
| MQTT | esp-mqtt; TLS behind `CONFIG_RDOF_MQTT_TLS` |
| OLED | the board shows its own state (`CONFIG_RDOF_OLED`): the frame, font and panel commands |
| Radios a board | two roamers or repeaters and one SDR at most (`run`), as roles of one app; a settings change restarts it; the web page checks settings |
| The SDR receiver (channelizer, demod, receiver, AGC) | in (`sdr`); on the S3 at 288 kS/s, eight busy channels take 116% of one core, 93% of the wall clock on two ("CPU budget on the S3"); ten at 1.536 MS/s on an ESP32-P4, later |

Other notes: JSON timestamps are wrong until NTP (or a browser on the page) has set the clock. The HBlink4 peers
don't use HomeBrew's RPTL / RPTK / RPTC login but the DMRC protocol (a DMRC packet every 5 or 10 s, DMRP back).
Golay(20,8) and QR(16,7) decode from tables of their 256 and 128 codewords (`layer2.c`): a lookup when the word is
clean, else the first codeword within 3 or 2 bits, which is RadioFoundry's answer (the host tests check every
possible word against RadioFoundry's decoders); Golay(23,12) decodes by syndrome search instead of an 8 KB
syndrome table (same answers, far less memory). The OpenGD77 quirks are handled: the 0.5 s `TX_TAIL_S` after the
transmitting flag clears, a missed status reply ridden out for 2 s, the 2 s idle status that keeps it in hotspot
mode; the 1.2 s lead-in is fixed in its firmware.

## SDR core: sdr

The SDR receive path (I/Q input, a multi-channel channelizer, FM discriminator and matched filter, the 4FSK demod,
AGC, the per-channel call trackers) in portable C11: the S3's SDR role runs it at 288 kS/s ("The SDR role"
below), and an ESP32-P4 firmware to come (and later the S31) on ten. No P4 firmware yet: there's no board.

### Layout

`sdr/` is its own library (`rdof_sdr`, an ESP-IDF component `sdr` that requires `common`), not part of
`common/`. A roamer only build leaves it out, and the SDR code can grow without touching the files the roamer work
edits. Headers in `include/rdof/sdr/`.

| Module | What |
|---|---|
| `fft` | complex float FFT behind `rdof_fft_new` / `rdof_fft_run` / `rdof_fft_free`; built in: mixed radix 4, 2, 3, 5 (2048 at 1.536 MS/s, 384 at 288 kS/s, 3200 at 2.4 MS/s) |
| `iq` | cu8 (lookup table), cs8, cs16 to complex float |
| `channelizer` | overlap-save channelizer (N = 64 x decimation, hop 3N/4), the Kaiser filter, bin-shift fix, per-channel NCO; `plan_center` |
| `fm` | RRC taps, discriminator (a polynomial atan2), symmetric 81-tap matched filter, power |
| `demod` | squelch, sync search over the four families, 4FSK slicing, CACH slot, slot guess, voice flywheel with EMB checks; bursts carry position, power, level center (AFC) and deviation |
| `agc` | clip measurement (float or straight from cu8), the gain control law, level stats |
| `mod` | the transmit side: DMR 4FSK modulator, bursts to I/Q or shaped frequency ("Modulator" below) |
| `sdrmodem`, `loopback` | the SDR as an MMDVM modem for an `sdr_transceiver`, and its end-to-end check ("Transmit: the SDR as a radio") |
| `receiver` | one tuner: channelizer, a demod and a `common` call tracker per channel, AFC after each chunk |
| `cli/rdofsdr.c` | host tool: a cu8 recording (or, with SoapySDR, a live device: `--soapy`) in, bursts and tracker JSON out; `--bench` times each stage |

Switches (`include/rdof/sdr/features.h`, Kconfig menu "RadioFoundry Gateway SDR (sdr)"; all on by default):

| Switch | What |
|---|---|
| `RDOF_FEATURE_SDR` | the whole library; off, it builds empty |
| `RDOF_FEATURE_SDR_AGC` | the gain control law (measurement and level stats stay) |
| `RDOF_FEATURE_SDR_BUILTIN_FFT` | the built-in FFT; off when the firmware defines `rdof_fft_*` itself (esp-dsp) |
| `RDOF_FEATURE_SDR_RECEIVER` | the multi-channel receiver; needs `RDOF_FEATURE_TRACKER` |

`RDOF_SDR_FAST_ATAN2=0` swaps the polynomial atan2 for libm's.

Every per-sample loop is single precision: the P4 and S3 FPUs have no double, so double there is soft-float.
Double is left only per block, per burst or at setup (checked by cross-compiling for `rv32imafc`: about 23 KB of
code at -O2).

### Tests

`host-tests/sdr.cmake` (included from `host-tests/CMakeLists.txt`) adds `rdof_sdr_tests` (every switch on,
1816 checks) and `rdof_sdr_tests_min` (every `RDOF_FEATURE_*` off, the FFT supplied by the test through the hook,
1708 checks), both under ASan/UBSan, plus a `RDOF_FEATURE_SDR=0` build; `rtlsdr_tests` (the RTL-SDR driver and its USB gap filler against
a fake dongle that can fail any one transfer, checked against docs/spec/rtlsdr.md: 293 checks, and 23 for the gap filler), `sdr_selftest_tests` (the S3 firmware's self-test check on the host) and `sdrmodem_tests`
(the SDR modem's half and full duplex loopbacks). Same commands
as above. Besides what's listed below, `rdof_sdr_tests` checks the status and Receiver messages, the board settings
(a plain SDR config and a box config's SDR section against the golden resolved settings, refusals included), the window planner, and the FFT, FIR and allocator hooks.

`rdof_sdr_edge_tests` (1393 checks, about 10 s) takes the corners the golden vectors don't reach, on the receive path
alone (no modem):

- an allocator that runs out at each allocation in turn (channelizer, demod, receiver, FFT, spectrum, resampler,
  modulator, window planner, settings, the CPU bench): each failure is reported and nothing is left allocated;
- the idle gate on and off over a recording with bursts after a silent stretch (same bursts, same AFC, the empty
  channels skipped), and the same recording through the two-core split, with and without memory for the block store;
- `rdof_demod_skip` against feeding the same quiet samples, the squelch off, small feed sizes, the CPU profile with a
  clock, bursts built symbol by symbol (a deviation too small or too large, another color code, weak sync symbols, a
  broken slot type, a second sync inside a burst, a sync just under the variance floor), and two voice calls on
  both slots (older slot served first, calls cut short, a weaker sync that can't take a slot over);
- AFC: the 5 kHz limit, the step limit, muted bursts, the correction history; the ppm learning and the sample rate
  fit; `rdof_sdr_bench` on a clip with a few hundred bursts;
- the AGC against a plain reference version of its law, in the test, over 300 random runs (it agrees on every
  decision; with peaks falling for longer than its 256-entry window queue holds it may only be slower to raise the
  gain); settings the golden table doesn't cover (the board's own keys, limits, `receiver_ids` forms); status messages with nothing heard yet and in
  buffers too small.

`rdof_loopback_edge_tests` runs the modem loopback with its set-up running out of memory at each step.

The vectors are committed fixtures in `host-tests/fixtures/sdr-vectors/`: `sdr_vectors.h` and the inputs and float
outputs in `sdr-data/` (1.5 MB), from a reference receiver in float64. The inputs are simulated traffic (the voice
is random AMBE parameter bits, so no codec is needed), quantized to cu8 so the C reads the very floats the reference
did.

| What | Must match |
|---|---|
| IQ conversion, `plan_center`, AGC decisions on five scripted level sequences, clip measurement | exactly |
| 162 bursts in nine demod cases (BS both slots, MS, TS1, TS2, late entry, data, noise, offsets to 800 Hz, color code filter, noise only; chunks of 331 to 48000): position, type, voice index, slot, family, color code, error count, all 33 bytes | exactly |
| the receiver end to end at 288 kS/s (4 channels: voice with header, late entry through a repeater, data, an emergency call; AFC on): 44 tracker JSON / raw messages | byte for byte |
| FFT (sizes 1 to 3200) | 2e-6 of the largest bin (gets 7e-8) |
| RRC and channel filter design | 1e-12 (double); the float taps within 1 ulp |
| channelizer output, 1.536 MS/s and 288 kS/s, with NCO changes mid-stream | 5e-5 of the output RMS (gets 1.7e-5; the reference's own NCO phase is float32) |
| discriminator + RRC | 2 Hz (gets 0.67 Hz with the polynomial atan2, 0.0007 Hz with libm's) |
| burst power / level center and deviation / AFC at the end | 0.01 dB / 1 Hz / 1 Hz |

How the receiver handles a real dongle's faults (noise, a tuner off by several ppm, IQ imbalance and its image, lost
USB transfers, overload, a strong station between two channels) shaped three of its rules:
- AFC: each chunk moves a channel half the way to where its bursts say it should be, at most 300 Hz, both per
  0.1 s of the chunk's length (in proportion for a shorter chunk), and against the correction each burst's
  samples actually went through (the last 32 are kept per channel: the demod looks a chunk or two ahead). Adding a
  share of each burst's error to the correction instead lets a chunk jump the whole offset under a voice
  superframe, and at the S3's 14 ms chunks steps the correction two or three times inside one burst.
- The flywheel measures each voice burst's own level center (slice, then move the center by the mean slicing
  error, twice).
- A voice sync more than 15 dB weaker (over the sync) than the call a slot is following can't take the slot over,
  which is what the tuner's IQ image of a busy repeater does whenever the call's radio is between bursts.

The firmware's self-test runs the clip three more times with a fault put in (`selftest_core.c`, in integer steps
on the bytes): +4 kHz, noise to about 12 dB C/N, and overload x4. The expected message and burst counts must come
out, so the S3's own DSP (the int16 filter, the 6 x 64 FFT) is checked under faults; on the board the CRC came out
as expected too, noise included.

### Modulator

`mod.c` (`include/rdof/sdr/mod.h`) is the transmit side, for front ends that can transmit (LR2021, SX1255, half or
full duplex), written from ETSI TS 102 361-1 clause 10.2. Symbols +3 +1
-1 -3 (dibits 01 00 10 11) at 648 Hz a unit, 4800 a second, each an RRC pulse (alpha 0.2, 8 symbols): the
demod's matched filter, so the pair is raised-cosine end to end. The shaped frequency goes into a 32-bit phase
accumulator; sin / cos come from a 256-entry table and a second-order step (error under 4e-7). Float per sample,
doubles only in init and queue, every allocation through `rdof_sdr_alloc` (pulse table P x 9 floats, P = fs /
gcd(fs, 4800): 60 at 288 kS/s; the queue, 0.7 KB a segment).

- **Timing** is in output samples, so TX can be scheduled against the RX sample counter. A segment's start is
  the sample where its symbol 0 peaks; symbol k peaks at start + k x fs / 4800 (whole at every multiple of 4800;
  an exact fraction at other whole rates, such as 250 kS/s). The demod reports a burst at symbol 0's peak +
  `RDOF_FM_DELAY` (40 at 48 kS/s), plus the channelizer's N / 8 inputs (8 outputs) when it goes through one.
- **Keying**: from half a symbol before the first symbol to half after the last, plus a raised-cosine ramp each
  side when the segment asks (`RDOF_MOD_RAMP_UP` / `_DOWN`; 4 symbols, 0.83 ms, by default, up to 6). TDMA: one
  burst a slot with both ramps keys 29.2 ms of 30. Continuous (a repeater's both slots): CACH + burst every
  30 ms, ramping only at the ends. Nothing queued: zeros, the phase still running.
- **Streaming**: `rdof_mod_queue` / `rdof_mod_queue_burst` (in start order, before the output reaches the segment's
  lead-in, else -1: late), then `rdof_mod_iq` (I/Q) or `rdof_mod_freq` (Hz and envelope, for a chip with its own FM
  modulator) for any n. The output doesn't depend on how it's pulled.

`rdof_mod_tests` (golden vectors committed in `host-tests/fixtures/mod-vectors/`): the C against a reference
modulator at 48, 288, 96 (frequency output) and 250 kS/s (I/Q within 4e-6, frequency within 0.002 Hz); C modulator
to C demod, through the C channelizer at 288 kS/s (a repeater downlink 62.5 kHz up the band, 400 Hz off), clean
and with noise: every burst bit-exact at its predicted position; queueing just in time gives the same samples
as queueing everything first. Cost: 4.6 ns a sample on the M1 Pro (Release), about 1.3 ms of CPU per second at
288 kS/s. On the S3 the kbench (self-test) prints its cycles per sample; from the code, roughly 100 cycles (about
30 Mcycles/s at 288 kS/s, an eighth of a core).

### CPU budget

`rdofsdr --bench` (Release, no sanitizers: `cmake -S host-tests -B build/bench -DRDOF_SANITIZE=OFF
-DCMAKE_BUILD_TYPE=Release`), on simulated traffic, M1 Pro at an assumed 3.2 GHz, best of 9. Mcycles per
second of IQ; "busy" has every channel in a call the whole time, "idle" is noise only.

| Stage | 10 ch, 1.536 MS/s busy | idle | 5 ch, 288 kS/s busy | idle |
|---|---|---|---|---|
| cu8 to float | 2.7 | 2.8 | 0.5 | 0.5 |
| forward FFT (2048 / 384 points, 1000 a second) | 42.1 | 42.7 | 7.7 | 7.6 |
| per channel: bins x filter, 64-point IFFT, NCO | 12.2 | 11.7 | 6.6 | 6.6 |
| discriminator | 4.6 | 4.6 | 2.3 | 2.3 |
| RRC + power | 7.6 | 7.7 | 3.8 | 3.8 |
| squelch, sync search, slicing, flywheel | 21.8 | 2.9 | 9.7 | 1.4 |
| **total** | **91** | **72** | **31** | **22** |

The search without the squelch would cost 34 (10 ch) and 17 (5 ch): the squelch saves most of it while idle.

**ESP32-P4 estimate.** Assumptions: esp-dsp does a 1024-point fc32 FFT in about 126k cycles, so 2048 points in
about 277k (N log N); everything else costs P4 cycles in the ratio of esp-dsp's 1024-point FFT to the host's
(126k against 14.4k cycles, x8.7), which treats the P4 running plain C as no better per operation than its
hand-tuned FFT. 384 points isn't a power of two, so it runs on the built-in FFT on the P4 too. Two HP cores at
400 MHz: 800 Mcycles a second.

| P4, Mcycles per second | 10 ch, 1.536 MS/s busy | idle | 5 ch, 288 kS/s busy | idle |
|---|---|---|---|---|
| cu8 to float (table) | ~9 | ~9 | ~2 | ~2 |
| forward FFT | 277 (esp-dsp) | 277 | 67 (built in) | 66 |
| per channel | 106 | 102 | 57 | 57 |
| discriminator + RRC | 106 | 107 | 53 | 53 |
| search | 190 | 25 | 84 | 12 |
| **total** | **~690 (86% of both cores)** | **~520 (65%)** | **~265 (33%)** | **~190 (24%)** |

Verdict: 4 or 5 channels at 288 kS/s fit easily on one P4 core. Ten channels at 1.536 MS/s fit on paper with
both cores while most channels are idle, but with everything keyed at once there's no room left for the USB
host, the network and MQTT. Ten channels needs some of the optimizations below.

**ESP32-S3** (two LX7 cores at 240 MHz, 480 Mcycles; assume about the P4's cycles per operation): 4 or 5 channels
at 288 kS/s need about 190 to 265 Mcycles, 40 to 55% of both cores, so they fit with care. The harder limit is
USB: the S3 is Full Speed only. 288 kS/s of cu8 is 576 KB/s, under what FS bulk can carry; an RTL2832U at
Full Speed has since run on the S3 ("The SDR role" below). Ten channels at 1.536 MS/s (3 MB/s, and over 500
Mcycles) is out of reach.

**Optimizations, biggest first:**

1. **int16 front end.** The samples are 8-bit: an int16 FFT (esp-dsp's `dsps_fft2r_sc16`; check how far it uses
   the P4's PIE SIMD) could be 2 to 3 times faster than fc32, about 150 Mcycles saved at 1.536 MS/s.
2. **Gate the per-channel chain by power.** The discriminator and RRC run on every channel all the time (about
   106 Mcycles for 10). A cheap power gate on the channelizer output (mean |y|^2 per hop over the noise floor),
   with a short history ring so the first burst isn't lost, would skip them on idle channels.
3. **Search at the symbol rate first.** Correlate every 10th sample (not every 2nd) with a lower threshold, then
   refine around hits: about 5 times fewer correlations while the squelch is open (up to ~150 Mcycles busy).
   This changes the search, so it needs checking against the golden vectors.
4. **Fixed-point RRC** (int16 taps, PIE multiply-accumulate) for the 81-tap filter.
5. **Polyphase channelizer.** About 1.5 times fewer operations than overlap-save for the front end, but DMR's
   12.5 kHz raster doesn't sit on a 48 kHz filter-bank grid, so it needs an oversampled bank and the NCO; worth
   it only after 1.
6. Less overlap (hop 7N/8) cuts the FFTs by about 15%, but changes the output against the golden vectors.

Memory: about 470 KB of heap for 10 channels with 16384-sample chunks (most of it 20 KB of buffer per demod
and the 2048-point FFT), 1.1 MB with 131072-sample chunks. The P4 has 768 KB of internal RAM plus
PSRAM.

### What the P4 firmware will need

- **An RTL2832U USB host driver** on the P4's High Speed USB: vendor control requests for the demod registers,
  the R820T / R828D tuner over the RTL's I2C repeater, and bulk IN streaming at 3 MB/s with several transfers in
  flight into a PSRAM ring. `rtlsdr/` is that driver for the S3, written from the register documentation
  (docs/spec/rtlsdr.md); the P4 needs its USB glue and the 512-byte packets. Don't take code from `esp-rtl-sdr`
  (AGPL) or librtlsdr (GPLv2): either makes the firmware copyleft.
- **esp-dsp** for the FFT: build with `RDOF_FEATURE_SDR_BUILTIN_FFT=0` and implement `rdof_fft_*` on
  `dsps_fft2r_fc32` / `dsps_fft4r_fc32` (in place, then bit reversal; copy in from `in`). esp-dsp only does powers
  of two, so a 288 kS/s build (384 points) keeps the built-in FFT.
- **The ESP32-C6 Wi-Fi companion** (esp-hosted over SDIO): the P4 has no radio. MQTT and HBlink4 go over it.
- Tasks: the USB reader, the channelizer on one core and the channel demods split over both, and the network.
  The tracker output is the same JSON and DMRD as the S3's.
- Gain: `rdof_agc_*` driven by `rdof_agc_measure_cu8` on each USB transfer, and the samples scaled by the gain taken
  away so RSSI holds.

### SDR: not done

The status message (`rdof/sdr/status.h`, `{root}/_sdr/{name}/status` and each channel's Receiver), the AGC on a
source, the dropped-chunk count, re-anchoring stream time and one HBlink4 peer per channel are in: the JSON in
`sdr`, the rest in the S3 firmware (`sdr_task.c`, `sdr_mqtt.c`, `sdr_hblink.c`). Not:

- Calibration, gain scans, recording and a simulator on the board (the board learns its ppm instead: "The tuner's
  ppm, learned" below); of channel planning, only `plan_center` and the window planner, `rdof_plan_window`.
- cf32 input.

Numerics: the polynomial atan2 reads levels up to about 0.5 Hz higher than an exact atan2, and gives -pi for -0.0,
as atan2 does; float where a float64 receiver would use double (squelch sums, score variance); the NCO advanced per
block. The AGC's peak window is a 256-entry max-queue: if it ever fills, it keeps the larger peak a little longer,
so the gain goes up later, never sooner. Level stats round in binary, not decimal.

## The SDR role (esp_idf with an RTL-SDR)

An RTL-SDR (RTL2832U with an R820T / R820T2 / R828D) on the S3's USB-C, and the SDR core above on 4 or 5
channels at 288 kS/s. It publishes each channel's calls as MMDVMHost-compatible JSON and raw bursts on MQTT, with a
status and a Receiver message per channel, and feeds HBlink4 a receive-only peer per channel. Status (2026-10-21):
it runs on a SuperMini and an N16R8, with the simulated input (`"sim": true`: the self-test clip in a loop, in real
time), where its output matches the expected, and with an RTL-SDR: on 2026-10-21 a real radio's call, received by
a dongle on the SuperMini, went end to end to a server subscribing to its MQTT topics.

| Path | What |
|---|---|
| `rtlsdr/` | `rtlsdr`: the RTL2832U + R820T / R828D driver. `src/` is portable register logic over one control-transfer hook (tested on the host); `esp/rtl_usb.c` is the ESP-IDF USB host glue: enumeration (on the app's USB host, or its own), descriptor dump, control transfers, bulk IN into a PSRAM ring, the throughput spike |
| `esp_idf/main/sdr*.c`, `dsp_esp.c`, `fir_s3.S`, `selftest*.c`, `kbench.c` | the role: settings, page section (with the spectrum and waterfall), MQTT, HBlink4, the DSP task, the S3's DSP kernels (esp-dsp's radix-4 FFT, the 384-point FFT as 6 x 64, the int16 SIMD matched filter), the self-test and its kernel timings |
| `esp_idf/main/selftest/` | the self-test clip (0.6 s, five busy channels at 288 kS/s) and its expected output (message and burst counts, CRC) |

The board's parts (Wi-Fi, the clock, settings storage, the page, OTA, the log, the USB host, MQTT setup) are the
app's, shared with the roamer ("Firmware: esp_idf" above).

### Why 288 kS/s

The S3's USB is Full Speed: 64-byte bulk packets, at most 19 a 1 ms frame, about 1.2 MB/s on the wire. cu8 at
1.536 MS/s needs 3 MB/s, so the ten-channel plan is out. 288 kS/s (576 KB/s) and 240 kS/s (480 KB/s) are the
RTL's lowest rates that are a multiple of 48 kS/s; they cover 230 or 192 kHz (80% of the rate). With the 25 kHz
DC guard, the 461 group's 12.5 kHz-step channels fit 4 at a time (the window planner picks which) or 5 with one
near DC; the first five (461.0375 to 461.2375) is the default.

Measured on the SuperMini (the spike, 2026-10-21): 240, 250, 288 and 300 kS/s arrive whole with every transfer
plan; 960 kS/s (1.92 MB/s asked) delivers 1152 KB/s, 60%, the same with every plan. 1152 KB/s is 18 of the 64-byte
packets in each 1 ms frame: the S3 host's ceiling with the RTL. So 300 kS/s (240 kHz usable) is the most this USB
carries: the RTL has no rates between 300 and 900 kS/s, and 900 kS/s and up need more than the ceiling.

The gap is the chip's, not the driver's. Tried 2026-10-21 by writing the whole resampler ratio (28.8 MHz x 2^22 /
rate) for 384 to 576 kS/s and measuring what arrived: 480, 528 and 576 kS/s came out at 232, 243 and 253 kS/s,
exactly this model of the register (28 bits, bit 27 standing for 2^28 too, so ratios 32 to 96 can't be
written); 384 and 432 kS/s, whose ratios need bit 28, gave about 2.6 MS/s and a stalled stream. Sampling at 900 kS/s
and decimating doesn't help either: the bytes cross the USB before any decimation, and the RTL only sends 8-bit
I/Q at the set rate. Nor does aliasing on purpose (letting channels outside the band fold in at known places): with
the DDC's FIR set to pass everything and the tuner's IF filter at 2 MHz, the dongle's own 460.8 MHz crystal harmonic
stood 14 to 24 dB over the noise in the band and nothing where it should fold to from 200 and 350 kHz out; the
RTL2832U's fixed decimation filters remove it. More than 300 kS/s needs High Speed USB (the P4).

The RTL drops samples silently when its FIFO overflows, so throughput is the risk. Drivers usually set the bulk
endpoint's FIFO to 512-byte packets; at Full Speed this driver sets 64. The spike measures what really arrives.

### The RTL-SDR driver: where its facts come from

Written from a fact spec, [docs/spec/rtlsdr.md](spec/rtlsdr.md), without librtlsdr or any other GPL driver:
`rtlsdr/src/rtl2832u.c` (the RTL2832U) and `rtlsdr/src/r820.c` with `include/rtlsdr/r820.h` (the R820T / R820T2 /
R828D). The spec cites, for each register write, Realtek's RTL2832U datasheet (Rev. 1.4: vendor requests, the
USB, system and GPIO registers, the IF and resampler formulas), Rafael's R820T2 register description (the tuner's
fields, the PLL formula, the VGA steps), jtarrio/webrtlsdr (Apache-2.0, `src/rtlsdr/` at 9b3f587: the init values
and sequences, the SDR-mode writes, the I2C framing, the RF filter tables, the PLL's rules, the gain scheme, the
R828D's inputs), and our bench findings above. Its section 3 lists what no source settles and how to measure it.
`rtlsdr/esp/rtl_usb.c` and `rtlsdr/src/gaps.c` are ours. Tests: `host-tests/test_rtl.c` against a fake dongle.

Where it differs from what was there before (not yet run on a dongle):
- **Tuning.** The PLL's fraction is rounded, not truncated, and the DDC's IF is set to the IF the PLL really gives
  (LO minus the center), so the center lands within the DDC's 6.9 Hz step instead of up to one PLL step (220 Hz at
  461 MHz) off. The sign of that correction is to be checked with a carrier (spec G.4).
- **IF filter.** The IF is 3.57 MHz at every rate, with webrtlsdr's filter setting (the narrowest coarse band, the
  calibrated fine code, HPF 11) also at 225 to 300 kS/s. It passes far more than the narrow rates need; a narrower
  setting waits for the sweep in spec G.2. The table it comes from takes the measured values directly.
- **Gain.** 31 manual steps, LNA and mixer codes alternating (webrtlsdr's scheme), the VGA fixed at 16 dB; the
  steps' dB are webrtlsdr's estimate (3.5 dB a full step, 2.3 the LNA's half), 0 to 52.5 dB. A gain setting in dB
  picks the step at or below it, which isn't the same codes as before; the board's AGC scales RSSI by these
  nominal values until measured ones replace them (spec G.3).
- **FIR.** The DDC's programmable low-pass is our own design (`tools/rtl_fir_design.py`): wider than the chip's
  usual default wherever it runs, flat over the narrow rates.
- **Blog V4.** The R828D's input switching and the HF upconverter on GPIO5 follow webrtlsdr.

### Front ends

The SDR role reads samples through one interface, `sdr/include/rdof/sdr/frontend.h` (portable; the drivers are
in `esp_idf/main/fe_*.c`). The SDR section's `device.type` picks one: `rtlsdr` (the default), `lr2021`,
`sim`, or `sx1255` (planned). `"sim": true` still means `sim`.

- **`rdof_fe_caps_t`** says what a front end is: its sample format (cu8, cs8, cs16; `rdof_fe_convert` makes cu8 for
  the DSP), its rates (a list or a range), tuning range, usable share of the band, DC guard, whether it has gain
  steps or sits on USB; and for transmit, rx-only / half / full duplex, the TX mode (I/Q, frequency, or native
  4FSK), TX rate and format, TX tuning and power range, and the RX/TX turnaround.
- **`rdof_fe_ops_t`**: init, wait (for a device), tune (rate, center, ppm; returns the real rate), set_ppm,
  set_gain / gain_steps, start, stop, read (with a count of bytes filled for gaps), gone, stats, identity, error,
  and optional post (after the DSP), spike and tx_set (transmit) hooks.
- **`rdof_fe_check`** checks a rate and the channels against the caps when settings are saved, with a message that
  says what the front end takes. The RTL-SDR is also checked when it tunes.
- **Resampler** (`rdof/sdr/resample.h`): a rational polyphase filter (Kaiser window, 32 taps a phase) for a front end
  whose rate isn't a multiple of 48 kS/s. Flat to 0.001 dB over the usable band, aliases under -60 dB (host tests).

| Type | Status |
|---|---|
| `rtlsdr` | the default (`fe_rtlsdr.c` over `rtl_usb.c`); status fields (`rtl_*`, `usb_*`) |
| `sim` | the self-test clip in a loop, paced in real time (`fe_sim.c`); transmits too, into its own samples |
| `lr2021` | raw I/Q receive written and built (`w12` overlay), **not run on a chip** (below) |
| `sx1255` | a plan (below) |

Provenance: the front-end interface, the drivers, the resampler (a textbook Kaiser-window polyphase design), the
LR2021 driver (Semtech's datasheet, DS.LR20xx Rev. 2.2, and for the raw I/Q path the facts in
`docs/spec/lr2021-iq.md`) and the USB matcher (the USB 2.0 specification's descriptors and hub ports) are written
here from those specs. Nothing is copied from RadioFoundry or from GPL sources; see [PROVENANCE.md](../PROVENANCE.md).

#### LR2021 (Semtech), and the Meshnology W12

`components/lr2021` is a plain ESP-IDF SPI driver for the chip; `fe_lr2021.c` is the front end. Semtech
documents no raw I/Q output. ROM 1.24 has one (an undocumented route into the RX FIFO), and its facts are written
down in `docs/spec/lr2021-iq.md`; the code here is ours, written from that spec and the datasheet. A receive session
(spec 2.2): check ROM 1.24 and no active patch, set the receiver up, route, map the 1 KB RX FIFO and check it, SetRx
continuous, then the gain step (SetRx forces step 13; spec R2.25). It runs in one of two modes, by the settings'
`sample_rate`:

- **Narrow, up to 120000:** the LoRa modem at 31.25 kHz bandwidth, int16 I/Q at 125 kS/s (source 2), resampled on
  the DSP core to the settings' rate.
- **Wide, 120001 to 576000:** the FSK modem with its receive bandwidth as the rate code (spec R2.19), and the
  route's own rate converter making the settings' rate exactly (source 0, int8 I/Q; R2.21). No resampler on the
  ESP. 576000 gives about 355 kHz of channels; 131 of 131 bench bursts decoded at 461 MHz.
- **Wide in int16, `device.settings.iq_bits` 16, 120001 to 240000:** the FSK modem's own rate (source 2, int16),
  resampled on the DSP core. About 50 dB more headroom than int8, for a box near strong radios; 240000 gives about
  77 kHz of channels (250 kS/s, 1 MB/s of SPI). 130 of 131 bench bursts. 288000 needs the SPI at 20 MHz (R2.29).

The reader task on core 0 sleeps on the FIFO IRQ (DIO7), reads by DMA, and in the wide mode reads by the FIFO's
level until it's nearly empty before waiting on the IRQ again. Each read is n − 2 bytes, n a multiple of 4, so the
SPI frame (2 status bytes and the samples) is a multiple of 4 and the IDF needs no DMA bounce buffer; a sample cut
in two by a read waits for the next (`fe_lr2021.c:drain`, `carry_n`). It gives other tasks waiting for the chip a tick
(the web server's lab requests stalled behind it). A loss, a retune or a new ppm ends the session (standby, reset,
set up again) and starts a new one: CalibFE overwrites the extended FIFO, so a session can't span a retune. In the
wide mode a loss only empties the FIFO (R2.28).

- **Why 576 k at most.** The FIFO can't grow past 1 KB (R2.22). At 768 k the reader lost about 30 FIFO-fulls a
  second at 16 MHz SPI; at 26.67 MHz it kept up but starved core 0. The DSP core is at about 0.93 with two
  channels at 576 k.
- **Dynamic range.** int8 is 6 bits in practice (it clips at ±32, R2.26): about 20 dB from the noise to clipping,
  so a strong radio anywhere in the window clips it. int16 has 50 dB more, at twice the SPI bytes.
- **DC.** Not measured: the DC guard is 5 kHz, so the planner keeps channels off the center.

`CONFIG_RDOF_SDR_LR2021` (off by default) builds it; "LR2021 pins and options" in menuconfig has the pins, which
default to the W12's: NSS 8, SCK 9, MOSI 10, MISO 11, NRESET 12, BUSY 13, IRQ (DIO7) 7, PA_EN 4, SPI at 16 MHz.
`sdkconfig.defaults.w12` (on top of `16mb`) turns it on with the RGB LED on GPIO46. Facts from the datasheet that
matter here:

- SPI is 16 MHz in the datasheet: 2 MB/s raw. 20 and 26.67 MHz read clean on the W12 (R2.23); 40 MHz breaks BUSY.
- **DMR 4FSK receive in the chip works, roughly.** The documented FSK modem is 2-level. ROM 1.24's
  undocumented 4-level variant receives DMR bursts with our patch RAM's correlator table (`lr2021_fsk4_dmr_rx`,
  `POST /api/lr2021/dmr_rx` on a held chip; IQ R3.40, R3.42), but at about 6% symbol errors against about 1% from a
  demodulator run offline on the chip's own I/Q of the same signal (R3.41), so the gateway receives from I/Q. DMR's RRC 4FSK for transmit is made
  by the S3 (`mod.c`) and sent as frequency samples (the raw TX path, spec 3).
- **One synthesizer**: half duplex only (RX to TX 107 us, TX to RX 91 us). A duplex repeater on one LR2021 isn't
  possible.
- LF path 150 to 960 MHz; LF PA -9.5 to +22 dBm in 0.5 dB steps.

**The W12's front end** (schematic W12-MB-V0.2). The antenna goes through a GC1109 front-end module and then a
902–928 MHz SAW filter to the LR2021's LF input. The GC1109 has an LNA (17 dB, NF 2 dB in its 860–930 MHz band), a
PA, and a transmit bypass; it has **no receive bypass**. DIO9, 10 and 11 drive its CTX, CPS and CSD: receive is
CTX 0, CSD 1; transmit is CTX 1, with CPS 1 for the PA (inside `CONFIG_RDOF_LR2021_PA_LO_MHZ` to `_HI_MHZ`) or 0
for the bypass. GPIO4 powers it. So the W12 receives well only in 902–928 MHz: elsewhere the SAW takes about 46 dB,
and at 461 MHz only strong signals decode (the noise there is the chip's own, after its gain; the gain step and RX
boost can't make it up). Transmit at 461 MHz through the bypass gives about +3 dBm.

The W12 also has: ESP32-S3R8 (8 MB octal PSRAM, so GPIO33 to 37 are taken), 16 MB flash, a 32 MHz crystal for the
LR2021 (no TCXO), native USB only, OLED on I2C SDA 17 / SCL 18, VEXT_EN 45, and an 8-pin header for an L76K GNSS
module (sold separately): UART RX 39 / TX 38, WAKE 40, PPS 41, RESET 42 (active low), power switch 48 (Meshnology's
pin table; the power switch's level, active low, from the Meshtastic port, unmerged). `w12` powers it, feeds its
PPS to netclock and reads its NMEA on UART1 (RX 39 / TX 38, 9600 baud; [components/gnss](../components/gnss/README.md)):
each second's NMEA time numbers the pulse's edges, so the pulse sets the clock with no NTP or browser, and the fix,
position and satellites are in `/api/status` (`gnss`). Checked on a board on 2026-10-24: found at once on RX 39 at
9600, a 3D fix with 6 of 10 satellites, every pulse edge after the first numbered by the NMEA, the burst 29 ms
after the pulse, and SDR decodes unchanged with the reader on (131 of 131 three times at 384 kS/s). Its L76K said
`ANTENNA OPEN` (no active antenna seen) and still had its fix.

**The W12's factory firmware** (the whole 16 MB flash, read before it was first flashed) is kept in
`firmware-backups.local/w12/` (git-ignored, not published: a vendor image), with its SHA-256 and how to write it back
(`README.txt` there).

**The W12's OLED** is a 0.96" SSD1315 (SSD1306 compatible, 128x64 at 0x3C), yellow in the top 16 rows and blue
below, powered through VEXT_EN, active low (Meshtastic's W12 variant). `w12` turns it on with no `oled` key in the
settings (`CONFIG_RDOF_OLED_BUILTIN`), on SDA 17 / SCL 18, and drives GPIO45 low before opening it; `"oled": false`
leaves it off and unpowered. It shows the SDR's screen (docs/spec/fw/device.md, "The SDR's screen"), a receiver
box's layout for one board: the network and the health flags in the yellow band, then a badge per channel (lit while it hears a
call), the calls being heard (or each channel's frequency and name, or the LR2021's error), and the tuner's center,
rate and DSP load at the bottom. Its task and the I2C interrupt are on core 0, it redraws every 0.5 s at most, in
one I2C write, and it takes no lock the DSP holds.

```
 ((( 192.0.2.8
 [MQ] -HB- [SDR]        12:41:07
 -------------------------------
 RX 4 [13]
 13 T9<9990123

 -------------------------------
 461.0475 576k cpu 19%
```

On the board (2026-10-24) the panel takes the init commands at 0x3C with GPIO45 low (no "no display" warning in
the log); what it shows hasn't been looked at yet.

#### SX1255 (plan)

Semtech's SX1255 is an I/Q transceiver for 400 to 510 MHz with separate RX and TX synthesizers, so it could do full
duplex, and it streams I/Q over a digital interface (I2S style) with SPI for control. A front end would be
`fe_sx1255.c`: SPI register setup, the S3's I2S in (and out, for TX) by DMA into the ring, cs16, with the
resampler for a rate that isn't a multiple of 48 kS/s. To check in its datasheet before writing it: the
interface's sample rates and word format, the clock it needs (a reference crystal or TCXO), and the PA power,
which is low (an external PA for useful power). Nothing written yet.

### Transmit: the SDR as a radio

Built and run on the host and in the self-test; on a board only with `sim`, which loops what it sends back into
what it receives. No front end with a real transmitter does it yet (the LR2021 and SX1255 plans are below).

- **Config.** A radio of type `sdr_transceiver` in `radios` (and in `run`, with the SDR): `duplex: half` (the
  default) is a roamer, with a roamer's keys (`idle`, `channels`, `scan` ...); `duplex: full` is a duplex repeater,
  with a repeater's keys (`rx_freq`, `tx_freq`, `cc`, `slots` ...). `tx_power_dbm` (10 by default) is the power.
  Its modem port is the SDR (`modem_port` "sdr"; a `modem` key is ignored), one a board. It's checked as the
  roamer or repeater it is, its frequencies on one of the SDR's channels. Saving settings on the board also refuses
  a front end that can't transmit, `full` on a half duplex front end, a frequency the SDR doesn't hear, and a TX
  frequency outside the front end's range.
- **The modem** (`sdr/src/sdrmodem.c`, `rdof/sdr/sdrmodem.h`): an emulated MMDVM modem, protocol 1, describing
  itself as "RadioFoundry Gateway SDR modem (MMDVM_HS compatible) DCDM", written from the protocol as RadioFoundry's
  `docs/spec/mmdvm-protocol.md` (MIT) writes it down. `modem_sdr.c` is the transport under the roamer's or
  repeater's `rdof_modem`, as `uart_modem.c` is for a HAT, so `txvr.c` and `repeater.c` run unchanged. SET_FREQ picks
  the receiver channel for the RX frequency (NAK if none) and asks the front end where the TX carrier goes.
  SET_CONFIG with duplex on a half duplex front end is NAKed (except as a split half). CW id is ACKed and not sent.
  DMR_SHORTLC is taken in the 9-byte encoded form or the 5-byte form if its CRC checks (a bad one is counted and
  dropped).
- **Half duplex**: bursts from DMR_DATA2, one every 60 ms, each with a 4-symbol ramp each side; status reports
  the free queue (8 bursts). What the receiver hears on the tuned channel goes up as DMR_DATA2 (the same control
  bytes as a HAT: 0x20 for voice sync, 1 to 5 for B to F, 0x40 | data type); idle bursts don't. A heard burst within
  half a slot of one it sent is its own signal and is dropped (the mute); the receiver's AFC, and so the ppm
  learning, skip bursts there too (`rdof_sdr_rx_t.muted`). DMR_LOST2 half a second after a call stops without a
  terminator; a data packet's call ends at its last block (no DMR_LOST, no carrier after it).
- **Full duplex**: DMR_START keys a continuous BS downlink, slot 1 first: each slot a CACH and the slot's queued
  burst, or an idle burst; ramps only at key up and down (DMR_START off ramps down the next slot). The CACH carries
  the short LC (the Null message until DMR_SHORTLC brings one; a new one starts on a slot counter that's a multiple
  of 4, a quarter in each CACH) and its AT bit is the inbound slot's busy state (a call from its first burst to
  its terminator or DMR_LOST, 360 ms after a data burst). Heard bursts go up as DMR_DATA1 or 2 by their slot,
  DMR_LOST1 or 2 likewise. Only on a front end whose caps say `RDOF_FE_FULL_DUPLEX` (just `sim` for now).
- **Dual capacity direct mode** (SET_CONFIG simplex with 0x40; [docs/spec/repeater.md](spec/repeater.md)
  section 13, the contract in [docs/spec/fw/radio.md](spec/fw/radio.md) 1.6): two simplex calls at once on
  one frequency, one a slot. DMR_DATA1 and DMR_DATA2 queue TS1 and TS2; each burst goes out in its slot's window,
  ramped on its own, TS2 30 ms after TS1, every 60 ms. The slot timing comes from TS1 / TS2 bursts it hears (stale
  after 1 s), except within 60 ms of one it sent; with none heard it keeps its own. Heard bursts go up by their
  sync: TS1 as DMR_DATA1, TS2 or MS as DMR_DATA2. Both slots report their real space.
- **A split half** (`RDOF_FEATURE_REPEATER`; the description ends " SPLIT1"): RadioFoundry's split duplex
  extension, either role. INFO offers RX always and TX when the front end transmits. As the RX half it never
  transmits and labels what it hears TS1 or TS2 from the epoch SET_EPOCH gives; as the TX half it runs the full
  duplex downlink above on a slot clock that runs from SET_ROLE 1 keyed or not (GET_EPOCH gives its slot start),
  with SET_AT's AT bits, the prebuffer, the key-up limit and cool-down. Either role ends with its lease. The modem
  clock is the sample counter (48 kS/s, in µs). `rdof_sdrmodem_t` keeps everything in its struct, so one SDR could
  serve as the RX half of several pairs (a modem a channel, sharing the receiver); `modem_sdr.c` runs one today.
  The board doesn't run an SDR half yet ("Not done" below).
- **Timing.** The receiver's sample counter is the clock: the front end pulls the modulator's samples by the same
  index as the samples it reads (`rdof_fe_tx_source_t`, scaled by `tx_rate / rate` when they differ), and a burst is
  queued 24 symbols (5 ms) ahead of the pull position. The receiver reports one of them at `start / D + 40 + 8`
  (48 kS/s; the demod's and the channelizer's delays), which is what the mute compares. Lining up with a
  repeater's slots (its CACH) isn't done: a simplex roamer doesn't need it. In DCDM the modem lines up with the
  radios' direct mode slots (above).
- **UTC slot timing** (`slot_timing: utc`, [docs/spec/slot-timing.md](spec/slot-timing.md)): the board's `slot_timing.c` stamps each
  chunk of the SDR stream on netclock's UTC as it arrives, fits the sample clock through the earliest arrivals
  (`rdof_sample_clock_t`) and runs the gate from `netclock_quality()` once a second; before every pull the modem
  gets the map and the gate (`rdof_sdrmodem_grid`, `_grid_map`, `_set_transmit`). Unkeyed, the duplex slot clock
  snaps to the grid (`grid_n0 = n & ~3`); keyed, it's steered a sample a slot; the carrier is corrected by the
  crystal's ppm; heard bursts are labeled with their n. As a split TX half it takes RadioFoundry's ALIGN (a snap, at once)
  and stream mode 1's TX_SCHED. `slot_timing` in /api/status and on the page: state, source, error, the sample
  clock's error, the modem's phase, slips and snaps. Host tests: `test_utc_tx` (25 ppm fast, 0–2 ms of arrival
  jitter: 30 s keyed within 97 µs of the grid), `test_utc_rx`, `test_split_tx_utc`, `test_pair_utc`.
- **The front end's part**: `tx_set(src, power)` in `rdof_fe_ops_t` and the TX fields of `rdof_fe_caps_t`. `sim`:
  full duplex, I/Q at the receive rate, mixed into the samples it reads at a quarter of full scale. `rtlsdr`,
  `lr2021`: receive only (refused when settings are saved).
- **Tests.** `sdrmodem_tests` (host): the CACH (short LC forms, the 4-slot cycle, AT), each split role on its own
  (labels, the lease, GET_EPOCH, SET_AT's modes and hold, the key-up limit, a retune), and the split matrix: a
  radio or an SDR modem as each half, `rdof_split_t` between them, 90 s of inbound bursts on the TX half's slots,
  each labeled right and the RX half's epoch within 100 µs of the TX half's slot starts (it's within about 25);
  with the SDR as the TX half, also the AT bits through SET_AT, its downlink and the lease. It and the self-test's
  transmit step run `rdof_sdrmodem_loopback`: a real `rdof_modem`
  client configures the SDR modem; three CSBKs it sends are heard back by the C receiver bit-exact where the modem
  sent them, and muted; another station's three bursts, in the gaps on the same channel, reach the host; duplex is
  refused; DMR_LOST once. Then full duplex on a second channel: keyed, idle bursts between ours, unkeyed. Noise at
  about 25 dB under the signals. About 0.35 s on the host. DCDM: two radios' calls on TS1 and TS2 of one channel
  through the receiver, the tracker and the HBlink4 sink (two calls, each slot its own stream); the modem's DCDM
  setup, both slots' queues and space, DMR_ABORT per slot, heard bursts labeled by sync; and end to end, the
  repeater in `mode: dcdm` over the SDR modem: the network down on TS1 30 ms off a radio's TS2 call while that call
  goes up, two network calls down at once, two radios up at once (and the network refused on both).
- **Not done.** A transmitting hardware front end (below); TX power control (`tx_power_dbm` is passed, `sim` ignores
  it); CW id; an SDR split half on the board (the receive stream is stamped on UTC now, the transmit side isn't);
  the UTC slot in the board's MQTT raw bursts (the modem labels it, the burst doesn't carry it); the time the TX
  path takes on real hardware (measured per front end, then subtracted from `start`).

Plans for real transmitters:

- **LR2021, half duplex**: frequency samples through the raw TX path (`docs/spec/lr2021-iq.md`, 3: selector 1 at
  125 kS/s, 32767 samples a run at most), so `tx_rate` 125000 and `pull_freq`. A run means leaving the RX session
  (standby, reset), so the turnaround is a session restart, not 107 us; the cost of that against the 30 ms between
  simplex bursts is the first thing to measure. The W12's GC1109 masks must be checked first (above).
- **SX1255, full duplex**: RX and TX on two synthesizers; a duplex repeater's downlink on one chip, RX on the input
  frequency. Needs a duplexer: a board question, not firmware.
- **Cost.** The modulator is about 100 cycles a sample, 30 Mcycles/s at 288 kS/s: an eighth of a core (sim's
  loopback pays it). At a 48 kS/s TX rate, about 5 Mcycles/s.

### Building

As "Firmware: esp_idf" above; `sdkconfig.defaults.sdr` makes an SDR only image. Sizes are in its "Size"
table. Only `sdr/`, esp-dsp and the DSP task build `-O2` (`CONFIG_RDOF_SDR_FAST`), which saves 64 KB against
building everything `-O2`; the per-sample code is all in that.

The self-test clip is 338 KB (`CONFIG_RDOF_SDR_SELFTEST`). At run time the DSP takes about 150 KB of internal RAM
for five channels with 4096-sample chunks (the demod buffers, 22 KB each, are the bulk): sdr allocates internal
while `CONFIG_RDOF_SDR_INTERNAL_RESERVE_KB` (40) stays free and PSRAM after that (`rdof_sdr_set_allocator`). The
buffers touched for every sample (the channelizer's FFT input and output, its twiddles, the FFT plans' twiddles)
go in internal RAM whatever the reserve (`rdof_sdr_set_hot_allocator`): on the W12 at 576 kS/s, the OLED's few KB
taken first once pushed the FFT input to PSRAM, and every call lost 6–48 of 131 bursts. They come from the memory
plan's hot arena, sized at boot by a dry run of the receiver's set-up. The chunk buffers (48 KB at 4096 samples, 32
KB of it in one piece), the arena and the DSP tasks' stacks are planned and allocated at boot before Wi-Fi, when the
heap is still whole ([memory.md](spec/fw/memory.md)). The 256 KB sample ring and the log ring
are in PSRAM. The self-test's stage table keeps the whole clip's channel output (1.15 MB) in PSRAM, so its
per-stage figures read a little high; its "whole check" and `prof` lines run the real receiver as it runs live.

Gates (`main/Kconfig.projbuild`, menu "RadioFoundry Gateway", under `CONFIG_RDOF_SDR`): `CONFIG_RDOF_SDR_HBLINK` (a peer a
channel), `CONFIG_RDOF_SDR_FAST` (`-O2` for the DSP), `CONFIG_RDOF_SDR_ESP_DSP` (esp-dsp for the FFTs: the
64-point IFFTs, the forward FFT as 6 x 64 or 3 to 6 x 256; and the matched filter, each checked against the
built-in code when the role starts and left out if they disagree),
`CONFIG_RDOF_SDR_TWO_CORES` (allows `"dsp_cores": 2`), `CONFIG_RDOF_SDR_HELPER_STACK` (6144), `CONFIG_RDOF_SDR_CHUNK`
(4096), `CONFIG_RDOF_SDR_RING_KB` (256), `CONFIG_RDOF_SDR_INTERNAL_RESERVE_KB` (40; 64 in `16mb`; the DSP buffers outside the hot arena),
`CONFIG_RDOF_SDR_SELFTEST`, `CONFIG_RDOF_SDR_SPIKE`, `CONFIG_RDOF_SDR_LR2021` (off; "Front ends" above); and the board's
MQTT, OTA, softAP and log options. The SDR core's own switches are under "RadioFoundry Gateway SDR (sdr)".

Cores: core 0 runs USB, Wi-Fi, lwIP, MQTT, HBlink4 and the web server; core 1 runs the DSP task; the roamer's
tasks, when it runs too, go where there's room (they outrank the DSP helper). With
`"dsp_cores": 2`, core 1 runs the channelizer's forward FFTs (the one part shared by every channel: 28 of
181 Mcycles/s with five busy channels), then both cores take channels end to end: each channel's share of the
channelizer (its bins, filter, 64-point inverse FFT and NCO), then its demod. The trackers' ticks and AFC run on
core 1 after. The forward step keeps each block's spectrum (about 43 KB at 288 kS/s with 4096-sample chunks,
allocated on the first two-core chunk; without the memory it falls back to channelizing everything on core 1),
and core 0 has its own inverse FFT plan, since an esp-dsp plan's work buffer can't be shared. The forward FFT at
576, 960 and 1152 kS/s (768, 1280 and 1536 points) is 3, 5 or 6 esp-dsp radix-4 FFTs of 256 and a small DFT per
bin (`dsp_esp.c`), checked against the built-in one at boot; the built-in mixed radix took 365 Mcycles/s at 960 kS/s. The split starts at
`round(C / 2 - 0.6)` channels on core 1 (forward FFT and trackers about 33 Mcycles/s, a busy channel about 28)
and is then chosen by what a chunk really takes, start to end: every 64 chunks it tries one channel more on the
less busy core, keeps that if the next 64 chunks were 2% faster, else goes back and waits 8 windows. Core 1 always
keeps at least one channel, and "all of them on core 1" is a choice too, so it can't settle on something slower than
one core (the per-core-time rule it replaced moved all eight off core 1 at 960 kS/s, and that was slower than one
core). The status's `dsp_split` says where it is. (Before, only the demods were split and the whole channelizer stayed on core 1:
8 busy channels went from 148% of a core to 135% of the wall clock.) The helper task is
created before the DSP buffers, so the buffers that no longer fit go to PSRAM rather than leaving internal RAM at
4 KB. Its stack is `CONFIG_RDOF_SDR_HELPER_STACK` (6144, as before): the status's `dsp_stack_free` and
`helper_stack_free`, and a log line a minute into streaming, give the high-water marks to trim it by (not measured
on a board yet). The output is the same either way (`sdr_selftest_tests` checks both).

### Settings

The board's box config ("Settings" above) with an `sdr` section in `radios` ([docs/spec/fw/config.md](spec/fw/config.md)):
`name`, `device {sample_rate, center ("auto" or MHz / Hz), gain (dB or "auto"), agc
(true or {min, ...}), ppm, settings {biastee}}`, `receiver_ids` (`"SDR{ch:02d}"`), `peer_ids` (each channel's peer
is this plus its number), `channels` (plan numbers, or `{ch, ...}` to override `id`, `name`, `freq`, `cc`, `slot`,
`tg`, `peer_id`, `hblink`), `callsign`, `hblink` (false: no peers), `location`, `rssi_offset`, `afc`,
`status_seconds`; the box's `mqtt` (app `sdr`), `hblink` (`host`, `port`) and `location`. Board keys in the
section: `usb {transfers, transfer_kb}` (8 x 8 KB by default), `dsp_cores` (1 or 2), `selftest`, `spike` (run at
boot), `sim` (no dongle: the self-test clip, five busy channels around 461.1225 MHz at 288 kS/s, looped in real
time; a chunk more than half a second late is dropped and counted, as the USB ring would. For measuring the DSP
and testing the board end to end; needs `CONFIG_RDOF_SDR_SELFTEST`), `sim_ppm` (with `sim`: a simulated tuner this
many ppm fast, to watch `learn_ppm` work), `idle_gate` (default true; "The idle gate" below) and `learn_ppm`
(default true; "The tuner's ppm, learned" below). `rdof_sdr_tests` checks the resolving against golden results,
for a full box config and hand-made ones.

A plain SDR config (no `radios`: `defaults` and `channels [{id, name, freq, ...}]`, the board keys at the top) is
taken too.

#### The idle gate

A channel with nothing on it costs nearly as much as a busy one: the discriminator, matched filter and sync
search run on noise too. The gate skips them. Each chunk, the channelizer measures each channel's output power
against what noise alone would give there (`rdof_chz_t` `level`): the band's noise is the 25th-percentile bin of a
Hann-windowed spectrum (made from the channelizer's own FFT, every 4th bin, every 8th chunk), carried through the
channel filter. Noise alone reads 1 to 2; a channel skips its demod for a chunk when that chunk and the one
before stayed under 3 (a signal 3 dB over the noise in the filter, several dB under what DMR decodes at), it
isn't in a call, it had no burst in the last 2 s (a keyed repeater's idle bursts hold it open, whatever its level
reads), and it isn't in its probe (0.1 s in every second, staggered over the channels, when it runs anyway, so a
signal the level misses is still found). The first 2 s run everything. A skipped chunk goes into the demod as
zeros, and the FM stage is warmed up on the last 82 samples before it runs again, so its output is exactly what
it would have been.

It changes nothing the receiver puts out (`rdof_sdr_edge_tests` runs a recording with and without it and compares
the bursts and AFC). Measured on a 7 s simulated scene with channels idle for seconds (a call 3 s in, a repeater
keyed from the start, a call 5 s in, a channel with nothing), down to the noise where decoding starts to fail, at
8 ppm and under a clipped station 40 dB up: the same output, with 37% of the channel-chunks skipped. The status's
channels show `idle`, the share skipped. It saves CPU only.

#### The tuner's ppm, learned

A crystal that's fast puts every signal low by f x ppm, and each channel's AFC follows its signal, so the AFC
corrections say what the tuner's error is (`rdof/sdr/ppm.h`), plus each transmitter's own (a radio may be 1.5 ppm
off). Every 5 s the board takes the channels with a burst in the last 30 s: when two or more agree within 1 ppm
of their median, the median is the estimate; held within 0.5 ppm for 60 s, the RTL's correction moves by the
whole ppm and every channel's correction by the same amount the other way, so nothing jumps. What it learns
goes into a temperature table in NVS (namespace `rdof_ppm`, key `cal`, with the dongle's `VID:PID serial`; another
dongle starts from the setting): the board's die temperature in 2 C bins against the learned ppm, averaged with
what the bin had (netclock's `nc_tcal`). A value goes in only once the board has settled (below) and the estimate
has been steady 15 minutes, and the same bin at most every 2 hours in one run (`CAL_AGAIN_S`). At boot the table's
value for the die's temperature sets the RTL (whole ppm) and the channels (the rest), so the AFC starts near zero;
with no table yet, a single value saved by older firmware (key `learned`) does. Until it has an estimate of its
own, a settled board follows the table as the room warms or cools (`cal_follow`). The status's `ppm`:
`learning`, `rtl` (whole ppm set), `learned`, `channels` (agreeing), `steady_s`, `saved`. Off with
`"learn_ppm": false`; the `ppm` setting is then the RTL's. With `"sim": true, "sim_ppm": 6.5` the board shows it
working: the AFC goes to -3 kHz, the estimate to 6.6 (the clip's transmitters' median adds 0.1), the RTL to 7 at
60 s, the AFC back to the transmitters' own offsets.

The sample clock gives the same error without any traffic, and without the transmitters' errors in it. The
RTL2832U's samples and the R820T's LO come from one 28.8 MHz crystal, so the rate the samples really arrive at
says how far off the crystal is (`rdof/sdr/rate.h`):

- The USB driver stamps each transfer as it completes (`on_data`: esp_timer, and the bytes sent, dropped ones
  included). Late arrivals are the only noise, so the least late transfer in each 10 s bin tracks the stream.
  Those minima are fitted with a line over up to 30 minutes.
- A bin more than 200 us off the line means the RTL dropped samples without saying so (its FIFO overflowed): the
  fit starts over, keeping the last estimate until the new fit has a minute of bins.
- The board's own crystal is taken out with netclock's rate (`netclock_rate`). Its standard error goes into the
  estimate's error. A rate stored from the last run, not yet fitted in this one, counts as 0.5 ppm.
- The correction moves only the tuner's LO and the IF: the front end sets `fixed_sample_rate`, which leaves the
  RTL's sample frequency register at 0, so the samples run on the bare crystal. On the bench (an R820T dongle) that
  register didn't do what its formula says (docs/spec/rtlsdr.md D1.8): +10 moved the rate by -3.4 ppm, not -10 (`raw` -29.4 at 0,
  -32.8 at 10). A few ppm of sample rate is about 1 S/s at 288 kS/s, which the demod's timing follows anyway.
- Nothing is trusted or saved while the crystals warm. The board's die went from 41 to 51 C over the first 10
  minutes on the bench, and the sample rate against the board's clock moved 1.2 ppm meanwhile. Settled means the
  die temperature (`netclock_die_c`) stayed within 3 C (its sensor reads in 1 C steps and wanders), and the sample
  rate over the last 5 minutes within 0.2 ppm, for 10 minutes (netclock's `nc_settle_t`). The page says which one
  it's waiting on. The simulated front end stamps each chunk when it was due, not when a busy board read it, so its
  sample clock doesn't see lost samples that weren't. On settling, the fit forgets the bins before that window. Until then the AFC
  still moves the RTL, but no learned value is saved. A stream restart on the same dongle keeps the history; a
  new dongle starts it over.
- Once the board has settled, the estimate's error is 0.3 ppm or better and its fit spans 5 minutes, it is the
  learned value
  (`rdof_ppm_measured`). The same rules then move the RTL and save the value. From then on the AFC isn't used in
  that run, even while the fit times afresh after a move, because the AFC carries the radios' own errors.
- The board can't time the clock while its own rate is unknown (no NTP or PPS yet, nothing stored). Then, and
  before the fit is good enough, the AFC learns as above.
- What's saved is a table, not one value (netclock's `nc_tcal_t`, NVS key `cal` with the dongle's serial): once
  settled, the learned value goes into the 2 C bin for the board's die temperature, averaged with what the bin had,
  at most every 2 hours per bin in a run. Over runs it fills in the dongle's error across the temperatures it has
  sat at. At boot the RTL starts from the table at the die's temperature now (interpolated; the nearest bin beyond
  the ends; the old single `learned` value if there's no table). Until the run has learned its own value, and
  once settled, the RTL and channels follow the table as the room warms or cools (when it differs by 0.25 ppm).
  Not while warming: the table holds settled values, and the dongle has no thermometer. Neither the RTL2832U nor
  the R820T/R828D has a documented temperature sensor, so the board's die stands in for the room, and a dongle
  just plugged in starts cold whatever the die reads. The board's own crystal has the same kind of table in
  netclock (its README, "Temperature").

The status's `ppm.by` says which one learned it (`clock` or `afc`), `ppm.settled`, `die_c`, `die_spread` and
`die_tol` the warm-up. `ppm.clock` has `ppm` (the crystal), `err`, `trusted`, `raw` (the samples against the local clock),
`span_s`, `rms_us` (the bins' scatter), `steps` (restarts), `spread` (the recent rate's over the settle window), `spread_tol` and
`board` / `board_learned` (netclock's rate). `ppm.table` is the table (`c`, `ppm`, `n`) and `ppm.expected` what
it says at the die's temperature now. The page shows it under "Tuner ppm". In the host test, a dongle 37.2
ppm fast, with 0.1 to 2 ms of USB latency, 30 ms stalls and one silent 512-sample drop, comes out within 0.003 ppm
over 30 minutes. With `sim`, the simulated front end paces its chunks `sim_ppm` fast in true time, so the sample
clock finds `sim_ppm` too. The LR2021 front end doesn't time its clock yet.

On the bench (an R820T dongle, NTP over Wi-Fi): `rms_us` 8 to 30 us, far under the 200 us step; the dongle came out
about -2 ppm. What limits it on NTP alone is the board's own rate: with about 2 ms of Wi-Fi jitter, netclock's fit
went -22.0, -25.8, -27.5 ppm at 10, 12 and 14 minutes (skew 4.0 to 2.9 ppm), so the crystal estimate isn't trusted
for an hour or two (40 references 300 s apart). Until then the AFC learns it. With a GNSS PPS the board's rate is
good to hundredths within minutes.

Sign: in the RTL's convention (librtlsdr, SoapySDR, `rtl_set_freq_correction`) a positive ppm fixes a fast
crystal. A calibration that reports the measured offset over the frequency gives a negative number for a fast
crystal (signals low), the other way round. Not checked against a dongle yet.

Notes on the receiver:
- With `"center": "auto"`, when the channels don't all fit the rate the board takes the best window
  (`rdof_plan_window`) and leaves the rest out, with a log line each. The web page's "As resolved" shows the plan.
- `ppm` is set on the RTL as a whole ppm, as SoapySDR's RTL module does; with `learn_ppm` the board learns it
  (above).
- The idle gate (above) is the board's own: it saves CPU and changes no output.
- The status's `droppedChunks` counts samples lost in the ring (the DSP fell behind). `overflows` counts failed
  USB transfers. Samples the RTL drops on its side show only as a shortfall in USB bytes per second (web page:
  "USB samples").
- JSON times are wrong until NTP (or a browser on the page) has set the clock; the stream time re-anchors when it
  steps. Times come from netclock, so after that they slew rather than step.
- A board without a `name` is "radiofoundry" (a plain SDR config's, "sdr").

### The SuperMini: build, flash, test

The board and its USB-C, power and flashing are as in "The ESP32-S3 SuperMini" and "Flashing" above. Build the
`4mb` profile, set the settings to run only the SDR (`"run": "s3-sdr"` in the defaults), then:

```sh
idf.py -B ../build/4mb -D SDKCONFIG=../build/4mb/sdkconfig -p /dev/cu.usbmodem1101 flash    # in ROM download mode
idf.py -p /dev/cu.usbserial-XXXX monitor                                               # console on UART0
```

On a `16mb` board the OTG port is the dongle's, and the USB-UART port does both flash and console. The bridge
resets the board into download mode itself:

```sh
idf.py -B ../build/16mb -D SDKCONFIG=../build/16mb/sdkconfig -p /dev/cu.usbserial-XXXX flash monitor
```

Many of these boards only power VBUS on the OTG port when a solder jumper (often marked `USB-OTG`) is bridged;
without it the dongle needs power from an OTG adapter or hub.

**1. Boot, no dongle.** Expect `esp32: s3-sdr`, `SDR s3-sdr: 5 channels at 288000 S/s`, then from `esp-dsp`:
`FFT 768 / 1280 / 1536 as 3 / 5 / 6 x 256: in use`, `FFT (dsps_fft4r_fc32 to 256 and 384 as 6 x 64, dsps_fft2r_fc32
to 512): in use` and `matched filter: int16 SIMD`.
Each is checked against the built-in code at boot; `LEFT OUT` (FFT) or `dsps_fir_f32` / `built-in` (the filter)
means it didn't match, and the board runs anyway, only slower. Then `USB host up`,
`USB host up (ring 256 KB)` from the driver, and `waiting for an RTL-SDR (0BDA:2838)`. (With the roamer in `run` too
and on USB, the role starts, with these lines, only once the dongle is plugged in.) With no Wi-Fi networks, the access point
`radiofoundry-XXXX` (password `radiofoundry`) comes up, page at <http://192.168.4.1/>.

**2. Self-test (still no dongle).** On the page, press "Run the self-test" (or put `"selftest": true` in the
settings). The board restarts and, before anything else starts, logs:
- `output, one core:  PASS: 53 messages ..., CRC 283d7439 ...` and the same for two cores, and `output
  with ...` for the clip with each fault in it (+4 kHz, noise, overload). FAIL means the board's DSP doesn't give
  the expected output: report it with the log.
- the whole check's load (`whole check: N% of one core on one core, M% of the wall clock on two ...`), then a stage
  table in Mcycles per second of IQ (five busy channels), the FFT timings (1280 points built-in against 5 x 256; 384
  built-in against 6 x 64; 512 and 64, built-in against esp-dsp), the kernel timings (`kbench.c`), and at the end
  a verdict: `one core keeps up`, `one core is tight: set "dsp_cores": 2`, or `one core can't keep up`.
- a load test: `selftest_load.channels` busy channels (8 by default; each a repeater's downlink, a burst every
  slot) at each of `selftest_load.rates` (288000 by default; any multiple of 48000 the channels fit in, 960000 say),
  made as it goes and run through the receiver on one core and then on two with the balanced split:
  `load: 8 busy channels at 288 kS/s: N% of one core on one; M% of the wall clock on two (k of the 8 demodded on
  core 1 after balancing)`. Settings: `"selftest_load": {"channels": 8, "rates": [288000, 960000]}` at the top
  (or in the SDR's section). A first line says what it read (`load: 8 channels at 288000, 960000 S/s
  (selftest_load)`), and a rate it can't run says why.
- `prof` lines after the clip runs and each load run (`sdr_prof.h`): per core, Mcycles per second of IQ for the
  forward FFTs, the per-channel channelizer, the demods (and their steps) and the trackers (counted after the join);
  the search's own steps (`search steps squelch+prefilter+score ... syncs ... (flywheel ..., at sync ...)`: the
  sync's cost split into the flywheel up to it and the burst at it, with positions, scored and syncs tried a
  second); the forward step (FFT, copy and window move, noise floor) and internal RAM free; the wall time, core 1's
  wait for the helper and the helper's wake-up time; and for the load runs where each buffer of the receiver went
  (internal or PSRAM, and how big). Each load rate also runs a third time on two cores taking turns (the helper's
  share, then core 1's, at the split the balanced run settled on): the same work without the other core running, so
  the difference is what sharing the cache and PSRAM costs. The "whole check" line splits the clip check's time into
  the receiver's chunks and the rest (setup, the cu8 conversion, the check's own sinks).
- `sdkconfig.defaults.dspfast` (on top of `16mb`) is an experiment: the receive path's hot code in IRAM
  (`CONFIG_RDOF_SDR_DSP_IRAM`, about 33 KB of internal RAM) and the 64 KB data cache. The self-test's first line says
  which build it is.
- the transmit loopbacks (half and full duplex; "Transmit: the SDR as a radio").
The page's "Self-test" row shows the verdict. If it says two cores, set `"dsp_cores": 2` in the settings.

**3. Plug in the RTL-SDR** through a USB-C OTG adapter that gives the dongle 5 V on VBUS (the S3 doesn't power
VBUS; the dongle draws about 280 mA, plus the bias tee if on). Expect:
- `RTL-SDR 0BDA:2838 "Realtek" "RTL2838UHIDIR" ..., Full Speed`, the device and configuration descriptor dump,
  `bulk IN 0x81: 64-byte packets`, then `R820T tuner, bulk FIFO set for 64-byte packets`.
- then from `sdr`: `tuned 461.12250 MHz at 288000 S/s, ppm 0, gain 29.7 dB` (or the window it picked), and
  `streaming: 8 transfers of 8192 bytes in flight`.

What a failure means:
- Nothing after `waiting for an RTL-SDR`: it never enumerated. Check VBUS at the dongle (5 V), the OTG adapter,
  and the cable.
- `XXXX:XXXX isn't an RTL2832U: not ours`: a different device.
- `rtl_open failed (-1/-2)` or `control ... timed out`: control transfers fail. Note the descriptor dump and the
  error; replugging resets the dongle.
- `rtl_open failed (-4): no R820T / R828D tuner answered`: an E4000 / FC0012 / FC0013 dongle, not supported.
- `rtl_open failed (-6): tuner init failed ... PLL didn't lock`: report it with the log.

**4. Throughput spike.** Press "Run the USB spike" (restarts; the dongle must be in). For each of 288000, 250000,
240000, 300000 and 960000 S/s and three transfer plans (8 x 8 KB, 4 x 16 KB, 16 x 4 KB) it streams 5 s and logs, for example:
`spike: 288000 S/s, 8 x 8192 bytes: 576.0 KB/s of 576.0 (100.0%), 0 errors, 0 short, 0 ring overflows`.
- 99.5% or more at 288000: Full Speed keeps up. Keep the plan that did best (`usb {transfers, transfer_kb}`).
- Less, marked `SAMPLES LOST`: the RTL's FIFO overflowed between transfers. Try more or larger transfers. If 240000
  is clean and 288000 isn't, set `"sample_rate": 240000` (4 channels at most; 192 kHz usable).
- Errors or short transfers in large numbers: the dongle or the host controller is unhappy at Full Speed. Send
  the log.
960000 always shows `SAMPLES LOST`: it asks for more than Full Speed carries, and is there to measure the ceiling,
which the last line gives (`spike done: ... the most that arrived 1152.5 KB/s` on the SuperMini). Expect 288000 and
300000 whole: they were, with every plan, on 2026-10-21.

**5. Receive.** Set Wi-Fi, MQTT and the channels; key a radio on one of them. On the page:
- "USB samples" near 100% of the expected rate;
- "DSP load" under about 80% (or set `"dsp_cores": 2`);
- the channel's bursts count up, with RSSI and AFC.
On MQTT: `DMR/<id>/sdr/json` call start / end, `DMR/<id>/sdr/raw`, the retained `Receiver` messages, and
`DMR/_sdr/<name>/status` every 30 s. With `hblink.enabled`, each channel's peer registers (`peer ... registered`).

What a failure means:
- `ring overflows` or `droppedChunks` climbing: the DSP can't keep up. Set `"dsp_cores": 2` or use fewer channels.
- `no samples for 3 s: restarted` (`stream_restarts` in the status): the stream stalled (USB errors?) and was
  stopped, retuned and started again. A dongle that won't open is tried four times; then `rtl_error` says to replug.
- `bytes_lost` / `bytes_filled`: transfers dropped because the ring was full, read back as mid-scale filler (up
  to 250 ms a gap) so the stream keeps its timing and only the bursts in the gap are lost.
- Bursts never appear but USB is at 100%: check the frequency, the ppm (the AFC follows up to 5 kHz, `RDOF_AFC_LIMIT_HZ`, about
  10.8 ppm at 461 MHz; past that set `ppm`), the color code and the gain (look at `peakDbfs` / `clipPct` in the status).

### CPU budget on the S3 (measured)

Measured 2026-10-09 on a SuperMini (240 MHz) with the self-test clip, five busy channels at 288 kS/s, 4096-sample
chunks. The self-test's stage table, in Mcycles per second of IQ, before and after the S3 work below:

| Stage | First run | Now | What changed |
|---|---|---|---|
| cu8 to float | 7.1 | 7.1 | |
| forward FFT (384 points, 1000 a second) | 50.5 | 28.4 | 6 x 64: esp-dsp's radix-4 64-point FFT, then a 6-point DFT a bin (`dsp_esp.c`) |
| per channel: bins, 64-point IFFT, NCO | 63 | 41.4 | radix-4 IFFT read through one permutation (no bit reversal or conjugate passes); bins gathered in runs; the NCO's software doubles only when the AFC moves it |
| discriminator | 45.4 | 37.8 | atan2 with no divide (a library call on the S3) and no branches: octant from the float bits, reciprocal by the bit trick and Newton |
| matched filter + power | 60.4 | 36.4 | int16 SIMD (PIE) 96-tap FIR, 43 cycles a sample against esp-dsp's float FIR's 175 (`fir_s3.S`) |
| squelch, sync search, slicing, flywheel | 113 | 33.9 | a cheap prefilter before the exact sync score; the demod's history moved down once every 9 feeds instead of every feed |
| call trackers | 4.7 | 4.9 | |
| **total** | **344** | **190** | |

The bench's discriminator and filter figures include reading the whole clip's channel output from PSRAM. The
output is the expected one throughout (53 messages, CRC 283d7439, 39 bursts).

Live, with `"sim": true` (the page's "DSP load" row breaks it down by stage, and the status's `demod_steps` the
demods by step), on the same clip:

| Channels | One core | Two cores (`"dsp_cores": 2`) |
|---|---|---|
| 2 | all samples, load 60% | all samples, 54% |
| 3 | all samples, 75% | all samples, 68% |
| 4 | all samples, 88% | all samples, 78% |
| 5 | 95% of the samples (drops) | 85% of the samples |

So on 2026-10-09 four busy channels ran on one core, and five didn't quite. Two cores helped little and hurt at
five: core 0 also runs Wi-Fi, lwIP and the web server, and the second core's demod buffers left internal RAM at
about 5 KB. (Now: "2026-10-21, eight busy channels" below.) Most of
the DSP's buffers are in PSRAM (about 60 KB of the receiver's are internal), and PSRAM traffic is now a large
part of the cost: about 17 cycles a byte through the 32 KB data cache. The demods' frequency and power arrays
cost about as much to write as their discriminator and filter cost to compute.

Live, busy, by step at five channels on one core: input (level, float conversion; with `sim`, reading the clip
from flash) 10%, channelizer 30%, FM (discriminator, filter, power) 26%, sync search 22%, history moves 5%,
flywheel 2%.

A lower sample rate saves only the wideband part (input and forward FFT, about 15% of the total at 288 kS/s;
240 kS/s would save about 6 Mcycles); the per-channel work is the same at any rate, so channel count is the lever.

Later (2026-10-10): 64-byte data cache lines (`sdkconfig.defaults`, no RAM cost) bring the bench to 180 and four
busy channels live from 0.89 to 0.86 load. Tried and dropped: a 32 KB instruction cache (178.9, but 15 KB less
internal RAM: 2.7 KB free at five channels); the sync prefilter as constant signed sums (DMR syncs are all ±3,
so no multiplies; slower on the S3, 46.6 to 60.9 with the squelch off).

2026-10-21, eight busy channels (`selftest_load`) at 288 kS/s, by the search's own steps (`prof ... search steps`):
the squelch and prefilter were 100 Mcycles/s (about 530 cycles a position, 189k positions a second) and each data
burst about 118k cycles (263 a second). Three changes took the load from 139% of one core to 116%, and from 103% of
the wall clock on two cores to 93%, output unchanged:

- The prefilter as signed sums again, but shared: the 24 symbols summed by their 4-bit sign key across the four
  families (15 keys), then the keys by row and column, so each family is two adds from the shared sums
  (`maybe_sync_keyed`, unrolled; the loop is the fallback if the keys ever disagree with the families). 100 to 65.
  Block versions (each phase gathered into a row, then esp-dsp's `dsps_corr_f32`, or the int16 SIMD dot product)
  were slower: the search is instruction bound, and the gather and second pass cost more than they saved.
- Golay(20,8) and QR(16,7) decoded from codeword tables (`layer2.c`), a lookup when the word is clean. RadioFoundry's
  decoders encode all 256 or 128 candidates as they go, about 45k cycles a word; every data burst's slot type and
  every voice burst's EMB paid it. A data burst went from 118k to 49k cycles.
- Burst power with `log10f`: a double log is software on the S3. 49k to 37k.

What's left at eight busy channels, one core: FM (discriminator, filter, power) 88, the forward FFT 96, the
channelizer's per-channel work about 68, squelch and prefilter 65, the bursts 10. At 960 kS/s the same eight take
176% of one core and 168% of the wall clock on two.

Then the idle gate ("The idle gate" above), live with `sim` (channels off the clip's frequencies are idle):

| Channels | Before | With the gate |
|---|---|---|
| 4 busy + 1 idle | all samples, 0.96 load | all samples, 0.95 |
| 3 busy + 3 idle | 86% of the samples | 92% |
| 2 busy + 4 idle (six channels) | 88% of the samples | all samples, 0.96 |

An idle channel's FM stage and search now cost almost nothing (2 busy + 4 idle: FM 0.13 of the core, search
0.12); its channelizer share (bins, inverse FFT, NCO: about 0.07 of the core a channel) is what's left, and the
next lever: a skipped channel needs only its last two blocks inverse-FFT'd (for the warm-up) and its NCO phase
moved on. After that, the forward FFT in int16 (8-bit samples lose nothing; esp-dsp's `dsps_fft2r_sc16` does 64
points in 775 cycles against radix-4 float's 2,300; about 4%), and fewer PSRAM bytes per sample. Five busy
channels on one core then reached about 93% of real time.

### The bench API

The board answers the bench API ([docs/spec/bench-api.md](spec/bench-api.md)) under `/api/bench/v1`, so
bench tools written for the authors' bench daemon run against it in its place (`tools/bench/benchapi.py` is a client for it:
info, status, spectrum, bursts, capture, radios, link, tune, tx; the HIL tests use it). HTTP for the snapshots, WebSockets for the streams (bursts,
I/Q channel taps and the wideband samples, with up to 1 s of pre-roll and a level trigger), a run-time retune of the
SDR (`PUT /sdr`: center, gain, taps at any frequency in the window, under an exclusive lease that puts the settings
back when it ends), radios wired to the bench (`"bench": {"radios": [...]}` in the settings: an MCU UART, or USB
CDC-ACM or a USB-serial bridge on the host port) behind the bench daemon's guard, a RadioFoundry radio's tune, and DMR
burst transmit (`WS /tx`) through the SDR's modem or an MMDVM bench radio, only on what `"bench": {"tx": {...}}`
allows. No auth: it's for a private test network. Code in `bench_api.c`, `bench_radio.c` and `bench_tx.c`
(`CONFIG_RDOF_BENCH`, `CONFIG_RDOF_BENCH_TX`); the spec's section 9 says what's in and what isn't yet.

### Not yet done (S3 SDR)

- The dongle's faults on a board: it has run with an RTL-SDR (a real radio's call end to end, 2026-10-21), but the
  gap filler is tested on the host only, and the stall restart and open retries not at all.
- RTL-SDR Blog V4 specifics: the R828D's inputs and the HF upconverter are written (from webrtlsdr) but not run on
  one; its notch switching isn't done. E4000 / FC00xx tuners; direct sampling.
- The RTL-SDR driver's open measurements (docs/spec/rtlsdr.md section 3): the IF filter's codes in Hz, the gain
  steps in dB, the FIR's rate, the sign of the IF correction.
- Eight busy channels on one core: at 288 kS/s they take 116% of one core, 93% of the wall clock on two; at 960
  kS/s 176% and 168% ("CPU budget on the S3"). Next: the channelizer's share skipped on idle channels, an int16
  forward FFT, fewer PSRAM bytes per sample.
- The fall back from MQTT 5 to 3.1.1 hasn't met a broker without MQTT 5 (mosquitto has had it since 1.6).
- **Two carriers from one LR2021 (bench test, for trunking).** Can its raw I/Q transmit (selector 0, with the OOK
  setup for amplitude, `docs/spec/lr2021-iq.md` 3) send two DMR carriers at once, 12.5, 25 and 50 kHz apart, at
  125 kS/s? A W12 into a dummy load through an attenuator, the RTL-SDR watching (the bench API's spectrum and a
  capture). Measure the third-order products (one spacing outside each carrier, on the neighboring channels)
  against the carriers and the Part 90 emission mask; check that both carriers decode, and how the chip handles
  the 180° phase steps at each envelope null (R3.7) and the 8-bit amplitude (saturates near 64). Doubtful: the
  chip's PA is built for constant envelope, and a continuous downlink needs runs chained without a gap, which
  nobody has tried (R3.5, R3.6). If it fails, the
  SX1255 (below) or an I/Q transmitter with a linear PA on a Linux host is the way to two carriers. The LR2021 is
  half duplex, so it would only ever be a TX half.

## License

Private for now. The license is to be decided: MIT is intended, pending a provenance review. Third-party license
texts are in [LICENSES/](../LICENSES/), and where each piece of copied or ported code came from is in
[PROVENANCE.md](../PROVENANCE.md).

