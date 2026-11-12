#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# rdofradio on a PlutoSDR: started at boot by /mnt/jffs2/autorun.sh, started again 5 s after it exits.
# Settings: /mnt/jffs2/rdof/config.json (the web page edits it). The device's flags: /mnt/jffs2/rdof/flags.
# touch /mnt/jffs2/rdof/disabled to keep it from starting. Log: /tmp/rdof/log (RAM; the last 1 MB and log.1).
# Each time it ends on its own (a crash, an error), /tmp/rdof/last_exit records how (rc, 128 + the signal) and the
# log's last lines: the web page's crash card.
# The program is kept in flash as rdofradio.xz (xz, the bundle's: about 15 % smaller than gzip) or rdofradio.gz (an
# install from before the bundle used xz, or an update sent gzipped or as an ELF).
# The web page's update (POST /api/ota) leaves the new program in /tmp/rdof/rdofradio.ota (xz, gzip, or an ELF) and
# rdofradio stops: it's checked, put in place of the one in flash and started. The flash has no room for two (about
# 900 KB in all), so the one it replaces stays in RAM until the next boot: a new one that ends in error within 30 s
# of its first start is put back.
D=/mnt/jffs2/rdof
T=/tmp/rdof
mkdir -p $T
echo $$ > $T/run.pid
say() { echo "== $* $(date)" >> $T/log; }
up() { cut -d. -f1 /proc/uptime; }
magic() { head -c 2 "$1" | od -An -tx1 | tr -d ' '; }
# the image in flash, unpacked to stdout
img() { if [ -f $D/rdofradio.xz ]; then echo $D/rdofradio.xz; else echo $D/rdofradio.gz; fi; }
unpack() {
    case "$(magic "$1")" in
    1f8b) gunzip -c "$1" ;;
    fd37) unxz -c "$1" ;;
    *) return 1 ;;
    esac
}
# put image $1 (.gz or .xz) in flash in place of the one there
place() { rm -f $D/rdofradio.gz $D/rdofradio.xz && cp "$1" $D/rdofradio.${1##*.} && sync; }

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
        P=$(img)
        rm -f $T/prev.gz $T/prev.xz
        cp $P $T/prev.${P##*.} && rm -f $D/rdofradio.gz $D/rdofradio.xz
        if cp $N $D/rdofradio.tmp && mv $D/rdofradio.tmp $D/rdofradio.${N##*.}; then
            sync
            touch $T/fresh
            say "updated"
        else
            rm -f $D/rdofradio.tmp
            place $T/prev.${P##*.}
            say "update failed: no room in $D; the program as it was"
        fi
    fi
    rm -f $T/new.gz $T/new.xz $T/new
}

while [ ! -f $D/disabled ]; do
    take_update
    unpack $(img) > $T/rdofradio && chmod 755 $T/rdofradio || exit 1
    [ -f $T/log ] && [ "$(wc -c < $T/log)" -gt 1000000 ] && mv $T/log $T/log.1
    say start
    t0=$(up)
    $T/rdofradio --iio local: --allow-tx --config $D/config.json --last-exit $T/last_exit --ota $T/rdofradio.ota \
        $(cat $D/flags) >> $T/log 2>&1
    rc=$?
    say "exit $rc"
    if [ -f $T/fresh ]; then
        rm -f $T/fresh
        P=$(ls $T/prev.xz $T/prev.gz 2> /dev/null | head -n 1)
        if [ $rc -ne 0 ] && [ $(($(up) - t0)) -lt 30 ] && [ -n "$P" ]; then
            place $P # no room for both: the new one goes first
            say "the update ended in error at once: the program before it put back"
        fi
    fi
    if [ $rc -eq 0 ] && [ -f $T/rdofradio.ota ]; then
        sleep 1 # stopped for an update: not a crash
        continue
    fi
    { echo "rc=$rc"; echo "at=$(date +%s)"; echo "---"; tail -n 60 $T/log; } > $T/last_exit.tmp && mv $T/last_exit.tmp $T/last_exit
    sleep 5
done
