#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#
# rdofradio on a Linux box, run by rdofradio.service (restart: always, 5 s apart).
# Program: ~/rfg/bin/rdofradio; the one before an update stays as rdofradio.prev. Settings: ~/rfg/config.json (the web
# page edits it). Flags: ~/rfg/flags (the device's and the bench's; one line). touch ~/rfg/disabled to keep it from
# starting. Log: the journal (journalctl -u rdofradio); the SD is written only by the state files below.
# An update (POST /api/ota on the web page) leaves the new program in state/rdofradio.ota (ELF or gzip) and rdofradio
# exits 0: it's checked here, the old one kept as .prev. A program that ends in error within 30 s, 3 starts in a row,
# is put back to .prev (the failed one kept as rdofradio.bad).
D=$(cd "$(dirname "$0")" && pwd)
B=$D/bin
S=$D/state
mkdir -p "$S"
say() { echo "run.sh: $*"; }
up() { cut -d. -f1 /proc/uptime; }

if [ -f "$D/disabled" ]; then say "disabled (rm $D/disabled)"; sleep 3600; exit 0; fi

U=$S/rdofradio.ota
if [ -f "$U" ]; then
    if [ "$(head -c 2 "$U" | od -An -tx1 | tr -d ' ')" = "1f8b" ]; then gunzip -c "$U" > "$S/new" 2> /dev/null; else cp "$U" "$S/new"; fi
    rm -f "$U"
    chmod 755 "$S/new"
    if "$S/new" --help 2>&1 | grep -q "^usage: rdofradio"; then
        [ -f "$B/rdofradio" ] && cp "$B/rdofradio" "$B/rdofradio.prev"
        mv "$S/new" "$B/rdofradio"
        echo 0 > "$S/fails"
        say "updated"
    else
        rm -f "$S/new"
        say "update refused: it doesn't run here"
    fi
fi

# the flags: one line in ~/rfg/flags
FLAGS=$(grep -v '^#' "$D/flags" | tr '\n' ' ')
t0=$(up)
# shellcheck disable=SC2086
"$B/rdofradio" --config "$D/config.json" --last-exit "$S/last_exit" --ota "$U" $FLAGS
rc=$?
say "exit $rc after $(($(up) - t0)) s"

f=$(cat "$S/fails" 2> /dev/null || echo 0)
if [ $rc -ne 0 ] && [ $(($(up) - t0)) -lt 30 ]; then f=$((f + 1)); else f=0; fi
if [ $f -ge 3 ] && [ -f "$B/rdofradio.prev" ]; then
    mv "$B/rdofradio" "$B/rdofradio.bad"
    cp "$B/rdofradio.prev" "$B/rdofradio"
    say "3 quick failures: the program before it put back (the failed one is rdofradio.bad)"
    f=0
fi
echo $f > "$S/fails"
exit $rc
