#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
"""Route the radios' network (12.0.0.0/8) on this host to a RadioFoundry Gateway board in routed mode.

The board's address comes from its name (mDNS, "gateway.local", or any name or address), so a DHCP lease that
changes is followed: run it again, or leave it running with --watch. Needs root (sudo) to change the route.

  sudo tools/rfg_route.py gateway.local              # add or update the route
  sudo tools/rfg_route.py gateway.local --watch 60   # and follow the board's address
  sudo tools/rfg_route.py --delete                   # remove it

Linux (ip route) and macOS (route). A router for the whole LAN: add "12.0.0.0/8 via <board>" there instead, or turn
on the board's RIP announcements (apps.<gw>.routed.rip) if the router learns RIPv2 (docs/spec/fw/ipgw.md 9.2).
"""
from __future__ import annotations

import argparse
import ipaddress
import platform
import socket
import subprocess
import sys
import time


def resolve(name: str) -> str:
    return socket.getaddrinfo(name, None, socket.AF_INET)[0][4][0]


def current(net: str) -> str | None:
    """The gateway this host routes net through now, or None."""
    if platform.system() == "Darwin":
        r = subprocess.run(["route", "-n", "get", "-net", net], capture_output=True, text=True)
        got = {}
        for line in r.stdout.splitlines():
            k, _, v = line.strip().partition(":")
            got[k] = v.strip()
        # with no route of its own, "get" answers with the one that covers it (the default)
        n = ipaddress.ip_network(net)
        if got.get("destination") != str(n.network_address) or got.get("mask") != str(n.netmask):
            return None
        return got.get("gateway")
    r = subprocess.run(["ip", "-4", "route", "show", net], capture_output=True, text=True)
    words = r.stdout.split()
    return words[words.index("via") + 1] if "via" in words else None


def set_route(net: str, via: str) -> bool:
    if platform.system() == "Darwin":
        verb = "change" if current(net) else "add"
        cmd = ["route", "-n", verb, "-net", net, via]
    else:
        cmd = ["ip", "-4", "route", "replace", net, "via", via]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(f"rfg_route: {' '.join(cmd)}: {(r.stderr or r.stdout).strip()}", file=sys.stderr)
    return r.returncode == 0


def delete_route(net: str) -> bool:
    if platform.system() == "Darwin":
        cmd = ["route", "-n", "delete", "-net", net]
    else:
        cmd = ["ip", "-4", "route", "del", net]
    return subprocess.run(cmd, capture_output=True).returncode == 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("board", nargs="?", help="the board's name or address (gateway.local)")
    ap.add_argument("--net", default="12.0.0.0/8", help="the radios' network, the board's radio_net (12.0.0.0/8)")
    ap.add_argument("--watch", type=float, default=0, metavar="S", help="check the board's address every S seconds")
    ap.add_argument("--delete", action="store_true", help="remove the route")
    a = ap.parse_args()
    net = str(ipaddress.ip_network(a.net))
    if a.delete:
        ok = delete_route(net)
        print(f"rfg_route: {net} {'removed' if ok else 'not removed (none?)'}")
        return 0 if ok else 1
    if not a.board:
        ap.error("the board's name or address")
    last = None
    while True:
        try:
            via = resolve(a.board)
        except OSError as e:
            print(f"rfg_route: {a.board}: {e}", file=sys.stderr)
            via = None
        if via and via != last:
            if current(net) == via:
                print(f"rfg_route: {net} via {via} ({a.board}) already")
                last = via
            elif set_route(net, via):
                print(f"rfg_route: {net} via {via} ({a.board})")
                last = via
            elif not a.watch:
                return 1
        elif not via and not a.watch:
            return 1
        if not a.watch:
            return 0
        time.sleep(a.watch)


if __name__ == "__main__":
    sys.exit(main())
