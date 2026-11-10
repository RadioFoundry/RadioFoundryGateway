# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
"""tools/bench/burstramp.py on synthetic captures whose ramps are known."""
import json
import os
import sys

import pytest

np = pytest.importorskip("numpy")
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools", "bench"))
import burstramp  # noqa: E402

FS = 48000
SPS = 10
DEV = 648.0
FALL_DELAY_MS = 0.1


def envelope_db(t_ms, start_ms, stop_ms, shape):
    """The burst's level (dB, settled 0) at t_ms; shape(t after the ramp's top) is what the PA does there."""
    t = np.asarray(t_ms, float)
    e = np.full(len(t), -200.0)
    rise = (t >= start_ms - 0.3) & (t < start_ms)
    top = float(shape(np.array([0.0]))[0])
    e[rise] = -60 + (t[rise] - (start_ms - 0.3)) / 0.3 * (top + 60)
    on = (t >= start_ms) & (t < stop_ms)
    e[on] = shape(t[on] - start_ms)
    fall = (t >= stop_ms) & (t < stop_ms + 0.3)
    e[fall] = 0 - (t[fall] - stop_ms) / 0.3 * 60
    return e


def capture(tmp_path, offset_ms, shape, bursts=14):
    """Voice superframes one slot in two: A with its sync, B-F without. Returns the path and the payload starts (ms)."""
    rng = np.random.default_rng(1)
    n = int(FS * (0.1 + bursts * 0.06 + 0.1))
    t_ms = np.arange(n) * 1000.0 / FS
    freq, level = np.zeros(n), np.full(n, -200.0)
    starts = []
    pat = burstramp.PATTERNS["MS_VOICE"]
    for i in range(bursts):
        p0 = int(FS * (0.1 + i * 0.06))
        starts.append(p0 * 1000.0 / FS)
        sy = rng.choice([-3.0, -1.0, 1.0, 3.0], 132)
        if i % 6 == 0:
            sy[burstramp.SYNC_K:burstramp.SYNC_K + 24] = pat
        hold = np.repeat(sy, SPS) * DEV
        a, b = p0 - 3 * SPS, p0 + 132 * SPS + 3 * SPS
        freq[a:b] = np.concatenate((np.full(3 * SPS, sy[0] * DEV), hold, np.full(3 * SPS, sy[-1] * DEV)))
        stop = starts[-1] + 27.5 + FALL_DELAY_MS
        lo, hi = max(0, p0 - 20 * SPS), p0 + 140 * SPS
        level[lo:hi] = envelope_db(t_ms[lo:hi], starts[-1] + offset_ms, stop, shape)
    freq = np.convolve(freq, np.ones(5) / 5, "same")
    x = 10 ** (level / 20) * np.exp(2j * np.pi * np.cumsum(freq) / FS)
    x += 1e-3 / np.sqrt(2) * (rng.standard_normal(n) + 1j * rng.standard_normal(n))
    path = str(tmp_path / "x.cf32")
    np.array([x.real, x.imag], np.float32).T.tofile(path)
    json.dump({"fmt": "cf32", "fs": FS, "bw": 12500, "freq": 461e6, "center": 461e6, "t0": 1000.0}, open(path + ".json", "w"))
    return path, starts


def by_kind(res, kind):
    return res["summary"]["by_kind"][kind]


def test_overshoot_early_key_up(tmp_path):
    tau = 0.8
    path, starts = capture(tmp_path, -0.5, lambda u: 8 * np.exp(-u / tau))
    res = burstramp.analyze(path)
    assert res["summary"]["bursts"] == 14 and res["summary"]["unplaced"] == 0
    assert abs(res["bursts"][1]["t_s"] * 1000 - starts[1]) < 0.05  # B-F placed from the grid
    g = by_kind(res, "VOICE_B_F")
    assert g["rise_ms.-3"]["median"] == pytest.approx(-0.5 - 0.3 * 11 / 68, abs=0.06)
    assert g["rise_ms.-30"]["median"] == pytest.approx(-0.5 - 0.3 * 38 / 68, abs=0.06)
    assert g["overshoot_db"]["median"] == pytest.approx(8, abs=1.2)
    assert g["settle_1db_ms"]["median"] == pytest.approx(-0.5 + tau * np.log(8), abs=0.2)
    assert g["sym1_db"]["median"] == pytest.approx(8 * np.exp(-0.604 / tau), abs=0.5)
    assert g["fall_ms.-3"]["median"] == pytest.approx(FALL_DELAY_MS + 0.015, abs=0.06)
    assert g["fall_ms.-30"]["median"] == pytest.approx(FALL_DELAY_MS + 0.15, abs=0.06)
    assert g["settled_db"]["max"] - g["settled_db"]["min"] < 0.5 and abs(g["settled_db"]["median"]) < 0.3


def test_sag_is_measured(tmp_path):
    path, _ = capture(tmp_path, 0.0, lambda u: -3 * np.exp(-u / 0.5))
    g = by_kind(burstramp.analyze(path), "VOICE_B_F")
    assert g["sym1_db"]["median"] == pytest.approx(-3 * np.exp(-0.104 / 0.5), abs=0.5)
    assert g["err5_mean_db"]["median"] < -0.8
    assert g["rise_ms.-1"]["median"] == pytest.approx(0.5 * np.log(3), abs=0.12)  # the last rising through -1, after the sag


def test_late_key_up_keeps_its_edges(tmp_path):
    path, _ = capture(tmp_path, 4.0, lambda u: 0 * u)
    res = burstramp.analyze(path)
    g = by_kind(res, "VOICE_B_F")
    assert g["rise_ms.-3"]["median"] == pytest.approx(4.0 - 0.3 * 3 / 60, abs=0.06)
    assert g["rise_ms.-1"]["median"] == pytest.approx(4.0 - 0.3 * 1 / 60, abs=0.06)
    assert g["sym1_db"]["median"] >= -62.0  # the floor, not log of nothing


def test_first_burst_reported_apart(tmp_path):
    path, _ = capture(tmp_path, 0.0, lambda u: 0 * u)
    res = burstramp.analyze(path)
    assert [k for k in res["summary"]["by_kind"] if k.startswith("first:")] == ["first:VOICE_A"]
    assert res["bursts"][0]["first"] and not res["bursts"][1]["first"]
    assert res["summary"]["edge_res_us"] == pytest.approx(112, abs=2)
