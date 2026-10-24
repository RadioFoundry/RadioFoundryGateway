#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
"""
LR2021 experiments over a board's /api/lr2021/* endpoints (web.c): hold the chip, send it commands, capture its RX
FIFO flat out, and look at what came back. Needs numpy.

  lr2021_lab.py --board <board ip> rates 0x1C 0x13 0x22     route source 2 at each rate code; rate, losses, passband
  lr2021_lab.py --board <board ip> cmd 0101 --n 2           one command frame (hex) and its answer
  lr2021_lab.py --board <board ip> release                  give the chip back to the reader

The route command (0x0290) and its rate byte are described in docs/spec/lr2021-iq.md; the rate byte is read as the
FSK rx_bw byte: bits 2:0 log2(decimation / 4), bits 5:3 mode - 1, bits 7:6 a channel filter; the rate is
32 MHz / (mode x decimation). That reading is inferred, and this tool is how it gets checked.
"""
import argparse
import json
import sys
import urllib.request

import numpy as np

RX_FIFO_BASE, RX_FIFO_BASE_1K = 0xF30028, 0x804400
RX_FIFO_SIZE, RX_FIFO_SIZE_1K = 0xF30030, 0x3E8


class Board:
    def __init__(self, host):
        self.base = f"http://{host}/api/lr2021"

    def _req(self, path, method="POST", timeout=30):
        r = urllib.request.Request(self.base + path, method=method)
        with urllib.request.urlopen(r, timeout=timeout) as resp:
            return resp.read(), resp.headers

    def hold(self, keep=True):
        return self._req(f"/hold?on=1&keep={int(keep)}")[0].decode().strip()

    def release(self):
        return self._req("/hold?on=0")[0].decode().strip()

    def cmd(self, frame, n=0):
        body, _ = self._req(f"/cmd?f={frame.hex()}&n={n}")
        j = json.loads(body)
        if not j["ok"]:
            raise RuntimeError(f"{frame.hex()}: {j['error']} (status cmd {j['cmd']} mode {j['mode']})")
        return bytes.fromhex(j["rx"])

    # WriteRegMem32 / ReadRegMem32 take a 24-bit address
    def write32(self, addr, value):
        return self.cmd(bytes([0x01, 0x04]) + addr.to_bytes(3, "big") + value.to_bytes(4, "big"))

    def write32_mask(self, addr, mask, value):
        """WriteRegMem32Mask: only the bits in mask."""
        return self.cmd(bytes([0x01, 0x05]) + addr.to_bytes(3, "big") + mask.to_bytes(4, "big") +
                        value.to_bytes(4, "big"))

    def read32(self, addr, words=1):
        b = self.cmd(bytes([0x01, 0x06]) + addr.to_bytes(3, "big") + bytes([words]), 4 * words)
        return [int.from_bytes(b[4 * i:4 * i + 4], "big") for i in range(words)]

    def capture(self, n):
        body, h = self._req(f"/capture?n={n}", method="GET", timeout=60)
        return body, int(h.get("X-Overflows", -1)), int(h.get("X-Microseconds", 0))

    def spi(self, hz=0, delay=0):
        return self._req(f"/spi?hz={hz}&delay={delay}" if hz else "/spi")[0].decode().strip()


def route_frame(rate, source=2, dest=1, nco=0, width=1):
    """0x0290: rate, NCO (BE32), dest and source, a delay, a length, the FIFO width flag."""
    return bytes([0x02, 0x90, rate]) + nco.to_bytes(4, "big") + bytes([(dest << 3) | source, 0, 0, 0, 0, width])


def nominal_rate(code):
    if code == 0xFF:
        return None
    dec = 4 << (code & 7)
    mode = ((code >> 3) & 7) + 1
    return 32e6 / (mode * dec)


# bytes a sample by route source (the ROM's table at 0x13948): 2 is int8 I then Q, 4 is int16 I then Q (measured
# for sources 0, 1, 2, 3; source 6 has fixed bits, source 4 9-bit values in the top of int16)
SAMPLE_BYTES = (2, 2, 4, 4, 4, 4, 2)
FILTER = (5 / 13, 5 / 14, 5 / 15, 5 / 18)  # rx_bw bits 7:6: the channel filter's share of fs (inferred)


def fsk_frame(rx_bw):
    """SetFskModulationParams for a given rx_bw: a bitrate and deviation well inside its filter (not used)."""
    fs = nominal_rate(rx_bw)
    bw = fs * FILTER[rx_bw >> 6]
    br = max(600, int(bw / 4))
    return (bytes([0x02, 0x40]) + br.to_bytes(4, "big") + bytes([0x00, rx_bw]) + (br // 2).to_bytes(3, "big"))


def start(b, rate, source=2, fsk=False, gain=None, dio=None):
    b.cmd(bytes([0x01, 0x28, 0x00]))                     # SetStandby RC
    for pin, level in (dio or {}).items():               # SetDioFunction (Standby RC only): GPIO low / high
        b.cmd(bytes([0x01, 0x12, pin, (0x6 if level else 0x5) << 4]))
    if fsk:
        b.cmd(bytes([0x02, 0x07, 0x02]))                 # SetPacketType FSK
        b.cmd(fsk_frame(rate))                           # rx_bw = the rate code
        rate = 0xFF                                      # the route follows the modem
    b.cmd(route_frame(rate, source))
    b.write32(RX_FIFO_BASE, RX_FIFO_BASE_1K)
    b.write32(RX_FIFO_SIZE, RX_FIFO_SIZE_1K)
    b.cmd(bytes([0x01, 0x14, 0x3F, 0x3F]))               # ClearFifoIrqFlags
    b.cmd(bytes([0x02, 0x0C, 0xFF, 0xFF, 0xFF]))         # SetRx, continuous
    b.cmd(bytes([0x01, 0x1E]))                           # ClearRxFifo
    if gain is not None:
        b.cmd(bytes([0x02, 0x1A, gain]))                 # SetAgcGainManual, 1 to 13, 0 the AGC: in RX (SetRx sets 13)


def passband(iq, rate, nfft=1024):
    """Welch PSD (dB), centered; and the width where the noise sits within 3 dB of its middle."""
    seg = len(iq) // nfft
    x = iq[: seg * nfft].reshape(seg, nfft) * np.hanning(nfft)
    p = np.fft.fftshift((np.abs(np.fft.fft(x, axis=1)) ** 2).mean(axis=0))
    db = 10 * np.log10(p + 1e-12)
    f = (np.arange(nfft) - nfft // 2) * rate / nfft
    mid = np.median(db[nfft // 2 - nfft // 16: nfft // 2 + nfft // 16])
    inside = np.where(db > mid - 3)[0]
    # the contiguous run around the center
    c = nfft // 2
    lo = hi = c
    while lo > 0 and db[lo - 1] > mid - 3:
        lo -= 1
    while hi < nfft - 1 and db[hi + 1] > mid - 3:
        hi += 1
    return f, db, (f[hi] - f[lo]), mid


def rssi(b):
    """GetRssiInst, dBm (9 bits, -Rssi / 2)."""
    r = b.cmd(bytes([0x02, 0x0B]), 2)
    return -((r[0] << 1) | (r[1] >> 7)) / 2


def channel_snr(iq, rate, half_bw=6000, nfft=4096):
    """The strongest carrier's power within +-half_bw over the median noise in the same width, in dB; and its
    offset."""
    seg = len(iq) // nfft
    x = iq[: seg * nfft].reshape(seg, nfft) * np.hanning(nfft)
    p = np.fft.fftshift((np.abs(np.fft.fft(x, axis=1)) ** 2).mean(axis=0))
    f = (np.arange(nfft) - nfft // 2) * rate / nfft
    # the carrier: the strongest bin outside DC
    q = p.copy()
    q[np.abs(f) < 2000] = 0
    k = int(np.argmax(q))
    inside = np.abs(f - f[k]) <= half_bw
    passband = np.abs(f) < 0.15 * rate
    noise = np.median(p[passband & ~(np.abs(f - f[k]) <= 3 * half_bw)])
    sig = p[inside].sum() - noise * inside.sum()
    return 10 * np.log10(max(sig, 1e-12) / (noise * inside.sum())), f[k]


def cmd_frontend(b, args):
    """Every level of DIO9, 10 and 11 (GPIO outputs): RSSI and the carrier's in-channel SNR."""
    print(b.hold(keep=True))
    try:
        for combo in range(8):
            dio = {9: combo & 1, 10: (combo >> 1) & 1, 11: (combo >> 2) & 1}
            start(b, args.code, 1, True, args.gain, dio)
            b.capture(4096)                              # let the AGC settle
            r = rssi(b)
            body, ovf, us = b.capture(args.bytes)
            d = np.frombuffer(body, np.int8).astype(float)
            iq = d[0::2] + 1j * d[1::2]
            u, d2 = nominal_rate(args.code), None
            snr, off = channel_snr(iq, u)
            print(f"DIO9 {dio[9]} DIO10 {dio[10]} DIO11 {dio[11]}: RSSI {r:6.1f} dBm, carrier at {off / 1e3:+6.1f} kHz"
                  f" SNR {snr:5.1f} dB, rms {np.sqrt(np.mean(np.abs(iq) ** 2)):.1f}, overflows {ovf}")
    finally:
        print(b.release())


def cmd_rates(b, args):
    print(b.hold(keep=True))
    try:
        for code in args.codes:
            code = int(code, 0)
            start(b, code, args.source, args.fsk, args.gain)
            body, ovf, us = b.capture(args.bytes)
            nb = len(body)
            width = SAMPLE_BYTES[args.source]
            rate = nb / width / (us / 1e6) if us else 0
            if not nb:
                print(f"code 0x{code:02X} source {args.source}: no samples in {us / 1e3:.0f} ms")
                continue
            d = np.frombuffer(body, "<i2" if width == 4 else np.int8).astype(float)
            iq = d[0::2] + 1j * d[1::2]
            nom = nominal_rate(code)
            # with overflows the measured rate is the SPI's, not the chip's: trust the nominal one then
            fs = nom if (ovf and nom) else rate
            f, db, width, mid = passband(iq, fs)
            print(f"code 0x{code:02X} source {args.source}: {nb} bytes in {us / 1e3:.1f} ms = {rate / 1e3:.1f} kS/s"
                  f" (nominal {nom / 1e3 if nom else float('nan'):.1f}), overflows {ovf}; rms {np.sqrt(np.mean(np.abs(iq) ** 2)):.1f},"
                  f" max {np.abs(d).max():.0f}; noise passband (3 dB) {width / 1e3:.1f} kHz = {width / fs:.2f} of the rate")
            if args.save:
                np.save(f"{args.save}-0x{code:02X}.npy", iq)
                np.savetxt(f"{args.save}-0x{code:02X}-psd.txt", np.c_[f, db], fmt="%.1f")
    finally:
        if not args.keep_held:
            print(b.release())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--board", required=True, help="the board's address")
    sub = ap.add_subparsers(dest="what", required=True)
    r = sub.add_parser("rates")
    r.add_argument("codes", nargs="+")
    r.add_argument("--source", type=int, default=2)
    r.add_argument("--bytes", type=int, default=262144)
    r.add_argument("--save", help="file prefix for the samples (.npy) and PSDs")
    r.add_argument("--keep-held", action="store_true")
    r.add_argument("--gain", type=int, help="SetAgcGainManual's step (1 to 13; 0 the AGC)")
    r.add_argument("--fsk", action="store_true", help="the FSK modem with rx_bw = the code; the route at 0xFF")
    fe = sub.add_parser("frontend")
    fe.add_argument("--code", type=lambda v: int(v, 0), default=0x12)
    fe.add_argument("--gain", type=int)
    fe.add_argument("--bytes", type=int, default=131072)
    c = sub.add_parser("cmd")
    c.add_argument("frame")
    c.add_argument("--n", type=int, default=0)
    sub.add_parser("release")
    s = sub.add_parser("spi")
    s.add_argument("--hz", type=int, default=0)
    s.add_argument("--delay", type=int, default=0)
    args = ap.parse_args()
    b = Board(args.board)
    if args.what == "rates":
        cmd_rates(b, args)
    elif args.what == "frontend":
        cmd_frontend(b, args)
    elif args.what == "cmd":
        print(b.cmd(bytes.fromhex(args.frame), args.n).hex())
    elif args.what == "release":
        print(b.release())
    elif args.what == "spi":
        print(b.spi(args.hz, args.delay))


if __name__ == "__main__":
    sys.exit(main())
