# RadioFoundry Gateway for the PlutoSDR

`rdofradio` (a DMR repeater on the SDR, with its web page) for a PlutoSDR or a Zynq 7010 / 7020 clone on ADI's
firmware v0.3x as it comes. Nothing else to install on the Pluto: it uses the firmware's own libiio and libad9361.
The guide, from building to the FPGA channelizer: [docs/plutosdr.md](../../../docs/plutosdr.md).

## Install

From a computer the Pluto is plugged into (it asks for the Pluto's root password, `analog` as it comes):

```sh
./install.sh --config my.json            # first install, with your settings
./install.sh --example                   # first install, with config.example.json
./install.sh                             # upgrade: keeps the settings and flags already there
./install.sh --host root@pluto.local     # a Pluto at another address (192.168.2.1 by default)
./install.sh --uninstall
```

Or copy the bundle to the Pluto and run `./install.sh --here ...` there. Before installing, it checks the settings
with the new `rdofradio` on the Pluto.

It goes in the Pluto's persistent flash (`/mnt/jffs2`), starts at boot from `/mnt/jffs2/autorun.sh`, and starts
again 5 s after it exits.

**The FPGA bitstream** (`fpga/`, for `--fpga-chz` and `--fpga-duc`: every channel at once, 15 on a 7010 clone in
place of about 10) doesn't fit in the flash.

- `install.sh` writes the board's to `rdof/` on the SD card's boot partition (the one with `BOOT.bin`), only when the
  one there differs. It never touches the boot files.
- It adds `--fpga-chz --fpga-duc` to `flags`, so `rdofradio` loads it at every boot (`/tmp/rdof-fpga` says which is
  in). A reboot brings the stock design back.
- With `--split-tx` or `--mmdvm` in `flags` (a USB radio transmits), the channelizer still runs and `--fpga-duc` is
  unused. The firmware's kernel has no `cdc_acm`: `run.sh` gives a RadioFoundry radio the generic usb-serial driver,
  and it's `/dev/ttyUSB0` (the next one `ttyUSB1`).
- With no SD card, or no bitstream for the board, it runs without: the ARM channelizes, and the channels that don't
  fit its window are left out.

## On the Pluto

| What | Where |
|---|---|
| The settings (the page's Settings tab edits them; a save restarts it) | `/mnt/jffs2/rdof/config.json` |
| The device's flags (RX gain, TX gain, web address, the clock's state file) | `/mnt/jffs2/rdof/flags` |
| The clock's learned rate | `/mnt/jffs2/rdof/netclock.state` |
| The log (RAM: the last 1 MB, then `log.1`) | `/tmp/rdof/log` |
| How it last ended on its own (a crash, an error): the page's crash card (Logs & maintenance) | `/tmp/rdof/last_exit` |
| An update from the page, until `run.sh` takes it | `/tmp/rdof/rdofradio.ota` |
| The FPGA bitstream (when the board has one) | `rdof/` on the SD card's boot partition |
| Keep it from starting | `touch /mnt/jffs2/rdof/disabled`, then reboot |
| The web page | `http://192.168.2.1:8080/` |

## Updates

The page's Firmware update (Logs & maintenance), or `curl --data-binary @rdofradio.gz http://192.168.2.1:8080/api/ota`,
sends a new `rdofradio`: the bundle's `rdofradio.gz`, or an uncompressed ELF for the Pluto's ARM.

- `rdofradio` stops; `run.sh` checks that the new one unpacks and runs, puts it in flash in place of the old one and
  starts it.
- The flash has no room for both, so the old one stays in RAM until the next boot: if the new one ends in error within
  30 s of its first start, the old one goes back.
- `install.sh` updates `run.sh` and `VERSION` too; an update from the page doesn't (the page shows the program's own
  version).

## Settings

The firmware's settings document, version 3 ([config-v3.md](../../../docs/spec/fw/config-v3.md); check a file with
`tools/config_check.py` in the repo). Recipes: [docs/settings.md](../../../docs/settings.md).

- **The example** is a repeater on the Pluto's own SDR: in 438.25 MHz, out 438.0125 MHz, with a parrot on TG 9990.
  **It transmits whenever it hears a call: use frequencies you're licensed for.**
- **Power:** `flags` starts at the lowest TX power (`--tx-gain 0`, 89 dB of attenuation); raise it there. At that
  power the start's calibration (a pulse sent and heard back) isn't heard, and it says so; it runs anyway.
- **A network:** add a `networks` list (`[{"type": "hblink", "host": "your.master", "port": 62031}]`) and a
  `callsign`, and the box is an HBlink4 peer with the DMR ID in `id`. Which talkgroups go up and come down is each
  path's policy (`tg` on the repeater app, or on its `slots`). Without a `tg`, the repeater repeats locally and sends
  nothing up. With `"identity": "per_path"` on the network, each frequency or repeater is its own peer and the
  master decides.
- **The clock:** `ntp` names the servers, and the Pluto's clock (which starts in 1970) is set from them
  (`--set-os-clock`). With no route to them, opening the page sets it from the browser's clock.
