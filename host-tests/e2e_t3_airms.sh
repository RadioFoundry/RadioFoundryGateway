#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# No RF: a one-carrier Tier III site on a file device (receives zeros, sends to /dev/null) with two simulated MSs
# beside the air (--air-ms): both register, one calls the talkgroup and the other follows, then an individual call
# with the availability check.
#
#   sh host-tests/e2e_t3_airms.sh <dir with rdofradio>
set -u
BIN=$1
D=$(mktemp -d "${TMPDIR:-/tmp}/rdof-t3air.XXXXXX")
trap 'rm -rf "$D"' EXIT
cat > "$D/site.json" <<'J'
{"version": 3, "name": "t3air", "id": 9990121, "callsign": "N0CALL", "defaults": {"cc": 2},
 "devices": [{"name": "sdr", "kind": "sdr", "driver": "iio", "match": {"uri": "local:"}, "tx": {}, "transmits": ["OUT"]}],
 "frequencies": [{"name": "OUT", "freq": 438.25}, {"name": "IN", "freq": 433.25, "tx": false}],
 "apps": [{"name": "SITE", "type": "tier3", "system": {"model": "small", "net": 5, "site": 1, "par": "ab"},
           "carriers": [{"out": "OUT", "in": "IN", "chan": 1, "tscc": true}],
           "registration": "open", "wide_tg": "1-99999", "timers": {"hang_s": 2}}]}
J
printf '6 1000001 group 1 1500\n10 1000002 private 1000001 1500\n' > "$D/script"
"$BIN/rdofradio" --file /dev/zero,/dev/null --allow-tx --config "$D/site.json" --air-ms 2 --sim-tg 1 \
    --sim-script "$D/script" --seconds 18 > "$D/log" 2>&1
fail=0
check() {
    grep -q "$1" "$D/log" || { echo "FAIL: $2"; fail=1; }
}
check "air MS 1000001: idle, registered" "1000001 registers"
check "air MS 1000002: idle, registered" "1000002 registers"
check "air MS 1000002: payload, registered on ch 1; .* heard 1000001 -> 1$" "1000002 follows 1000001's group call"
check "air MS 1000001: idle, registered on ch 0; calls 1 done" "1000001's group call ends done"
check "air MS 1000002: idle, registered on ch 0; calls 1 done" "1000002's individual call ends done"
check "air MS 1000001: payload, registered on ch 1; .* heard 1000002 -> 1000001" "1000001 takes the individual call"
[ $fail = 0 ] && echo "air MSs: all passed" || { grep "air MS\|tier3" "$D/log" | head -40; exit 1; }
