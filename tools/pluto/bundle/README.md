# RadioFoundry Gateway for the PlutoSDR

`rdofradio` (a DMR repeater on the SDR, with its web page) for a PlutoSDR or a Zynq 7010 / 7020 clone running ADI's
firmware v0.3x as it comes. Nothing else to install on the Pluto: it uses the firmware's own libiio and libad9361.

## Install

From a computer the Pluto is plugged into (it asks for the Pluto's root password, `analog` as it comes):

```sh
./install.sh --config my.json            # first install, with your settings
./install.sh --example                   # first install, with config.example.json
./install.sh                             # upgrade: keeps the settings and flags already there
./install.sh --host root@pluto.local     # a Pluto at another address (192.168.2.1 by default)
./install.sh --uninstall
```

Or copy the bundle to the Pluto and run `./install.sh --here ...` there.

It goes in the Pluto's persistent flash (`/mnt/jffs2`, about 300 KB), starts at boot from `/mnt/jffs2/autorun.sh`, and
starts again 5 s after it exits.

## On the Pluto

| What | Where |
|---|---|
| The settings (the web page's Settings tab edits them; a save restarts it) | `/mnt/jffs2/rdof/config.json` |
| The device's flags (RX gain, TX gain, web address, the clock's state file) | `/mnt/jffs2/rdof/flags` |
| The clock's learned rate | `/mnt/jffs2/rdof/netclock.state` |
| The log (RAM: the last 1 MB, then `log.1`) | `/tmp/rdof/log` |
| How it last ended on its own (a crash, an error): the page's crash card (Logs & maintenance) | `/tmp/rdof/last_exit` |
| An update from the web page (Logs & maintenance, Firmware update): the bundle's `rdofradio.gz`, or `rdofradio` itself | `/tmp/rdof/rdofradio.ota` until `run.sh` takes it |
| Keep it from starting | `touch /mnt/jffs2/rdof/disabled`, then reboot |
| The web page | `http://192.168.2.1:8080/` |

**Updates:** the web page's Firmware update (or `curl --data-binary @rdofradio.gz http://192.168.2.1:8080/api/ota`)
sends a new `rdofradio`: gzipped (the bundle's `rdofradio.gz`) or not, an ELF for the Pluto's ARM. rdofradio stops,
and `run.sh` checks that it unpacks and runs, puts it in place of the one in flash and starts it. The flash has no
room for both, so the old one stays in RAM until the next boot: if the new one ends in error within 30 s of its first
start, the old one goes back. `install.sh` updates `run.sh` and `VERSION` too; an update from the page doesn't
(the page shows the program's own version).

The settings are the firmware's settings document, version 3 ([config-v3.md](../../../docs/spec/fw/config-v3.md),
checked by `tools/config_check.py` against `config/rdof-config.schema.json` in the repo). The
example is a repeater on the Pluto's own SDR: it receives on 438.25 MHz and transmits on 438.0125 MHz, with a parrot
on talkgroup 9990. **It transmits whenever it hears a call: use frequencies you're licensed for.** The flags start at
the lowest TX power (`--tx-gain 0`, 89 dB of attenuation); raise it in `flags`. At that power the start's calibration
(a pulse sent and heard back) isn't heard, and it says so; it runs anyway.

The network: add a `networks` list (`[{"type": "hblink", "host": "your.master", "port": 62031}]`) and a `callsign`,
and the box is an HBlink4 peer with the DMR ID in `id`. Which talkgroups go up and come down is each path's policy
(`tg` on the repeater app, or on its `slots`); with `"identity": "per_path"` on the network, each frequency or
repeater is its own peer and the master decides. Without the `tg` keys, the repeater repeats locally and sends
nothing up. The clock: `ntp` names the servers; the Pluto's own clock (which starts in 1970) is set from it
(`--set-os-clock`). Without a route to them, opening the web page sets it from the browser's clock.
