#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# rdofradio on a PlutoSDR: started at boot by /mnt/jffs2/autorun.sh, started again 5 s after it exits.
# Settings: /mnt/jffs2/rdof/config.json (the web page edits it). The device's flags: /mnt/jffs2/rdof/flags.
# touch /mnt/jffs2/rdof/disabled to keep it from starting. Log: /tmp/rdof/log (RAM; the last 1 MB and log.1).
D=/mnt/jffs2/rdof
T=/tmp/rdof
mkdir -p $T
gunzip -c $D/rdofradio.gz > $T/rdofradio && chmod 755 $T/rdofradio || exit 1
echo $$ > $T/run.pid
while [ ! -f $D/disabled ]; do
    [ -f $T/log ] && [ "$(wc -c < $T/log)" -gt 1000000 ] && mv $T/log $T/log.1
    echo "== start $(date)" >> $T/log
    $T/rdofradio --iio local: --allow-tx --config $D/config.json $(cat $D/flags) >> $T/log 2>&1
    echo "== exit $? $(date)" >> $T/log
    sleep 5
done
