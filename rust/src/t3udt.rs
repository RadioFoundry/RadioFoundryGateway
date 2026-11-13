// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! The Unified Data Transport (ETSI TS 102 361-4 clause 6.5, 361-1 clauses 8.2.1.8 and 8.2.2.5): the UDT header
//! (C_UDTHD outbound, C_UDTHU inbound) and its one to four appended rate 1/2 blocks, and the identifiers, Service_Kinds
//! and reasons of the short data, status and polling services that use it (361-4 6.6.4 to 6.6.6). The header is the
//! 10 bytes before the CRC, as t3pdu's CSBKs (the radio layer adds the data header CRC mask); an appended block is all
//! 12 bytes of a rate 1/2 block, the last block's message CRC included (it has no mask, 361-1 B.3.12).

use crate::t3pdu::Rand;

/// Data types (361-1 table 9.2): a UDT header goes as a data header, its appended blocks as rate 1/2 data.
pub const DT_DATA_HDR: u8 = 6;
pub const DT_RATE12: u8 = 7;

// Opcodes (annex B, table B.1)
pub const OP_UDTHD: u8 = 26;
pub const OP_UDTHU: u8 = 27;
// the same header for Dynamic Group Numbering Assignment (6.6.8, tables 6.69 and 6.71)
pub const OP_DGNAHD: u8 = 36;
pub const OP_DGNAHU: u8 = 37;

// Gateways and identifiers (annex A.4, table A.8) that the data services use
pub const LINEI: u32 = 0xFFFEC2;
pub const IPI: u32 = 0xFFFEC3;
pub const SUPLI: u32 = 0xFFFEC4;
pub const SDMI: u32 = 0xFFFEC5;
pub const DIVERTI: u32 = 0xFFFEC9;
pub const DISPATI: u32 = 0xFFFECB;
pub const STUNI: u32 = 0xFFFECC;
pub const AUTHI: u32 = 0xFFFECD;
pub const KILLI: u32 = 0xFFFECF;
pub const DGNAI: u32 = 0xFFFED6;

// Service_Kind (7.2.12, table 7.49)
pub const SK_IND_PD: u8 = 0b0010;
pub const SK_TG_PD: u8 = 0b0011;
pub const SK_IND_UDT: u8 = 0b0100;
pub const SK_TG_UDT: u8 = 0b0101;
pub const SK_UDT_POLL: u8 = 0b0110;
pub const SK_STATUS: u8 = 0b0111;
pub const SK_DIVERT: u8 = 0b1000;
pub const SK_SUPPL: u8 = 0b1101;

// Reasons (7.2.8, tables 7.42 and 7.43) beside t3pdu's
pub const R_STORE_FORWARD: u8 = 0x61;
pub const R_POLL_TS: u8 = 0x63; // Accepted for the Status Polling Service, by the TS
pub const R_POLL_MS: u8 = 0x47; // the same by the MS (mirrored by the TS)
pub const R_TEMP_USER_REFUSED: u8 = 0x22;
pub const R_DIV_CAUSE_FAIL: u8 = 0x26;
pub const R_UDT_CRC: u8 = 0x30; // CRC error in the UDT upload phase
pub const R_MS_NOT_SUPPORTED: u8 = 0x00;
pub const R_STACK_FULL: u8 = 0x12;

// UDT_Format (361-1 9.3.39, table 9.51)
pub const FMT_BINARY: u8 = 0;
pub const FMT_ADDRESS: u8 = 1;
pub const FMT_BCD: u8 = 2;
pub const FMT_ISO7: u8 = 3;
pub const FMT_ISO8: u8 = 4;
pub const FMT_NMEA: u8 = 5;
pub const FMT_IP: u8 = 6;
pub const FMT_UTF16: u8 = 7;
pub const FMT_MIXED: u8 = 10;

// Status values (6.6.6.0)
pub const STATUS_EMERGENCY_CANCEL: u8 = 124;
pub const STATUS_EMERGENCY: u8 = 126;
pub const STATUS_POLL: u8 = 127;

/// Appended blocks: 1 to 4 (the UAB element is this minus one).
pub const MAX_BLOCKS: usize = 4;
/// User octets of n appended blocks: 12 each, less the last block's CRC.
pub const fn user_len(n: usize) -> usize {
    12 * n - 2
}
/// The most user octets a UDT carries (368 bits, 6.5.0).
pub const MAX_DATA: usize = user_len(MAX_BLOCKS);

/// The blocks n user octets need (1 to 4); 0 when they don't fit.
pub const fn blocks_for(len: usize) -> usize {
    if len > MAX_DATA {
        0
    } else if len <= 10 {
        1
    } else {
        (len + 2).div_ceil(12)
    }
}

/// The UDT header (361-4 7.1.1.1.8 table 7.24 and 7.1.1.2.4 table 7.28; 361-1 8.2.1.8 figure 8.10).
/// Octet 0: G/I, A, then outbound Emergency and UDT_Option_Flag or inbound reserved and UDT_DIV, then the Data Packet
/// Format 0000; octet 1: SAP 0000 and UDT_Format; octets 2-4 the target, 5-7 the source; octet 8: Pad Nibble (5),
/// reserved 0, UAB (2); octet 9: SF, PF, opcode.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct UdtHead {
    pub op: u8,
    pub group: bool,
    pub response: bool,
    /// outbound: Emergency; inbound: reserved
    pub emergency: bool,
    /// outbound: UDT_Option_Flag (7.2.12.2); inbound: UDT_DIV
    pub option: bool,
    pub format: u8,
    pub target: u32,
    pub source: u32,
    pub pad_nibble: u8,
    /// appended blocks, 1 to 4
    pub blocks: u8,
    pub sf: bool,
}

impl UdtHead {
    pub fn encode(&self) -> [u8; 10] {
        let mut b = [0u8; 10];
        b[0] = (self.group as u8) << 7
            | (self.response as u8) << 6
            | (self.emergency as u8) << 5
            | (self.option as u8) << 4;
        b[1] = self.format & 0x0F;
        b[2..5].copy_from_slice(&self.target.to_be_bytes()[1..]);
        b[5..8].copy_from_slice(&self.source.to_be_bytes()[1..]);
        b[8] = (self.pad_nibble & 0x1F) << 3 | (self.blocks.clamp(1, 4) - 1);
        b[9] = (self.sf as u8) << 7 | (self.op & 0x3F);
        b
    }
    /// A UDT header (DPF and SAP 0000, opcode UDTHD, UDTHU, DGNAHD or DGNAHU); None otherwise.
    pub fn decode(b: &[u8; 10]) -> Option<UdtHead> {
        let op = b[9] & 0x3F;
        if b[0] & 0x0F != 0 || b[1] >> 4 != 0 || !matches!(op, OP_UDTHD | OP_UDTHU | OP_DGNAHD | OP_DGNAHU) {
            return None;
        }
        let a24 = |o: usize| u32::from_be_bytes([0, b[o], b[o + 1], b[o + 2]]);
        Some(UdtHead {
            op,
            group: b[0] & 0x80 != 0,
            response: b[0] & 0x40 != 0,
            emergency: b[0] & 0x20 != 0,
            option: b[0] & 0x10 != 0,
            format: b[1] & 0x0F,
            target: a24(2),
            source: a24(5),
            pad_nibble: b[8] >> 3,
            blocks: (b[8] & 3) + 1,
            sf: b[9] & 0x80 != 0,
        })
    }
}

/// CRC-CCITT as 361-1 B.3.8: x^16 + x^12 + x^5 + 1, from zero, inverted.
pub fn crc16(data: &[u8]) -> u16 {
    let mut crc: u16 = 0;
    for &d in data {
        for i in (0..8).rev() {
            let fb = ((crc >> 15) ^ (d as u16 >> i)) & 1;
            crc <<= 1;
            if fb != 0 {
                crc ^= 0x1021;
            }
        }
    }
    !crc
}

/// The appended blocks of a UDT: n blocks of 12 octets, the user data then zero fill, the last block's final two
/// octets the message CRC over everything before them (361-1 8.2.2.5, 9.2.14).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct Blocks {
    pub n: u8,
    pub b: [[u8; 12]; MAX_BLOCKS],
}

impl Blocks {
    /// Blocks for data (at most 46 octets) in n blocks (0: the fewest that hold it); None when it doesn't fit.
    pub fn of(data: &[u8], n: usize) -> Option<Blocks> {
        let n = if n == 0 { blocks_for(data.len()) } else { n };
        if n == 0 || n > MAX_BLOCKS || data.len() > user_len(n) {
            return None;
        }
        let mut flat = [0u8; 12 * MAX_BLOCKS];
        flat[..data.len()].copy_from_slice(data);
        let crc = crc16(&flat[..user_len(n)]);
        flat[user_len(n)..12 * n].copy_from_slice(&crc.to_be_bytes());
        let mut out = Blocks { n: n as u8, ..Default::default() };
        for i in 0..n {
            out.b[i].copy_from_slice(&flat[12 * i..12 * i + 12]);
        }
        Some(out)
    }
    /// The user octets (12 n - 2), CRC left off.
    pub fn data(&self, out: &mut [u8; MAX_DATA]) -> usize {
        let n = self.n as usize;
        for i in 0..n {
            let take = if i + 1 == n { 10 } else { 12 };
            out[12 * i..12 * i + take].copy_from_slice(&self.b[i][..take]);
        }
        user_len(n.max(1))
    }
    /// The last block's CRC is right.
    pub fn crc_ok(&self) -> bool {
        let n = self.n as usize;
        if n == 0 || n > MAX_BLOCKS {
            return false;
        }
        let mut flat = [0u8; 12 * MAX_BLOCKS];
        for i in 0..n {
            flat[12 * i..12 * i + 12].copy_from_slice(&self.b[i]);
        }
        let got = u16::from_be_bytes([flat[12 * n - 2], flat[12 * n - 1]]);
        crc16(&flat[..user_len(n)]) == got
    }
}

/// The status value of a status C_RAND (6.6.6.1.2, tables 6.57 and 6.58): the Service_Options' five LSBs are its
/// five MSBs, the dependent bits' two LSBs its two LSBs.
pub fn rand_status(r: &Rand) -> u8 {
    (r.service_options & 0x1F) << 2 | (r.dep & 3)
}

/// The G/I bit of a status C_RAND (Service_Options MSB, table 6.57).
pub fn rand_status_group(r: &Rand) -> bool {
    r.service_options & 0x40 != 0
}

/// Appended_UDT_Short_Data of a short data C_RAND (table 6.55): the UAB of the upload, blocks minus one.
pub fn rand_sdata_blocks(r: &Rand) -> u8 {
    (r.dep & 3) + 1
}

/// Appended_Supplementary_Data (table 6.55), only with SUPED_SV.
pub fn rand_suped(r: &Rand) -> bool {
    r.service_options & 0x20 != 0
}

/// The most addresses an Address Format UDT carries (B.3.2: 3, 7, 11 or 15 in one to four blocks).
pub const MAX_ADDRS: usize = 15;

/// Appended data in the Address Format (B.3.2, UDT_Format 0001b) or the Mixed Format (B.3.9, 1010b): octet 0
/// reserved and the One-key flag (OK, its LSB), then 24-bit addresses from octet 1 (the user octets run on across
/// the blocks; the Mixed Format has one address, then characters).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct AddrList {
    pub ok: bool,
    pub n: usize,
    pub a: [u32; MAX_ADDRS],
}

impl AddrList {
    /// The addresses of user data (Blocks::data's octets; len 12 n - 2): 3 per block less one, the first block 3.
    pub fn of(data: &[u8]) -> AddrList {
        let mut l = AddrList { ok: data.first().is_some_and(|b| b & 1 != 0), ..Default::default() };
        let mut o = 1;
        while o + 3 <= data.len() && l.n < MAX_ADDRS {
            l.a[l.n] = u32::from_be_bytes([0, data[o], data[o + 1], data[o + 2]]);
            l.n += 1;
            o += 3;
        }
        l
    }
    /// The addresses; the unused ones ADRNULL.
    pub fn addrs(&self) -> &[u32] {
        &self.a[..self.n.min(MAX_ADDRS)]
    }
    /// The blocks for addrs (at most 15; the rest of the last block ADRNULL).
    pub fn blocks(ok: bool, addrs: &[u32]) -> Option<Blocks> {
        if addrs.is_empty() || addrs.len() > MAX_ADDRS {
            return None;
        }
        let n = (addrs.len() + 1).div_ceil(4); // n blocks hold 4 n - 1
        let mut d = [0u8; MAX_DATA];
        d[0] = ok as u8;
        for i in 0..4 * n - 1 {
            let a = addrs.get(i).copied().unwrap_or(crate::t3pdu::ADRNULL);
            d[1 + 3 * i..4 + 3 * i].copy_from_slice(&a.to_be_bytes()[1..]);
        }
        Blocks::of(&d[..user_len(n)], n)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn address_format() {
        // B.3.2 figures B.6 to B.9: OK, then the addresses run on from octet 1, 3 / 7 / 11 / 15 in 1 to 4 blocks
        for (k, n) in [(1usize, 1u8), (3, 1), (4, 2), (7, 2), (8, 3), (11, 3), (12, 4), (15, 4)] {
            let a: [u32; 15] = core::array::from_fn(|i| 0x10_0000 + i as u32);
            let b = AddrList::blocks(k == 1, &a[..k]).unwrap();
            assert_eq!(b.n, n, "{k}");
            assert!(b.crc_ok());
            let mut d = [0u8; MAX_DATA];
            let len = b.data(&mut d);
            let l = AddrList::of(&d[..len]);
            assert_eq!(l.n, 4 * n as usize - 1);
            assert_eq!(l.ok, k == 1);
            assert_eq!(&l.addrs()[..k], &a[..k]);
            assert!(l.addrs()[k..].iter().all(|&x| x == 0), "ADRNULL fill");
        }
        // ADDRESS4 straddles blocks 1 and 2: octets 10-11 of the first, octet 0 of the second
        let b = AddrList::blocks(false, &[1, 2, 3, 0xABCDEF]).unwrap();
        assert_eq!((b.b[0][10], b.b[0][11], b.b[1][0]), (0xAB, 0xCD, 0xEF));
        assert!(AddrList::blocks(false, &[0; 16]).is_none());
        // the DGNA opcodes are UDT headers too
        let h = UdtHead { op: OP_DGNAHU, response: true, format: FMT_ADDRESS, blocks: 2, ..Default::default() };
        assert_eq!(UdtHead::decode(&h.encode()), Some(h));
    }

    #[test]
    fn header_round_trip() {
        // 361-4 7.1.1.2.4 table 7.28: an inbound header, individual, response demanded, ISO 8 text, 2 blocks
        let h = UdtHead {
            op: OP_UDTHU,
            response: true,
            format: FMT_ISO8,
            target: 0x00_30_39,
            source: 0x12_34_56,
            pad_nibble: 3,
            blocks: 2,
            ..Default::default()
        };
        let b = h.encode();
        assert_eq!(b, [0x40, 0x04, 0x00, 0x30, 0x39, 0x12, 0x34, 0x56, 0x19, 0x1B]);
        assert_eq!(UdtHead::decode(&b), Some(h));
        // outbound, talkgroup, emergency, SF set
        let d = UdtHead { op: OP_UDTHD, group: true, emergency: true, sf: true, blocks: 4, ..h };
        let b = d.encode();
        assert_eq!(b[0], 0xE0);
        assert_eq!(b[8] & 3, 3);
        assert_eq!(b[9], 0x80 | OP_UDTHD);
        assert_eq!(UdtHead::decode(&b), Some(d));
        // a data header of another DPF, or a CSBK opcode, isn't one
        let mut x = b;
        x[0] |= 0x02;
        assert_eq!(UdtHead::decode(&x), None);
        let mut x = b;
        x[9] = 31;
        assert_eq!(UdtHead::decode(&x), None);
    }

    #[test]
    fn crc_is_the_header_crc() {
        // B.3.8 check value: CRC-CCITT of "123456789" from zero, inverted, is !0x31C3
        assert_eq!(crc16(b"123456789"), !0x31C3);
    }

    #[test]
    fn blocks() {
        assert_eq!(blocks_for(0), 1);
        assert_eq!(blocks_for(10), 1);
        assert_eq!(blocks_for(11), 2);
        assert_eq!(blocks_for(22), 2);
        assert_eq!(blocks_for(46), 4);
        assert_eq!(blocks_for(47), 0);
        let text = b"Hello from the trunk site, 34 byte";
        let bl = Blocks::of(text, 0).unwrap();
        assert_eq!(bl.n, 3);
        assert!(bl.crc_ok());
        let mut out = [0u8; MAX_DATA];
        let n = bl.data(&mut out);
        assert_eq!(n, 34);
        assert_eq!(&out[..text.len()], text);
        let mut bad = bl;
        bad.b[1][3] ^= 0x10;
        assert!(!bad.crc_ok());
        assert!(Blocks::of(&[0; 47], 0).is_none());
        assert!(Blocks::of(&[0; 11], 1).is_none());
        assert_eq!(Blocks::of(&[1, 2], 2).unwrap().n, 2);
    }
}
