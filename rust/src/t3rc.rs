// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! The outbound Reverse Channel (RC) burst field (docs/spec/fw/tier3.md section 3.9). ETSI TS 102 361-1: the RC
//! PDU (9.1.5, table 9.6) is 4 bits of RC info and a 7-bit CRC (B.3.13, G7 = x^7 + x^5 + x^2 + x + 1, no
//! inversion, initial 0) XORed with the RC mask 7Ah (B.3.12, table B.21); the 11 bits go through the RC single
//! burst BPTC (B.2.2.2: one Hamming (16,11,4) row, table B.16, over one odd parity row, interleave index =
//! index x 17 mod 32) into the 32 bits of the embedded field, with PI = 1 and LCSS single fragment in the EMB
//! (6.4.2, figure 6.8; the radio layer puts it there). The RC info values are 361-4's (table 6.32).

/// RC info (ETSI TS 102 361-4 table 6.32)
pub const RC_POWER_UP: u8 = 0b0000;
pub const RC_POWER_DOWN: u8 = 0b0001;
pub const RC_POWER_HIGHEST: u8 = 0b0010;
pub const RC_POWER_LOWEST: u8 = 0b0011;
pub const RC_CEASE: u8 = 0b0100; // cease transmission command
pub const RC_CEASE_REQUEST: u8 = 0b0101; // cease transmission request

const RC_MASK: u8 = 0x7A;

/// Table B.16's parity columns H(4)..H(0) for RC(10)..RC(0), first row first.
const H16_11: [u8; 11] = [
    0b10011, 0b11010, 0b11111, 0b11100, 0b01110, 0b10101, 0b01011, 0b10110, 0b11001, 0b01101, 0b00111,
];

/// B.3.13: x^7 M(x) mod G7(x) for the 4 info bits.
pub fn crc7(info: u8) -> u8 {
    let mut r = 0u8;
    for i in (0..4).rev() {
        let fb = ((r >> 6) ^ (info >> i)) & 1;
        r = (r << 1) & 0x7F;
        if fb != 0 {
            r ^= 0x27; // x^5 + x^2 + x + 1
        }
    }
    r
}

/// The 11 bits RC(10)..RC(0): the info, then its masked CRC.
fn rc11(info: u8) -> u16 {
    (u16::from(info & 0x0F) << 7) | u16::from(crc7(info & 0x0F) ^ RC_MASK)
}

fn hamming(rc11: u16) -> u8 {
    (0..11).filter(|&i| rc11 >> (10 - i) & 1 != 0).fold(0, |h, i| h ^ H16_11[i])
}

/// The 32 transmitted bits TX(31)..TX(0) (0/1 bytes, TX(31) first) of RC info.
pub fn rc_bits(info: u8) -> [u8; 32] {
    let rc = rc11(info);
    let h = hamming(rc);
    let mut row = [0u8; 16]; // RC(10)..RC(0), H(4)..H(0)
    for (i, b) in row.iter_mut().enumerate() {
        *b = if i < 11 { (rc >> (10 - i) & 1) as u8 } else { h >> (15 - i) & 1 };
    }
    let mut tx = [0u8; 32];
    for (i, &b) in row.iter().enumerate() {
        // top to bottom, left to right: index 2i is the row's bit, 2i + 1 the parity row's (its opposite)
        for (index, bit) in [(2 * i, b), (2 * i + 1, b ^ 1)] {
            tx[index * 17 % 32] = bit; // interleave index n is TX(31 - n)
        }
    }
    tx
}

/// RC info from the 32 transmitted bits, when the parity, the Hamming code and the CRC all check.
pub fn rc_decode(tx: &[u8; 32]) -> Option<u8> {
    let mut row = [0u8; 16];
    for (i, b) in row.iter_mut().enumerate() {
        let (x, p) = (tx[2 * i * 17 % 32], tx[(2 * i + 1) * 17 % 32]);
        if x == p {
            return None;
        }
        *b = x;
    }
    let rc = row[..11].iter().fold(0u16, |v, &b| v << 1 | u16::from(b));
    let h = row[11..].iter().fold(0u8, |v, &b| v << 1 | b);
    let info = (rc >> 7) as u8;
    (hamming(rc) == h && (rc & 0x7F) as u8 == crc7(info) ^ RC_MASK).then_some(info)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crc7_is_the_remainder() {
        // F7 = x^7 M mod G7, by long division with the full generator 1010_0111
        for m in 0..16u16 {
            let mut v = m << 7;
            for i in (7..11).rev() {
                if v >> i & 1 != 0 {
                    v ^= 0b1010_0111 << (i - 7);
                }
            }
            assert_eq!(crc7(m as u8), v as u8, "M = {m}");
        }
    }

    #[test]
    fn rc_round_trips_and_parity_is_opposite() {
        for info in 0..16u8 {
            let tx = rc_bits(info);
            assert_eq!(rc_decode(&tx), Some(info));
            // TX(31) is RC(10) (info bit 3); interleave index 17 (TX(14)) is PC(15), its opposite
            assert_eq!(tx[0], info >> 3 & 1);
            assert_eq!(tx[17], tx[0] ^ 1);
            let mut bad = tx;
            bad[5] ^= 1;
            assert_eq!(rc_decode(&bad), None, "one bit flipped breaks a column's parity");
        }
    }

    #[test]
    fn the_hamming_rows_are_table_b16() {
        // a single data bit gives that row's parity columns
        for i in 0..11 {
            assert_eq!(hamming(1 << (10 - i)), H16_11[i]);
        }
        // the code's minimum distance is 4: any two different codewords differ in 4 bits or more
        let cw = |d: u16| (u32::from(d) << 5) | u32::from(hamming(d));
        for a in 0..2048u16 {
            assert!(a == 0 || cw(a).count_ones() >= 4, "codeword of {a}");
        }
    }
}
