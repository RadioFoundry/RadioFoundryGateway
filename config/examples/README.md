# Example settings

Settings documents (version 3, [docs/spec/fw/config-v3.md](../../docs/spec/fw/config-v3.md)) to start from. The
frequencies are amateur ones and the IDs are placeholders: set your own, with the color code.

## Standalone: no network

Nothing in `networks`: the box links to no HBlink4 master and no MQTT broker. It runs two apps:

- the parrot: a call to TG 9990 is played back to the radio;
- the IP gateway: IP packets as DMR data between radios and the box's own network, sent to ID 9991
  ([docs/spec/fw/ipgw.md](../../docs/spec/fw/ipgw.md)). `"nat": true` lets radios reach IP hosts through the box;
  `routes` sends chosen datagrams to one host instead.

| File | Hardware | Mode |
|---|---|---|
| `standalone-mmdvm-simplex.json` | an MMDVM hotspot board (MMDVM_HS) | simplex, 438.5 MHz |
| `standalone-mmdvm-duplex.json` | a duplex MMDVM modem | a repeater, in 438.25 / out 438.75 MHz |
| `standalone-mmdvm-rtl-simplex.json` | an MMDVM hotspot board and an RTL-SDR | simplex; the RTL-SDR hears the frequency too |
| `standalone-mmdvm-rtl-duplex.json` | a duplex MMDVM modem and an RTL-SDR | a repeater; the RTL-SDR hears its input too |
| `standalone-pluto-simplex.json` | a PlutoSDR | simplex, half duplex |
| `standalone-pluto-duplex.json` | a PlutoSDR | a repeater, full duplex |

A simplex box answers a call after it ends, on the frequency it was heard on. A repeater repeats what it hears and
sends the apps' answers on its output.

Run one with `rdofradio` (the device stays on the command line):

```sh
rdofradio --allow-tx --mmdvm /dev/ttyAMA0 --config standalone-mmdvm-simplex.json --web 8080
rdofradio --soapy driver=rtlsdr -r 288000 --allow-tx --mmdvm /dev/ttyAMA0 --config standalone-mmdvm-rtl-simplex.json --web 8080
rdofradio --iio local: --allow-tx --config standalone-pluto-duplex.json --web 0.0.0.0:8080
```

With `"nat": true`, ping from a radio needs a raw socket: run as root or give the program `CAP_NET_RAW`; without it
NAT carries UDP only.
