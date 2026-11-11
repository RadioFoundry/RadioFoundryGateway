#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# No RF, on one host: rdofradio on a simulated hotspot modem (fakemodem) logs in to a simulated Homebrew master
# (fakebm), sends a radio's private call to the parrot up, and puts the echo back on air.
#
#   sh host-tests/e2e_login.sh <dir with fakebm, fakemodem, rdofradio> <brandmeister|homebrew> <port>
set -u
BIN=$1 TYPE=$2 PORT=$3
D=$(mktemp -d "${TMPDIR:-/tmp}/rdof-e2e.XXXXXX")
trap 'kill $RR $FM $BM 2>/dev/null; wait 2>/dev/null; rm -rf "$D"' EXIT # wait: the port is free for the next run
RR= FM= BM=
"$BIN/fakebm" --port "$PORT" --password pw1 --ids 310012301 --log "$D/bm.log" >/dev/null 2>&1 &
BM=$!
"$BIN/fakemodem" --cc 1 --src 3100123 --delay-s 2 --call 9990:private:1 --path-file "$D/pty" --exit-after-s 7 \
    --log "$D/modem.log" >/dev/null 2>&1 &
FM=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -s "$D/pty" ] && break; sleep 0.3; done
[ -s "$D/pty" ] || { echo "FAIL: fakemodem gave no pty"; exit 1; }
cat > "$D/cfg.json" <<EOF
{"version": 3, "callsign": "N0CALL", "defaults": {"cc": 1},
 "devices": [{"name": "hs", "kind": "modem", "type": "mmdvm_hs", "port": {"path": "$(cat "$D/pty")"}}],
 "frequencies": [{"name": "f1", "freq": 438.5, "ts": 2}],
 "networks": [{"type": "$TYPE", "host": "127.0.0.1", "port": $PORT, "password": "pw1", "peer_ids": {"f1": 310012301}}]}
EOF
"$BIN/rdofradio" --allow-tx --mmdvm "$(cat "$D/pty")" --config "$D/cfg.json" > "$D/radio.out" 2>&1 &
RR=$!
wait $FM
kill $RR 2>/dev/null
wait $RR 2>/dev/null
kill $BM 2>/dev/null
wait $BM 2>/dev/null
fail=0
need() { grep -q "$2" "$1" || { echo "FAIL: no '$2' in $(basename "$1")"; fail=1; }; }
need "$D/bm.log" 'RPTK 310012301: password good'
need "$D/bm.log" 'RPTC 310012301: up: N0CALL rx 438500000 tx 438500000 cc 01 slots 4'
need "$D/bm.log" 'DMRD peer 310012301 3100123 -> 9990 PC s2 VLC'
need "$D/bm.log" 'DMRD peer 310012301 3100123 -> 9990 PC s2 TLC'
need "$D/bm.log" 'parrot: back to peer 310012301'
need "$D/bm.log" 'RPTCL 310012301'
need "$D/modem.log" 'TX s2 VLC 9990 -> 3100123 PC'
need "$D/modem.log" 'TX s2 TLC 9990 -> 3100123 PC'
if [ $fail = 1 ]; then
    echo "--- fakebm"; cat "$D/bm.log"; echo "--- rdofradio"; tail -20 "$D/radio.out"; echo "--- fakemodem"; tail -20 "$D/modem.log"
    exit 1
fi
echo "ok: $TYPE login, call up, echo on air, RPTCL"
