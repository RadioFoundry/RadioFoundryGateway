#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# No RF, on one host: two rdofradio boxes, each on a simulated hotspot modem (fakemodem), joined by the MQTT network
# (format v3) through a broker on this host. A radio's group call heard by box A goes out on air at box B, and not
# back out at A.
#
#   sh host-tests/e2e_mqttnet.sh <dir with fakemodem, rdofradio> <mosquitto> <port>
set -u
BIN=$1 BROKER=$2 PORT=$3
D=$(mktemp -d "${TMPDIR:-/tmp}/rdof-e2e.XXXXXX")
trap 'kill $RA $RB $FA $FB $MQ 2>/dev/null; wait 2>/dev/null; rm -rf "$D"' EXIT
RA= RB= FA= FB= MQ=
printf 'listener %s 127.0.0.1\nallow_anonymous true\npersistence false\n' "$PORT" > "$D/broker.conf"
"$BROKER" -c "$D/broker.conf" > "$D/broker.log" 2>&1 &
MQ=$!
# A hears the call; B hears nothing and transmits what the network brings
"$BIN/fakemodem" --cc 1 --src 3100123 --delay-s 4 --call 91:group:1 --path-file "$D/ptya" --exit-after-s 6 \
    --log "$D/modem-a.log" >/dev/null 2>&1 &
FA=$!
"$BIN/fakemodem" --cc 1 --src 3100124 --delay-s 600 --path-file "$D/ptyb" \
    --log "$D/modem-b.log" >/dev/null 2>&1 &
FB=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -s "$D/ptya" ] && [ -s "$D/ptyb" ] && break; sleep 0.3; done
[ -s "$D/ptya" ] && [ -s "$D/ptyb" ] || { echo "FAIL: fakemodem gave no pty"; exit 1; }
for b in a b; do
    cat > "$D/cfg-$b.json" <<EOF
{"version": 3, "name": "box$b", "id": 310012$( [ $b = a ] && echo 1 || echo 2 ), "callsign": "N0CALL", "defaults": {"cc": 1},
 "devices": [{"name": "hs", "kind": "modem", "type": "mmdvm_hs", "port": {"path": "$(cat "$D/pty$b")"}}],
 "frequencies": [{"name": "f1", "freq": 438.5, "ts": 2, "tg": [91]}],
 "networks": [{"type": "mqtt", "host": "127.0.0.1", "port": $PORT, "calls": true, "format": "v3"}]}
EOF
done
"$BIN/rdofradio" --allow-tx --mmdvm "$(cat "$D/ptya")" --config "$D/cfg-a.json" > "$D/radio-a.out" 2>&1 &
RA=$!
"$BIN/rdofradio" --allow-tx --mmdvm "$(cat "$D/ptyb")" --config "$D/cfg-b.json" > "$D/radio-b.out" 2>&1 &
RB=$!
wait $FA # A plays its call and waits out the echo's time; B has nothing to play and is stopped with the rest
kill $RA $RB $FB 2>/dev/null
wait $RA $RB $FB 2>/dev/null
kill $MQ 2>/dev/null
wait $MQ 2>/dev/null
fail=0
need() { grep -q "$2" "$1" || { echo "FAIL: no '$2' in $(basename "$1")"; fail=1; }; }
never() { grep -q "$2" "$1" && { echo "FAIL: '$2' in $(basename "$1")"; fail=1; }; }
need "$D/modem-a.log" 'RX s2 VLC'
need "$D/modem-b.log" 'TX s2 VLC 3100123 -> 91 TG'
need "$D/modem-b.log" 'TX s2 TLC 3100123 -> 91 TG'
never "$D/modem-a.log" 'TX s2 VLC'
if [ $fail = 1 ]; then
    for f in radio-a.out radio-b.out modem-a.log modem-b.log broker.log; do echo "--- $f"; tail -25 "$D/$f"; done
    exit 1
fi
echo "ok: a group call heard at box A went out on air at box B over the MQTT network"
