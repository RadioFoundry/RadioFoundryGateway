# SPDX-License-Identifier: MIT
"""
The bench API's client (docs/spec/bench-api.md): one interface over the two benches the bench tools use.

    b = open_bench("http://BOARD")             # an ESP32 board (esp_idf): the v1 API, else its /api/* today
    b = open_bench("daemon")                   # the bench daemon on $RDOF_BENCH_DAEMON_HOST over ssh, or its local socket
    b = open_bench("daemon://HOST")
    b.info(); b.status(); b.spectrum(); b.bursts(freq_hz=461.4625e6, seconds=10); b.capture(...)

Each backend answers in the spec's shapes (info, status, spectrum, burst records, capture metadata), so a tool
doesn't care which bench it has. What a backend can't do raises Unsupported (the spec's "unsupported" code, exit
69 in the tools), never a guess.

Backends:

- Esp32Bench: the board's v1 API (/api/bench/v1/...) when it serves one. Until the firmware does, its existing
  endpoints, read only: /api/status and /api/resolved (info, status), /api/spectrum (spectrum), and the bursts its
  SDR publishes to MQTT ({root}/{channel}/{app}/raw; broker, root and app from /api/resolved, login from
  $RDOF_BENCH_MQTT_USER / $RDOF_BENCH_MQTT_PASSWORD). No I/Q, no TX, no radio control there. With v1: tx() (WS /tx,
  a TxStream) besides the streams, sessions and radio links.
- DaemonBench: the bench daemon's protocol (one JSON request line on its unix socket, JSON lines back, closing cancels),
  through `ssh HOST python3 -c RELAY` (the relay joins ssh's stdin / stdout to the socket) or on the bench host
  itself. info, status, bursts (the daemon's `bursts` op, else `listen`), capture (the daemon's `capture` op, the
  file fetched over ssh and removed). No spectrum op in the daemon yet.

Environment: RDOF_BENCH (default bench for the tools), RDOF_BENCH_DAEMON_HOST (ssh host for "daemon"),
RDOF_BENCH_DAEMON_SOCKET (the daemon's socket path; no default), RDOF_BENCH_DAEMON_PY (python on the bench host for
the relay, default python3), RDOF_BENCH_LABEL (this session's name in the daemon's queue).
"""
from __future__ import annotations

import base64
import json
import os
import re
import shlex
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from typing import Any, Callable, Dict, Iterator, List, Optional

API = "/api/bench/v1"

# Exit codes the tools share with the bench daemon's client
EX_NO_SIGNAL, EX_UNAVAILABLE, EX_TEMPFAIL, EX_NOPERM = 3, 69, 75, 77



class BenchError(Exception):
    """An error in the spec's terms: code is bad_request, refused, busy, timeout, no_signal, unavailable,
    unsupported or internal."""

    EXIT = {"refused": EX_NOPERM, "busy": EX_TEMPFAIL, "timeout": EX_TEMPFAIL, "no_signal": EX_NO_SIGNAL,
            "unavailable": EX_UNAVAILABLE, "unsupported": EX_UNAVAILABLE}

    def __init__(self, code: str, msg: str, **extra: Any) -> None:
        super().__init__(msg)
        self.code = code
        self.extra = extra

    @property
    def exit_code(self) -> int:
        return self.EXIT.get(self.code, 1)


class Unsupported(BenchError):
    def __init__(self, what: str, bench: str) -> None:
        super().__init__("unsupported", f"{bench} can't {what}; see docs/spec/bench-api.md")


# ── burst records (spec "Bursts") ──

_KIND_BY_DT = {0x00: "PI_HDR", 0x01: "VLC_HDR", 0x02: "TERM_LC", 0x03: "CSBK", 0x04: "MBC_HDR", 0x05: "MBC_CONT",
               0x06: "DATA_HDR", 0x07: "R1/2", 0x08: "R3/4", 0x09: "IDLE", 0x0A: "R1", 0x0B: "USBD"}


# The 48-bit sync patterns (ETSI TS 102 361-1 table 9.2), by the spec's names; a data sync is its voice sync with
# every other bit flipped.
_VOICE_SYNC = {"BS": 0x755FD7DF75F7, "MS": 0x7F7D5DD57DFD, "TS1": 0x5D577F7757FF, "TS2": 0x7DFFD5F55D5F}
_DATA_XOR = 0xAAAAAAAAAAAA


def _bits(burst: bytes, start: int, n: int) -> int:
    """n bits of a 33-byte burst from bit `start` (most significant first), as an int."""
    v = int.from_bytes(burst, "big") >> (264 - start - n)
    return v & ((1 << n) - 1)


def sync_family(burst: bytes) -> Optional[str]:
    """The burst's sync as the spec names it (BS_VOICE ... TS2_DATA) when its middle field is one within 6 bit
    errors, else None (a voice burst B-F carries an EMB there)."""
    field = _bits(burst, 108, 48)
    best = (None, 7)
    for name, voice in _VOICE_SYNC.items():
        for kind, pattern in (("VOICE", voice), ("DATA", voice ^ _DATA_XOR)):
            e = bin(field ^ pattern).count("1")
            if e < best[1]:
                best = (f"{name}_{kind}", e)
    return best[0]


def raw_body(payload: bytes) -> Optional[Dict[str, Any]]:
    """A {root}/{channel}/{app}/raw message (version 1, 49 bytes, or version 2, 62: common/include/rdof/rawburst.h)
    as a dict: ms (UTC ms, 0 if unknown), slot, source, dt, n, data (the 33-byte burst), and src_id / dst_id / group
    when addressed. None if it isn't one."""
    if len(payload) not in (49, 62) or {1: 49, 2: 62}.get(payload[0]) != len(payload):
        return None
    flags, dt, n = payload[1], payload[2], payload[3]
    body: Dict[str, Any] = {"ms": int.from_bytes(payload[4:10], "little"), "slot": 2 if flags & 1 else 1,
                            "source": "network" if flags & 2 else "rf", "dt": dt, "n": n, "data": payload[16:49]}
    if flags & 4:
        body.update(src_id=int.from_bytes(payload[10:13], "little"), dst_id=int.from_bytes(payload[13:16], "little"),
                    group=bool(flags & 8))
    return body


def record_from_raw(body: Dict[str, Any], channel: Optional[str] = None,
                    freq_hz: Optional[float] = None) -> Dict[str, Any]:
    """A burst record from a raw message (raw_body): the spec's required fields that the message has. No sample
    clock, level or carrier offset: MQTT doesn't carry them. The color code is read from the slot type or EMB
    without its FEC, so a burst with bit errors there can show the wrong one."""
    raw = body["data"]
    dt = body.get("dt")
    if dt in (0xF0, 0xF1):
        kind = "VOICE_" + "ABCDEF"[int(body.get("n", 0)) % 6]
        cc = _bits(raw, 108, 4) if dt == 0xF1 else None
    else:
        kind = _KIND_BY_DT.get(dt, str(dt))
        cc = _bits(raw, 98, 4)
    ms = body.get("ms") or 0
    rec: Dict[str, Any] = {"t": ms / 1000.0 if ms else None, "sc": None, "channel": channel, "freq_hz": freq_hz,
                           "slot": body.get("slot"), "sync": sync_family(raw) or ("EMB" if dt == 0xF1 else None),
                           "kind": kind, "dt": dt, "n": body.get("n"), "cc": cc, "source": body.get("source"),
                           "raw_hex": raw.hex()}
    if "src_id" in body:
        rec.update(src=body["src_id"], dst=body["dst_id"], group=body.get("group"))
    return rec


class Bench:
    """What every backend offers; anything it can't do raises Unsupported."""

    name = "bench"

    def info(self) -> Dict[str, Any]:
        raise Unsupported("describe itself", self.name)

    def status(self) -> Dict[str, Any]:
        raise Unsupported("report its status", self.name)

    def spectrum(self, since: int = 0) -> Dict[str, Any]:
        """{"seq", "center_hz", "rate", "bins", "row_ms", "rows": [[dBFS per bin, lowest frequency first], ...]}"""
        raise Unsupported("give a spectrum", self.name)

    def spectrum_stream(self, seconds: float = 0.0) -> Iterator[Dict[str, Any]]:
        """Spectrum rows as they come: the start frame first ({"type": "start", "bins", "row_ms", "rate",
        "center_hz", ...}), then {"type": "row", "sc", "row_ms", "row": [dBFS per bin]} and {"type": "drop",
        "rows"}. seconds 0: until the caller stops."""
        raise Unsupported("stream the spectrum", self.name)

    def bursts(self, freq_hz: Optional[float] = None, channel: Optional[str] = None, seconds: float = 10.0,
               max_bursts: Optional[int] = None) -> Iterator[Dict[str, Any]]:
        raise Unsupported("stream bursts", self.name)

    def capture(self, freq_hz: float, seconds: float, out: str, **kw: Any) -> Dict[str, Any]:
        raise Unsupported("capture I/Q", self.name)

    def calls(self, freq_hz: Optional[float] = None, channel: Optional[str] = None, seconds: float = 60.0,
              ambe: bool = False) -> Iterator[Dict[str, Any]]:
        """Call records as calls start and end ({"type": "call", "event": start | late_entry | end | lost, ...}),
        the trackers' other events ({"type": "event", ...}) and, with ambe, each voice burst's AMBE+2 frames
        ({"type": "ambe", "frames": [hex x 3]})."""
        raise Unsupported("stream calls", self.name)

    def regs(self) -> Dict[str, Any]:
        raise Unsupported("dump tuner registers", self.name)

    def log(self) -> str:
        raise Unsupported("show its log", self.name)

    def radios(self) -> List[Dict[str, Any]]:
        raise Unsupported("list its radios", self.name)

    def link(self, name: str, session: Optional[str] = None, proto: Optional[str] = None) -> "RadioLink":
        raise Unsupported("link a radio", self.name)

    def session(self, label: str, ttl_s: float = 30.0) -> Dict[str, Any]:
        raise Unsupported("open a session", self.name)

    def tx(self, session: str, freq_hz: float, via: str = "modem", cc: int = 1,
           on_event: Optional[Callable[[Dict[str, Any]], None]] = None) -> "TxStream":
        raise Unsupported("transmit", self.name)

    def tune(self, name: str, session: str, freq_hz: float, mode: str = "dmr", cc: Optional[int] = None,
             slot: Optional[int] = None, tg: Optional[int] = None, private: bool = False,
             proto: Optional[str] = None) -> Dict[str, Any]:
        """Tune an ODR bench radio over its debug link (the session's lease must cover freq_hz); returns
        {"radio", "freq_hz", "mode", "rx_hz", "power"} once the radio has been checked there."""
        raise Unsupported("tune a radio", self.name)

    def close(self) -> None:
        pass

    def __enter__(self) -> "Bench":
        return self

    def __exit__(self, *exc: Any) -> None:
        self.close()


# ── the ESP32 board ──

def parse_resolved(text: str) -> Dict[str, Any]:
    """The facts the tools need from the board's /api/resolved: MQTT broker, root and app; each SDR channel's cc,
    slot and tg; the tuner plan line."""
    out: Dict[str, Any] = {"channels": {}}
    for line in text.splitlines():
        m = re.match(r"mqtt (\S+?):(\d+) root (\S+) app (\S+)(?: user (\S+))?(?: tls (\d))?", line)
        if m:
            out["mqtt"] = {"host": m[1], "port": int(m[2]), "root": m[3], "app": m[4], "user": m[5],
                           "tls": m[6] == "1"}
            continue
        m = re.match(r'channel (\S+) ([\d.]+) "([^"]*)" cc (\d+) slot (\d+) tg (\d+)', line)
        if m:
            out["channels"][m[1]] = {"freq_hz": float(m[2]), "name": m[3], "cc": int(m[4]), "slot": int(m[5]),
                                     "tg": int(m[6])}
            continue
        if line.startswith("plan:"):
            out["plan"] = line[5:].strip()
    return out


def spectrum_frame(b: bytes) -> Dict[str, Any]:
    """A WS /spectrum/ws binary frame: RDSP, u16 bins, u16 row_ms, u64 sc (the row's last sample), a byte a bin
    (-2 x dBFS)."""
    import struct
    if len(b) < 16 or b[:4] != b"RDSP":
        raise BenchError("internal", "not a spectrum frame")
    bins, row_ms, sc = struct.unpack_from("<HHQ", b, 4)
    return {"type": "row", "sc": sc, "row_ms": row_ms, "row": [-v / 2.0 for v in b[16:16 + bins]]}


def spectrum_rows(d: Dict[str, Any]) -> Dict[str, Any]:
    """The board's /api/spectrum answer (each row base64, a byte a bin, -2 x dBFS) as the spec's spectrum."""
    rows = [[-b / 2.0 for b in base64.b64decode(r)] for r in d.get("rows", [])]
    return {"seq": d.get("seq"), "center_hz": d.get("center"), "rate": d.get("rate"), "bins": d.get("bins"),
            "row_ms": d.get("row_ms"), "rows": rows}


class Esp32Bench(Bench):
    """An esp_idf board over HTTP. v1 when /api/bench/v1/info answers, else the legacy endpoints."""

    def __init__(self, url: str, timeout: float = 8.0, mqtt_login: Optional[tuple] = None) -> None:
        self.url = url.rstrip("/")
        self.timeout = timeout
        self.name = f"the board at {self.url}"
        self.mqtt_login = mqtt_login
        self._v1: Optional[bool] = None

    # HTTP
    def _get(self, path: str, raw: bool = False) -> Any:
        try:
            with urllib.request.urlopen(self.url + path, timeout=self.timeout) as r:
                body = r.read()
        except urllib.error.HTTPError as e:
            msg = e.read().decode("utf-8", "replace")
            try:
                j = json.loads(msg)
                raise BenchError(j.get("code", "internal"), j.get("error", msg)) from None
            except ValueError:
                pass
            code = {404: "unsupported", 403: "refused", 409: "busy", 503: "unavailable"}.get(e.code, "internal")
            raise BenchError(code, f"GET {path}: HTTP {e.code} {msg.strip()[:200]}") from None
        except (urllib.error.URLError, OSError) as e:
            raise BenchError("unavailable", f"{self.url}: {getattr(e, 'reason', e)}") from None
        return body if raw else json.loads(body)

    @property
    def v1(self) -> bool:
        if self._v1 is None:
            try:
                self._v1_info = self._get(API + "/info")
                self._v1 = True
            except BenchError as e:
                if e.code not in ("unsupported",):
                    raise
                self._v1 = False
        return self._v1

    def info(self) -> Dict[str, Any]:
        if self.v1:
            return self._get(API + "/info")
        st = self._get("/api/status")
        res = parse_resolved(self._get("/api/resolved", raw=True).decode("utf-8", "replace"))
        sdr = st.get("sdr") or {}
        chans = []
        for c in sdr.get("channels", []):
            r = res["channels"].get(c.get("id"), {})
            chans.append({"id": c.get("id"), "name": c.get("name"), "freq_hz": float(c.get("freq")),
                          "cc": r.get("cc"), "slot": r.get("slot"), "in_band": c.get("in"), "caps": ["rx", "dmr"]})
        rate = sdr.get("sample_rate")
        return {
            "api": "legacy",
            "device": {"kind": "esp32s3-gateway", "name": st.get("name"), "version": st.get("version"),
                       "uptime_s": st.get("uptime_s"), "clock": st.get("clock")},
            "sdr": None if not sdr else {
                "name": sdr.get("name"), "frontend": sdr.get("frontend"), "tuner": sdr.get("rtl"),
                "rate": rate, "center_hz": sdr.get("center_hz"), "gain_db": sdr.get("gain_db"),
                "ppm": (sdr.get("ppm") or {}).get("rtl"), "receiving": sdr.get("receiving"),
                "window": {"lo_hz": sdr["center_hz"] - 0.4 * rate, "hi_hz": sdr["center_hz"] + 0.4 * rate}
                if rate and sdr.get("center_hz") else None,
                "plan": res.get("plan")},
            "channels": chans,
            "radios": [],
            "caps": {"spectrum": True, "bursts": "mqtt" if res.get("mqtt") else False, "iq": False, "tune": False,
                     "tx": False, "radio_control": False},
            "mqtt": res.get("mqtt"),
        }

    def status(self) -> Dict[str, Any]:
        if self.v1:
            return self._get(API + "/status")
        st = self._get("/api/status")
        sdr = st.get("sdr") or {}
        return {"api": "legacy", "device": {k: st.get(k) for k in ("name", "version", "uptime_s", "heap_free",
                                                                     "psram_free", "wifi", "ip", "clock")},
                "sdr": {k: sdr.get(k) for k in ("receiving", "cpu_load", "usb_bytes_per_s", "usb_expected_per_s",
                                                "usb_errors", "ring_overflows", "bytes_lost", "stream_restarts",
                                                "mqtt", "mqtt_dropped")} if sdr else None,
                "channels": [{k: c.get(k) for k in ("id", "freq", "bursts", "rssi", "afc", "idle")}
                             for c in sdr.get("channels", [])],
                "sessions": [], "tx": None}

    def spectrum_stream(self, seconds: float = 0.0) -> Iterator[Dict[str, Any]]:
        self._need_v1("the spectrum stream")
        q = {"seconds": seconds} if seconds else {}
        for m in ws_messages(self.url, API + "/spectrum/ws", q, seconds + 10 if seconds else 1e9):
            if isinstance(m, (bytes, bytearray)):
                yield spectrum_frame(m)
                continue
            d = json.loads(m)
            if d.get("type") == "error":
                raise BenchError(d.get("code", "internal"), d.get("error", ""))
            yield d
            if d.get("type") == "done":
                return

    def spectrum(self, since: int = 0) -> Dict[str, Any]:
        path = (API + "/spectrum" if self.v1 else "/api/spectrum") + f"?since={int(since)}"
        d = self._get(path)
        return spectrum_rows(d) if d.get("rows") and isinstance(d["rows"][0], str) else d

    def regs(self) -> Dict[str, Any]:
        return self._get("/api/rtl/regs")

    def log(self) -> str:
        return self._get("/api/log", raw=True).decode("utf-8", "replace")

    def bursts(self, freq_hz: Optional[float] = None, channel: Optional[str] = None, seconds: float = 10.0,
               max_bursts: Optional[int] = None) -> Iterator[Dict[str, Any]]:
        if self.v1:
            yield from self._ws_bursts(freq_hz, channel, seconds, max_bursts)
            return
        info = self.info()
        mq = info.get("mqtt")
        if not mq:
            raise Unsupported("stream bursts (no MQTT in its settings)", self.name)
        chans = {c["id"]: c for c in info["channels"]}
        if channel is None and freq_hz is not None:
            channel = next((c["id"] for c in info["channels"] if abs(c["freq_hz"] - freq_hz) < 1.0), None)
            if channel is None:
                raise BenchError("refused", f"{freq_hz / 1e6:.5f} MHz is not one of the board's SDR channels")
        login = self.mqtt_login or (os.environ.get("RDOF_BENCH_MQTT_USER"), os.environ.get("RDOF_BENCH_MQTT_PASSWORD"))
        topic = f"{mq['root']}/{channel or '+'}/{mq['app']}/raw"
        yield from mqtt_bursts(mq["host"], mq["port"], topic, login, seconds, max_bursts,
                               lambda ch: chans.get(ch, {}).get("freq_hz"), tls=mq.get("tls", False))

    def _send(self, method: str, path: str, body: Optional[Dict[str, Any]] = None) -> Any:
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(self.url + path, data=data, method=method,
                                     headers={"Content-Type": "application/json"} if data else {})
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                return json.loads(r.read() or b"{}")
        except urllib.error.HTTPError as e:
            msg = e.read().decode("utf-8", "replace")
            try:
                j = json.loads(msg)
            except ValueError:
                j = {}
            code = j.get("code") or {404: "unsupported", 403: "refused", 409: "busy"}.get(e.code, "internal")
            raise BenchError(code, j.get("error") or f"{method} {path}: HTTP {e.code} {msg.strip()[:200]}") from None
        except (urllib.error.URLError, OSError) as e:
            raise BenchError("unavailable", f"{self.url}: {getattr(e, 'reason', e)}") from None

    def _need_v1(self, what: str) -> None:
        if not self.v1:
            raise Unsupported(f"{what}: the firmware has no bench API v1", self.name)

    def radios(self) -> List[Dict[str, Any]]:
        self._need_v1("radios")
        return self._get(API + "/radios")

    def session(self, label: str, ttl_s: float = 30.0) -> Dict[str, Any]:
        """A session ({"session": id, ...}); renew it with renew() within ttl_s, end it with end_session()."""
        self._need_v1("sessions")
        return self._send("POST", API + "/session", {"label": label, "ttl_s": ttl_s})

    def lease(self, sid: str, freqs_hz: List[float], tx: bool = False) -> Dict[str, Any]:
        return self._send("POST", f"{API}/session/{sid}/lease", {"freq_hz": list(freqs_hz), "rx": True, "tx": tx})

    def renew(self, sid: str) -> Dict[str, Any]:
        return self._get(f"{API}/session/{sid}")

    def end_session(self, sid: str) -> None:
        try:
            self._send("DELETE", f"{API}/session/{sid}")
        except BenchError:
            pass                                              # gone already (expired)

    def tune(self, name: str, session: str, freq_hz: float, mode: str = "dmr", cc: Optional[int] = None,
             slot: Optional[int] = None, tg: Optional[int] = None, private: bool = False,
             proto: Optional[str] = None) -> Dict[str, Any]:
        self._need_v1("radio tune")
        body: Dict[str, Any] = {"freq_hz": round(freq_hz), "mode": mode}
        body.update({k: v for k, v in (("cc", cc), ("slot", slot), ("tg", tg)) if v is not None})
        if private:
            body["private"] = True
        q = urllib.parse.urlencode({k: v for k, v in (("session", session), ("proto", proto)) if v})
        return self._send("POST", f"{API}/radios/{urllib.parse.quote(name)}/tune?{q}", body)

    def link(self, name: str, session: Optional[str] = None, proto: Optional[str] = None) -> "RadioLink":
        self._need_v1("radio links")
        q = {k: v for k, v in (("session", session), ("proto", proto)) if v}
        return RadioLink(self.url, f"{API}/radios/{urllib.parse.quote(name)}/link", q)

    def _ws_bursts(self, freq_hz, channel, seconds, max_bursts) -> Iterator[Dict[str, Any]]:
        q = {"seconds": seconds}
        if channel:
            q["channel"] = channel
        if freq_hz:
            q["freq_hz"] = freq_hz
        if max_bursts:
            q["max_bursts"] = max_bursts
        for msg in ws_messages(self.url, API + "/bursts", q, seconds + 15):
            if isinstance(msg, (bytes, bytearray)):
                continue
            d = json.loads(msg)
            if d.get("type") == "burst":
                yield d["burst"]
            elif d.get("type") == "error":
                raise BenchError(d.get("code", "internal"), d.get("error", ""))
            elif d.get("type") == "done":
                return

    def calls(self, freq_hz: Optional[float] = None, channel: Optional[str] = None, seconds: float = 60.0,
              ambe: bool = False) -> Iterator[Dict[str, Any]]:
        self._need_v1("calls")
        q: Dict[str, Any] = {"seconds": seconds}
        if channel:
            q["channel"] = channel
        if freq_hz:
            q["freq_hz"] = freq_hz
        if ambe:
            q["audio"] = "ambe"
        for m in ws_messages(self.url, API + "/calls", q, seconds + 15):
            if isinstance(m, (bytes, bytearray)):
                continue
            d = json.loads(m)
            if d.get("type") == "error":
                raise BenchError(d.get("code", "internal"), d.get("error", ""))
            if d.get("type") in ("call", "event", "ambe", "drop"):
                yield d
            elif d.get("type") == "done":
                return

    def tx(self, session: str, freq_hz: float, via: str = "modem", cc: int = 1,
           on_event: Optional[Callable[[Dict[str, Any]], None]] = None) -> "TxStream":
        """A transmit job (WS /tx): the session must hold a TX lease on freq_hz (lease(..., tx=True))."""
        self._need_v1("transmit")
        q = {"session": session, "freq_hz": int(round(freq_hz)), "via": via, "cc": cc}
        return TxStream(self.url, API + "/tx", q, renew=lambda: self.renew(session), on_event=on_event)

    def capture(self, freq_hz: float, seconds: float, out: str, **kw: Any) -> Dict[str, Any]:
        if not self.v1:
            raise Unsupported("capture I/Q: the firmware has no /api/bench/v1/iq yet", self.name)
        if kw.get("wide"):
            q = {"rate": "wide", "seconds": seconds, "fmt": kw.get("fmt") or "cs8"}
            if kw.get("pre_s"):
                q["pre_s"] = kw["pre_s"]
            return iq_to_file(ws_messages(self.url, API + "/iq", q, seconds + 20), out)
        q = {"freq_hz": freq_hz, "seconds": seconds, "fmt": kw.get("fmt") or "cs16"}
        for k in ("bw_hz", "pre_s", "trigger", "threshold_db", "min_ms", "post_s"):
            if kw.get(k) is not None:
                q[k] = kw[k]
        if any(abs(c["freq_hz"] - freq_hz) < 1 and c.get("in_band") for c in self._get(API + "/status")["channels"]):
            return iq_to_file(ws_messages(self.url, API + "/iq", q, seconds + 20), out)
        # no receiver channel there: a tap for the capture, under an exclusive lease that puts the settings back
        sid = self.session("benchapi capture", ttl_s=seconds + 30)["session"]
        try:
            self._send("POST", f"{API}/session/{sid}/lease", {"freq_hz": [freq_hz], "exclusive": True})
            try:
                self.retune(sid, taps_hz=[freq_hz])
            except BenchError as e:
                if "outside the window" not in str(e):
                    raise
                rate = self._get(API + "/sdr")["rate"]
                self.retune(sid, center_hz=freq_hz - rate / 8, taps_hz=[freq_hz])  # the tap clear of the DC spike
            print(f"bench: capturing {seconds:g} s at {freq_hz / 1e6:.5f} MHz through a tap", file=sys.stderr)
            return iq_to_file(ws_messages(self.url, API + "/iq", q, seconds + 20), out)
        finally:
            self.end_session(sid)

    def retune(self, session: str, center_hz: Any = None, gain_db: Any = None,
               taps_hz: Optional[List[float]] = None) -> Dict[str, Any]:
        """PUT /sdr under the session's exclusive lease: center_hz / gain_db a number or "auto" (the settings'),
        taps_hz receiver channels at any frequency in the window (ids T1, ...); None leaves each as it is. Lasts
        until the lease ends. Returns GET /sdr."""
        self._need_v1("retune")
        body = {k: v for k, v in (("center_hz", center_hz), ("gain_db", gain_db), ("taps_hz", taps_hz))
                if v is not None}
        return self._send("PUT", f"{API}/sdr?session={urllib.parse.quote(session)}", body)


def ws_messages(base: str, path: str, query: Dict[str, Any], timeout: float) -> Iterator[Any]:
    """Messages (str for text, bytes for binary) from a v1 WebSocket stream until it closes."""
    from websockets.sync.client import connect
    from websockets.exceptions import ConnectionClosed
    u = urllib.parse.urlsplit(base)
    scheme = "wss" if u.scheme == "https" else "ws"
    url = f"{scheme}://{u.netloc}{path}?{urllib.parse.urlencode(query)}"
    deadline = time.monotonic() + timeout
    try:
        with connect(url, open_timeout=10, max_size=None) as ws:
            while True:
                left = deadline - time.monotonic()
                if left <= 0:
                    return
                try:
                    yield ws.recv(timeout=left)
                except TimeoutError:
                    return
    except ConnectionClosed:
        return
    except OSError as e:
        raise BenchError("unavailable", f"{url}: {e}") from None


class RadioLink:
    """A bench radio's serial port over WS /radios/{name}/link: send() bytes to it, recv() its bytes (or the guard's
    events, as dicts)."""

    def __init__(self, base: str, path: str, query: Dict[str, Any]) -> None:
        from websockets.sync.client import connect
        u = urllib.parse.urlsplit(base)
        scheme = "wss" if u.scheme == "https" else "ws"
        self.url = f"{scheme}://{u.netloc}{path}" + (f"?{urllib.parse.urlencode(query)}" if query else "")
        try:
            self._cm = connect(self.url, open_timeout=10, max_size=None)
            self.ws = self._cm.__enter__()
        except Exception as e:  # the board refuses the upgrade: no such radio, unavailable, already linked
            raise BenchError("unavailable", f"{self.url}: {e} (see bench.py radios)") from None

    def send(self, data: bytes) -> None:
        self.ws.send(bytes(data))

    def recv(self, timeout: Optional[float] = None) -> Any:
        """bytes from the radio, a dict for a guard event, None on timeout; raises EOFError when the link closes."""
        from websockets.exceptions import ConnectionClosed
        try:
            m = self.ws.recv(timeout=timeout)
        except TimeoutError:
            return None
        except ConnectionClosed:
            raise EOFError("the link closed") from None
        return m if isinstance(m, bytes) else json.loads(m)

    def close(self) -> None:
        self._cm.__exit__(None, None, None)

    def __enter__(self) -> "RadioLink":
        return self

    def __exit__(self, *exc: Any) -> None:
        self.close()


class TxStream:
    """A transmit job over WS /tx (spec section 7): send() queues bursts as fast as the air takes them; the keying
    ends by itself once they're out (unkeyed "end", wait_keyings()); unkey() stops it now and drops what's queued (an
    abort); wait_done() returns the done frame. A thread reads the bench's frames and keeps the job alive (a ping
    every half second, the dead-man rule; the session renewed every 10 s). An error frame raises BenchError from the
    next call. Frames seen so far are in .events."""

    CHUNK = 16          # bursts a frame
    AHEAD_S = 1.5       # keep this much queued on the bench

    def __init__(self, base: str, path: str, query: Dict[str, Any], renew: Optional[Callable[[], Any]] = None,
                 on_event: Optional[Callable[[Dict[str, Any]], None]] = None) -> None:
        import queue
        import threading
        from websockets.sync.client import connect
        u = urllib.parse.urlsplit(base)
        scheme = "wss" if u.scheme == "https" else "ws"
        self.url = f"{scheme}://{u.netloc}{path}?{urllib.parse.urlencode(query)}"
        try:
            self._cm = connect(self.url, open_timeout=10, max_size=None)
            self.ws = self._cm.__enter__()
        except Exception as e:  # one job at a time: the board refuses the upgrade while another is open
            raise BenchError("busy", f"{self.url}: {e} (another transmit job?)") from None
        self.events: List[Dict[str, Any]] = []
        self.start: Optional[Dict[str, Any]] = None
        self.done: Optional[Dict[str, Any]] = None
        self.error: Optional[BenchError] = None
        self._acks: "queue.Queue[Dict[str, Any]]" = queue.Queue()
        self._renew, self._on_event = renew, on_event
        self._stop = threading.Event()
        self._closed = threading.Event()
        self._lock = threading.Lock()
        self._reader = threading.Thread(target=self._read, daemon=True)
        self._pinger = threading.Thread(target=self._ping, daemon=True)
        self._reader.start()
        self._pinger.start()

    def _read(self) -> None:
        from websockets.exceptions import ConnectionClosed
        try:
            while True:
                m = self.ws.recv()
                if isinstance(m, bytes):
                    continue
                d = json.loads(m)
                t = d.get("type")
                if t == "start":
                    self.start = d
                    continue
                self.events.append(d)
                if self._on_event:
                    self._on_event(d)
                if t == "ack":
                    self._acks.put(d)
                elif t == "error":
                    self.error = BenchError(d.get("code", "internal"), d.get("error", ""))
                elif t == "done":
                    self.done = d
                    break
        except (ConnectionClosed, OSError):
            pass
        finally:
            self._closed.set()
            self._acks.put({})

    def _ping(self) -> None:
        last = time.monotonic()
        while not self._stop.wait(0.5) and not self._closed.is_set():
            try:
                self._send({"type": "ping"})
                if self._renew and time.monotonic() - last > 10:
                    last = time.monotonic()
                    self._renew()
            except Exception:
                pass

    def _send(self, d: Dict[str, Any]) -> None:
        from websockets.exceptions import ConnectionClosed
        try:
            with self._lock:
                self.ws.send(json.dumps(d))
        except ConnectionClosed:
            # the bench closed the job (a refusal races our next frame): its error frame says why
            self._closed.wait(2)
            self._check()
            raise

    def _check(self) -> None:
        if self.error:
            raise self.error
        if self._closed.is_set():
            raise BenchError("internal", "the transmit job closed: " + json.dumps(self.done or {}))

    def send(self, bursts: List[bytes], slot: int = 2, timeout: float = 10.0) -> None:
        """Queue bursts (33 bytes each), CHUNK a frame, waiting while more than AHEAD_S is queued."""
        for i in range(0, len(bursts), self.CHUNK):
            self._check()
            items = [{"raw_hex": bytes(b).hex(), "slot": slot} for b in bursts[i:i + self.CHUNK]]
            self._send({"type": "bursts", "items": items})
            try:
                ack = self._acks.get(timeout=timeout)
            except Exception:
                raise BenchError("timeout", "no ack from the bench") from None
            self._check()
            if ack.get("queued_s", 0) > self.AHEAD_S:
                time.sleep(ack["queued_s"] - self.AHEAD_S)

    def unkey(self) -> None:
        self._check()
        self._send({"type": "unkey"})

    def wait_keyings(self, n: int = 1, timeout: float = 30.0) -> Optional[Dict[str, Any]]:
        """Wait for the n-th unkeyed frame (or done): that keying's frame, None on timeout."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            un = [e for e in self.events if e.get("type") == "unkeyed"]
            if len(un) >= n:
                return un[n - 1]
            if self._closed.is_set():
                break
            time.sleep(0.05)
        self._check()
        return None

    def wait_done(self, timeout: float = 10.0) -> Optional[Dict[str, Any]]:
        self._closed.wait(timeout)
        return self.done

    def close(self) -> None:
        self._stop.set()
        try:
            self._cm.__exit__(None, None, None)
        except Exception:
            pass
        self._reader.join(3)

    def __enter__(self) -> "TxStream":
        return self

    def __exit__(self, *exc: Any) -> None:
        self.close()


def link_pty(link: RadioLink, on_event: Callable[[Dict[str, Any]], None],
             on_ready: Callable[[str], None], stop: Optional[Any] = None, tick: Optional[Callable[[], None]] = None
             ) -> None:
    """Bridge the link to a new pty, as the bench daemon's stations do: tools open the pty's path like the radio's own
    port. Runs until the link closes or stop (a threading.Event) is set; tick() is called about once a second."""
    import select
    import threading
    import tty
    master, slave = os.openpty()
    tty.setraw(slave)
    on_ready(os.ttyname(slave))
    stop = stop or threading.Event()

    def from_radio() -> None:
        try:
            while not stop.is_set():
                m = link.recv(0.5)
                if isinstance(m, bytes):
                    os.write(master, m)
                elif isinstance(m, dict):
                    on_event(m)
        except (EOFError, OSError):
            pass
        stop.set()

    t = threading.Thread(target=from_radio, daemon=True)
    t.start()
    last = time.monotonic()
    try:
        while not stop.is_set():
            r, _, _ = select.select([master], [], [], 0.2)
            if r:
                try:
                    data = os.read(master, 4096)
                except OSError:
                    data = b""
                if data:
                    link.send(data)
            if tick and time.monotonic() - last >= 1.0:
                last = time.monotonic()
                tick()
    finally:
        stop.set()
        link.close()
        t.join(2)
        for fd in (master, slave):
            try:
                os.close(fd)
            except OSError:
                pass


# ── I/Q frames (spec "I/Q stream") ──

IQ_MAGIC = b"RDIQ"
IQ_HEADER = 32
IQ_FORMATS = {1: "cs8", 2: "cu8", 3: "cs16", 4: "cf32"}
IQ_BYTES = {"cs8": 2, "cu8": 2, "cs16": 4, "cf32": 8}
FLAG_GAP, FLAG_CLIPPED, FLAG_RETUNED, FLAG_TX, FLAG_LAST, FLAG_PREROLL = 1, 2, 4, 8, 16, 32


def iq_header(b: bytes) -> Dict[str, Any]:
    """The 32-byte frame header: magic, version, format, flags, seq, n, sc, t_us."""
    import struct
    if len(b) < IQ_HEADER or b[:4] != IQ_MAGIC:
        raise BenchError("bad_request", "not an I/Q frame")
    ver, fmt, flags, seq, n, sc, t_us = struct.unpack_from("<BBHIIQq", b, 4)
    return {"version": ver, "fmt": IQ_FORMATS.get(fmt, str(fmt)), "flags": flags, "seq": seq, "n": n, "sc": sc,
            "t_us": t_us}


def iq_frame(fmt: str, flags: int, seq: int, sc: int, t_us: int, samples: bytes) -> bytes:
    import struct
    code = {v: k for k, v in IQ_FORMATS.items()}[fmt]
    n = len(samples) // IQ_BYTES[fmt]
    return IQ_MAGIC + struct.pack("<BBHIIQq", 1, code, flags, seq, n, sc, t_us) + samples


def iq_to_file(messages: Iterator[Any], out: str) -> Dict[str, Any]:
    """Write a v1 I/Q stream to OUT (raw samples, gaps zero-filled so sample index = file position) and OUT.json
    (the start message, then the done message's counts). Returns the metadata."""
    meta: Dict[str, Any] = {}
    nxt = None  # the sample clock the next frame should start at
    gaps = pre = 0
    last = False  # after the last frame, only the done message (its counts) is read
    with open(out, "wb") as f:
        for msg in messages:
            if isinstance(msg, str):
                d = json.loads(msg)
                t = d.get("type")
                if t == "start":
                    meta.update({k: v for k, v in d.items() if k != "type"})
                elif t == "error":
                    raise BenchError(d.get("code", "internal"), d.get("error", ""))
                elif t == "done":
                    meta.update({k: v for k, v in d.items() if k != "type"})
                    if d.get("why") == "no_signal":
                        raise BenchError("no_signal", "the trigger never fired")
                    break
                continue
            if last:
                continue
            h = iq_header(msg)
            body = msg[IQ_HEADER:IQ_HEADER + h["n"] * IQ_BYTES[h["fmt"]]]
            # sc counts wideband samples; a channel tap's samples are sc_rate / rate of them apart
            ratio = (meta.get("sc_rate") or meta.get("rate") or 1) / (meta.get("rate") or 1)
            if nxt is None:
                nxt = h["sc"]
                meta.setdefault("sc0", h["sc"])
            missing = int(round((h["sc"] - nxt) / ratio))
            if missing > 0:
                gaps += missing
                f.write(b"\0" * (missing * IQ_BYTES[h["fmt"]]))
            f.write(body)
            if h["flags"] & FLAG_PREROLL:
                pre += h["n"]
            nxt = h["sc"] + h["n"] * ratio
            last = bool(h["flags"] & FLAG_LAST)
    meta["gap_samples"] = gaps
    if pre:
        meta["preroll_samples"] = pre
    with open(out + ".json", "w") as f:
        json.dump(meta, f, indent=1)
    return meta


# ── MQTT bursts (the board today) ──

def mqtt_bursts(host: str, port: int, topic: str, login: tuple, seconds: float, max_bursts: Optional[int],
                freq_of: Callable[[str], Optional[float]], tls: bool = False) -> Iterator[Dict[str, Any]]:
    """Burst records from {root}/{channel}/{app}/raw for `seconds`. Subscribes only."""
    import queue
    import paho.mqtt.client as mqtt
    q: "queue.Queue" = queue.Queue()
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"rdof-bench-{uuid.uuid4().hex[:8]}")
    if login and login[0]:
        c.username_pw_set(login[0], login[1])
    if tls:
        c.tls_set()

    def on_message(_c, _u, m):
        body = raw_body(m.payload)
        if body is not None:
            ch = m.topic.split("/")[-3]
            q.put(record_from_raw(body, ch, freq_of(ch)))

    def on_connect(_c, _u, _f, rc, _p=None):
        if rc == 0:
            c.subscribe(topic)
        else:
            q.put(BenchError("refused", f"MQTT {host}:{port}: {rc}"))
    c.on_message = on_message
    c.on_connect = on_connect
    try:
        c.connect(host, port, 30)
    except OSError as e:
        raise BenchError("unavailable", f"MQTT {host}:{port}: {e}") from None
    c.loop_start()
    n = 0
    deadline = time.monotonic() + seconds
    try:
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                return
            try:
                rec = q.get(timeout=left)
            except Exception:
                return
            if isinstance(rec, BenchError):
                raise rec
            yield rec
            n += 1
            if max_bursts and n >= max_bursts:
                return
    finally:
        c.loop_stop()
        c.disconnect()


# ── The bench daemon ──

RELAY = (
    "import os,socket,sys,threading\n"
    "s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)\n"
    "try:\n s.connect(sys.argv[1])\n"
    "except OSError as e:\n sys.stdout.write('{\"error\":\"the bench daemon is not running (%s)\",\"code\":\"unavailable\"}\\n'"
    " % e.strerror);sys.exit(0)\n"
    "def up():\n"
    " while True:\n"
    "  b=os.read(0,65536)\n"
    "  if not b:\n   os._exit(0)\n"
    "  s.sendall(b)\n"
    "threading.Thread(target=up,daemon=True).start()\n"
    "while True:\n"
    " b=s.recv(65536)\n"
    " if not b:\n  break\n"
    " os.write(1,b)\n"
)


def relay_cmd(host: str, py: str, sock: str) -> List[str]:
    """The command that joins its stdin / stdout to the daemon's socket on HOST (the tests run RELAY locally)."""
    return ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", host,
            f"{py} -c {shlex.quote(RELAY)} {shlex.quote(sock)}"]


class _Conn:
    """One request to the bench daemon: send a JSON line, read JSON lines. Closing it early cancels the request, as
    closing the daemon's socket does."""

    def __init__(self, host: Optional[str], sock: str, py: str) -> None:
        self.proc = None
        self.s = None
        if host:
            cmd = relay_cmd(host, py, sock)
            self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.r, self.w = self.proc.stdout, self.proc.stdin
        else:
            self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                self.s.connect(sock)
            except OSError as e:
                raise BenchError("unavailable", f"the bench daemon is not running ({sock}: {e.strerror})") from None
            self.r = self.s.makefile("rb")
            self.w = None

    def send(self, req: Dict[str, Any]) -> None:
        line = (json.dumps(req) + "\n").encode()
        if self.w is not None:
            self.w.write(line)
            self.w.flush()
        else:
            self.s.sendall(line)

    def lines(self) -> Iterator[Dict[str, Any]]:
        for line in self.r:
            line = line.strip()
            if line:
                yield json.loads(line)
        if self.proc is not None:
            err = self.proc.stderr.read().decode("utf-8", "replace").strip() if self.proc.poll() is not None else ""
            if self.proc.wait(5) == 255:
                raise BenchError("unavailable", f"ssh: {err[-300:]}")

    def close(self) -> None:
        if self.proc is not None:
            try:
                self.w.close()
            except OSError:
                pass
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        if self.s is not None:
            self.s.close()


def _daemon_error(d: Dict[str, Any]) -> BenchError:
    code = d.get("code", "internal")
    code = {"deadlock": "busy"}.get(code, code)
    return BenchError(code, d.get("error", ""), **{k: v for k, v in d.items() if k not in ("error", "code")})


class DaemonBench(Bench):
    """The authors' bench daemon over its unix socket, through ssh when HOST is given."""

    def __init__(self, host: Optional[str] = None, sock: Optional[str] = None, py: Optional[str] = None,
                 label: Optional[str] = None, wait_max: Optional[float] = None,
                 on_note: Optional[Callable[[str], None]] = None) -> None:
        self.host = host
        self.sock = sock or os.environ.get("RDOF_BENCH_DAEMON_SOCKET")
        if not self.sock:
            raise BenchError("bad_request", "the bench daemon's socket: set $RDOF_BENCH_DAEMON_SOCKET "
                             "(or, on the bench host, use daemon+unix://PATH)")
        self.py = py or os.environ.get("RDOF_BENCH_DAEMON_PY") or "python3"
        self.label = label or os.environ.get("RDOF_BENCH_LABEL") or "rd-bench"
        self.wait_max = wait_max
        self.name = f"bench daemon{' on ' + host if host else ''}"
        self.on_note = on_note or (lambda s: print(f"bench: {s}", file=sys.stderr, flush=True))

    def request(self, req: Dict[str, Any]) -> Iterator[Dict[str, Any]]:
        """Send one request; yield its lines (queued / granted / note lines go to on_note); raise on an error
        line. Leaving the iterator early cancels the request."""
        req.setdefault("label", self.label)
        if self.wait_max is not None:
            req.setdefault("wait_max", self.wait_max)
        conn = _Conn(self.host, self.sock, self.py)
        try:
            conn.send(req)
            for d in conn.lines():
                if "error" in d:
                    raise _daemon_error(d)
                if d.get("state") == "queued":
                    self.on_note(f"waiting: about {d.get('eta_s', 0):.0f} s (at most {d.get('worst_s', 0):.0f} s), "
                                 f"place {d.get('position')}, blockers {d.get('blockers')}")
                    continue
                if "note" in d and len(d) == 1:
                    self.on_note(d["note"])
                    continue
                if d.get("state") == "granted":
                    continue
                yield d
        finally:
            conn.close()

    def one(self, req: Dict[str, Any]) -> Dict[str, Any]:
        for d in self.request(req):
            return d
        raise BenchError("internal", f"the bench daemon gave no answer to {req.get('op')}")

    def info(self) -> Dict[str, Any]:
        d = self.one({"op": "info"})
        w = d.get("window") or {}
        chans = [{"id": c.get("receiver") or f"{c['freq']:.5f}", "name": c.get("name"), "freq_hz": c["freq"] * 1e6,
                  "caps": c.get("caps", []), "in_band": c.get("in_window"), "listed": c.get("listed"),
                  "claimed_by": c.get("claimed_by")} for c in d.get("channels", [])]
        sdr_ok = w.get("enabled", True) and not str(d.get("sdr") or "").startswith("failed")
        return {
            "api": "daemon",
            "device": {"kind": "bench-daemon", "name": self.host or "local"},
            "sdr": None if not w else {
                "frontend": "soapysdr", "rate": w.get("fs"), "center_hz": (w.get("center") or 0) * 1e6,
                "gain_db": w.get("gain"), "ppm": w.get("ppm"), "receiving": sdr_ok, "note": d.get("sdr"),
                "window": {"lo_hz": w["lo"] * 1e6, "hi_hz": w["hi"] * 1e6} if "lo" in w else None},
            "channels": chans,
            "radios": [{"name": s.get("name"), "kind": s.get("kind"), "model": s.get("model"),
                        "held_by": s.get("held_by")} for s in d.get("stations", [])],
            "caps": {"spectrum": False, "bursts": "decoded", "iq": "file", "tune": "exclusive capture",
                     "tx": "modem jobs", "radio_control": "stations"},
            "raw": d,
        }

    def status(self) -> Dict[str, Any]:
        d = self.one({"op": "status"})
        return {"api": "daemon", "sessions": d.get("sessions", []), "held": d.get("held", []),
                "waiting": d.get("waiting", []), "sdr": {"note": d.get("sdr"), "receiver": d.get("receiver")},
                "raw": d}

    def bursts(self, freq_hz: Optional[float] = None, channel: Optional[str] = None, seconds: float = 10.0,
               max_bursts: Optional[int] = None) -> Iterator[Dict[str, Any]]:
        if freq_hz is None:
            if channel is None:
                raise BenchError("bad_request", "the bench daemon needs a frequency (or a channel id it lists)")
            match = [c for c in self.info()["channels"] if c["id"] == channel]
            if not match:
                raise BenchError("refused", f"the bench daemon lists no channel {channel}")
            freq_hz = match[0]["freq_hz"]
        mhz = round(freq_hz / 1e6, 6)
        req = {"op": "bursts", "freq": mhz, "seconds": seconds}
        if max_bursts:
            req["max_bursts"] = max_bursts
        try:
            for d in self.request(req):
                if "burst" in d:
                    yield {"channel": channel, "freq_hz": freq_hz, **d["burst"]}
                elif "done" in d:
                    return
        except BenchError as e:
            if e.code != "bad_request" or "unknown op" not in str(e):
                raise
            # an older daemon: the channel monitor's live bursts
            for d in self.request({"op": "listen", "freq": mhz, "seconds": seconds}):
                if "burst" in d:
                    yield {"channel": channel, "freq_hz": freq_hz, **d["burst"]}
                elif "end" in d:
                    return

    def capture(self, freq_hz: float, seconds: float, out: str, **kw: Any) -> Dict[str, Any]:
        """A raw cs8 capture by the daemon (shared receiver when it can, with pre_s of pre-roll), written on the
        bench host, then copied to OUT (+ OUT.json) and removed there."""
        remote = f"/tmp/rdof-bench-{uuid.uuid4().hex[:12]}.cs8"
        req = {"op": "capture", "freq": round(freq_hz / 1e6, 6), "seconds": seconds, "out": remote,
               "pre_s": kw.get("pre_s") or 0.0}
        if kw.get("gain_db") is not None:
            req["gain"] = kw["gain_db"]
        meta = None
        for d in self.request(req):
            if "ready" in d:
                self.on_note(f"capturing {seconds:g} s at {freq_hz / 1e6:.5f} MHz (shared: {d.get('shared')})")
            elif "done" in d:
                meta = d["done"]
        if meta is None:
            raise BenchError("internal", "the bench daemon ended the capture without a result")
        self._fetch(remote, out)
        meta = {**meta, "fmt": "cs8", "source": "bench-daemon"}
        with open(out + ".json", "w") as f:
            json.dump(meta, f, indent=1)
        return meta

    def _fetch(self, remote: str, out: str) -> None:
        if not self.host:
            os.replace(remote, out)
            try:
                os.remove(remote + ".json")
            except OSError:
                pass
            return
        with open(out, "wb") as f:
            r = subprocess.run(["ssh", "-o", "BatchMode=yes", self.host, f"cat {shlex.quote(remote)}"], stdout=f,
                               stderr=subprocess.PIPE, timeout=600)
        subprocess.run(["ssh", "-o", "BatchMode=yes", self.host,
                        f"rm -f {shlex.quote(remote)} {shlex.quote(remote + '.json')}"], timeout=60)
        if r.returncode:
            raise BenchError("internal", f"fetching {remote}: {r.stderr.decode()[-300:]}")


def open_bench(spec: Optional[str] = None, **kw: Any) -> Bench:
    """A bench from its spec: http(s)://board, daemon (host from $RDOF_BENCH_DAEMON_HOST, else the local socket),
    daemon://HOST, or daemon+unix:///path/to/bench.sock."""
    spec = spec or os.environ.get("RDOF_BENCH")
    if not spec:
        raise BenchError("bad_request", "which bench? --bench http://BOARD | daemon | daemon://HOST (or $RDOF_BENCH)")
    if spec.startswith(("http://", "https://")):
        return Esp32Bench(spec, **kw)
    if spec == "daemon":
        return DaemonBench(os.environ.get("RDOF_BENCH_DAEMON_HOST") or None, **kw)
    if spec.startswith("daemon+unix://"):
        return DaemonBench(None, sock=spec[len("daemon+unix://"):], **kw)
    if spec.startswith("daemon://"):
        return DaemonBench(spec[len("daemon://"):] or None, **kw)
    raise BenchError("bad_request", f"bench {spec!r}: http://BOARD, daemon, daemon://HOST or daemon+unix://PATH")
