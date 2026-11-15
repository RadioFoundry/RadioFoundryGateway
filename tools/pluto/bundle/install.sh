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
# A program too big for the flash (the trunked site's) goes on the SD card: install.sh makes a partition for it,
# labeled RDOF, once, in the free space after the boot partition (the MBR gets a second entry; the boot partition and
# its files aren't touched; the MBR as it was is kept in /mnt/jffs2/rdof/sd-mbr.orig). run.sh runs the newer of the
# two places' programs (run.sh's top).
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
SDTEST=
ARGS=
while [ $# -gt 0 ]; do
    case "$1" in
    --host) HOST=$2; shift ;;
    --here) HERE=1 ;;
    --config) CONFIG=$2; shift ;;
    --example) EXAMPLE=1; ARGS="$ARGS --example" ;;
    --uninstall) UNINSTALL=1; ARGS="$ARGS --uninstall" ;;
    --sd-test) SDTEST="$2 $3"; shift 2 ;; # the host test: the plan for MBR file $2 on a disk of $3 sectors
    *) echo "install.sh: unknown $1 (see the top of the file)" >&2; exit 2 ;;
    esac
    shift
done
DIR=$(cd "$(dirname "$0")" && pwd)

# The SD card's RDOF partition: where it goes, for an MBR (a file) and the card's size in sectors. Prints the start
# and the length in sectors, or why not. Only beside a lone FAT boot partition (entries 2-4 empty). It starts on the
# MiB after that partition and ends 1 MiB below the power of two (in MiB) above it: a card labeled 128 MB that says it
# holds 3.76 GiB keeps nothing past 128 MiB, and its boot partition ends at 119.4 MiB. At least 4 MiB.
le32() { od -An -tu4 -j$2 -N4 $1 | tr -d ' '; }
sd_plan() {
    [ "$(od -An -tx1 -j510 -N2 $1 | tr -d ' \n')" = 55aa ] || { echo "no MBR"; return 1; }
    [ -z "$(od -An -tx1 -j462 -N48 $1 | tr -d ' 0\n*')" ] ||
        { echo "the card has partitions after the first"; return 1; }
    case "$(od -An -tx1 -j450 -N1 $1 | tr -d ' \n')" in
    0b | 0c | 0e | 06 | 04) ;;
    *) echo "its first partition isn't FAT"; return 1 ;;
    esac
    E1=$(($(le32 $1 454) + $(le32 $1 458)))
    ST=$(((E1 + 2047) / 2048 * 2048))
    P=2048
    while [ $P -lt $E1 ]; do P=$((P * 2)); done
    EN=$((P - 2048))
    [ $EN -gt $2 ] && EN=$(($2 / 2048 * 2048))
    [ $((EN - ST)) -ge 8192 ] || { echo "no room after the boot partition"; return 1; }
    echo $ST $((EN - ST))
}
# a little-endian 32-bit number as 4 octets
oct32() { V=$1; for i in 1 2 3 4; do printf "\\$(printf %03o $((V % 256)))"; V=$((V / 256)); done; }
# MBR file $1 with entry 2 = FAT32 (LBA) at $2, $3 sectors: $1.new
sd_mbr() {
    { head -c 462 $1; printf '\000\376\377\377\014\376\377\377'; oct32 $2; oct32 $3; tail -c 34 $1; } > $1.new
}
if [ -n "$SDTEST" ]; then
    set -- $SDTEST
    PLAN=$(sd_plan $1 $2) || { echo "$PLAN"; exit 1; }
    echo $PLAN
    sd_mbr $1 $PLAN
    exit 0
fi

if [ $HERE = 0 ]; then
    # from a computer: the bundle (and the settings, if given) to the Pluto's /tmp, then this script there
    FILES="install.sh run.sh rdofradio.xz config.example.json flags.example VERSION"
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
    # the program on the SD card goes too; its partition stays
    RDOF_RUN_LIB=1 . ./run.sh
    if sd_mount && sd_rw; then rm -rf $S/rdof; sd_ro; fi
    grep -q " $S " /proc/mounts && umount $S
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
unxz -c rdofradio.xz > /tmp/rdofradio.check && chmod 755 /tmp/rdofradio.check
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

# The SD card's RDOF partition (sd_plan above), made once. Before the MBR changes, a write to the last sector of each
# MiB of the new partition is read back, and the card's start (as long as the partition) is read before and after: a
# card smaller than it says keeps nothing there, or puts it at its start. Either way the start goes back and nothing
# changes. (One labeled 128 MB that says 3.76 GiB keeps, past 121 MiB, only the front of each MiB.)
SD=/dev/mmcblk0
sdpart() {
    [ -b $SD ] || return 0
    sd_find > /dev/null && return 0
    W=/tmp/rdof-part
    rm -rf $W && mkdir -p $W
    dd if=$SD of=$W/mbr bs=512 count=1 iflag=direct 2> /dev/null
    PLAN=$(sd_plan $W/mbr $(cat /sys/block/${SD#/dev/}/size)) ||
        { echo "sd: no RDOF partition: $PLAN"; rm -rf $W; return 0; }
    set -- $PLAN
    dd if=$SD of=$W/head bs=64k count=$(($2 / 128)) iflag=direct 2> /dev/null
    L=$(($1 + 2047))
    LOST=
    while [ $L -lt $(($1 + $2)) ]; do
        head -c 512 /dev/urandom > $W/probe
        dd if=$W/probe of=$SD bs=512 seek=$L count=1 oflag=direct conv=notrunc,fsync 2> /dev/null
        dd if=$SD of=$W/back bs=512 skip=$L count=1 iflag=direct 2> /dev/null
        cmp -s $W/probe $W/back || LOST="$LOST $L"
        L=$((L + 2048))
    done
    dd if=$SD of=$W/head2 bs=64k count=$(($2 / 128)) iflag=direct 2> /dev/null
    if ! cmp -s $W/head $W/head2; then
        dd if=$W/head of=$SD bs=64k oflag=direct conv=notrunc,fsync 2> /dev/null
        echo "sd: a write past sector $1 landed at the card's start (put back): the card is smaller than it says" >&2
        rm -rf $W; return 0
    fi
    [ -z "$LOST" ] ||
        { echo "sd: the card doesn't keep a write at sector$LOST: no RDOF partition" >&2; rm -rf $W; return 0; }
    sd_mbr $W/mbr $1 $2
    mkdir -p $D && cp $W/mbr $D/sd-mbr.orig
    dd if=$W/mbr.new of=$SD bs=512 count=1 oflag=direct conv=notrunc,fsync 2> /dev/null
    dd if=$SD of=$W/mbr.back bs=512 count=1 iflag=direct 2> /dev/null
    if ! cmp -s $W/mbr.new $W/mbr.back; then
        dd if=$W/mbr of=$SD bs=512 count=1 oflag=direct conv=notrunc,fsync 2> /dev/null
        echo "sd: the MBR didn't take the new entry: put back as it was" >&2
        rm -rf $W; return 0
    fi
    partprobe $SD 2> /dev/null || hdparm -z $SD > /dev/null 2>&1 || true
    i=0
    while [ ! -b ${SD}p2 ] && [ $i -lt 10 ]; do sleep 1; i=$((i + 1)); done
    if [ -b ${SD}p2 ] && mkfs.vfat -n RDOF ${SD}p2 > /dev/null; then
        echo "sd: RDOF partition made: ${SD}p2, $(($2 / 2048)) MiB from $(($1 / 2048)) MiB" \
            "(the MBR as it was: $D/sd-mbr.orig)"
    else
        echo "sd: ${SD}p2 is in the MBR but not formatted: reboot and install again" >&2
    fi
    rm -rf $W
}

# started away from this session, as at boot
start() {
    if command -v setsid > /dev/null; then setsid $D/run.sh < /dev/null > /dev/null 2>&1 &
    else $D/run.sh < /dev/null > /dev/null 2>&1 &
    fi
}

# stopped first: the card is read and written with the radio quiet. run.sh's functions: the SD card's partition,
# and putting the program in place
stop
RDOF_RUN_LIB=1 . ./run.sh
sdpart
mkdir -p $D /tmp/rdof
# the program: in the flash if it fits, else on the SD card's RDOF partition; the newer runs (run.sh). No room in
# either: the one there starts again
sd_mount || true
Q=$(($(newest_seq) + 1))
if fits rdofradio.xz; then TO=$D
elif sd_ok; then TO=$S/rdof
else
    echo "no room for the program in $D ($(df -k $D | awk 'NR == 2 {print $4}') KB free), and no SD card" \
        "partition (RDOF): nothing changed" >&2
    [ -x $D/run.sh ] && [ -f $D/config.json ] && start
    exit 1
fi
rm -f $D/rdofradio.gz # from before the bundle used xz: no room for both
if ! put rdofradio.xz $TO $Q; then
    echo "writing the program to $TO failed" >&2
    exit 1
fi
echo "the program: in $TO"
cp run.sh VERSION $D/
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
start
echo "installed $(cat $D/VERSION): settings $D/config.json, flags $D/flags, log /tmp/rdof/log;"
echo "the web page at http://$(ip -4 addr show usb0 2>/dev/null | awk '/inet /{sub("/.*","",$2); print $2; exit}'):8080/"
