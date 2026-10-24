# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
"""
An SCTP echo server on usrsctp, over UDP encapsulation, for tinysctp's interop test. It's HBlink4's
own usrsctp transport (hblink4/usrsctp_transport.py: UsrsctpListener), with a stand-in for its
HBProtocol that sends every message back on the same association.

    python -B usrsctp_echo.py --hblink4 /path/to/HBlink4 --port 62031 --encap-port 9899

Needs libusrsctp (brew install libusrsctp / apt install libusrsctp-dev) and an HBlink4 checkout (it's
only imported; -B keeps Python from writing __pycache__ there).
"""
import argparse
import asyncio
import logging
import os
import sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--hblink4", required=True, help="path to an HBlink4 checkout")
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=62031, help="SCTP port")
    ap.add_argument("--encap-port", type=int, default=9899, help="UDP encapsulation port")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()
    sys.dont_write_bytecode = True
    sys.path.insert(0, a.hblink4)
    logging.basicConfig(level=logging.WARNING if a.quiet else logging.INFO,
                        format="%(asctime)s %(name)s %(message)s")
    from hblink4 import usrsctp_transport as ut
    from hblink4.utils import normalize_addr
    if not ut.USRSCTP_AVAILABLE:
        print("libusrsctp not found", file=sys.stderr)
        return 2

    class Echo:
        """What UsrsctpListener needs of HBProtocol."""

        def __init__(self) -> None:
            self._sctp_transports = {}
            self._repeaters = {}
            self.count = 0

        def datagram_received(self, data: bytes, addr) -> None:
            self.count += 1
            t = self._sctp_transports.get(normalize_addr(addr))
            if t is not None:
                t.write(data)
            if self.count % 500 == 0:
                logging.info("echoed %d messages", self.count)

        def _remove_repeater(self, rid, reason) -> None:
            pass

    async def run() -> None:
        loop = asyncio.get_running_loop()
        echo = Echo()
        listener = ut.UsrsctpListener(echo, a.bind, a.port, loop, encap_port=a.encap_port)
        listener.start()
        print(f"usrsctp echo on SCTP port {a.port}, UDP encapsulation port {a.encap_port}", flush=True)
        await asyncio.Event().wait()

    try:
        asyncio.run(run())
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
