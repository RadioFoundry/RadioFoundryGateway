#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# No RF: late entry on a lent composite TSCC slot (services.composite_late_entry, tier3.md 3.19). One carrier with a
# composite TSCC; three simulated MSs (--air-ms) on talkgroup 1. 1000003 is switched off before 1000001's 20 s call
# to talkgroup 1 gets TS2, so it misses its grants. 1000002 leaves that call for an emergency call to talkgroup 2,
# which takes the TSCC's slot. 1000003, switched on meanwhile, waits on that slot; in the emergency call's hang time
# the site sends a Late_Entry P_GRANT for talkgroup 1 there, and 1000003 follows it to TS2 before the TSCC is back.
# Without the setting, it joins only once the TSCC is back.
#
#   sh host-tests/e2e_t3_late.sh <dir with rdofradio>
set -u
BIN=$1
D=$(mktemp -d "${TMPDIR:-/tmp}/rdof-t3late.XXXXXX")
trap 'rm -rf "$D"' EXIT
site() {
    cat <<J
{"version": 3, "name": "t3late", "id": 9990122, "callsign": "N0CALL", "defaults": {"cc": 2},
 "devices": [{"name": "sdr", "kind": "sdr", "driver": "iio", "match": {"uri": "local:"}, "tx": {}, "transmits": ["OUT"]}],
 "frequencies": [{"name": "OUT", "freq": 438.25}, {"name": "IN", "freq": 433.25, "tx": false}],
 "apps": [{"name": "SITE", "type": "tier3", "system": {"model": "small", "net": 5, "site": 1, "par": "ab"},
           "carriers": [{"out": "OUT", "in": "IN", "chan": 1, "tscc": true}],
           "registration": "open", "wide_tg": "1-99999", "timers": {"hang_s": 3},
           "services": {"composite_tscc": true, "composite_late_entry": $1}}]}
J
}
printf '5 1000003 off\n6 1000001 group 1 20000\n10 1000002 egroup 2 1500\n11 1000003 on\n' > "$D/script"
run() {
    site "$1" > "$D/site.json"
    "$BIN/rdofradio" --file /dev/zero,/dev/null --allow-tx --config "$D/site.json" --air-ms 3 --sim-tg 1 \
        --sim-script "$D/script" --seconds 22 -v > "$D/log.$1" 2>&1
}
run true
run false
fail=0
# 1 if 1000003 heard 1000001 on TS2 while the TSCC's slot was lent, 0 if only after it came back
early() {
    awk '/composite TSCC: the control slot carries a call/ { lent = 1 }
         /composite TSCC: the control channel again/ { lent = 0 }
         /air MS 1000003: payload, registered on ch 1; .* heard 1000001 -> 1$/ { print lent; exit }' "$D/log.$1"
}
grep -q "air MS 1000003: switched on" "$D/log.true" || { echo "FAIL: 1000003 switched on"; fail=1; }
grep -q "composite TSCC: the control slot carries a call" "$D/log.true" || { echo "FAIL: the TSCC's slot lent"; fail=1; }
[ "$(early true)" = 1 ] || { echo "FAIL: 1000003 follows the P_GRANT on the lent slot"; fail=1; }
[ "$(early false)" = 0 ] || { echo "FAIL: without the setting, 1000003 joins once the TSCC is back"; fail=1; }
[ $fail = 0 ] && echo "late entry: all passed" ||
    { for f in true false; do echo "== $f"; grep "air MS\|composite" "$D/log.$f" | head -40; done; exit 1; }
