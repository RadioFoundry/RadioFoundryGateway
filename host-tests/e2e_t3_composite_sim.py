#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
"""No RF, no Pluto: a composite TSCC (services.composite_tscc, tier3.md 5.3.2) on the simulated PlutoSDR, with the
board's own flags (--sim-sdr --fpga-chz --fpga-duc). One carrier, three MSs beside the air (--air-ms) on talkgroup 1.
1000001 calls talkgroup 1 for 20 s on TS2; 1000002 leaves it for an emergency call to talkgroup 2, which takes the
TSCC's slot (the lend) until its hang time ends and the control channel comes back.

From the log: the lend and its end, each call's MSs in payload on its slot; from the MQTT status (a broker on this
host, on a free port): trunk.composite.lends. Then rdofsdr decodes what the simulated SDR sent (--sim-tx, at the
interpolator's 96 kS/s): both slots' bursts on one 30 ms grid, each on its own slot; voice on both slots while the
TSCC's slot is lent; the CACH's short LC P_SYS_Parms with the composite bit (Payload Channel Type 1) for the lend
and C_SYS_Parms on both sides of it. The host's load can leave the transmit stream dry for a moment (the sim logs
each one; zeros on the air): bursts lost within a voice superframe after one are excused, none elsewhere.

Run by ctest; arguments: the directory with rdofradio and rdofsdr, and the mosquitto broker (optional: without it,
the lend count is taken from the log only)."""
import json
import os
import re
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

BIN = sys.argv[1]
BROKER = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2] else None
D = tempfile.mkdtemp(prefix="rdof-t3comp-")
SLOT = 1440  # a slot in rdofsdr's 48 kS/s channel samples
EXCUSE_S = 0.45  # after a dry stream: up to the next voice sync, a superframe on (360 ms), and the dry part itself

checks = failures = 0


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print("FAIL", what)
    return cond


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


# ── a broker and a subscriber of every status (MQTT 3.1.1, QoS 0) ──────────

status = []  # the status JSONs, as they came


def mqtt_packet(kind, body):
    n, rl = len(body), b""
    while True:
        b, n = n % 128, n // 128
        rl += bytes([b | (128 if n else 0)])
        if not n:
            return bytes([kind]) + rl + body


def mqtt_str(s):
    return struct.pack(">H", len(s)) + s.encode()


def subscriber(port, stop):
    s = None
    for _ in range(100):  # until the broker listens
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=1)
            break
        except OSError:
            time.sleep(0.1)
    if not s:
        return
    s.sendall(mqtt_packet(0x10, mqtt_str("MQTT") + bytes([4, 2]) + struct.pack(">H", 60) + mqtt_str("t3comp-test")))
    s.sendall(mqtt_packet(0x82, struct.pack(">H", 1) + mqtt_str("#") + b"\x00"))
    s.settimeout(0.5)
    buf = b""
    while not stop.is_set():
        try:
            got = s.recv(65536)
        except socket.timeout:
            continue
        except OSError:
            break
        if not got:
            break
        buf += got
        while len(buf) >= 2:
            n = mul = 0
            i = 1
            while i < len(buf):
                n += (buf[i] & 127) << mul
                mul += 7
                i += 1
                if not buf[i - 1] & 128:
                    break
            else:
                break
            if len(buf) < i + n:
                break
            kind, body, buf = buf[0], buf[i:i + n], buf[i + n:]
            if kind >> 4 == 3:  # PUBLISH
                tl = struct.unpack(">H", body[:2])[0]
                topic, rest = body[2:2 + tl].decode(errors="replace"), body[2 + tl:]
                if (kind >> 1) & 3:
                    rest = rest[2:]
                if topic.endswith("/status"):
                    try:
                        status.append(json.loads(rest))
                    except ValueError:
                        pass
    s.close()


def lends_of(j):
    """trunk.composite.lends in a status, wherever the trunk is"""
    if isinstance(j, dict):
        c = j.get("composite")
        if isinstance(c, dict) and "lends" in c:
            return c["lends"]
        for v in j.values():
            n = lends_of(v)
            if n is not None:
                return n
    elif isinstance(j, list):
        for v in j:
            n = lends_of(v)
            if n is not None:
                return n
    return None


def max_lends():
    got = [n for n in (lends_of(j) for j in list(status)) if n is not None]
    return max(got) if got else None


# ── the site ──────────

port = free_port() if BROKER else None
net = f',\n "networks": [{{"type": "mqtt", "host": "127.0.0.1", "port": {port}}}]' if BROKER else ""
with open(os.path.join(D, "site.json"), "w") as f:
    f.write(f"""{{"version": 3, "name": "t3comp", "id": 9990124, "callsign": "N0CALL", "defaults": {{"cc": 2}},
 "devices": [{{"name": "sdr", "kind": "sdr", "driver": "iio", "match": {{"uri": "local:"}}, "tx": {{}}, "transmits": ["OUT"]}}],
 "frequencies": [{{"name": "OUT", "freq": 438.25}}, {{"name": "IN", "freq": 433.25, "tx": false}}]{net},
 "apps": [{{"name": "SITE", "type": "tier3", "system": {{"model": "small", "net": 5, "site": 1, "par": "ab"}},
           "carriers": [{{"out": "OUT", "in": "IN", "chan": 1, "tscc": true}}],
           "registration": "open", "wide_tg": "1-99999", "timers": {{"hang_s": 3}},
           "services": {{"composite_tscc": true, "composite_late_entry": false}}}}]}}
""")
with open(os.path.join(D, "script"), "w") as f:
    f.write("6 1000001 group 1 20000\n10 1000002 egroup 2 1500\n")

mq = None
stop = threading.Event()
sub = None
if BROKER:
    with open(os.path.join(D, "broker.conf"), "w") as f:
        f.write(f"listener {port} 127.0.0.1\nallow_anonymous true\npersistence false\n")
    mq = subprocess.Popen([BROKER, "-c", os.path.join(D, "broker.conf")], stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL)
    sub = threading.Thread(target=subscriber, args=(port, stop), daemon=True)
    sub.start()

logp, txp = os.path.join(D, "log"), os.path.join(D, "tx.cf32")
env = dict(os.environ, RDOF_HWID="c0c0c1")
with open(logp, "w") as lf:
    rp = subprocess.Popen([os.path.join(BIN, "rdofradio"), "--sim-sdr", "--fpga-chz", "--fpga-duc", "--sim-tx", txp,
                           "--allow-tx", "--config", os.path.join(D, "site.json"), "--air-ms", "3", "--sim-tg", "1",
                           "--sim-script", os.path.join(D, "script"), "--seconds", "45", "-v"],
                          stdout=lf, stderr=subprocess.STDOUT, env=env)


def log():
    with open(logp, errors="replace") as f:
        return f.read()


# Until the lend is over and the control channel has been on 3 s more (the stream's time: its status lines), and
# the status counted the lend; at most the run's 45 s, or 150 s of a loaded host's wall time.
t_wall = time.monotonic()
back_at = None
while rp.poll() is None and time.monotonic() - t_wall < 150:
    text = log()
    if back_at is None and "composite TSCC: the control channel again" in text:
        st = re.findall(r"^status ([0-9.]+) s", text[:text.index("composite TSCC: the control channel again")], re.M)
        back_at = float(st[-1]) if st else 0.0
    if back_at is not None:
        st = re.findall(r"^status ([0-9.]+) s", text, re.M)
        if st and float(st[-1]) >= back_at + 3 and (not BROKER or (max_lends() or 0) >= 1):
            break
    time.sleep(0.2)
if rp.poll() is None:
    rp.send_signal(signal.SIGINT)
    try:
        rp.wait(30)
    except subprocess.TimeoutExpired:
        rp.kill()
        rp.wait()
stop.set()
if mq:
    mq.terminate()
    mq.wait()
text = log()

# ── the log ──────────

lent = text.find("composite TSCC: the control slot carries a call")
m = re.search(r"composite TSCC: the control channel again after ([0-9.]+) s(?: \((\d+) lends)?", text)
check(lent >= 0, "the TSCC's slot lent to a call")
check(m is not None and m.start() > lent, "the control channel back after the lend")
lend_s = float(m.group(1)) if m else 0.0
check(1.0 <= lend_s <= 12.0, f"the lend's length: {lend_s} s")
check(re.search(r"air MS 1000002: payload, registered on ch 0; .* heard 1000002 -> 2$", text, re.M),
      "1000002's emergency call on the lent slot (ch 0)")
check(re.search(r"air MS 1000001: payload, registered on ch 1; .* heard 1000001 -> 1$", text, re.M),
      "1000001's call on TS2 (ch 1)")
check(re.search(r"air MS 1000003: payload, registered on ch 1; .* heard 1000001 -> 1$", text, re.M),
      "1000003 follows talkgroup 1 to TS2")
check(re.search(r"air MS 1000002: payload, registered on ch 1; calls 1 done", text, re.M),
      "1000002 back on talkgroup 1's call after its own")
if BROKER:
    n = max_lends()
    check(n is not None and n >= 1, f"the MQTT status's trunk.composite.lends: {n} ({len(status)} statuses)")
elif m and m.group(2):
    check(int(m.group(2)) >= 1, "the log's lend count")
check("--fpga-duc: STATUS 0x" in text, "the TX interpolator's status at the end")
lo = re.search(r"TX 438\.2500 MHz \(LO ([0-9.]+)\)", text)
check(lo is not None, "the TX LO in the repeater's line")

# the stream's dry moments, in rdofsdr's channel samples: [from, to), from a slot before (a burst the zeros cut short)
dry = [((float(t) - 0.03) * 48000, (float(t) + float(ms) / 1000 + EXCUSE_S) * 48000)
       for t, ms in re.findall(r"sim: the transmit stream ran dry at ([0-9.]+) s: ([0-9.]+) ms", text)]
nd = re.search(r"sim: the transmit stream ran dry (\d+) times", text)
check(nd is not None and int(nd.group(1)) == len(dry), "every dry moment logged")

# ── what was sent ──────────

bursts, slc = [], []
if lo and os.path.exists(txp):
    r = subprocess.run([os.path.join(BIN, "rdofsdr"), "-i", txp, "-r", "96000", "--center",
                        str(round(float(lo.group(1)) * 1e6)), "--ch", "T:438.25", "--bursts", "--quiet"],
                       capture_output=True, text=True, timeout=120)
    with open(os.path.join(D, "bursts"), "w") as f:
        f.write(r.stdout)
    for line in r.stdout.splitlines():
        f = line.split()
        if len(f) >= 7 and f[1] == "burst":  # <id> burst <pos> <dt> <n> <slot> ...
            bursts.append((int(f[2]), int(f[3]), int(f[5])))
        elif len(f) == 4 and f[1] == "slc":  # <id> slc <pos> <slc28 hex>
            slc.append((int(f[2]), int(f[3], 16)))
bursts.sort()
slc.sort()
check(len(bursts) > 300, f"bursts decoded: {len(bursts)}")


def excused(a, b):
    return any(a < hi and b > lo_ for lo_, hi in dry)


# both slots on one grid, each on its own: consecutive bursts a whole number of slots apart (the keyed modem steers
# to the UTC grid by a sample a slot at most: half one at 48 kS/s), the slot changing with an odd number
off_grid, wrong_slot, lost = [], [], []
end = bursts[-1][0] - 48000 // 2 if bursts else 0  # the run's end: the stream stops mid-burst
for (p1, _, s1), (p2, _, s2) in zip(bursts, bursts[1:]):
    k = round((p2 - p1) / SLOT)
    if excused(p1, p2) or p2 > end:
        continue
    if abs(p2 - p1 - k * SLOT) > 2 + k:
        off_grid.append(p2 / 48000)
    elif (s1 != s2) != (k % 2 == 1):
        wrong_slot.append(p2 / 48000)
    lost += [p1 / 48000] * max(k - 1, 0)
# A loaded host's other marks (a turn of the loop late, the receiver's lag taken for a loss) have no place in the
# stream to excuse; with them in the log, a burst or two.
loaded = re.search(r"^slow turn|^receive: [0-9.]+ ms of samples lost", text, re.M) is not None
fmt = lambda ts: ", ".join(f"{t:.2f}" for t in ts[:8])  # noqa: E731
check(not off_grid, f"bursts off the slot grid: {len(off_grid)} (at {fmt(off_grid)} s)")
check(not wrong_slot, f"bursts on the wrong slot: {len(wrong_slot)} (at {fmt(wrong_slot)} s)")
check(len(lost) <= (2 if loaded else 0),
      f"bursts lost, not after a dry stream: {len(lost)} (after {fmt(lost)} s){', the host loaded' if loaded else ''}")

# the short LC: C_SYS_Parms (SLCO 2), P_SYS_Parms (3) with the composite bit (bit 9) for the lend, C_SYS around it
ps = [p for p, v in slc if v >> 24 == 3]
cs = [p for p, v in slc if v >> 24 == 2]
check(ps and all((v >> 9) & 1 for p, v in slc if v >> 24 == 3), f"P_SYS_Parms, Payload Channel Type 1: {len(ps)}")
if ps:
    p0, p1 = ps[0], ps[-1]
    inside = [p for p in cs if p0 < p < p1 and not excused(p - 4 * SLOT, p)]
    check(not inside, f"no C_SYS_Parms while lent: {len(inside)}")
    check(any(p < p0 for p in cs), "C_SYS_Parms before the lend")
    check(sum(p > p1 for p in cs) >= 4, "C_SYS_Parms again after the P_CLEAR")
    span = (p1 - p0) / 48000
    check(abs(span - lend_s) < 1.0, f"P_SYS_Parms for {span:.2f} s, the lend {lend_s} s")
    for s in (1, 2):
        v = sum(1 for p, dt, sl in bursts if p0 <= p <= p1 and sl == s and dt in (240, 241))
        check(v >= 10, f"voice on TS{s} while lent: {v} bursts")

print(f"composite TSCC on the simulated Pluto: lend {lend_s} s, {len(bursts)} bursts, {len(ps)} P_SYS_Parms, "
      f"{len(cs)} C_SYS_Parms, {len(dry)} dry moments, {len(lost)} lost elsewhere; "
      f"{checks - failures}/{checks} checks passed")
if os.path.exists(txp):
    os.unlink(txp)  # 15 MB
if failures:
    print("\n".join(line for line in text.splitlines() if re.search(r"composite|air MS|sim:|transmit|fpga", line))[:6000])
    print("the run's log and settings:", D)
    sys.exit(1)
for f in os.listdir(D):
    os.unlink(os.path.join(D, f))
os.rmdir(D)
