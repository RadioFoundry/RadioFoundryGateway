#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# No RF, no SDR: a one-carrier Tier III site on two radios, a split TX half (the TSCC's CACH with each burst) and a
# split RX half, both simulated (the ports "sim-split-*": host-tests/fake_radio.h in this build), each on its own
# modem clock. Simulated MSs beside the air (--air-ms) hear each slot as the TX half's pump sent it; what they send
# goes to the simulated RX half, which reports each burst after its slot as a radio does. Run twice: the RX half with
# RX_META (each burst's slot from its slot number) and without (its label's parity, "sim-split-rx-nometa"). The MSs
# register on the TSCC, 1000003 polls 1000002's short data and 1000001 calls 1000002 on the payload slot.
#
#   sh host-tests/e2e_t3_radios.sh <dir with rdofradio>
set -u
BIN=$1
D=$(mktemp -d "${TMPDIR:-/tmp}/rdof-t3radios.XXXXXX")
trap '[ -n "${KEEP:-}" ] || rm -rf "$D"' EXIT
fail=0
run() { # $1: the RX half's port; $2: how its bursts are placed
    cat > "$D/site.json" <<J
{"version": 3, "name": "t3radios", "id": 9990124, "callsign": "N0CALL", "defaults": {"cc": 2},
 "devices": [{"name": "txa", "kind": "modem", "type": "radio_hotspot", "port": {"path": "sim-split-tx"},
              "hears": false, "transmits": ["A-OUT"]},
             {"name": "rxa", "kind": "modem", "type": "radio_hotspot", "port": {"path": "$1"},
              "hears": {"idle": "A-IN"}, "transmits": []}],
 "frequencies": [{"name": "A-OUT", "freq": 438.75}, {"name": "A-IN", "freq": 438.25, "tx": false}],
 "apps": [{"name": "SITE", "type": "tier3", "system": {"model": "small", "net": 5, "site": 1, "par": "ab"},
           "carriers": [{"out": "A-OUT", "in": "A-IN", "chan": 1, "tscc": true}],
           "registration": "open", "wide_tg": "1-99999", "timers": {"hang_s": 2}}]}
J
    printf '7 1000001 private 1000002 4000\n12 1000003 upoll 1000002 2\n' > "$D/script"
    "$BIN/rdofradio" --allow-tx --config "$D/site.json" --air-ms 3 --sim-tg 1 --sim-script "$D/script" \
        --seconds 20 -v > "$D/log" 2>&1
    bad=0
    check() {
        grep -q "$1" "$D/log" || { echo "FAIL ($2): $3"; bad=1; }
    }
    check "carrier 1: split TX half on sim-split-tx, RX half on $1 (tx 438.75000, rx 438.25000 MHz, the CACH with each burst; each burst's slot $2)" "$1" "carrier 1 on two radios"
    check "every carrier on two radios, a split TX half and a split RX half; no SDR" "$1" "the site without an SDR"
    for id in 1000001 1000002 1000003; do
        check "air MS $id: .*registered" "$1" "$id registers through the RX half"
    done
    check "air MS 1000002: payload, registered on ch 1; .* heard 1000001 -> 1000002$" "$1" "1000001's call heard"
    check "air MS 1000001: idle, registered on ch 0; calls 1 done" "$1" "1000001's call ends done"
    # each C_RAND's answer a few TSCC slots after the slot it was heard in: the RX half's bursts on their slots
    check "C_RAND from 1000001 in slot [0-9]*: answered +[2468] (" "$1" "1000001's C_RAND answered within 8 slots"
    # a UDT short data poll (6.6.5): 1000002's default answer, its ID, down to 1000003
    check 'air MS 1000003: short data from 1000002, format 4, 14 octets: "air MS 1000002"' "$1" \
        "1000003's poll answered"
    check "carrier 1: split: [1-9][0-9]* scheduled ([1-9][0-9]* with the CACH)" "$1" "bursts scheduled with the CACH"
    case $2 in
    from*) check "its RX half's bursts [1-9][0-9]* by RX_META's slot, 0 by parity" "$1" "every burst by RX_META" ;;
    *) check "its RX half's bursts 0 by RX_META's slot, [1-9][0-9]* by parity" "$1" "every burst by parity" ;;
    esac
    if [ $bad != 0 ]; then
        grep "air MS\|tier3\|carrier\|split" "$D/log" | head -60
        fail=1
    fi
}
run sim-split-rx "from RX_META"
run sim-split-rx-nometa "by its parity (no RX_META)"
[ $fail = 0 ] && echo "two radios a carrier: all passed" || exit 1
