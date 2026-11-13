#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# No RF: a one-carrier Tier III site on a stock duplex MMDVM modem, simulated (--mmdvm sim-hat: its own slot clock
# 3.6 ppm fast, a queue per timeslot taken 106 ms ahead, its idle fill and CACH, its watchdog; rdof/t3hat.h), with
# two simulated MSs beside the air (--air-ms). The box's own settings are a modem's (--config); the site's are
# --tier3's. Both MSs register, one calls the talkgroup and the other follows, then an individual call. Every burst
# written must air on its own slot, the AT as the controller's, the key held all run on one key-up, and the modem's
# own reports of the inbound in the trunk's timeslots.
#
#   sh host-tests/e2e_t3_hat.sh <dir with rdofradio>
set -u
BIN=$1
D=$(mktemp -d "${TMPDIR:-/tmp}/rdof-t3hat.XXXXXX")
trap 'rm -rf "$D"' EXIT
cat > "$D/box.json" <<'J'
{"version": 3, "name": "hatbox", "id": 9800100, "callsign": "N0CALL", "defaults": {"cc": 2},
 "devices": [{"name": "hat", "kind": "modem", "type": "mmdvm_hs", "port": {"path": "sim-hat"}}],
 "frequencies": [438.5]}
J
cat > "$D/site.json" <<'J'
{"version": 3, "name": "t3hat", "id": 9990123, "callsign": "N0CALL", "defaults": {"cc": 2},
 "devices": [{"name": "hat", "kind": "modem", "type": "mmdvm_hs", "port": {"path": "sim-hat"}, "transmits": ["OUT"]},
             {"name": "rx", "kind": "sdr", "driver": "rtlsdr"}],
 "frequencies": [{"name": "OUT", "freq": 438.75}, {"name": "IN", "freq": 438.25, "tx": false}],
 "apps": [{"name": "SITE", "type": "tier3", "system": {"model": "small", "net": 5, "site": 2, "par": "ab"},
           "carriers": [{"out": "OUT", "in": "IN", "chan": 1, "tscc": true}],
           "registration": "open", "wide_tg": "1-99999", "timers": {"hang_s": 2}}]}
J
printf '6 1000001 group 1 1500\n10 1000002 private 1000001 1500\n' > "$D/script"
"$BIN/rdofradio" --allow-tx --mmdvm sim-hat --config "$D/box.json" --tier3 "$D/site.json" --air-ms 2 --sim-tg 1 \
    --sim-script "$D/script" --seconds 16 > "$D/log" 2>&1
rc=$?
fail=0
[ $rc = 0 ] || { echo "FAIL: rdofradio exited $rc"; fail=1; }
check() {
    grep -q "$1" "$D/log" || { echo "FAIL: $2"; fail=1; }
}
check "trunked site: MMDVM_HS sim duplex" "the site has the modem"
check "carrier 1 (modem): trunk slot [0-9]* is its slot" "the modem's slots placed on the trunk's"
check "air MS 1000001: idle, registered" "1000001 registers"
check "air MS 1000002: idle, registered" "1000002 registers"
check "air MS 1000002: payload, registered on ch 1; .* heard 1000001 -> 1$" "1000002 follows 1000001's group call"
check "air MS 1000001: idle, registered on ch 0; calls 1 done" "1000001's group call ends done"
check "air MS 1000002: idle, registered on ch 0; calls 1 done" "1000002's individual call ends done"
# the run's totals: every burst on its own slot, none lost, one grid, the clock measured
line=$(grep "carrier 1 (modem): [0-9]* written, .* grids" "$D/log" | sed 's/.*(modem): //')
echo "$line" | awk '{ w = $1; ok = $3; late = $7; early = $9; lost = $11 }
END {
    if (w < 400) { print "FAIL: " w " bursts written"; exit 1 }
    if (ok + 6 < w) { print "FAIL: " ok " of " w " on their slot"; exit 1 }  # (the last ~160 ms not aired yet)
    if (late + early + lost > 0) { print "FAIL: late " late ", early " early ", lost " lost; exit 1 }
}' || fail=1
# (the sim's slot positions rounded to whole samples: a few tenths of a ppm in 16 s)
grep -q "grids, .*its clock +3\.[3-9][0-9] ppm$" "$D/log" || { echo "FAIL: the modem's clock (3.6 ppm fast) not measured"; fail=1; }
grep -q "AT busy 0 of [1-9]" "$D/log" || { echo "FAIL: an AT busy after a just-in-time write"; fail=1; }
# one key-up held all run by the status polls (the watchdog never fired), no Null short LC while written
grep -q "sim-hat: 1 key-ups, 0 unkeys, 0 watchdog drops, 0 short LC cycles Null" "$D/log" ||
    { echo "FAIL: the modem's key not held (one key-up, no unkey, no watchdog drop)"; fail=1; }
grep -q "status [1-9][0-9]* (0 IDLE, 0 unkeyed, 0 overflow), its queues 1 / 1 deep at most; 0 flushed, 0 NAKed" "$D/log" ||
    { echo "FAIL: the modem's status"; fail=1; }
# the modem's own reports of the MSs' inbound: each tagged with the timeslot the trunk put it in
grep "(modem): status .*, 0 NAKed; it heard" "$D/log" | tail -1 |
    grep -q "it heard [1-9][0-9]* inbound: [1-9][0-9]* in the receiver's timeslot, 0 in the other" ||
    { echo "FAIL: the modem's inbound timeslot not the trunk's"; fail=1; }
# the AT heard before each slot of ours is the one the controller set
grep -o "as the controller's [0-9]* of [0-9]*" "$D/log" | awk '$4 != $6 || $6 < 100 { bad = 1 } END { exit bad }' ||
    { echo "FAIL: an AT not the controller's"; fail=1; }
[ $fail = 0 ] && echo "site on a modem: all passed ($line)" || { grep "air MS\|tier3\|modem" "$D/log" | head -40; exit 1; }
