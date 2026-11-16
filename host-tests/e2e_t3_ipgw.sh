#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# No RF: a one-carrier Tier III site on a file device with its IP gateway app and two simulated MSs (--air-ms). An MS
# sends confirmed UDP text to IPI (tier3.md 3.16); the gateway sends it to a listener on this host, which sends it
# back. The MS gets its ACK and then the reply, both as the site's own data on air. Unconfirmed text follows, and
# its reply comes back the same way (sent to the host's address: @A.B.C.D). Then confirmed text whose first ACK the MS
# misses: it sends again on the channel, and the site, still listening, answers again.
#
#   sh host-tests/e2e_t3_ipgw.sh <dir with rdofradio> <udp port>
set -u
BIN=$1 PORT=$2
D=$(mktemp -d "${TMPDIR:-/tmp}/rdof-t3ipgw.XXXXXX")
UD=
trap 'kill $UD 2>/dev/null; wait 2>/dev/null; rm -rf "$D"' EXIT
python3 - "$PORT" "$D/udp.log" <<'PY' &
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.0.1", int(sys.argv[1])))
while True:
    d, a = s.recvfrom(2048)
    open(sys.argv[2], "a").write("got %d bytes\n" % len(d))
    s.sendto(d, a)
PY
UD=$!
cat > "$D/site.json" <<J
{"version": 3, "name": "t3ipgw", "id": 9990125, "callsign": "N0CALL", "defaults": {"cc": 2},
 "devices": [{"name": "sdr", "kind": "sdr", "driver": "iio", "match": {"uri": "local:"}, "tx": {}, "transmits": ["OUT"]}],
 "frequencies": [{"name": "OUT", "freq": 438.25}, {"name": "IN", "freq": 433.25, "tx": false}],
 "apps": [{"name": "SITE", "type": "tier3", "system": {"model": "small", "net": 5, "site": 1, "par": "ab"},
           "carriers": [{"out": "OUT", "in": "IN", "chan": 1, "tscc": true}],
           "registration": "open", "wide_tg": "1-99999", "timers": {"hang_s": 3}},
          {"type": "ipgw", "pc": "9991", "routes": [{"all": true, "to": "127.0.0.1:$PORT"}]}]}
J
printf '6 1000001 cdata 16776899 ping confirmed\n20 1000001 data 16776899 @127.0.0.1 ping plain\n34 1000001 missack 1\n35 1000001 cdata 16776899 @127.0.0.1 ping again\n' > "$D/script"
"$BIN/rdofradio" --file /dev/zero,/dev/null --allow-tx -v --config "$D/site.json" --air-ms 2 --sim-tg 1 \
    --sim-script "$D/script" --seconds 56 > "$D/log" 2>&1
fail=0
check() {
    [ "$(grep -c "$1" "$D/log")" -ge "${3:-1}" ] || { echo "FAIL: $2"; fail=1; }
}
check "ip gateway" "the site starts its IP gateway"
check "tier3: site data .* -> 1000001, [0-9]* bursts" "the site sends the gateway's data down" 3
check 'air MS 1000001: packet data from .*UTF-16BE text: "ping confirmed"' "the confirmed text comes back to the MS"
check 'air MS 1000001: packet data from .*UTF-16BE text: "ping plain"' "the unconfirmed text comes back to the MS"
check "air MS 1000001: idle, registered on ch 0; calls 1 done, 0 failed" "the confirmed data is acknowledged"
check 'air MS 1000001: packet data from .*UTF-16BE text: "ping again"' "a missed ACK: the text comes back"
check "air MS 1000001: idle, registered on ch 0; calls 3 done, 0 failed" "a missed ACK: answered again on the retry"
grep -q "not sent" "$D/log" && { echo "FAIL: the site refused the gateway's data"; fail=1; }
[ "$(grep -c got "$D/udp.log" 2>/dev/null)" = 3 ] || { echo "FAIL: the listener didn't get the three datagrams once each"; fail=1; }
[ $fail = 0 ] && echo "ok: a Tier III site's IP gateway: out to a host and back to the MS, confirmed and not" ||
    { grep "air MS 1000001\|tier3: site data\|packet data\|ip gateway" "$D/log" | tail -40; exit 1; }
