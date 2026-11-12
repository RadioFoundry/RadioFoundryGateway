# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
"""Load on a running board's I/Q streams: what a long wideband stream, a second stream, status polling, log reads
and a client that stops reading cost the receiver.

    RDOF_BOARD=<board ip> RDOF_BOARD_STRESS=1 python -m pytest -q hil-tests/test_stress.py

About two minutes. Nothing here transmits or changes settings. Each test reads the board's own counters before and
after (`/api/status`): `stalls` (a core's tick stopped for over 5 ms: interrupts were off that long), `flash`
(writes and erases, each stops both cores), and the SDR's `fifo_loss_n`, `ring_overflows` and `bytes_lost`. A stall
or a flash write during a stream is a defect whatever it cost; a chip FIFO loss is the reader's; a ring overflow is
the DSP not keeping up (docs/spec/lr2021-iq.md R3.58, R3.59).
"""

import json
import os
import socket
import threading
import time
import urllib.parse

import pytest

from test_board import (API, BOARD, URL, get_json, request, text_frames, timed_messages, ws_messages,
                        xfail_slow_link)

pytestmark = pytest.mark.skipif(not BOARD or os.environ.get("RDOF_BOARD_STRESS") != "1",
                                reason="RDOF_BOARD and RDOF_BOARD_STRESS=1 are not both set")
WIDE = {"rate": "wide", "fmt": "cs8"}
SECONDS = 8  # of samples; a W12 sends a wideband stream at about 40% of real time


@pytest.fixture(scope="module")
def info():
    d = get_json(API + "/info")
    if not d["caps"].get("iq_wideband"):
        pytest.skip("no wideband I/Q")
    return d


class Counters:
    """The board's loss counters now; minus an earlier one gives what happened between."""

    def __init__(self):
        d = get_json("/api/status")
        s = d.get("sdr", {})
        if "stalls" not in d or "flash" not in d:
            pytest.skip("firmware older than the stalls and flash counters")
        self.stalls = {k: v["n"] for k, v in d["stalls"].items()}
        self.stall_us = {k: v["us"] for k, v in d["stalls"].items()}
        self.flash_long = d["flash"]["long_ops"]
        self.fifo = s.get("fifo_loss_n", 0)
        self.ring = s.get("ring_overflows", 0)
        self.lost = s.get("bytes_lost", 0)
        self.load = s.get("cpu_load", 0)

    def since(self, before):
        new = sorted(us for k, us in self.stall_us.items() if before.stalls.get(k) != self.stalls[k])
        return {"stalls_us": new, "flash_writes": self.flash_long - before.flash_long,
                "fifo_losses": self.fifo - before.fifo, "ring_overflows": self.ring - before.ring,
                "bytes_lost": self.lost - before.lost}


RATES = {}  # each stream's bytes a second, by key (run_stream)


def run_stream(query, out, key, timeout=SECONDS * 4 + 15):
    try:
        msgs, RATES[key] = timed_messages(API + "/iq", dict(query, seconds=SECONDS), timeout)
        out[key] = text_frames(msgs)
    except Exception as e:  # the assertion is in the test's thread
        out[key] = [{"type": "error", "error": repr(e)}]


def streams(queries, also=None):
    """Run the streams at once (and `also`, a function, beside them); returns each one's text frames by key."""
    out = {}
    ts = [threading.Thread(target=run_stream, args=(q, out, k)) for k, q in queries.items()]
    if also:
        ts.append(threading.Thread(target=also))
    [t.start() for t in ts]
    [t.join() for t in ts]
    for k, texts in out.items():
        if texts and texts[0]["type"] == "start":
            xfail_slow_link(texts, sum(RATES.get(q, 0) for q in queries), f"stream {k}")
        assert texts and texts[0]["type"] == "start" and texts[-1]["type"] == "done", (k, texts[:1], texts[-1:])
    return out


def check(before, what):
    """No stall and no flash write, whatever the board; then the two losses with a known cause still open
    (lr2021-iq.md R3.59, "Left"), which are expected failures where that cause applies."""
    time.sleep(0.5)
    d = Counters().since(before)
    assert d["stalls_us"] == [], f"{what}: a core stopped for {d['stalls_us']} us ({d})"
    assert d["flash_writes"] == 0, f"{what}: the flash was written during a stream ({d})"
    fifo_only = d["ring_overflows"] - d["fifo_losses"]  # ring_overflows counts the chip's FIFO losses too
    if fifo_only > 0 and before.load > 0.7:
        pytest.xfail(f"{what}: the sample ring overflowed {fifo_only} times; the DSP is at {before.load} of its "
                     f"core at rest, and a stream adds about 0.15 ({d})")
    assert fifo_only <= 0, f"{what}: the sample ring overflowed, DSP load {before.load} at rest ({d})"
    if 0 < d["fifo_losses"] <= 2:
        pytest.xfail(f"{what}: {d['fifo_losses']} chip FIFO loss of about 1 KB; the reader's turn is 1.1 ms of the "
                     f"FIFO's 1.33 ms with a transmit source set, so a little more on core 0 is a loss ({d})")
    assert d["fifo_losses"] == 0, f"{what}: the chip's FIFO overflowed ({d})"
    return d


def test_log_reads_stop_nothing():
    """Reading the log (256 KB out of PSRAM) keeps interrupts on: no stall, no FIFO loss."""
    before = Counters()
    for _ in range(5):
        code, _, body = request("GET", "/api/log")
        assert code == 200 and body
    check(before, "5 log reads")


def test_wideband_stream_alone(info):
    """A wideband stream long enough for a watchdog or a deferred save to land in it."""
    before = Counters()
    streams({"wide": WIDE})
    check(before, "a wideband stream")


def test_wideband_stream_with_status_polling(info):
    """The page polls /api/status while a stream runs: every poll answers, and the reader loses nothing to them."""
    before = Counters()
    stop = threading.Event()
    slow = []

    def poll():
        while not stop.is_set():
            t = time.time()
            code, _, _ = request("GET", "/api/status")
            slow.append((code, time.time() - t))
            time.sleep(0.2)

    t = threading.Thread(target=poll)
    t.start()
    try:
        streams({"wide": WIDE})
    finally:
        stop.set()
        t.join()
    assert all(code == 200 for code, _ in slow), slow
    # core 0 is full during a wideband stream (the reader, Wi-Fi, lwIP above the web server): a poll takes 0.5 to
    # 2.5 s then, and one of 5.8 s was seen in a board's first minute (W12 #1, lr2021-iq.md R3.59)
    took = sorted(s for _, s in slow)
    assert took[len(took) // 2] < 2, f"half the status polls took over {took[len(took) // 2]:.1f} s during a stream"
    assert took[-1] < 10, f"a status poll took {took[-1]:.1f} s during a stream"
    check(before, "a wideband stream with status polls")


def test_wideband_and_channel_streams(info):
    if not info["channels"]:
        pytest.skip("no channels")
    before = Counters()
    streams({"wide": WIDE, "ch": {"freq_hz": info["channels"][0]["freq_hz"], "fmt": "cs16"}})
    check(before, "a wideband and a channel stream")


def stalled_client(query, hold):
    """Open a stream's WebSocket with a small receive buffer and never read it; close after `hold` seconds."""
    host, _, port = BOARD.replace("http://", "").partition(":")
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    s.settimeout(5)
    s.connect((host, int(port or 80)))
    path = API + "/iq?" + urllib.parse.urlencode(query)
    s.sendall((f"GET {path} HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
    time.sleep(hold)
    s.close()


def test_client_that_stops_reading(info):
    """A client that opens a channel stream and never reads it doesn't hold up another client's stream, the status
    page, or the receiver; its stream slot is free again once it's gone."""
    if not info["channels"]:
        pytest.skip("no channels")
    ch = {"freq_hz": info["channels"][0]["freq_hz"], "fmt": "cs16"}
    before = Counters()
    t0 = time.time()
    out = streams({"wide": WIDE}, also=lambda: stalled_client(dict(ch, seconds=SECONDS), SECONDS))
    took = time.time() - t0
    assert took < SECONDS * 4 + 10, f"the reading client's stream took {took:.0f} s beside a stalled one"
    assert out["wide"][-1].get("samples", 0) > 0
    t = time.time()
    get_json("/api/status")
    assert time.time() - t < 3
    check(before, "a stream beside a client that stopped reading")
    deadline = time.time() + 15
    while get_json(API + "/status")["streams"] and time.time() < deadline:
        time.sleep(0.5)
    assert get_json(API + "/status")["streams"] == [], "the stalled client's stream is still open"


def test_second_wideband_stream_is_refused_cleanly(info):
    """One wideband stream at a time: the second is refused, and the first is whole."""
    before = Counters()
    out = {}
    first = threading.Thread(target=run_stream, args=(WIDE, out, "first"))
    first.start()
    time.sleep(1.0)
    try:
        second = text_frames(ws_messages(URL, API + "/iq", dict(WIDE, seconds=1), 10))
    except Exception as e:
        second = [{"type": "error", "error": repr(e)}]
    first.join()
    xfail_slow_link(out["first"], RATES.get("first", 0), "the first wideband stream")
    assert out["first"][-1]["type"] == "done", out["first"][-1:]
    assert not second or second[-1]["type"] != "done" or second[-1].get("samples", 0) == 0 or \
        get_json(API + "/info")["limits"].get("wide_streams", 1) > 1, json.dumps(second)[:300]
    check(before, "a refused second wideband stream")


def test_pings_do_not_break_frames(info):
    """A client that pings while a stream runs gets every pong and every frame whole. The server's own pong used to
    go out from its task while the stream's task was mid-frame on the same socket: it landed inside the frame, and
    the stream ended there for a client that pings every 20 s (lr2021-iq.md R3.59)."""
    import urllib.parse as up
    from websockets.exceptions import ConnectionClosed
    from websockets.sync.client import connect
    from test_board import iq_header
    u = up.urlsplit(URL)
    url = f"ws://{u.netloc}{API}/iq?{up.urlencode(dict(WIDE, seconds=4))}"
    texts, seqs, pongs = [], [], []
    got, t0 = 0, time.monotonic()
    with connect(url, open_timeout=10, max_size=None, ping_interval=None) as ws:
        deadline = time.monotonic() + 40
        next_ping = time.monotonic()
        try:
            while time.monotonic() < deadline:
                if time.monotonic() >= next_ping:
                    try:
                        pongs.append(ws.ping())
                    except ConnectionClosed:
                        pass  # the server closed after its done frame: what it sent is still to be read
                    next_ping = time.monotonic() + 0.2
                m = ws.recv(timeout=10)
                if isinstance(m, str):
                    texts.append(json.loads(m))
                else:
                    h = iq_header(m)  # raises on a frame that doesn't start with the magic
                    seqs.append((h["sc"], h["n"], h["flags"]))
                    got += len(m)
        except (ConnectionClosed, TimeoutError):
            pass
    xfail_slow_link(texts, got / max(time.monotonic() - t0, 1e-3), "a wideband stream with pings")
    assert texts and texts[-1]["type"] == "done", texts[-1:]
    # a frame that doesn't start where the last one ended says so (flag 1, gap): a W12 sends a wideband stream's
    # frames as fast as Wi-Fi takes them and drops the rest whole
    for a, b in zip(seqs, seqs[1:]):
        assert b[0] >= a[0] + a[1], "frames out of order"
        assert (b[0] == a[0] + a[1]) or (b[2] & 1), f"samples missing before sc {b[0]} with no gap flag"
    assert sum(n for _, n, _ in seqs) == texts[-1]["samples"]
    assert texts[-1]["gaps"] == sum(1 for _, _, f in seqs if f & 1)
    assert len(pongs) >= 10
    answered = sum(1 for p in pongs if p.is_set())
    assert answered >= len(pongs) - 3, f"{answered} of {len(pongs)} pings answered"
