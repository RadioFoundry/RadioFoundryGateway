#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
"""config/examples end to end, with no RF: each settings file run by rdofradio as written, on simulated hardware in
place of its devices (fakemodem's pty for an MMDVM modem, --sim-sdr for the RTL-SDR, --file for the PlutoSDR,
fakebm for BrandMeister). Each one's clock is netclock from the public servers, its bench takes transmit jobs in the
amateur bands and refuses one outside them, a key-up past max_key_s, and receives where it physically can; the
standalone MMDVM ones play a call to the parrot back, the BrandMeister one logs in, carries every talkgroup up and
puts BrandMeister's echo back on air. Run by ctest; arguments: rdofradio, fakemodem, fakebm, config/examples."""
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

RDOF, FAKE, FAKEBM, EX = sys.argv[1:5]
SRC = 3100123
HAM_MHZ = [[50, 54], [144, 148], [222, 225], [420, 450], [902, 928], [1240, 1300], [2300, 2310], [2390, 2450]]
SERVERS = "time-a-g.nist.gov time-a-b.nist.gov tick.usno.navy.mil tock.usno.navy.mil time.apple.com time.cloudflare.com"

checks = failures = 0


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print("FAIL", what)
    return cond


def wait_for(pred, seconds, step=0.05):
    end = time.time() + seconds
    while time.time() < end:
        if pred():
            return True
        time.sleep(step)
    return pred()


def read(path):
    return open(path).read() if os.path.exists(path) else ""


class Box:
    """One example on rdofradio: its modem a fakemodem (if it has one), the rest as the command line says."""

    def __init__(self, name, kind=None, sdr=(), modem_args=(), edit=None, fakebm=None):
        self.name = name
        self.d = tempfile.mkdtemp(prefix="rdof-ex-")
        self.procs = []
        text = open(os.path.join(EX, name)).read()
        argv = [RDOF, "--allow-tx", *sdr]
        self.log = os.path.join(self.d, "modem.log")
        if kind:
            pty = os.path.join(self.d, "pty")
            self.procs.append(subprocess.Popen(
                [FAKE, "--kind", kind, "--cc", "1", "--src", str(SRC), "--path-file", pty, "--log", self.log,
                 "--exit-after-s", "60", *modem_args], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
            assert wait_for(lambda: read(pty).strip(), 5), "fakemodem gave no pty"
            path = read(pty).strip()
            assert "/dev/ttyAMA0" in text, name + ": no /dev/ttyAMA0 to swap"
            text = text.replace("/dev/ttyAMA0", path)
            argv += ["--mmdvm", path]
        if edit:
            text = edit(text)
        cfg = os.path.join(self.d, "cfg.json")
        open(cfg, "w").write(text)
        self.err = os.path.join(self.d, "rdofradio.err")
        argv += ["--config", cfg, "--bench", "127.0.0.1:0", "--seconds", "50"]
        self.radio = subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=open(self.err, "w"))
        self.procs.append(self.radio)
        m = None

        def port():
            nonlocal m
            m = re.search(r"bench: 127\.0\.0\.1:(\d+)", read(self.err))
            return m or self.radio.poll() is not None

        wait_for(port, 10)
        assert m, name + ": the bench line didn't start: " + read(self.err)[-1500:]
        self.port = int(m.group(1))

    def ask(self, req, timeout=15.0):
        """The reply lines of one request, up to its last (an error, an exit, a done; one line for info and status)."""
        with socket.create_connection(("127.0.0.1", self.port), timeout=timeout) as s:
            s.settimeout(timeout)
            s.sendall((json.dumps(req) + "\n").encode())
            out = []
            try:
                for line in s.makefile("rb"):
                    out.append(json.loads(line))
                    last = out[-1]
                    if "error" in last or "exit" in last or "done" in last or req["op"] in ("info", "status"):
                        break
                    if req["op"] == "bursts" and "ready" in last:
                        break
            except socket.timeout:
                pass
        return out

    def job(self, freq, cmd="csbk", **kw):
        j = {"tool": "bench_tx", "cmd": cmd, "freq": freq, "cc": 1, "src": 1, "dst": 2, "listen_after": 0, **kw}
        return self.ask({"op": "run", "job": j, "wait_max": 15}, 25)

    def stop(self):
        for p in self.procs:
            if p.poll() is None:
                p.terminate()
        for p in self.procs:
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()
        out = read(self.err)
        check("AddressSanitizer" not in out and "runtime error" not in out, f"{self.name}: sanitizer: {out[-1500:]}")
        shutil.rmtree(self.d, ignore_errors=True)
        return out


def exited(out, code=0):
    return any(x.get("exit") == code for x in out)


def common(b, has_sdr, label, uhf_only=False):
    """What every example's box does: the clock, the bench's bands and its gates. uhf_only: a hotspot board, whose
    modem tunes 70 cm only (the bench lets a job ask; the modem refuses it)."""
    err = read(b.err)
    check("clock: netclock from " + SERVERS in err, f"{label}: the clock: {err[:600]}")
    info = b.ask({"op": "info"})[0]
    check(info.get("tx_bands") == HAM_MHZ, f"{label}: tx_bands {info.get('tx_bands')}")
    check(info["jobs"]["bench_tx"] is None, f"{label}: bench_tx {info['jobs']}")
    # the window opens once the receiver's first samples are in
    win = lambda: b.ask({"op": "info"})[0]["window"]["enabled"]
    check(wait_for(win, 3, 0.2) if has_sdr else not win(), f"{label}: the receive window {has_sdr}")
    # in the bands: 2 m, 70 cm (the box's own band) and its edges, 23 cm; outside: a business frequency, below 6 m,
    # between bands
    for mhz in (146.52, 420.0125, 446.0, 449.9875, 1294.0):
        out = b.job(mhz)
        if uhf_only and not 420 <= mhz <= 450:
            check(out and "the modem refused SET_FREQ" in json.dumps(out), f"{label}: a csbk on {mhz} MHz: {out[-3:]}")
        else:
            check(exited(out), f"{label}: a csbk on {mhz} MHz: {out[-3:]}")
    for mhz in (461.0375, 29.6, 160.0):
        out = b.job(mhz)
        check(out and out[-1].get("code") == "refused" and "transmit frequency" in out[-1]["error"],
              f"{label}: a csbk on {mhz} MHz should be refused: {out[-2:]}")
    return info


def rx(b, has_sdr, label):
    """Receiving: an SDR tunes to a frequency anywhere in the bands for a request; a modem alone has no receiver."""
    out = b.ask({"op": "bursts", "freq": 146.52, "seconds": 0.5}, 10)
    if has_sdr:
        check(any("tuning the receiver to 146.5200" in x.get("note", "") for x in out) and
              not any("error" in x for x in out), f"{label}: bursts on 2 m: {out[-3:]}")
    else:
        check(out and out[-1].get("code") == "refused", f"{label}: a modem alone receives nothing: {out[-2:]}")


# ── standalone MMDVM: a modem, with or without the RTL-SDR (--sim-sdr in its place) ──
for name, kind, sdr in (("standalone-mmdvm-simplex.json", "mmdvm_hs", ()),
                        ("standalone-mmdvm-duplex.json", "mmdvm", ()),
                        ("standalone-mmdvm-rtl-simplex.json", "mmdvm_hs", ("--sim-sdr", "-r", "288000")),
                        ("standalone-mmdvm-rtl-duplex.json", "mmdvm", ("--sim-sdr", "-r", "960000"))):
    duplex, has_sdr = kind == "mmdvm", bool(sdr)
    slot = "1" if duplex else "2"
    # a radio calls the parrot (TG 9990) once the bench's jobs are done
    b = Box(name, kind, sdr, ("--delay-s", "8", "--slot", slot, "--call", "9990:group:1"))
    try:
        info = common(b, has_sdr, name, uhf_only=not duplex)
        check(info["stations"][0]["kind"] == "mmdvm", f"{name}: the station {info['stations']}")
        for job in ("mssim", "outbound"):  # fakemodem's hotspot is a dual hat: two transceivers, an outbound's
            check((info["jobs"][job] is None) == has_sdr, f"{name}: {job} {info['jobs'][job]}")
        modem = read(b.log)
        check(re.search(r"TX s[12] ", modem), f"{name}: the modem sent the jobs' bursts: {modem[-300:]}")
        if duplex and has_sdr:
            out = b.job(438.75, "outbound", seconds=12)
            check(out and "max_key_s 10.0" in json.dumps(out), f"{name}: a 12 s outbound: {out[-2:]}")
            out = b.job(438.75, "outbound", seconds=0.6)
            check(exited(out), f"{name}: a 0.6 s outbound: {out[-3:]}")
        elif duplex:
            out = b.job(438.75, "outbound", seconds=0.6)
            check(out and out[-1].get("code") == "unsupported", f"{name}: an outbound with no SDR: {out[-2:]}")
        if duplex:  # the bench's own repeater, on the modem's receiver
            rpt = {"tool": "bench_repeater", "rx": 438.25, "tx": 438.75, "cc": 1, "seconds": 2}
            out = b.ask({"op": "run", "job": rpt, "wait_max": 15}, 25)
            check(exited(out) and "summary" in json.dumps(out), f"{name}: a bench repeater: {out[-3:]}")
        rx(b, has_sdr, name)
        # the parrot: the call comes back on air, from the parrot's ID to the talkgroup
        check(wait_for(lambda: re.search(rf"TX s{slot} TLC {SRC} -> 9990 TG", read(b.log)), 15),
              f"{name}: the parrot's echo: {read(b.log)[-600:]}")
    finally:
        b.stop()

# a key-up past max_key_s is cut there: a voice call of 30 superframes (10.8 s) on a modem
b = Box("standalone-mmdvm-simplex.json", "mmdvm_hs")
try:
    out = b.job(446.0, "voice", superframes=30)
    err = read(b.err)
    m = re.search(r"bench: keyed ([\d.]+) s, \d+ bursts \(max_key\)", err)
    check(m and 9.5 <= float(m.group(1)) <= 10.1, f"the voice call cut at 10 s: {err[-600:]}")
finally:
    b.stop()

# ── the PlutoSDR (--file in its place: no RF, a transmitter that writes to /dev/null) ──
for name in ("standalone-pluto-simplex.json", "standalone-pluto-duplex.json"):
    d = tempfile.mkdtemp(prefix="rdof-exf-")
    empty = os.path.join(d, "rx.cf32")
    open(empty, "w").close()
    # the least rate the examples' LOs take: a sanitizer build at 960 kS/s falls behind real time while it transmits
    b = Box(name, sdr=("--file", empty + ",/dev/null", "-r", "288000"))
    try:
        info = common(b, True, name)
        check(info["jobs"]["signal_tx"] is None, f"{name}: signal_tx {info['jobs']['signal_tx']}")
        tone = lambda s: {"op": "tx", "mode": "fm", "freq": 446.0, "program": [{"kind": "tone", "hz": 1000, "s": s}]}
        out = b.ask(tone(12))
        check(out and "max_key_s 10.0" in out[-1].get("error", ""), f"{name}: a 12 s FM tone: {out[-2:]}")
        out = b.ask(tone(0.5))
        check(out and "done" in out[-1], f"{name}: a 0.5 s FM tone: {out[-2:]}")
        if "simplex" in name:  # duplex runs two chains: a sanitizer build falls behind, and the modem's queue fills
            out = b.job(146.52, "voice", superframes=2)
            check(exited(out) and "sent 23 bursts" in json.dumps(out), f"{name}: a voice call on 2 m: {out[-3:]}")
        if "duplex" in name:
            out = b.job(438.75, "outbound", seconds=12)
            check(out and "max_key_s 10.0" in json.dumps(out), f"{name}: a 12 s outbound: {out[-2:]}")
        rx(b, True, name)
    finally:
        b.stop()
        shutil.rmtree(d, ignore_errors=True)

# ── BrandMeister on a duplex MMDVM modem, against fakebm ──
def bm(tg_all):
    port = 62079 if tg_all else 62080
    d = tempfile.mkdtemp(prefix="rdof-exbm-")
    log = os.path.join(d, "bm.log")
    master = subprocess.Popen([FAKEBM, "--port", str(port), "--password", "pw1", "--ids", "980010001", "--log", log],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def edit(t):
        t = t.replace("3102.master.brandmeister.network\"", f"127.0.0.1\", \"port\": {port}")
        t = t.replace("your-hotspot-password", "pw1")
        return t if tg_all else t.replace(', "tg": "*"', "")

    label = "brandmeister-mmdvm-duplex.json" + ("" if tg_all else " without tg")
    # a radio keys TG 91, then calls BrandMeister's echo (a private call to 9990)
    b = Box("brandmeister-mmdvm-duplex.json", "mmdvm", (),
            ("--delay-s", "3", "--slot", "1", "--call", "91:group:1", "--call", "9990:private:1"), edit)
    try:
        if tg_all:
            common(b, False, label)
        check(wait_for(lambda: "RPTC 980010001: up: N0CALL rx 438250000 tx 438750000 cc 01 slots 3" in read(log), 10),
              f"{label}: the login: {read(log)[-600:]}")
        check(wait_for(lambda: "parrot: back to peer 980010001" in read(log), 20), f"{label}: the echo: {read(log)[-600:]}")
        check(wait_for(lambda: re.search(rf"TX s1 TLC 9990 -> {SRC} PC", read(b.log)), 10),
              f"{label}: the echo on air: {read(b.log)[-600:]}")
        up = re.search(rf"DMRD peer 980010001 {SRC} -> 91 TG s1", read(log))
        if tg_all:
            check(up, f"{label}: TG 91 went up: {read(log)[-800:]}")
        else:  # no talkgroup in the policy: a group call goes nowhere, a private call still does
            check(not up, f"{label}: TG 91 shouldn't go up without a tg: {read(log)[-800:]}")
    finally:
        b.stop()
        master.terminate()
        master.wait(5)
        shutil.rmtree(d, ignore_errors=True)


bm(True)
bm(False)

print(f"{checks} checks, {failures} failures")
sys.exit(1 if failures else 0)
