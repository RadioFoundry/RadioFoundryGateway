#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
"""A DMR transmitter's PA ramps, burst by burst, from an I/Q capture (bench-api.md 14.8.2).

    burstramp.py FILE [--json OUT] [--bursts]

FILE is a capture with its FILE.json (fmt, fs or rate, freq, center; capture and capture-trigger write them). A channel
capture (48 kS/s, about 12.5 kHz wide) smears an edge by about 0.1 ms; a wideband one is cut to 120 kHz here and doesn't. The
figure a capture can claim is `edge_res_us` in the summary.

Everything is against the burst's own settled level (0 dB) and its payload: t = 0 is the leading edge of the first symbol
(symbol 0's center is 104 us later), and the falling edge's t = 0 is the trailing edge of the last. Negative is before.
Symbols come from the sync; voice B-F, with no sync, from the 30 ms grid of the nearest burst that has one.

Needs numpy.
"""
from __future__ import annotations

import argparse
import json
import math
import sys

import numpy as np

SYM = 4800.0
SLOT_S = 0.030
SYNC_K = 54  # the sync's first symbol
SYNCS = {"BS_VOICE": "755FD7DF75F7", "BS_DATA": "DFF57D75DF5D", "MS_VOICE": "7F7D5DD57DFD", "MS_DATA": "D5D7F77FD757",
         "RC": "77D55F7DFD77", "TS1_VOICE": "5D577F7757FF", "TS1_DATA": "F7FDD5DDFD55", "TS2_VOICE": "7DFFD5F55D5F",
         "TS2_DATA": "D7557F5FF7F5"}
DIBIT = {1: 3.0, 0: 1.0, 2: -1.0, 3: -3.0}
DATA_TYPES = {0: "PI_HDR", 1: "VLC_HDR", 2: "TERM_LC", 3: "CSBK", 4: "MBC_HDR", 5: "MBC", 6: "DATA_HDR", 7: "RATE_1_2",
              8: "RATE_3_4", 9: "IDLE", 10: "RATE_1"}
RISE_DB = (-40, -30, -20, -10, -6, -3, -1)
FALL_DB = (-3, -10, -30)
MIN_CORR = 0.85


def sync_symbols(h: str) -> np.ndarray:
    bits = bin(int(h, 16))[2:].zfill(48)
    return np.array([DIBIT[int(bits[i:i + 2], 2)] for i in range(0, 48, 2)], float)


PATTERNS = {k: sync_symbols(v) for k, v in SYNCS.items()}


def smooth(a: np.ndarray, n: float) -> np.ndarray:
    """A centered moving mean over n samples (made odd); the ends average what there is."""
    n = max(1, int(round(n))) | 1
    if n == 1:
        return a.copy()
    c = np.concatenate(([0.0], np.cumsum(a)))
    h = n // 2
    lo = np.clip(np.arange(len(a)) - h, 0, len(a))
    hi = np.clip(np.arange(len(a)) + h + 1, 0, len(a))
    return (c[hi] - c[lo]) / (hi - lo)


def db(p):
    return 10 * np.log10(np.maximum(p, 1e-14))


def load(path: str):
    """The channel's samples, their rate, the sidecar and the bandwidth they carry (Hz, two-sided)."""
    m = json.load(open(path + ".json"))
    fmt, fs = m.get("fmt", "cs8"), float(m.get("rate") or m["fs"])
    raw = np.fromfile(path, dtype={"cs8": np.int8, "cs16": np.int16, "cf32": np.float32}[fmt]).astype(np.float64)
    x = raw[0::2] + 1j * raw[1::2]
    if fs <= 150e3:
        return x, fs, m, float(m.get("bw") or 0.26 * fs)
    x = x * np.exp(-2j * np.pi * (float(m["freq"]) - float(m["center"])) * np.arange(len(x)) / fs)
    d = max(1, int(fs / 288e3))
    n = 16 * d + 1
    h = np.sinc((np.arange(n) - (n - 1) / 2) * 2 * 60e3 / fs) * np.hamming(n)
    y = np.convolve(x, h / h.sum(), "same")[::d]
    return y, fs / d, m, 120e3


def segments(p: np.ndarray, fs: float):
    """The keyed stretches (start, end) in samples, and the quiet floor (dB of the mean power over 1 ms)."""
    e = db(smooth(p, fs * 5e-4))
    live = e[e > -130]
    floor, top = np.percentile(live, 10), np.percentile(live, 99.5)
    on = e > max(floor + 8, top - 25)
    d = np.diff(on.astype(np.int8))
    ups, downs = list(np.flatnonzero(d == 1) + 1), list(np.flatnonzero(d == -1) + 1)
    if on[0]:
        ups.insert(0, 0)
    if on[-1]:
        downs.append(len(on))
    segs: list = []
    for a, b in zip(ups, downs):
        if segs and a - segs[-1][1] < fs * 1.5e-3:
            segs[-1][1] = b
        else:
            segs.append([a, b])
    segs = [(a, b) for a, b in segs if b - a > fs * 8e-3]
    quiet = np.ones(len(p), bool)
    margin = int(fs * 3e-3)
    for a, b in segs:
        quiet[max(0, a - margin):b + margin] = False
    n = int(fs * 1e-3)
    blocks = [p[i:i + n].mean() for i in range(0, len(p) - n, n) if quiet[i:i + n].all()]
    return segs, float(np.median(db(np.array(blocks)))) if blocks else float(floor), float(top - floor)


def find_syncs(f: np.ndarray, a: int, b: int, sps: float) -> list:
    """Syncs inside samples a..b: [tau (center of the sync's first symbol, fractional), name, corr, f0, a], best first."""
    k = np.round(np.arange(24) * sps).astype(int)
    taus = np.arange(max(0, a - int(20 * sps)), min(len(f) - k[-1] - 2, b))
    if not len(taus):
        return []
    v = f[taus[:, None] + k]
    v = v - v.mean(axis=1, keepdims=True)
    vn = np.sqrt((v ** 2).sum(axis=1)) + 1e-9
    corr = {n: (v @ (pat - pat.mean())) / (vn * np.sqrt(((pat - pat.mean()) ** 2).sum())) for n, pat in PATTERNS.items()}
    names = list(corr)
    c = np.vstack([corr[n] for n in names])
    best, which = c.max(axis=0), c.argmax(axis=0)
    out: list = []
    for i in np.argsort(-best):
        if best[i] < MIN_CORR:
            break
        t = float(taus[i])
        if any(abs(t - o[0]) < 0.020 * SYM * sps for o in out):
            continue
        name = names[which[i]]
        y = corr[name]
        frac = 0.0
        if 0 < i < len(y) - 1:
            den = y[i - 1] - 2 * y[i] + y[i + 1]
            frac = float(0.5 * (y[i - 1] - y[i + 1]) / den) if den else 0.0
        at = (t + frac + k).astype(int)
        A = np.vstack((np.ones(24), PATTERNS[name])).T
        f0, dev = np.linalg.lstsq(A, f[at], rcond=None)[0]
        out.append([t + frac, name, float(best[i]), float(f0), float(dev)])
    return out


def place_bursts(segs, f, fs, sps) -> tuple:
    """Each burst's sync position: from its own sync, or the 30 ms grid. Returns (bursts, how many couldn't be placed)."""
    slot = fs * SLOT_S
    placed, pending = [], []
    for seg in segs:
        found = find_syncs(f, seg[0], seg[1], sps)
        if not found:
            pending.append(seg)
            continue
        if seg[1] - seg[0] < fs * 0.045:
            found = found[:1]
        for o in found:
            placed.append({"seg": seg, "tau": o[0], "sync": o[1], "corr": o[2], "f0": o[3], "dev": o[4], "anchored": True})
        if seg[1] - seg[0] >= fs * 0.045:  # a long key-up: the slots between its syncs
            first = min(found, key=lambda o: o[0])
            have = [o[0] for o in found]
            n0 = int(math.floor((seg[0] - first[0]) / slot)) - 1
            n1 = int(math.ceil((seg[1] - first[0]) / slot)) + 1
            for step in range(n0, n1):
                t = first[0] + step * slot
                if t - SYNC_K * sps < seg[0] - fs * 2e-3 or t + 78 * sps > seg[1] + fs * 2e-3:
                    continue
                if not any(abs(t - h) < 0.01 * fs for h in have):
                    placed.append({"seg": seg, "tau": t, "sync": "GRID", "corr": 0.0, "f0": first[3], "dev": first[4],
                                   "anchored": False})
    unplaced = 0
    anchors = [b for b in placed if b["anchored"]]
    for seg in pending:
        mid = (seg[0] + seg[1]) / 2
        if not anchors:
            unplaced += 1
            continue
        g = min(anchors, key=lambda b: abs(b["tau"] - mid))
        t = g["tau"] + round((mid - (g["tau"] + 12 * sps)) / slot) * slot
        if abs(t + 12 * sps - mid) > fs * 6e-3:
            unplaced += 1
            continue
        placed.append({"seg": seg, "tau": t, "sync": "GRID", "corr": 0.0, "f0": g["f0"], "dev": g["dev"], "anchored": False})
    placed.sort(key=lambda b: b["tau"])
    return placed, unplaced


def last_up(rel, lo, hi, level):
    """The last upward crossing of level in lo..hi, interpolated (sample); None without one."""
    i = np.flatnonzero((rel[lo:hi - 1] < level) & (rel[lo + 1:hi] >= level))
    if not len(i):
        return None
    k = lo + int(i[-1])
    return k + (level - rel[k]) / (rel[k + 1] - rel[k])


def rise_cross(rel, rel_env, lo, hi, level, hold):
    """The last upward crossing of level before the 0.1 ms envelope stays at or over it for hold samples: a dip in the
    plateau later on isn't the rising edge, and a sag at the start is. None if it never holds."""
    if hi - lo <= hold:
        return None
    below = np.concatenate(([0], np.cumsum(rel_env[lo:hi] < level)))
    i = np.flatnonzero(below[hold:] == below[:-hold])
    if not len(i):
        return None
    return last_up(rel, lo, min(hi, lo + int(i[0]) + hold // 8 + 2), level)


def settle(rel_env, a, m1, band, hold):
    """The first sample from a after which the 0.1 ms envelope stays within band for hold samples; None if none."""
    if m1 - a <= hold:
        return None
    out = np.concatenate(([0], np.cumsum(np.abs(rel_env[a:m1]) > band)))
    i = np.flatnonzero(out[hold:] == out[:-hold])
    return a + int(i[0]) if len(i) else None


def first_down(rel, lo, hi, level):
    i = np.flatnonzero((rel[lo:hi - 1] >= level) & (rel[lo + 1:hi] < level))
    if not len(i):
        return None
    k = lo + int(i[0])
    return k + (level - rel[k]) / (rel[k + 1] - rel[k])


def symbols(v, b):
    """Decisions over the burst: out from the sync, or twice along it where there's none. Returns (residual Hz, symbols)."""
    levels = np.array([-3.0, -1.0, 1.0, 3.0])
    f0, a = b["f0"], b["dev"]
    syms = np.zeros(132)

    def walk(order, cur):
        for k in order:
            s = levels[np.argmin(np.abs((v[k] - cur) - levels * a))]
            syms[k] = s
            cur += 0.25 * ((v[k] - s * a) - cur)

    if b["anchored"]:
        syms[SYNC_K:SYNC_K + 24] = PATTERNS[b["sync"]]
        walk(range(SYNC_K + 24, 132), f0)
        walk(range(SYNC_K - 1, -1, -1), f0)
    else:
        for _ in range(2):
            walk(range(132), f0)
            f0 = float(np.median((v - syms * a)[20:112]))
    return v - syms * a - f0, syms


def burst_kind(b, syms) -> str:
    if not b["anchored"]:
        return "VOICE_B_F"
    if "VOICE" in b["sync"]:
        return "VOICE_A"
    inv = {3: 1, 1: 0, -1: 2, -3: 3}
    st = np.concatenate((syms[49:54], syms[78:83])).astype(int)
    bits = "".join(format(inv[int(q)], "02b") for q in st)
    return DATA_TYPES.get(int(bits[4:8], 2), "DATA_" + str(int(bits[4:8], 2)))


def bins_db(p, at, n, width, quiet):
    """n bins of width samples from sample `at`: dB over the quiet floor (None outside the capture)."""
    out = []
    for j in range(n):
        lo, hi = int(round(at + j * width)), int(round(at + (j + 1) * width))
        out.append(None if lo < 0 or hi > len(p) or hi <= lo else round(float(db(p[lo:hi].mean())) - quiet, 1))
    return out


def rnd(x, n=3):
    return None if x is None or not math.isfinite(x) else round(float(x), n)


def measure(b, ctx) -> dict | None:
    p, fine, env, symp, fsym, fs, sps, quiet, segs = (ctx[k] for k in ("p", "fine", "env", "symp", "fsym", "fs", "sps", "quiet", "segs"))
    tn = lambda k: b["tau"] + (k - SYNC_K) * sps  # a symbol's center
    if tn(0) < 0 or tn(131) >= len(fsym) - 1:
        return None
    p0, p1 = tn(0) - sps / 2, tn(131) + sps / 2  # the payload's edges
    seg = b["seg"]
    ms = 1e-3 * fs
    m0, m1 = int(p0 + 4 * ms), int(p1 - 4 * ms)
    settled = float(np.median(fine[m0:m1]))
    rel, rel_env = fine - settled, env - settled
    ripple = float(np.std(rel_env[m0:m1]))
    us = lambda at: 1000.0 * (at - p0) / fs if at is not None else None
    usf = lambda at: 1000.0 * (at - p1) / fs if at is not None else None
    usable = lambda level: settled + level >= quiet + 6.0
    r = {"t_s": rnd(p0 / fs, 6), "kind": None, "sync": b["sync"], "corr": rnd(b["corr"], 3), "anchored": b["anchored"],
         "settled_db": rnd(settled, 2), "ripple_db": rnd(ripple, 2), "rise_ms": {}, "fall_ms": {}}
    res, syms = symbols(fsym[np.round(tn(np.arange(132))).astype(int)], b)
    r["kind"] = burst_kind(b, syms)
    # rising edge: only where this burst opens the key-up
    prev_end = max([s[1] for s in segs if s[1] <= seg[0]], default=0)
    has_rise = abs(p0 - seg[0]) < 12 * ms and seg[0] > 1
    lo = max(prev_end, int(p0 - 14 * ms), 0)
    hi = int(p0 + 10 * ms)
    up: dict = {}
    if has_rise:
        for level in RISE_DB:
            up[level] = rise_cross(rel, rel_env, lo, hi, level, int(ms)) if usable(level) else None
    r["rise_ms"] = {str(k): rnd(us(up.get(k))) for k in RISE_DB}
    r["rise_30_3_ms"] = rnd(us(up[-3]) - us(up[-30])) if up.get(-3) is not None and up.get(-30) is not None else None
    # overshoot and settling, from the -3 dB point (the 0.1 ms envelope: a sample's peak is the signal's own AM)
    start = up.get(-3) or up.get(-10)
    if start is None and has_rise:
        start = float(seg[0])
    r["overshoot_db"] = r["overshoot_t_ms"] = r["settle_1db_ms"] = r["settle_0p5db_ms"] = None
    if start is not None:
        a, b4 = int(start), int(start + 4 * ms)
        k = a + int(np.argmax(rel_env[a:b4]))
        r["overshoot_db"], r["overshoot_t_ms"] = rnd(rel_env[k], 2), rnd(us(k))
        for band, name, need in ((1.0, "settle_1db_ms", 0.4), (0.5, "settle_0p5db_ms", 0.2)):
            if ripple > need:  # the envelope's own ripple is as big as the band: no claim
                continue
            r[name] = rnd(us(settle(rel_env, a, m1, band, int(2 * ms))))
    # the first symbols, sag as well as overshoot
    lev = np.maximum(symp[np.round(tn(np.arange(24))).astype(int)] - settled, quiet - settled)  # no lower than the floor
    r["sym1_db"] = rnd(lev[0], 2)
    for n in (5, 12):
        r[f"err{n}_mean_db"], r[f"err{n}_abs_db"] = rnd(lev[:n].mean(), 2), rnd(np.abs(lev[:n]).mean(), 2)
    # falling edge: only where this burst closes the key-up
    next_start = min([s[0] for s in segs if s[0] >= seg[1]], default=len(rel))
    has_fall = abs(p1 - seg[1]) < 12 * ms and seg[1] < len(rel) - 1
    down: dict = {}
    if has_fall:
        lo2, hi2 = int(p1 - 4 * ms), min(next_start, int(p1 + 8 * ms), len(rel))
        for level in FALL_DB:
            down[level] = first_down(rel, lo2, hi2, level) if usable(level) else None
    r["fall_ms"] = {str(k): rnd(usf(down.get(k))) for k in FALL_DB}
    r["fall_3_30_ms"] = rnd(usf(down[-30]) - usf(down[-3])) if down.get(-3) is not None and down.get(-30) is not None else None
    # energy before it, and in the middle of the empty slot before
    half_ms = 0.5 * ms
    pre = bins_db(p, p0 - 10 * half_ms, 10, half_ms, quiet)
    r["pre_db"] = [None if prev_end and p0 - (10 - j) * half_ms < prev_end else v for j, v in enumerate(pre)]
    mid = tn(66) - SLOT_S * fs
    inside = any(s[0] < mid + 5 * half_ms and s[1] > mid - 5 * half_ms for s in segs)
    r["empty_slot_db"] = None if inside else bins_db(p, mid - 5 * half_ms, 10, half_ms, quiet)
    # frequency over the rise, once there's carrier; symbols are 5 to 1 ms
    k0 = next((k for k in range(24) if lev[k] > -30), None)
    r["f_first1ms_hz"] = r["f_next2ms_hz"] = None
    r["f_first1ms_unreliable"] = r["f_next2ms_unreliable"] = None
    if k0 is not None:
        for a, n, name in ((k0, 5, "f_first1ms"), (k0 + 5, 10, "f_next2ms")):
            r[name + "_hz"] = rnd(res[a:a + n].mean(), 1)
            r[name + "_unreliable"] = bool(rel[np.round(tn(np.arange(a, a + n))).astype(int)].min() < -20)
    r["dev3_hz"] = rnd(b["dev"] * 3, 0)
    return r


FIELDS = ["settled_db", "ripple_db", "rise_30_3_ms", "overshoot_db", "overshoot_t_ms", "settle_1db_ms", "settle_0p5db_ms",
          "sym1_db", "err5_mean_db", "err5_abs_db", "err12_mean_db", "err12_abs_db", "fall_3_30_ms", "f_first1ms_hz",
          "f_next2ms_hz"]
FIELDS += [f"rise_ms.{k}" for k in RISE_DB] + [f"fall_ms.{k}" for k in FALL_DB]


def field(r, name):
    if "." in name:
        a, b = name.split(".")
        return r[a].get(b)
    return r.get(name)


def stats(rows) -> dict:
    out: dict = {"n": len(rows)}
    for name in FIELDS:
        v = [field(r, name) for r in rows]
        v = np.array([x for x in v if x is not None], float)
        if len(v):
            out[name] = {"median": rnd(np.median(v), 3), "min": rnd(v.min(), 3), "max": rnd(v.max(), 3), "n": int(len(v))}
    for name in ("pre_db", "empty_slot_db"):
        cols = [r[name] for r in rows if r[name]]
        if cols:
            a = np.array([[np.nan if x is None else x for x in c] for c in cols], float)
            if np.isfinite(a).any():
                out[name + "_median"] = [None if not np.isfinite(a[:, j]).any() else rnd(np.nanmedian(a[:, j]), 1) for j in range(a.shape[1])]
    return out


def analyze(path: str) -> dict:
    x, fs, m, bw = load(path)
    p = np.abs(x) ** 2
    segs, quiet, snr = segments(p, fs)
    sps = fs / SYM
    ph = np.angle(x[1:] * np.conj(x[:-1]))
    f = np.concatenate((ph, [ph[-1] if len(ph) else 0.0])) * fs / (2 * np.pi)
    f = np.concatenate(([f[0]], (f[:-1] + f[1:]) / 2))  # the discriminator sits between two samples
    fsym = smooth(f, sps / 2)
    placed, unplaced = place_bursts(segs, fsym, fs, sps)
    ctx = dict(p=p, fine=db(smooth(p, max(1, fs * 3e-5))), env=db(smooth(p, fs * 1e-4)), symp=db(smooth(p, sps / 2)), fsym=fsym, fs=fs, sps=sps, quiet=quiet, segs=segs)
    rows = [r for r in (measure(b, ctx) for b in placed) if r]
    t0 = m.get("t0")
    prev = None
    for r in rows:
        r["first"] = prev is None or r["t_s"] - prev > 0.090
        prev = r["t_s"]
        if t0 is not None:
            r["t"] = round(float(t0) + r["t_s"], 6)
    groups: dict = {}
    for r in rows:
        groups.setdefault(("first:" if r["first"] else "") + r["kind"], []).append(r)
    return {"bursts": rows,
            "summary": {"file": path, "fs": fs, "bw_hz": bw, "edge_res_us": round(max(1e6 / fs, 1e6 * 0.7 / (bw / 2)), 1),
                        "quiet_db": rnd(quiet, 1), "snr_db": rnd(snr, 1), "segments": len(segs), "bursts": len(rows), "unplaced": unplaced,
                        "clip_pct": m.get("clip_pct"), "overflows": m.get("overflows"), "gap_samples": m.get("gap_samples"),
                        "by_kind": {k: stats(v) for k, v in sorted(groups.items())}}}


def text(res: dict, per_burst: bool) -> str:
    s = res["summary"]
    out = [f"{s['file']}: {s['bursts']} bursts in {s['segments']} key-ups ({s['unplaced']} not placed), {s['fs']:.0f} S/s, "
           f"{s['bw_hz'] / 1e3:g} kHz wide: edges good to about {s['edge_res_us']:g} us; floor {s['quiet_db']} dB, SNR {s['snr_db']} dB"]
    if s.get("gap_samples") or s.get("overflows"):
        out.append(f"  WARNING: capture lost samples ({s.get('gap_samples')} gap, {s.get('overflows')} overflows)")
    if s.get("clip_pct"):
        out.append(f"  WARNING: {s['clip_pct']}% of the capture clipped: overshoot reads low")
    if per_burst:
        out.append("     t_s kind        -30dB   -3dB    -1dB rise30_3 over@ms  over  set1dB  sym1  err5  err12  fall-3 fall-30 fall3_30")
        for r in res["bursts"]:
            g = lambda v, w=7, f=".2f": f"{v:{w}{f}}" if v is not None else " " * (w - 1) + "-"
            out.append(f"{r['t_s']:8.3f} {r['kind']:10.10s} {g(r['rise_ms']['-30'])} {g(r['rise_ms']['-3'])} {g(r['rise_ms']['-1'])} "
                       f"{g(r['rise_30_3_ms'])} {g(r['overshoot_t_ms'])} {g(r['overshoot_db'], 6, '.1f')} {g(r['settle_1db_ms'])} "
                       f"{g(r['sym1_db'], 5, '.1f')} {g(r['err5_mean_db'], 5, '.1f')} {g(r['err12_mean_db'], 5, '.1f')} "
                       f"{g(r['fall_ms']['-3'])} {g(r['fall_ms']['-30'])} {g(r['fall_3_30_ms'])}")
    for k, g in s["by_kind"].items():
        out.append(f"{k}: n={g['n']}  (median [min..max])")
        for name in FIELDS:
            if name in g:
                out.append(f"  {name:18s} {g[name]['median']:8.2f} [{g[name]['min']:8.2f} .. {g[name]['max']:8.2f}]  n={g[name]['n']}")
        for name in ("pre_db_median", "empty_slot_db_median"):
            if name in g:
                out.append(f"  {name:18s} " + " ".join("-" if v is None else f"{v:g}" for v in g[name]))
    return "\n".join(out)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="A DMR transmitter's PA ramps, burst by burst, from an I/Q capture")
    ap.add_argument("file", nargs="+")
    ap.add_argument("--json", help="write one JSON line per burst and a last {\"summary\": ...} line")
    ap.add_argument("--bursts", action="store_true", help="print each burst too")
    a = ap.parse_args(argv)
    sink = open(a.json, "w") if a.json else None
    for path in a.file:
        res = analyze(path)
        print(text(res, a.bursts))
        if sink:
            for r in res["bursts"]:
                sink.write(json.dumps(r) + "\n")
            sink.write(json.dumps({"summary": res["summary"]}) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
