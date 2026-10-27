# Provenance

Where this project's code came from: its origin, what was taken from outside sources, and the dependencies it
links. Keep a row per use, and add one before copying or porting anything new.

## Origin

RadioFoundry Gateway began as the `fw/` folder of RadioDesk (a private repository by the same authors), a port to
C, then partly to Rust, of RadioDesk's Python RF gateway (`rfgw`, the `radiodesk_rfgw` package) and the DMR helpers
it used (`radiodesk.dmr`). It was copied out on 2026-10-23 with no history, from RadioDesk commit 1242d63.

- The C and Rust were written in that repository by the same authors. The Python they port was too, and it isn't
  part of this project.
- The golden vectors in `host-tests/vectors.h`, `host-tests/scenarios.h` and `host-tests/fixtures/` were generated
  by that Python (RadioDesk `fw/tools/gen_*.py`) and are committed here as data. The tests check the C and the
  Rust against them; nothing here regenerates the Python's. The strings and addresses in them were changed to this
  project's names when it was copied (the HBlink4 config packets' expected bytes recomputed to match), and the
  OLED status screen's expected hash was updated for its new address. On 2026-10-24 the callsigns that may be
  real (a CW ID, an MMDVM_HS version string, an HBlink4 config packet) became N0CALL, and a lab address and a
  position became documentation values, each expected value recomputed by the C. The AGC cases now run on our
  measured R820T steps, as regression vectors from the C (`host-tests/fixtures/README.md`). The data call
  (`vectors.h` `sms_data_call`, `sdr_vectors.h` `sms_ms_slot2` and the e2e case's SDR08) is synthetic: built with
  our encoders and modulator by `host-tests/fixtures/gen_data_call.c`, between test IDs 9990001 and 9990002, which
  also rebuilds `e2e_288k.cu8` around it; their expected values are regression vectors from the C. Also on
  2026-10-24 the test radio and peer IDs moved to 999xxxx, a range RadioID has never assigned: the Python was rerun
  with the new IDs and only the rows that changed taken, the changes above kept (`host-tests/fixtures/README.md`,
  "Test IDs").
- The checks below were carried over from RadioDesk's `PROVENANCE.md` as they stood at that commit.

## Open items

- The file-by-file GPL review (2026-10-24) found nothing derived from GPL or AGPL code in `common/`, `rust/`,
  `sdr/` or `components/`; the `clean` rows below marked "reviewed" or "spot-checked" 2026-10-24 come from it.
  Still open:
  - `esp_idf/main/` (but `fir_s3.S`, `split_port.c` and `ipgw_task.c`, which have rows), `sdr/cli/`, `systest/`,
    `tools/` (but the rows) and the test trees had only the automated checks (a token comparison against MMDVMHost,
    MMDVM, MMDVM_HS, DMRGateway, dsd, dsd-fme, lr2021-sdr, librtlsdr and go-dmr, and the marker scan), not a read
    through. Nothing turned up.
  - `common/src/tracker.c` publishes MMDVMHost's MQTT JSON format, including its eight `csbk_desc` strings, as an
    interface for the programs that read it. Judged interoperability; reword the descriptions if no doubt at all is
    wanted (consumers would see the change).

## Third-party code

What was taken from outside sources, other than libraries linked as dependencies.

| Path | Upstream (repo @ commit) | License | What was taken |
|---|---|---|---|
| `common/src/ambe.c`, `common/include/rdof/ambe.h`, `rust/src/ambe.rs` | szechyjs/dsd @ 59423fa46be8b41ef0bd2f3d2b45590600be29f0 (`dmr_const.h`); szechyjs/mbelib @ 9a04ed5c78176a9965f3d43f7aa1b1f5330e771f (`ambe3600x2450.c`) | ISC (`LICENSES/ISC-DSD-mbelib.txt`) | The AMBE+2 frame interleave tables (rW, rX, rY, rZ) and the frame decode (Golay(24,12) on C0, C1's scrambling and Golay(23,12)), ported (by way of RadioDesk's Python); `ambe.rs` is a port of `ambe.c` |
| `common/src/oled.c` (`GLYPHS`) | adafruit/Adafruit-GFX-Library (`glcdfont.c`; license checked at 85ce269b270eb1241e36a22cceec2c5e0905f48c) | BSD-2-Clause (`LICENSES/BSD-2-Clause-Adafruit-GFX.txt`) | The 5x7 bitmap font's printable ASCII glyphs, copied |
| `sdr/src/fft.c` | mborgerding/kissfft | BSD-3-Clause (`LICENSES/BSD-3-Clause-KISSFFT.txt`) | The structure of the mixed-radix decimation-in-time FFT (the factor recursion, the radix 2, 3 and 4 butterflies); written here, not copied |
| `components/lr2021` (raw I/Q receive and transmit: `lr2021_cmd_raw_*`, the raw session in `lr2021_iq.c`) | SAMS0N1TE/lr2021-sdr @ 929d97a1b14b9563f418d6f4fd84a787a56e45aa | GPL-3.0 | Facts (hardware interface) learned from it; no code taken; spec `docs/spec/lr2021-iq.md` |
| `components/lr2021`: `lr2021_radio.*`, `lr2021_lora.*`, `lr2021_fsk.*`, `lr2021_proto.*` (the documented commands) | Lora-net/usp @ 540bba008477a8969e554b8d10dbf90ff6160c8e (`smtc_rac_lib/radio_drivers/lr20xx_driver`) | Clear BSD (`LICENSES/BSD-3-Clause-Clear-Semtech-USP.txt`) | Command encodings, value tables and time-on-air formulas, ported into this project's builders; the notice is at the top of each file. Checked against the datasheet (DS.LR20xx Rev. 2.2); where they differ, the files say which was followed |
| `components/lr2021/src/lr2021_patch_image.c` (Semtech's LR2021 patch RAM image, 560 words) | Lora-net/usp @ 540bba008477a8969e554b8d10dbf90ff6160c8e (`smtc_rac_lib/radio_drivers/lr20xx_driver/inc/lr20xx_pram_lr2021.h`) | Clear BSD (`LICENSES/BSD-3-Clause-Clear-Semtech-USP.txt`) | The binary image, copied unchanged as data, with the notice at the top of the file. The loader (`lr2021_patch_load.c`) is written from DS 22.3 |
| `components/lr2021/src/lr2021_lora_extra.c`, `include/lr2021_lora_extra.h` | TheClams/lr2021 @ 402a12ba350c7a5419acf595ca6ea529403d2dbc | MIT (`LICENSES/MIT-TheClams-lr2021.txt`) | Opcodes 0x022D and 0x022E (extended LoRa sync words) and the ranging base-delay table; the notice is in both files. Other files used it to cross-check only |
| `esp_idf/main/fir_s3.S` (the inner loop) | espressif/esp-dsp v1.8.2 (`modules/fir/fixed/dsps_fird_s16_aes3.S`) | Apache-2.0 (`LICENSES/Apache-2.0.txt`) | The four-instruction load / multiply-add loop, its q0 / q1 pre-loads and the ACCX rounding / shift framing, adapted; the notice and the changes are in the file's header |
| `rtlsdr/src/rtl2832u.c`, `rtlsdr/src/r820.c`, `rtlsdr/include/rtlsdr/r820.h` (through `docs/spec/rtlsdr.md`) | jtarrio/webrtlsdr @ 9b3f5872750fa119bddd07a2bd4984b4b6716a4c (`src/rtlsdr/rtlcom.ts`, `rtl2832u.ts`, `r8xx.ts`, `r820t.ts`, `r828d.ts`) | Apache-2.0 (`LICENSES/Apache-2.0.txt`) | Facts, cited in the spec: the vendor request framing and I2C chunking, the baseband and SDR-mode writes, the tuner's init values and sequence, the filter calibration, the RF filter tables (standard and Blog V4), the PLL's rules, the gain scheme and AGC values, standby values, the R828D's input switching, and its default FIR bytes as evidence of the FIR's register layout. The C was written from the spec, not ported; its copyright notice (2024 Jacobo Tarrio Barreiro, 2013 Google Inc.) is in each file's header |
| `components/gnss/src/gnss_nmea.c` (`gnss_days_from_civil`); `common/src/tracker.c`, `rust/src/tracker.rs` (`civil_from_days`) | Howard Hinnant, "chrono-Compatible Low-Level Date Algorithms" (`days_from_civil`, `civil_from_days`) | public domain (the algorithm as published) | The date arithmetic, written from the published algorithms; the NMEA parser itself is from the NMEA 0183 sentence formats. tracker's copy has no comment saying so yet |

## Test dependencies

Used by the system tests only (`systest/`); nothing of them is in the gateway's builds or copied into this repository.

| What | License | Used for |
|---|---|---|
| [RadioFoundry](https://github.com/RadioFoundry/RadioFoundry) (a checkout given as `RADIOFOUNDRY_DIR`, at `systest/radiofoundry.ref`, 4f92e513) | MIT | `radiosim` compiles its `core/hotspot`, the DMR code it uses and `tests/host/sim/dmr_sim_modem.c` (listed in `systest/CMakeLists.txt`) around our own `systest/radiosim/radiosim.c` |

## Host dependencies

Optional, found by the host build (CMake) when installed; linked, not copied.

| What | License | Used for |
|---|---|---|
| SoapySDR (C API, 0.8) | BSL-1.0 | `rdofsdr --soapy`: live input from any SoapySDR device |

## Firmware dependencies

ESP-IDF components pulled through the component manager (`esp_idf/main/idf_component.yml`, versions locked in
`esp_idf/dependencies.lock`). Linked, not copied into the repository.

| Component | Version | License | Used for |
|---|---|---|---|
| `espressif/esp-dsp` | 1.8.2 | Apache-2.0 | DSP kernels on the S3 |
| `espressif/usb_host_cdc_acm` | 2.4.1 | Apache-2.0 | the OpenGD77 / MMDVM modem on USB; bench radios on CDC-ACM |
| `espressif/usb_host_ftdi_vcp` | 2.1.1 | Apache-2.0 | bench radios on an FTDI USB-serial bridge |
| `espressif/usb_host_cp210x_vcp` | 2.2.0 | Apache-2.0 | bench radios on a CP210x |
| `espressif/usb_host_ch34x_vcp` | 2.2.1 | Apache-2.0 | bench radios on a CH340 / CH341 |

ESP-IDF itself is Apache-2.0. Rust's `core` library is compiled into `rust/`'s static library (MIT OR Apache-2.0;
Espressif's fork for Xtensa). The crate has no other dependencies; `compiler_builtins` is dropped from the archive
(`rust/README.md`).

## Files by origin

Origin values:

- `radiofoundry`: from [RadioFoundry](https://github.com/RadioFoundry/RadioFoundry) (formerly OpenDigitalRadio, ODR; MIT, the same authors), copied (C, with its
  SPDX header) or ported. The commit is given. RadioFoundry wrote these from ETSI TS 102 361; see its
  `PROVENANCE.md` and `docs/CLEANROOM.md`.
- `clean`: written from a standard, a datasheet, an interface description or our own spec, without GPL or AGPL
  code. The rows marked "reviewed 2026-10-24" were checked against the GPL projects nearest them and share only
  facts and interfaces with them.

| Path | Origin | Source / license | Notes |
|---|---|---|---|
| common/src/radiofoundry/dmrFec.c, dmrFec.h | radiofoundry | core/dmr/fec/dmrFec.{c,h} at 1ce780d1, MIT | include paths flattened, otherwise unchanged; the copyright line now reads RadioFoundry, as upstream's does |
| common/src/radiofoundry/dmrCach.c, dmrCach.h | radiofoundry | core/dmr/fec/dmrCach.{c,h} at 1ce780d1, MIT | include paths flattened; upstream's PROVENANCE.md had no row for these at that commit (the header cites ETSI TS 102 361-1 6.3, 7.1.4, 9.1.4, B.2.3, B.3.3, B.3.5, B.3.7, B.4.1) |
| common/src/radiofoundry/dmr_lc.c, dmr_lc.h | radiofoundry | core/dmr/call/dmr_lc.{c,h} at 1ce780d1, MIT | include paths flattened |
| common/src/radiofoundry/dmrTrellis.c, dmrTrellis.h | radiofoundry | core/hotspot/dmrDataFrame.c at 1ce780d1, MIT | an extract: the trellis and rate 1 functions, made public and renamed |
| common/src/radiofoundry/dmrDataCrc.c, dmrDataCrc.h | radiofoundry | core/dmr/dmrData.{c,h} at 1ce780d1, MIT | an extract: dmrDataCRC32, dmrDataCRC9 and the CRC-9 masks, unchanged |
| LICENSES/MIT.txt | radiofoundry | RadioFoundry LICENSE | the license of the files above |
| common/src/fec.c, common/include/rdof/fec.h | clean | ETSI TS 102 361-1 9.1, B.3.4, B.3.8, B.3.10 | the rdof_* API as thin wrappers over src/radiofoundry; Hamming(16,11,4) as (15,11,3) plus overall parity |
| common/src/layer2.c, common/include/rdof/layer2.h, rust/src/layer2.rs | clean | ETSI TS 102 361-1 9.1.1 (sync), B.3.1, B.3.2, B.3.5, B.4.1 | Golay(20,8), QR(16,7,6), TACT and CACH through src/radiofoundry |
| common/src/duplex.c, common/include/rdof/duplex.h | clean | ETSI TS 102 361-2 7.1.3.2 table 7.10 (Act_Updt), TS 102 361-1 D.2 (idle message, as RadioFoundry bsOutbound.c BS_IDLE_INFO); docs/spec/repeater.md R5.1-R5.11 and R9 | the short LC through src/radiofoundry/dmrCach.c. The voice and data regeneration (BPTC re-encode, AMBE FEC regeneration through ambe.c, the embedded LC one superframe late) was reviewed 2026-10-24: no MMDVMHost or MMDVM structure, names, tables or comments; two behaviors match MMDVMHost's as ideas only, each justified in repeater.md R5.7 / R5.8 from ETSI |
| rtlsdr/esp/rtl_usb.c, rtlsdr/include/rtlsdr/rtl_usb.h, rtlsdr/src/gaps.c, rtlsdr/include/rtlsdr/gaps.h | clean | ESP-IDF's USB host API; the RTL2832U datasheet (table 27: vendor requests 0x40 / 0xC0, bRequest 0); the device's own descriptors (VID / PID, bulk endpoint 0x81, product string) | the USB host tasks, the transfer pool, the ring, the gap filler and the retries are this project's; reviewed 2026-10-24, nothing follows librtlsdr's libusb code |
| docs/spec/repeater.md | clean | ETSI TS 102 361-1 V2.6.1, TS 102 361-2 V2.4.1; RadioFoundry (MIT, 1ce780d) docs/spec/mmdvm-protocol.md, split-duplex.md, sfr.md, core/dmr/bsOutbound.c; the authors' requirements | the base station behavior spec; written without opening MMDVMHost, MMDVM, MMDVM_HS, DMRGateway, hblink3 or ok-dmrlib. The code doesn't follow it everywhere yet |
| common/src/split.c (`ClockSync`, `epoch_for_rx`, `wrap_diff`) | radiofoundry | tools/split_duplex_host.py at 161a2a88 (last changed c24b9bd5), MIT | the clock filter and the epoch translation ported (to Python in RadioDesk, then C); the rest (the virtual modem as a pure core, routing, joins, CW ID, the AT mask) written from docs/spec/split-duplex.md section 8 |
| common/src/split.c (`rdof_epoch_relay_*`) | radiofoundry | tools/split_duplex_host.py and docs/spec/split-duplex.md (#109; introduced in ff3e44ac), MIT | the trust rules for relaying an epoch (and the ALIGN target): a bound limit, agreement by whole frames, three disagreeing in a row replace; ported to Python, then C |
| common/include/rdof/splitops.h, common/src/splitops.c | radiofoundry (interface facts) | docs/spec/split-duplex.md 3.1, 3.2, 3.6, 3.8, 3.9 on feature/split_utc_slots (e83857b2, a21110b3), MIT | the byte layouts of INFO 10-15, SET_ROLE 6, ALIGN, TX_SCHED and STATUS 32-47, encoded and decoded here; no RadioFoundry code taken |
| sdr/src/sdrmodem.c, sdr/include/rdof/sdr/sdrmodem.h, esp_idf/main/split_port.c | clean | RadioFoundry (MIT) docs/spec/mmdvm-protocol.md (1ce780d1) and docs/spec/split-duplex.md (both roles, the INFO / STATUS layouts, the lease, SET_AT's modes) | written from the specs; no RadioFoundry firmware code (hotspotSplit.c) read for it |
| common/src/benchguard.c, common/include/rdof/benchguard.h | radiofoundry | odr-testbench (the bench daemon; a private repository by the same authors) src/odr_sdrbench/benchd/stations.py at 4248694, MIT | ported to C as a state machine (no blocking); the MMDVM side (SET_FREQ, TX data) is new. Its SPDX header kept |
| tools/bench/benchapi.py, docs/spec/bench-api.md | clean | the board's own endpoints; odr-benchd's socket protocol and record field names (interface facts, from its README and client) | written in RadioDesk (tools/bench). Copied here with its burst decoding rewritten on its own: the raw message layout from common/include/rdof/rawburst.h and the sync patterns from ETSI TS 102 361-1 table 9.2. No odr_sdrbench code taken |
| tools/rtl_probe/rtl_probe.c | clean | Linux usbfs (`linux/usbdevice_fs.h`); docs/spec/rtlsdr.md; the radix-2 FFT and Hann window are textbook | the bench harness for the RTL-SDR measurements (spec section 3, R1.n) |
| rtlsdr/src/rtl2832u.c, rtlsdr/src/r820.c, rtlsdr/include/rtlsdr/r820.h, host-tests/test_rtl.c, tools/rtl_fir_design.py, docs/spec/rtlsdr.md | clean | Realtek RTL2832U datasheet Rev. 1.4; Rafael Micro R820T2 Register Description (2016); jtarrio/webrtlsdr @ 9b3f587 (Apache-2.0, row above); our bench findings (docs/reference.md); general DSP (the FIR is a Parks-McClellan design) | the clean-room rewrite of the RTL-SDR driver, 2026-10-24: the spec written first, every register value cited, then the code and tests from it. Written without opening librtlsdr (osmocom rtl-sdr), the Linux kernel's r820t / rtl2832 drivers, any other R820T / RTL2832U driver but webrtlsdr, or the files it replaces (`rtl2832.c`, `r82xx.c`, `r82xx.h`, `test_rtlsdr.c`) and their notes. What no source settles is marked "to measure" in the spec's section 3 |
| rtlsdr/include/rtlsdr/rtl.h | clean (rewritten) | as above | the public API kept for its callers (`fe_rtlsdr.c`, `selftest.c`, `rtl_usb.c`); its internals (the tuner's state) and comments rewritten, the error codes and the FIR packing added |
| common/src/ipgw.c, common/src/ipgwcfg.c, common/src/ipgwtx.c, common/include/rdof/ipgw.h, common/include/rdof/ipgwtx.h, esp_idf/main/ipgw_task.c, host-tests/test_ipgw*.c, docs/spec/fw/ipgw.md | clean | ETSI TS 102 361-1 9.2 and annex B (data header, blocks, CRC-9, CRC-32, responses), TS 102 361-3 (SAP 4; SAP 3 and table 7.14's port IDs); RFC 791, RFC 768, RFC 792, RFC 1071, RFC 1624; RadioFoundry's radio firmware docs (dmr-data.md 11.1, usb-network.md 3.1: addresses, the gateway id, chains, TTL), as interface facts | written from the standards and those interface descriptions; the CRCs, BPTC and trellis through src/radiofoundry. No other gateway's or router's code read for it |
| common/src/mmdvm.c, common/include/rdof/mmdvm.h, rust/src/mmdvm.rs | clean | the MMDVM serial protocol as interface facts (RadioFoundry docs/spec/mmdvm-protocol.md, MIT, 1ce780d1): framing, command numbers, the SET_CONFIG layouts for protocols 1 and 2, SET_FREQ, status and version replies, NAK codes; ETSI TS 102 361-1 (EMB, slot type, LCSS per burst), TS 102 361-2 (the preamble CSBK) | reviewed 2026-10-24. Other facts: the AMBE+2 silence frame (the codec's output, checked in host-tests/test_core.c), MMDVM_HS board names matched in a description, OpenGD77's AT1846S RSSI conversion (one line of arithmetic) |
| common/src/modem.c, common/include/rdof/modem.h, rust/src/modem.rs | clean | the MMDVM protocol as above | reviewed 2026-10-24: a request / reply client of our own; no MMDVMHost `CModem` structure |
| common/src/hblink.c, common/include/rdof/hblink.h, rust/src/hblink.rs | clean | HBlink4's packets as interface facts (DMRD, DMRC, DMRP, MSTN / MSTC, DMRT / DMRK) | reviewed 2026-10-24: byte builders and parsers of our own; nothing of hblink3's, HBlink4's or MMDVMHost's network code |
| common/src/pdu.c, common/include/rdof/pdu.h, rust/src/pdu.rs | clean | ETSI TS 102 361-1 9.3 (LC, CSBK, data header by DPF, UDT, response), TS 102 361-2 7.1 (FLCOs, CSBK opcodes, GPS and talker alias LCs) | reviewed 2026-10-24 |
| common/src/txvr.c, common/include/rdof/txvr.h, rust/src/txvr.rs | clean | docs/spec/repeater.md (sections 6, 7, 10, 11); ETSI as above; Motorola TMS's UDP port and ack flag (interface facts) | reviewed 2026-10-24: the roaming transceiver is our design; no MMDVMHost `CDMRSlot` network-side structure |
| common/src/repeater.c, common/include/rdof/repeater.h, rust/src/repeater.rs | clean | docs/spec/repeater.md (R3-R13) | reviewed 2026-10-24: no `CDMRSlot` RF / network state machine structure, names or tables |
| common/src/tracker.c, common/include/rdof/tracker.h, rust/src/tracker.rs | clean | ETSI as above; MMDVMHost's MQTT JSON format as an interface (message names, keys, action values and the `csbk_desc` strings, for the programs that read it); Hinnant (row above) | reviewed 2026-10-24: the call tracking is our own; see Open items for the JSON strings |
| common/src/rawburst.c, common/include/rdof/rawburst.h, rust/src/rawburst.rs | clean | the MQTTRaw JSON message of the authors' MMDVMHost fork, as an interface; the binary record is ours | reviewed 2026-10-24 |
| common/src/jitter.c, common/include/rdof/jitter.h, rust/src/jitter.rs | clean | docs/spec/repeater.md R6; RFC 3550 6.4.1 (the jitter estimate) | reviewed 2026-10-24: adaptive design of our own; MMDVMHost's fixed jitter delay shares only ideas (silence for a stalled stream, 6 bursts) |
| common/src/netcalls.c, common/src/lastheard.c, common/src/slotgrid.c, their headers, rust/src/netcalls.rs, rust/src/slotgrid.rs | clean | our own specs (docs/spec/slot-timing.md, docs/spec/fw/) | reviewed 2026-10-24 |
| common/src/oled.c (but `GLYPHS`), common/src/oled_sdr.c, common/include/rdof/oled.h, common/include/rdof/oled_sdr.h, rust/src/oled.rs | clean | the SSD1306 and SH1106 datasheets (command set, initialization sequence) | reviewed 2026-10-24: nothing of MMDVMHost's old OLED.cpp or ArduiPi_OLED; the font has its row above |
| common/src/bits.c, json.c, jsonp.c, config.c, status.c, benchtx.c, benchcfg.c, memplan.c, usbmatch.c, their headers; rust/src/bits.rs, json.rs, jsonp.rs, config.rs, status.rs, benchtx.rs, benchcfg.rs, usbmatch.rs, ffi.rs, layout.rs, text.rs, lib.rs | clean | our own specs (docs/spec/fw/config.md, docs/spec/bench-api.md, docs/reference.md) | reviewed 2026-10-24: application code, no outside source |
| sdr/src/demod.c, sdr/include/rdof/sdr/demod.h | clean | ETSI TS 102 361-1 9.1.1 (sync patterns), 10.2 (4FSK symbols and the dibit mapping); general DSP | reviewed 2026-10-24: correlation sync search, slicer and voice flywheel of our own; nothing of dsd, dsd-fme or SDRangel's DSD demodulator |
| sdr/src/mod.c, channelizer.c, fm.c, resample.c, spectrum.c, iq.c, rate.c, ppm.c, receiver.c, frontend.c, loopback.c, bench.c, status.c, config.c and their headers in sdr/include/rdof/sdr/ | clean | ETSI TS 102 361-1 10.2 (deviation, RRC alpha 0.2); textbook DSP (RRC, Kaiser window, overlap-save channelizer, polyphase resampler, a published minimax atan polynomial) | reviewed 2026-10-24 |
| components/lr2021/src/lr2021.c, lr2021_cmd.c, lr2021_tx.c, lr2021_tx_stream.c, lr2021_packet.c, lr2021_patch.c, lr2021_patch_load.c, include/lr2021.h, lr2021_cmd.h, lr2021_tx.h, lr2021_patch.h, test/ | clean | DS.LR20xx Rev. 2.2; docs/spec/lr2021-iq.md for the raw I/Q commands (facts from lr2021-sdr, row above) | reviewed 2026-10-24: the raw route's argument bytes are the chip's interface, built from named fields; no lr2021-sdr code or structure |
| components/lr2021/pram/rdof_pram.S, pram/build.sh, src/lr2021_rdof_pram_image.c | clean | facts read from LR2021 ROM 1.24 (the addresses it calls, the JLI slots it hooks; notes in docs/spec/lr2021-patch.local.md, not published) | reviewed 2026-10-24: nothing of the ROM or of Semtech's image is copied; the image is the assembled source |
| components/netclock/ | clean | RFC 5905, RFC 4330; chrony's documented behavior (no chrony code); ported from smartlaps (`timebase`, `clock_model`, `ntp_client`, `ntp_select`; the same author's project) | spot-checked 2026-10-24 against chrony's source: nothing shared. Checked again 2026-10-27 against chrony's master: no identical line (over 25 characters, comments and whitespace removed); 16.4% of 12-token windows (names and numbers blanked) match, as for components unrelated to chrony (gnss 14.5%, wifilist 15.5%, tinysctp 20.9%) |
| components/tinysctp/ | clean | RFC 9260, RFC 3758, RFC 6951; the SipHash paper; CRC32c from RFC 9260 appendix A | spot-checked 2026-10-24: no Linux SCTP or usrsctp code |
| components/wifilist/, components/gnss/ | clean | ESP-IDF's Wi-Fi and UART APIs; NMEA 0183 (gnss's date arithmetic has its row above) | spot-checked 2026-10-24 |
