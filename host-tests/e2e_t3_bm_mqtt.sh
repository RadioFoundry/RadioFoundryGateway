#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# No RF: two one-carrier Tier III sites on the MQTT network (a broker on this host) that are each logged in to fakebm
# too (tier3.md 5.11). A group call from site A comes to B both ways and plays once. With no private-call router on
# the MQTT network nothing answers a private call there, so after its wait the call goes through the master: to an
# MS at B (announced to the master when it registered) and to the parrot (9990), which plays it back. Each site's
# rules keep TG 2 on the MQTT network: B's MS registers to it and A calls it, and neither site keys it on the master
# or sends it there.
#
#   sh host-tests/e2e_t3_bm_mqtt.sh <dir with rdofradio and fakebm> <mosquitto> <broker port> <fakebm port>
set -u
BIN=$1 BROKER=$2 MPORT=$3 PORT=$4
D=$(mktemp -d "${TMPDIR:-/tmp}/rdof-t3bmq.XXXXXX")
FB= MQ=
trap 'kill $FB $MQ 2>/dev/null; wait 2>/dev/null; rm -rf "$D"' EXIT
printf 'listener %s 127.0.0.1\nallow_anonymous true\npersistence false\n' "$MPORT" > "$D/broker.conf"
"$BROKER" -c "$D/broker.conf" > "$D/broker.log" 2>&1 &
MQ=$!
"$BIN/fakebm" --port $PORT --password passw0rd > "$D/fakebm" 2>&1 &
FB=$!
site() { # name, site number, peer ID, out, in
    cat > "$D/site-$1.json" <<J
{"version": 3, "name": "t3$1", "id": 99901${2}1, "callsign": "N0CALL", "defaults": {"cc": 2},
 "devices": [{"name": "sdr", "kind": "sdr", "driver": "iio", "match": {"uri": "local:"}, "tx": {}, "transmits": ["OUT"]}],
 "frequencies": [{"name": "OUT", "freq": $4}, {"name": "IN", "freq": $5, "tx": false}],
 "apps": [{"name": "SITE", "type": "tier3", "system": {"model": "small", "net": 5, "site": $2, "par": "ab"},
           "carriers": [{"out": "OUT", "in": "IN", "chan": $2, "tscc": true}],
           "registration": "open", "wide_tg": "1-99999", "timers": {"hang_s": 2}}],
 "networks": [{"type": "brandmeister", "host": "127.0.0.1", "port": $PORT, "password": "passw0rd",
               "peer_ids": {"SITE": $3}},
              {"type": "mqtt", "host": "127.0.0.1", "port": $MPORT}],
 "rules": [{"name": "mqtt-only", "tg": 2, "nets": ["mqtt"]}, {"name": "local", "tg": 9, "nets": []}]}
J
}
site a 1 999012101 438.25 433.25
site b 2 999012201 438.75 433.75
printf '8 1000001 group 91 2000\n16 1000002 private 2000002 1500\n24 1000001 private 9990 2000\n34 1000001 group 2 1500\n' \
    > "$D/script-a"
printf '20 2000001 register 2\n' > "$D/script-b"
sleep 0.3
for s in a b; do
    [ $s = a ] && id=1000001 || id=2000001
    "$BIN/rdofradio" --file /dev/zero,/dev/null --allow-tx --config "$D/site-$s.json" --air-ms 2 --sim-id $id \
        --sim-tg 91 --sim-script "$D/script-$s" --seconds 48 > "$D/$s" 2>&1 &
done
wait %3 %4 2>/dev/null
fail=0
check() { # file, pattern, what
    grep -q "$2" "$D/$1" || { echo "FAIL: $3"; fail=1; }
}
check a "brandmeister: logged in" "A logs in to the master"
check a "mqtt: connected\|network up" "A is on the MQTT network"
check fakebm "relay TG 91 from peer 999012101 s1 to peer 999012201 s1" "A's group call goes through the master too"
check b "air MS 2000001: payload, registered on ch 1; .* heard 1000001 -> 91$" "B's MS hears A's group call"
check b "brandmeister: 1000001 -> TG 91: playing from" "B plays one copy of it"
check b "brandmeister: 2000002 registered here: announced" "B announces its MS to the master"
check a "1000002 -> 2000002: not at a site here, called through the master" "the call to B goes to the master"
check fakebm "relay PC 1000002 -> 2000002 from peer 999012101 s[12] to peer 999012201 s[12]" "the master routes it to B"
check b "air MS 2000002: payload, registered on ch 1; .* heard 1000002 -> 2000002$" "B's MS takes the private call"
check a "1000001 -> 9990: not at a site here, called through the master" "the parrot's call goes to the master"
check a "air MS 1000001: payload, registered on ch 1; .* heard 9990 -> 1000001$" "A's MS hears the parrot"
check a "air MS 1000001: .*calls 3 done.* heard 1000001 -> 2$" "A's call on TG 2 goes through"
grep -q "dynamic: TG 2 on\|DMRD peer [0-9]* [0-9]* -> 2 TG" "$D/fakebm" &&
    { echo "FAIL: TG 2 reaches the master"; fail=1; }
[ $fail = 0 ] && echo "brandmeister and mqtt tier3: all passed" || {
    for f in fakebm a b; do echo "== $f"; grep -v "samples lost\| voice sid" "$D/$f" | head -80; done
    exit 1
}
