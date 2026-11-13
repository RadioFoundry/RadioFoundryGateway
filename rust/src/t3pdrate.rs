// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Packet data at any rate, a party's side (docs/spec/fw/tier3.md 3.16): a message's blocks taken in (ETSI TS 102
//! 361-1 8.2.2: rate 1/2, 3/4 or 1, one rate a message; a confirmed block placed by its serial number and kept only
//! when its CRC-9 holds), the answer to a confirmed message (361-1 8.2.2.3, table 8.3; 361-3 5.4.2.2: ACK, NACK when
//! every block is good but the CRC-32 fails, SACK with a C_RDATA block listing the blocks to retry) and the sender's
//! side of a SACK (361-3 5.4.1: the listed blocks again, after a C_HEAD with F = 0). The MS simulator uses it.

use crate::t3site::{
    block_len, blocks_mask, crc32, DataHead, DataMsg, Mask, BLOCK_MAX, DPF_CONFIRMED, DPF_RESPONSE, MSG_BLOCKS,
};

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
    Sack(u128),
    /// Every block taken, the CRC-32 bad.
    CrcFailed,
}

/// A message being taken in.
#[derive(Clone, Copy, Debug, Default)]
pub struct DataAsm {
    pub m: DataMsg,
    pub good: Mask, // the blocks taken (confirmed: those whose CRC-9 held)
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
        self.good = Mask::default();
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
                    self.good = Mask::of(self.good.get() | 1 << dbsn);
                }
            } else if self.next < self.m.n {
                self.m.b[self.next as usize][..len].copy_from_slice(&blk[..len]);
                self.good = Mask::of(self.good.get() | 1 << self.next);
                self.next += 1;
            }
        }
        if self.left > 0 {
            return None;
        }
        let (all, good) = (blocks_mask(self.m.n), self.good.get());
        if self.m.head.dpf == DPF_CONFIRMED && good != all {
            return Some(Taken::Sack(good));
        }
        self.on = false;
        let mut d = [0u8; crate::t3site::MSG_OCTETS];
        Some(if good == all && self.m.data(&mut d).is_some() { Taken::Ok } else { Taken::CrcFailed })
    }
}

/// The response a confirmed message from h gets from me (361-1 9.2.3 C_RHEAD, table 8.3; NI is N(S): stop and wait,
/// 361-3 table 5.7), and a SACK's C_RDATA blocks (n the message's blocks: one block for up to 64, else two).
pub fn response(h: &DataHead, me: u32, t: Taken, n: u8) -> (DataHead, Option<RdataOut>) {
    let r = DataHead { dpf: DPF_RESPONSE, sap: h.sap, dst: h.src, src: me, status: h.ns, ..Default::default() };
    match t {
        Taken::Ok => (DataHead { class: CLASS_ACK, rtype: TYPE_ACK, ..r }, None),
        Taken::CrcFailed => (DataHead { class: CLASS_NACK, rtype: TYPE_NACK_CRC, ..r }, None),
        Taken::Sack(good) => {
            let o = RdataOut::of(good, n);
            (DataHead { class: CLASS_SACK, rtype: 0, blocks: o.n, ..r }, Some(o))
        }
    }
}

/// C_RDATA blocks for good of n (361-1 8.2.2.3, 9.2.5, figure 8.17): a retry flag a block, block j in C_RDATA j / 64,
/// octet j % 64 / 8, bit j % 8, 1 taken and 0 to retry (the unused 1); each C_RDATA's own 32-bit CRC (B.3.9, table
/// 9.14) over its 8 flag octets, in octets 8 to 11. 361-1 says two blocks carry up to 127 flags but not how they
/// split: the second holds blocks 64 on.
pub fn rdata_blocks(good: u128, n: u8) -> ([[u8; 12]; 2], u8) {
    let mut b = [[0xFFu8; 12]; 2];
    let n = n.min(MSG_BLOCKS as u8);
    for j in 0..usize::from(n) {
        if good >> j & 1 == 0 {
            b[j / 64][j % 64 / 8] &= !(1 << (j % 8));
        }
    }
    for k in &mut b {
        let c = crc32(&k[..8]);
        k[8..].copy_from_slice(&c.to_le_bytes());
    }
    (b, if n > 64 { 2 } else { 1 })
}

/// The first C_RDATA block alone (a message of up to 64 blocks).
pub fn rdata(good: u128, n: u8) -> [u8; 12] {
    rdata_blocks(good, n).0[0]
}

/// A SACK's C_RDATA blocks still to send.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RdataOut {
    pub b: [[u8; 12]; 2],
    pub n: u8,
    next: u8,
}

impl RdataOut {
    pub fn of(good: u128, n: u8) -> RdataOut {
        let (b, n) = rdata_blocks(good, n);
        RdataOut { b, n, next: 0 }
    }

    /// The next block to send.
    pub fn take(&mut self) -> Option<[u8; 12]> {
        if self.next >= self.n {
            return None;
        }
        self.next += 1;
        Some(self.b[usize::from(self.next - 1)])
    }

    pub fn left(&self) -> bool {
        self.next < self.n
    }
}

/// C_RDATA block k (0 or 1) of a SACK: its blocks to retry (of n), None when its CRC fails. The second block's CRC
/// is also taken over both blocks' flags (first: the first block's), as 361-1 leaves it open.
pub fn rdata_retry_at(b: &[u8], k: u8, n: u8, first: &[u8; 8]) -> Option<u128> {
    let mut c = [0u8; 4];
    c.copy_from_slice(&b[8..12]);
    let c = u32::from_le_bytes(c);
    let mut both = [0u8; 16];
    both[..8].copy_from_slice(first);
    both[8..].copy_from_slice(&b[..8]);
    if crc32(&b[..8]) != c && (k == 0 || crc32(&both) != c) {
        return None;
    }
    let base = u32::from(k) * 64;
    let to = u32::from(n).min(base + 64);
    Some((base..to).filter(|&j| b[((j - base) / 8) as usize] >> (j % 8) & 1 == 0).fold(0, |m, j| m | 1u128 << j))
}

/// A one-block SACK's C_RDATA: the blocks to retry (of n, up to 64), None when its CRC fails.
pub fn rdata_retry(b: &[u8], n: u8) -> Option<u128> {
    rdata_retry_at(b, 0, n, &[0; 8])
}

/// A SACK's C_RDATA blocks being taken in (its header's Blocks to Follow of them).
#[derive(Clone, Copy, Debug, Default)]
pub struct RdataIn {
    left: u8,
    k: u8,
    first: [u8; 8],
    retry: Mask,
    bad: bool,
}

impl RdataIn {
    /// A SACK header with blocks C_RDATA blocks to follow (1 or 2).
    pub fn start(&mut self, blocks: u8) {
        *self = RdataIn { left: blocks.clamp(1, 2), ..Default::default() };
    }

    pub fn on(&self) -> bool {
        self.left > 0
    }

    pub fn stop(&mut self) {
        self.left = 0;
    }

    /// A C_RDATA block. Some after the last: the blocks to retry (all n when none is listed); 0 when a block's CRC
    /// failed (the response is ignored: the sender times out and sends again).
    pub fn block(&mut self, b: &[u8], n: u8) -> Option<u128> {
        if self.left == 0 || b.len() < 12 {
            return None;
        }
        self.left -= 1;
        match rdata_retry_at(b, self.k, n, &self.first) {
            Some(r) => self.retry = Mask::of(self.retry.get() | r),
            None => self.bad = true,
        }
        if self.k == 0 {
            self.first.copy_from_slice(&b[..8]);
        }
        self.k += 1;
        if self.left > 0 {
            return None;
        }
        Some(if self.bad {
            0
        } else if self.retry.get() == 0 {
            blocks_mask(n)
        } else {
            self.retry.get()
        })
    }
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
        // the store: 127 blocks at each rate (Blocks to Follow's 7 bits)
        let h = DataHead { dpf: DPF_UNCONFIRMED, ..Default::default() };
        assert!(DataMsg::build_rate(h, &[0; 24 * 127 - 4], DT_R1).is_some());
        assert!(DataMsg::build_rate(h, &[0; 24 * 127 - 3], DT_R1).is_none());
        assert!(DataMsg::build_rate(h, &[0; 18 * 127 - 4], DT_R34).is_some());
        assert!(DataMsg::build_rate(h, &[0; 18 * 127 - 3], DT_R34).is_none());
        let c = DataHead { dpf: DPF_CONFIRMED, ..Default::default() };
        assert!(DataMsg::build(c, &[0; 10 * 127 - 4]).is_some());
        assert!(DataMsg::build(c, &[0; 10 * 127 - 3]).is_none());
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
        let rd = rd.unwrap().b[0];
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

    #[test]
    fn two_block_sack() {
        // 361-1 8.2.2.3: flags for up to 127 blocks in two C_RDATA blocks; blocks 64 on in the second
        let lost: u128 = 1 << 5 | 1 << 70 | 1 << 126;
        let o = RdataOut::of(crate::t3site::blocks_mask(127) & !lost, 127);
        assert_eq!(o.n, 2);
        assert_eq!(o.b[0][0], 0xDF);
        assert_eq!(&o.b[0][1..8], &[0xFF; 7]);
        assert_eq!(o.b[1][0], 0xBF, "block 70: the second block's bit 6");
        assert_eq!(o.b[1][7], 0xBF, "block 126: octet 7 bit 6; the unused 127th flag 1");
        let mut r = RdataIn::default();
        r.start(2);
        assert_eq!(r.block(&o.b[0], 127), None);
        assert_eq!(r.block(&o.b[1], 127), Some(lost));
        // the second block's CRC over both blocks' flags: taken too
        let mut b2 = o.b[1];
        let mut both = [0u8; 16];
        both[..8].copy_from_slice(&o.b[0][..8]);
        both[8..].copy_from_slice(&b2[..8]);
        b2[8..].copy_from_slice(&crc32(&both).to_le_bytes());
        r.start(2);
        r.block(&o.b[0], 127);
        assert_eq!(r.block(&b2, 127), Some(lost));
        // a block's CRC bad: 0, the response ignored
        b2[0] ^= 1;
        r.start(2);
        r.block(&o.b[0], 127);
        assert_eq!(r.block(&b2, 127), Some(0));
        // up to 64 blocks: one C_RDATA, as before
        let mut one = RdataOut::of(!0b100, 64);
        assert_eq!(one.n, 1);
        assert!(one.take().is_some() && one.take().is_none());
        let (h, _) = response(&DataHead::default(), 1, Taken::Sack(!lost), 127);
        assert_eq!((h.class, h.blocks), (CLASS_SACK, 2));
    }
}
