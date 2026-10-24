# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
"""
A UDP relay that loses, duplicates, reorders and delays packets, both ways, for testing SCTP over UDP
on a bad link. The client sends to --listen; the relay forwards to --to from its own socket and
sends the answers back to the client.

    python3 lossy_proxy.py --listen 19899 --to 127.0.0.1:9899 --drop 0.1 --dup 0.02 --reorder 0.05 \
        --delay-ms 10 --jitter-ms 10 [--outage-at 5 --outage-s 3]
"""
import argparse
import heapq
import random
import select
import socket
import sys
import time


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen", type=int, required=True)
    ap.add_argument("--to", required=True, help="host:port")
    ap.add_argument("--drop", type=float, default=0.0)
    ap.add_argument("--dup", type=float, default=0.0)
    ap.add_argument("--reorder", type=float, default=0.0)
    ap.add_argument("--delay-ms", type=float, default=0.0)
    ap.add_argument("--jitter-ms", type=float, default=0.0)
    ap.add_argument("--outage-at", type=float, default=-1, help="seconds after the first packet")
    ap.add_argument("--outage-s", type=float, default=0.0)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    rnd = random.Random(a.seed)
    host, port = a.to.rsplit(":", 1)
    server = (socket.gethostbyname(host), int(port))
    front = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    front.bind(("0.0.0.0", a.listen))
    back = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    back.bind(("0.0.0.0", 0))
    client = None
    queue = []  # (due, seq, sock, data, addr)
    seq = 0
    start = None
    stats = {"in": 0, "dropped": 0, "dup": 0, "reordered": 0}
    print(f"relay {a.listen} -> {server[0]}:{server[1]} via UDP {back.getsockname()[1]}", flush=True)

    def push(sock, data, addr):
        nonlocal seq
        now = time.monotonic()
        stats["in"] += 1
        if 0 <= a.outage_at <= now - start < a.outage_at + a.outage_s or rnd.random() < a.drop:
            stats["dropped"] += 1
            return
        copies = 2 if rnd.random() < a.dup else 1
        stats["dup"] += copies - 1
        for _ in range(copies):
            delay = a.delay_ms + rnd.random() * a.jitter_ms
            if rnd.random() < a.reorder:
                delay += a.delay_ms + 30
                stats["reordered"] += 1
            seq += 1
            heapq.heappush(queue, (now + delay / 1000, seq, sock, data, addr))

    last_report = time.monotonic()
    while True:
        timeout = max(0.0, queue[0][0] - time.monotonic()) if queue else 0.5
        r, _, _ = select.select([front, back], [], [], timeout)
        for s in r:
            data, addr = s.recvfrom(4096)
            if start is None:
                start = time.monotonic()
            if s is front:
                client = addr
                push(back, data, server)
            elif client is not None:
                push(front, data, client)
        now = time.monotonic()
        while queue and queue[0][0] <= now:
            _, _, sock, data, addr = heapq.heappop(queue)
            sock.sendto(data, addr)
        if now - last_report > 5:
            last_report = now
            print(f"relay: {stats}", flush=True)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        pass
