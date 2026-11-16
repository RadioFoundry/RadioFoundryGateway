#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
"""A PlutoSDR's FPGA channelizer with a USB radio transmitting, end to end, with no RF and no Pluto: rdofradio's
--fpga-chz path (probe, start, the DMA frames' parser, the AFC's steps to the NCOs, a raw capture's restart) on
--sim-sdr's simulated block, hearing e2e_288k.cu8 (fixtures/README.md: an MS voice call on 438.1125, cc 2; a direct
mode emergency call on 438.2625, cc 1), and fakemodem as the radios: a --split-tx radio, two split repeaters' radios
from a --config, an MMDVM modem. The Pluto's own flags (--fpga-chz --fpga-duc) throughout.
Run by ctest; arguments: rdofradio's path, fakemodem's path, the recording."""
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

RDOF, FAKE, IQ = sys.argv[1], sys.argv[2], sys.argv[3]
SIM_RX = ["--sim-sdr", "--sim-iq", IQ + ":438.1875", "--fpga-chz", "-r", "288000"]
SIM = SIM_RX + ["--fpga-duc"]
D = tempfile.mkdtemp(prefix="rdof-fpga-")

checks = failures = 0


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print("FAIL", what)


def fake(name, kind, *extra):
    """a fakemodem on a pty: (process, its path, its log's path)"""
    path, log = os.path.join(D, name + ".path"), os.path.join(D, name + ".log")
    p = subprocess.Popen([FAKE, "--kind", kind, "--path-file", path, "--log", log, "--exit-after-s", "60", *extra],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        if os.path.exists(path) and open(path).read().strip():
            break
        time.sleep(0.05)
    return p, open(path).read().strip(), log


def read(path):
    return open(path).read() if os.path.exists(path) else ""


def run(name, args, seconds):
    """rdofradio to its end: (exit status, stdout, stderr)"""
    out, err = os.path.join(D, name + ".out"), os.path.join(D, name + ".err")
    with open(out, "w") as o, open(err, "w") as e:
        try:
            rc = subprocess.run([RDOF, *args, "--seconds", str(seconds)], stdout=o, stderr=e,
                                timeout=seconds + 20).returncode
        except subprocess.TimeoutExpired:
            rc = None
    return rc, read(out), read(err)


def block_ran(err, what):
    """the block found, started on the receiver's channels, nothing lost on the way"""
    check("--fpga-chz: the block at 0x7c480000: 16 channels" in err, f"{what}: the block's probe: {err[:300]}")
    check(re.search(r"--fpga-chz: \d+ channels, R 1, D 6, 97 taps, 48 kS/s from 288000 S/s", err),
          f"{what}: the block's start: {err[:400]}")
    # the block's own counts: no junk ever; a frame missing only where the DMA overflowed (the sim's, when a loaded
    # host leaves the reader over half a second late), and each made up with zeros
    m = re.search(r"--fpga-chz: (\d+) channel frames lost \((\d+) made up with zeros\), (\d+) overflows \(STATUS\), "
                  r"(\d+) words of junk", err)
    lost, made, ovf, junk = (int(x) for x in m.groups()) if m else (-1, -1, -1, -1)
    check(m and junk == 0 and (lost == 0 or (ovf > 0 and made == lost)),
          f"{what}: the block's counts at the end: {m and m.group(0)} {err[-300:]}")
    if lost > 0:
        print(f"  {what}: {lost} frames lost to {ovf} overflow(s): the host was busy")


def calls(log):
    """the voice calls a fake radio transmitted: {(src, dst)} from its headers, embedded LCs and terminators"""
    got = {}
    for kind, src, dst in re.findall(r"TX s\d (VLC|TLC|voice E emb LC) (\d+) -> (\d+) TG", log):
        if (src, dst) != ("0", "0"):  # a superframe the receiver lost samples in: the null embedded LC
            got.setdefault((int(src), int(dst)), set()).add(kind)
    return got


# 1. --listen: the call decoded through the block, the AFC on the block's NCOs
rc, out, err = run("listen", [*SIM_RX, "--listen", "--rx", "438.1125", "--cc", "2", "--bursts"], 4)
check(rc == 0, f"listen's exit: {rc}: {err[-300:]}")
block_ran(err, "listen")
starts = [json.loads(l.split(" json ", 1)[1]) for l in out.splitlines() if " json " in l and '"start"' in l]
check(any(s["DMR"].get("src_id") == 9990070 and s["DMR"].get("dst_id") == 204 for s in starts),
      f"listen: the call's start: {starts[:2]}")
cfo = [float(l.split()[10]) for l in out.splitlines() if " burst " in l and len(l.split()) > 12]
check(len(cfo) > 20 and abs(cfo[-1]) < abs(cfo[0]) / 2 and abs(cfo[-1]) < 120,
      f"listen: the AFC took the frequency error out through the NCOs: {cfo[:2]} ... {cfo[-2:]}")

# refused: the simulated block without --sim-sdr, --sim-iq alone, --sim-tx on a run that doesn't transmit
for args, says, want in ((["--fpga-chz", "--listen", "--rx", "438.1125"], "with --iio", 2),
                         (["--sim-iq", IQ + ":438.1875", "--listen", "--rx", "438.1125", "--file", "/dev/null"],
                          "--sim-iq", 2),
                         (["--sim-iq", IQ, "--sim-sdr", "--listen", "--rx", "438.1125"], "--sim-iq FILE:MHZ", 2),
                         ([*SIM_RX, "--sim-tx", os.path.join(D, "no.cf32"), "--listen", "--rx", "438.1125"],
                          "--sim-tx: this run doesn't transmit on the SDR", 1)):  # the device's open refuses it
    rc, out, err = run("refused", args, 1)
    check(rc == want and says in err, f"refused {args[:3]}: exit {rc}: {err[-200:]}")

# 2. a split repeater, its radio on USB: the Pluto's flags, the radio transmits what the block heard
p, port, log = fake("split", "odr_split")
try:
    rc, out, err = run("split", [*SIM, "--allow-tx", "--split-tx", port, "--rx", "438.1125", "--tx", "438.6",
                                 "--duplex", "full", "--cc", "2"], 8)
finally:
    p.terminate()
    p.wait(5)
check(rc == 0, f"split's exit: {rc}: {err[-400:]}")
block_ran(err, "split")
check("--fpga-duc: the --split-tx radio transmits, not the SDR: not used" in err, "split: --fpga-duc set aside: " + err[:400])
check("RX simulated SDR, FPGA channelizer, 438.1125 MHz" in err, "split: the RX half is the block: " + err[:600])
m = re.search(r"SDR modem: (\d+) heard .*?, (\d+) lost", err)
check(m and int(m.group(1)) > 40 and m.group(2) == "0", f"split: heard through the block: {m and m.group(0)}")
c = calls(read(log))
check(c.get((9990070, 204), set()) >= {"VLC", "TLC", "voice E emb LC"},
      f"split: the radio repeated 9990070 > TG 204 (header, embedded LC, terminator): {c}")
check(set(c) == {(9990070, 204)}, f"split: nothing else on 438.1125 (the other channels' calls aren't its): {c}")

# 3. --config: two split repeaters, each its own USB radio, both inputs on the one block
p0, port0, log0 = fake("r0", "odr_split")
p1, port1, log1 = fake("r1", "odr_split")
cfg = {
    "version": 3, "id": 9800100, "callsign": "N0CALL", "defaults": {"cc": 1},
    "devices": [
        {"name": "pluto", "kind": "sdr", "driver": "iio", "transmits": False},
        {"name": "m0", "kind": "modem", "type": "radio_hotspot", "port": {"path": port0}, "hears": False,
         "transmits": ["out0"]},
        {"name": "m1", "kind": "modem", "type": "radio_hotspot", "port": {"path": port1}, "hears": False,
         "transmits": ["out1"]},
    ],
    "frequencies": [
        {"name": "in0", "freq": 438.1125, "cc": 2}, {"name": "out0", "freq": 438.6125, "cc": 2},
        {"name": "in1", "freq": 438.2625, "cc": 1}, {"name": "out1", "freq": 438.7625, "cc": 1},
    ],
    "apps": [{"name": "R0", "type": "repeater", "in": "in0", "out": "out0"},
             {"name": "R1", "type": "repeater", "in": "in1", "out": "out1"}],
}
cpath = os.path.join(D, "config.json")
with open(cpath, "w") as f:
    json.dump(cfg, f)
try:
    rc, out, err = run("config", [*SIM, "--allow-tx", "--config", cpath], 9)
finally:
    for q in (p0, p1):
        q.terminate()
        q.wait(5)
check(rc == 0, f"config's exit: {rc}: {err[-500:]}")
block_ran(err, "config")
check("--fpga-chz: 2 channels" in err, "config: both inputs on the block: " + err[:500])
c0, c1 = calls(read(log0)), calls(read(log1))
check((9990070, 204) in c0 and set(c0) == {(9990070, 204)}, f"config: R0's radio repeats in0's call only: {c0}")
check((9990055, 210) in c1 and set(c1) == {(9990055, 210)}, f"config: R1's radio repeats in1's call only: {c1}")


# 4. --mmdvm: a duplex modem transmits, the block taps both of its frequencies for the bench
class Bench:
    def __init__(self, port):
        self.port = port

    def ask(self, req, timeout=8.0, until=None):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=timeout)
        s.settimeout(timeout)
        s.sendall((json.dumps(req) + "\n").encode())
        f = s.makefile("rb")
        got = []
        try:
            for line in f:
                got.append(json.loads(line))
                if "error" in got[-1] or "exit" in got[-1] or "done" in got[-1] or (until and until(got[-1])):
                    break
        except (socket.timeout, ValueError):
            pass
        s.close()
        return got


DUAL = "MMDVM_HS_Dual_Hat-v1.5.2 20201108 14.7456MHz dual ADF7021 FW"
p, port, log = fake("mmdvm", "mmdvm_hs", "--description", DUAL)
err_path = os.path.join(D, "mmdvm.err")
e = open(err_path, "w")
radio = subprocess.Popen([RDOF, *SIM, "--allow-tx", "--mmdvm", port, "--rx", "438.1125", "--tx", "438.2625",
                          "--duplex", "half", "--cc", "2", "--bench", "127.0.0.1:0", "--bench-tx", "438.2625",
                          "--seconds", "60"], stderr=e, stdout=subprocess.DEVNULL)
try:
    bport = None
    for _ in range(100):
        m = re.search(r"bench: 127\.0\.0\.1:(\d+)", read(err_path))
        if m:
            bport = int(m.group(1))
            break
        time.sleep(0.1)
    check(bport, "mmdvm: the bench line: " + read(err_path)[-400:])
    b = Bench(bport)
    err = read(err_path)
    check("--fpga-duc: the modem transmits, not the SDR: not used" in err, "mmdvm: --fpga-duc set aside: " + err[:400])
    info = b.ask({"op": "info"}, until=lambda x: "stations" in x)[-1]
    check(info["stations"][0]["kind"] == "mmdvm", f"mmdvm: the modem's the station: {info.get('stations')}")

    def bursts(mhz, seconds=2.0):
        got = b.ask({"op": "bursts", "freq": mhz, "seconds": seconds}, seconds + 6)
        return [x["burst"] for x in got if x.get("burst", {}).get("lc")], [x for x in got if "burst" not in x]

    lc, got = bursts(438.1125)
    srcs = {(x["lc"].get("src"), x["lc"].get("dst")) for x in lc}
    check((9990070, 204) in srcs, f"mmdvm: the bench's bursts on the modem's RX, through the block: {srcs} {got[-2:]}")
    lc, got = bursts(438.2625)  # the call there is on cc 1, the modem's cc 2: the channel's there, its bursts gated
    check(got and got[-1].get("done", {}).get("complete") is True, f"mmdvm: and on its TX frequency: {got[-2:]}")
    # the modem keys while the block runs
    s = socket.create_connection(("127.0.0.1", bport), timeout=8)
    s.sendall(b'{"op":"session","label":"t"}\n')
    sid = json.loads(s.makefile("rb").readline())["session"]
    out = b.ask({"op": "run", "session": sid, "job": {"tool": "bench_tx", "cmd": "csbk", "freq": 438.2625, "cc": 2,
                                                      "src": 1, "dst": 2, "listen_after": 0}, "wait_max": 10}, 12)
    check(any(x.get("exit") == 0 for x in out), f"mmdvm: a csbk through the modem: {out[-3:]}")
    check("TX s" in read(log), "mmdvm: the modem sent it: " + read(log)[-300:])
    s.close()
    # a raw capture stops the block's stream and starts it again; the receiver hears on after
    with socket.create_connection(("127.0.0.1", bport), timeout=10) as s:
        s.sendall(json.dumps({"op": "capture", "raw": True, "freq": 438.1125, "seconds": 0.2}).encode() + b"\n")
        f = s.makefile("rb")
        data, head = b"", {}
        for _ in range(10000):
            head = json.loads(f.readline())
            if "iq" in head:
                data += f.read(head["bytes"])
            elif "done" in head or "error" in head:
                break
    meta = head.get("done", {})
    check(meta.get("raw") is True and meta.get("fmt") == "cs16", f"mmdvm: the raw capture: {head}")
    check(abs(len(data) / 4 - 0.2 * 288000) < 288000 * 0.02, f"mmdvm: 0.2 s of samples: {len(data)} bytes")
    if data:
        peak = max(abs(int.from_bytes(data[i:i + 2], "little", signed=True)) for i in range(0, len(data), 2))
        check(0 < peak <= 2048, f"mmdvm: 12-bit samples, the recording's: peak {peak}")
    time.sleep(0.5)
    lc, got = bursts(438.1125)
    check(any((x["lc"].get("src"), x["lc"].get("dst")) == (9990070, 204) for x in lc),
          f"mmdvm: bursts again after the raw capture (the block restarted): {got[-2:]}")
finally:
    radio.terminate()
    try:
        radio.wait(10)
    except subprocess.TimeoutExpired:
        radio.kill()
    p.terminate()
    p.wait(5)
    e.close()
err = read(err_path)
check(err.count("--fpga-chz: ") >= 2 and "R 1, D 6" in err, "mmdvm: the block started: " + err[:500])

print(f"{checks} checks, {failures} failures")
if failures:
    print("logs in", D)
sys.exit(1 if failures else 0)
