#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# The firmware's SCTP (components/tinysctp) against a real HBlink4 over UDP encapsulation, on this
# machine: HBlink4 runs with SCTP on (usrsctp, sctp_encap: udp) from a throwaway config, and
# tinysctp_peer registers a roamer peer with DMRC and waits for HBlink4's DMRP.
#
#   sh tools/hblink4_sctp_check.sh [--lossy]      # HBLINK4=<path to an HBlink4 checkout>
#
# Needs libusrsctp (brew install libusrsctp) and an HBlink4 checkout with its .venv (only read: Python
# runs with -B, and the config and log go to a temporary directory). --lossy puts lossy_proxy.py
# (15% loss, duplicates, reordering) between the two.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
HBLINK4=${HBLINK4:?set HBLINK4 to an HBlink4 checkout}
PY=${HBLINK4_PYTHON:-$HBLINK4/.venv/bin/python}
PORT=${PORT:-62098}
ENCAP=${ENCAP:-9899}
PEER_ID=${PEER_ID:-9990399}
TEST="$ROOT/components/tinysctp/test"
BUILD="$ROOT/build/tinysctp"

cmake -S "$TEST" -B "$BUILD" >/dev/null
cmake --build "$BUILD" --target tinysctp_peer >/dev/null

TMP=$(mktemp -d)
cat >"$TMP/config.json" <<EOF
{
  "global": {
    "max_missed": 3, "timeout_duration": 30, "disable_ipv6": true,
    "bind_ipv4": "127.0.0.1", "bind_ipv6": "::", "port_ipv4": $PORT, "port_ipv6": $PORT,
    "logging": {"file": "$TMP/hblink.log", "console_level": "INFO", "file_level": "DEBUG", "retention_days": 1},
    "stats_interval": 60, "report_stats": false, "stream_timeout": 2.0, "stream_hang_time": 10.0,
    "user_cache": {"timeout": 600},
    "sctp_enabled": true, "sctp_port_ipv4": $PORT, "sctp_port_ipv6": $PORT,
    "sctp_encap": "udp", "sctp_encap_port": $ENCAP
  },
  "dashboard": {"enabled": false, "disable_ipv6": true, "transport": "unix", "host_ipv4": "127.0.0.1",
                "host_ipv6": "::1", "port": 8765, "unix_socket": "$TMP/hblink4.sock", "buffer_size": 65536},
  "blacklist": {"patterns": []},
  "repeater_configurations": {
    "patterns": [{"name": "test", "match": {"id_ranges": [[1, 9999999999]]},
                  "config": {"passphrase": "passw0rd", "trust": true, "default_unit_calls": true}}],
    "default": {"passphrase": "passw0rd", "slot1_talkgroups": [], "slot2_talkgroups": []}
  },
  "outbound_connections": []
}
EOF

(cd "$TMP" && exec "$PY" -B -c "import sys; sys.dont_write_bytecode = True; sys.path.insert(0, '$HBLINK4'); \
sys.argv = ['hblink4', '$TMP/config.json']; from hblink4.hblink import main; main()" >"$TMP/stdout.log" 2>&1) &
HB=$!
PROXY=
trap 'kill $HB $PROXY 2>/dev/null || true' EXIT INT TERM
for _ in $(seq 50); do
    grep -q "SCTP/usrsctp" "$TMP/stdout.log" 2>/dev/null && break
    sleep 0.2
done
if ! grep -q "SCTP/usrsctp" "$TMP/stdout.log"; then
    echo "HBlink4 didn't start SCTP (log in $TMP):"
    tail -20 "$TMP/stdout.log"
    exit 1
fi

# A roamer's DMRC, as the firmware builds it (rdof/hblink.h).
DMRC=$(python3 -c "
peer = $PEER_ID
body = '%-8.8s%09u%09u%02u%02u%c%-40.40s%-40.40s' % ('SCTPTEST', 461062500, 461062500, 0, 1, '4',
       'RadioFoundry Gateway roaming', 'radiofoundry_gateway simplex')
print((b'DMRC' + peer.to_bytes(4, 'big') + body.encode()).hex())")

TO=$ENCAP
if [ "${1:-}" = "--lossy" ]; then
    python3 "$TEST/lossy_proxy.py" --listen 19898 --to 127.0.0.1:$ENCAP --drop 0.15 --dup 0.03 --reorder 0.05 \
        --delay-ms 10 --jitter-ms 10 >"$TMP/proxy.log" 2>&1 &
    PROXY=$!
    TO=19898
    sleep 0.5
fi

echo "HBlink4 on SCTP port $PORT over UDP $ENCAP; DMRC from peer $PEER_ID"
if "$BUILD/tinysctp_peer" --host 127.0.0.1 --encap-port "$TO" --port "$PORT" --interval-ms 500 \
        --send-hex "$DMRC" --expect-hex 444d5250 --timeout-ms 20000; then
    grep -E "usrsctp inbound|DMRC|registered|$PEER_ID" "$TMP/stdout.log" | head -8
    echo "PASS (logs in $TMP)"
else
    tail -30 "$TMP/stdout.log"
    echo "FAIL (logs in $TMP)"
    exit 1
fi
