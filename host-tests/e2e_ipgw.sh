#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# No RF, no network link, on one host: config/examples' standalone MMDVM settings (simplex, then duplex), each run by
# one rdofradio box on a simulated modem (fakemodem) with a parrot and the IP gateway and nothing in "networks". A
# radio's confirmed UDP text to the gateway's ID goes out as a UDP datagram to a listener on this host, the radio gets
# its ACK on air, and the listener's reply goes out on air to the radio. The examples' serial path and the gateway's
# NAT are swapped for the pty and a route to the listener.
#
#   sh host-tests/e2e_ipgw.sh <dir with fakemodem, rdofradio> <udp port> [config/examples]
set -u
BIN=$1 PORT=$2 EX=${3:-$(dirname "$0")/../config/examples}
D=$(mktemp -d "${TMPDIR:-/tmp}/rdof-e2e.XXXXXX")
trap 'kill $RA $FA $UD 2>/dev/null; wait 2>/dev/null; rm -rf "$D"' EXIT
RA= FA= UD=
python3 - "$PORT" "$D/udp.log" <<'PY' &
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.0.1", int(sys.argv[1])))
while True:
    d, a = s.recvfrom(2048)
    open(sys.argv[2], "a").write("got %d bytes\n" % len(d))
    s.sendto(d, a)  # the same payload back: the gateway sends it to the radio
PY
UD=$!
fail=0
need() { grep -q "$2" "$1" || { echo "FAIL ($MODE): no '$2' in $(basename "$1")"; fail=1; }; }
for MODE in simplex duplex; do
    if [ $MODE = simplex ]; then KIND=mmdvm_hs SLOT=2; else KIND=mmdvm SLOT=1; fi
    rm -f "$D/pty" "$D/udp.log" "$D/modem.log"
    "$BIN/fakemodem" --kind $KIND --cc 1 --src 3100123 --delay-s 3 --slot $SLOT --confirmed --text 9991:"hello gateway" \
        --path-file "$D/pty" --exit-after-s 8 --log "$D/modem.log" >/dev/null 2>&1 &
    FA=$!
    for i in 1 2 3 4 5 6 7 8 9 10; do [ -s "$D/pty" ] && break; sleep 0.3; done
    [ -s "$D/pty" ] || { echo "FAIL: fakemodem gave no pty"; exit 1; }
    sed -e "s|/dev/ttyAMA0|$(cat "$D/pty")|" -e "s|\"nat\": true|\"routes\": [{\"all\": true, \"to\": \"127.0.0.1:$PORT\"}]|" \
        "$EX/standalone-mmdvm-$MODE.json" > "$D/cfg.json"
    grep -q "127.0.0.1:$PORT" "$D/cfg.json" || { echo "FAIL: the example has no \"nat\": true to swap"; exit 1; }
    "$BIN/rdofradio" --allow-tx --mmdvm "$(cat "$D/pty")" --config "$D/cfg.json" > "$D/radio.out" 2>&1 &
    RA=$!
    wait $FA
    kill $RA 2>/dev/null
    wait $RA 2>/dev/null
    need "$D/radio.out" 'ip gateway:'
    need "$D/udp.log" 'got '
    need "$D/modem.log" "TX s$SLOT DATA .*9991 -> 3100123 PC blocks 0 response"
    need "$D/modem.log" "TX s$SLOT DATA .*9991 -> 3100123 PC blocks [1-9]"
    [ -n "${V:-}" ] && grep -h "ip gateway\|TX s. DATA\|got " "$D/radio.out" "$D/modem.log" "$D/udp.log" | head -40
    if [ $fail = 1 ]; then
        for f in radio.out modem.log udp.log; do echo "--- $f"; tail -30 "$D/$f" 2>/dev/null; done
        exit 1
    fi
done
kill $UD 2>/dev/null
echo "ok: simplex and duplex: a radio's UDP text went out through the IP gateway; its ACK and the reply came back on air"
