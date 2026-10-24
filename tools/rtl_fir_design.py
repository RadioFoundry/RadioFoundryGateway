#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
"""The RTL2832U's programmable DDC low-pass: design, quantize, print (docs/spec/rtlsdr.md, D1.9 and D1.10).

The chip takes 16 coefficients, the first half of a symmetric 32-tap FIR: c0..c7 as signed 8-bit, c8..c15 as
signed 12-bit, unity gain at a sum of 4096 over the 32 taps. Which rate the filter runs at isn't documented; the
design assumes the ADC's 28.8 MHz (to measure, spec G.1).

Target: flat over the widest band the driver streams (3.2 MS/s, 80% usable: +/-1.28 MHz), as much attenuation as
32 taps give from 3 MHz. At any rate the filter really runs at, it is wider than the chip's usual default (printed
for comparison), so it never cuts more of the band than that default would.

Prints the taps as a C initializer, the 20 register bytes, and both responses.

    python3 tools/rtl_fir_design.py
"""
import numpy as np
from scipy import signal

FS = 28.8e6        # assumed rate of the FIR (G.1)
PASS_HZ = 1.3e6    # flat to here: 80% of 3.2 MS/s is +/-1.28 MHz
STOP_HZ = 3.0e6    # attenuate from here
NTAPS = 32
SCALE = 4096       # sum of the 32 taps at unity gain (D1.9)

# The chip's usual default, as written by webrtlsdr (rtl2832u.ts), for comparison only.
DEFAULT_BYTES = bytes([0xCA, 0xDC, 0xD7, 0xD8, 0xE0, 0xF2, 0x0E, 0x35, 0x06, 0x50,
                       0x9C, 0x0D, 0x71, 0x11, 0x14, 0x71, 0x74, 0x19, 0x41, 0xA5])


def pack(c):
    """16 coefficients to the 20 register bytes (D1.9)."""
    assert len(c) == 16
    assert all(-128 <= x <= 127 for x in c[:8]) and all(-2048 <= x <= 2047 for x in c[8:])
    out = [x & 0xFF for x in c[:8]]
    for i in range(8, 16, 2):
        a, b = c[i] & 0xFFF, c[i + 1] & 0xFFF
        out += [a >> 4, ((a & 0xF) << 4) | (b >> 8), b & 0xFF]
    return bytes(out)


def unpack(raw):
    """The 20 register bytes to 16 signed coefficients."""
    s = lambda v, bits: v - (1 << bits) if v >= 1 << (bits - 1) else v
    c = [s(x, 8) for x in raw[:8]]
    for i in range(4):
        b0, b1, b2 = raw[8 + 3 * i: 11 + 3 * i]
        c += [s((b0 << 4) | (b1 >> 4), 12), s(((b1 & 0xF) << 8) | b2, 12)]
    return c


def design():
    h = signal.remez(NTAPS, [0, PASS_HZ, STOP_HZ, FS / 2], [1, 0], weight=[1, 10], fs=FS)
    # unity gain at DC (the narrow rates sit there), then round
    q = np.round(h / np.sum(h) * SCALE).astype(int)
    assert all(q[i] == q[NTAPS - 1 - i] for i in range(NTAPS // 2)), "not symmetric"
    return [int(x) for x in q[:16]]


def response(c):
    full = np.array(list(c) + list(c[::-1])) / SCALE
    w, H = signal.freqz(full, worN=8192, fs=FS)
    return w, 20 * np.log10(np.abs(H) + 1e-12)


def report(name, c):
    w, db = response(c)
    pb = db[w <= PASS_HZ]
    sb = db[w >= STOP_HZ]
    edge = w[np.argmax(db < -3)]
    print(f"{name}: sum {2 * sum(c)}, passband 0..{PASS_HZ / 1e6:.2f} MHz {pb.min():+.2f} to {pb.max():+.2f} dB, "
          f"-3 dB at {edge / 1e6:.3f} MHz ({edge / FS:.4f} of the FIR's rate), "
          f"worst from {STOP_HZ / 1e6:.1f} MHz {sb.max():.1f} dB")
    for f in (0, 0.144e6, 0.5e6, 1.0e6, 1.28e6, 1.6e6, 2.0e6, 3.0e6, 5.0e6, 8.0e6, 14.4e6):
        i = min(int(round(f / (FS / 2) * (len(w) - 1))), len(w) - 1)
        print(f"  {f / 1e6:6.3f} MHz {db[i]:7.2f} dB")


def main():
    c = design()
    raw = pack(c)
    assert unpack(raw) == c
    print("/* tools/rtl_fir_design.py: 32-tap symmetric low-pass, %.2f MHz flat, from %.1f MHz down, at an assumed "
          "%.1f MHz */" % (PASS_HZ / 1e6, STOP_HZ / 1e6, FS / 1e6))
    print("static const int16_t fir_taps[16] = {" + ", ".join(str(x) for x in c) + "};")
    print("register bytes (page 1, 0x1C..0x2F): " + " ".join(f"{b:02X}" for b in raw))
    print()
    report("designed", c)
    print()
    report("default (webrtlsdr's bytes)", unpack(DEFAULT_BYTES))


if __name__ == "__main__":
    main()
