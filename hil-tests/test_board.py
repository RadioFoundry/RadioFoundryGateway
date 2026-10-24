# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
"""Hardware-in-the-loop checks of a running esp_idf board over its HTTP API and bench API v1.

The host tests (host-tests) cover the portable C; this covers the ESP glue they can't reach (web.c, the bench
API, sdr_task, the netclock and wifilist HTTP handlers) against the specs: docs/spec/fw/interfaces-http.md and
docs/spec/bench-api.md.

    RDOF_BOARD=<board ip> python -m pytest -q hil-tests

Skipped when RDOF_BOARD is unset. Nothing here transmits or changes the board's settings. Sessions it opens are its
own and end at the end of each test. RDOF_BOARD_MUTATE=1 adds the checks that send bad requests to POST endpoints
that store things (the board must refuse them and store nothing), erasing an empty core dump, and on a
CONFIG_RDOF_CRASH_TEST build a crash on purpose (it reboots the board); neither touches a crash the board holds.
"""

import base64
import json
import os
import struct
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "bench"))
from benchapi import API, Esp32Bench, iq_header, ws_messages  # noqa: E402

BOARD = os.environ.get("RDOF_BOARD", "")
pytestmark = pytest.mark.skipif(not BOARD, reason="RDOF_BOARD is not set (the board's address)")
MUTATE = os.environ.get("RDOF_BOARD_MUTATE") == "1"
URL = BOARD if "://" in BOARD else f"http://{BOARD}"


def request(method, path, body=None, headers=None):
    """(status, headers, body bytes); HTTP errors are returned, not raised."""
    data = body if isinstance(body, (bytes, type(None))) else json.dumps(body).encode()
    req = urllib.request.Request(URL + path, data=data, method=method, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, r.headers, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.headers, e.read()


def get_json(path):
    status, hdrs, body = request("GET", path)
    assert status == 200, f"GET {path}: {status} {body[:200]!r}"
    return json.loads(body)


def has(d, spec):
    """Every key in spec is in d with a value of that type (a tuple of types allowed)."""
    for k, t in spec.items():
        assert k in d, f"missing {k!r} in {sorted(d)}"
        assert isinstance(d[k], t), f"{k}: {type(d[k]).__name__} {d[k]!r}, expected {t}"


NUM = (int, float)
OPT_NUM = (int, float, type(None))


@pytest.fixture(scope="module")
def status():
    return get_json("/api/status")


@pytest.fixture(scope="module")
def info():
    return get_json(API + "/info")


@pytest.fixture
def bench():
    return Esp32Bench(URL)


# --- the board's own endpoints ---------------------------------------------------------------------------------


def test_page_is_gzipped_html():
    status, hdrs, body = request("GET", "/")
    assert status == 200
    assert hdrs["Content-Encoding"] == "gzip"
    assert hdrs["Content-Type"].startswith("text/html")
    assert body[:2] == b"\x1f\x8b"


def test_status_fields(status):
    has(status, {"name": str, "version": str, "idf": str, "uptime_s": int, "reset": str, "heap_free": int,
                 "settings_ok": bool, "settings_error": str, "wifi": bool, "ap_mode": bool, "ip": str,
                 "clock": str, "build_roamer": bool, "build_sdr": bool})
    assert status["settings_ok"] == (status["settings_error"] == "")
    # The W12 (LR2021 front end) streams with about 10-11 KB of internal RAM free; the others keep far more.
    floor = 8192 if (status.get("sdr") or {}).get("frontend") == "lr2021" else 16384
    assert status["heap_free"] > floor, f"internal heap is nearly gone ({status['heap_free']} B)"


def test_status_headers_and_size():
    code, hdrs, body = request("GET", "/api/status")
    assert code == 200
    assert hdrs["Content-Type"].startswith("application/json")
    assert hdrs["Cache-Control"] == "no-store"
    assert len(body) <= 6144


def test_crash_report(status):
    crash = status.get("crash")
    if crash is None:
        pytest.skip("no core dump in flash")
    if "error" in crash:
        has(crash, {"size": int, "error": str})
        return
    has(crash, {"task": str, "pc": str, "backtrace": str})
    assert crash["pc"].startswith("0x") and len(crash["pc"]) == 10
    assert all(a.startswith("0x") for a in crash["backtrace"].split())
    if "elf" not in crash:
        pytest.skip("firmware older than the crash report's elf field")
    has(crash, {"elf": str, "this_image": bool})
    int(crash["elf"], 16)
    if "exccause" not in crash:
        pytest.skip("firmware older than the crash report's cause")
    has(crash, {"size": int, "exccause": int, "cause": str, "excvaddr": str, "backtrace_corrupted": bool})
    assert crash["excvaddr"].startswith("0x") and len(crash["excvaddr"]) == 10
    assert crash["cause"] != "unknown", crash["exccause"]
    assert isinstance(crash.get("details", ""), str)


def coredump_check(status):
    """GET /api/coredump: the dump /api/status describes (its size, an ELF core inside), or 404 "no core dump"."""
    code, hdrs, body = request("GET", "/api/coredump")
    crash = status.get("crash")
    if code == 404:
        assert body.startswith(b"no core dump"), body
        assert crash is None, "status has a crash the download doesn't"
        return None
    assert code == 200, (code, body[:200])
    assert hdrs["Content-Type"] == "application/octet-stream"
    assert "attachment" in hdrs.get("Content-Disposition", "")
    assert crash is not None, "a core dump to download, none in status"
    size, = struct.unpack_from("<I", body)
    assert size == len(body) == crash.get("size", len(body))
    if "error" not in crash:
        assert body[24:28] == b"\x7fELF", "no ELF core after the header"
    return body


def test_coredump_download(status):
    if coredump_check(status) is None and status.get("crash") is None:
        pytest.skip("no core dump in flash (the download is 404, as it should be)")


def test_sdr_status(status):
    if not status["build_sdr"] or "sdr" not in status:
        pytest.skip("no SDR in this build or settings")
    sdr = status["sdr"]
    has(sdr, {"name": str, "started": bool, "frontend": str, "receiving": bool, "sample_rate": int,
              "center_hz": NUM, "gain_db": OPT_NUM, "cpu_load": NUM, "usb_errors": int, "bytes_lost": int,
              "channels": list, "ppm": dict})
    assert 0 <= sdr["cpu_load"] <= 1.5
    for c in sdr["channels"]:
        has(c, {"id": str, "name": str, "freq": NUM, "bursts": int, "rssi": NUM, "afc": NUM, "in": bool})
    has(sdr["ppm"], {"learning": bool, "learned": OPT_NUM, "by": str, "settled": bool})  # null until learned


def test_resolved_matches_status(status):
    code, hdrs, body = request("GET", "/api/resolved")
    assert code == 200
    assert hdrs["Content-Type"].startswith("text/plain")
    assert len(body) <= 5120
    text = body.decode()
    assert f"{status['name']} runs:" in text
    if "sdr" in status:
        for c in status["sdr"]["channels"]:
            assert f"channel {c['id']} " in text


def test_config_round_trips_as_json():
    code, hdrs, body = request("GET", "/api/config?download=1")
    assert code == 200
    assert "attachment" in hdrs.get("Content-Disposition", "")
    assert hdrs["Content-Disposition"].endswith('.json"')
    assert body.strip().startswith(b"{")


def test_log_is_text():
    code, hdrs, body = request("GET", "/api/log")
    assert code == 200
    assert hdrs["Content-Type"].startswith("text/plain")
    assert len(body) <= 64 * 1024


def test_spectrum(status):
    if "sdr" not in status:
        pytest.skip("no SDR")
    # The board computes the spectrum for 10 s after a request: the first one after an idle spell has no rows yet.
    deadline = time.monotonic() + 3
    while True:
        d = get_json("/api/spectrum?since=0")
        has(d, {"seq": int, "bins": int, "row_ms": int, "rate": NUM, "center": NUM, "rows": list})
        if d["rows"] or time.monotonic() > deadline:
            break
        time.sleep(d["row_ms"] / 1000)
    assert d["rows"], "no rows: the spectrum isn't being computed"
    for r in d["rows"]:
        assert len(base64.b64decode(r)) == d["bins"]
    later = get_json(f"/api/spectrum?since={d['seq']}")
    assert later["seq"] >= d["seq"]
    assert len(later["rows"]) <= len(d["rows"]) + 10


def test_rtl_regs(status):
    code, _, body = request("GET", "/api/rtl/regs")
    if status.get("sdr", {}).get("frontend") == "rtlsdr":
        assert code == 200
        json.loads(body)
    else:
        assert (code, body) == (404, b"no dongle")


def test_unknown_path_is_404():
    assert request("GET", "/api/no-such-thing")[0] == 404


# --- netclock and wifilist ---------------------------------------------------------------------------------------


def test_clock(status):
    c = get_json("/api/clock")
    has(c, {"source": str, "synced": bool, "poll_s": int, "utc_ms": int, "sources": list, "log": list})
    assert c["source"] == status["clock"]
    if c["synced"]:
        assert abs(c["utc_ms"] / 1000 - time.time()) < 5, "board's UTC is off by more than 5 s"


def test_clock_settings():
    s = get_json("/api/clock/settings")
    has(s, {"servers": (str, list), "poll_s": int})


def test_old_browser_time_is_not_applied():
    code, _, body = request("POST", "/api/time", {"utc_ms": 1_000_000_000_000})
    assert code == 200
    d = json.loads(body)
    assert d["applied"] is False
    assert isinstance(d["source"], str)


def test_wifi(status):
    w = get_json("/api/wifi")
    has(w, {"state": str, "connected": bool, "ip": str, "networks": list, "seen": list, "fails": list})
    assert w["connected"] == status["wifi"]
    for n in w["networks"]:
        has(n, {"ssid": str, "priority": int, "hidden": bool, "password_set": bool})
        assert "password" not in n, "a stored password is on the wire"


def test_component_scripts_are_served():
    for path in ("/netclock.js", "/wifilist.js"):
        code, hdrs, body = request("GET", path)
        assert code == 200, path
        assert hdrs["Content-Encoding"] == "gzip"


# --- bench API v1 ------------------------------------------------------------------------------------------------


def test_info(info, status):
    has(info, {"api": str, "device": dict, "sdr": dict, "channels": list, "radios": list, "tx": dict,
               "caps": dict, "limits": dict, "clock": dict})
    assert info["api"] == "v1"
    assert info["device"]["version"] == status["version"]
    has(info["caps"], {"spectrum": bool, "bursts": bool, "calls": bool, "iq": list, "tx": bool,
                       "radio_control": bool})
    has(info["limits"], {"streams": int, "preroll_s": NUM})
    w = info["sdr"].get("window")
    for c in info["channels"]:
        has(c, {"id": str, "freq_hz": NUM, "caps": list, "in_band": bool})
        if w and c["in_band"]:
            assert w["lo_hz"] <= c["freq_hz"] <= w["hi_hz"]


def test_bench_status_matches_info(info):
    s = get_json(API + "/status")
    has(s, {"device": dict, "sdr": dict, "channels": list, "sessions": list, "streams": list, "tx": dict,
            "clock": dict})
    assert {c["id"] for c in s["channels"]} == {c["id"] for c in info["channels"]}
    assert s["tx"]["state"] in ("idle", "armed", "keyed")
    assert s["clock"]["run"] == info["clock"]["run"]


def test_clock_block(info):
    c = info["clock"]
    has(c, {"run": int, "rate": NUM, "sc0": int, "t0_us": int, "ppm": OPT_NUM, "utc_us": int, "synced": bool})
    if c["synced"]:
        assert abs(c["utc_us"] / 1e6 - time.time()) < 5


def test_sdr_endpoint(info):
    s = get_json(API + "/sdr")
    has(s, {"frontend": str, "rate": NUM, "center_hz": NUM, "receiving": bool})
    assert s["center_hz"] == info["sdr"]["center_hz"]
    assert s["retune"] is None, "a retune is in force: another tool holds the bench"


def test_session_lifecycle(bench):
    s = bench.session("hil-test", ttl_s=20)
    sid = s["session"]
    try:
        assert s["ttl_s"] == 20
        got = bench.renew(sid)
        has(got, {"session": str, "label": str, "ttl_s": NUM, "expires_in_s": NUM})
        assert got["label"] == "hil-test"
        assert got["lease"] is None
        assert sid in json.dumps(get_json(API + "/status")["sessions"])
    finally:
        bench.end_session(sid)
    code, _, body = request("GET", f"{API}/session/{sid}")
    assert code == 404


def test_receive_lease(bench, info):
    if not info["channels"]:
        pytest.skip("no channels")
    f = info["channels"][0]["freq_hz"]
    sid = bench.session("hil-lease", ttl_s=20)["session"]
    try:
        lease = bench.lease(sid, [f])
        assert lease.get("lease") or lease.get("freq_hz"), lease
        got = bench.renew(sid)["lease"]
        assert got and got["freq_hz"] == [f] and got["tx"] is False
        code, _, body = request("DELETE", f"{API}/session/{sid}/lease")
        assert code == 200 and json.loads(body) == {"ok": True}
        assert bench.renew(sid)["lease"] is None
    finally:
        bench.end_session(sid)


def test_unknown_session_is_404():
    code, _, body = request("GET", f"{API}/session/s-nope")
    assert code == 404


def test_tune_without_lease_is_refused():
    code, _, body = request("PUT", f"{API}/sdr?session=s-nope", {"gain_db": "auto"})
    assert code in (403, 404, 409), body


# --- streams -----------------------------------------------------------------------------------------------------


def text_frames(msgs):
    return [json.loads(m) for m in msgs if isinstance(m, str)]


def test_spectrum_stream(info):
    msgs = list(ws_messages(URL, API + "/spectrum/ws", {"seconds": 1.5}, 12))
    texts = text_frames(msgs)
    assert texts[0]["type"] == "start"
    assert texts[-1]["type"] == "done"
    rows = [m for m in msgs if isinstance(m, bytes)]
    assert rows, "no rows in 1.5 s"
    for r in rows:
        magic, bins, row_ms, sc = struct.unpack_from("<4sHHQ", r)
        assert magic == b"RDSP" and len(r) == 16 + bins


def test_bursts_stream(bench, info, status):
    if not info["caps"]["bursts"]:
        pytest.skip("no burst stream")
    active = [c for c in status.get("sdr", {}).get("channels", []) if c["bursts"]]
    if not active:
        pytest.skip("no channel has had a burst")
    got = list(bench.bursts(channel=active[0]["id"], seconds=4, max_bursts=5))
    assert got, f"no bursts on {active[0]['id']} in 4 s"


def test_iq_stream(info):
    if "cs16" not in info["caps"]["iq"] or not info["channels"]:
        pytest.skip("no cs16 I/Q")
    f = info["channels"][0]["freq_hz"]
    msgs = list(ws_messages(URL, API + "/iq", {"freq_hz": f, "fmt": "cs16", "seconds": 0.5}, 12))
    texts = text_frames(msgs)
    start, done = texts[0], texts[-1]
    assert start["type"] == "start" and start["fmt"] == "cs16"
    assert done["type"] == "done"
    n = 0
    for m in msgs:
        if isinstance(m, bytes):
            h = iq_header(m)
            assert h["version"] == 1
            assert len(m) == 32 + h["n"] * 4
            n += h["n"]
    assert n == done["samples"]
    assert abs(n - 0.5 * start["rate"]) < 0.2 * start["rate"]


def test_stream_limit_frees_on_close(info):
    """Streams opened and closed one after another never run the board out of stream slots."""
    for _ in range(info["limits"]["streams"] + 2):
        texts = text_frames(ws_messages(URL, API + "/spectrum/ws", {"seconds": 0.3}, 8))
        assert texts and texts[0]["type"] == "start", texts
    assert get_json(API + "/status")["streams"] == []


# --- refusals that would store something (RDOF_BOARD_MUTATE=1) ----------------------------------------------------

mutate = pytest.mark.skipif(not MUTATE, reason="RDOF_BOARD_MUTATE=1 not set")


@mutate
def test_bad_config_is_refused_and_not_stored():
    before = request("GET", "/api/config")[2]
    uptime = get_json("/api/status")["uptime_s"]
    for body in (b"", b"{not json", b'{"radios": 7}'):
        code, _, text = request("POST", "/api/config", body)
        assert code == 400, (body, code, text)
    time.sleep(2)
    assert get_json("/api/status")["uptime_s"] >= uptime, "the board restarted"
    assert request("GET", "/api/config")[2] == before


def wait_for_boot(timeout=60):
    time.sleep(5)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            return get_json("/api/status")
        except (OSError, AssertionError):
            time.sleep(2)
    raise AssertionError("the board didn't come back")


@mutate
def test_crash_round_trip():
    """POST /api/crash (CONFIG_RDOF_CRASH_TEST builds only): the dump is saved, described, downloaded, then erased.
    Skipped when the board holds a crash of its own, so a real one is never overwritten."""
    before = get_json("/api/status")
    if before.get("crash") and before["crash"].get("task") != "crash_test":
        pytest.skip("the board holds a real crash: download it and erase it first")
    code, _, body = request("POST", "/api/crash")
    if code == 404:
        pytest.skip("not a CONFIG_RDOF_CRASH_TEST build")
    assert code == 200, body
    after = wait_for_boot()
    assert after["reset"] == "panic"
    crash = after["crash"]
    assert crash["task"] == "crash_test" and crash["this_image"] is True
    assert (crash["exccause"], crash["cause"], crash["excvaddr"]) == (29, "StoreProhibited", "0x00000000")
    assert coredump_check(after) is not None
    code, _, body = request("DELETE", "/api/coredump")
    assert (code, body) == (200, b"erased")
    assert "crash" not in get_json("/api/status")
    assert request("GET", "/api/coredump")[0] == 404


@mutate
def test_coredump_erase_when_empty():
    """DELETE /api/coredump with nothing stored. Skipped while a dump is there: erasing a real one loses it."""
    if "crash" in get_json("/api/status"):
        pytest.skip("a core dump is stored: download it before erasing it by hand")
    code, _, body = request("DELETE", "/api/coredump")
    assert (code, body) == (200, b"erased")
    assert request("GET", "/api/coredump")[0] == 404


@mutate
def test_bad_wifi_list_is_refused():
    before = get_json("/api/wifi")["networks"]
    code, _, text = request("POST", "/api/wifi/networks", b"[{")
    assert code == 400, text
    assert get_json("/api/wifi")["networks"] == before


@mutate
def test_bad_clock_settings_are_refused():
    before = get_json("/api/clock/settings")
    code, _, text = request("POST", "/api/clock/settings", {"poll_s": -5})
    assert code == 400, text
    assert get_json("/api/clock/settings") == before
