// SPDX-License-Identifier: MIT AND ISC
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! AMBE+2 channel frames (rdof/ambe.h): Golay(23,12) and the frame decode. A port of common/src/ambe.c (the tables and the decode from dsd / mbelib, ISC: PROVENANCE.md).

use core::ffi::c_int;

use crate::bits::{bits_to_bytes, bits_to_u64};

// De-interleave: channel-frame dibit i -> (row, bit) for its high and low bit.
const RW: [u8; 36] =
    [0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2];
const RX: [u8; 36] = [
    23, 10, 22, 9, 21, 8, 20, 7, 19, 6, 18, 5, 17, 4, 16, 3, 15, 2, 14, 1, 13, 0, 12, 10, 11, 9, 10, 8, 9, 7, 8, 6, 7,
    5, 6, 4,
];
const RY: [u8; 36] =
    [0, 2, 0, 2, 0, 2, 0, 2, 0, 3, 0, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3];
const RZ: [u8; 36] = [
    5, 3, 4, 2, 3, 1, 2, 0, 1, 13, 0, 12, 22, 11, 21, 10, 20, 9, 19, 8, 18, 7, 17, 6, 16, 5, 15, 4, 14, 3, 13, 2, 12,
    1, 11, 0,
];

/// Channel-frame bit i -> its place in the four rows, row * 32 + bit (from RW..RZ).
const DST: [u8; 72] = {
    let mut d = [0; 72];
    let mut i = 0;
    while i < 36 {
        d[2 * i] = RW[i] * 32 + RX[i];
        d[2 * i + 1] = RY[i] * 32 + RZ[i];
        i += 1;
    }
    d
};

/// Byte v of a channel frame -> its two bits of each stream (see rows): A's in bits 24-25, B's in 16-17, C's in
/// 8-9, D's in 0-1, the earlier bit high.
const SPLIT: [u32; 256] = {
    let mut t = [0; 256];
    let mut v = 0;
    while v < 256 {
        let mut x = 0;
        while x < 4 {
            // frame bits 4m + x and 4(m + 1) + x are the byte's bits 7 - x and 3 - x
            let two = (v >> (7 - x) & 1) << 1 | (v >> (3 - x) & 1);
            t[v] |= (two as u32) << (24 - 8 * x);
            x += 1;
        }
        v += 1;
    }
    t
};

/// The four rows as words, the C's fr[r][j] as bit j of rows[r]: they're Golay words and parameter bits next.
/// The interleave (RW..RZ) is regular: frame bit 4m + x, m = 0..17, is bit 17 - m of stream x (A..D), and
/// A is row 0's bits 23..6; B row 0's 5..0, then row 1's 22..11; C row 1's 10..0, then row 2's 10..4; D row 2's
/// 3..0, then row 3's 13..0. SPLIT sorts a byte's bits into the streams: byte 0 gives their bits 17-16, bytes 1-4
/// bits 15..8 and bytes 5-8 bits 7..0, eight bits a stream in a word.
fn rows(f: &[u8; 9]) -> [u32; 4] {
    let s = |j: usize| SPLIT[usize::from(f[j])];
    let top = s(0);
    let mid = s(1) << 6 | s(2) << 4 | s(3) << 2 | s(4);
    let low = s(5) << 6 | s(6) << 4 | s(7) << 2 | s(8);
    let stream = |sh: u32| (top >> sh & 3) << 16 | (mid >> sh & 0xFF) << 8 | (low >> sh & 0xFF);
    let (a, b, c, d) = (stream(24), stream(16), stream(8), stream(0));
    [a << 6 | b >> 12, (b & 0xFFF) << 11 | c >> 7, (c & 0x7F) << 4 | d >> 14, d & 0x3FFF]
}

// Golay(23,12), generator x^11+x^10+x^6+x^5+x^4+x^2+1
const GOLAY_POLY: u32 = 0xC75;

/// golay_parity of six data bits, the low six (PARITY[0]) or the high (PARITY[1]): parity is linear, so a word's
/// is the XOR of its halves' (256 bytes, in place of the C's 12 shift-and-test steps).
const PARITY: [[u16; 64]; 2] = {
    let mut t = [[0; 64]; 2];
    let mut v = 0;
    while v < 64 {
        let mut i = 0;
        while i < 6 {
            if v >> i & 1 != 0 {
                t[0][v] ^= DATA_COL[i] as u16;
                t[1][v] ^= DATA_COL[6 + i] as u16;
            }
            i += 1;
        }
        v += 1;
    }
    t
};

/// The 11 parity bits of data12's low 12 bits (higher bits ignored, as by the C's division).
fn golay_parity(data12: u32) -> u32 {
    u32::from(PARITY[0][(data12 & 63) as usize] ^ PARITY[1][(data12 >> 6 & 63) as usize])
}

fn syndrome(word23: u32) -> u32 {
    golay_parity(word23 >> 11) ^ (word23 & 0x7FF)
}

/// The syndromes of the 12 data bits (bit 11 + i); a parity bit's (bit k < 11) is 1 << k.
const DATA_COL: [u32; 12] = {
    let mut col = [0; 12];
    let mut i = 0;
    while i < 12 {
        let mut reg = 1u32 << (11 + i);
        let mut k = 22;
        while k > 10 {
            if reg & (1 << k) != 0 {
                reg ^= GOLAY_POLY << (k - 11);
            }
            k -= 1;
        }
        col[i] = reg & 0x7FF;
        i += 1;
    }
    col
};

/// The error pattern of syndrome s. The code is perfect: every syndrome has exactly one pattern of up to 3 bits,
/// so any search that finds one finds the C's. By how many data bits it flips: none (the syndrome is the parity
/// bits flipped), then one, two, three, each leaving what's left of the syndrome as parity bits. At most 299 tries,
/// where trying every pattern of up to 3 of the 23 bits takes up to 1771.
#[allow(clippy::needless_range_loop)] // the bit numbers are the point
fn error_pattern(s: u32) -> u32 {
    if s.count_ones() <= 3 {
        return s;
    }
    for i in 0..12 {
        let t = s ^ DATA_COL[i];
        if t.count_ones() <= 2 {
            return 1 << (11 + i) | t;
        }
    }
    for i in 0..12 {
        for j in i + 1..12 {
            let t = s ^ DATA_COL[i] ^ DATA_COL[j];
            if t.count_ones() <= 1 {
                return 1 << (11 + i) | 1 << (11 + j) | t;
            }
        }
    }
    for i in 0..12 {
        for j in i + 1..12 {
            for k in j + 1..12 {
                if s == DATA_COL[i] ^ DATA_COL[j] ^ DATA_COL[k] {
                    return 1 << (11 + i) | 1 << (11 + j) | 1 << (11 + k);
                }
            }
        }
    }
    0 // unreachable: the code is perfect
}

#[no_mangle]
pub extern "C" fn rdof_golay2312_encode(data12: u32) -> u32 {
    (data12 << 11) | golay_parity(data12)
}

/// A Golay(23,12) word's 12 data bits, and how many of them were corrected (as mbelib counts).
#[inline(always)]
fn golay_decode(word23: u32) -> (u32, c_int) {
    let s = syndrome(word23);
    if s == 0 {
        return (word23 >> 11, 0); // clean, the usual case: no search, no count
    }
    let err = error_pattern(s);
    ((word23 ^ err) >> 11, (err >> 11).count_ones() as c_int)
}

#[no_mangle]
pub extern "C" fn rdof_golay2312_decode(word23: u32, errors: Option<&mut c_int>) -> u32 {
    let (data, e) = golay_decode(word23);
    if let Some(errors) = errors {
        *errors = e;
    }
    data
}

/// The body once for each listed i, a constant in each. opt-level "s" doesn't unroll loops, and in these the
/// counter, the branch and the variable shifts (two instructions on Xtensa) cost more than the work.
macro_rules! unroll {
    ($i:ident in [$($n:literal)+] $body:block) => {
        $({
            let $i: u32 = $n;
            $body
        })+
    };
}

/// C1's scrambling sequence (23 bits, the first for C1's bit 22) seeded from C0.
fn prng_mask(c0: u32) -> u32 {
    // The generator works mod 2^16 and gives its top bit: kept in a word's top half, the wrap is free and the bit
    // is bit 31.
    let (mut pr, mut mask) = (c0 << 20, 0);
    unroll!(_i in [0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22] {
        pr = pr.wrapping_mul(173).wrapping_add(13849 << 16);
        mask = mask << 1 | pr >> 31;
    });
    mask
}

#[no_mangle]
pub extern "C" fn rdof_ambe_decode_frame(frame72: &[u8; 9], params49: Option<&mut [u8; 49]>) -> c_int {
    let fr = rows(frame72);
    let (c0, e0) = golay_decode(fr[0] >> 1 & 0x7F_FFFF); // bit 0 is the Golay(24,12) extra parity
    let (c1, e1) = golay_decode(fr[1] & 0x7F_FFFF ^ prng_mask(c0));
    if let Some(p) = params49 {
        // a bit a byte, first bit high: C0 and C1 (12 bits each), then row 2's bits 10..0 and row 3's 13..0
        let (hi, lo) = (c0 << 12 | c1, fr[2] << 14 | fr[3]);
        unroll!(i in [0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23] {
            p[i as usize] = (hi >> (23 - i) & 1) as u8;
        });
        unroll!(i in [0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24] {
            p[24 + i as usize] = (lo >> (24 - i) & 1) as u8;
        });
    }
    e0 + e1
}

#[no_mangle]
pub extern "C" fn rdof_ambe_encode_frame(params49: &[u8; 49], frame72: &mut [u8; 9]) {
    let (c0, c1) = (bits_to_u64(&params49[..12]) as u32, bits_to_u64(&params49[12..24]) as u32);
    let w0 = rdof_golay2312_encode(c0);
    let mut fr = [w0 << 1 | (w0.count_ones() & 1), rdof_golay2312_encode(c1) ^ prng_mask(c0), 0, 0];
    for i in 0..11 {
        fr[2] |= u32::from(params49[24 + i] != 0) << (10 - i);
    }
    for i in 0..14 {
        fr[3] |= u32::from(params49[35 + i] != 0) << (13 - i);
    }
    let bits: [u8; 72] = core::array::from_fn(|i| (fr[usize::from(DST[i] >> 5) & 3] >> (DST[i] & 31) & 1) as u8);
    bits_to_bytes(&bits, frame72);
}
