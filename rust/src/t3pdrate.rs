// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Packet data at any rate, a party's side (docs/spec/fw/tier3.md 3.16): a message's blocks taken in (ETSI TS 102
//! 361-1 8.2.2: rate 1/2, 3/4 or 1, one rate a message; a confirmed block placed by its serial number and kept only
//! when its CRC-9 holds), the answer to a confirmed message (361-1 8.2.2.3, table 8.3; 361-3 5.4.2.2: ACK, NACK when
//! every block is good but the CRC-32 fails, SACK with a C_RDATA block listing the blocks to retry) and the sender's
//! side of a SACK (361-3 5.4.1: the listed blocks again, after a C_HEAD with F = 0). The MS simulator uses it.

use crate::t3site::{block_len, crc32, DataHead, DataMsg, BLOCK_MAX, DPF_CONFIRMED, DPF_RESPONSE, MSG_BLOCKS};

/// Response classes and types (361-1 table 8.3).
pub const CLASS_ACK: u8 = 0;
pub const CLASS_NACK: u8 = 1;
pub const CLASS_SACK: u8 = 2;
pub const TYPE_ACK: u8 = 1;
pub const TYPE_NACK_CRC: u8 = 1; // the packet's CRC failed

/// Sends of a confirmed message at most (361-3 N_RtryLmt; the standard gives no value).
pub const TRIES: u8 = 4;

/// What a transmission came to, after its last block.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Taken {
    /// The message, its CRC-32 good.
    Ok,
    /// Confirmed: blocks still bad or missing; the bits are the good ones.
    Sack(u16),
    /// Every block taken, the CRC-32 bad.
    CrcFailed,
}

/// A message being taken in.
#[derive(Clone, Copy, Debug, Default)]
pub struct DataAsm {
    pub m: DataMsg,
    pub good: u16, // the blocks taken (confirmed: those whose CRC-9 held)
    pub left: u8,  // blocks still to come in this transmission
    next: u8,      // unconfirmed: the next block's place
    pub on: bool,  // a confirmed message held for a retry
}

impl DataAsm {
    /// A data header for this party (an unconfirmed or confirmed one): F = 1 starts a message; F = 0 is a selective
    /// retry of the confirmed message held (the same source and N(S)). false: no blocks to take.
    pub fn header(&mut self, h: &DataHead) -> bool {
        if !h.full {
            let held = self.on && h.dpf == DPF_CONFIRMED && h.src == self.m.head.src && h.ns == self.m.head.ns;
            self.left = if held { h.blocks.min(self.m.n) } else { 0 };
            return self.left > 0;
        }
        self.on = false;
        self.left = 0;
        if h.blocks == 0 || h.blocks as usize > MSG_BLOCKS {
            return false;
        }
        self.m = DataMsg { head: *h, n: h.blocks, ..Default::default() };
        self.good = 0;
        self.next = 0;
        self.left = h.blocks;
        self.on = h.dpf == DPF_CONFIRMED;
        true
    }

    /// A data block of data type dt (its octets in blk); what the transmission came to after its last.
    pub fn block(&mut self, dt: u8, blk: &[u8; BLOCK_MAX]) -> Option<Taken> {
        let len = block_len(dt);
        if self.left == 0 || len == 0 {
            return None;
        }
        self.left -= 1;
        if self.m.dt == 0 {
            self.m.dt = dt; // the first block's rate is the message's
        }
        if dt == self.m.dt {
            if self.m.head.dpf == DPF_CONFIRMED {
                let t = DataMsg { b: [*blk; MSG_BLOCKS], ..self.m };
                let (dbsn, ok) = t.block_ok(0);
                if ok && dbsn < self.m.n {
                    self.m.b[dbsn as usize][..len].copy_from_slice(&blk[..len]);
                    self.good |= 1 << dbsn;
                }
            } else if self.next < self.m.n {
                self.m.b[self.next as usize][..len].copy_from_slice(&blk[..len]);
                self.good |= 1 << self.next;
                self.next += 1;
            }
        }
        if self.left > 0 {
            return None;
        }
        let all = (1u16 << self.m.n) - 1;
        if self.m.head.dpf == DPF_CONFIRMED && self.good != all {
            return Some(Taken::Sack(self.good));
        }
        self.on = false;
        let mut d = [0u8; crate::t3site::MSG_OCTETS];
        Some(if self.good == all && self.m.data(&mut d).is_some() { Taken::Ok } else { Taken::CrcFailed })
    }
}

/// The response a confirmed message from h gets from me (361-1 9.2.3 C_RHEAD, table 8.3; NI is N(S): stop and wait,
/// 361-3 table 5.7), and a SACK's C_RDATA block (n the message's blocks).
pub fn response(h: &DataHead, me: u32, t: Taken, n: u8) -> (DataHead, Option<[u8; 12]>) {
    let r = DataHead { dpf: DPF_RESPONSE, sap: h.sap, dst: h.src, src: me, status: h.ns, ..Default::default() };
    match t {
        Taken::Ok => (DataHead { class: CLASS_ACK, rtype: TYPE_ACK, ..r }, None),
        Taken::CrcFailed => (DataHead { class: CLASS_NACK, rtype: TYPE_NACK_CRC, ..r }, None),
        Taken::Sack(good) => (DataHead { class: CLASS_SACK, rtype: 0, blocks: 1, ..r }, Some(rdata(good, n))),
    }
}

/// A C_RDATA block (361-1 9.2.5, figure 8.17): a retry flag a block, block j in octet j / 8 bit j % 8, 1 taken and 0
/// to retry (the unused 1); the 32-bit CRC (B.3.9) over the 8 flag octets in octets 8 to 11.
pub fn rdata(good: u16, n: u8) -> [u8; 12] {
    let mut b = [0xFFu8; 12];
    for j in 0..usize::from(n.min(64)) {
        if j >= 16 || good >> j & 1 == 0 {
            b[j / 8] &= !(1 << (j % 8));
        }
    }
    let c = crc32(&b[..8]);
    b[8..].copy_from_slice(&c.to_le_bytes());
    b
}

/// A C_RDATA block's blocks to retry (of n), None when its CRC fails.
pub fn rdata_retry(b: &[u8], n: u8) -> Option<u16> {
    let mut c = [0u8; 4];
    c.copy_from_slice(&b[8..12]);
    if crc32(&b[..8]) != u32::from_le_bytes(c) {
        return None;
    }
    Some((0..n.min(16)).filter(|&j| b[usize::from(j / 8)] >> (j % 8) & 1 == 0).fold(0, |m, j| m | 1 << j))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::t3site::{crc9, DataMsg, DPF_UNCONFIRMED, DT_R1, DT_R12, DT_R34, MSG_OCTETS, SAP_IP};

    /// CRC-9 as 361-1 B.3.10 writes it: F9(x) = (x^9 M(x) mod G9(x)) + I9(x), M(x) the block data's bits then the
    /// serial number's, by polynomial long division over a bit vector; then the mask.
    fn crc9_ref(d: &[u8], dbsn: u8, mask: u16) -> u16 {
        let mut bits = std::vec::Vec::new();
        for o in d {
            for i in (0..8).rev() {
                bits.push(o >> i & 1);
            }
        }
        for i in (0..7).rev() {
            bits.push(dbsn >> i & 1);
        }
        bits.extend([0u8; 9]); // x^9 M(x)
        let g = [1u8, 0, 0, 1, 0, 1, 1, 0, 0, 1]; // x^9 + x^6 + x^4 + x^3 + 1, high term first
        for i in 0..bits.len() - 9 {
            if bits[i] == 1 {
                for (k, gk) in g.iter().enumerate() {
                    bits[i + k] ^= gk;
                }
            }
        }
        let rem = bits[bits.len() - 9..].iter().fold(0u16, |r, &b| r << 1 | u16::from(b));
        (rem ^ 0x1FF) ^ mask
    }

    /// The message CRC as 361-1 B.3.9 writes it: x^32 M(x) mod G32(x), M(x) the octets taken in pairs, the second of
    /// each first (an odd one last, after a zero octet), bit 7 first.
    fn crc32_ref(d: &[u8]) -> u32 {
        let mut bits = std::vec::Vec::new();
        let mut o = std::vec::Vec::from(d);
        if o.len() % 2 == 1 {
            o.push(0);
        }
        for p in o.chunks(2) {
            for x in [p[1], p[0]] {
                for i in (0..8).rev() {
                    bits.push(x >> i & 1);
                }
            }
        }
        bits.extend([0u8; 32]);
        let g: u64 = 0x1_04C1_1DB7;
        for i in 0..bits.len() - 32 {
            if bits[i] == 1 {
                for k in 0..33 {
                    bits[i + k] ^= (g >> (32 - k) & 1) as u8;
                }
            }
        }
        bits[bits.len() - 32..].iter().fold(0u32, |r, &b| r << 1 | u32::from(b))
    }

    #[test]
    fn crcs_against_the_spec_formulas() {
        let d: [u8; 22] = core::array::from_fn(|i| (i as u8).wrapping_mul(37) ^ 0x5A);
        // the block data sizes of B.3.10: 10, 16 and 22 octets, with the masks of table B.21
        for (len, mask) in [(10, 0x0F0), (16, 0x1FF), (22, 0x10F)] {
            for dbsn in [0u8, 1, 5, 0x7F] {
                assert_eq!(crc9(&d[..len], dbsn, mask), crc9_ref(&d[..len], dbsn, mask), "{len} {dbsn}");
            }
        }
        for n in [0usize, 1, 7, 8, 21, 22] {
            assert_eq!(crc32(&d[..n]), crc32_ref(&d[..n]), "{n}");
        }
        // worked values (from the formulas above): zeros, and a single 1 in the serial number's last bit
        assert_eq!(crc9(&[0u8; 10], 0, 0), 0x1FF);
        assert_eq!(crc9(&[0u8; 10], 1, 0), 0x059 ^ 0x1FF);
        assert_eq!(crc32(&[1, 0]), 0x04C1_1DB7); // the pair's second octet first: M(x) = 1
    }

    #[test]
    fn blocks_at_each_rate() {
        // 361-1 8.2.2: unconfirmed 12, 18, 24 user octets a block; confirmed 10, 16, 22
        let data: [u8; 100] = core::array::from_fn(|i| i as u8 ^ 0xA5);
        for (dt, len) in [(DT_R12, 12usize), (DT_R34, 18), (DT_R1, 24)] {
            for confirmed in [false, true] {
                let dpf = if confirmed { DPF_CONFIRMED } else { DPF_UNCONFIRMED };
                let h = DataHead { dpf, sap: SAP_IP, dst: 1, src: 2, response: confirmed, ..Default::default() };
                let m = DataMsg::build_rate(h, &data, dt).unwrap();
                let per = if confirmed { len - 2 } else { len };
                let n = (100 + 4usize).div_ceil(per);
                // 9.2.6 / 9.2.7: Blocks to Follow, the pad octet count, F
                assert_eq!((m.n as usize, m.head.blocks as usize, m.head.full), (n, n, true), "{dt} {confirmed}");
                assert_eq!(m.head.pad as usize, n * per - 104);
                assert_eq!(m.rate(), dt);
                let mut out = [0u8; MSG_OCTETS];
                assert_eq!(m.data(&mut out), Some(100));
                assert_eq!(&out[..100], &data);
                // the last block's last four octets: the CRC-32 over the rest, least significant first
                let last = &m.b[n - 1][..len];
                let mut u = std::vec::Vec::new();
                for j in 0..n {
                    u.extend_from_slice(&m.b[j][if confirmed { 2 } else { 0 }..len]);
                }
                let c = crc32(&u[..u.len() - 4]);
                assert_eq!(&last[len - 4..], &c.to_le_bytes());
                if confirmed {
                    for j in 0..n {
                        let (dbsn, ok) = m.block_ok(j);
                        assert!(ok && dbsn as usize == j);
                        let b = &m.b[j];
                        let stored = u16::from(b[0] & 1) << 8 | u16::from(b[1]);
                        let mask =
                            [0x0F0, 0x1FF, 0x10F][[DT_R12, DT_R34, DT_R1].iter().position(|&x| x == dt).unwrap()];
                        assert_eq!(stored, crc9_ref(&b[2..len], j as u8, mask));
                    }
                }
                let mut bad = m;
                bad.b[0][len - 1] ^= 0x10;
                assert_eq!(bad.data(&mut out), None);
            }
        }
        // the store: twelve blocks at each rate
        let h = DataHead { dpf: DPF_UNCONFIRMED, ..Default::default() };
        assert!(DataMsg::build_rate(h, &[0; 24 * 12 - 4], DT_R1).is_some());
        assert!(DataMsg::build_rate(h, &[0; 24 * 12 - 3], DT_R1).is_none());
        assert!(DataMsg::build_rate(h, &[0; 18 * 12 - 4], DT_R34).is_some());
        assert!(DataMsg::build_rate(h, &[0; 18 * 12 - 3], DT_R34).is_none());
        assert!(DataMsg::build_rate(h, b"x", 9).is_none());
    }

    #[test]
    fn selective_retry() {
        let data: [u8; 60] = core::array::from_fn(|i| i as u8);
        let h =
            DataHead { dpf: DPF_CONFIRMED, response: true, sap: SAP_IP, dst: 1, src: 2, ns: 3, ..Default::default() };
        let m = DataMsg::build_rate(h, &data, DT_R34).unwrap();
        assert_eq!(m.n, 4); // 64 octets in 16s
        let mut a = DataAsm::default();
        assert!(a.header(&m.head));
        let mut taken = None;
        for j in 0..4 {
            let mut b = m.b[j];
            if j == 2 {
                b[9] ^= 0x40; // lost on the air
            }
            taken = a.block(DT_R34, &b);
        }
        assert_eq!(taken, Some(Taken::Sack(0b1011)));
        let (r, rd) = response(&m.head, 1, taken.unwrap(), m.n);
        assert_eq!((r.dpf, r.class, r.rtype, r.status, r.blocks, r.dst), (DPF_RESPONSE, CLASS_SACK, 0, 3, 1, 2));
        let rd = rd.unwrap();
        assert_eq!(rd[0], 0xFB, "figure 8.17: block 2 to retry, the rest 1");
        assert_eq!(&rd[1..8], &[0xFF; 7]);
        assert_eq!(rdata_retry(&rd, m.n), Some(0b0100));
        let mut broken = rd;
        broken[1] = 0;
        assert_eq!(rdata_retry(&broken, m.n), None);
        // another sender's F = 0 header: not this message
        assert!(!a.header(&DataHead { full: false, blocks: 1, src: 9, ..m.head }));
        // the retry: C_HEAD with F = 0, one block
        assert!(a.header(&DataHead { full: false, blocks: 1, ..m.head }));
        assert_eq!(a.block(DT_R34, &m.b[2]), Some(Taken::Ok));
        let mut d = [0u8; MSG_OCTETS];
        assert_eq!(a.m.data(&mut d), Some(60));
        assert_eq!(&d[..60], &data);
        assert_eq!(response(&m.head, 1, Taken::Ok, 4).0.class, CLASS_ACK);
        // every block good, the message CRC not: NACK
        let mut bad = DataMsg::build_rate(h, &data, DT_R1).unwrap();
        let len = 24;
        bad.b[0][len - 1] ^= 1;
        let (dbsn, _) = bad.block_ok(0);
        let c = crc9(&bad.b[0][2..len], dbsn, 0x10F);
        bad.b[0][0] = dbsn << 1 | (c >> 8) as u8;
        bad.b[0][1] = c as u8;
        assert!(a.header(&bad.head));
        let mut t = None;
        for j in 0..bad.n as usize {
            t = a.block(DT_R1, &bad.b[j]);
        }
        assert_eq!(t, Some(Taken::CrcFailed));
        let (r, rd) = response(&bad.head, 1, Taken::CrcFailed, bad.n);
        assert_eq!((r.class, r.rtype, rd), (CLASS_NACK, TYPE_NACK_CRC, None));
        // unconfirmed at rate 1: no CRC-9, the CRC-32 decides
        let u = DataMsg::build_rate(DataHead { dpf: DPF_UNCONFIRMED, ..h }, &data, DT_R1).unwrap();
        assert!(a.header(&u.head));
        for j in 0..u.n as usize {
            t = a.block(DT_R1, &u.b[j]);
        }
        assert_eq!(t, Some(Taken::Ok));
        assert!(!a.on);
    }
}
