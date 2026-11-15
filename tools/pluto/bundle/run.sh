#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# rdofradio on a PlutoSDR: started at boot by /mnt/jffs2/autorun.sh, started again 5 s after it exits.
# Settings: /mnt/jffs2/rdof/config.json (the web page edits it). The device's flags: /mnt/jffs2/rdof/flags.
# touch /mnt/jffs2/rdof/disabled to keep it from starting. Log: /tmp/rdof/log (RAM; the last 1 MB and log.1).
# Each time it ends on its own (a crash, an error), /tmp/rdof/last_exit records how (rc, 128 + the signal) and the
# log's last lines: the web page's crash card.
# The program is kept in flash as rdofradio.xz (xz, the bundle's: about 15 % smaller than gzip) or rdofradio.gz (an
# install from before the bundle used xz, or an update sent gzipped or as an ELF). A program too big for the flash
# goes on the SD card instead, in rdof/ on its RDOF partition (install.sh makes it after the boot partition, which is
# never touched): mounted read-only at /tmp/rdof/sd, read-write only while an image is written. Each place has a
# sequence number (seq beside the image): the newest image runs, the other is the fallback when it won't unpack.
# The web page's update (POST /api/ota) leaves the new program in /tmp/rdof/rdofradio.ota (xz, gzip, or an ELF) and
# rdofradio stops: it's checked, put in the flash if it fits there, else on the SD card, and started. The flash has
# no room for two (about 900 KB in all), so the one it replaces stays in RAM until the next boot: a new one that ends
# in error within 30 s of its first start is put back where the new one went.
# RDOF_RUN_ROOT, RDOF_RUN_SD, RDOF_RUN_ROOM_KB and RDOF_RUN_PAUSE are for the host test (host-tests/pluto_bundle.sh).
D=${RDOF_RUN_ROOT:-}/mnt/jffs2/rdof
T=${RDOF_RUN_ROOT:-}/tmp/rdof
S=${RDOF_RUN_SD:-$T/sd}
say() { echo "== $* $(date)" >> $T/log; }
up() { cut -d. -f1 /proc/uptime 2> /dev/null || date +%s; }
magic() { head -c 2 "$1" | od -An -tx1 | tr -d ' \n'; }
unpack() {
    case "$(magic "$1")" in
    1f8b) gunzip -c "$1" ;;
    fd37) unxz -c "$1" ;;
    *) return 1 ;;
    esac
}

# the SD card's RDOF partition, by the label in its FAT32 boot sector (busybox's blkid doesn't see it: it has too few
# clusters for FAT32 by count, though made as FAT32)
sd_find() {
    for P in /dev/mmcblk[0-9]p[0-9]; do
        [ -b $P ] && [ "$(dd if=$P bs=1 skip=71 count=5 2> /dev/null)" = "RDOF " ] && { echo $P; return 0; }
    done
    return 1
}
# the SD card's RDOF partition, read-only; rw/ro around a write. RDOF_RUN_SD: a plain directory (the test)
sd_mount() {
    [ -n "${RDOF_RUN_SD:-}" ] && return 0
    grep -q " $S " /proc/mounts && return 0
    P=$(sd_find) && mkdir -p $S && mount -t vfat -o ro,noatime "$P" $S 2> /dev/null ||
        { rmdir $S 2> /dev/null; return 1; }
}
sd_rw() { [ -n "${RDOF_RUN_SD:-}" ] || mount -o remount,rw $S; }
sd_ro() { sync; [ -n "${RDOF_RUN_SD:-}" ] || mount -o remount,ro $S; }
sd_ok() { [ -n "${RDOF_RUN_SD:-}" ] && [ -d $S ] || grep -q " $S " /proc/mounts 2> /dev/null; }

# the image in place $1 (a directory), its sequence number, and the places by age (the newest first)
img_in() { for E in xz gz; do [ -f $1/rdofradio.$E ] && { echo $1/rdofradio.$E; return 0; }; done; return 1; }
seq_of() { N=$(tr -cd 0-9 2> /dev/null < $1/seq); echo ${N:-0}; }
places() {
    if sd_ok && img_in $S/rdof > /dev/null && [ $(seq_of $S/rdof) -gt $(seq_of $D) ]; then echo $S/rdof $D
    elif sd_ok; then echo $D $S/rdof
    else echo $D
    fi
}
newest_seq() { A=$(seq_of $D); B=0; sd_ok && B=$(seq_of $S/rdof); [ $A -gt $B ] && echo $A || echo $B; }
# room for image $1 in the flash, the image there freed (the flash's own estimate, a little to spare)
fits() {
    O=0
    F=$(img_in $D) && O=$(($(wc -c < $F) / 1024))
    R=${RDOF_RUN_ROOM_KB:-$(df -k $D | awk 'NR == 2 {print $4}')}
    [ $(($(wc -c < $1) / 1024 + 16)) -le $((R + O)) ]
}
# put image $1 in place $2 with sequence number $3. The flash has no room for two: the one there goes first
put() {
    E=${1##*.}
    [ $2 = $D ] && rm -f $D/rdofradio.gz $D/rdofradio.xz
    if [ $2 != $D ]; then sd_rw || return 1; fi
    mkdir -p $2
    if cp "$1" $2/rdofradio.tmp && sync && mv $2/rdofradio.tmp $2/rdofradio.$E; then
        rm -f $2/rdofradio.$([ $E = xz ] && echo gz || echo xz)
        echo $3 > $2/seq.tmp && mv $2/seq.tmp $2/seq
        R=0
    else
        rm -f $2/rdofradio.tmp $2/rdofradio.$E
        R=1
    fi
    if [ $2 = $D ]; then sync; else sd_ro; fi
    return $R
}

# no image in place $1
drop() {
    if [ $1 = $D ]; then rm -f $D/rdofradio.gz $D/rdofradio.xz; sync
    elif sd_rw; then rm -f $1/rdofradio.gz $1/rdofradio.xz; sd_ro
    fi
}

take_update() {
    U=$T/rdofradio.ota
    [ -f $U ] || return 0
    rm -f $T/new.gz $T/new.xz $T/new
    case "$(magic $U)" in
    1f8b) N=$T/new.gz; mv $U $N ;;
    fd37) N=$T/new.xz; mv $U $N ;;
    *) N=$T/new.gz; gzip -c $U > $N; rm -f $U ;;
    esac
    if ! unpack $N > $T/new 2> /dev/null || ! chmod 755 $T/new; then
        say "update refused: not a whole xz or gzip"
    elif ! $T/new --help 2>&1 | grep -q "^usage: rdofradio"; then
        say "update refused: it doesn't run here"
    else
        Q=$(($(newest_seq) + 1))
        # the one running now, for a rollback, and the flash's own (it may be the SD card's that runs)
        P=$(cat $T/from)
        rm -f $T/prev.gz $T/prev.xz $T/flash.gz $T/flash.xz
        cp $P $T/prev.${P##*.}
        F=$(img_in $D) && cp $F $T/flash.${F##*.}
        W=
        if fits $N && put $N $D $Q; then W=$D
        else
            F=$(ls $T/flash.xz $T/flash.gz 2> /dev/null | head -n 1)
            [ -n "$F" ] && ! img_in $D > /dev/null && put $F $D $(seq_of $D)
            sd_ok && put $N $S/rdof $Q && W=$S/rdof
        fi
        rm -f $T/flash.gz $T/flash.xz
        if [ -n "$W" ]; then
            echo $W > $T/fresh
            say "updated: in $W"
        else
            say "update failed: no room in $D and no SD card partition (RDOF); the program as it was"
        fi
    fi
    rm -f $T/new.gz $T/new.xz $T/new
}

# install.sh takes the functions above (RDOF_RUN_LIB=1 . run.sh)
[ -n "${RDOF_RUN_LIB:-}" ] && return 0
mkdir -p $T
echo $$ > $T/run.pid
sd_mount
while [ ! -f $D/disabled ]; do
    take_update
    F=
    for L in $(places); do
        I=$(img_in $L) || continue
        if unpack $I > $T/rdofradio 2> /dev/null; then F=$I; break; fi
        say "$I doesn't unpack: the other one"
    done
    [ -n "$F" ] && chmod 755 $T/rdofradio && echo $F > $T/from || exit 1
    [ -f $T/log ] && [ "$(wc -c < $T/log)" -gt 1000000 ] && mv $T/log $T/log.1
    say start
    t0=$(up)
    $T/rdofradio --iio local: --allow-tx --config $D/config.json --last-exit $T/last_exit --ota $T/rdofradio.ota \
        $(cat $D/flags) >> $T/log 2>&1
    rc=$?
    say "exit $rc"
    if [ -f $T/fresh ]; then
        W=$(cat $T/fresh)
        rm -f $T/fresh
        P=$(ls $T/prev.xz $T/prev.gz 2> /dev/null | head -n 1)
        if [ $rc -ne 0 ] && [ $(($(up) - t0)) -lt 30 ] && [ -n "$P" ]; then
            # where the new one went, with its number: no room there (the flash) leaves that place empty, and the
            # other place's image runs: the one before it, which is still there
            put $P $W $(seq_of $W) || drop $W
            say "the update ended in error at once: the program before it put back"
        fi
    fi
    if [ $rc -eq 0 ] && [ -f $T/rdofradio.ota ]; then
        sleep 1 # stopped for an update: not a crash
        continue
    fi
    { echo "rc=$rc"; echo "at=$(date +%s)"; echo "---"; tail -n 60 $T/log; } > $T/last_exit.tmp && mv $T/last_exit.tmp $T/last_exit
    sleep ${RDOF_RUN_PAUSE:-5}
done
