#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# No RF: a two-carrier Tier III site whose carriers both go out of split TX halves (two simulated radios, the port
# "sim-split-*": host-tests/fake_radio.h in this build), the SDR (a file device) receiving only. Carrier 1's half
# takes the CACH with each burst (the TSCC's AT and short LC slot by slot). Simulated MSs beside the air (--air-ms)
# hear each slot as the pumps sent it: they register on the TSCC, 1000001 calls 1000002 on carrier 1's payload slot,
# so 1000003's call to 1000004 goes on carrier 2. The SDR's modem sends nothing. Both simulated halves take the
# CACH (RadioFoundry's do), so carrier 2's slots go with theirs too while it carries a call.
#
#   sh host-tests/e2e_t3_split.sh <dir with rdofradio>
set -u
BIN=$1
D=$(mktemp -d "${TMPDIR:-/tmp}/rdof-t3split.XXXXXX")
trap '[ -n "${KEEP:-}" ] || rm -rf "$D"' EXIT
cat > "$D/site.json" <<'J'
{"version": 3, "name": "t3split", "id": 9990123, "callsign": "N0CALL", "defaults": {"cc": 2},
 "devices": [{"name": "sdr", "kind": "sdr", "driver": "iio", "match": {"uri": "local:"}, "transmits": []},
             {"name": "txa", "kind": "modem", "type": "radio_hotspot", "port": {"path": "sim-split-a"},
              "hears": false, "transmits": ["A-OUT"]},
             {"name": "txb", "kind": "modem", "type": "radio_hotspot", "port": {"path": "sim-split-b"},
              "hears": false, "transmits": ["B-OUT"]}],
 "frequencies": [{"name": "A-OUT", "freq": 438.75}, {"name": "A-IN", "freq": 438.25, "tx": false},
                 {"name": "B-OUT", "freq": 438.8125}, {"name": "B-IN", "freq": 438.3125, "tx": false}],
 "apps": [{"name": "SITE", "type": "tier3", "system": {"model": "small", "net": 5, "site": 1, "par": "ab"},
           "carriers": [{"out": "A-OUT", "in": "A-IN", "chan": 1, "tscc": true},
                        {"out": "B-OUT", "in": "B-IN", "chan": 2}],
           "registration": "open", "wide_tg": "1-99999", "timers": {"hang_s": 2}}]}
J
printf '7 1000001 private 1000002 6000\n8.5 1000003 private 1000004 3000\n' > "$D/script"
"$BIN/rdofradio" --file /dev/zero,/dev/null -r 240000 --tx-lo 438.7 --allow-tx --config "$D/site.json" --air-ms 4 --sim-tg 1 \
    --sim-script "$D/script" --seconds 16 > "$D/log" 2>&1
fail=0
check() {
    grep -q "$1" "$D/log" || { echo "FAIL: $2"; fail=1; }
}
check "carrier 1: split TX half on sim-split-a (tx 438.75000, rx 438.25000 MHz, the CACH with each burst)" \
    "carrier 1 on its split half, with the CACH"
check "carrier 2: split TX half on sim-split-b (tx 438.81250, rx 438.31250 MHz)" "carrier 2 on its split half"
check "every carrier on a split TX half; the SDR receives only" "the SDR receives only"
for id in 1000001 1000002 1000003 1000004; do
    check "air MS $id: .*registered" "$id registers through carrier 1's half"
done
check "air MS 1000002: payload, registered on ch 1; .* heard 1000001 -> 1000002$" "1000001's call on carrier 1"
check "air MS 1000004: payload, registered on ch [23]; .* heard 1000003 -> 1000004$" "1000003's call on carrier 2"
check "air MS 1000003: idle, registered on ch 0; calls 1 done" "1000003's call ends done"
check "carrier 1: split: [1-9][0-9]* scheduled ([1-9][0-9]* with the CACH)" "carrier 1's bursts scheduled with the CACH"
check "carrier 2: split: [1-9][0-9]* scheduled ([1-9][0-9]* with the CACH)" "carrier 2's bursts scheduled"
check "modem: 0 bursts sent, 0 idle" "the SDR's modem sent nothing"
[ $fail = 0 ] && echo "split carriers: all passed" || { grep "air MS\|tier3\|carrier\|split" "$D/log" | head -60; exit 1; }
