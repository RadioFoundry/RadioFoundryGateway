#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# rdofradio on a PlutoSDR: started at boot by /mnt/jffs2/autorun.sh, started again 5 s after it exits.
# Settings: /mnt/jffs2/rdof/config.json (the web page edits it). The device's flags: /mnt/jffs2/rdof/flags.
# touch /mnt/jffs2/rdof/disabled to keep it from starting. Log: /tmp/rdof/log (RAM; the last 1 MB and log.1).
# Each time it ends on its own (a crash, an error), /tmp/rdof/last_exit records how (rc, 128 + the signal) and the
# log's last lines: the web page's crash card.
# The web page's update (POST /api/ota) leaves the new program in /tmp/rdof/rdofradio.ota (gzipped, or an ELF) and
# rdofradio stops: it's checked, put in place of rdofradio.gz and started. The flash has no room for two (about
# 900 KB in all), so the one it replaces stays in RAM until the next boot: a new one that ends in error within 30 s
# of its first start is put back.
D=/mnt/jffs2/rdof
T=/tmp/rdof
mkdir -p $T
echo $$ > $T/run.pid
say() { echo "== $* $(date)" >> $T/log; }
up() { cut -d. -f1 /proc/uptime; }

take_update() {
    U=$T/rdofradio.ota
    [ -f $U ] || return 0
    rm -f $T/new.gz $T/new
    if [ "$(head -c 2 $U | od -An -tx1 | tr -d ' ')" = "1f8b" ]; then
        mv $U $T/new.gz
    else
        gzip -c $U > $T/new.gz; rm -f $U
    fi
    if ! gunzip -c $T/new.gz > $T/new 2> /dev/null || ! chmod 755 $T/new; then
        say "update refused: not a whole gzip"
    elif ! $T/new --help 2>&1 | grep -q "^usage: rdofradio"; then
        say "update refused: it doesn't run here"
    else
        cp $D/rdofradio.gz $T/prev.gz && rm -f $D/rdofradio.gz
        if cp $T/new.gz $D/rdofradio.gz.tmp && mv $D/rdofradio.gz.tmp $D/rdofradio.gz; then
            sync
            touch $T/fresh
            say "updated"
        else
            rm -f $D/rdofradio.gz.tmp
            cp $T/prev.gz $D/rdofradio.gz && sync
            say "update failed: no room in $D; the program as it was"
        fi
    fi
    rm -f $T/new.gz $T/new
}

while [ ! -f $D/disabled ]; do
    take_update
    gunzip -c $D/rdofradio.gz > $T/rdofradio && chmod 755 $T/rdofradio || exit 1
    [ -f $T/log ] && [ "$(wc -c < $T/log)" -gt 1000000 ] && mv $T/log $T/log.1
    say start
    t0=$(up)
    $T/rdofradio --iio local: --allow-tx --config $D/config.json --last-exit $T/last_exit --ota $T/rdofradio.ota \
        $(cat $D/flags) >> $T/log 2>&1
    rc=$?
    say "exit $rc"
    if [ -f $T/fresh ]; then
        rm -f $T/fresh
        if [ $rc -ne 0 ] && [ $(($(up) - t0)) -lt 30 ] && [ -f $T/prev.gz ]; then
            rm -f $D/rdofradio.gz # no room for both
            cp $T/prev.gz $D/rdofradio.gz && sync
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
