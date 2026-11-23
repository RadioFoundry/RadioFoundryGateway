# Docs

## Guides

| I want to... | Read |
|---|---|
| run a hotspot, a repeater or a Tier III site on a Raspberry Pi with an MMDVM_HS HAT | [raspberry-pi.md](raspberry-pi.md) |
| run a repeater or a Tier III site on a PlutoSDR's own ARM, with or without the FPGA channelizer | [plutosdr.md](plutosdr.md) |
| build and flash a W12, an N16R8 or a SuperMini, get it on Wi-Fi, update it | [esp32-boards.md](esp32-boards.md) |
| build `rdofsdr` and `rdofradio` on Linux or macOS; receive many channels; run an SDR or a modem; the web page; a bench line | [linux-macos.md](linux-macos.md) |
| write a settings file: a monitor, a W12, a BrandMeister hotspot, a repeater, an IP gateway, a Tier III site | [settings.md](settings.md) |
| start from a complete settings file | [config/examples/](../config/examples/README.md) |
| test radios with a bench gateway over TCP 7373 | [bench-api.md](spec/bench-api.md) section 14, `tools/bench/rfgbench.py -h` |

## Reference

[reference.md](reference.md): the build options, the layout, each board, the settings a board adds, the SDR role
and its CPU budgets, the host tools in full, the PlutoSDR's measurements, and what isn't done.

## Specs

How each part works, in [spec/](spec/). Start with [overview.md](spec/fw/overview.md).

| Topic | Spec |
|---|---|
| the settings, every key | [config-v3.md](spec/fw/config-v3.md) |
| a trunked site (DMR Tier III); one on a stock MMDVM modem | [tier3.md](spec/fw/tier3.md), [tier3-hat.md](spec/fw/tier3-hat.md) |
| the bench: the line protocol (TCP 7373), signal modes, measurements | [bench-api.md](spec/bench-api.md) |
| the HTTP API and the web page | [interfaces-http.md](spec/fw/interfaces-http.md) |
| the status LED, OLED, self-test, crashes, power | [device.md](spec/fw/device.md) |
| HBlink4, BrandMeister and other masters | [hblink.md](spec/fw/hblink.md), [brandmeister.md](spec/fw/brandmeister.md) |
| MQTT topics; calls between gateways and sites over MQTT | [mqtt.md](spec/fw/mqtt.md), [mqtt-net.md](spec/fw/mqtt-net.md) |
| the roamer, the repeater, the modems | [roamer.md](spec/fw/roamer.md), [repeater.md](spec/repeater.md), [radio.md](spec/fw/radio.md) |
| the IP gateway | [ipgw.md](spec/fw/ipgw.md) |
| the SDR: receive, transmit, front ends, the device API | [sdr.md](spec/fw/sdr.md), [sdr-rx.md](spec/fw/sdr-rx.md), [sdr-tx.md](spec/fw/sdr-tx.md), [sdr-frontends.md](spec/fw/sdr-frontends.md), [sdr-device-api.md](spec/fw/sdr-device-api.md) |
| many receivers and transmitters on one gateway | [rxtx-combinations.md](spec/fw/rxtx-combinations.md), [transmitters.md](spec/fw/transmitters.md) |
| DMR itself: bursts, FEC, LC, the call tracker | [dmr-core.md](spec/fw/dmr-core.md) |
| UTC slot timing | [slot-timing.md](spec/slot-timing.md) |
| memory on the S3 | [memory.md](spec/fw/memory.md) |
| the LR2021's raw I/Q; the RTL-SDR's registers | [lr2021-iq.md](spec/lr2021-iq.md), [rtlsdr.md](spec/rtlsdr.md) |
| the end-to-end tests | [system-tests.md](spec/system-tests.md) |
| what the RadioFoundry radio firmware is asked for | [radiofoundry-handoff.md](spec/fw/radiofoundry-handoff.md) |

## Components and modules

- `components/*/README.md`: the standalone ESP-IDF components: [netclock](../components/netclock/README.md) (the
  disciplined clock), [wifilist](../components/wifilist/README.md) (Wi-Fi networks), [tinysctp](../components/tinysctp/README.md),
  [lr2021](../components/lr2021/README.md), [gnss](../components/gnss/README.md).
- [rust/README.md](../rust/README.md): the Rust modules and their C twins.
- [hostweb/README.md](../hostweb/README.md): the web page on the host tools.
