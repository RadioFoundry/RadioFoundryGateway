# RadioFoundry Gateway: reference

DMR gateway firmware for the ESP32-S3 (`esp_idf/`) and host tools for Linux, macOS and the PlutoSDR (`sdr/cli/`).
One build runs what its settings ([config-v3.md](spec/fw/config-v3.md)) make: SDR receivers, roaming transceivers,
repeaters, the IP gateway. The core (`common/`, `sdr/`) is portable C11 and is shared by every target.

Outputs:

- **MQTT**: MMDVMHost-compatible call JSON, each burst as a binary raw burst (49 bytes; 62 with UTC timing), and a
  retained status and `Receiver` message per radio ([mqtt.md](spec/fw/mqtt.md)).
- **HBlink4**: a peer per radio, or per SDR channel; DMRC, DMRD both ways, DMRT / DMRK call routing; UDP, or SCTP
  over UDP for the roamer ([hblink.md](spec/fw/hblink.md)).
- **HTTP**: the web page, a JSON API and the bench API ([interfaces-http.md](spec/fw/interfaces-http.md),
  [bench-api.md](spec/bench-api.md)).

The specs are in [docs/spec/](spec/), starting with [overview.md](spec/fw/overview.md). Each component in
`components/` has its own README.

**Status (2026-11-04).** Running on hardware: the SDR receiver on the SuperMini, the N16R8 and the W12 (RTL-SDR and
LR2021); the W12 transmitting as a roamer, on the bench and as the IP gateway; `rdofradio` as a full or half duplex
repeater on a PlutoSDR, over USB and on the Pluto's own ARM, with the FPGA channelizer and TX interpolator. Host
tested only: a split repeater on a board, a USB hub with two radios, `rdofradio --split-tx` with a real radio.

## Build and test

### Host tests

```sh
cmake -S host-tests -B build/host && cmake --build build/host && ctest --test-dir build/host
```

CMake and a C compiler; cargo for the Rust build of the tests; Node for the settings schema and the page's scripts.
Offline, a few seconds, under AddressSanitizer and UBSan (`-DRDOF_SANITIZE=OFF` turns them off). The golden fixtures
are committed (`host-tests/vectors.h`, `scenarios.h`, `fixtures/`) and the C must reproduce them byte for byte.

| Test | What |
|---|---|
| `rdof_tests`, `rdof_tests_min` | `common/` with every feature on, and with every `RDOF_FEATURE_*` off: FEC, layer 2, PDUs, AMBE, MMDVM, HBlink4 and the OLED frame against golden vectors; the tracker, the roaming transceiver and the repeater (duplex, SFR, DCDM) against golden scenarios; split duplex; the jitter buffer; settings; two radios a board |
| `rdof_tests_rust` | `rdof_tests` with `rust/`'s modules in place of their C (needs cargo) |
| `rdof_site_tests`, `rdof_plan_tests`, `rdof_schema_tests` | settings version 3: the parser, router and pool; the plan and its units; the JSON schema (Node) over the fixtures in `fixtures/config/v3/` |
| `rdof_sdr_tests`, `rdof_sdr_tests_min`, `rdof_sdr_edge_tests` | the SDR receive path against golden vectors; the corners they don't reach ("SDR core" below) |
| `rdof_mod_tests`, `sdrmodem_tests`, `rdof_loopback_edge_tests` | the modulator, the SDR modem, its loopbacks |
| `rdof_chzfx_tests`, `rdof_chzhw_tests`, `rdof_chzrx_tests`, `rdof_plutofpga_tests` | the PlutoSDR FPGA channelizer's model, its driver, the receiver on its frames, the board and bitstream checks |
| `rtlsdr_tests`, `rtlsdr_gaps_tests` | the RTL-SDR driver against a fake dongle, and its USB gap filler |
| `sdr_selftest_tests` | the firmware's self-test check, on the host |
| `rdof_hostweb_tests`, `rdof_webapi_tests`, `rdof_radiocfg_tests` | the host web server, the shared web API (byte for byte against the firmware's status), `rdofradio --config` |
| `rdof_benchline_tests`, `benchsig`, `rdof_fm_tests`, `rdof_am_tests` | the bench line protocol, signal modes, the FM and AM generators and receivers |
| the rest | `rdof_memplan_tests`, `rdof_parrot_tests`, `rdof_power_tests`, `rdof_lastheard_tests`, `rdof_rdofnet_tests`, `rdof_pffft_tests`, `ipgw_sdr_tests`, and the components' own (`test_netclock`, `test_netclock_posix`, `test_wifilist`, `lr2021`, `tinysctp_tests`) |

Coverage: `sh host-tests/coverage.sh [build dir] [file regex]` (clang; default `build/cov`) prints lines and branches
per file and writes an HTML report to `<build dir>/html`.

Checked on macOS arm64 (Apple clang; GCC 15 with `-DRDOF_SANITIZE=OFF -DTINYSCTP_SANITIZE=OFF`, which has no
sanitizer runtime there), Ubuntu 24.04 x86_64 (GCC 13), and Debian 13 aarch64 on a Raspberry Pi (GCC 14, sanitizers
off: GCC's ASan doesn't start under the Pi kernel's 39-bit address space).

### Host build and system tests

```sh
cmake -S . -B build/linux -DRADIOFOUNDRY_DIR=/path/to/radiofoundry && cmake --build build/linux && ctest --test-dir build/linux
```

The top-level build builds the libraries, the host tools (`rdofsdr`, `rdofradio`, `rdofchz`) and the host tests.
`-DRDOF_BUILD_TESTS=OFF` builds the libraries and tools only; `cmake --install` puts the tools in `bin/`.
`-DRDOF_HOST_WEB=ON` adds the web page to `rdofradio` ("Host web page").

The system tests ([system-tests.md](spec/system-tests.md)) run the gateway against RadioFoundry's own MMDVM and
hotspot core (`radiosim`), built from a [RadioFoundry](https://github.com/RadioFoundry/RadioFoundry) checkout at the
commit in `systest/radiofoundry.ref`. Without `RADIOFOUNDRY_DIR` (a CMake option or the environment) they're left
out.

tinysctp against usrsctp and HBlink4 (on macOS `brew install libusrsctp`, and an HBlink4 checkout with its
`.venv`): `sh components/tinysctp/test/interop.sh` (an echo server on HBlink4's usrsctp transport: clean, then 15%
loss with duplicates, reordering and a 3 s outage, then with a 300 ms lifetime) and
`sh tools/hblink4_sctp_check.sh [--lossy]` (a roamer's DMRC into HBlink4 with SCTP on, DMRP back).

### Hardware-in-the-loop tests

```sh
RDOF_BOARD=<board ip> python -m pytest -q hil-tests
```

Against a running board (pytest and websockets; paho-mqtt for the MQTT burst fallback), for the ESP glue the host
can't run (`web.c`, the bench API, `sdr_task.c`, netclock's and wifilist's handlers), checked against
[interfaces-http.md](spec/fw/interfaces-http.md) and [bench-api.md](spec/bench-api.md). They read status, settings,
the clock, Wi-Fi and the spectrum, open and end bench sessions and a receive lease, and run short spectrum, burst
and I/Q streams. Nothing transmits or changes settings. `RDOF_BOARD_MUTATE=1` adds bad requests to the endpoints
that store settings: each must be refused, with nothing stored and no restart. Skipped without `RDOF_BOARD`.

### Firmware builds

ESP-IDF v5.5 (built with v5.5.4). In each shell:

```sh
. $IDF_PATH/export.sh
cd esp_idf
```

Each profile is `sdkconfig.defaults` plus an overlay, with its own build directory under `build/`:

```sh
idf.py -B ../build/16mb -D SDKCONFIG=../build/16mb/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.16mb" build
idf.py -B ../build/w12 -D SDKCONFIG=../build/w12/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.16mb;sdkconfig.defaults.w12" build
idf.py -B ../build/4mb -D SDKCONFIG=../build/4mb/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.4mb" build
idf.py -B ../build/16mb -D SDKCONFIG=../build/16mb/sdkconfig -p <port> flash monitor
idf.py -B ../build/16mb -D SDKCONFIG=../build/16mb/sdkconfig size
curl -X POST --data-binary @../build/16mb/radiofoundry_gateway.bin http://<board>/api/ota
```

| Profile | Board | Partitions | Features |
|---|---|---|---|
| `4mb` | ESP32-S3 SuperMini (ESP32-S3FH4R2: 4 MB flash, 2 MB quad PSRAM) | `partitions-4mb.csv`: two 1.875 MB OTA slots, a 128 KB core dump | no SDR (`CONFIG_RDOF_SDR=n`, since 2026-11-01): a roamer or repeater on a modem or radio; no MQTT TLS; 16 KB log ring; Wi-Fi at 10 dBm at most (at 15 or more the SuperMini can brown out) |
| `16mb` | ESP32-S3 N16R8 (16 MB flash, 8 MB octal PSRAM; a quad PSRAM part needs `CONFIG_SPIRAM_MODE_QUAD`), RGB LED on GPIO48 | `partitions-16mb.csv`: two 4 MB OTA slots, a 128 KB core dump, 7.75 MB data | everything, MQTT TLS, 256 KB log ring, USB hubs |

| Overlay | On | What |
|---|---|---|
| `sdkconfig.defaults.w12` | `16mb` | the Meshnology W12: the LR2021 front end and its pins, GNSS, the OLED, the power key, the RGB LED on GPIO46 |
| `sdkconfig.defaults.roamer` / `.sdr` | any | one role only |
| `sdkconfig.defaults.rust` | any | the Rust modules in place of their C (`CONFIG_RDOF_RUST`, [rust/README.md](../rust/README.md)) |
| `sdkconfig.defaults.hub` | `4mb` | USB hub support (`16mb` has it) |
| `sdkconfig.defaults.dspfast` | `16mb` | experiment: the receive path's hot code in IRAM (`CONFIG_RDOF_SDR_DSP_IRAM`, about 33 KB of internal RAM) and the 64 KB data cache |

The USB host CDC-ACM driver (`espressif/usb_host_cdc_acm`), esp-dsp (`espressif/esp-dsp`) and mDNS
(`espressif/mdns`) are fetched on the first build and pinned in `dependencies.lock`; all are fetched for every build,
and a role that's off leaves its library unlinked. The image builds `-Os`; with `CONFIG_RDOF_SDR_FAST`, `sdr/`, esp-dsp and the DSP task's files
build `-O2`. `idf.py menuconfig` has the menus "RadioFoundry Gateway", "RadioFoundry Gateway common (common)" and
"RadioFoundry Gateway SDR (sdr)".

## Layout

| Path | What |
|---|---|
| `common/` | `rdof_common`: portable C11, libc and libm only. An ESP-IDF component under `ESP_PLATFORM`, a static library otherwise. Headers in `include/rdof/`; `src/radiofoundry/` holds RadioFoundry's FEC, CACH, LC and CRC code (MIT) |
| `sdr/` | `rdof_sdr`: the SDR receive and transmit path on top of `common/`, and the host tools in `cli/` |
| `rtlsdr/` | the RTL2832U + R820T / R828D driver: portable register logic, and `esp/rtl_usb.c`, the ESP-IDF USB host glue |
| `hostweb/` | the firmware's web page served from a host tool ([hostweb/README.md](../hostweb/README.md)) |
| `components/` | ESP-IDF components with nothing of the gateway in them: `netclock` (a disciplined UTC clock), `wifilist` (a prioritized Wi-Fi list with a setup access point), `tinysctp` (SCTP over UDP), `lr2021` (the Semtech LR2021), `gnss` (NMEA). Each has its own README and keeps its logic in plain C the host tests build |
| `rust/` | `rdof_rust`: modules of `common/` in Rust, linked in place of their C with `CONFIG_RDOF_RUST` ([rust/README.md](../rust/README.md)) |
| `esp_idf/` | the ESP-IDF project (`radiofoundry_gateway`) |
| `host-tests/` | the host build of `common/`, `sdr/`, `rtlsdr/` and the components' portable parts, the tests, the golden fixtures, the fake radios (`fake_radio.c`) |
| `systest/` | the system tests: `radiosim`, `radiolink.c`, `test_radiosim.c` |
| `hil-tests/` | hardware-in-the-loop tests (pytest) |
| `tools/` | `bench/` (bench API clients: `benchapi.py`, `rfgbench.py`), `pluto/` (the PlutoSDR build, bundle and bitstream), `config_check.py`, `hblink4_sctp_check.sh`, `rtl_fir_design.py` |
| `config/` | the settings' JSON schema (`rdof-config.schema.json`) |
| `docs/spec/` | the specs |
| `LICENSES/`, `PROVENANCE.md` | third-party license texts, and where each piece of copied or ported code came from |

Nothing in `common/` blocks or makes threads: the platform gives the modem client a port (write, wait for a reply,
a clock) and calls the transceiver's or repeater's `poll` every 20 ms. Platform code (USB, UART, I2C, Wi-Fi,
sockets, NVS, HTTP, MQTT) stays in each target.

`common/` modules:

| Module | What |
|---|---|
| `bits`, `fec`, `ambe` | Hamming, BPTC(196,96), embedded LC, trellis 3/4, rate 1, CRCs, RS(12,9), Golay(23,12), AMBE frames |
| `layer2`, `pdu` | sync, slot type, EMB, CACH, burst builders; LC, CSBK, data headers and blocks |
| `mmdvm` | MMDVM framing and the frame reader, config v1 / v2, replies, radio and SFR detection, RSSI, regeneration, CSBK preambles, `rdof_mmdvm_cap_rf_level` |
| `modem` | the modem client, the radio: modem and fixed tuners, RSSI maps, profiles |
| `txvr` | the roaming transceiver |
| `repeater`, `duplex` | the duplex and single frequency repeater, DCDM, burst regeneration, the CACH short LC |
| `split` | split duplex: a TX half and an RX half (RadioFoundry's 0xD6 extension) as one duplex modem |
| `hblink`, `hbpeer` | DMRC, DMRD, DMRT, DMRK, the HBlink sink; the peer's state machine |
| `tracker` | the call tracker: MMDVMHost-style JSON, raw bursts, frames for HBlink4 |
| `netcalls`, `status` | network calls, `tx`, status and Receiver messages |
| `site`, `idset` | the settings parser (version 3), ID sets, the resolved view (C only) |
| `siteplan` | one parse of the settings into what runs: units, devices, checks ([config-v3.md](spec/fw/config-v3.md) §12, §13.1) (C only) |
| `config`, `jsonp` | a unit's settings from the plan (`rdof_plan_radio`), what a board runs, the port checks; the JSON reader |
| `router`, `siteroute` | the call router: which network an RF call goes to, which paths a network call goes out on (C only) |
| `txpool` | the transmitter pool ([transmitters.md](spec/fw/transmitters.md)) (C only) |
| `slotgrid` | the UTC slot grid, `slot_timing`, the gate, the sample clock |
| `rawburst` | the binary raw burst: version 1 (49 bytes), version 2 (62, with the UTC start and grid slot) |
| `usbmatch` | which radio a USB device goes to (VID / PID, serial, hub port) |
| `oled`, `oled_sdr` | the frame, font and panel commands; the roamer's and the SDR's screens |
| `ipgw`, `ipgwcfg` | the IP gateway and its settings (C only) |
| `parrot` | the parrot: a call recorded and sent back |
| `json` | a compact JSON writer, byte for byte stable for the golden tests |
| `webapi` | the web API shared by the firmware and the host tools: `/api/status`'s shared parts and `caps`, the settings, `/api/log`, `/api/lastheard`, `/api/restart` |
| `jitter`, `lastheard`, `memplan` | the network jitter buffer; Last heard; the board's memory plan ([memory.md](spec/fw/memory.md)) |
| `bench*`, `sigmode` | the bench line protocol, its jobs, its guard and its route through the pool; signal modes ([bench-api.md](spec/bench-api.md) §14) |

`sdr/` modules (headers in `include/rdof/sdr/`):

| Module | What |
|---|---|
| `fft` | complex float FFT, mixed radix 4, 2, 3, 5 (`rdof_fft_*`; the firmware supplies esp-dsp's) |
| `iq` | cu8, cs8, cs16 to complex float |
| `channelizer` | overlap-save channelizer (N = 64 x decimation, hop 3N/4), Kaiser filter, per-channel NCO; `plan_center` |
| `chzfx`, `chzhw` | the PlutoSDR FPGA channelizer's bit-exact integer model and its frames; the block's driver (probe, start, frames, the TX interpolator) |
| `plutofpga` | a Pluto board from its facts, a bitstream's check (host only) |
| `fm` | RRC taps, discriminator (a polynomial atan2), the 81-tap matched filter, power |
| `demod` | squelch, sync search over the four families, 4FSK slicing, CACH, voice flywheel with EMB checks |
| `agc` | clip measurement, the gain law, level stats |
| `mod` | the DMR 4FSK modulator ("Modulator") |
| `sdrmodem`, `loopback` | the SDR as an MMDVM modem, and its end-to-end check |
| `receiver` | one tuner: channelizer, a demod and a call tracker per channel, AFC |
| `resample` | a rational polyphase resampler |
| `ppm`, `rate` | the crystal's error from the AFC and from the sample clock |
| `txguard` | the transmit stream's timekeeper for a host device |
| `spectrum`, `bench` | the page's spectrum; the bench's wideband history and taps |
| `modes`, `mode_fm`, `mode_am`, `fmgen`, `fmrx`, `amrx` | signal modes besides DMR: analog FM and AM, their generators and measuring receivers |
| `cli/` | `rdofsdr`, `rdofradio`, `rdofchz` and their device, serial, network and FPGA glue; `tools.cmake` (SoapySDR, libiio and libad9361 found, pffft and the web page linked) |
| `pffft/` | pffft (FFTPACK license) unchanged, and `rdof_pffft.c`: the host tools' FFT (`--fft builtin` turns it off); 2.6 to 2.9 times the built-in FFT on a Cortex-A9 |

## Firmware

One ESP-IDF app with two roles, started from the settings: the **SDR** (an RTL-SDR on USB or the W12's LR2021, the
SDR core on several channels: "The SDR role") and the **roamer or repeater** (an MMDVM modem: a RadioFoundry or
OpenGD77 radio in hotspot mode on USB or a UART, an MMDVM_HS on a UART; or the SDR's own transmitter). One image,
one settings document, one page.

### Roles

- **What runs** is every unit the settings make ([config-v3.md](spec/fw/config-v3.md) §12): up to two units that
  transmit (roamers or repeaters, `RDOF_MAX_RADIOS`) and one SDR.
- **Both at once**: the modem on a UART and the RTL-SDR on USB, or both on a powered USB hub (`"usb_hub": true`, a
  build with hub support). Without `usb_hub` and with both on USB, whichever device is plugged in runs; plugging in
  the other kind restarts the board into that role.
- **CPU.** The SDR's DSP task runs on core 1; with `"dsp_cores": 2` core 0 takes whole channels too. Core 0 runs USB,
  Wi-Fi, lwIP, MQTT, HBlink4 and the web server. The roamer's tasks are light (a 20 ms poll) and outrank the DSP
  helper.
- **RAM.** Nothing of a role is allocated until it starts. The memory plan places stacks and buffers at boot
  ([memory.md](spec/fw/memory.md)). The DSP's buffers go internal while `CONFIG_RDOF_SDR_INTERNAL_RESERVE_KB` stays
  free, then PSRAM; the buffers touched every sample come from the plan's hot arena.
- **Shared:** settings (NVS), Wi-Fi (`wifilist`), the clock (`netclock`), the web server and page, OTA, the log ring,
  one USB host (`usb_host.c`: each class driver is its own client and takes only its own VID / PID), MQTT and UDP
  setup (`net.c`). Each role has its own MQTT connection (its own status topic as the last will) and its own HBlink4
  peers.
- **MQTT publishing** goes through `net_mqtt_post`, a PSRAM queue with its own task, so the DSP and the radio loop
  never wait on the network (esp-mqtt holds its lock through a connect attempt of up to 10 s). QoS 0 messages while
  disconnected are dropped and counted (`mqtt_dropped`).

| File | Part |
|---|---|
| `main.c`, `settings.c`, `site_run.c`, `mem.c`, `wifi.c`, `web.c`, `net.c`, `usb_host.c`, `logring.c`, `crash.c`, `heard.c`, `slot_timing.c`, `status_led.c`, `oled_task.c`, `power.c`, `power_core.c` | the board |
| `ipgw_task.c` | the IP gateway |
| `roamer.c`, `radio_task.c`, `modem_port.c`, `usb_modem.c`, `uart_modem.c`, `split_port.c`, `roam_hblink.c`, `rx_path.c`, `roam_mqtt.c` | the roamers and repeaters (`CONFIG_RDOF_ROAMER`), a `roam_t` each |
| `sdr.c`, `sdr_task.c`, `dsp_esp.c`, `fir_s3.S`, `kbench.c`, `fe_rtlsdr.c`, `fe_sim.c`, `fe_lr2021.c`, `sdr_mqtt.c`, `sdr_hblink.c`, `selftest*.c`, `selftest/` | the SDR (`CONFIG_RDOF_SDR`) |
| `modem_sdr.c` | the modem port of a unit on the SDR |
| `bench_api.c`, `bench_line.c`, `bench_radio.c`, `bench_tx.c` | the bench API and line protocol |

### Feature gates

`main/Kconfig.projbuild`, menu "RadioFoundry Gateway":

| Option | Default | What |
|---|---|---|
| `CONFIG_RDOF_ROAMER` | y | the roamer and repeater: modem drivers, transceiver, repeater, SCTP |
| `CONFIG_RDOF_SDR` | y (n in `4mb`) | the SDR: `sdr/`, the RTL-SDR driver, esp-dsp, the self-test. Its options are under "The SDR role" |
| `CONFIG_RDOF_MQTT`, `_RAW`, `_TLS` | y, y, n (y in `16mb`) | esp-mqtt; raw bursts on `.../raw`; TLS with the CA bundle |
| `CONFIG_MQTT_PROTOCOL_5` (esp-mqtt's) | y | MQTT 5 and topic aliases, falling back to 3.1.1 |
| `CONFIG_RDOF_WEB_FULL` | y | the full page; off: a settings form only. Both carry the Wi-Fi and clock sections |
| `CONFIG_RDOF_OTA` | y | image upload from the page; needs two OTA slots |
| `CONFIG_RDOF_SOFTAP` | y | the setup access point |
| `CONFIG_RDOF_GNSS_EN_GPIO`, `_LEVEL` | -1 (48, 0 in `w12`) | a GNSS receiver's power switch; its PPS goes to netclock (`CONFIG_NETCLOCK_PPS_GPIO`, 41 in `w12`) |
| `CONFIG_GNSS_UART_RX_GPIO`, `_TX_GPIO`, `_NUM`, `CONFIG_GNSS_BAUD` | -1 (39, 38, 1, 9600 in `w12`) | the receiver's NMEA ([components/gnss](../components/gnss/README.md)): fix and position in the status, each second's time to netclock. Not started when a modem uses that UART |
| `CONFIG_RDOF_STATUS_LED_GPIO` | -1 (48 in `4mb` and `16mb`, 46 in `w12`) | the WS2812 status LED: red without Wi-Fi; orange while a configured device isn't working; else green |
| `CONFIG_RDOF_LOG_RING_KB` | 16 (256 in `16mb`) | the log kept for the page, in PSRAM when there is some; 0 = off |
| `CONFIG_RDOF_CRASH_TEST` | n | `POST /api/crash` crashes the board on purpose ("Crashes"); never in service |
| `CONFIG_RDOF_MODEM_USB` | y | the CDC-ACM driver for a radio on USB (VID / PID / interface settable) |
| `CONFIG_RDOF_MODEM_UART` | y | an MMDVM_HS or MMDVM board on UART1 or UART2 |
| `CONFIG_RDOF_HBLINK_SCTP` | y | the roamer's HBlink4 link over SCTP in UDP (tinysctp); off: UDP only |
| `CONFIG_RDOF_SCAN` | y | a roamer's scanning between calls |
| `CONFIG_RDOF_IPGW`, `_MAX_SOCKETS` | y, 8 | the IP gateway and the flows it may hold |
| `CONFIG_RDOF_OLED` | y | the OLED status screen (I2C) |
| `CONFIG_RDOF_OLED_BUILTIN`, `_SDA`, `_SCL`, `_POWER_GPIO`, `_POWER_LEVEL` | n, 8, 9, -1, 0 (y, 17, 18, 45, 0 in `w12`) | a board's own display: on with no `oled` key, its pins and power switch |
| `CONFIG_RDOF_BENCH`, `CONFIG_RDOF_BENCH_TX`, `CONFIG_RDOF_BENCH_LINE_PORT` | | the bench API, its transmit jobs, the line protocol's TCP port (7373; 0 = off) |

The components have their own menus ("netclock", "wifilist", "tinysctp (SCTP over UDP)"); the app uses their
defaults. Core options (`common/Kconfig`, "RadioFoundry Gateway common (common)") reach `common/` as
`RDOF_FEATURE_*` defines (`include/rdof/features.h`); the host build tests all on and all off:

| Option | What |
|---|---|
| `CONFIG_RDOF_FEATURE_TX` | transmitting: the roaming transceiver, regeneration, CSBK preambles (off: a receive-only board) |
| `CONFIG_RDOF_FEATURE_REPEATER` | the duplex and SFR repeater and DCDM (needs TX) |
| `CONFIG_RDOF_FEATURE_DATA_BLOCKS` | data block decode (trellis 3/4, rate 1, CRC-9, CRC-32) |
| `CONFIG_RDOF_FEATURE_TRACKER` | the call tracker and the HBlink sink (the SDR needs it) |

### Size

From `idf.py size`. Run it after a change that adds code or RAM, and update this table.

| Build | Date | App image | Free in its slot | DIRAM used (of 334 KB) |
|---|---|---|---|---|
| `16mb` + `w12` | 2026-11-05 | 2274 KB | 1.8 MB of 4 MB | 181.1 KB (54.3%) |
| `16mb` + `w12` + `rust` | 2026-10-27 | 2063 KB | 2.0 MB of 4 MB | |
| `16mb` | 2026-10-28 | 1960 KB | 2.1 MB of 4 MB | 167 KB (49.9%) |
| `4mb` (no SDR) | 2026-11-02 | 1279 KB | 641 KB of 1.875 MB | 134 KB (40.3%) |

- **`4mb` is full with the SDR.** With everything it went over its slot on 2026-10-29; it builds without the SDR
  since 2026-11-01. If it runs out again, drop the repeater, the OLED or the full page before giving up a slot.
- **`w12`** turns Wi-Fi's IRAM options off (its hot code runs from flash), which gives the heap about 27 KB, and
  keeps a 96 KB margin (`CONFIG_RDOF_MEM_MARGIN_KB`): with 80 and Wi-Fi in IRAM, the SDR and an `sdr_transceiver`
  together left Wi-Fi without memory. About 20 KB of internal heap is free while it streams.
- **The LR2021 component's `linker.lf`** puts its read and transmit paths in IRAM (6.7 KB): the reader takes 30% of
  core 0 with it, 36% without.
- **PSRAM, not in the table:** the transceiver (about 20 KB), the repeater (about 55 KB), the settings parser (under
  65 KB while parsing), the parrot's recording (34 KB, only when on), the bench line protocol (about 410 KB at rest),
  the sample ring, the log ring, mDNS (its task, records and packet buffers; 420 bytes of it internal: the task's
  TCB and its state).
- IRAM shows 100% in `idf.py size`: that's the fixed 16 KB vector / cache-miss region filling as usual.

### Boards

**ESP32-S3 SuperMini (`4mb`).**

- One USB-C, on the native USB (GPIO19 / 20), no UART bridge. The firmware runs it as the USB host, so the console
  is on UART0: TX GPIO43, RX GPIO44, 115200 (a 3.3 V USB-serial adapter). Without one, the page shows the log.
- Power from the 5V pin. The S3 doesn't switch VBUS: the device's 5 V must come from the OTG adapter. Check it reads
  5 V at the device end before plugging in.
- PSRAM is quad SPI in the package: the boot log says `Found 2MB PSRAM device`.
- Default pins: an MMDVM_HS on UART1 at TX GPIO5 / RX GPIO6, the OLED on SDA GPIO8 / SCL GPIO9.

Flashing:

1. Plug the USB-C into the computer (no OTG adapter).
2. Hold BOOT, tap RST, release BOOT: the ROM's download mode, `/dev/cu.usbmodem*` on macOS (`/dev/ttyACM*` on Linux).
3. `idf.py -B ../build/4mb -D SDKCONFIG=../build/4mb/sdkconfig -p <port> flash`. The first flash writes the
   partition table and OTA data too.
4. Tap RST if it stays in download mode, then put the OTG adapter and the radio on the USB-C.
5. Console: `idf.py -p /dev/cu.usbserial-XXXX monitor` through the serial adapter.

Later updates go over Wi-Fi (the page's Firmware update, or `POST /api/ota`). The settings carry over.

**N16R8 boards (`16mb`).** The USB-UART port flashes and carries the console (the bridge resets the board into
download mode); the OTG port is the device's. Many of these boards power VBUS on the OTG port only when a solder
jumper (often `USB-OTG`) is bridged.

**Meshnology W12 (`16mb` + `w12`).** The LR2021 front end is under "Front ends". The rest of the board:

- ESP32-S3R8 (8 MB octal PSRAM, so GPIO33 to 37 are taken), 16 MB flash, a 32 MHz crystal for the LR2021 (no TCXO),
  native USB only.
- **GNSS:** an 8-pin header for an L76K module: UART RX 39 / TX 38, WAKE 40, PPS 41, RESET 42 (active low), power
  switch 48 (active low). `w12` powers it, feeds its PPS to netclock and reads its NMEA at 9600 baud
  ([components/gnss](../components/gnss/README.md)): each second's NMEA time numbers the pulse's edges, so the pulse
  sets the clock with no NTP. Fix, position and satellites are in `/api/status` `gnss`. Checked 2026-10-24: a 3D fix,
  every edge after the first numbered, SDR decodes unchanged with the reader on.
- **OLED:** a 0.96" SSD1315 (SSD1306 compatible, 128x64 at 0x3C, yellow top 16 rows), on SDA 17 / SCL 18, powered
  through VEXT_EN (GPIO45, active low). On with no `oled` key (`CONFIG_RDOF_OLED_BUILTIN`); `"oled": false` leaves it
  off and unpowered. It shows the SDR's screen ([device.md](spec/fw/device.md), "The SDR's screen"), redrawn every
  0.5 s at most in one I2C write from core 0, taking no lock the DSP holds.
- **Power key and battery** (the enclosure version): the top button is the USER key (GPIO0), the bottom one RST.
  Nothing cuts the 3.3 V while a battery is in, so "off" is deep sleep ([device.md](spec/fw/device.md) §6): held
  2 s, the board stops the radios, sleeps the LR2021, turns off the front end's, OLED's and GNSS's supplies and
  sleeps; held 1 s it wakes (a tap goes back to sleep). The battery is read every 2 s through the switched divider
  (GPIO2 enables, GPIO1 reads, 390k over 100k): `/api/status` `power`. Under 3.3 V for 30 s the board turns off, if the battery
  has read 3.3 V or more since the boot; a board with no battery (it reads the charger's output, 1.8 to 3 V) never
  does: one reading under 2.5 V and it's wall-powered, battery "none", until the next boot.
  `POST /api/power_off` does the same as the key.

### More than one radio

Each unit that transmits is a `roam_t` (`app.h`): its own modem and port, transceiver or repeater, tracker, HBlink4
peer and MQTT connection. The OLED shows the first (with an SDR, the SDR's screen with the first one's TX row);
`/api/status` has `roamer` (the first) and, with two, `roamers`.

- **Ports.** Each device's `port`: `{"usb": "auto"}`, or `{"uart": 1, "tx", "rx"}` and `baud` (8N1, 115200 by
  default) ([config-v3.md](spec/fw/config-v3.md) §4.3). UART1 is `uart` 1, UART2 `uart` 2; UART0 is the console. A
  RadioFoundry radio without USB runs its hotspot on the programming cable's UART at 115200 (check the cable's
  levels are 3.3 V). Radios can't share a UART or a pin (`rdof_config_check_ports`).
- **A split** (a repeater whose `in` one modem hears and whose `out` another sends; [repeater.md](spec/repeater.md),
  [radio.md](spec/fw/radio.md)) is one unit on two devices. `split_port.c` runs each half on its transport and
  `rdof_split_t` between them. At most one half on USB. Not run on a board yet.
- **USB.** Two radios on USB need a hub, and each needs `port.serial` (its USB serial number, as the log prints it
  when plugged in) or `port.usb_port` (hub ports from the board down: `"2"`, `"1.3"`). A watcher client in
  `usb_host.c` gives each arriving device to the radio it matches (`rdof/usbmatch.h`), and each radio's CDC-ACM
  driver opens only that address. Settings refuse two USB radios that could match the same device.
- **Hubs** are ESP-IDF's (`CONFIG_USB_HOST_HUBS_SUPPORTED`; on in `16mb`, the `hub` overlay elsewhere). In v5.5.4 it
  has no overcurrent handling, no port debounce and no remote wakeup, and the S3 supplies no VBUS: use a powered hub,
  and expect a replug after a fault. Not run with a hub yet.
- **Host channels.** The S3 has 8; enumeration takes one, each open endpoint one. A hub takes 1, a RadioFoundry
  radio 3 (CDC-ACM), an RTL-SDR 1 or 2. Two radios and a hub fit; two radios, a hub and an RTL-SDR don't: put the
  second radio on a UART.
- **Bandwidth.** Full Speed carries about 1.2 MB/s for everything on the hub. An RTL-SDR at 288 kS/s takes
  576 KB/s, a radio a few KB/s.
- **RAM.** The first radio needs about 45 to 50 KB of internal RAM (task stacks, esp-mqtt's task, buffers). A second
  radio's stacks, tracker, MQTT state and receive buffer go in PSRAM, leaving about 20 KB it still needs internal.
  The memory plan places them at boot; a radio it refuses doesn't start and says why (`radio_error`). Two radios on
  a board without PSRAM are refused. Two radios and an SDR on the N16R8 haven't been measured.

### Settings

One JSON document, version 3 (comments allowed), stored in NVS and edited on the page (edit, Save and restart,
Export, Import). Every key, the checks and the schema: [config-v3.md](spec/fw/config-v3.md). It's opened and
checked in full before it's saved; `/api/resolved` shows it as resolved. A document without `"version": 3` is
refused.

Board keys besides the sections:

| Key | What |
|---|---|
| `oled {sda, scl, ...}` | the screen |
| `usb_hub` | `true` when the USB port has a hub |
| `wifi` | `{"networks": [{"ssid", "priority", "hidden", "password", "was"}]}`: wifilist's list |
| `ntp` | `{"servers": "pool.ntp.org", "poll_s": 300}`: netclock's |
| on the SDR device | `usb {transfers, transfer_kb}`, `dsp_cores`, `selftest`, `spike`, `sim`, `sim_ppm`, `idle_gate`, `learn_ppm` ("The SDR role") |

- `wifi` and `ntp` live in their components' NVS namespaces, not in the stored document: Export adds them, Save and
  Import hand them to the components. **Passwords never leave the board:** the exported `wifi` has none, and a
  network without `password` keeps the one stored for its name (or for `was`, its name before a rename). On another
  board, add `"password"` to each protected network.
- The page's Wi-Fi and Clock sections edit the same things without a restart.
- `hblink` `transport: "sctp"`: HBlink4 over SCTP in UDP, with `sctp_encap_port` (9899), `sctp_heartbeat_ms`
  (10000), `sctp_max_retransmit` (3) and `sctp_ttl_ms` (0): MMDVMHost's `SCTPHeartbeat`, `SCTPMaxRetransmit` and
  `SCTPTTL`. `sctp_ttl_ms` is a DMRD's lifetime (PR-SCTP); 0 keeps every burst. DMRC and DMRK are always reliable.
- A W12's settings: [config-v3.md](spec/fw/config-v3.md) §10.5. An S3's RTL-SDR takes an `rx.rate` of 288000 or
  240000.
- **First boot.** With no Wi-Fi networks, or none reached in 20 s, the board opens the access point
  `radiofoundry-XXXX` (password `radiofoundry`), page at <http://192.168.4.1/>, while the station keeps trying. A new
  board starts with a placeholder roamer (an OpenGD77 on USB, peer 9990301) and an SDR on five 438 MHz channels at
  288 kS/s, both on USB; a one-role build starts with its one.

### The IP gateway

Off unless the settings have an `ipgw` app. Radios' IPv4 datagrams sent as DMR packet data (ARS, text messages,
location, any UDP app) go to the network, and the replies come back over the air. Each message is put back together
from the bursts any receiver hears (the SDR's channels, each roamer's or repeater's radio), checked, then:

- **routes** send it to a set address: by private call (an id or range), talkgroup, source radio, or all, to
  `host:port` (or `host`, keeping the datagram's port), as the UDP payload or the whole datagram;
- **NAT** sends any other UDP datagram to the address in its header, from the board's address with a local port per
  flow, if the destination is allowed (private ranges, 12.0.0.0/8 and 13.0.0.0/8, loopback, multicast and broadcast
  are denied by default).

It takes the full IPv4 header (SAP 4) and the compressed UDP/IPv4 header (SAP 3), packets chained in one
transmission, and ping through NAT. Replies go back as unconfirmed packet data, through the transmitter pool on the
path the radio was last heard on, on its frequency and slot; an SDR-only board relays but can't answer. Confirmed
data is ACKed. Rate limits, a flow cap, an idle timeout and a size cap bound it; counters are in `/api/status`
`ip_gateway`.

```jsonc
"apps": [
  {"type": "ipgw", "pc": 9991,
   "routes": [{"name": "ars", "private": 9990, "to": "192.0.2.20:4005"}],
   "nat": true}
]
```

Keys: [config-v3.md](spec/fw/config-v3.md) §6.3; how it works and what's left: [ipgw.md](spec/fw/ipgw.md).

### Wi-Fi, the clock and MQTT

- **Wi-Fi** ([wifilist](../components/wifilist/README.md)): up to 8 networks with priorities. The board joins the
  highest priority one in sight (the stronger signal among equals), falls back down the list when one fails, and
  scans again when the connection drops. The page shows the connection, the list with each network's signal, the
  last scan and the last 8 failures with their reason.
- **The clock** ([netclock](../components/netclock/README.md)): UTC from a GNSS PPS, up to 6 NTP servers (chrony
  style: the crystal's rate learned and stored, offsets slewed in, outliers and falsetickers dropped), GNSS NMEA, or
  the browser. Every timestamp the board writes comes from it. The page's Clock section shows the servers table,
  the last 32 references and a residual plot. JSON timestamps are wrong until a source has set the clock.
- **MQTT topics** ([mqtt.md](spec/fw/mqtt.md)): the roamer or repeater publishes `{root}/{name}/{app}/json` (calls,
  network calls, the retained Receiver), `.../raw`, `.../tx`, and the retained `{root}/_sdr/{name}/status` with
  `{"state": "offline"}` as the last will; `app` is `roaming` or `repeater`. The SDR's are under "The SDR role".
- **Raw bursts:** `.../raw` carries each burst as 49 bytes (`rdof/rawburst.h`; [mqtt.md](spec/fw/mqtt.md), "Raw
  bursts"), as MMDVMHost's MQTTRaw does. An SDR with `slot_timing` utc sends version 2 (62 bytes, with the burst's
  UTC start and grid slot) while its clock is on the grid.
- **MQTT 5 and topic aliases** (`net.c`): a connection asks for MQTT 5 and falls back to 3.1.1 when refused. On each
  connect, aliases go to every channel's `raw` topic, then their `json`, up to the broker's Topic Alias Maximum,
  which the queue's task finds by trying (esp-mqtt doesn't report it). Only QoS 0 uses them: esp-mqtt resends QoS 1
  from its outbox as first built, so the `json` topics (QoS 1) don't. Aliases start over at every connect. A publish
  built just as the connection drops can reach the next connection with an alias it doesn't know; the broker then
  disconnects and the next connection starts over. With the SDR's 17-byte topic a burst is 70 bytes on the wire, 57
  with its alias.
- **mDNS and DNS-SD** (`dnssd.c`, espressif/mdns): the board answers as `BOX-XXXX.local`: the box's name as a host name
  and the base MAC's last two bytes (a box `w12`: `w12-c3d4.local`; `radiofoundry-XXXX` with no name), which is its
  DHCP host name too and lists `_http._tcp` (the page) and,
  when the bench line runs, `_rfgbench._tcp` with TXT `kind`, `ver` and `mac`. `rfgbench.py find` lists the boards
  from it, whatever address DHCP gave them.
- **`/api/status`**: `name`, `version` (`git describe --always --tags --dirty` at the last CMake configure;
  `idf.py reconfigure` refreshes `-dirty`), `idf`, Wi-Fi, clock, memory, the SDR's test results, then `roamer` and
  `sdr` for what runs. Every field: [device.md](spec/fw/device.md).

### Crashes

A panic (an exception, abort(), the interrupt watchdog) saves a core dump (ELF, every task) to the 128 KB
`coredump` partition and reboots. The dump stays until the next crash or until it's erased, so it may be from an
earlier image. The boot log says so in one line:

```
W (1018) crash: core dump in flash (44004 bytes, image 00390efb7): task dsp_helper, PC 0x4038c4b4, CacheError (71), EXCVADDR 0x938b8d90; GET /api/coredump
```

`/api/status` `crash` has the same (`elf`, the start of the crashed image's ELF SHA-256; `this_image`; `task`, `pc`,
`exccause`, `cause`, `excvaddr`, `backtrace`, abort()'s message as `details`), and the page links the dump. Keep each
flashed build's ELF (say `build/ota-elfs/<commit>-<profile>.elf`; `shasum -a 256` finds the one whose hash starts
with `elf`), then:

```sh
curl -o dump.bin http://<board>/api/coredump
esp-coredump --chip esp32s3 info_corefile -t raw -c dump.bin build/ota-elfs/<commit>-16mb.elf   # dbg_corefile: GDB
curl -X DELETE http://<board>/api/coredump                                                     # 404 after
```

A task in the dump takes about 1.8 KB (TCB, the used stack, a register note), so 128 KB holds about 70. A dump that
doesn't fit isn't written and the old one stays (the console says `Not enough space to save core dump!`): check
`this_image`. A board updated over the air keeps its old partition table until it's flashed over USB. To test the
round trip, build with `CONFIG_RDOF_CRASH_TEST=y`: `POST /api/crash` stores to address 0 from a task `crash_test`,
and after the reboot `crash` says `StoreProhibited` at `0x00000000`. The HIL suite does that with
`RDOF_BOARD_MUTATE=1`.

### Bring-up: a roamer or repeater

1. Power up with the serial console: PSRAM found, `USB host up`, `waiting for the radio (1FC9:0094)`.
2. Set Wi-Fi through the access point, then import settings with a roamer on a USB modem.
3. Plug in the radio in hotspot mode (with 5 V on VBUS): `radio connected`, the CDC descriptor dump, `modem: ...`,
   `tuned to ...` on the idle channel. If it connects and never answers, try `CONFIG_RDOF_USB_INTERFACE`.
4. Key a radio on the idle channel: start / end JSON in the log and on MQTT, and HBlink4 gets the stream (DMRC
   first).
5. Have HBlink4 route a call to the roamer (a private call to a radio last heard on another channel): DMRT, retune,
   0.3 s listen, DMRK "on air", the call out, `tx` and network-call messages, back to the idle channel. Watch
   "without a reply" in the status (missed status replies at key-up).
6. A radio check from another peer: two preamble CSBKs, the check, and the 0.75 s answer window.
7. SCTP (`hblink.transport: "sctp"`): HBlink4 needs `sctp_enabled` and its usrsctp backend with UDP encapsulation.
   The log says `SCTP association with ... up`; walk the board to the edge of Wi-Fi during a call: bursts arrive
   late rather than not at all, or are given up after `sctp_ttl_ms`; a long dropout ends in `peer lost` and a
   reconnect within 10 s of Wi-Fi coming back.
8. Both roles: an MMDVM_HS HAT on the UART and the dongle on USB; key the HAT's idle channel and an SDR channel at
   once, and watch the SDR's DSP load, `droppedChunks`, "without a reply" and internal RAM free.

### Scope

| Part | Status |
|---|---|
| DMR FEC, PDUs, AMBE frames, layer 2 | in; talker alias text, LRRP / GPS and packet data decoding aren't (a server on the raw bursts can do those) |
| MMDVM protocol: requests, config v1 / v2, SET_FREQ, CW ID, duplex | in |
| MMDVM over UDP (an OpenGD77 as a USB network adapter) | not supported: needs USB CDC-NCM host; refused in the settings |
| Radios: detection, SFR detection, profiles, RSSI maps, modem and fixed tuners, settle time | in |
| rigctld and command tuners | not supported: they need a computer; refused in the settings |
| The roaming transceiver: DMRT / DMRK, listen-before-talk, the jitter buffer ([roamer.md](spec/fw/roamer.md) 3.9), header / terminator repeats, orphans, CSBK lead-in, the answer window, TMS / confirmed data replies, CW ID, the transmit tail, the 2 s idle status | in; queues bounded (256 bursts a call, 4 early streams of 16 bursts) |
| Scan | in, `CONFIG_RDOF_SCAN` |
| The repeater: duplex, network per slot, RF priority, echo drop, hang, CW ID, short LC; SFR; DCDM (two simplex calls at once on one frequency, on the SDR modem; no stock MMDVM or OpenGD77 firmware does DCDM) | in; queues bounded (192 bursts a slot, 6 streams) |
| UTC slot timing | in (`slotgrid.c`); the gate on the board from netclock |
| Split duplex | in (`split.c`; on the board `split_port.c`). The board's SDR modem as a half: host-tested only. `rdofradio --split-tx`: run against a simulated radio, not a real one; several splits on one SDR (`--config`): host-tested only |
| Network calls, `tx`, status every 10 s and on change, Receiver, restart with backoff | in |
| Settings | version 3, checked in full. Not: `${VAR}` from the environment, ids unique across a box |
| HBlink4: DMRC, DMRD, DMRT, DMRK over UDP; the HBlink sink | in. Not HomeBrew's RPTL / RPTK / RPTC login |
| HBlink4 over SCTP | SCTP over UDP (RFC 6951, tinysctp), not kernel SCTP: lwIP has none. One stream, PPID 0, ordered, as MMDVMHost. HBlink4 takes it with its usrsctp backend (`sctp_encap: udp`) |
| MQTT | esp-mqtt; TLS behind `CONFIG_RDOF_MQTT_TLS`. The fallback to 3.1.1 hasn't met a broker without MQTT 5 |
| Radios a board | two units that transmit and one SDR |

Decoder notes: Golay(20,8) and QR(16,7) decode from tables of their 256 and 128 codewords (`layer2.c`), a lookup
when the word is clean, else the first codeword within 3 or 2 bits, the same answer as RadioFoundry's decoders
(checked over every word). Golay(23,12) decodes by syndrome search instead of an 8 KB table. OpenGD77 quirks
handled: the 0.5 s `TX_TAIL_S` after the transmitting flag clears, a missed status reply ridden out for 2 s, the 2 s
idle status that keeps it in hotspot mode.

## SDR core: sdr

`sdr/` is its own library (`rdof_sdr`, the ESP-IDF component `sdr`, requiring `common`): I/Q in, a multi-channel
channelizer, FM discriminator and matched filter, the 4FSK demod, AGC, a call tracker per channel; and the
modulator and SDR modem for transmit. A roamer-only build leaves it out.

Switches (`include/rdof/sdr/features.h`, Kconfig "RadioFoundry Gateway SDR (sdr)"; all on by default):

| Switch | What |
|---|---|
| `RDOF_FEATURE_SDR` | the whole library; off, it builds empty |
| `RDOF_FEATURE_SDR_AGC` | the gain control law (measurement and level stats stay) |
| `RDOF_FEATURE_SDR_BUILTIN_FFT` | the built-in FFT; off when the firmware supplies `rdof_fft_*` (esp-dsp) |
| `RDOF_FEATURE_SDR_RECEIVER` | the multi-channel receiver; needs `RDOF_FEATURE_TRACKER` |

`RDOF_SDR_FAST_ATAN2=0` swaps the polynomial atan2 for libm's. Every per-sample loop is single precision (the S3's
and P4's FPUs have no double); double is used only per block, per burst or at setup.

### Tests

`host-tests/sdr.cmake`: `rdof_sdr_tests` (every switch on), `rdof_sdr_tests_min` (every `RDOF_FEATURE_*` off, the
FFT through the hook), a `RDOF_FEATURE_SDR=0` build, `rtlsdr_tests` (the driver against a fake dongle that can fail
any one transfer, checked against [rtlsdr.md](spec/rtlsdr.md)), `sdr_selftest_tests` and `sdrmodem_tests`.

The vectors are committed in `host-tests/fixtures/sdr-vectors/`, from a float64 reference receiver over simulated
traffic (random AMBE bits, so no codec is needed), quantized to cu8 so the C reads the floats the reference did:

| What | Must match |
|---|---|
| IQ conversion, `plan_center`, AGC decisions, clip measurement | exactly |
| 162 bursts in nine demod cases (both slots, MS, late entry, data, noise, offsets to 800 Hz, the color code filter; chunks of 331 to 48000) | exactly, all 33 bytes |
| the receiver end to end at 288 kS/s (4 channels, AFC on): 44 tracker JSON and raw messages | byte for byte |
| FFT (sizes 1 to 3200) | 2e-6 of the largest bin |
| RRC and channel filter design | 1e-12 (double); the float taps within 1 ulp |
| channelizer output, with NCO changes mid-stream | 5e-5 of the output RMS |
| discriminator + RRC | 2 Hz |
| burst power, level center and deviation, AFC | 0.01 dB, 1 Hz, 1 Hz |

`rdof_sdr_edge_tests` covers what the vectors don't: an allocator that fails at each allocation in turn (each
failure reported, nothing left allocated); the idle gate on and off over the same recording; the two-core split;
`rdof_demod_skip`; bursts built symbol by symbol at the edges of the sync and slot type checks; two voice calls on
both slots; the AFC's limits; the ppm learning and the sample rate fit; the AGC against a plain reference of its law
over 300 random runs. `rdof_loopback_edge_tests` runs the modem loopback out of memory at each step.

Rules a real dongle's faults set (noise, a tuner several ppm off, IQ imbalance and its image, lost transfers,
overload):

- **AFC:** each chunk moves a channel half the way to where its bursts say, at most 300 Hz, both per 0.1 s of chunk,
  against the correction each burst's samples actually had (the last 32 are kept per channel).
- **Flywheel:** each voice burst's own level center (slice, then move the center by the mean slicing error, twice).
- **Image:** a voice sync more than 15 dB weaker than the call a slot is following can't take the slot over.

Numerics: the polynomial atan2 reads levels up to about 0.5 Hz higher than an exact one; the NCO advances per
block; the AGC's peak window is a 256-entry max-queue (when full it keeps the larger peak a little longer, so the
gain rises later, never sooner).

### Modulator

`mod.c` (`rdof/sdr/mod.h`), from ETSI TS 102 361-1 clause 10.2: symbols +3 +1 -1 -3 (dibits 01 00 10 11) at 648 Hz a
unit, 4800 a second, each an RRC pulse (alpha 0.2, 8 symbols): the demod's matched filter, so the pair is raised
cosine end to end. The shaped frequency drives a 32-bit phase accumulator; sin / cos from a 256-entry table and a
second-order step (error under 4e-7).

- **Timing** is in output samples, so TX is scheduled against the RX sample counter. A segment starts at the sample
  where symbol 0 peaks; symbol k peaks at start + k x fs / 4800. The demod reports a burst at symbol 0's peak +
  `RDOF_FM_DELAY` (40 at 48 kS/s), plus the channelizer's N / 8 inputs when it goes through one.
- **Keying:** half a symbol before the first symbol to half after the last, plus a raised-cosine ramp each side
  when asked (`RDOF_MOD_RAMP_UP` / `_DOWN`; 4 symbols by default). One burst a slot with both ramps keys 29.2 ms of
  30; a continuous downlink ramps only at the ends. Nothing queued: zeros, the phase still running.
- **Streaming:** `rdof_mod_queue` / `rdof_mod_queue_burst` (in start order, before the output reaches the segment's
  lead-in, else -1: late), then `rdof_mod_iq` (I/Q) or `rdof_mod_freq` (Hz and envelope, for a chip with its own FM
  modulator) for any n.
- **Cost:** about 100 cycles a sample on the S3 (an eighth of a core at 288 kS/s); 4.6 ns a sample on an M1 Pro.

`rdof_mod_tests` (vectors in `host-tests/fixtures/mod-vectors/`): the C against a reference modulator at 48, 288,
96 and 250 kS/s; C modulator to C demod through the channelizer, clean and with noise, every burst bit-exact at its
predicted position; queueing just in time gives the same samples as queueing everything first.

### CPU budget

`rdofsdr --bench` (Release, no sanitizers: `cmake -S host-tests -B build/bench -DRDOF_SANITIZE=OFF
-DCMAKE_BUILD_TYPE=Release`), simulated traffic, M1 Pro at an assumed 3.2 GHz. Mcycles per second of IQ; "busy" has
every channel in a call, "idle" is noise only:

| Stage | 10 ch, 1.536 MS/s busy | idle | 5 ch, 288 kS/s busy | idle |
|---|---|---|---|---|
| cu8 to float | 2.7 | 2.8 | 0.5 | 0.5 |
| forward FFT (2048 / 384 points) | 42.1 | 42.7 | 7.7 | 7.6 |
| per channel: bins x filter, 64-point IFFT, NCO | 12.2 | 11.7 | 6.6 | 6.6 |
| discriminator | 4.6 | 4.6 | 2.3 | 2.3 |
| RRC + power | 7.6 | 7.7 | 3.8 | 3.8 |
| squelch, sync search, slicing, flywheel | 21.8 | 2.9 | 9.7 | 1.4 |
| **total** | **91** | **72** | **31** | **22** |

**ESP32-P4 (estimate, no board).** Scaling the host's cycles by esp-dsp's 1024-point FFT against the host's (x8.7)
gives about 690 Mcycles/s for ten busy channels at 1.536 MS/s (86% of both 400 MHz cores) and 265 for five at
288 kS/s. So ten channels on a P4 need some of: an int16 front end (esp-dsp's `dsps_fft2r_sc16`), skipping idle
channels' FM stage (done: "The idle gate"), a symbol-rate sync search first, a fixed-point RRC, a polyphase
channelizer. A P4 firmware would also need the RTL2832U driver's High Speed glue (512-byte packets, 3 MB/s), and
the ESP32-C6 for Wi-Fi (esp-hosted over SDIO). Don't take code from `esp-rtl-sdr` (AGPL) or librtlsdr (GPLv2).

### SDR: not done

- Calibration, gain scans and recording on the board (the board learns its ppm instead).
- cf32 recordings in `rdofsdr` (live SoapySDR streams are CF32).
- The P4 firmware (no board).

## The SDR role

The SDR on a board: an RTL-SDR on the S3's USB, the W12's LR2021, or `sim`. It publishes each channel's calls as
MMDVMHost-compatible JSON and raw bursts on MQTT, with a status and a Receiver message per channel, and feeds
HBlink4 a receive-only peer per channel (or one through the box's peer).

| Path | What |
|---|---|
| `rtlsdr/` | the RTL2832U + R820T / R828D driver: `src/` is portable register logic over one control-transfer hook; `esp/rtl_usb.c` is the USB host glue (enumeration, control transfers, bulk IN into a PSRAM ring, the throughput spike) |
| `esp_idf/main/sdr*.c`, `dsp_esp.c`, `fir_s3.S`, `selftest*.c`, `kbench.c` | settings, the page's section (spectrum, waterfall), MQTT, HBlink4, the DSP task, the S3's DSP kernels (esp-dsp's radix-4 FFT, the forward FFT as P x 64 or P x 256, the int16 SIMD matched filter), the self-test |
| `esp_idf/main/selftest/` | the self-test clip (0.6 s, five busy channels at 288 kS/s) and its expected output |
| `esp_idf/main/fe_*.c` | the front ends |

### Why 288 kS/s

The S3's USB is Full Speed: 64-byte bulk packets, about 1.2 MB/s on the wire. cu8 at 1.536 MS/s needs 3 MB/s.
288 kS/s (576 KB/s) and 240 kS/s (480 KB/s) are the RTL's lowest rates that are multiples of 48 kS/s; they cover
230 or 192 kHz. With the 25 kHz DC guard the 461 group's 12.5 kHz channels fit 4 at a time, or 5 with one near DC.

- **The ceiling is 300 kS/s.** Measured on the SuperMini (the spike, 2026-10-21): 240, 250, 288 and 300 kS/s arrive
  whole with every transfer plan; 960 kS/s delivers 1152 KB/s (60%), 18 packets a 1 ms frame. The RTL has no rates
  between 300 and 900 kS/s.
- **The gap is the chip's.** The resampler ratio register (28.8 MHz x 2^22 / rate) is 28 bits with bit 27 standing
  for 2^28 too, so ratios 32 to 96 can't be written: 480, 528 and 576 kS/s came out at 232, 243 and 253 kS/s, and
  384 and 432 kS/s gave about 2.6 MS/s and a stalled stream (2026-10-21).
- Decimating 900 kS/s doesn't help (the bytes cross USB first), nor does aliasing channels in on purpose (the
  RTL2832U's fixed decimation filters remove them). More than 300 kS/s needs High Speed USB.
- The RTL drops samples silently when its FIFO overflows. The driver sets the bulk FIFO to 64-byte packets (others
  set 512). The spike measures what really arrives.

### The RTL-SDR driver: where its facts come from

Written from a fact spec, [rtlsdr.md](spec/rtlsdr.md), without librtlsdr or any GPL driver: `rtlsdr/src/rtl2832u.c`
and `rtlsdr/src/r820.c` (`include/rtlsdr/r820.h`). The spec cites, for each register write, Realtek's RTL2832U
datasheet (Rev. 1.4), Rafael's R820T2 register description, jtarrio/webrtlsdr (Apache-2.0, `src/rtlsdr/` at
9b3f587) and our bench findings; its section 3 lists what no source settles. `rtlsdr/esp/rtl_usb.c` and
`rtlsdr/src/gaps.c` are ours.

- **Tuning:** the PLL's fraction is rounded, and the DDC's IF is set to the IF the PLL really gives, so the center
  lands within the DDC's 6.9 Hz step. The sign of that correction is still to be checked with a carrier (spec G.4).
- **IF filter:** 3.57 MHz IF at every rate, webrtlsdr's filter setting; a narrower one waits for the sweep in spec G.2.
- **Gain:** 31 manual steps, LNA and mixer codes alternating, the VGA fixed at 16 dB; the steps' dB are
  webrtlsdr's estimate (0 to 52.5 dB) until measured (spec G.3).
- **FIR:** the DDC's low-pass is our own design (`tools/rtl_fir_design.py`).
- **Blog V4:** the R828D's input switching and the HF upconverter on GPIO5 follow webrtlsdr; not run on one.

### Front ends

The SDR reads samples through `sdr/include/rdof/sdr/frontend.h`; the drivers are `esp_idf/main/fe_*.c`. The SDR
device's `driver` picks one.

- **`rdof_fe_caps_t`:** the sample format (cu8, cs8, cs16), rates (a list or a range), tuning range, usable share of
  the band, DC guard, gain steps, USB or not; for transmit: rx-only / half / full duplex, the TX mode (I/Q,
  frequency, native 4FSK), TX rate and format, TX tuning and power range, the turnaround.
- **`rdof_fe_ops_t`:** init, wait, tune (rate, center, ppm; returns the real rate), set_ppm, set_gain / gain_steps,
  start, stop, read (with the bytes filled for gaps), gone, stats, identity, error; optional post, spike, tx_set.
- **`rdof_fe_check`** checks a rate and the channels against the caps when settings are saved.
- **Resampler** (`rdof/sdr/resample.h`): a rational polyphase filter (Kaiser window, 32 taps a phase) for a front end
  whose rate isn't a multiple of 48 kS/s. Flat to 0.001 dB over the usable band, aliases under -60 dB.

| Driver | Status |
|---|---|
| `rtlsdr` | the default (`fe_rtlsdr.c` over `rtl_usb.c`); receive only |
| `lr2021` | the W12: receive, half duplex transmit (`w12` overlay; below) |
| `sim` | the self-test clip in a loop, in real time (`fe_sim.c`); full duplex into its own samples |
| `sx1255` | a plan: Semtech's 400 to 510 MHz I/Q transceiver with separate RX and TX synthesizers (full duplex), I/Q over I2S. Nothing written |
| `soapy`, `iio` | host only (`rdofradio`, `rdofsdr`) |

Provenance: the front-end interface, the drivers, the resampler (a textbook design), the LR2021 driver (Semtech's
datasheet DS.LR20xx Rev. 2.2, and for raw I/Q the facts in [lr2021-iq.md](spec/lr2021-iq.md)) and the USB matcher
are written here from those specs ([PROVENANCE.md](../PROVENANCE.md)).

#### LR2021 and the Meshnology W12

`components/lr2021` is the chip's driver and its raw I/Q session (`lr2021_iq.c`; its README says how it's kept in
step with SoapyLR20xx's copy); `fe_lr2021.c` is the front end. Semtech documents no raw I/Q output; ROM 1.24 has
one, and its facts are in [lr2021-iq.md](spec/lr2021-iq.md). A receive session (spec 2.2): check ROM 1.24 and no
active patch, set the receiver up, route, map the 1 KB RX FIFO, SetRx continuous, then the gain step.

Modes, by the settings' `sample_rate`:

| Mode | Rates | How |
|---|---|---|
| Narrow | up to 120000 | the LoRa modem at 31.25 kHz bandwidth, int16 I/Q at 125 kS/s (source 2), resampled on the DSP core |
| Wide | 120001 to 576000 | the FSK modem, its bandwidth as the rate code (R2.19), the route's rate converter making the rate exactly (source 0, int8; R2.21). No resampler. 576000 gives about 355 kHz of channels |
| Wide int16 (`device.settings.iq_bits` 16) | 120001 to 240000 | the FSK modem's own rate (source 2, int16), resampled. About 50 dB more headroom than int8; 240000 gives about 77 kHz of channels. 288000 needs the SPI at 20 MHz (R2.29) |

- **Gain:** `rx.gain_db` is the nearest step's dB (`rdof_lr2021_gain_db`); unset, step 13 (53.5 dB), fixed, so each
  TDMA burst's level is at a known gain (the chip's AGC, `"auto"`, can change it between slots). At 446 MHz on the
  W12 step 13 has the best SNR, about 75 dB from noise to full scale (R3.50, R3.51). A burst's RSSI is its dBFS
  minus the step's dB + 5 (`RDOF_LR2021_RSSI_DBM`, R3.52): dBm at the chip's input, after the W12's LNA and SAW.
- **The reader** on core 0 runs `lr2021_iq_poll`: in the narrow mode it sleeps on the FIFO IRQ (DIO7); in the wide
  modes it reads by the FIFO's level and sleeps until enough is in, reading when 600 us is left before it fills
  (`READ_SLACK_US`). Each read is n - 2 bytes, n a multiple of 4, so the SPI frame is a multiple of 4 and needs no
  DMA bounce buffer. Its log lines go out from a timer (one `ESP_LOGI` in the reader held it past the FIFO).
- **Sessions:** a loss, a retune or a new ppm ends the session and starts a new one (CalibFE overwrites the
  extended FIFO, R2.28). A FIFO loss is filled with zeros so the stream keeps real time.
- **The sample clock:** each read's sample count and end time time the chip's crystal (`sdr_rate_block`), as the
  RTL-SDR's transfers do ("The tuner's ppm, learned").
- **576 kS/s at most:** the FIFO can't grow past 1 KB (R2.22); at 768 kS/s the reader lost about 30 FIFO-fulls a
  second at 16 MHz SPI, and at 26.67 MHz it kept up but starved core 0.
- **Dynamic range:** int8 clips at ±32 (R2.26), about 20 dB from noise to clipping; int16 has 50 dB more at twice the
  SPI bytes.
- **DMR 4FSK in the chip:** ROM 1.24's undocumented 4-level FSK receives DMR with our patch RAM's correlator table
  (`lr2021_fsk4_dmr_rx`, `POST /api/lr2021/dmr_rx` on a held chip; R3.40, R3.42), at about 6% symbol errors against
  about 1% from our demod on the chip's I/Q (R3.41), so the gateway receives from I/Q.
- **One synthesizer:** half duplex only (RX to TX 107 us, TX to RX 91 us).
- **Range:** the LF path is 150 to 960 MHz in the datasheet; it receives from 100 MHz (R2.38), so the front end
  starts there, transmit too (not measured under 150). On the W12, 126-134 MHz has a broad noise rise of up to 28 dB.
  LF PA -9.5 to +22 dBm in 0.5 dB steps.
- **AM:** the bench's `tx am` goes out as polar I/Q (`LR2021_TX_IQ_POLAR`): the envelope drives the PA through
  `lr2021_polar_iq8` and the W12's AM table, measured at 920 MHz and +22 dBm. The PA's floor rises at low power, so
  deep modulation flattens there. Runs are at most 32,400 samples (R3.43).

`CONFIG_RDOF_SDR_LR2021` builds it; "LR2021 pins and options" in menuconfig defaults to the W12's pins: NSS 8, SCK 9,
MOSI 10, MISO 11, NRESET 12, BUSY 13, IRQ (DIO7) 7, PA_EN 4, SPI at 16 MHz (20 and 26.67 MHz read clean, R2.23;
40 MHz breaks BUSY).

**The W12's RF front end** (schematic W12-MB-V0.2): the antenna goes through a GC1109 front-end module, then a
902–928 MHz SAW filter, to the LR2021's LF input. The GC1109 has an LNA (17 dB, NF 2 dB in 860–930 MHz), a PA and a
transmit bypass, but no receive bypass. DIO9, 10 and 11 drive its CTX, CPS and CSD: receive is CTX 0, CSD 1;
transmit is CTX 1, with CPS 1 for the PA (inside `CONFIG_RDOF_LR2021_PA_LO_MHZ` to `_HI_MHZ`) or 0 for the bypass.
GPIO4 powers it. So the W12 receives well only in 902–928 MHz; elsewhere the SAW costs about 46 dB, and at 439 MHz
only strong signals decode. Transmit at 439 MHz through the bypass gives about +3 dBm.

### Transmit: the SDR as a radio

The SDR modem (`sdr/src/sdrmodem.c`, `rdof/sdr/sdrmodem.h`) is an emulated MMDVM modem, protocol 1, describing
itself as "RadioFoundry Gateway SDR modem (MMDVM_HS compatible) DCDM", written from the protocol as RadioFoundry's
`docs/spec/mmdvm-protocol.md` (MIT) sets it down. `modem_sdr.c` is the transport under the roamer's or repeater's
`rdof_modem`, as `uart_modem.c` is for a HAT, so `txvr.c` and `repeater.c` run unchanged. On the board it runs on
`sim` (full duplex) and the LR2021 (half duplex); on a host, in `rdofradio`. The roamer on it is the router's pool
transmitter ([transmitters.md](spec/fw/transmitters.md)).

- **Settings:** an SDR device that transmits ([config-v3.md](spec/fw/config-v3.md) §4.1): on simplex frequencies
  it's a transceiver unit; a repeater app on it is a duplex repeater (a full duplex front end); a DCDM frequency it
  sends on is a DCDM gateway. `tx.power_dbm` (10 by default) is the power. Saving refuses a front end that can't
  transmit, `full` on a half duplex front end, a frequency the SDR doesn't hear, and a TX frequency outside the
  front end's range.
- **The modem:** SET_FREQ picks the receiver channel for the RX frequency (NAK if none) and asks the front end where
  the TX carrier goes. SET_CONFIG with duplex on a half duplex front end is NAKed (except as a split half). CW ID is
  ACKed and not sent. DMR_SHORTLC is taken in the 9-byte encoded form, or the 5-byte form if its CRC checks.
- **Half duplex:** bursts from DMR_DATA2, one every 60 ms, ramped; the status reports 8 bursts of space. What the
  tuned channel hears goes up as DMR_DATA2 (idle bursts don't). A heard burst within half a slot of one it sent is
  its own and is dropped; the AFC and the ppm learning skip those too (`rdof_sdr_rx_t.muted`). DMR_LOST2 half a
  second after a call stops without a terminator.
- **Full duplex:** DMR_START keys a continuous BS downlink, slot 1 first: each slot a CACH and the slot's queued
  burst or an idle burst. The CACH carries the short LC (Null until DMR_SHORTLC; a new one starts on a slot counter
  that's a multiple of 4) and its AT bit is the other channel's busy state. Heard bursts go up as DMR_DATA1 or 2 by
  their slot (aligned timing: a burst under outbound TSx is TSx). Needs `RDOF_FE_FULL_DUPLEX`: `sim` on a board, a Pluto or another I/Q device in `rdofradio`.
- **Dual capacity direct mode** (SET_CONFIG simplex with 0x40; [repeater.md](spec/repeater.md) §13,
  [radio.md](spec/fw/radio.md) 1.6): two simplex calls on one frequency, one a slot. DMR_DATA1 and DMR_DATA2 queue
  TS1 and TS2, each burst ramped on its own, TS2 30 ms after TS1. The slot timing follows the TS1 / TS2 bursts it
  hears (stale after 1 s), else its own.
- **A split half** (`RDOF_FEATURE_REPEATER`; the description ends " SPLIT1"): RadioFoundry's split duplex extension,
  either role. As the RX half it labels what it hears from SET_EPOCH's epoch; as the TX half it runs the full duplex
  downlink on a slot clock from SET_ROLE 1, with SET_AT's AT bits, the prebuffer, the key-up limit and cool-down.
  The board doesn't run an SDR half yet.
- **Timing.** The receiver's sample counter is the clock: the front end pulls the modulator's samples by the same
  index as the samples it reads (`rdof_fe_tx_source_t`, scaled by `tx_rate / rate`), and a burst is queued 24
  symbols (5 ms) ahead of the pull. The receiver reports one at `start / D + 40 + 8` (48 kS/s), which the mute
  compares.
- **UTC slot timing** (`slot_timing: utc`, [slot-timing.md](spec/slot-timing.md)): `slot_timing.c` stamps each
  chunk on netclock's UTC, fits the sample clock through the earliest arrivals (`rdof_sample_clock_t`) and runs the
  gate from `netclock_quality()`. Unkeyed, the duplex slot clock snaps to the grid; keyed, it's steered a sample a
  slot; the carrier is corrected by the crystal's ppm. Host tests: `test_utc_tx` (a crystal 25 ppm fast and 0–2 ms of
  arrival jitter: 30 s keyed within 97 µs of the grid), `test_utc_rx`, `test_split_tx_utc`, `test_pair_utc`.
- **The LR2021's part:** half duplex, `RDOF_FE_TX_FREQ` at 48 kS/s, tuned by `tx_tune` within 1 MHz of the
  receiver's center. The reader pulls the modulator 40 ms ahead, 2 ms a turn; each keyed span goes out as one run of
  frequency samples (fdev 2048) through `lr2021_iq_transmit`, its SetTx timed to about ±10 us (R3.48), and the
  receiver restarts with no CalibFE (R3.16). Every channel is muted while it sends. A log line a transmission: runs,
  late, early (R3.17), underflows, the key-up error.
- **Measured:** on the W12 (2026-10-24) a 29-burst call through `WS /tx via=modem` at +10 dBm on 438.2625 MHz
  decoded whole on the SDR bench (0 FEC failures, about 0.9% bit errors, within 26 us of the TDMA grid); a 3 s call
  (2026-10-27): 50 of 50 bursts, spaced 59.96-60.04 ms.
- **Tests:** `sdrmodem_tests`: the CACH, each split role, the split matrix (a radio or an SDR modem as each half,
  90 s of inbound bursts labeled right, the RX half's epoch within 100 µs of the TX half's slot starts), the
  loopbacks (`rdof_sdrmodem_loopback`: a real `rdof_modem` client, three CSBKs heard back bit-exact and muted,
  another station's bursts in the gaps reaching the host), DCDM end to end.
- **Not done:** CW ID; TX power control on `sim`; an SDR split half on the board; the UTC slot in the board's MQTT
  raw bursts (the modem labels it, the burst doesn't carry it).

### SDR settings

An SDR device ([config-v3.md](spec/fw/config-v3.md) §4.1): `name`, `driver`, `rx {rate, center_hz ("auto" or MHz /
Hz), gain_db (dB or "auto"), agc (true or {min, ...}), ppm, biastee}`, `rssi_offset`, `afc`, `status_seconds`. It
hears every frequency in its window (or the ones pinned to it); `callsign`, `location` and `slot_timing`, the `mqtt`
network (app `sdr`) and the `hblink` network (a peer per channel with `identity: per_path`) apply.

Board keys on the device:

| Key | Default | What |
|---|---|---|
| `usb {transfers, transfer_kb}` | 8 x 8 KB | the RTL-SDR's bulk transfers |
| `dsp_cores` | 1 | 2: core 0 takes channels too |
| `selftest`, `spike` | false | run the self-test or the USB spike at boot |
| `sim` | false | no dongle: the self-test clip (five busy channels around 438.1475 MHz at 288 kS/s) looped in real time; a chunk more than half a second late is dropped and counted. Needs `CONFIG_RDOF_SDR_SELFTEST` |
| `sim_ppm` | 0 | with `sim`, a tuner this many ppm fast |
| `idle_gate` | true | "The idle gate" |
| `learn_ppm` | true | "The tuner's ppm, learned" |

- With `"center": "auto"` and channels that don't all fit, the board takes the best window (`rdof_plan_window`) and
  leaves the rest out, with a log line each. The page's "As resolved" shows the plan.
- `droppedChunks` counts samples lost in the ring (the DSP fell behind); `overflows` failed USB transfers. Samples
  the RTL drops show only as a shortfall in "USB samples".
- A board without a `name` is "radiofoundry".

Firmware options, under `CONFIG_RDOF_SDR`:

| Option | Default | What |
|---|---|---|
| `CONFIG_RDOF_SDR_HBLINK` | y | a peer a channel |
| `CONFIG_RDOF_SDR_FAST` | y | `-O2` for `sdr/`, esp-dsp and the DSP task |
| `CONFIG_RDOF_SDR_ESP_DSP` | y | esp-dsp for the FFTs and the matched filter, each checked against the built-in code when the role starts and left out if they disagree |
| `CONFIG_RDOF_SDR_TWO_CORES` | y | allows `"dsp_cores": 2` |
| `CONFIG_RDOF_SDR_HELPER_STACK` | 6144 | the helper task's stack (`helper_stack_free` in the status) |
| `CONFIG_RDOF_SDR_CHUNK` | 4096 | samples a chunk |
| `CONFIG_RDOF_SDR_RING_KB` | 256 (1024 in `w12`) | the sample ring, in PSRAM |
| `CONFIG_RDOF_SDR_INTERNAL_RESERVE_KB` | 40 (64 in `16mb`) | internal RAM left free before DSP buffers go to PSRAM |
| `CONFIG_RDOF_SDR_SELFTEST`, `CONFIG_RDOF_SDR_SPIKE` | y | the self-test (and its clip) and the USB spike |
| `CONFIG_RDOF_SDR_LR2021` | n (y in `w12`) | the LR2021 front end |

**Memory.** For five channels with 4096-sample chunks the DSP takes about 150 KB (the demods' buffers, 22 KB each,
are the bulk). Buffers touched every sample (the channelizer's FFT input and output and twiddles) come from the
memory plan's hot arena, sized at boot by a dry run of the receiver's set-up ([memory.md](spec/fw/memory.md)): with
the FFT input in PSRAM the W12 at 576 kS/s lost 6–48 of 131 bursts a call. The chunk buffers, the arena and the DSP
stacks are allocated at boot before Wi-Fi, while the heap is whole.

**Two cores** (`"dsp_cores": 2`): core 1 runs the forward FFTs (shared by every channel), then both cores take
channels end to end (each channel's share of the channelizer, then its demod); trackers and AFC run on core 1 after.
The split starts at `round(C / 2 - 0.6)` channels on core 1, and every 64 chunks it tries one channel more on the
less busy core, keeping it if the next 64 chunks were 2% faster. Core 1 always keeps at least one channel, and "all
on core 1" is a choice too. The status's `dsp_split` shows where it is. The output is the same either way.

#### The idle gate

An idle channel costs nearly as much as a busy one: the discriminator, matched filter and sync search run on
noise too. Each chunk the channelizer measures each channel's power against what noise alone gives there (`rdof_chz_t`
`level`; the band's noise is the 25th-percentile bin of a Hann-windowed spectrum from the channelizer's own FFT).
Noise alone reads 1 to 2. A channel skips its demod for a chunk when that chunk and the one before stayed under 3,
it isn't in a call, it had no burst in the last 2 s, and it isn't in its probe (0.1 s in every second, staggered).
The first 2 s run everything. A skipped chunk goes into the demod as zeros, and the FM stage is warmed up on the last
82 samples before it runs again, so the output is exactly what it would have been (`rdof_sdr_edge_tests` compares
with and without). On a 7 s scene with channels idle for seconds, 37% of channel-chunks were skipped. The status's
`idle` is the share skipped. Off with `"idle_gate": false`.

#### The tuner's ppm, learned

`rdof/sdr/ppm.h`, `rdof/sdr/rate.h`. The tuner's crystal error comes from one of two sources:

- **The sample clock** (with a time reference: NTP, a GNSS PPS or NMEA). The RTL2832U's samples and the R820T's LO
  share one 28.8 MHz crystal, and the LR2021's rate converter runs from its crystal, so the rate the samples arrive
  at gives the error. The USB driver stamps each transfer (the LR2021 each read) with esp_timer and the samples sent;
  the least late arrival in each 10 s bin tracks the stream, and those minima are fitted with a line over up to 30
  minutes. A bin more than 200 us off the line means samples were dropped: the fit starts over. The board's own
  crystal is taken out with netclock's rate (`netclock_rate`). The fit is the learned value once the board has
  settled, its error is 0.3 ppm or better and it spans 5 minutes.
- **The AFC**, only with no time reference at all. Every 5 s the board takes the channels with a burst in the last
  30 s; when two or more agree within 1 ppm of their median, the median is the estimate. A heard carrier's AFC is
  that transmission's error plus the board's, and on a bench it is the radio under test, so with a reference it's
  never used; an AFC value learned before a reference came is dropped.

How it's applied:

- The chip moves by whole ppm once 0.75 ppm is left for 60 s (`RDOF_PPM_MOVE`), and every channel's correction moves
  the other way, so nothing jumps. The channels take the rest, the sample clock's share included where the front end
  doesn't correct its rate (the LR2021: a channel 300 kHz from the center, 32 ppm off, is 9.6 Hz further).
- The board's best value (learned, else the table, else the setting) also goes to the front end (`set_xtal_ppm`):
  the W12's transmit carrier is set to it, fraction and all. The bench's reports use it ([bench-api.md](spec/bench-api.md)
  14.7).
- **The temperature table** (netclock's `nc_tcal_t`, NVS namespace `rdof_ppm`, key `cal` plus a hash of the tuner's
  serial): once settled, the sample clock's value goes into the 2 C bin for the die's temperature, averaged with
  what the bin had, at most every 2 hours a bin in a run. At boot the table at the die's temperature (interpolated)
  sets the chip and the channels; until the run learns its own, a settled board follows the table as the room
  changes (when it differs by 0.25 ppm). The board's die stands in for the room: neither the RTL2832U nor the
  R820T has a documented temperature sensor.
- **Settled** means the die temperature stayed within 3 C and the sample rate over the last 5 minutes within
  0.2 ppm, for 10 minutes (netclock's `nc_settle_t`). The die went from 41 to 51 C over the first 10 minutes on the
  bench while the rate moved 1.2 ppm. Nothing is saved before.
- **Tolerance:** each report gives `ppm_tol` (from the source: the clock fit's error and recent drift, 1 ppm for the
  AFC, 0.5 for the table) and `freq_tol_hz` at its frequency.
- The RTL's sample frequency register stays 0 (`fixed_sample_rate`): on an R820T dongle it didn't follow its formula
  (+10 moved the rate by -3.4 ppm, [rtlsdr.md](spec/rtlsdr.md) D1.8).
- Sign: in the RTL convention (librtlsdr, SoapySDR) a positive ppm fixes a fast crystal.

Status: `ppm` has `learning`, `rtl` (whole ppm set), `learned`, `by` (`clock` or `afc`), `channels`, `steady_s`,
`saved`, `settled`, `die_c`, `die_spread`, `die_tol`, `clock` (`ppm`, `err`, `trusted`, `raw`, `span_s`, `rms_us`,
`steps`, `spread`, `spread_tol`, `board`, `board_learned`), `table` and `expected`. The page shows it under "Tuner
ppm". In the host test, a dongle 37.2 ppm fast with 0.1 to 2 ms of USB latency, 30 ms stalls and a silent 512-sample
drop comes out within 0.003 ppm over 30 minutes. On NTP over Wi-Fi alone the board's own rate takes an hour or two
to trust (about 2 ms of jitter); with a GNSS PPS, minutes. With `"sim": true, "sim_ppm": 6.5` the board shows it
working.

### Bring-up: the SDR

Build a profile with the SDR (`16mb`, `w12`). On an N16R8 the USB-UART port flashes and carries the console; the OTG
port is the dongle's.

1. **Boot, no dongle.** `SDR ...: 5 channels at 288000 S/s`, then from esp-dsp `FFT ... : in use` and `matched
   filter: int16 SIMD` (each checked against the built-in code; `LEFT OUT` means it didn't match and the board runs
   slower), `USB host up`, `waiting for an RTL-SDR (0BDA:2838)`.
2. **Self-test** (the page's "Run the self-test", or `"selftest": true`): the board restarts and logs `output, one
   core: PASS: 53 messages ..., CRC 283d7439 ...`, the same on two cores and with each fault in the clip (+4 kHz,
   noise to about 12 dB C/N, overload x4); a stage table in Mcycles per second; the FFT and kernel timings; a load
   test (`"selftest_load": {"channels": 8, "rates": [288000, 960000]}`); `prof` lines per core; the transmit
   loopbacks; and a verdict: `one core keeps up`, `one core is tight: set "dsp_cores": 2`, or `one core can't keep
   up`. FAIL means the board's DSP doesn't give the expected output: report it with the log.
3. **Plug in the RTL-SDR** with 5 V on VBUS (it draws about 280 mA): `RTL-SDR 0BDA:2838 ..., Full Speed`, the
   descriptor dump, `R820T tuner, bulk FIFO set for 64-byte packets`, `tuned 438.14750 MHz at 288000 S/s ...`,
   `streaming: 8 transfers of 8192 bytes in flight`.

   | Log | Meaning |
   |---|---|
   | nothing after `waiting for an RTL-SDR` | never enumerated: check VBUS, the adapter, the cable |
   | `XXXX:XXXX isn't an RTL2832U: not ours` | another device |
   | `rtl_open failed (-1/-2)`, `control ... timed out` | control transfers fail; replug |
   | `rtl_open failed (-4): no R820T / R828D tuner answered` | an E4000 / FC0012 / FC0013 dongle: not supported |
   | `rtl_open failed (-6): tuner init failed ... PLL didn't lock` | report it with the log |

4. **USB spike** (the page's "Run the USB spike"): 5 s at each of 288000, 250000, 240000, 300000 and 960000 S/s with
   three transfer plans: `spike: 288000 S/s, 8 x 8192 bytes: 576.0 KB/s of 576.0 (100.0%), 0 errors ...`. 99.5% or
   more at 288000 is fine; keep the plan that did best (`usb {transfers, transfer_kb}`). `SAMPLES LOST` at 288000:
   try more or larger transfers, or 240000 (4 channels). 960000 always loses: it measures the ceiling.
5. **Receive.** Set Wi-Fi, MQTT and the channels, key a radio. On the page: "USB samples" near 100%, "DSP load"
   under about 80%, the channel's bursts with RSSI and AFC. On MQTT: `DMR/<id>/sdr/json`, `DMR/<id>/sdr/raw`, the
   retained `Receiver` messages, `DMR/_sdr/<name>/status` every 30 s.

   | Symptom | Meaning |
   |---|---|
   | `ring overflows` or `droppedChunks` climbing | the DSP can't keep up: `"dsp_cores": 2` or fewer channels |
   | `no samples for 3 s: restarted` (`stream_restarts`) | the stream stalled and was restarted; a dongle that won't open is tried four times, then `rtl_error` says to replug |
   | `bytes_lost` / `bytes_filled` | transfers dropped with the ring full, read as mid-scale filler (up to 250 ms a gap) so the stream keeps its timing |
   | no bursts with USB at 100% | check the frequency, the ppm (the AFC follows up to 5 kHz, `RDOF_AFC_LIMIT_HZ`, about 11.4 ppm at 439 MHz), the color code, the gain (`peakDbfs`, `clipPct`) |

### CPU budget on the S3 (measured)

**RTL-SDR, the self-test clip** (SuperMini, 240 MHz, five busy channels at 288 kS/s, 4096-sample chunks), Mcycles
per second of IQ:

| Stage | First run (2026-10-09) | After the S3 work | What changed |
|---|---|---|---|
| cu8 to float | 7.1 | 7.1 | |
| forward FFT (384 points) | 50.5 | 28.4 | 6 x 64: esp-dsp's radix-4 64-point FFT, then a 6-point DFT a bin |
| per channel: bins, 64-point IFFT, NCO | 63 | 41.4 | radix-4 IFFT read through one permutation; bins gathered in runs |
| discriminator | 45.4 | 37.8 | atan2 with no divide and no branches |
| matched filter + power | 60.4 | 36.4 | int16 SIMD (PIE) 96-tap FIR, 43 cycles a sample against esp-dsp's float FIR's 175 (`fir_s3.S`) |
| squelch, sync search, slicing, flywheel | 113 | 33.9 | a cheap prefilter before the exact sync score; the history moved every 9 feeds |
| call trackers | 4.7 | 4.9 | |
| **total** | **344** | **190** | |

Then (2026-10-21), eight busy channels at 288 kS/s (`selftest_load`) went from 139% of one core to 116%, and from
103% of the wall clock on two cores to 93%: the prefilter as shared signed sums across the four sync families
(`maybe_sync_keyed`), Golay(20,8) and QR(16,7) from codeword tables, and `log10f` for burst power. What's left at
eight busy channels on one core: FM 88, forward FFT 96, the channelizer's per-channel work about 68, squelch and
prefilter 65, bursts 10. At 960 kS/s the same eight take 176% of one core and 168% of the wall clock on two.

PSRAM traffic is a large part of the cost: about 17 cycles a byte through the 32 KB data cache. 64-byte cache lines
(`sdkconfig.defaults`) took the clip from 190 to 180. A lower sample rate saves only the wideband part (about 15% at
288 kS/s): channel count is the lever.

With the idle gate, live with `sim`:

| Channels | Before | With the gate |
|---|---|---|
| 4 busy + 1 idle | all samples, 0.96 load | all samples, 0.95 |
| 3 busy + 3 idle | 86% of the samples | 92% |
| 2 busy + 4 idle | 88% of the samples | all samples, 0.96 |

**The W12** (LR2021, int8, real channels, mostly idle; 2026-10-27, 1 to 2 minutes a run; `cpu` is each core's busy
share):

| Run | `cpu` (core 0, core 1) | DSP load | Samples lost |
|---|---|---|---|
| 384 kS/s, 5 channels | 0.41, 0.83 | | none |
| 384 kS/s, 6 channels | 0.41, 0.89 | | none |
| 384 kS/s, 7 channels | 0.42, 0.97 | | none while idle, no margin for calls |
| 576 kS/s, 5 channels (before the changes below) | 0.52, 0.99 | 0.95 | about 33% |
| 384 kS/s, 5 channels, `"dsp_cores": 2` (before) | 0.72, 0.78 | 0.95 | about 6% |

Six channels at 384 kS/s is the limit with room for traffic. What got it there: the forward FFT at 512 points as
2 x 256 on radix-4 (0.31 to 0.23 of core 1), the LR2021's `read` waiting for a whole chunk, its int8 samples kept
as they came, `raw` in PSRAM, and the hot arena grown from the spare as an elastic internal claim
([memory.md](spec/fw/memory.md#5-the-sdrs-hot-arena)). On the W12 neither two cores nor a pipeline gains: both keep
every block's spectrum in PSRAM (about 4 MB/s written by one core and read by the other), where one core reuses a
spectrum buffer that stays in the cache.

### The bench API

The board answers the bench API ([bench-api.md](spec/bench-api.md)) under `/api/bench/v1`, so bench tools written
for the bench daemon run against it (`tools/bench/benchapi.py` is a client; the HIL tests use it): HTTP snapshots,
WebSocket streams (bursts, I/Q channel taps, the wideband samples with up to 1 s of pre-roll and a level trigger), a
run-time retune (`PUT /sdr`, under an exclusive lease that puts the settings back when it ends), radios wired to the
bench (`"bench": {"devices": [...]}`: a UART, USB CDC-ACM or a USB-serial bridge), and DMR transmit (`WS /tx`)
through the SDR's modem or an MMDVM bench radio, only on what `"bench": {"tx": {...}}` allows. No auth: it's for a
private test network. Code: `bench_api.c`, `bench_radio.c`, `bench_tx.c`; the spec's section 9 says what's in.

**The line protocol** on TCP `CONFIG_RDOF_BENCH_LINE_PORT` (7373; spec §14, `bench_line.c`), for
`tools/bench/rfgbench.py`:

- the receiver channels' bursts, cs8 captures and level triggers from a 2 s wideband history (kept only while a
  capture or I/Q stream used it in the last ten minutes, so a first capture has no pre-roll);
- `bench_tx` jobs through the SDR modem under `bench.tx`'s gates, in WS /tx's one job slot, `--power` capped at
  `bench.tx.rf_level`. A roamer keeps its SDR modem, so a W12 running one refuses transmit jobs;
- the `tune` op moves the receiver's window for a session (`PUT /sdr`'s retune) until it ends or releases it; a
  `trigger` or `rx` outside the window tunes for itself;
- analog `tx` and `rx` with a `mode`: FM, and AM for airband (`rfgbench.py tx am` / `rx am`: 1 kHz at 30% by
  default; the receiver gives depth, SINAD, THD, the carrier's level and offset, residual FM, and the audio and
  envelope as PCM). Receive is in software on any front end; transmit needs I/Q (a Pluto) or polar I/Q (the W12).

PSRAM: about 410 KB at rest (the engine 71 KB, its parser's strings 197 KB, send buffers 144 KB), and while in use
16 KB a connection, 115 KB a job, 256 KB a capture, a trigger its whole capture (1 MB at most). A wideband capture
needs 2 bytes a sample (768 KB/s at 384 kS/s), more than one TCP stream from the W12 carries (lwIP's 5.76 KB send
buffer): a 4 s capture got 14% of its samples, the rest reported as `gap_samples` (2026-10-29). A trigger's channel
(cs16 at 48 kS/s) is buffered whole.

### Not yet done (S3 SDR)

- The dongle's faults on a board: the gap filler is tested on the host only, the stall restart and open retries not
  at all.
- RTL-SDR Blog V4: written from webrtlsdr, not run; notch switching not done. E4000 / FC00xx tuners; direct sampling.
- The RTL-SDR driver's open measurements ([rtlsdr.md](spec/rtlsdr.md) section 3): the IF filter's codes in Hz, the
  gain steps in dB, the FIR's rate, the sign of the IF correction.
- Eight busy channels on one core (116% at 288 kS/s). Next: skip an idle channel's channelizer share, an int16
  forward FFT, fewer PSRAM bytes a sample.
- Two DMR carriers from one LR2021 (for trunking): an open bench question. Its raw I/Q transmit with the OOK setup
  for amplitude (lr2021-iq.md 3) at 125 kS/s; the PA is built for constant envelope, and a continuous downlink needs
  runs chained with no gap, which hasn't been tried (R3.5, R3.6, R3.7).

## Host tools

Built by the top-level CMake ("Host build and system tests"); SoapySDR (Debian / Ubuntu `libsoapysdr-dev`, Homebrew
`soapysdr`) and libiio / libad9361 are used when found.

### rdofsdr

A receiver: a cu8 recording, or with SoapySDR a live device, in; bursts and tracker JSON out; `--bench` times each
stage.

```sh
rdofsdr --soapy driver=rtlsdr -r 1536000 --ch C1:438.4875 ... [--gain DB] [--setting biastee=true] [--seconds S]
```

The device streams CF32 at the rate, tuned to the planned center; `--ppm` is the receiver's software correction. The
rate must be a multiple of 48000 the device runs at exactly (`rdofsdr` warns when it can't).

| Device | Soapy module | Rate (`-r`) | Status |
|---|---|---|---|
| RTL-SDR | SoapyRTLSDR | 1536000 | tested: SoapySDR 0.8, Debian aarch64 (2026-10-24) |
| PlutoSDR | SoapyPlutoSDR (libiio) | 2400000 | tested: Rev.C (Z7010-AD9361), firmware v0.38, USB, macOS arm64 (2026-10-27) |
| LimeSDR | SoapyLMS7 | 1536000 | untested |
| HackRF One | SoapyHackRF | 2400000 or more | untested; 2-20 MS/s |
| Ettus B200 / B210 | SoapyUHD | 1536000 | untested |
| bladeRF | SoapyBladeRF | 1536000 | untested |
| SDRplay RSP | SoapySDRPlay3 | 2400000 | untested; 2 MS/s or more |
| Airspy Mini | SoapyAirspy | 6000000 | untested; 3 and 10 MS/s aren't multiples of 48000 |

The Airspy R2 (2.5 and 10 MS/s only) doesn't fit. On the tested Pluto, SoapyPlutoSDR turns the FPGA's x8 decimator
on below about 2.08 MS/s, and the board then streams a constant (through libiio too): run it at 2.4 MS/s or more.

### rdofradio

A device that transmits, run as the SDR's unit, with the board's code: a SoapySDR device (`--soapy`) or an AD9361 /
AD9363 through libiio (`--iio`). `--duplex full` is the repeater, `half` the roaming transceiver; `--parrot ID` sends
back what it hears.

```sh
rdofradio --soapy driver=plutosdr -r 2400000 --allow-tx --rx 438.25 --tx 438.75 \
    --duplex full --cc 1 --gain 20 --tx-gain 65 --tx-offset 0.063
```

On a Pluto over USB (2026-10-27), RX and TX antennas on one bench: full duplex decoded the inbound throughout while
transmitting, the outbound on the 30 ms grid; half duplex sent back a whole call with 0 FEC failures.

**Timing.**

- **A thread for the transmit stream.** A Pluto's read and write both block on USB; one thread doing both fell
  behind, and libiio drops receive samples not read in time without an overflow.
- **`--stream`** (default `bufflen=32768` on a Pluto): with 65536 the loop delay changed by up to 27 ms from one start
  to the next; with 16384 the transmit stream underran silently. With 32768 it stays at 61-65 ms: `--tx-offset 0.063`.
  `--monitor` measures it by hearing the outbound back.
- **Calibration.** Which slot a handheld's burst is in comes from where it falls against our outbound, so the
  transmit path's delay must be known to a few ms. At the start (and when needed) the receiver tunes to the TX LO and
  times a 20 ms pulse sent 100 kHz below it ("calibration: the transmit path is ... ms past its place"), up to 5
  tries. A split wider than the receiver's band can't hear its outbound while repeating.
- **The transmit guard** (`rdof/sdr/txguard.h`, `--iio`): a loop stall longer than the transmit queue (3 buffers)
  would let the device run dry, and everything after would go out late for good. When the loop hasn't filled a
  buffer 6 ms before the queue runs out, the writer finishes it with zeros and drops as many late samples
  ("transmit: the loop fell behind; ... ms of zeros kept the stream's timing"): the air has a gap, the slots stay put.
  The writer runs at SCHED_FIFO 10, the receive loop and its helper at 5 (nice -10 when refused).
- **Receive losses.** A reader stall past the device's buffers loses samples without an overflow: the stream falls
  behind the wall clock ("receive: ... ms of samples lost"; zeros take their place), the guard is told
  (`rdof_txguard_slipped`), and rdofradio calibrates again at the next idle moment. Only a half second with a read
  that waited counts: working through a backlog the device still holds loses nothing.
- **SoapySDR** has no guard: a writer stall lets the device run dry and the stream moves one buffer later; idle,
  rdofradio takes it out ("transmit: ... ms of delay taken out").
- **Slots** are numbered by aligned timing ([slot-timing.md](spec/slot-timing.md)): a handheld's TSx burst falls
  under outbound TSx and is repeated on TSx.
- **Bandwidth.** A Pluto over USB 2 streams about 3.5 to 4 MS/s each way in full duplex. The modulator's phase table
  allows 16384 phases on a host (a 25 MHz split at 30 MS/s needs 6250), 1024 on the ESP32.

**The rate** without `-r` is the least multiple of 48 kS/s, from the device's usual rate (768 kS/s through libiio,
2.4 MS/s through SoapySDR), whose window holds every receive channel and which the channelizer takes (its FFT is 64
times the decimation; pffft does factors 2, 3 and 5). The most: 2.4 MS/s on a Pluto's own ARM, 3.84 MS/s from a host
through libiio, 3.2 MS/s through SoapySDR. With the FPGA channelizer: "FPGA channelizer".

#### Settings file

`--config FILE` takes the firmware's settings, version 3 ([config-v3.md](spec/fw/config-v3.md)), opened by the
firmware's parser (`rdof_plan_open`, `rdof_plan_check` with `rdof_plan_host`), calls going through the call router.
It runs the file's one unit that transmits: a repeater, or a transceiver on the SDR (half duplex, on its idle
frequency), or either on a modem with `--mmdvm PORT`. `mode` sfr or dcdm is refused. The unit's keys are the
firmware's (`rdof_plan_radio`). The device stays on the command line (`--soapy` or `--iio`, `-r`, `--gain`,
`--tx-gain`, `--tx-offset`, `--bw`, `--center`, `--allow-tx`, `--seconds`, `--web`, `-v`, `--bursts`, `--monitor`,
`--tx-lo`, `--stream`, `--fft`, `--mmdvm`, `--baud`, `--rf-level`); `--rx`, `--tx`, `--cc`, `--duplex` and
`--parrot` are refused beside it.

```sh
cat > example.json <<'JSON'
{
  "version": 3,
  "name": "example",
  "id": 9990601,
  "defaults": {"cc": 1},
  "devices": [{"name": "pluto", "kind": "sdr", "driver": "soapy"}],
  "frequencies": [{"name": "in", "freq": 438.25}, {"name": "out", "freq": 438.75}],
  "apps": [
    {"name": "example", "type": "repeater", "in": "in", "out": "out"},
    {"type": "parrot", "tg": 9990}
  ]
}
JSON
rdofradio --soapy driver=plutosdr -r 2400000 --allow-tx --config example.json --gain 20 --tx-gain 65 --tx-offset 0.063 --web 8080
```

- **HBlink4:** with an `hblink` network it's a peer, the board's peer state (`common/src/hbpeer.c`) under a POSIX
  UDP socket (`sdr/cli/rdofradio_net.c`). UDP only: `transport` sctp falls back to UDP with a warning.
- **MQTT:** an `mqtt` network with `calls` carries the calls.
- **Channels:** the receiver's channels are the SDR's as the board resolves them (`rdof_plan_sdr`): its receiver's
  frequencies, then the ones it hears for the unit (a transceiver's, a repeater's input), all in the rate's window,
  up to 16. They're all alike: with `identity: per_path` each is a peer of its own (one with the unit's peer id is the
  unit's peer, not a second login), else what it hears goes up through the unit's peer as its path's policy says; each publishes on the SDR's MQTT topics and shows on the SDR's card. The
  unit hears on the channel at its frequency (`io_channel`), and its own topics and card carry that channel's calls.
  A duplex repeater's output isn't a channel. `--monitor` adds `out` (the TX heard back) when no channel is on it.
- **Simplex on every channel** (a transceiver on the SDR): it sends on every frequency the SDR `transmits` on and
  hears on any receiver channel (`io_channel`). The TX LO moves when a call's channel is past the modulator's reach
  (0.4 of the rate, less half a channel), unkeyed. DMRD to any channel's peer goes out there through the same
  transceiver, one channel at a time. Our own transmissions heard back are dropped within 60 ms of the key-up
  (`echoes`). `"tx": false` on a frequency keeps every device off it.
- **The clock:** with `ntp` servers it's netclock (`components/netclock`, `netclock_posix`) and the page's clock card
  is the board's; `ntp.clock` `"os"` leaves the time to the OS. `--clock-state FILE` keeps netclock's rate across
  restarts; `--set-os-clock` steps the OS clock when it's 100 ms off (root).
- **The crystal:** `--ppm` corrects the receiver's channels and the modulator's carrier; `--ppm-tol` is how far to
  trust it in the bench's reports. rdofradio doesn't learn it.
- **The page:** with `--web`, the Settings tab edits the file. A save is checked by the same parser, written whole
  (a temporary file renamed over it), and rdofradio restarts with the same command line. Settings that can't run keep
  the page up: the card shows why and a save starts it again. Without `--config` the tab shows the flags as a read-only
  version 3 document. `/api/status` `version` is `RDOF_VERSION`, else git describe at configure time.

#### A radio transmits, the SDR receives (split duplex)

`rdofradio DEVICE --allow-tx --split-tx PORT --rx MHZ --tx MHZ`: a split duplex repeater
([repeater.md](spec/repeater.md)). A RadioFoundry radio with the split duplex extension ("SPLIT1" in its
description) on PORT is the TX half; the SDR, opened receive only, is the RX half (any receiver: an RTL-SDR, a Pluto
with its transmitter off). `rdof_split_t` joins them, as `split_port.c` does on a board. PORT is the radio's USB
serial device or a programming cable's UART (`--baud`, 115200). `--split-power-cap` caps the radio's RF level (MMDVM
scale 1-255). The SDR's transmitter flags (`--tx-gain`, `--tx-lo`, `--tx-offset`, `--monitor`) are refused.

- **The SDR modem's clock** is the receive stream's sample count against the wall clock, at the freshest read; its
  least lag over half a second follows the device's crystal. A lag 2 ms over that floor for a second is dropped
  samples: zeros take their place.
- **The port** is read by a thread that stamps each read, so a TIME round trip doesn't include the main loop's wait.
- **Waits:** while the repeater waits for the radio's reply, the receiver keeps reading.

Run on the Pluto (2026-10-28) against a simulated radio on a pty: the split came up, the repeater ran 60.0 s in
60.0 s with no samples lost, the TX half's round trip settled at 1.1 ms. Not yet run with a real radio.

**Several split repeaters on one SDR** (`--config`): every repeater in the file whose RX half is the SDR and whose TX
half is a modem runs, up to 8 (`RDOF_ROLES_MAX`), each with its own radio. The first is the radio at `--split-tx`,
or its device's `port.path`; the others need a `port.path` (and `baud`). Each split has its own receive-only SDR
modem (`rdof_sdrsplits_t`, `sdrsplit.c`) on its input's channel, and each repeater registers with the router. Every
split's output frequency is left out of the channels; its input is a channel of the receiver and goes up
through that channel's sink. Bring-up runs every split at once, 15 s for all; one that fails stops the run.
Host-tested (`sdrmodem_tests`: 8 simulated radios with different crystals on one receiver at 1.632 MS/s, each burst
up on its own repeater and slot; `rdof_radiocfg_tests`: the file's units and ports). Not yet run with real radios.
The web page and MQTT status show the first repeater only.

#### An MMDVM modem on a serial port

`rdofradio --allow-tx --mmdvm PORT --rx MHZ [--tx MHZ]`: the repeater (`--duplex full`, the default) or the roaming
transceiver (`--duplex half`) on an MMDVM modem: an MMDVM_HS hat on a UART (`/dev/ttyS0`), a hotspot or an MMDVM
board on USB. A duplex modem runs the repeater, any modem the transceiver; the radio's type comes from its
description (`rdof_radio_detect`). Its own call tracker feeds the page, MQTT, HBlink4 and listen before talk. The port
is read by a thread.

- `--rf-level N` caps every SET_FREQ's RF level (0-255; 255 = no cap), whatever sent it (`rdof_mmdvm_cap_rf_level`).
- `--rx-offset-hz`, `--tx-offset-hz`, `--rx-level`, `--tx-level`: the modem's settings; with `--config`, the modem
  device's `mmdvm` keys, and each frequency's `power` is its RF level.
- **An SDR beside it** (DEVICE, optional), opened receive only: it hears the modem's RX frequency as a channel of its
  own (`modem-rx`, when no channel is on it) and the settings' channels, with the spectrum, and carries the bench's line protocol. Bench DMR
  jobs transmit on the modem in simplex. `--split-tx` and the SDR's transmitter flags are refused.

#### The bench's line protocol

`--bench [ADDR:]PORT` serves [bench-api.md](spec/bench-api.md) §14 for `tools/bench/rfgbench.py`: the channels'
bursts, wideband cs8 captures from a 2.5 s history, level-triggered channel captures, `bench_tx` jobs (`voice`,
`csbk`, `data`, `listen`) and `bench_repeater` jobs. A transmit job needs `--bench-tx MHZ[,MHZ...]` (up to 32
frequencies) or bands, and the half duplex transceiver: it takes the SDR modem for its length, at `--tx-gain`
(bench_tx's `--power` is noted, not applied). The `tune` op is `unsupported`, and a `trigger` or `rx` outside the
window is refused. With the FPGA channelizer there's no wideband I/Q: capture, trigger, track and rx take the
receiver's channels (2.5 s of each kept). When the bench line and the page listen past loopback, rdofradio lists them
over DNS-SD (`_rfgbench._tcp` with TXT `kind` and `ver`, `_http._tcp`) through the host's responder, by running
`avahi-publish-service` (a PlutoSDR's firmware has avahi) or macOS's `dns-sd` for as long as it runs; with neither,
nothing is listed (`rdofradio_dnssd.c`).

### Host web page

`hostweb/` serves the firmware's page (embedded from `esp_idf/main/web/index.html` and the components' scripts) from
a host tool. Build with `-DRDOF_HOST_WEB=ON`, run with `--web [ADDR:]PORT` (127.0.0.1 unless ADDR is given). The tool
publishes a status snapshot, log lines and calls (`rdof/hostweb.h`), and may give settings hooks. The API is the
firmware's code (`rdof/webapi.h`): `/api/status`, `/api/lastheard`, `/api/log`, with the hooks `/api/config`,
`/api/resolved`, `/api/restart` and `/api/ota`; `501 not on this platform` for the rest. The status's `caps` say what
the server has, and the page hides the rest. Publishing never waits on the server.

### PlutoSDR

`rdofradio --iio URI` drives an AD9361 or AD9363 through libiio 0.x (`sdr/cli/rdofradio_iio.c`), with no SoapySDR. A
PlutoSDR's firmware has libiio and libad9361, so a build for its ARM runs on a stock Pluto (or a Zynq 7010 / 7020
clone on ADI's firmware) with nothing installed. `ip:192.168.2.1` or `usb:` reach a Pluto from a host.

**Build:** `tools/pluto/build.sh` (an x86_64 host with Docker) builds in Debian 9, whose glibc is older than the
firmware's, against ADI's v0.38 sysroot. It writes `build/pluto/rdof-pluto.tar.gz` (`rdofradio`, `rdofsdr`,
`rdofchz`; they need only libiio, libad9361 and libc) and `build/pluto/rdof-pluto-bundle-VERSION.tar.gz` (the
installer, below).

**Run** from `/tmp` (RAM):

```sh
gunzip -c rdof-pluto.tar.gz | tar -xf -
./rdofradio --iio local: --allow-tx --rx 438.25 --tx 438.75 --duplex full --cc 1 --gain 20 --tx-gain 65 \
    --parrot 9990 --web 0.0.0.0:8080
```

- `--iio` defaults to 768 kS/s: libad9361's `ad9361_set_bb_rate` loads the chip's own FIR under 2.083 MS/s, and the
  FPGA's decimator stays off.
- The RX and TX LOs are separate, so the split is anything both tune to (70 MHz to 6 GHz on an AD9361, 325 MHz to
  3.8 GHz on an AD9363). The receiver hears its own outbound only during the calibration.
- TX gain is 0..89 dB (89..0 dB attenuation), as SoapyPlutoSDR's. The LOs are powered up at the start and down at the
  end: SoapyPlutoSDR leaves both down when it closes, and the transmitter then sends nothing.
- The receiver shares each chunk with a helper thread on the other core: the forward FFTs split in half, the
  channels a third and two thirds; the bursts go to the sinks on the loop's thread. The IIO buffer is copied out
  before it's read (16-bit loads straight from the DMA buffer took 20% of a core, against 4%).

Measured on a Rev.C (Z7010-AD9361), firmware v0.38, two Cortex-A9 cores at 667 MHz, the ARM channelizing
(2026-10-28 and 29):

| Run | Real time | CPU |
|---|---|---|
| SoapySDR at 2.4 MS/s, the built-in FFT | 34%: samples lost every second | the main thread 100% of a core |
| `rdofsdr --bench`, 2.4 MS/s, built-in FFT / pffft | | 1989 / 762 ms per s of IQ |
| `--iio local:` 768 kS/s, pffft, full duplex | 100% (60.0 s in 60.0 s) | 32% of the two cores, 20 MB |
| `--iio local:` 864 kS/s, 9 channels, half duplex, two handhelds keyed at once, one a slot | 100%, no samples lost | main thread 70%, helper 34%, writer 20% |
| `--iio local:` 1.296 MS/s, 10 channels (438.0625 to 438.975 MHz) | 100% | the receive loop 69% busy |

#### Installing on the Pluto

The bundle ([tools/pluto/bundle/README.md](../tools/pluto/bundle/README.md)): `./install.sh --config FILE` (or
`--example`) from a computer, over ssh, puts `rdofradio`, the settings and a supervisor loop (`run.sh`) in the
Pluto's persistent flash (`/mnt/jffs2`). The firmware's `/mnt/jffs2/autorun.sh` starts the loop at boot, and the loop
starts `rdofradio` again 5 s after it exits.

- The settings stay in `/mnt/jffs2/rdof/config.json` (the page edits them), the device flags in `flags`
  (`flags.example`: `--gain 20 --tx-gain 0 --web 0.0.0.0:8080 --clock-state ... --set-os-clock --bench 7373`).
  netclock's state is kept in `/mnt/jffs2/rdof/netclock.state` and sets the Pluto's clock (it starts in 1970).
- **Updates:** the page's Firmware update (`POST /api/ota`; `--ota FILE`: the bundle's `rdofradio.gz`, or an ELF for
  the Pluto's ARM) leaves the new program in RAM and stops rdofradio; `run.sh` checks it runs, puts it in the flash
  in place of the old one (there's no room for both), and puts the old one back if the new one ends in error within
  30 s.
- The Pluto makes a new ssh host key at every boot, so the script doesn't check it.
- At `--tx-gain 0` (the default) the start's calibration pulse isn't heard, so the outbound slot timing isn't
  measured.
- Tested on a Pluto+ style clone (Z7010, 1 GB, Ethernet, SD boot, v0.38): the page answers about 25 s after power-on.

#### FPGA channelizer

`--fpga-chz [ADDR]` (with `--iio`) decodes from the FPGA channelizer block (`chzhw.h`) in place of the ARM's
channelizer: every channel at once, 15 on the 7010 clone where the ARM manages 10. It probes the block (at
`0x7C480000` unless ADDR is given) and plans the rate as usual, up to 48 kS/s times the most the block decimates
(24.576 MS/s with its CIC: R D at most 512).

- **The plan:** no CIC when the FIR alone decimates enough; else the CIC with the smallest D from 8 that divides the
  step. The FIR's taps (16 D + 1) are the block's time: R 9, D 46 overflowed its input at 19.872 MS/s.
- **The frames** reach the receiver through `rdof_sdr_rx_channels_in`: int16 with 3 fraction bits over the 12-bit
  ADC (`RDOF_CHZFX_OUT_SCALE`, 1/16384); a tone reads within 0.001 dB of the float path. Lost frames become zeros (the
  status's lost count); STATUS overflow bits count as overflows. The block is stopped on exit.
- **The idle gate's noise** comes from the channels: the 25th percentile across all channels, the least of the last
  16 chunks.
- **The AFC** writes the block's NCO steps once a turn.
- **No wideband I/Q:** the page's spectrum is the channels' own: a 64-point FFT of each channel's 48 kS/s, laid into
  the row at its frequency over 6.25 kHz either side (`rdof_spectrum_place`, rescaled to the row's bin width so levels
  read as on the wideband spectrum); the span between the channels is empty, the floor. The bench keeps every channel's
  outputs for 2.5 s instead (about 130 bytes an output instant for 15 channels, 16 MB; the wideband ring at 19.872 MS/s
  is 99 MB), with each one's NCO offset: capture, trigger, track and rx on a receiver channel take its I/Q from there
  (bench-api.md 14). The block's int16 LSB is near the noise, so a quiet channel's floor reads about -67 dB. The duplex
  calibration points channel 0 at the pulse while it runs.
- **With `--split-tx` or `--mmdvm`** the SDR receives only and the block runs as usual. Not yet tested with a radio
  on the Pluto's USB.
- `rdofchz` brings the block up by hand: `--probe` reads ID and CAPS through `/dev/mem`; with `--fs`, `--center`,
  `--ch MHZ ...` (and `--cic R`) it starts the block and prints the frames, gaps and each channel's level once a
  second.
- Host test: `rdof_chzrx_tests` runs `e2e_288k.cu8` through the block's model into this entry and matches the float
  path burst for burst.

`--fpga-duc [ADDR]` (on a bitstream with the TX interpolator, at `0x7C4A0000` unless ADDR is given) runs the transmit
stream at 192 kS/s, or 96 kS/s when the rate isn't a multiple of 192 kS/s, and the FPGA's CIC (order 3, R 17 to
4095) brings it to the device's rate, so the ARM makes a narrow transmit stream while the receiver runs wide.

- The rate is planned to a multiple of 96 kS/s the interpolator makes (9 channels over 650 kHz: 1.632 MS/s, R 17);
  with none for the channels it turns off.
- 48 kS/s is refused: its first images, 56 dB down, land on the next channels of a 50 kHz grid. At 96 kS/s they are
  76 dB down, at 192 kS/s 94 dB.
- The TX LO sits 62.5 kHz above the carrier at 192 kS/s and 18.75 kHz at 96 kS/s (`--tx-lo` unless given). Only TX1.
- The TX buffers are about 5 ms (512 samples at 96 kS/s), so the queue stays inside the stream's 50 ms lead.
- With `--split-tx` or `--mmdvm` the SDR doesn't transmit, and it's left unused.
- Without the block nothing answers at ADDR, and a read there is a bus error: both options need the bitstream.

Measured on the 7010 clone (2026-11-03 and 04):

| Run | Result |
|---|---|
| `rdofchz`, no CIC: 10 channels from 2.4 MS/s at 438.525 MHz | no frame lost over 6 s; a 20 dB gain step moved every channel 19-20 dB; flat to 3 kHz, -3 dB near 5.5 kHz, -59 dB beyond 14 kHz |
| `rdofchz` with the CIC: 15 channels across 438.0125 to 438.6 MHz from 23.04 MS/s (R 60, D 8, 129 taps) | no frame lost and no input overflow over 10 s; the ARM 95% idle |
| `--fpga-chz --fpga-duc`, 15 channels from 19.872 MS/s, TX at 96 kS/s (R 207) | 30.0 s in 30.0 s, no channel frame lost; without `--fpga-duc`, over 1,000 frames lost a second |
| loopback with `--bench`, the 15 frequencies in `--bench-tx` (TX gain 79) | 8 CSBKs on each of the 15 in turn, all 15 receiving: 120 of 120 decoded (CC 2, no sync errors, -35 dBFS, 11-12 Hz off); a 10-superframe voice call, 71 of 71 bursts, nothing on the channels 50 kHz either side |

#### The bitstream

The bitstream for both blocks is kept in `tools/pluto/fpga/` as `rfg-pluto-<board>.bit.bin` (MIT and ADI BSD; its
README and `MANIFEST` say where it was built from). There is one for `fishball7010` (a 7010 Pluto+ style clone with
32-bit DDR, 1 GB and Ethernet) so far; `rdof/sdr/plutofpga.h` also knows `plutoplus` and `adalm-pluto`.

With `--iio local:`, `--fpga-chz` or `--fpga-duc` loads it before anything else (`sdr/cli/rdofradio_fpga.h`):

1. **The board** from the Zynq's IDCODE, its DDR width, MIO 16's function, the memory size and the AD9361's LVDS
   mode. `rdofradio --fpga-board` prints it and the file's name.
2. **The file:** `--fpga-bit FILE`, else the first found in `/mnt/jffs2/rdof/`, `rdof/` on any SD card or USB stick
   partition (mounted read-only for the look if it isn't mounted), `/lib/firmware/`.
3. **The check:** its IDCODE against the chip's.
4. **The load:** the PL's drivers unbound, the FPGA manager loads it, the drivers bound again, the AD9361's ADC and
   DDS cores also when a probe before had failed. A driver that doesn't probe again (the ADC's RX tuning, in
   `dmesg`) is an error and nothing is noted. `/tmp/rdof-fpga` notes what's in, so a restart loads nothing; a reboot
   brings the stock design back.

With none for the board both options are off and the ARM channelizes: the channels that don't fit its
window are left out, in the settings' order, the unit's kept (15 configured on the clone: 10 kept, at 1.296 MS/s).

`install.sh` writes the board's bitstream to `rdof/` on the SD card's boot partition (the one with `BOOT.bin`, which
it never touches), only when it differs; the flash has no room for it. It then adds `--fpga-chz --fpga-duc` to
`flags`, so every boot loads it.

## License

MIT ([LICENSE](../LICENSE)). Third-party license texts are in [LICENSES/](../LICENSES/), and where each piece of
copied or ported code came from is in [PROVENANCE.md](../PROVENANCE.md).
