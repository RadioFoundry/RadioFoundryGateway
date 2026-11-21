#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# No RF: emergency pre-emption (361-4 6.4.14.3.1) on a one-carrier Tier III site with no composite TSCC, so TS2 is
# its only payload channel. Three simulated MSs (--air-ms): 1000001 talks to talkgroup 1 for 12 s; 1000002,
# listening to it, makes an emergency call to talkgroup 2 (it leaves the call for the TSCC, as a radio's emergency key
# does). The site sends the RC cease to 1000001 in the TSCC's slot, 1000001 stops
# (sooner than the site's 3 s give-up), the site clears its call and grants the emergency call on TS2.
#
#   sh host-tests/e2e_t3_preempt.sh <dir with rdofradio>
set -u
BIN=$1
D=$(mktemp -d "${TMPDIR:-/tmp}/rdof-t3pre.XXXXXX")
trap 'rm -rf "$D"' EXIT
cat > "$D/site.json" <<'J'
{"version": 3, "name": "t3pre", "id": 9990121, "callsign": "N0CALL", "defaults": {"cc": 2},
 "devices": [{"name": "sdr", "kind": "sdr", "driver": "iio", "match": {"uri": "local:"}, "tx": {}, "transmits": ["OUT"]}],
 "frequencies": [{"name": "OUT", "freq": 438.25}, {"name": "IN", "freq": 433.25, "tx": false}],
 "apps": [{"name": "SITE", "type": "tier3", "system": {"model": "small", "net": 5, "site": 1, "par": "ab"},
           "carriers": [{"out": "OUT", "in": "IN", "chan": 1, "tscc": true}],
           "registration": "open", "wide_tg": "1-99999", "timers": {"hang_s": 2},
           "services": {"emergency_preempt": true}}]}
J
printf '6 1000001 group 1 12000\n10 1000002 egroup 2 1500\n' > "$D/script"
"$BIN/rdofradio" --file /dev/zero,/dev/null -r 288000 --allow-tx --config "$D/site.json" --air-ms 3 --sim-tg 1 \
    --sim-script "$D/script" --seconds 22 -v > "$D/log" 2>&1
fail=0
check() {
    grep -q "$1" "$D/log" || { echo "FAIL: $2"; fail=1; }
}
check "pre-empting 1000001 -> 1 on channel 1: an emergency waits" "the site pre-empts 1000001's call"
# 1000001 ceases on the RC: the emergency call has TS2 within 2 s of the pre-emption, before the site's 3 s give-up
awk '/pre-empting/ { t = s } /^status / { s = $2 + 0 } /heard 1000002 -> 2$/ && t != "" && !d { d = 1; exit !(s - t <= 2) }
     END { if (!d) exit 1 }' "$D/log" || { echo "FAIL: 1000001 ceases before the give-up"; fail=1; }
check "air MS 1000002: payload, registered on ch 1; .* heard 1000002 -> 2" "1000002's emergency call gets TS2"
check "air MS 1000002: idle, registered on ch 0; calls 1 done" "1000002's emergency call ends done"
[ $fail = 0 ] && echo "pre-emption: all passed" || { grep "air MS\|tier3\|pre-empt" "$D/log" | head -60; exit 1; }
