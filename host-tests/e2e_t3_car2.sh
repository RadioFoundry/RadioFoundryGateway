#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# No RF: a two-carrier Tier III site whose carriers both go out of one SDR (a file device: receives zeros, writes
# what it transmits), with four simulated MSs beside the air (--air-ms). An individual call holds carrier 1's payload
# slot, so the next call is granted on carrier 2: 1000003 calls 1000004 there, then calls its talkgroup there, and
# 1000004 follows both. Then rdofsdr decodes the transmit file at both outs: both carriers carry the site's blocks on
# the same slots, at the same sample, CC 2, carrier 2 the calls' voice, each at the same level.
#
#   sh host-tests/e2e_t3_car2.sh <dir with rdofradio and rdofsdr>
set -u
BIN=$1
D=$(mktemp -d "${TMPDIR:-/tmp}/rdof-t3car2.XXXXXX")
trap 'rm -rf "$D"' EXIT
cat > "$D/site.json" <<'J'
{"version": 3, "name": "t3car2", "id": 9990122, "callsign": "N0CALL", "defaults": {"cc": 2},
 "devices": [{"name": "sdr", "kind": "sdr", "driver": "iio", "match": {"uri": "local:"}, "tx": {"carriers": 2},
              "transmits": ["A-OUT", "B-OUT"]}],
 "frequencies": [{"name": "A-OUT", "freq": 438.75}, {"name": "A-IN", "freq": 438.25, "tx": false},
                 {"name": "B-OUT", "freq": 438.625}, {"name": "B-IN", "freq": 438.375, "tx": false}],
 "apps": [{"name": "SITE", "type": "tier3", "system": {"model": "small", "net": 5, "site": 1, "par": "ab"},
           "carriers": [{"out": "A-OUT", "in": "A-IN", "chan": 1, "tscc": true},
                        {"out": "B-OUT", "in": "B-IN", "chan": 2}],
           "registration": "open", "wide_tg": "1-99999", "timers": {"hang_s": 2}}]}
J
# --air-ms 2 registers 1000001 and 1000002; the script brings in 1000003 and 1000004
printf '4.4 1000003 register 1\n5.1 1000004 register 1\n6 1000001 private 1000002 7000\n8 1000003 private 1000004 3000\n14 1000001 private 1000002 5000\n15.5 1000003 group 1 2000\n' \
    > "$D/script"
"$BIN/rdofradio" --file /dev/zero,"$D/tx.cf32" -r 240000 --allow-tx --config "$D/site.json" --air-ms 2 --sim-tg 1 \
    --sim-script "$D/script" --seconds 21 > "$D/log" 2>&1
fail=0
check() {
    grep -q "$1" "$D/log" || { echo "FAIL: $2"; fail=1; }
}
check "carrier 2: sent by the SDR (tx 438.62500, rx 438.37500 MHz), -62.5 kHz from the TX LO 438.68750 MHz" \
    "carrier 2 on the SDR, the TX LO between the carriers"
check "carrier 1: on the slot grid" "carrier 1 on the slot grid"
check "carrier 2: on the slot grid" "carrier 2 on the slot grid"
check "air MS 1000001: payload, registered on ch 1; .* heard 1000001 -> 1000002$" "1000001's call on carrier 1"
check "air MS 1000003: payload, registered on ch [23]; .* heard 1000003 -> 1000004$" "1000003's call on carrier 2"
check "air MS 1000004: payload, registered on ch [23]; .* heard 1000003 -> 1000004$" "1000004 takes it on carrier 2"
check "air MS 1000003: idle, registered on ch 0; calls 1 done" "1000003's individual call ends done"
check "air MS 1000004: payload, registered on ch [23]; .* heard 1000003 -> 1$" "1000004 follows the group call on carrier 2"
check "air MS 1000003: idle, registered on ch 0; calls 2 done" "1000003's group call ends done"
check "carrier 2: [1-9][0-9]* bursts from the site" "carrier 2's modem sent the site's bursts"

# the transmit file, centered on the TX LO: both outs
"$BIN/rdofsdr" -i "$D/tx.cf32" -r 240000 --center 438687500 --ch A:438.75 --ch B:438.625 --bursts --quiet \
    > "$D/bursts" 2> "$D/rdofsdr.log"
# <id> burst <pos> <dt> <n> <slot> <family> <cc> <errors> <power> ...
awk '
$2 == "burst" {
    n[$1]++
    if ($4 != 240) { cc[$1]++; if ($8 == 2) ok[$1]++ }
    if ($4 == 240 || $4 == 241) voice[$1]++
    pw[$1] += $10
    if ($1 == "A") { at[$3] = $6; at[$3 - 1] = $6; at[$3 + 1] = $6; at[$3 - 2] = $6; at[$3 + 2] = $6 }
    else { bp[nb] = $3; bs[nb] = $6; nb++ }
}
END {
    for (i = 0; i < nb; i++)
        if ((bp[i] in at) && at[bp[i]] == bs[i]) same++
    da = n["A"] ? pw["A"] / n["A"] : 0; db = n["B"] ? pw["B"] / n["B"] : 0
    printf "carrier 1: %d bursts, %d of %d CC 2; carrier 2: %d bursts, %d of %d CC 2, %d voice; %d of carrier 2 on carrier 1'"'"'s slots; %.2f / %.2f dBFS\n", \
        n["A"], ok["A"], cc["A"], n["B"], ok["B"], cc["B"], voice["B"], same, da, db
    bad = 0
    if (n["A"] < 500 || n["B"] < 500) { print "FAIL: too few bursts decoded"; bad = 1 }
    if (ok["A"] < 0.98 * cc["A"] || ok["B"] < 0.98 * cc["B"]) { print "FAIL: bursts not on CC 2"; bad = 1 }
    if (voice["B"] < 60) { print "FAIL: no calls'"'"' voice on carrier 2"; bad = 1 }
    if (same < 0.97 * nb) { print "FAIL: carrier 2'"'"'s bursts off carrier 1'"'"'s slots"; bad = 1 }
    if (da - db > 1 || db - da > 1) { print "FAIL: the carriers'"'"' levels differ"; bad = 1 }
    exit bad
}' "$D/bursts" || fail=1
[ $fail = 0 ] && echo "two carriers: all passed" || { grep "air MS\|tier3\|carrier" "$D/log" | head -60; cat "$D/rdofsdr.log"; exit 1; }
