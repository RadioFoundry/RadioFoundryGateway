#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# tinysctp against usrsctp over UDP encapsulation, on this machine: an echo server on HBlink4's usrsctp
# transport (usrsctp_echo.py), then tinysctp_peer three times: a clean link, 15% loss with duplicates,
# reordering and a 3 s outage (lossy_proxy.py), and the same with a 300 ms PR-SCTP lifetime.
#
#   sh components/tinysctp/test/interop.sh      # HBLINK4=<path to an HBlink4 checkout>, its .venv's Python
#
# Needs libusrsctp (brew install libusrsctp / apt install libusrsctp-dev) and an HBlink4 checkout (only
# imported, with -B).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
HBLINK4=${HBLINK4:?set HBLINK4 to an HBlink4 checkout}
PY=${HBLINK4_PYTHON:-$HBLINK4/.venv/bin/python}
BUILD=${BUILD:-$HERE/../../../build/tinysctp}
PORT=62099
ENCAP=9899
RELAY=19899

cmake -S "$HERE" -B "$BUILD" >/dev/null
cmake --build "$BUILD" --target tinysctp_peer >/dev/null
PEER="$BUILD/tinysctp_peer"

"$PY" -B "$HERE/usrsctp_echo.py" --hblink4 "$HBLINK4" --port $PORT --encap-port $ENCAP --quiet &
ECHO=$!
python3 "$HERE/lossy_proxy.py" --listen $RELAY --to 127.0.0.1:$ENCAP --drop 0.15 --dup 0.03 --reorder 0.05 \
    --delay-ms 10 --jitter-ms 10 --outage-at 6 --outage-s 3 >/dev/null &
RELAY_PID=$!
trap 'kill $ECHO $RELAY_PID 2>/dev/null || true' EXIT INT TERM
sleep 1

fail=0
echo "== clean link"
"$PEER" --host 127.0.0.1 --encap-port $ENCAP --port $PORT --count 500 --interval-ms 10 || fail=1
echo "== 15% loss, duplicates, reordering, a 3 s outage"
"$PEER" --host 127.0.0.1 --encap-port $RELAY --port $PORT --count 1000 --interval-ms 20 --timeout-ms 90000 || fail=1
echo "== the same link, 300 ms PR-SCTP lifetime"
"$PEER" --host 127.0.0.1 --encap-port $RELAY --port $PORT --count 1000 --interval-ms 20 --lifetime-ms 300 \
    --timeout-ms 60000 || fail=1
[ $fail = 0 ] && echo "interop: PASS" || echo "interop: FAIL"
exit $fail
