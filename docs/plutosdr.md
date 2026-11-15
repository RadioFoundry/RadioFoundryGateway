# PlutoSDR

`rdofradio` runs on the PlutoSDR's own ARM as a DMR repeater or transceiver, with its web page, MQTT and the bench
line. It runs on ADI's stock firmware (v0.3x) on a PlutoSDR or a Zynq 7010 / 7020 clone, through the firmware's own
libiio and libad9361: nothing else to install on the Pluto.

A Pluto on a PC's USB, driven from the PC, is the host tools' job instead: [linux-macos.md](linux-macos.md).

| | ARM alone | With the FPGA channelizer |
|---|---|---|
| Boards | any Pluto or 7010 / 7020 clone on stock firmware | a 7010 clone with 1 GB of 32-bit DDR and Ethernet (`fishball7010`), so far |
| Channels | about 10 (at 1.296 MS/s) | 15 |
| Tested | a Rev.C and a 7010 clone, firmware v0.38: full and half duplex with handhelds | the 7010 clone: 15 channels in loopback |

## 1. Build

On an x86_64 machine with Docker:

```sh
tools/pluto/build.sh
```

It builds in a Debian 9 container against ADI's v0.38 sysroot, and writes:

| File | What |
|---|---|
| `build/pluto/rdof-pluto.tar.gz` | `rdofradio`, `rdofsdr`, `rdofchz`, to try from `/tmp` |
| `build/pluto/rdof-pluto-bundle-VERSION.tar.gz` | the installer: `rdofradio.gz`, `install.sh`, `run.sh`, the examples, the FPGA bitstreams |

The programs need only libiio, libad9361 and libc. MQTT is linked in, without TLS.

## 2. Try it from RAM

Nothing is written to the Pluto's flash; a reboot clears it. The Pluto's root password is `analog` as it comes, its
address over USB `192.168.2.1`.

```sh
scp -O build/pluto/rdof-pluto.tar.gz root@192.168.2.1:/tmp
ssh root@192.168.2.1
cd /tmp && gunzip -c rdof-pluto.tar.gz | tar -xf -
./rdofradio --iio local: --allow-tx --rx 438.25 --tx 438.75 --duplex full --cc 1 --gain 20 --tx-gain 65 \
    --parrot 9990 --web 0.0.0.0:8080
```

Then open `http://192.168.2.1:8080/`. **It transmits whenever it hears a call: use frequencies you're licensed for.**

- `--iio` runs at 768 kS/s by default; up to 2.4 MS/s on the Pluto's ARM.
- RX and TX tune separately: any split both reach (325 MHz to 3.8 GHz on an AD9363, 70 MHz to 6 GHz on an AD9361).
- TX gain is 0..89 dB (89..0 dB of attenuation).

## 3. Install

The bundle puts `rdofradio` in the Pluto's flash and starts it at every boot. From a computer the Pluto is plugged
into:

```sh
tar -xzf build/pluto/rdof-pluto-bundle-VERSION.tar.gz && cd rdof-pluto-bundle-VERSION
./install.sh --config my.json      # or --example: config.example.json, a repeater with the parrot
```

- `my.json` is a settings file with an `iio` device ([settings.md](settings.md); start from `config.example.json` or
  `config/examples/standalone-pluto-duplex.json`). `install.sh` checks it on the Pluto before it installs anything.
- It asks for the root password. `--host root@<address>` for a Pluto that isn't at 192.168.2.1.
- The page answers about 25 s after power-on (tested on a 7010 clone).
- Every option, and where each file goes: [the bundle's README](../tools/pluto/bundle/README.md).

The device flags (RX and TX gain, the page's address, the clock, the bench) are in `/mnt/jffs2/rdof/flags`; the
example starts at TX gain 0, the least. Raise it there; the next start takes it (the page's restart, in Logs &
maintenance, or a reboot):

```
--gain 20 --tx-gain 0 --web 0.0.0.0:8080 --clock-state /mnt/jffs2/rdof/netclock.state --set-os-clock --bench 7373
```

At TX gain 0 the start's calibration pulse isn't heard, so the outbound's slot timing isn't measured; it says so and
runs anyway.

## 4. Run

- **The page:** `http://192.168.2.1:8080/`. The Settings tab edits `/mnt/jffs2/rdof/config.json`; a save restarts
  `rdofradio`.
- **The log:** `/tmp/rdof/log` (in RAM, the last 1 MB, then `log.1`). How it last ended: `/tmp/rdof/last_exit` and the
  page's crash card.
- **The clock:** the Pluto starts in 1970. With `ntp` servers in the settings, `rdofradio` sets it
  (`--set-os-clock`); with no route to them, opening the page sets it from the browser.
- **Keep it from starting:** `touch /mnt/jffs2/rdof/disabled`, then reboot.
- **HBlink4:** add a `networks` list and a `callsign` to the settings: [settings.md](settings.md).

## 5. Update

- The page's Logs & maintenance tab, Firmware update: the bundle's `rdofradio.gz` (or `rdofradio` itself). Or:
  `curl --data-binary @rdofradio.gz http://192.168.2.1:8080/api/ota`.
- `run.sh` checks the new one runs, puts it in flash in place of the old, and puts the old one back if the new one
  fails within 30 s.
- `./install.sh` with no options upgrades too: it keeps the settings and flags, and also updates `run.sh` and
  `VERSION`. An update from the page doesn't.
- `./install.sh --uninstall` removes it.

## 6. The FPGA channelizer

On a board with a bitstream, the FPGA does the channelizing and the transmit interpolation, and the ARM only
demodulates: 15 channels across a wide window, where the ARM alone keeps about 10.

- `install.sh` copies the board's bitstream (`fpga/rfg-pluto-<board>.bit.bin`) to `rdof/` on the SD card's boot
  partition and adds `--fpga-chz --fpga-duc` to `flags`. It never touches `BOOT.bin` or the other boot files.
- `rdofradio` loads it at start, after checking it's for this chip. `/tmp/rdof-fpga` says what's loaded. A reboot
  brings the stock design back.
- `rdofradio --fpga-board` prints the board it finds and the bitstream's file name.
- No SD card, or no bitstream for the board: the ARM channelizes, and channels that don't fit its window are left
  out, in the settings' order.
- With the FPGA there's no wideband I/Q: the page's spectrum is each channel's own, and the bench captures channels.
- With a radio on the Pluto's USB transmitting (`--split-tx` or `--mmdvm`), the channelizer still runs and
  `--fpga-duc` is unused. Not yet tested with a radio.

How it works, the rates and the measurements: [reference.md](reference.md), "FPGA channelizer" and "The bitstream";
the design's license and origin: [tools/pluto/fpga/README.md](../tools/pluto/fpga/README.md).
