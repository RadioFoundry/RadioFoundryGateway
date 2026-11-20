# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
"""The performance gate: a board's heavy bench jobs, each checked for lost samples, real time and room to spare.

    RDOF_BOARD=<board ip> RDOF_BOARD_PERF=1 python -m pytest -q hil-tests/test_perf.py

About two and a half minutes. Each job runs on the bench line (TCP 7373) while the test polls `/api/status`. A job
passes when:

- the receiver lost nothing: `fifo_loss_n` (the chip's FIFO, the reader late), `ring_overflows` (the DSP behind),
  `bytes_lost`, no stall (a core's tick stopped for 2.5 ms) and no flash write;
- the job kept real time, with no `gap` and no `gap_samples` (rx), its channel from a receiver tap (rx; again after
  the capture and the trigger, whose history holds PSRAM a while);
- the web server answered each poll within POLL_MAX_S (a busy core 0 starves it);
- the internal heap never fell under HEAP_FLOOR (RDOF_PERF_HEAP_FLOOR to change it).

The board at rest must also leave REST_CPU_MAX of core 0 unused. During a job the bench line, at a low priority,
takes what core 0 has left, so `cpu` then reads near 1: real time and the poll's latency are the measure there.
Each job prints its `cpu_s` split (rx), the cores' load and the worst poll, so a regression shows where it went.

`RDOF_PERF_TX=<MHz>` adds an FM transmission there at RDOF_PERF_TX_DBM (0) dBm and a DMR voice call whose bursts
must all go out on time (lease the frequency first; nothing else here transmits). The
frequencies received are RDOF_PERF_FREQ (446.075) and 25 kHz above it.
"""

import json
import os
import socket
import threading
import time
import urllib.request

import pytest

BOARD = os.environ.get("RDOF_BOARD")
pytestmark = pytest.mark.skipif(not BOARD or os.environ.get("RDOF_BOARD_PERF") != "1",
                                reason="RDOF_BOARD and RDOF_BOARD_PERF=1 are not both set")
FREQ = float(os.environ.get("RDOF_PERF_FREQ", "446.075"))
TX_FREQ = os.environ.get("RDOF_PERF_TX")
HEAP_FLOOR = int(os.environ.get("RDOF_PERF_HEAP_FLOOR", "8192"))
POLL_MAX_S = 2.0
POLL_EVERY_S = 2.0
REST_CPU_MAX = 0.75  # core 0 at rest: the reader and the radio (a W12 about 0.55, 2026-11-19)
PRE_KEY_MS = 500  # a transmission's setup may cost reads this long before its key-up (test_outbound)


def status():
    t0 = time.monotonic()
    with urllib.request.urlopen(f"http://{BOARD}/api/status", timeout=20) as r:
        d = json.load(r)
    return d, time.monotonic() - t0


class Counters:
    """The board's loss counters now; minus an earlier one gives what happened between."""

    def __init__(self, d):
        s = d.get("sdr", {})
        self.stalls = {k: v["n"] for k, v in d.get("stalls", {}).items()}
        self.flash = d.get("flash", {}).get("long_ops", 0)
        self.fifo = s.get("fifo_loss_n", 0)
        self.ring = s.get("ring_overflows", 0)
        self.lost = s.get("bytes_lost", 0)

    def since(self, before):
        return {"stalls": sorted(k for k, n in self.stalls.items() if before.stalls.get(k) != n),
                "flash_writes": self.flash - before.flash, "fifo_losses": self.fifo - before.fifo,
                "ring_overflows": self.ring - before.ring, "bytes_lost": self.lost - before.lost}


class Watch:
    """Polls /api/status beside a job: the worst latency, the lowest heap, the cores' load."""

    def __init__(self):
        self.stop = threading.Event()
        self.worst_s, self.heap_min, self.cpu, self.errors = 0.0, None, [], []
        self.t = threading.Thread(target=self.run, daemon=True)

    def run(self):
        while not self.stop.wait(POLL_EVERY_S):
            try:
                d, s = status()
            except Exception as e:  # noqa: BLE001: a timeout is the finding
                self.errors.append(repr(e))
                continue
            self.worst_s = max(self.worst_s, s)
            h = d.get("heap_free")
            if h is not None:
                self.heap_min = h if self.heap_min is None else min(self.heap_min, h)
            if d.get("cpu"):
                self.cpu.append(d["cpu"])

    def __enter__(self):
        self.t.start()
        return self

    def __exit__(self, *a):
        self.stop.set()
        self.t.join(30)


def job(req, body=b"", timeout=180):
    """Runs one bench-line request to its end: (frames by kind, the gap frames, the last frame, wall seconds)."""
    s = socket.create_connection((BOARD, 7373), 10)
    s.settimeout(timeout)
    s.sendall((json.dumps(req) + "\n").encode() + body)
    f = s.makefile("rb")
    kinds, gaps, last, t0 = {}, [], None, time.monotonic()
    try:
        while True:
            line = f.readline()
            if not line:
                break
            d = json.loads(line)
            k = next(iter(d))
            kinds[k] = kinds.get(k, 0) + 1
            if "bytes" in d:
                f.read(d["bytes"])
                continue
            if k == "gap":
                gaps.append(d["gap"])
            last = d
            if k in ("done", "error"):
                break
    finally:
        s.close()
    return kinds, gaps, last, time.monotonic() - t0


NOTHING_LOST = {"stalls": [], "flash_writes": 0, "fifo_losses": 0, "ring_overflows": 0, "bytes_lost": 0}


def gated(req, body=b"", timeout=180, allow=None):
    """A job between two counter reads, watched: returns what job() does and the counters' differences. allow(lost,
    status) passes losses the job itself explains (a half-duplex board's transmission)."""
    before = Counters(status()[0])
    with Watch() as w:
        kinds, gaps, last, wall = job(req, body, timeout)
    time.sleep(1)  # the status counts the job's last second
    d_after = status()[0]
    after = Counters(d_after)
    lost = after.since(before)
    cpu = [round(max(c[i] for c in w.cpu), 2) for i in range(len(w.cpu[0]))] if w.cpu else None
    print(f"\n{req['op']} {req.get('mode', '')}: wall {wall:.1f} s, worst poll {w.worst_s:.2f} s, heap min "
          f"{w.heap_min}, cpu max {cpu}, lost {lost}, last {json.dumps(last)[:300]}")
    assert not w.errors, f"status polls failed during the job: {w.errors[:3]}"
    assert w.worst_s <= POLL_MAX_S, f"the web server took {w.worst_s:.2f} s to answer during the job"
    assert w.heap_min is None or w.heap_min >= HEAP_FLOOR, f"internal heap fell to {w.heap_min}"
    assert lost == NOTHING_LOST or (allow and allow(lost, d_after)), lost
    return kinds, gaps, last, wall


def test_rest():
    d, _ = status()
    if not d.get("cpu"):
        pytest.skip("firmware without cpu in /api/status")
    before = Counters(d)
    time.sleep(10)
    d, _ = status()
    print(f"\nrest: cpu {d['cpu']}, heap {d.get('heap_free')}, psram {d.get('memory', {}).get('psram')}")
    assert d["cpu"][0] <= REST_CPU_MAX, f"core 0 at rest {d['cpu'][0]}"
    assert Counters(d).since(before)["fifo_losses"] == 0


def fm_over():
    secs = 30
    kinds, gaps, last, wall = gated({"op": "rx", "mode": "fm", "freq": FREQ, "seconds": secs, "bw": 16000,
                                     "streams": "audio", "tones": True, "block_s": 0.01})
    assert "done" in last, last
    done = last["done"]
    print("cpu_s", done.get("cpu_s"))
    assert not gaps and done["gap_samples"] == 0, gaps[:5]
    assert abs(done["seconds"] - secs) < 0.1
    assert wall < secs + 4, f"{wall:.1f} s for {secs} s: not real time"
    assert kinds.get("rec", 0) >= 0.98 * secs / 0.01
    # a receiver channel's tap, not the channel cut from the wideband history in software (core 0, with gaps): the
    # tap's ring needs PSRAM, and a capture's history rings once kept it all (2026-11-20)
    assert done.get("cpu_s", {}).get("chan", 0) == 0, f"no channel tap: cut in software, cpu_s {done['cpu_s']}"


def test_fm_over_with_audio():
    fm_over()


def test_capture():
    # The capture's own gaps are the link's (one TCP stream from a W12 carries less than 768 KB/s, docs/reference.md);
    # the receiver behind it must lose nothing
    kinds, _, last, _ = gated({"op": "capture", "freq": FREQ, "seconds": 4})
    assert "done" in last, last
    done = last["done"]  # with the pre-roll the history had (warm after an earlier capture)
    assert abs(done["samples"] - round((4 + done["pre_s"]) * done["fs"])) <= 1, done


def test_trigger_beside_the_radio():
    # nothing rises 60 dB: the trigger watches its channel to the timeout, beside the gateway's own receiver
    _, _, last, wall = gated({"op": "trigger", "freq": FREQ + 0.025, "bw": 12500, "threshold_db": 60,
                              "timeout_s": 20, "pre_s": 0.1, "post_s": 0.1})
    assert last.get("code") == "no_signal", last
    assert "fell behind the history 0 times" in last.get("error", ""), last
    assert wall < 25


def test_fm_after_captures():
    # again after the capture and the trigger, whose history stays warm (PSRAM taken) for ten minutes
    fm_over()


@pytest.mark.skipif(not TX_FREQ, reason="RDOF_PERF_TX (a leased frequency, MHz) not set")
def test_outbound():
    secs = 8  # with the lead and tail (0.25 s each), under a board's max_key_s (10 s by default)

    def own_keying(lost, d):
        # a half-duplex front end (a W12) stops receiving while it sends: that gap is the transmission's own, its
        # record held by the transmit turn for the time on air. The transmission's setup, the source's first pulls
        # rendered on the reader's thread, may cost a read or two in the PRE_KEY_MS before it (W12: 1.3 ms gaps
        # 310-340 ms before, in about half the runs, 2026-11-19). Nothing else may be lost
        recs = d.get("sdr", {}).get("fifo_losses") or []
        recs = sorted(recs.values() if isinstance(recs, dict) else recs, key=lambda r: r.get("ago_ms", 0))
        if not recs or recs[0].get("held_by") != "tx_service" or recs[0].get("gap_us", 0) < secs * 1e6:
            return False
        key_ago = recs[0]["ago_ms"] + recs[0]["gap_us"] / 1000  # the key-up, ms before the status read
        pre = [r for r in recs[1:lost["fifo_losses"]] if 0 <= r["ago_ms"] - key_ago <= PRE_KEY_MS]
        print(f"own keying {recs[0]['gap_us'] / 1e6:.2f} s, {len(pre)} reads lost before it")
        return (lost["fifo_losses"] == 1 + len(pre) and lost["ring_overflows"] <= lost["fifo_losses"] and
                not lost["stalls"] and not lost["flash_writes"])

    _, _, last, wall = gated({"op": "tx", "mode": "fm", "freq": float(TX_FREQ), "max_s": secs + 5,
                              "power_dbm": float(os.environ.get("RDOF_PERF_TX_DBM", "0")),
                              "program": [{"kind": "tone", "hz": 1000, "s": secs, "level": 0.6}]},
                             timeout=60, allow=own_keying)
    assert "done" in last, last
    done = last["done"]
    assert not done.get("cut") and not done.get("underruns"), done
    assert done["on_air_s"] >= secs


@pytest.mark.skipif(not TX_FREQ, reason="RDOF_PERF_TX (a leased frequency, MHz) not set")
def test_dmr_voice_on_time():
    # a DMR call keys up once a burst, 60 ms apart: each burst must be staged and its run built before its time.
    # The LR2021's log line counts the runs sent and the late ones (dropped); the receiver goes off for each, so
    # only the timing is checked here
    import re
    import subprocess
    import sys
    tools = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "bench")
    out = subprocess.run([sys.executable, os.path.join(tools, "rfgbench.py"), "--bench", BOARD, "run", "--",
                          "bench_tx.py", "--freq", TX_FREQ, "--power-dbm", os.environ.get("RDOF_PERF_TX_DBM", "0"),
                          "voice", "--superframes", "6"], capture_output=True, text=True, timeout=60)
    m = re.search(r"sent (\d+) bursts", out.stdout + out.stderr)
    assert m, (out.stdout + out.stderr)[-500:]
    bursts = int(m.group(1))
    time.sleep(1)
    with urllib.request.urlopen(f"http://{BOARD}/api/log", timeout=20) as r:
        log = r.read().decode(errors="replace")
    lines = [ln for ln in log.splitlines() if "lr2021: TX at" in ln]
    if not lines:
        pytest.skip("no LR2021 transmit line in /api/log (not a W12?)")
    t = re.search(r"(\d+) runs \(\d+ polar\), (\d+) late, (\d+) early.*?(\d+) failed", lines[-1])
    assert t, lines[-1]
    runs, late, early, failed = map(int, t.groups())
    print(f"\ndmr voice: {bursts} bursts, {runs} runs, {late} late, {early} early, {failed} failed")
    assert late == 0 and failed == 0 and runs + early == bursts, lines[-1]
