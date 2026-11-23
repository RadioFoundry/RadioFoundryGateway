#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
"""Several 4FSK carriers summed into one complex baseband, for the bench's iq transmit mode (bench-api.md 14.6): a
trunked system's control channel and a voice channel from one transmitter.

    iqmix.py --rate 192000 --out FILE.cs16 OFFSET_HZ:DIBITS[:KEY=VALUE...] ...
    rfgbench.py tx iq --freq MHz --carriers MHz... --rate 192000 --iq FILE.cs16

Each carrier is OFFSET_HZ from the job's --freq (baseband 0 Hz), its symbols a file of dibits packed 4 to a byte, the
first in bits 7-6 (01 +3, 00 +1, 10 -1, 11 -3), as tx fsk4 takes them. Keys: repeat=N (plays back to back), start=S
(seconds of silence before it), dev=HZ (outer level, 1800), baud=B (4800), shape=c4fm|nrz, level=0-1 (its share of
full scale; the default splits it evenly). The pulse is tx fsk4's c4fm: the raised cosine (roll-off 0.2) times
pi f T / sin(pi f T), unit response behind a one-symbol integrate-and-dump. Output: cs16, I then Q, little-endian.
The sum's peak stays under full scale: the levels add to at most 1.

As a module: c4fm_pulse(), carrier(), mix().

Needs numpy.
"""
from __future__ import annotations

import argparse
import sys
from typing import List, Optional

import numpy as np

ALPHA = 0.2
LEVELS = np.array([1.0, 3.0, -1.0, -3.0])  # dibit 00, 01, 10, 11


def dibits(data: bytes) -> np.ndarray:
    """The symbols' levels (-3..3), first dibit in bits 7-6."""
    b = np.frombuffer(data, dtype=np.uint8)
    d = np.stack([(b >> 6) & 3, (b >> 4) & 3, (b >> 2) & 3, b & 3], axis=1).reshape(-1)
    return LEVELS[d]


def c4fm_pulse(sps: float, span: int = 12) -> np.ndarray:
    """tx fsk4's c4fm frequency pulse at sps samples a symbol, over span symbols either side."""
    nf = 2400
    f = np.linspace(0, (1 + ALPHA) / 2, nf + 1)
    f1, f2 = (1 - ALPHA) / 2, (1 + ALPHA) / 2
    h = np.where(f <= f1, 1.0, np.where(f >= f2, 0.0, 0.5 * (1 + np.cos(np.pi / ALPHA * (f - f1)))))
    with np.errstate(divide="ignore", invalid="ignore"):
        lift = np.where(f > 0, np.pi * f / np.sin(np.pi * f), 1.0)
    s = h * lift
    n = int(np.ceil(span * sps))
    t = np.arange(-n, n + 1) / sps
    trap = getattr(np, "trapezoid", None) or np.trapz
    g = 2 * trap(s[None, :] * np.cos(2 * np.pi * f[None, :] * t[:, None]), f, axis=1)
    mid = np.abs(t) <= 0.5
    return g / g[mid].mean()


def carrier(levels: np.ndarray, rate: float, offset_hz: float, dev: float = 1800.0, baud: float = 4800.0,
            shape: str = "c4fm", ramp_s: float = 0.002) -> np.ndarray:
    """One keying of the symbols as complex baseband at offset_hz, unit amplitude, raised-cosine ramps at its ends."""
    sps = rate / baud
    n = int(np.ceil(len(levels) * sps))
    imp = np.zeros(n)
    idx = np.minimum((np.arange(len(levels)) * sps + sps / 2).astype(int), n - 1)
    imp[idx] = levels * dev / 3
    if shape == "nrz":
        hz = np.repeat(levels * dev / 3, int(round(sps)))[:n]
        hz = np.pad(hz, (0, n - len(hz)))
    else:
        hz = np.convolve(imp, c4fm_pulse(sps), mode="same")
    ph = 2 * np.pi * np.cumsum(hz + offset_hz) / rate
    x = np.exp(1j * ph)
    r = min(int(ramp_s * rate), n // 2)
    if r:
        w = 0.5 - 0.5 * np.cos(np.pi * np.arange(r) / r)
        x[:r] *= w
        x[-r:] *= w[::-1]
    return x


def mix(parts: List[tuple]) -> np.ndarray:
    """Sum (start_sample, level, x) parts into one complex array."""
    n = max(s + len(x) for s, _, x in parts)
    y = np.zeros(n, dtype=np.complex128)
    for s, lvl, x in parts:
        y[s:s + len(x)] += lvl * x
    return y


def to_cs16(y: np.ndarray) -> bytes:
    out = np.empty(2 * len(y), dtype="<i2")
    out[0::2] = np.clip(np.round(y.real * 32767), -32767, 32767)
    out[1::2] = np.clip(np.round(y.imag * 32767), -32767, 32767)
    return out.tobytes()


def parse_spec(spec: str) -> dict:
    off, path, *kv = spec.split(":")
    d = {"offset": float(off), "path": path, "repeat": 1, "start": 0.0, "dev": 1800.0, "baud": 4800.0,
         "shape": "c4fm", "level": None}
    for item in kv:
        k, _, v = item.partition("=")
        if k not in d or k in ("offset", "path"):
            raise ValueError(f"{spec}: unknown key {k}")
        d[k] = v if k == "shape" else int(v) if k == "repeat" else float(v)
    return d


def main(argv: Optional[List[str]] = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--rate", type=float, required=True, help="S/s: the job's rate (a divisor of the gateway's tx_fs)")
    p.add_argument("--out", required=True, help="cs16 file, or - for stdout")
    p.add_argument("carriers", nargs="+", help="OFFSET_HZ:DIBITS[:repeat=N][:start=S][:dev=HZ][:level=L]...")
    a = p.parse_args(argv)
    try:
        specs = [parse_spec(s) for s in a.carriers]
    except ValueError as e:
        print(e, file=sys.stderr)
        return 1
    even = 1.0 / len(specs)
    parts = []
    for s in specs:
        with open(s["path"], "rb") as f:
            lv = dibits(f.read())
        x = carrier(np.tile(lv, s["repeat"]), a.rate, s["offset"], s["dev"], s["baud"], s["shape"])
        if abs(s["offset"]) > 0.4 * a.rate:
            print(f"{s['offset']:.0f} Hz is past this rate's reach", file=sys.stderr)
            return 1
        parts.append((int(s["start"] * a.rate), s["level"] if s["level"] is not None else even, x))
    if sum(lvl for _, lvl, _ in parts) > 1.0 + 1e-9:
        print("the levels add to more than 1: the sum would clip", file=sys.stderr)
        return 1
    y = mix(parts)
    data = to_cs16(y)
    if a.out == "-":
        sys.stdout.buffer.write(data)
    else:
        with open(a.out, "wb") as f:
            f.write(data)
    print(f"{len(y)} samples, {len(y) / a.rate:.2f} s at {a.rate:.0f} S/s, {len(parts)} carriers", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
