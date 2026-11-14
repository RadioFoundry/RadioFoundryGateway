#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# No RF: a four-carrier Tier III site whose carriers all go out of one SDR (a file device: receives zeros, writes what
# it transmits), the TX LO mid-way between the outest two, with twelve simulated MSs beside the air (--air-ms). Six
# individual calls at once fill carriers 1 to 3's payload channels and the last takes carrier 4. Then rdofsdr decodes
# the transmit file at the four outs: each carries the site's blocks on carrier 1's slots, CC 2, at the same level,
# and carrier 4 the last call's voice.
#
#   sh host-tests/e2e_t3_car4.sh <dir with rdofradio and rdofsdr>
set -u
BIN=$1
D=$(mktemp -d "${TMPDIR:-/tmp}/rdof-t3car4.XXXXXX")
trap 'rm -rf "$D"' EXIT
cat > "$D/site.json" <<'J'
{"version": 3, "name": "t3car4", "id": 9990124, "callsign": "N0CALL", "defaults": {"cc": 2},
 "devices": [{"name": "sdr", "kind": "sdr", "driver": "iio", "match": {"uri": "local:"}, "tx": {"carriers": 4},
              "transmits": ["A-OUT", "B-OUT", "C-OUT", "D-OUT"]}],
 "frequencies": [{"name": "A-OUT", "freq": 438.75}, {"name": "A-IN", "freq": 438.25, "tx": false},
                 {"name": "B-OUT", "freq": 438.6}, {"name": "B-IN", "freq": 438.3, "tx": false},
                 {"name": "C-OUT", "freq": 438.7125}, {"name": "C-IN", "freq": 438.3375, "tx": false},
                 {"name": "D-OUT", "freq": 438.65}, {"name": "D-IN", "freq": 438.4, "tx": false}],
 "apps": [{"name": "SITE", "type": "tier3", "system": {"model": "small", "net": 5, "site": 1, "par": "ab"},
           "carriers": [{"out": "A-OUT", "in": "A-IN", "chan": 1, "tscc": true},
                        {"out": "B-OUT", "in": "B-IN", "chan": 2},
                        {"out": "C-OUT", "in": "C-IN", "chan": 3},
                        {"out": "D-OUT", "in": "D-IN", "chan": 4}],
           "registration": "open", "wide_tg": "1-99999", "timers": {"hang_s": 2}}]}
J
# --air-ms 12 registers 1000001 to 1000012 (by 12 s); six calls at once, 1000011's the last
{
    echo '12 1000001 private 1000002 10000'
    echo '13 1000003 private 1000004 9000'
    echo '14 1000005 private 1000006 8000'
    echo '15 1000007 private 1000008 7000'
    echo '16 1000009 private 1000010 6000'
    echo '17 1000011 private 1000012 5000'
} > "$D/script"
"$BIN/rdofradio" --file /dev/zero,"$D/tx.cf32" -r 240000 --allow-tx --config "$D/site.json" --air-ms 12 --sim-tg 1 \
    --sim-script "$D/script" --seconds 26 > "$D/log" 2>&1
fail=0
check() {
    grep -q "$1" "$D/log" || { echo "FAIL: $2"; fail=1; }
}
check "carrier 2: sent by the SDR (tx 438.60000, rx 438.30000 MHz), -75.0 kHz from the TX LO 438.67500 MHz" \
    "carrier 2 on the SDR, the TX LO mid-way"
check "carrier 4: sent by the SDR (tx 438.65000, rx 438.40000 MHz), -25.0 kHz from the TX LO 438.67500 MHz" \
    "carrier 4 on the SDR"
for c in 1 2 3 4; do
    check "carrier $c: on the slot grid" "carrier $c on the slot grid"
done
check "air MS 1000011: payload, registered on ch [67]; .* heard 1000011 -> 1000012$" "1000011's call on carrier 4"
check "air MS 1000012: payload, registered on ch [67]; .* heard 1000011 -> 1000012$" "1000012 takes it on carrier 4"
check "air MS 1000011: idle, registered on ch 0; calls 1 done" "1000011's call ends done"
for c in 2 3 4; do
    check "carrier $c: [1-9][0-9]* bursts from the site" "carrier $c's modem sent the site's bursts"
done

# the transmit file, centered on the TX LO: the four outs
"$BIN/rdofsdr" -i "$D/tx.cf32" -r 240000 --center 438675000 --ch A:438.75 --ch B:438.6 --ch C:438.7125 \
    --ch D:438.65 --bursts --quiet > "$D/bursts" 2> "$D/rdofsdr.log"
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
    bad = 0
    split("A B C D", ids, " ")
    for (j = 1; j <= 4; j++) {
        c = ids[j]; p[c] = n[c] ? pw[c] / n[c] : 0
        printf "carrier %d: %d bursts, %d of %d CC 2, %d voice, %.2f dBFS\n", j, n[c], ok[c], cc[c], voice[c], p[c]
        if (n[c] < 500) { print "FAIL: too few bursts decoded on carrier " j; bad = 1 }
        if (ok[c] < 0.98 * cc[c]) { print "FAIL: carrier " j " not on CC 2"; bad = 1 }
        if (p[c] - p["A"] > 1 || p["A"] - p[c] > 1) { print "FAIL: carrier " j "'"'"'s level differs"; bad = 1 }
    }
    printf "%d of %d of carriers 2 to 4 on carrier 1'"'"'s slots\n", same, nb
    if (voice["D"] < 60) { print "FAIL: no call'"'"'s voice on carrier 4"; bad = 1 }
    if (same < 0.97 * nb) { print "FAIL: bursts off carrier 1'"'"'s slots"; bad = 1 }
    exit bad
}' "$D/bursts" || fail=1
[ $fail = 0 ] && echo "four carriers: all passed" || { grep "air MS\|tier3\|carrier" "$D/log" | head -80; cat "$D/rdofsdr.log"; exit 1; }
