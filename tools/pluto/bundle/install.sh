#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#
# Install RadioFoundry Gateway's rdofradio on a PlutoSDR (or a 7010 / 7020 clone) running ADI's firmware v0.3x,
# so it starts at boot and again whenever it exits. Everything goes in the Pluto's persistent flash, /mnt/jffs2
# (about 300 KB); the firmware runs /mnt/jffs2/autorun.sh at boot. The FPGA's bitstream for the board (fpga/, for
# --fpga-chz and --fpga-duc) doesn't fit there: it goes in rdof/ on the SD card's boot partition (the one with
# BOOT.bin, which is never touched), written only when it differs from the one there, and the flags get
# --fpga-chz --fpga-duc, so rdofradio loads it at every boot. No SD card, or no bitstream for the board: rdofradio
# runs without it.
#
# From a computer the Pluto is plugged into (asks for the Pluto's root password, "analog" as it comes):
#   ./install.sh [--host root@192.168.2.1] --config my.json     first install, with your settings
#   ./install.sh --example                                         first install, with config.example.json
#   ./install.sh                                                   upgrade: keeps the settings and flags there
#   ./install.sh --uninstall
# On the Pluto itself: the same, with --here.
#
# The settings are a repeater (config.example.json: RX 438.25, TX 438.0125 MHz, a parrot on TG 9990). It transmits
# when it hears a call: use frequencies you're licensed for. The flags start at the lowest TX power (--tx-gain 0);
# edit /mnt/jffs2/rdof/flags for more. The web page is at http://<pluto>:8080/.
set -e
HOST=root@192.168.2.1
HERE=0
CONFIG=
EXAMPLE=0
UNINSTALL=0
ARGS=
while [ $# -gt 0 ]; do
    case "$1" in
    --host) HOST=$2; shift ;;
    --here) HERE=1 ;;
    --config) CONFIG=$2; shift ;;
    --example) EXAMPLE=1; ARGS="$ARGS --example" ;;
    --uninstall) UNINSTALL=1; ARGS="$ARGS --uninstall" ;;
    *) echo "install.sh: unknown $1 (see the top of the file)" >&2; exit 2 ;;
    esac
    shift
done
DIR=$(cd "$(dirname "$0")" && pwd)

if [ $HERE = 0 ]; then
    # from a computer: the bundle (and the settings, if given) to the Pluto's /tmp, then this script there
    FILES="install.sh run.sh rdofradio.gz config.example.json flags.example VERSION"
    [ -d "$DIR/fpga" ] && FILES="$FILES fpga"
    if [ -n "$CONFIG" ]; then
        cp "$CONFIG" "$DIR/config.user.json"
        FILES="$FILES config.user.json"
        ARGS="$ARGS --config config.user.json"
    fi
    echo "$HOST: its root password is \"analog\" unless you changed it"
    # the Pluto makes a new host key at each boot (its root file system is in RAM): no known_hosts entry for it
    R=0
    (cd "$DIR" && tar -cf - $FILES) | ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o LogLevel=ERROR "$HOST" \
        "rm -rf /tmp/rdof-install && mkdir -p /tmp/rdof-install && cd /tmp/rdof-install && tar -xf - && sh ./install.sh --here $ARGS; R=\$?; rm -rf /tmp/rdof-install; exit \$R" || R=$?
    rm -f "$DIR/config.user.json"
    exit $R
fi

# on the Pluto
D=/mnt/jffs2/rdof
A=/mnt/jffs2/autorun.sh
LINE='[ -x /mnt/jffs2/rdof/run.sh ] && /mnt/jffs2/rdof/run.sh < /dev/null > /dev/null 2>&1 &'
[ -d /mnt/jffs2 ] || { echo "no /mnt/jffs2: not a PlutoSDR running ADI's firmware" >&2; exit 1; }

stop() {
    [ -f /tmp/rdof/run.pid ] && kill "$(cat /tmp/rdof/run.pid)" 2>/dev/null || true
    killall rdofradio 2>/dev/null || true
    rm -f /tmp/rdof/run.pid
}

if [ $UNINSTALL = 1 ]; then
    stop
    rm -rf $D
    if [ -f $A ]; then
        grep -vF "$LINE" $A | grep -v '^# rdofradio:' > $A.new || true
        if grep -q '[^[:space:]]' $A.new; then mv $A.new $A; else rm -f $A $A.new; fi
    fi
    sync
    echo "removed"
    exit 0
fi

if [ -z "$CONFIG" ] && [ $EXAMPLE = 0 ] && [ ! -f $D/config.json ]; then
    echo "first install: give your settings (--config FILE) or take the example's (--example)" >&2
    exit 2
fi
# check the settings before anything changes: a start that fails at once
gunzip -c rdofradio.gz > /tmp/rdofradio.check && chmod 755 /tmp/rdofradio.check
NEW=
[ -n "$CONFIG" ] && NEW=$CONFIG
[ -z "$NEW" ] && [ $EXAMPLE = 1 ] && NEW=config.example.json
if [ -n "$NEW" ]; then
    # a device that isn't there: the settings are read first, and nothing opens the radio
    OUT=$(/tmp/rdofradio.check --iio none: --allow-tx --config "$NEW" 2>&1) || true
    if echo "$OUT" | grep "^$NEW:" | grep -qv "not used here"; then
        echo "$OUT" | grep "^$NEW:" >&2
        rm -f /tmp/rdofradio.check
        exit 1
    fi
fi

# The bitstream: rdof/ on the SD card's boot partition, written only if it differs (rdofradio_fpga.h looks there)
FPGA=0
fpga() {
    BF=$(/tmp/rdofradio.check --fpga-board 2> /dev/null) || { echo "fpga: no bitstream for this board: it runs without one"; return 0; }
    FILE=${BF#* }
    [ -f fpga/$FILE ] || { echo "fpga: no $FILE in this bundle: it runs without one, or with one already there"; return 0; }
    M=/tmp/rdof-sd
    for P in /dev/mmcblk[0-9]p[0-9]; do
        [ -b $P ] || continue
        grep -q "^$P " /proc/mounts && continue # mounted by someone else: not this one
        mkdir -p $M && mount -t vfat -o ro $P $M 2> /dev/null || { rmdir $M; continue; }
        if [ ! -f $M/BOOT.bin ]; then umount $M; rmdir $M; continue; fi
        if [ -f $M/rdof/$FILE ] && cmp -s fpga/$FILE $M/rdof/$FILE; then
            echo "fpga: $FILE on $P is up to date"
            FPGA=1
        elif mount -o remount,rw $M; then
            mkdir -p $M/rdof && cp fpga/$FILE $M/rdof/$FILE.new && sync && mv $M/rdof/$FILE.new $M/rdof/$FILE &&
                cp fpga/LICENSE fpga/LICENSE_ADIBSD $M/rdof/ && sync && echo "fpga: $FILE written to rdof/ on $P" && FPGA=1 ||
                echo "fpga: $FILE: writing rdof/ on $P failed" >&2
        else
            echo "fpga: $P won't mount read-write: $FILE not written" >&2
        fi
        umount $M; rmdir $M
        return 0
    done
    echo "fpga: no SD card with a boot partition: $FILE not written; it runs without it"
}
fpga
rm -f /tmp/rdofradio.check

stop
mkdir -p $D
cp rdofradio.gz run.sh VERSION $D/
chmod 755 $D/run.sh
[ -n "$NEW" ] && cp "$NEW" $D/config.json
[ -f $D/flags ] || cp flags.example $D/flags
# the board's bitstream is on its SD card: rdofradio loads it at start
if [ $FPGA = 1 ] && ! grep -q -- '--fpga-chz' $D/flags; then
    printf '%s --fpga-chz --fpga-duc\n' "$(cat $D/flags)" > $D/flags.new && mv $D/flags.new $D/flags
    echo "fpga: --fpga-chz --fpga-duc added to $D/flags"
fi
rm -f $D/disabled
if ! { [ -f $A ] && grep -qF "$LINE" $A; }; then
    printf '# rdofradio: RadioFoundry Gateway (/mnt/jffs2/rdof/run.sh)\n%s\n' "$LINE" >> $A
fi
sync
# started away from this session, as at boot
if command -v setsid > /dev/null; then setsid $D/run.sh < /dev/null > /dev/null 2>&1 &
else $D/run.sh < /dev/null > /dev/null 2>&1 &
fi
echo "installed $(cat $D/VERSION): settings $D/config.json, flags $D/flags, log /tmp/rdof/log;"
echo "the web page at http://$(ip -4 addr show usb0 2>/dev/null | awk '/inet /{sub("/.*","",$2); print $2; exit}'):8080/"
