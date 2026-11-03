#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
"""
The bench's line protocol client (docs/spec/bench-api.md section 14): the bench daemon's client, against a gateway
(a board, or rdofradio on an SDR) over TCP. Same subcommands, options, output and exit codes, so a tool written for
the daemon's client only changes the command it runs.

    rfgbench.py [--bench HOST[:PORT]] info [--json]
    rfgbench.py ... status [--json]
    rfgbench.py ... session --label L [--expect S] [--max S]     prints {"session": ID}; held until stdin closes
    rfgbench.py ... claim --session ID MHz... [--caps rx,dmr] [--wait-max S]
    rfgbench.py ... release --session ID [MHz...] [--hold ID] [--tune]
    rfgbench.py ... tune --session ID [--center MHz|auto] [--gain dB|auto] [--taps MHz...]   the session's own window
    rfgbench.py ... hold --session ID [--expect S] [--max S] [--wait-max S]
    rfgbench.py ... run [--session ID] [--modem] [--expect S] [--kind K] -- [python] bench_tx.py ARGS...
    rfgbench.py ... capture [--session ID] --freq MHz --seconds S --out FILE [--pre S] [--gain dB] [--offset-khz K]
    rfgbench.py ... dmr-bursts --freq MHz [--seconds S] [--bursts N] [--until-end] [--cc CC] [--out FILE]
    rfgbench.py ... listen --freq MHz [--seconds S]
    rfgbench.py ... capture-trigger --freq MHz --out FILE [--bw Hz] [--pre S] [--post S] [--timeout S] ...
    rfgbench.py ... tx fm --freq MHz [SEGMENT...] [--pcm FILE] [--ctcss HZ | --dcs 023N] [--dev HZ] [--sub-dev HZ] ...
    rfgbench.py ... rx fm --freq MHz --seconds S [--audio FILE.wav] [--disc FILE.wav] [--records FILE] [--bw HZ] ...

A tx fm SEGMENT is KIND:ARG:ARG..., with key=value arguments and @LEVEL anywhere (level 0-1 of the deviation):
tone:1000:2 (Hz, seconds; 1000+1500 for two tones), sweep:300-3000:5[:log], dtmf:123#[:0.1] (seconds a digit),
silence:0.5, noise:1, afsk:TEXT or afsk:hex=C0FF (Bell 202, 1200 baud; baud=300:hz=1600+1800 for others), pcm (where
the --pcm audio goes; without it the PCM is the program). --program JSON gives the segments as bench-api.md's list.

The gateway: --bench, else $RDOF_BENCH_LINE (HOST or HOST:PORT; port 7373 by default). Any number of clients may use
one gateway at once: sessions, claims and the modem queue keep them apart, and the receive ops share the receiver.

Exit codes: 0; 1 an error; 3 nothing heard; 69 the gateway can't be reached or can't do it; 75 busy or timed out;
77 refused.
"""
from __future__ import annotations

import argparse
import json
import os
import select
import shlex
import signal
import socket
import sys
import threading
import time
import wave
from typing import Callable, Iterator, Optional, Tuple

PORT = 7373
EX_NO_SIGNAL, EX_UNAVAILABLE, EX_TEMPFAIL, EX_NOPERM = 3, 69, 75, 77
EXIT = {"timeout": EX_TEMPFAIL, "busy": EX_TEMPFAIL, "deadlock": EX_TEMPFAIL, "refused": EX_NOPERM,
        "no_signal": EX_NO_SIGNAL, "unavailable": EX_UNAVAILABLE, "unsupported": EX_UNAVAILABLE}
NOTE = "rfoundry-bench:"      # the daemon client's prefix, which the HIL tools look for


class Gone(Exception):
    pass


def address(given: Optional[str]) -> Tuple[str, int]:
    spec = given or os.environ.get("RDOF_BENCH_LINE")
    if not spec:
        raise Gone("no gateway: give --bench HOST[:PORT] or set RDOF_BENCH_LINE")
    host, _, port = spec.rpartition(":") if spec.count(":") == 1 else (spec, "", "")
    return (host or spec), int(port) if port else PORT


class Conn:
    """One request: a JSON line out, JSON lines (and binary payloads) back. Closing it cancels the request."""

    def __init__(self, where: Tuple[str, int]) -> None:
        try:
            self.s = socket.create_connection(where, timeout=10)
        except OSError as e:
            raise Gone(f"the gateway at {where[0]}:{where[1]} doesn't answer ({e})")
        self.s.settimeout(None)
        # a board that restarts or drops off the network mid-request ends it (else the read waits for ever)
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        for opt, v in (("TCP_KEEPIDLE", 10), ("TCP_KEEPALIVE", 10), ("TCP_KEEPINTVL", 5), ("TCP_KEEPCNT", 3)):
            if hasattr(socket, opt):
                try:
                    self.s.setsockopt(socket.IPPROTO_TCP, getattr(socket, opt), v)
                except OSError:
                    pass
        self.f = self.s.makefile("rb")

    def send(self, req: dict) -> None:
        self.s.sendall((json.dumps(req) + "\n").encode())

    def lines(self) -> Iterator[Tuple[dict, Optional[bytes]]]:
        """(line, payload): payload is the bytes a line with "bytes" announces, else None."""
        for raw in self.f:
            d = json.loads(raw)
            data = None
            if "bytes" in d:
                data = self.f.read(d["bytes"])
                if len(data) < d["bytes"]:
                    return
            yield d, data

    def end(self) -> None:
        """From another thread: the reads see the end of the stream."""
        try:
            self.s.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass

    def close(self) -> None:
        for x in (self.f, self.s):
            try:
                x.close()
            except OSError:
                pass


def describe_wait(d: dict) -> str:
    who = [f"{b.get('label') or b.get('session')} ({b.get('kind', '')}, {b.get('since_s', 0):.0f} s in, "
           f"~{b.get('remaining_s', 0):.0f} s left)" for b in d.get("blockers", [])]
    ahead = d.get("ahead", 0)
    return (f"{NOTE} waiting for the {d.get('kind')}: about {d.get('eta_s', 0):.0f} s (at most "
            f"{d.get('worst_s', 0):.0f} s), {'%d queued ahead; ' % ahead if ahead else ''}"
            f"held by {', '.join(who) or 'requests ahead'}")


def request(where, req: dict) -> dict:
    c = Conn(where)
    try:
        c.send(req)
        for d, _ in c.lines():
            return d
        return {"error": "no reply", "code": "internal"}
    finally:
        c.close()


def stream(where, req: dict, on_line: Callable[[dict, Optional[bytes]], Optional[int]],
           hold: Optional[dict] = None) -> int:
    """Send req; waits and notes to stderr, every other line to on_line (which may return an exit code). hold: gets
    the connection as hold["conn"], for a deadline to end it."""
    c = Conn(where)
    if hold is not None:
        hold["conn"] = c

    def stop(signum, frame):
        c.close()
        sys.exit(128 + signum)
    for sg in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
        signal.signal(sg, stop)
    try:
        c.send(req)
        for d, data in c.lines():
            if "error" in d:
                print(f"{NOTE} {d['error']}", file=sys.stderr, flush=True)
                return EXIT.get(d.get("code"), 1)
            if d.get("state") == "queued":
                print(describe_wait(d), file=sys.stderr, flush=True)
                continue
            if d.get("state") == "granted":
                print(f"{NOTE} got the {d.get('kind')} after {d.get('waited_s', 0):.0f} s", file=sys.stderr, flush=True)
                continue
            if "note" in d:
                if d["note"]:
                    print(f"{NOTE} {d['note']}", file=sys.stderr, flush=True)
                if len(d) == 1:
                    continue
            rc = on_line(d, data)
            if rc is not None:
                return rc
        return 0
    finally:
        c.close()


def printer(d, data):
    print(json.dumps(d), flush=True)


# ── commands ──

def cmd_info(a) -> int:
    d = request(a.where, {"op": "info"})
    if a.json or "error" in d:
        print(json.dumps(d, indent=1))
        return 0 if "error" not in d else 1
    w = d["window"]
    if not w.get("enabled", True):
        print("the receiver isn't running: leases and the modem only")
    else:
        print(f"shared RX window {w['lo']:.4f}-{w['hi']:.4f} MHz (center {w['center']:.5f}, {w['fs'] / 1e6:.3f} MS/s, "
              f"gain {w['gain']} dB): {d['sdr']}")
    print(f"{'MHz':>10} {'ch':>3} {'window':6} {'caps':34} held by")
    for ch in d["channels"]:
        held = ch["claimed_by"]
        held = f"{held['label'] or held['session']} ({held['session']})" if held else "-"
        print(f"{ch['freq']:10.4f} {ch['ch'] if ch['ch'] is not None else '':>3} "
              f"{'yes' if ch['in_window'] else 'no':6} {','.join(ch['caps']):34} {held}")
    m = d["modem"]
    st = next((s for s in d.get("stations", []) if s["name"] == "modem"), {})
    print(f"modem: {'held by ' + ', '.join(h['label'] or h['session'] for h in m['held_by']) if m['held_by'] else 'free'}"
          f"; {m['waiting']} waiting" + ("" if st.get("tx") else f"; can't transmit: {st.get('why_not')}"))
    dev = d.get("device", {})
    print(f"device: {dev.get('kind')} {dev.get('name')} {dev.get('version')}")
    return 0


def cmd_status(a) -> int:
    d = request(a.where, {"op": "status"})
    if a.json or "error" in d:
        print(json.dumps(d, indent=1))
        return 0 if "error" not in d else 1
    print(f"SDR: {d['sdr']}")
    r = d.get("receiver") or {}
    if r:
        print(f"  {r['seconds']:.0f} s, {r['overflows']} overflows, clipping {r['clip_pct']} %, "
              f"dsp skipped {r['dsp_skipped']}, decode dropped {r['decode_dropped']}, {r['captures']} captures")
    print("held:")
    for i in d["held"]:
        print(f"  #{i['id']} {i['kind']:8} {i.get('label') or i['session']:30} {i.get('since_s', 0):6.0f} s in, "
              f"~{i.get('remaining_s', 0):.0f} s left  {json.dumps({k: i[k] for k in ('freq',) if k in i})}")
    print("waiting:")
    for i in d["waiting"]:
        print(f"  #{i['id']} {i['kind']:8} {i.get('label') or i['session']:30} pos {i.get('position')}, "
              f"{i.get('since_s', 0):.0f} s waiting")
    print("sessions:")
    for s in d["sessions"]:
        print(f"  {s['session']} {s['label']:30} {s['since_s']:6.0f} s  claims {','.join(s['claims']) or '-'}")
    return 0


def cmd_session(a) -> int:
    c = Conn(a.where)
    c.send({"op": "session", "label": a.label, "expect_s": a.expect, "max_s": a.max})
    first = next(c.lines(), (None, None))[0]
    if first is None or "error" in first:
        print(json.dumps(first), file=sys.stderr)
        return 1
    print(json.dumps(first), flush=True)
    for sg in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
        signal.signal(sg, lambda *_: (c.close(), sys.exit(0)))
    # until stdin closes (the caller is done, or its ssh went away) or the gateway ends the session
    while True:
        r, _, _ = select.select([sys.stdin, c.s], [], [])
        if c.s in r:
            line = c.f.readline()
            if line:
                print(line.decode().strip(), file=sys.stderr)
            break
        if sys.stdin in r and not os.read(sys.stdin.fileno(), 4096):
            break
    c.close()
    return 0


def cmd_claim(a) -> int:
    return stream(a.where, {"op": "claim", "session": a.session, "freqs": a.freqs,
                            "caps": [x for x in (a.caps or "").split(",") if x], "wait_max": a.wait_max}, printer)


def cmd_release(a) -> int:
    req = {"op": "release", "session": a.session, "freqs": a.freqs, "hold": a.hold}
    if a.tune:
        req["tune"] = True
    return stream(a.where, req, printer)


def _mhz_or_auto(x: str):
    return x if x == "auto" else float(x)


def cmd_tune(a) -> int:
    req = {"op": "tune", "session": a.session}
    for k in ("center", "gain", "taps"):
        if getattr(a, k) is not None:
            req[k] = getattr(a, k)
    return stream(a.where, req, printer)


def cmd_hold(a) -> int:
    return stream(a.where, {"op": "hold", "session": a.session, "expect_s": a.expect, "max_s": a.max,
                            "label": a.label, "wait_max": a.wait_max, "station": a.station}, printer)


def cmd_listen(a) -> int:
    def on(d, data):
        if "burst" in d:
            print(json.dumps(d["burst"]), flush=True)
    return stream(a.where, {"op": "listen", "freq": a.freq, "seconds": a.seconds, "session": a.session}, on)


def cmd_dmr_bursts(a) -> int:
    if a.file:
        print(f"{NOTE} --file: analyze a saved capture with the bench's own tools; the gateway decodes live only",
              file=sys.stderr)
        return EX_UNAVAILABLE
    out = open(a.out, "w") if a.out else sys.stdout
    human = sys.stdout if a.out else sys.stderr
    n = [0]
    hold = {"done": False}
    timer = []

    def on(d, data):
        if "ready" in d:
            t0 = d["ready"] or time.time()
            print(f"READY {time.strftime('%H:%M:%S', time.localtime(t0))}.{int(t0 * 1000) % 1000:03d} t0={t0:.3f}",
                  file=sys.stderr, flush=True)
            if a.seconds and not a.until_end:     # the gateway ends it at --seconds; this is in case it doesn't
                timer.append(threading.Timer(a.seconds + 5.0, lambda: hold["conn"].end()))
                timer[0].daemon = True
                timer[0].start()
        elif "burst" in d:
            n[0] += 1
            out.write(json.dumps(d["burst"]) + "\n")
            out.flush()
        elif "done" in d:
            s = d["done"]
            hold["done"] = True
            out.write(json.dumps({"summary": s}) + "\n")
            out.flush()
            kinds = ", ".join(f"{v} {k}" for k, v in sorted(s.get("kinds", {}).items()))
            print(f"{s['bursts']} bursts on {s['freq'] / 1e6:.5f} MHz ({s['reason']})" + (f": {kinds}" if kinds else ""),
                  file=human, flush=True)
            return 0 if s["bursts"] else EX_NO_SIGNAL
        return None
    try:
        rc = stream(a.where, {"op": "bursts", "session": a.session, "freq": a.freq, "seconds": a.seconds,
                              "max_bursts": a.bursts, "until_end": a.until_end, "cc": a.cc, "soft": a.soft,
                              "wait_max": a.wait_max}, on, hold)
        if not hold["done"] and rc == 0:          # the stream ended with no summary: one from here
            s = {"bursts": n[0], "freq": a.freq * 1e6, "reason": "the gateway stopped answering"
                 if timer else "the connection ended"}
            out.write(json.dumps({"summary": s}) + "\n")
            out.flush()
            print(f"{n[0]} bursts on {a.freq:.5f} MHz ({s['reason']})", file=human, flush=True)
            rc = 0 if n[0] else EX_NO_SIGNAL
        return rc
    finally:
        for t in timer:
            t.cancel()
        if a.out:
            out.close()


def write_iq(a, req: dict, fmt_bytes: int) -> int:
    """capture / capture-trigger: the samples into a.out by their sample clock (a gap left as zeros), FILE.json after."""
    out = os.path.abspath(os.path.expanduser(a.out))
    f = open(out, "wb")
    st = {"sc0": None, "pos": 0}

    def on(d, data):
        if "ready" in d or "armed" in d:
            if d.get("sc0") is not None:
                st["sc0"] = d["sc0"]
            t0 = d.get("ready") or time.time()
            if "ready" in d:
                print(f"READY {time.strftime('%H:%M:%S', time.localtime(t0))}.{int(t0 * 1000) % 1000:03d} t0={t0:.3f} "
                      f"({'shared' if d.get('shared') else 'exclusive'}, tuned {d.get('center', 0):.5f}, "
                      f"pre-roll {d.get('pre_s', 0):.1f} s)", flush=True)
            else:
                print(f"ARMED floor {d['armed'].get('floor_db')} dB", flush=True)
        elif "iq" in d and data is not None:
            sc = d["iq"]["sc"]
            if st["sc0"] is None:
                st["sc0"] = sc
            at = (sc - st["sc0"]) * fmt_bytes
            if at > st["pos"]:
                f.write(b"\0" * (at - st["pos"]))        # a gap: zeros, so file position stays sample index
                st["pos"] = at
            f.write(data)
            st["pos"] += len(data)
        elif "done" in d:
            m = d["done"]
            end = m.get("samples", 0) * fmt_bytes
            if end > st["pos"]:
                f.write(b"\0" * (end - st["pos"]))      # a gap at the end, or no samples at all
            f.close()
            with open(out + ".json", "w") as j:
                json.dump(m, j, indent=1)
            print(f"wrote {m['seconds']:.2f} s of {m['freq'] / 1e6:.5f} MHz (tuned {m['center'] / 1e6:.5f}) to {out}"
                  f" ({m.get('overflows', 0)} overflows, ADC clipping {m.get('clip_pct', 0):.2f} %)", flush=True)
            if m.get("clip_pct", 0) > 1.0:
                print(f"WARNING: {m['clip_pct']:.1f} % of I/Q samples at the ADC rails: the capture is saturated",
                      flush=True)
            return 0
        return None
    try:
        rc = stream(a.where, req, on)
    finally:
        if not f.closed:
            f.close()
    return rc


def cmd_capture(a) -> int:
    req = {"op": "capture", "session": a.session, "freq": a.freq, "seconds": a.seconds, "pre_s": a.pre,
           "quiet": a.quiet, "mode": a.mode, "offset_khz": a.offset_khz, "wait_max": a.wait_max, "label": a.label}
    if a.gain is not None:
        req["gain"] = a.gain
    return write_iq(a, req, 2)


def cmd_capture_trigger(a) -> int:
    req = {"op": "trigger", "session": a.session, "freq": a.freq, "bw": a.bw, "pre_s": a.pre, "post_s": a.post,
           "threshold_db": a.threshold, "min_ms": a.min_ms, "timeout_s": a.timeout, "fmt": a.fmt,
           "until_end": a.until_end, "max_s": a.max_s, "floor_db": a.floor_db, "floor_s": a.floor_s}
    return write_iq(a, req, 8 if a.fmt == "cf32" else 4)


# ── signal modes: tx / rx (bench-api.md section 14.6) ──

def _num(x: str) -> float:
    try:
        return float(x)
    except ValueError:
        raise ValueError(f"{x!r} isn't a number")


def _hz(x: str):
    v = [_num(h) for h in x.replace("-", "+").split("+")]
    return v[0] if len(v) == 1 else v


def segment(spec: str) -> dict:
    """One tx fm SEGMENT (KIND:ARG...) as a program entry."""
    level = None
    if "@" in spec:
        spec, _, lv = spec.rpartition("@")
        level = _num(lv)
    kind, *args = spec.split(":")
    kv = dict(x.split("=", 1) for x in args if "=" in x)
    pos = [x for x in args if "=" not in x]
    seg: dict = {"kind": kind}
    if kind == "tone":
        if not pos:
            raise ValueError("tone:HZ[:S]")
        seg["hz"] = _hz(pos[0])
        if len(pos) > 1:
            seg["s"] = _num(pos[1])
    elif kind == "sweep":
        if not pos or "-" not in pos[0]:
            raise ValueError("sweep:FROM-TO[:S][:log]")
        seg["hz"] = [_num(h) for h in pos[0].split("-")]
        for x in pos[1:]:
            if x == "log":
                seg["log"] = True
            else:
                seg["s"] = _num(x)
    elif kind == "dtmf":
        if not pos:
            raise ValueError("dtmf:DIGITS[:S]")
        seg["digits"] = pos[0]
        if len(pos) > 1:
            seg["s"] = _num(pos[1])
    elif kind in ("silence", "noise"):
        if pos:
            seg["s"] = _num(pos[0])
    elif kind == "pcm":
        pass
    elif kind in ("afsk", "fsk"):
        seg["kind"] = "fsk"
        if "hex" in kv:
            seg["hex"] = kv.pop("hex")
        elif pos:
            seg["text"] = ":".join(pos)
        else:
            raise ValueError("afsk:TEXT or afsk:hex=HEX")
    else:
        raise ValueError(f"no segment kind {kind!r} (tone, sweep, dtmf, silence, noise, afsk, pcm)")
    for k, v in kv.items():
        if k == "hz":
            seg["hz"] = _hz(v)
        elif k in ("nrzi", "lsb", "log"):
            seg[k] = v not in ("0", "false", "no")
        elif k in ("text", "hex", "digits"):
            seg[k] = v
        else:
            seg[k] = _num(v)
    if level is not None:
        seg["level"] = level
    return seg


def read_pcm(path: str, rate: Optional[float]) -> Tuple[bytes, float]:
    """A WAV (its first channel), or raw int16 LE mono at rate: (int16 LE bytes, rate)."""
    if path.lower().endswith(".wav"):
        with wave.open(path, "rb") as w:
            ch, width, fs = w.getnchannels(), w.getsampwidth(), w.getframerate()
            raw = w.readframes(w.getnframes())
        if width not in (1, 2):
            raise ValueError(f"{path}: {8 * width}-bit WAV (16 or 8 bit PCM only)")
        n = len(raw) // (width * ch)
        if width == 2:
            pcm = b"".join(raw[i * 2 * ch:i * 2 * ch + 2] for i in range(n)) if ch > 1 else raw[:2 * n]
        else:   # unsigned 8 bit
            pcm = b"".join(((b - 128) << 8).to_bytes(2, "little", signed=True) for b in raw[::ch][:n])
        return pcm, float(rate or fs)
    with open(path, "rb") as f:
        raw = f.read()
    return raw[:len(raw) & ~1], float(rate or 8000)


def cmd_tx_fm(a) -> int:
    try:
        prog = json.loads(a.program) if a.program else [segment(x) for x in a.segments]
    except (ValueError, TypeError) as e:
        print(f"{NOTE} {e}", file=sys.stderr)
        return 1
    req: dict = {"op": "tx", "mode": "fm", "freq": a.freq, "session": a.session, "wait_max": a.wait_max,
                 "label": a.label, "dev_hz": a.dev}
    for k, v in (("power", a.power), ("ctcss", a.ctcss), ("dcs", a.dcs), ("sub_dev_hz", a.sub_dev),
                 ("sub_end_s", a.sub_end), ("preemph_us", a.preemph_us), ("lead_s", a.lead), ("tail_s", a.tail),
                 ("repeat", a.repeat), ("seed", a.seed)):
        if v is not None:
            req[k] = v
    if prog:
        req["program"] = prog
    pcm = b""
    if a.pcm:
        try:
            pcm, rate = read_pcm(a.pcm, a.pcm_rate)
        except (OSError, ValueError, wave.Error) as e:
            print(f"{NOTE} {e}", file=sys.stderr)
            return 1
        req["pcm"] = {"rate": rate, "samples": len(pcm) // 2}
    elif any(s.get("kind") == "pcm" for s in prog):
        print(f"{NOTE} a pcm segment needs --pcm FILE", file=sys.stderr)
        return 1
    hold: dict = {}

    def send_pcm():    # TCP's flow control paces it: the gateway reads as its buffer empties
        try:
            hold["conn"].s.sendall(pcm)
        except OSError:
            pass

    def on(d, data):
        if "tx" in d and pcm:
            threading.Thread(target=send_pcm, daemon=True).start()
        print(json.dumps(d), flush=True)
        if "done" in d:
            return 1 if d["done"].get("cut") else 0
        return None
    return stream(a.where, req, on, hold)


class _Wav:
    def __init__(self, path: str, rate: float) -> None:
        self.w = wave.open(path, "wb")
        self.w.setnchannels(1)
        self.w.setsampwidth(2)
        self.w.setframerate(int(round(rate)))
        self.rate = rate

    def zeros(self, n: int) -> None:
        self.w.writeframes(b"\0\0" * n)


def cmd_rx_fm(a) -> int:
    streams = [x for x, f in (("audio", a.audio), ("disc", a.disc)) if f]
    req: dict = {"op": "rx", "mode": "fm", "freq": a.freq, "seconds": a.seconds, "session": a.session,
                 "wait_max": a.wait_max, "label": a.label, "streams": ",".join(streams)}
    for k, v in (("bw", a.bw), ("squelch_dbfs", a.squelch_dbfs), ("squelch_over_db", a.squelch_over),
                 ("deemph_us", a.deemph_us), ("block_s", a.block_s), ("nominal_dev_hz", a.nominal_dev),
                 ("spectrum_hz", a.spectrum_hz), ("audio_full_hz", a.audio_full_hz)):
        if v is not None:
            req[k] = v
    files = {"audio": a.audio, "disc": a.disc}
    st: dict = {"wavs": {}, "rate": None}
    rec = open(a.records, "w") if a.records else None

    def on(d, data):
        if "rx" in d:
            h = d["rx"]
            st["rate"] = h.get("rate")
            for sd in h.get("streams", []):
                if files.get(sd["name"]):
                    st["wavs"][sd["name"]] = _Wav(files[sd["name"]], sd["rate"])
            print(json.dumps(d), flush=True)
            return None
        if "pcm" in d and data is not None:
            w = st["wavs"].get(d["pcm"]["stream"])
            if w:
                w.w.writeframes(data)
            return None
        if "gap" in d and "samples" in d["gap"] and st["rate"]:     # lost: zeros keep the audio's timing
            for w in st["wavs"].values():
                w.zeros(int(round(d["gap"]["samples"] * w.rate / st["rate"])))
        if rec:
            rec.write(json.dumps(d) + "\n")
        if "rec" in d and not a.quiet:
            print(json.dumps(d), flush=True)
        elif "rec" not in d:
            print(json.dumps(d), flush=True)
        if "done" in d:
            return 0 if d["done"].get("samples") else EX_NO_SIGNAL
        return None
    try:
        return stream(a.where, req, on)
    finally:
        for w in st["wavs"].values():
            w.w.close()
        if rec:
            rec.close()


def cmd_unsupported(a) -> int:
    return stream(a.where, {"op": a.cmd_name}, printer)


# ── run: the bench tools' command lines, as jobs ──

def _tool_index(argv) -> int:
    for i, x in enumerate(argv):
        if os.path.basename(x) in ("bench_tx.py", "bench_repeater.py"):
            return i
    return -1


def bench_tx_parser() -> argparse.ArgumentParser:
    """bench_tx.py's options (its --help is the reference); the gateway refuses the ones it can't honor."""
    ap = argparse.ArgumentParser(prog="bench_tx.py", add_help=False)
    ap.add_argument("--port")
    ap.add_argument("--freq", type=float, default=438.5)
    ap.add_argument("--cc", type=int, default=2)
    ap.add_argument("--power", type=int, default=255)
    ap.add_argument("--rx-offset", type=int)
    ap.add_argument("--tx-offset", type=int)
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--listen-after", type=float, default=0)
    ap.add_argument("--ambe-out")
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p, dst):
        p.add_argument("--src", type=int, default=9990301)
        p.add_argument("--dst", type=int, default=dst)
        p.add_argument("--family", choices=("ms", "bs", "ts1", "ts2"), default="ms")
        p.add_argument("--burst-cc", type=int)
    v = sub.add_parser("voice", add_help=False)
    common(v, 215)
    v.add_argument("--private", action="store_true")
    v.add_argument("--superframes", type=int, default=3)
    v.add_argument("--headers", type=int)
    v.add_argument("--terminators", type=int)
    v.add_argument("--late", type=int)
    v.add_argument("--calls", type=int, default=1)
    v.add_argument("--gap-ms", type=int, default=120)
    v.add_argument("--src2", type=int, default=9990302)
    v.add_argument("--ambe")
    v.add_argument("--talker-alias")
    v.add_argument("--gps")
    v.add_argument("--pi", action="store_true")
    v.add_argument("--service-options")
    v.add_argument("--emergency", action="store_true")
    c = sub.add_parser("csbk", add_help=False)
    common(c, 9801276)
    c.add_argument("--count", type=int, default=4)
    c.add_argument("--opcode")
    c.add_argument("--fid", default="0")
    c.add_argument("--octet3", default="0")
    d = sub.add_parser("data", add_help=False)
    common(d, 9801276)
    for o in ("--confirmed", "--tms-no-ack", "--etsi-no-response", "--group"):
        d.add_argument(o, action="store_true")
    for o in ("--tms", "--etsi", "--ip-text", "--udp-payload"):
        d.add_argument(o)
    for o, dflt in (("--octets", 40), ("--tms-seq", 1), ("--udp-port", None), ("--udp-src-port", None),
                    ("--udp-octets", 40), ("--preambles", 2), ("--repeat", 1), ("--repeat-gap-ms", 0)):
        d.add_argument(o, type=int, default=dflt)
    ob = sub.add_parser("outbound", add_help=False)     # a repeater's downlink: BS idle bursts, the CACH, both slots
    ob.add_argument("--seconds", type=float, default=10)
    ob.add_argument("--src", type=int)
    ob.add_argument("--slots", default="1,2")            # the slots reported (both are sent)
    ob.add_argument("--voice-slot", type=int)            # refused by the gateway: idle bursts only for now
    ob.add_argument("--family", choices=("ms", "bs", "ts1", "ts2"))
    ls = sub.add_parser("listen", add_help=False)
    ls.add_argument("--seconds", type=float, default=20)
    ls.add_argument("--json", action="store_true")
    sub.add_parser("selftest", add_help=False)
    return ap


def bench_repeater_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="bench_repeater.py", add_help=False)
    ap.add_argument("--rx", type=float, required=True)
    ap.add_argument("--tx", type=float, required=True)
    ap.add_argument("--cc", type=int, default=2)
    ap.add_argument("--power", type=int, default=255)
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--slots", default="1,2")
    ap.add_argument("--hang", type=float, default=4.0)
    ap.add_argument("--call-hang", type=float, default=3.0)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--beacon", type=float, default=0.0)     # seconds between idle beacons (0: none)
    ap.add_argument("--beacon-len", type=float, default=4.0)
    ap.add_argument("--beacon-phase", type=float, default=2.0)
    ap.add_argument("--rx-offset", type=float)                # refused by the gateway: its own receiver and transmitter
    ap.add_argument("--tx-offset", type=float)
    ap.add_argument("--port")
    return ap


def job_of(argv) -> Tuple[Optional[dict], str]:
    """The job a bench tool's command line describes, or (None, why not)."""
    i = _tool_index(argv)
    if i < 0:
        return None, ("the gateway runs the bench tools' jobs (bench_tx.py, bench_repeater.py), not programs: "
                      + shlex.join(argv))
    tool = os.path.basename(argv[i])[:-3]
    p = bench_tx_parser() if tool == "bench_tx" else bench_repeater_parser()
    try:
        ns, extra = p.parse_known_args(argv[i + 1:])
    except SystemExit:
        return None, f"{tool}: can't read its options: {shlex.join(argv[i + 1:])}"
    if extra:
        return None, f"{tool}: options the gateway doesn't know: {' '.join(extra)}"
    job = {"tool": tool, **{k: v for k, v in vars(ns).items() if v is not None}}
    for k in ("ambe",):                                   # local files go along as their bytes
        if job.get(k):
            with open(os.path.expanduser(job[k]), "rb") as f:
                job[k + "_hex"] = f.read().hex()
    return job, ""


def cmd_run(a) -> int:
    argv = a.cmd[1:] if a.cmd and a.cmd[0] == "--" else a.cmd
    if not argv:
        print("rfgbench run: no command", file=sys.stderr)
        return 2
    if a.station:
        print(f"{NOTE} this bench has no stations (radios on its host)", file=sys.stderr)
        return EX_UNAVAILABLE
    job, why = job_of(argv)
    if job is None:
        print(f"{NOTE} {why}", file=sys.stderr)
        return EX_UNAVAILABLE
    files = {}

    def on(d, data):
        if "out" in d:
            sys.stdout.write(d["out"] + "\n")
            sys.stdout.flush()
        elif "err" in d:
            sys.stderr.write(d["err"] + "\n")
            sys.stderr.flush()
        elif "file" in d and data is not None:              # a file the job writes (--ambe-out): here
            name = os.path.join(a.cwd or os.getcwd(), d["file"])
            with open(name, "ab" if name in files else "wb") as f:
                f.write(data)
            files[name] = True
        elif "started" in d:
            print(f"{NOTE} started job {d['started']}", file=sys.stderr, flush=True)
        elif "exit" in d:
            if d.get("killed"):
                print(f"{NOTE} {d['killed']}", file=sys.stderr)
            rc = d["exit"]
            return rc if rc >= 0 else 128 - rc
        return None
    return stream(a.where, {"op": "run", "session": a.session, "job": job, "argv": argv, "modem": a.modem,
                            "kind": a.kind, "label": a.label, "expect_s": a.expect, "max_s": a.max,
                            "wait_max": a.wait_max}, on)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="rfgbench", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bench", default=None, help="HOST[:PORT] of the gateway (default $RDOF_BENCH_LINE)")
    ap.add_argument("--socket", default=None, help=argparse.SUPPRESS)      # the daemon client's; not used here
    sub = ap.add_subparsers(dest="cmd_name", required=True)

    def p(name, fn, session=True):
        s = sub.add_parser(name)
        s.set_defaults(fn=fn)
        if session:
            s.add_argument("--session", default=None)
            s.add_argument("--wait-max", type=float, default=None, help="give up after this many seconds (exit 75)")
            s.add_argument("--label", default=None)
        return s
    s = p("info", cmd_info, False)
    s.add_argument("--json", action="store_true")
    s = p("status", cmd_status, False)
    s.add_argument("--json", action="store_true")
    s = p("session", cmd_session, False)
    s.add_argument("--label", default="")
    s.add_argument("--expect", type=float, default=None)
    s.add_argument("--max", type=float, default=None)
    s = p("claim", cmd_claim)
    s.add_argument("freqs", type=float, nargs="+")
    s.add_argument("--caps", default="")
    s = p("release", cmd_release)
    s.add_argument("freqs", type=float, nargs="*")
    s.add_argument("--hold", type=int, default=None)
    s.add_argument("--tune", action="store_true", help="the session's tune: the receiver back to its settings")
    s = p("tune", cmd_tune)
    s.add_argument("--center", type=_mhz_or_auto, default=None, help="MHz, or auto (the settings' plan)")
    s.add_argument("--gain", type=_mhz_or_auto, default=None, help="dB, or auto (the settings')")
    s.add_argument("--taps", type=float, nargs="*", default=None,
                   help="receiver channels in the window, MHz (none: drop the taps)")
    s = p("hold", cmd_hold)
    s.add_argument("--station", default="modem")
    s.add_argument("--expect", type=float, default=None)
    s.add_argument("--max", type=float, default=None)
    s = p("run", cmd_run)
    s.add_argument("--modem", action="store_true")
    s.add_argument("--sdr", action="store_true")
    s.add_argument("--expect", type=float, default=None)
    s.add_argument("--max", type=float, default=None)
    s.add_argument("--kind", default=None)
    s.add_argument("--cwd", default=None)
    s.add_argument("--station", action="append")
    s.add_argument("--tx", type=float, action="append")
    s.add_argument("--mode", default=None)
    s.add_argument("cmd", nargs=argparse.REMAINDER)
    s = p("capture", cmd_capture)
    s.add_argument("--freq", type=float, required=True)
    s.add_argument("--seconds", type=float, required=True)
    s.add_argument("--out", required=True)
    s.add_argument("--gain", type=float, default=None)
    s.add_argument("--pre", type=float, default=2.0)
    s.add_argument("--quiet", action="store_true")
    s.add_argument("--mode", default="rx", choices=("rx", "dmr", "nfm", "wfm"))
    s.add_argument("--offset-khz", type=float, default=-200.0)
    s = p("listen", cmd_listen)
    s.add_argument("--freq", type=float, required=True)
    s.add_argument("--seconds", type=float, default=10.0)
    s = p("dmr-bursts", cmd_dmr_bursts)
    s.add_argument("--freq", type=float, default=None)
    s.add_argument("--seconds", type=float, default=10.0)
    s.add_argument("--bursts", type=int, default=None)
    s.add_argument("--until-end", action="store_true")
    s.add_argument("--cc", type=int, default=None)
    s.add_argument("--soft", action="store_true")
    s.add_argument("--out", default=None)
    s.add_argument("--file", default=None)
    s.add_argument("--center-mhz", type=float, default=None)
    s.add_argument("--fs", type=float, default=None)
    s = p("capture-trigger", cmd_capture_trigger)
    s.add_argument("--freq", type=float, required=True)
    s.add_argument("--out", required=True)
    s.add_argument("--bw", type=float, default=12500.0)
    s.add_argument("--pre", type=float, default=1.0)
    s.add_argument("--post", "--seconds", dest="post", type=float, default=2.0)
    s.add_argument("--timeout", type=float, default=None)
    s.add_argument("--threshold", type=float, default=10.0)
    s.add_argument("--min-ms", type=float, default=10.0)
    s.add_argument("--fmt", choices=("cf32", "cs16"), default="cf32")
    s.add_argument("--floor-s", type=float, default=1.0)
    s.add_argument("--floor-db", type=float, default=None)
    s.add_argument("--until-end", action="store_true")
    s.add_argument("--max-s", type=float, default=30.0)
    s.add_argument("--json", action="store_true")
    for op, fn in (("tx", cmd_tx_fm), ("rx", cmd_rx_fm)):
        s = sub.add_parser(op, help=f"{'send' if op == 'tx' else 'receive'} a signal mode")
        modes = s.add_subparsers(dest="mode_name", required=True)
        s = modes.add_parser("fm", help="analog FM")
        s.set_defaults(fn=fn)
        s.add_argument("--session", default=None)
        s.add_argument("--wait-max", type=float, default=None, help="give up after this many seconds (exit 75)")
        s.add_argument("--label", default=None)
        s.add_argument("--freq", type=float, required=True, help="MHz")
        if op == "tx":
            s.add_argument("segments", nargs="*", help="KIND:ARG... (see above)")
            s.add_argument("--program", default=None, help="the segments as a JSON list")
            s.add_argument("--pcm", default=None, help="audio to send: a WAV, or raw int16 LE mono")
            s.add_argument("--pcm-rate", type=float, default=None, help="raw PCM's rate (8000), or a WAV's override")
            s.add_argument("--dev", type=float, default=2500.0, help="the program's peak deviation, Hz")
            s.add_argument("--ctcss", type=float, default=None, help="tone squelch, Hz")
            s.add_argument("--dcs", default=None, help="digital squelch: octal code, N or I (023N, 023I)")
            s.add_argument("--sub-dev", type=float, default=None, help="the CTCSS/DCS deviation, Hz (0.2 of --dev)")
            s.add_argument("--sub-end", type=float, default=None, help="reverse burst / turn-off code, seconds")
            s.add_argument("--preemph-us", type=float, default=None)
            s.add_argument("--lead", type=float, default=None, help="carrier before the program, s (0.25)")
            s.add_argument("--tail", type=float, default=None, help="carrier after it, s (0.25)")
            s.add_argument("--repeat", type=int, default=None)
            s.add_argument("--seed", type=int, default=None)
            s.add_argument("--power", type=int, default=None, help="0-255, capped by the gateway's rf_level")
        else:
            s.add_argument("--seconds", type=float, required=True)
            s.add_argument("--audio", default=None, help="write the filtered audio to this WAV")
            s.add_argument("--disc", default=None, help="write the raw discriminator to this WAV")
            s.add_argument("--records", default=None, help="write every line (records, gaps, done) to this file")
            s.add_argument("--quiet", action="store_true", help="don't print the records")
            s.add_argument("--bw", type=float, default=None, help="channel bandwidth, Hz (16000)")
            s.add_argument("--squelch-dbfs", type=float, default=None, help="else learned from the noise floor")
            s.add_argument("--squelch-over", type=float, default=None, help="dB over the learned floor (10)")
            s.add_argument("--deemph-us", type=float, default=None)
            s.add_argument("--block-s", type=float, default=None, help="seconds a record (0.1)")
            s.add_argument("--nominal-dev", type=float, default=None, help="Hz, for the records' deviation ratio")
            s.add_argument("--spectrum-hz", type=float, default=None)
            s.add_argument("--audio-full-hz", type=float, default=None, help="deviation at full scale in the WAV")
    for name in ("activity", "burstlog", "bridge"):
        s = p(name, cmd_unsupported, False)
        s.add_argument("rest", nargs=argparse.REMAINDER)
    a, extra = ap.parse_known_args(argv)
    if extra and a.cmd_name not in ("activity", "burstlog", "bridge"):
        ap.error(f"unrecognized arguments: {' '.join(extra)}")
    if a.cmd_name == "dmr-bursts" and a.freq is None and not a.file:
        ap.error("dmr-bursts needs --freq")
    try:
        a.where = address(a.bench)
        return a.fn(a)
    except Gone as e:
        print(f"{NOTE} {e}", file=sys.stderr)
        return EX_UNAVAILABLE


if __name__ == "__main__":
    sys.exit(main())
