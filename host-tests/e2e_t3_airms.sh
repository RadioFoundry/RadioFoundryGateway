#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# No RF: a one-carrier Tier III site on a file device (receives zeros, sends to /dev/null) with two simulated MSs
# beside the air (--air-ms): both register, one calls the talkgroup and the other follows, then an individual call
# with the availability check, a status to an MS, UDT short data to an MS (the outbound's rate 1/2 blocks), a status poll, and packet data: unconfirmed
# and confirmed to an MS, and to the talkgroup, each a UDP datagram on the payload channel.
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
printf '6 1000001 group 1 1500\n10 1000002 private 1000001 1500\n14 1000001 status 1000002 5\n17 1000002 sdm 1000001 hello\n19 1000002 setstatus 12\n20 1000001 status 1000002 127\n23 1000001 data 1000002 hello by packet\n28 1000002 cdata 1000001 confirmed one\n33 1000001 gdata 1 to the group\n' \
    > "$D/script"
"$BIN/rdofradio" --file /dev/zero,/dev/null --allow-tx --config "$D/site.json" --air-ms 2 --sim-tg 1 \
    --sim-script "$D/script" --seconds 38 > "$D/log" 2>&1
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
check "air MS 1000001: idle, registered on ch 0; calls 2 done, 0 failed (reason 0x44)" "1000001's status delivered"
check "air MS 1000001: idle, registered on ch 0; calls 3 done, 0 failed (reason 0x47)" "1000001's poll answered"
check "air MS 1000002: idle, registered on ch 0; calls 2 done, 0 failed (reason 0x44)" "1000002's short data delivered"
check 'air MS 1000002: short data from 1000002\|air MS 1000001: short data from 1000002, format 4, 5 octets: "hello"' "1000001 shows the short data"
check 'air MS 1000002: packet data from 1000001, UDP 12.15.66.65:5016 to 12.15.66.66:5016, 15 octets: "hello by packet"' "1000002 takes the packet data"
check 'air MS 1000001: packet data from 1000002, UDP 12.15.66.66:5016 to 12.15.66.65:5016, 13 octets: "confirmed one"' "1000001 takes the confirmed packet data"
check 'air MS 1000002: packet data from 1000001, UDP 12.15.66.65:5016 to 225.0.0.1:5016, 12 octets: "to the group"' "1000002 takes the talkgroup's packet data"
[ $fail = 0 ] && echo "air MSs: all passed" || { grep "air MS\|tier3" "$D/log" | head -40; exit 1; }
