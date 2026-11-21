#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# No RF: config/examples' Tier III duplex hotspot on BrandMeister as written, the hat simulated (--mmdvm sim-hat) and
# fakebm in BrandMeister's place. Two MSs register (TG 91 keyed on the master), one calls TG 91 and the other follows,
# then a private call to the parrot (9990) comes back on air; every burst written on its own slot.
#
#   sh host-tests/e2e_t3_hat_bm.sh <dir with rdofradio and fakebm> <config/examples> [fakebm's port]
set -u
BIN=$1 EX=$2 PORT=${3:-47017}
D=$(mktemp -d "${TMPDIR:-/tmp}/rdof-t3hatbm.XXXXXX")
trap 'kill $FB 2>/dev/null; rm -rf "$D"' EXIT
sed -e "s/3102.master.brandmeister.network\"/127.0.0.1\", \"port\": $PORT/" -e 's/your-hotspot-password/pw1/' \
    -e 's#/dev/ttyAMA0#sim-hat#' "$EX/ham-tier3-hotspot-brandmeister.json" > "$D/site.json"
sed 's#/dev/ttyAMA0#sim-hat#' "$EX/ham-tier3-hotspot-box.json" > "$D/box.json"
grep -q "127.0.0.1" "$D/site.json" || { echo "FAIL: the master not swapped for fakebm"; exit 1; }
printf '6 1000001 group 91 1500\n10 1000002 private 9990 1500\n' > "$D/script"
"$BIN/fakebm" --port $PORT --password pw1 > "$D/fakebm" 2>&1 &
FB=$!
sleep 0.3
"$BIN/rdofradio" --allow-tx --mmdvm sim-hat --config "$D/box.json" --tier3 "$D/site.json" --air-ms 2 --sim-tg 91 \
    --sim-script "$D/script" --seconds 22 > "$D/log" 2>&1
rc=$?
kill $FB 2>/dev/null
wait $FB 2>/dev/null
fail=0
[ $rc = 0 ] || { echo "FAIL: rdofradio exited $rc"; fail=1; }
check() { # file, pattern, what
    grep -q "$2" "$D/$1" || { echo "FAIL: $3"; fail=1; }
}
check log "trunked site: MMDVM_HS sim duplex" "the site has the hat"
check fakebm "RPTC 980010501: up: N0CALL rx 438250000 tx 438750000 cc 01" "the site logs in"
check fakebm "dynamic: TG 91 on peer 980010501 s1" "TG 91 keyed on the master"
check log "air MS 1000002: payload, registered on ch 1; .* heard 1000001 -> 91$" "1000002 follows the group call"
check fakebm "parrot: back to peer 980010501: 9990 -> 1000002 PC" "the parrot plays the private call back"
check log "air MS 1000002: idle, registered on ch 0; calls 1 done, .* heard 9990 -> 1000002$" "1000002 hears it on air"
line=$(grep "carrier 1 (modem): [0-9]* written, .* grids" "$D/log" | sed 's/.*(modem): //')
echo "$line" | awk '{ w = $1; ok = $3; late = $7; early = $9; lost = $11 }
END { if (w < 500 || ok + 6 < w || late + early + lost > 0) { print "FAIL: slots: " $0; exit 1 } }' || fail=1
[ $fail = 0 ] && echo "tier3 hotspot on brandmeister: all passed" || {
    for f in fakebm log; do echo "== $f"; grep -v "samples lost\| voice sid" "$D/$f" | head -60; done
    exit 1
}
