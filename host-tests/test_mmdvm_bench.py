#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
"""rdofradio's MMDVM + SDR bench end to end, with no RF: rdofradio --mmdvm on fakemodem's pty, --sim-sdr as the
receiver, the bench line protocol over TCP. Run by ctest; arguments: rdofradio's path, fakemodem's path."""
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

RDOF, FAKE = sys.argv[1], sys.argv[2]


class Rig:
    def __init__(self, duplex, description=None):
        self.d = tempfile.mkdtemp(prefix="rdof-mmdvm-")
        self.path = os.path.join(self.d, "path")
        self.log = os.path.join(self.d, "modem.log")
        args = [FAKE, "--kind", "mmdvm_hs", "--path-file", self.path, "--log", self.log, "--trace", "--exit-after-s", "90"]
        if description:
            args += ["--description", description]
        self.fake = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            if os.path.exists(self.path) and open(self.path).read().strip():
                break
            time.sleep(0.05)
        self.err = open(os.path.join(self.d, "rdofradio.err"), "w+")
        self.radio = subprocess.Popen(
            [RDOF, "--allow-tx", "--mmdvm", open(self.path).read().strip(), "--sim-sdr", "-r", "240000",
             "--rx", "438.5", "--tx", "438.6", "--duplex", duplex, "--cc", "1", "--bench", "127.0.0.1:0",
             "--bench-tx", "438.6,438.5", "--seconds", "80"],
            stderr=self.err, stdout=subprocess.DEVNULL)
        self.port = None
        for _ in range(100):
            self.err.seek(0)
            m = re.search(r"bench: 127\.0\.0\.1:(\d+)", self.err.read())
            if m:
                self.port = int(m.group(1))
                break
            time.sleep(0.1)
        assert self.port, "rdofradio's bench line didn't start: " + open(self.err.name).read()

    def ask(self, req, timeout=8.0):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=timeout)
        s.settimeout(timeout)
        s.sendall((json.dumps(req) + "\n").encode())
        f = s.makefile("rb")
        out = []
        try:
            for line in f:
                out.append(json.loads(line))
                if "error" in out[-1] or req["op"] in ("info", "status", "claim", "release", "mssim", "session") and "state" not in out[-1]:
                    break
                if "exit" in out[-1] or "tuned" in out[-1]:
                    break
        except socket.timeout:
            pass
        s.close()
        return out

    def stop(self):
        self.radio.terminate()
        self.fake.terminate()
        self.radio.wait(5)
        self.fake.wait(5)

    def modem(self):
        return open(self.log).read() if os.path.exists(self.log) else ""


checks = failures = 0


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print("FAIL", what)


def session(r):
    s = socket.create_connection(("127.0.0.1", r.port), timeout=8)
    s.sendall(b'{"op":"session","label":"t"}\n')
    f = s.makefile("rb")
    return s, json.loads(f.readline())["session"]


DUAL = "MMDVM_HS_Dual_Hat-v1.5.2 20201108 14.7456MHz dual ADF7021 FW"
SIMPLEX = "MMDVM_HS_Hat-v1.5.2 20201108 14.7456MHz ADF7021 FW"

# a duplex modem, as a transceiver
r = Rig("half", DUAL)
try:
    info = r.ask({"op": "info"})[0]
    st = info["stations"][0]
    check(st["kind"] == "mmdvm" and "Dual" in st["model"], f"the station: {st}")
    ch = next(c for c in info["channels"] if abs(c["freq"] - 438.6) < 1e-6)
    check("mmdvm_tx" in ch["caps"] and "sdr_tx" not in ch["caps"], f"a modem's TX caps: {ch}")
    check(info["jobs"]["bench_tx"] is None and info["jobs"]["bench_repeater"] is None and info["jobs"]["outbound"] is None
          and info["jobs"]["mssim"] is None, f"jobs: {info['jobs']}")
    check(info["signal_modes"]["tx"] == [] and "MMDVM" in info["jobs"]["signal_tx"], f"signal_tx: {info['jobs']['signal_tx']}")
    check("fm" in info["signal_modes"]["rx"], f"rx modes: {info['signal_modes']}")
    sock, sid = session(r)
    out = r.ask({"op": "tx", "session": sid, "mode": "fm", "freq": 438.6, "program": []})
    check(out[-1].get("code") == "unsupported" and "FM" in out[-1]["error"], f"tx fm: {out}")
    # a csbk job through the modem
    out = r.ask({"op": "run", "session": sid, "job": {"tool": "bench_tx", "cmd": "csbk", "freq": 438.6, "cc": 1,
                                                      "src": 1, "dst": 2, "listen_after": 0}, "wait_max": 10}, 12)
    check(any(x.get("exit") == 0 for x in out), f"csbk: {out}")
    check("TX" in r.modem(), "the modem sent the csbk")
    # a tune with a gain, on a device that sets it, and its release
    out = r.ask({"op": "tune", "session": sid, "center": 438.1, "gain": 25.0, "taps": [438.2]}, 10)
    check("tuned" in out[-1] and out[-1]["tuned"]["window"]["gain"] is not None, f"tune with a gain: {out}")
    out = r.ask({"op": "release", "session": sid, "tune": True}, 10)
    check("released" in out[-1], f"release of the tune: {out}")
    # the responder arms on the modem (438.5 is the modem-rx channel)
    out = r.ask({"op": "mssim", "session": sid, "action": "arm", "freq": 438.5, "cc": 1, "seconds": 3, "answer": "ack", "slots": 4})
    check("mssim" in out[-1] and out[-1]["mssim"]["armed"], f"mssim arm: {out}")
    time.sleep(0.5)
    out = r.ask({"op": "mssim", "session": sid, "action": "status"})
    check(out[-1]["mssim"]["armed"], f"mssim status: {out}")
    r.ask({"op": "mssim", "session": sid, "action": "disarm"})
    # an outbound through the duplex modem
    out = r.ask({"op": "run", "session": sid, "job": {"tool": "bench_tx", "cmd": "outbound", "freq": 438.6, "cc": 1, "seconds": 0.6},
                 "wait_max": 10}, 15)
    check(any(x.get("exit") == 0 for x in out), f"outbound: {out[-3:]}")
    recs = [json.loads(x["out"]) for x in out if "out" in x and x["out"].startswith("{")]
    check(any("start" in x for x in recs) and any("summary" in x for x in recs), f"outbound's records: {recs[:2]}")
    modem = r.modem()
    check("DMR_START" in modem or "START" in modem, "DMR_START in the modem's log: " + modem[-300:])
    # a repeater job through the same modem
    out = r.ask({"op": "run", "session": sid, "job": {"tool": "bench_repeater", "rx": 438.5, "tx": 438.6, "cc": 1, "seconds": 1.0},
                 "wait_max": 10}, 15)
    check(any(x.get("exit") == 0 for x in out), f"repeater: {out[-3:]}")
    out = r.ask({"op": "status"})
    check("sessions" in out[-1], f"status: {out}")
    sock.close()
finally:
    r.stop()

# a simplex modem: repeater and outbound are refused with the physical reason, the rest still run
r = Rig("half", SIMPLEX)
try:
    info = r.ask({"op": "info"})[0]
    check("one transceiver" in (info["jobs"]["outbound"] or "") and "one transceiver" in (info["jobs"]["bench_repeater"] or ""),
          f"simplex jobs: {info['jobs']}")
    check(info["jobs"]["bench_tx"] is None, f"simplex bench_tx: {info['jobs']}")
    sock, sid = session(r)
    out = r.ask({"op": "run", "session": sid, "job": {"tool": "bench_tx", "cmd": "outbound", "freq": 438.6, "cc": 1, "seconds": 0.5}}, 8)
    check(out[-1].get("code") == "unsupported" and "duplex" in out[-1]["error"], f"simplex outbound: {out}")
    out = r.ask({"op": "run", "session": sid, "job": {"tool": "bench_repeater", "rx": 438.5, "tx": 438.6, "cc": 1, "seconds": 0.5}}, 8)
    check(out[-1].get("code") == "unsupported", f"simplex repeater: {out}")
    sock.close()
finally:
    r.stop()

# a duplex repeater on the modem gives a job the modem and takes it back
r = Rig("full", DUAL)
try:
    info = r.ask({"op": "info"})[0]
    check(info["jobs"]["bench_tx"] is None, f"repeater bench_tx: {info['jobs']}")
    sock, sid = session(r)
    out = r.ask({"op": "run", "session": sid, "job": {"tool": "bench_tx", "cmd": "csbk", "freq": 438.6, "cc": 1,
                                                      "src": 1, "dst": 2, "listen_after": 0}, "wait_max": 10}, 12)
    check(any(x.get("exit") == 0 for x in out), f"csbk with a repeater: {out[-3:]}")
    time.sleep(0.5)
    err = open(r.err.name).read()
    check("stops the repeater" in err, "the job stopped the repeater: " + err[-500:])
    check("the repeater's again" in err or "the transmitter is the repeater" in err, "and it came back: " + err[-500:])
    sock.close()
finally:
    r.stop()

print(f"{checks} checks, {failures} failures")
sys.exit(1 if failures else 0)
