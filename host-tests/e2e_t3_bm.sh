#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# No RF: two one-carrier Tier III sites on file devices, each with two simulated MSs (--air-ms) and its own login to
# fakebm (a simulated BrandMeister master), no MQTT network (tier3.md 5.11). The sites key TG 91 when their MSs
# register to it, then: a group call from site A heard at site B, a private call to the parrot (9990) played back,
# a call from B heard at A, a private call from an MS at A to one at B. Then A's MSs move to TG 92 (pinned to slot 2):
# A keys it there and, unlink_s after the last call on 91, keys TG 4000 on slot 1; B's next call on 91 doesn't come
# to A. A key-up is never played to the other site's MSs.
#
#   sh host-tests/e2e_t3_bm.sh <dir with rdofradio and fakebm> [fakebm's port]
set -u
BIN=$1 PORT=${2:-47015}
D=$(mktemp -d "${TMPDIR:-/tmp}/rdof-t3bm.XXXXXX")
trap 'kill $FB 2>/dev/null; rm -rf "$D"' EXIT
site() { # name, site number, peer ID, out, in, extra network keys
    cat > "$D/site-$1.json" <<J
{"version": 3, "name": "t3$1", "id": 99901${2}1, "callsign": "N0CALL", "defaults": {"cc": 2},
 "devices": [{"name": "sdr", "kind": "sdr", "driver": "iio", "match": {"uri": "local:"}, "tx": {}, "transmits": ["OUT"]}],
 "frequencies": [{"name": "OUT", "freq": $4}, {"name": "IN", "freq": $5, "tx": false}],
 "apps": [{"name": "SITE", "type": "tier3", "system": {"model": "small", "net": 5, "site": $2, "par": "ab"},
           "carriers": [{"out": "OUT", "in": "IN", "chan": $2, "tscc": true}],
           "registration": "open", "wide_tg": "1-99999", "timers": {"hang_s": 2}}],
 "networks": [{"type": "brandmeister", "host": "127.0.0.1", "port": $PORT, "password": "passw0rd",
               "peer_ids": {"SITE": $3}$6}]}
J
}
site a 1 999012101 438.25 433.25 ', "unlink_s": 5, "ts2_tg": [92]'
site b 2 999012201 438.75 433.75 ''
printf '8 1000001 group 91 2000\n14 1000001 private 9990 2000\n30 1000002 private 2000002 1500\n36 1000001 register 92\n37 1000002 register 92\n' \
    > "$D/script-a"
printf '25 2000002 group 91 1000\n50 2000001 group 91 1000\n' > "$D/script-b"
"$BIN/fakebm" --port $PORT --password passw0rd > "$D/fakebm" 2>&1 &
FB=$!
sleep 0.3
for s in a b; do
    [ $s = a ] && id=1000001 || id=2000001
    "$BIN/rdofradio" --file /dev/zero,/dev/null --allow-tx --config "$D/site-$s.json" --air-ms 2 --sim-id $id \
        --sim-tg 91 --sim-script "$D/script-$s" --seconds 58 > "$D/$s" 2>&1 &
done
wait %2 %3 2>/dev/null || wait
kill $FB 2>/dev/null
wait $FB 2>/dev/null
fail=0
check() { # file, pattern, what
    grep -q "$2" "$D/$1" || { echo "FAIL: $3"; fail=1; }
}
none() {
    ! grep -q "$2" "$D/$1" || { echo "FAIL: $3"; fail=1; }
}
check a "brandmeister: logged in" "A logs in"
check b "brandmeister: logged in" "B logs in"
check fakebm "dynamic: TG 91 on peer 999012101 s1" "A keys TG 91 on slot 1"
check fakebm "dynamic: TG 91 on peer 999012201 s1" "B keys TG 91 on slot 1"
check b "brandmeister: 2000002 registered here: announced on slot" "B announces its MS to the master"
check fakebm "DMRD peer 999012201 2000002 -> 9 TG" "the announcement is a key-up on TG 9"
none fakebm "relay TG 9 " "TG 9 goes nowhere"
none b "heard 9990121 -> 91" "B's MSs don't play A's key-up"
none a "heard 9990122 -> 91" "A's MSs don't play B's key-up"
check b "air MS 2000001: payload, registered on ch 1; .* heard 1000001 -> 91$" "B's MS hears A's group call"
check b "air MS 2000002: payload, registered on ch 1; .* heard 1000001 -> 91$" "B's other MS hears it"
check fakebm "parrot: back to peer 999012101: 9990 -> 1000001 PC" "the parrot plays A's call back"
check a "air MS 1000001: payload, registered on ch 1; .* heard 9990 -> 1000001$" "A's MS hears the parrot"
check a "air MS 1000001: payload, registered on ch 1; .* heard 2000002 -> 91$" "A's MS hears B's group call"
check fakebm "relay PC 1000002 -> 2000002 from peer 999012101 s[12] to peer 999012201 s1" "the private call goes to B"
check b "air MS 2000002: payload, registered on ch 1; .* heard 1000002 -> 2000002$" "B's MS takes the private call"
check a "air MS 1000002: idle, registered on ch 0; calls 1 done" "A's private call ends done"
check a "brandmeister: keying TG 92 on slot 2" "A keys TG 92 on its pinned slot"
check fakebm "dynamic: TG 92 on peer 999012101 s2" "the master has TG 92 on A's slot 2"
check a "brandmeister: TG 4000 on slot 1" "A unlinks slot 1 once no MS wants TG 91"
check fakebm "unlink: peer 999012101 s1: 1 dynamic TGs dropped" "the master drops TG 91 from A"
check fakebm "DMRD peer 999012201 2000001 -> 91" "B calls TG 91 after the unlink"
sed -n '/unlink: peer 999012101/,$p' "$D/fakebm" | grep -q "relay TG 91 from peer 999012201 s1 to peer 999012101" &&
    { echo "FAIL: TG 91 still goes to A after the unlink"; fail=1; }
none a "both slots busy" "no call waits for a slot"
[ $fail = 0 ] && echo "brandmeister tier3: all passed" || {
    for f in fakebm a b; do echo "== $f"; grep -v "samples lost\| voice sid" "$D/$f" | head -80; done
    exit 1
}
