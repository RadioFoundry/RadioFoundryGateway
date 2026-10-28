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
| The device's flags (RX gain, TX gain, web address) | `/mnt/jffs2/rdof/flags` |
| The log (RAM: the last 1 MB, then `log.1`) | `/tmp/rdof/log` |
| Keep it from starting | `touch /mnt/jffs2/rdof/disabled`, then reboot |
| The web page | `http://192.168.2.1:8080/` |

The settings are the firmware's settings document, version 2 (`config/rdof-config.schema.json` in the repo); a
repeater on the Pluto's own SDR. The example's repeater receives on 438.25 MHz and transmits on 438.0125 MHz, with a
parrot on talkgroup 9990. **It transmits whenever it hears a call: use frequencies you're licensed for.** The flags
start at the lowest TX power (`--tx-gain 0`, 89 dB of attenuation); raise it in `flags`. At that power the start's
calibration (a pulse sent and heard back) isn't heard, and it says so; it runs anyway.
