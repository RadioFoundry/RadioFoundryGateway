#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# No RF, on one host: a standby pair (the tier3 app's "standby", tier3.md 6.2). Two boxes with the same settings
# (--sim-air, each its own RDOF_HWID) on a broker on this host. The first goes on the air once nothing beats on the
# node; the second waits. The first is killed: its will goes out and the second is on the air within seconds. The
# first comes back as the standby. The broker hangs (SIGSTOP: to both boxes a link gone silent): the box on the air
# hears no standby beat and goes off the air (exit 75); the standby doesn't claim the node without the broker. The
# broker back, the standby goes on the air. (A broker that shuts down sends every client's will: the box on the air
# then sees its standby gone and stays on.) The registration records outlive the box: the second, on the air, holds
# the first's MS (1000001, read back from the broker) and its own (2000001).
#
#   sh host-tests/e2e_t3_standby.sh <dir with rdofradio> <mosquitto> <port>
set -u
BIN=$1 BROKER=$2 PORT=$3
D=$(mktemp -d "${TMPDIR:-/tmp}/rdof-t3sb.XXXXXX")
RA= RB= MQ=
trap 'kill -CONT $MQ 2>/dev/null; kill $RA $RB $MQ 2>/dev/null; wait 2>/dev/null; rm -rf "$D"' EXIT
printf 'listener %s 127.0.0.1\nallow_anonymous true\npersistence false\n' "$PORT" > "$D/broker.conf"
"$BROKER" -c "$D/broker.conf" > "$D/broker.log" 2>&1 &
MQ=$!
cat > "$D/site.json" <<J
{"version": 3, "name": "t3sb", "id": 9990123, "callsign": "N0CALL", "defaults": {"cc": 2},
 "devices": [{"name": "sdr", "kind": "sdr", "driver": "iio", "match": {"uri": "local:"}, "tx": {}, "transmits": ["OUT"]}],
 "frequencies": [{"name": "OUT", "freq": 438.25}, {"name": "IN", "freq": 433.25, "tx": false}],
 "networks": [{"type": "mqtt", "host": "127.0.0.1", "port": $PORT}],
 "apps": [{"name": "SITE", "type": "tier3", "system": {"model": "small", "net": 5, "site": 1, "par": "ab"},
           "carriers": [{"out": "OUT", "in": "IN", "chan": 1, "tscc": true}],
           "registration": "open", "wide_tg": "1-99999", "standby": {"takeover_s": 12, "fence_s": 9}}]}
J
box() {
    RDOF_HWID=$1 exec "$BIN/rdofradio" --config "$D/site.json" --sim-air 1 --sim-id "$2" --seconds 120 \
        >> "$D/$1.log" 2>&1
}
SUB=$(command -v mosquitto_sub || true)
# until the node's status (retained) counts $1 registration records, at most $2 s (status goes out every 10 s: a
# count can take two periods to show)
until_regs() {
    [ -n "$SUB" ] || return 0
    i=0
    while [ $i -lt $(($2 * 2)) ]; do
        r=$("$SUB" -h 127.0.0.1 -p "$PORT" -t DMR/node/t3sb/status -C 1 -W 1 2>/dev/null |
            sed -n 's/.*"regs":\([0-9]*\).*/\1/p')
        [ "$r" = "$1" ] && return 0
        sleep 0.5; i=$((i + 1))
    done
    echo "regs: ${r:-none}"; return 1
}
# until a line is in a log, at most $3 s
until_in() {
    i=0
    while [ $i -lt $(($3 * 10)) ]; do grep -q "$2" "$D/$1.log" 2>/dev/null && return 0; sleep 0.1; i=$((i + 1)); done
    return 1
}
on() { grep -c "standby: on the air" "$D/$1.log" 2>/dev/null || true; }
fail=0
# a takeover inside the fence is refused
sed 's/"takeover_s": 12/"takeover_s": 10/' "$D/site.json" > "$D/bad.json"
"$BIN/rdofradio" --config "$D/bad.json" --sim-air 1 --seconds 1 2>&1 | grep -q "takeover_s at least fence_s + 3" ||
    { echo "FAIL: takeover_s 10 with fence_s 9 refused"; fail=1; }
sleep 0.5
box aaaaa1 1000001 & RA=$!
until_in aaaaa1 "standby: on the air" 16 || { echo "FAIL: the first box on the air"; fail=1; }
until_regs 1 25 || { echo "FAIL: the first box's MS registered"; fail=1; }
box bbbbb2 2000001 & RB=$!
until_in aaaaa1 "standby: bbbbb2 is there" 5 || { echo "FAIL: the active box hears the standby"; fail=1; }
sleep 2
[ "$(on bbbbb2)" = 0 ] || { echo "FAIL: the standby waits while the first is on the air"; fail=1; }
kill -9 $RA; wait $RA 2>/dev/null
t0=$(date +%s)
until_in bbbbb2 "standby: on the air" 6 || { echo "FAIL: the standby on the air after the first's will"; fail=1; }
grep -q "the node's will: claiming" "$D/bbbbb2.log" || { echo "FAIL: the claim on the will"; fail=1; }
echo "takeover in about $(($(date +%s) - t0)) s"
until_regs 2 25 || { echo "FAIL: the second box holds the first's record and its own"; fail=1; }
box aaaaa1 1000001 & RA=$!
until_in bbbbb2 "standby: aaaaa1 is there" 5 || { echo "FAIL: the first back as the standby"; fail=1; }
kill -STOP $MQ
t0=$(date +%s)
wait $RB; rc=$?
echo "off the air in about $(($(date +%s) - t0)) s, exit $rc"
[ $rc = 75 ] || { echo "FAIL: the box on the air stops (75) without the broker"; fail=1; }
grep -q "no beat from the standby or the broker" "$D/bbbbb2.log" || { echo "FAIL: the fence's line"; fail=1; }
sleep 3
[ "$(on aaaaa1)" = 1 ] || { echo "FAIL: the standby stays off the air without the broker"; fail=1; }
kill -CONT $MQ
until [ "$(on aaaaa1)" = 2 ] || [ $(($(date +%s) - t0)) -gt 50 ]; do sleep 0.2; done
[ "$(on aaaaa1)" = 2 ] || { echo "FAIL: the standby on the air once the broker is back"; fail=1; }
[ $fail = 0 ] && echo "standby: all passed" ||
    { for b in aaaaa1 bbbbb2; do echo "== $b"; grep "standby\|mqtt" "$D/$b.log" | head -30; done; exit 1; }
