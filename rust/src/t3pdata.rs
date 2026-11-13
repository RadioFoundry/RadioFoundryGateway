// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Packet data calls (docs/spec/fw/tier3.md 3.16): ETSI TS 102 361-4 6.6.3. An MS asks on the TSCC (C_RAND
//! Service_Kind 0010b individual, 0011b talkgroup); the site checks the called MS is there (C_AHOY, 6.6.3.1.4),
//! grants a payload channel (PD_GRANT, TD_GRANT: 7.1.1.1.1.4, 7.1.1.1.1.5), and the parties exchange packet data on
//! it (ETSI TS 102 361-1 9.2, 361-3: a data header, then blocks; confirmed or unconfirmed), which the site repeats.
//! The channel clears at the end of a single item (unconfirmed), after the data hang time, or on P_MAINT.
//!
//! The site is a party too: an MS's data to TSI, or to the site's data ID, goes to the site's IP gateway, and the
//! site can call an MS or a talkgroup with data of its own (rdof_t3_pd_send). A part of the site controller (a child
//! module of t3site, for its state); its own state is PdState. The data header codec, the message CRCs and the
//! block counter (DataRx) are here too: the radio layer, the air and the MS simulator use them.

use super::*;
use crate::t3udt::{IPI, SK_IND_PD, SK_TG_PD};

pub const PDS: usize = 8;

/// Data channel grants (annex B, table B.1): single item and multi-item.
pub const OP_PD_GRANT: u8 = 51;
pub const OP_TD_GRANT: u8 = 52;
pub const OP_PD_GRANT_MI: u8 = 55;
pub const OP_TD_GRANT_MI: u8 = 56;

/// C_RAND Service_Options for packet data (6.6.3.2.1, table 6.50); emergency and priority are as for voice.
pub const SO_SUPED: u8 = 0x10;
pub const SO_HI_RATE: u8 = 0x08;
pub const SO_SIMI: u8 = 0x04;

/// What the radio layer heard on a data channel (rdof_t3_rx_data).
pub const DATA_HEADER: u8 = 1;
pub const DATA_BLOCK: u8 = 2;
pub const DATA_END: u8 = 3; // the last block of an unconfirmed message
pub const DATA_END_CONFIRMED: u8 = 4; // the last block of a confirmed message
pub const DATA_RESPONSE: u8 = 5; // a response header (361-1 9.2.3)

/// EV_PD's op.
pub const PD_OPEN: u8 = 1; // a data channel: ch, src, dst; kind 1 when the site is a party
pub const PD_FAILED: u8 = 2; // the site's own call failed: reason, src, dst

/// Data packet formats (361-1, the Data Packet Format element): a data header's octet 0, low four bits.
pub const DPF_RESPONSE: u8 = 1;
pub const DPF_UNCONFIRMED: u8 = 2;
pub const DPF_CONFIRMED: u8 = 3;
pub const SAP_IP: u8 = 4;

/// The last block of an unconfirmed single item is repeated within this long; then the channel clears.
const DRAIN_MS: i64 = 500;

// ── the data header (361-1 9.2.1 unconfirmed, 9.2.2 confirmed, 9.2.3 response): 10 octets, the CRC the radio
// layer's (mask CCCCh) ──

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DataHead {
    pub dpf: u8,
    pub group: bool,
    pub response: bool, // A: a response requested
    pub sap: u8,
    pub pad: u8, // pad octet count (5 bits)
    pub dst: u32,
    pub src: u32,
    pub full: bool, // F: the full message
    pub blocks: u8, // blocks to follow
    pub ns: u8,     // N(S), confirmed
    pub fsn: u8,
    pub class: u8, // a response's class, type and status
    pub rtype: u8,
    pub status: u8,
}

impl DataHead {
    pub fn encode(&self) -> [u8; 10] {
        let mut d = [0u8; 10];
        d[0] = (self.group as u8) << 7 | (self.response as u8) << 6 | (self.dpf & 0x0F);
        if self.dpf == DPF_RESPONSE {
            d[1] = (self.sap & 0x0F) << 4;
            d[8] = self.blocks & 0x7F;
            d[9] = (self.class & 3) << 6 | (self.rtype & 7) << 3 | (self.status & 7);
        } else {
            d[0] |= self.pad & 0x10;
            d[1] = (self.sap & 0x0F) << 4 | (self.pad & 0x0F);
            d[8] = (self.full as u8) << 7 | (self.blocks & 0x7F);
            let ns = if self.dpf == DPF_CONFIRMED { (self.ns & 7) << 4 } else { 0 };
            d[9] = ns | (self.fsn & 0x0F);
        }
        d[2..5].copy_from_slice(&self.dst.to_be_bytes()[1..]);
        d[5..8].copy_from_slice(&self.src.to_be_bytes()[1..]);
        d
    }

    /// Response, unconfirmed and confirmed headers; None for the other formats.
    pub fn decode(d: &[u8; 10]) -> Option<DataHead> {
        let dpf = d[0] & 0x0F;
        if !(DPF_RESPONSE..=DPF_CONFIRMED).contains(&dpf) {
            return None;
        }
        let be24 = |p: &[u8]| u32::from(p[0]) << 16 | u32::from(p[1]) << 8 | u32::from(p[2]);
        let mut h = DataHead {
            dpf,
            group: d[0] & 0x80 != 0,
            response: d[0] & 0x40 != 0,
            sap: d[1] >> 4,
            dst: be24(&d[2..5]),
            src: be24(&d[5..8]),
            blocks: d[8] & 0x7F,
            ..Default::default()
        };
        if dpf == DPF_RESPONSE {
            h.class = d[9] >> 6;
            h.rtype = (d[9] >> 3) & 7;
            h.status = d[9] & 7;
        } else {
            h.pad = (d[0] & 0x10) | (d[1] & 0x0F);
            h.full = d[8] & 0x80 != 0;
            h.ns = if dpf == DPF_CONFIRMED { (d[9] >> 4) & 7 } else { 0 };
            h.fsn = d[9] & 0x0F;
        }
        Some(h)
    }
}

/// The message CRC (361-1 B.3.9): CRC-32, generator 04C11DB7h, zero start, no inversion, over the octets taken
/// in pairs with the second first; it goes in the last block's last four octets, least significant first.
pub fn crc32(d: &[u8]) -> u32 {
    let mut crc = 0u32;
    for i in 0..(d.len() + 1) & !1 {
        let o = d.get(i ^ 1).copied().unwrap_or(0);
        crc ^= u32::from(o) << 24;
        for _ in 0..8 {
            crc = if crc & 0x8000_0000 != 0 { crc << 1 ^ 0x04C1_1DB7 } else { crc << 1 };
        }
    }
    crc
}

/// A confirmed block's CRC (361-1 B.3.10): x^9 + x^6 + x^4 + x^3 + 1 over the block's data then its 7-bit serial
/// number, inverted, then the rate's mask (table B.21: rate 1/2 0F0h, rate 3/4 1FFh, rate 1 10Fh).
pub fn crc9(d: &[u8], dbsn: u8, mask: u16) -> u16 {
    let mut crc = 0u16;
    let n = d.len() * 8;
    for i in 0..n + 7 {
        let bit = if i < n { (d[i >> 3] >> (7 - (i & 7))) & 1 } else { (dbsn >> (6 - (i - n))) & 1 };
        let fb = ((crc >> 8) as u8 & 1) ^ bit;
        crc = (crc << 1) & 0x1FF;
        if fb != 0 {
            crc ^= 0x059;
        }
    }
    (crc ^ 0x1FF) ^ mask
}

pub const CRC9_RATE12: u16 = 0x0F0;
pub const CRC9_RATE34: u16 = 0x1FF;
pub const CRC9_RATE1: u16 = 0x10F;
pub const MSG_BLOCKS: usize = 12;
/// A block's octets at most (rate 1).
pub const BLOCK_MAX: usize = 24;
/// A message's octets at most (MSG_BLOCKS rate 1 blocks).
pub const MSG_OCTETS: usize = BLOCK_MAX * MSG_BLOCKS;

/// Data blocks' data types (361-1 table 9.2 / 9.3.6): rate 1/2, rate 3/4, rate 1.
pub const DT_R12: u8 = 7;
pub const DT_R34: u8 = 8;
pub const DT_R1: u8 = 10;

/// A data block's octets for its data type (361-1 8.2.2: rate 1/2 12, rate 3/4 18, rate 1 24); 0 not a data block.
pub fn block_len(dt: u8) -> usize {
    match dt {
        DT_R12 => 12,
        DT_R34 => 18,
        DT_R1 => 24,
        _ => 0,
    }
}

/// A confirmed block's CRC-9 mask for its data type (361-1 B.3.12, table B.21).
pub fn crc9_mask(dt: u8) -> u16 {
    match dt {
        DT_R34 => CRC9_RATE34,
        DT_R1 => CRC9_RATE1,
        _ => CRC9_RATE12,
    }
}

/// A packet data message in blocks of one rate (361-1 8.2.2.1, 8.2.2.2, 9.2.6, 9.2.7): unconfirmed the whole block is
/// user data (rate 1/2 12 octets, rate 3/4 18, rate 1 24), confirmed the block's octets 2 on (10, 16, 22) after the
/// 7-bit serial number and the CRC-9; the CRC-32 ends the last block.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DataMsg {
    pub head: DataHead,
    pub n: u8,
    pub dt: u8, // the blocks' data type, DT_R12 (0 reads as it), DT_R34 or DT_R1
    pub b: [[u8; BLOCK_MAX]; MSG_BLOCKS],
}

impl DataMsg {
    /// In rate 1/2 blocks.
    pub fn build(head: DataHead, data: &[u8]) -> Option<DataMsg> {
        Self::build_rate(head, data, DT_R12)
    }

    /// In blocks of data type dt (DT_R12, DT_R34, DT_R1): the header's Blocks to Follow, pad octet count and F set.
    pub fn build_rate(head: DataHead, data: &[u8], dt: u8) -> Option<DataMsg> {
        let confirmed = head.dpf == DPF_CONFIRMED;
        let len = block_len(dt);
        if len == 0 {
            return None;
        }
        let per = if confirmed { len - 2 } else { len };
        let total = data.len() + 4;
        let n = total.div_ceil(per);
        if n > MSG_BLOCKS || n == 0 {
            return None;
        }
        let pad = n * per - total; // fewer than a block's: 5 bits are enough
        let mut u = [0u8; MSG_OCTETS];
        u[..data.len()].copy_from_slice(data);
        let crc = crc32(&u[..data.len() + pad]);
        u[data.len() + pad..total + pad].copy_from_slice(&crc.to_le_bytes());
        let mut m = DataMsg {
            head: DataHead { blocks: n as u8, pad: pad as u8, full: true, ..head },
            n: n as u8,
            dt,
            ..Default::default()
        };
        for j in 0..n {
            let src = &u[j * per..(j + 1) * per];
            if confirmed {
                let dbsn = j as u8 & 0x7F;
                let c = crc9(src, dbsn, crc9_mask(dt));
                m.b[j][0] = dbsn << 1 | (c >> 8) as u8;
                m.b[j][1] = c as u8;
                m.b[j][2..len].copy_from_slice(src);
            } else {
                m.b[j][..len].copy_from_slice(src);
            }
        }
        Some(m)
    }

    /// The blocks' data type.
    pub fn rate(&self) -> u8 {
        if self.dt == 0 {
            DT_R12
        } else {
            self.dt
        }
    }

    /// Confirmed: block j's serial number and whether its CRC-9 holds (361-1 B.3.10).
    pub fn block_ok(&self, j: usize) -> (u8, bool) {
        let len = block_len(self.rate());
        let b = &self.b[j];
        let (dbsn, stored) = (b[0] >> 1, u16::from(b[0] & 1) << 8 | u16::from(b[1]));
        (dbsn, crc9(&b[2..len], dbsn, crc9_mask(self.rate())) == stored)
    }

    /// The user data (pad octets off) when the CRC-32 holds.
    pub fn data(&self, out: &mut [u8; MSG_OCTETS]) -> Option<usize> {
        let confirmed = self.head.dpf == DPF_CONFIRMED;
        let len = block_len(self.rate());
        let per = if confirmed { len - 2 } else { len };
        let n = self.n as usize;
        if n == 0 || n > MSG_BLOCKS || per == 0 {
            return None;
        }
        for j in 0..n {
            let src = if confirmed { &self.b[j][2..len] } else { &self.b[j][..len] };
            out[j * per..(j + 1) * per].copy_from_slice(src);
        }
        let total = n * per;
        let mut c = [0u8; 4];
        c.copy_from_slice(&out[total - 4..total]);
        if crc32(&out[..total - 4]) != u32::from_le_bytes(c) {
            return None;
        }
        (total - 4).checked_sub(self.head.pad as usize)
    }
}

/// Counts a data channel's message: a header, then its blocks; what each burst means to the controller.
#[derive(Clone, Copy, Debug, Default)]
pub struct DataRx {
    pub on: bool,
    pub confirmed: bool,
    pub left: u8,
    pub src: u32,
    pub dst: u32,
    pub group: bool,
}

impl DataRx {
    /// A data header (its CRC checked).
    pub fn header(&mut self, h: &[u8; 10]) -> u8 {
        self.on = false;
        let Some(d) = DataHead::decode(h) else { return DATA_HEADER };
        (self.src, self.dst, self.group) = (d.src, d.dst, d.group);
        if d.dpf == DPF_RESPONSE {
            return DATA_RESPONSE;
        }
        self.confirmed = d.dpf == DPF_CONFIRMED;
        self.left = d.blocks;
        self.on = d.blocks > 0;
        if self.on {
            DATA_HEADER
        } else {
            self.end()
        }
    }

    /// A data block (rate 1/2, 3/4 or 1).
    pub fn block(&mut self) -> u8 {
        if !self.on {
            return DATA_BLOCK;
        }
        self.left -= 1;
        if self.left > 0 {
            return DATA_BLOCK;
        }
        self.on = false;
        self.end()
    }

    fn end(&self) -> u8 {
        if self.confirmed {
            DATA_END_CONFIRMED
        } else {
            DATA_END
        }
    }
}

/// A data channel grant (7.1.1.1.1.4, 7.1.1.1.1.5; HI_RATE in the voice grants' Late_Entry place).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DataGrant {
    pub op: u8,
    pub chan: u16,
    pub lcn: u8,
    pub hi_rate: bool,
    pub emergency: bool,
    pub offset: bool,
    pub target: u32,
    pub source: u32,
}

impl DataGrant {
    pub fn encode(&self) -> [u8; 10] {
        let g = Grant {
            op: self.op,
            chan: self.chan,
            lcn: self.lcn,
            late_entry: self.hi_rate,
            emergency: self.emergency,
            offset: self.offset,
            target: self.target,
            source: self.source,
        };
        g.encode()
    }
    pub fn decode(b: &[u8; 10]) -> Option<DataGrant> {
        let op = b[0] & 0x3F;
        if !matches!(op, OP_PD_GRANT | OP_TD_GRANT | OP_PD_GRANT_MI | OP_TD_GRANT_MI) || b[1] != 0 {
            return None;
        }
        let be24 = |p: &[u8]| u32::from(p[0]) << 16 | u32::from(p[1]) << 8 | u32::from(p[2]);
        Some(DataGrant {
            op,
            chan: u16::from(b[2]) << 4 | u16::from(b[3] >> 4),
            lcn: (b[3] >> 3) & 1,
            hi_rate: b[3] & 0x04 != 0,
            emergency: b[3] & 0x02 != 0,
            offset: b[3] & 0x01 != 0,
            target: be24(&b[4..7]),
            source: be24(&b[7..10]),
        })
    }
    pub fn group(&self) -> bool {
        matches!(self.op, OP_TD_GRANT | OP_TD_GRANT_MI)
    }
}

/// The grant opcode for a data channel.
pub fn data_op(group: bool, multi: bool) -> u8 {
    match (group, multi) {
        (false, false) => OP_PD_GRANT,
        (true, false) => OP_TD_GRANT,
        (false, true) => OP_PD_GRANT_MI,
        (true, true) => OP_TD_GRANT_MI,
    }
}

// ── the controller's part ──────────────────────────────────────────────────────

const PD_AHOY: u8 = 1; // the called MS's availability check
const PD_QUEUED: u8 = 2; // waiting for a payload channel

#[derive(Clone, Copy, Default)]
struct Pd {
    used: bool,
    state: u8,
    group: bool,
    from_site: bool, // the site's own call (rdof_t3_pd_send): no caller on the TSCC
    svc: u8,
    prio: u8,
    ahoy_left: u8,
    caller: u32,
    called: u32,
    since_ms: i64,
    deadline_ms: i64,
    next_ahoy_ms: i64,
    next_ack_ms: i64,
}

#[derive(Clone, Copy, Default)]
pub struct PdState {
    pds: [Pd; PDS],
    multi: [bool; CHANNELS], // a multi-item call on the channel: no clearing after an item
    pub calls: u32,
    pub items: u32,
}

impl PdState {
    pub(super) fn multi_on(&self, ch: usize) -> bool {
        self.multi.get(ch).copied().unwrap_or(false)
    }
}

impl Site {
    fn pd_hang(&self) -> i64 {
        self.cfg.data_hang_ms.max(500) as i64
    }

    /// The site itself as a party: TSI, or its data ID.
    pub(super) fn pd_site(&self, a: u32) -> bool {
        a == TSI || (self.cfg.data_id != 0 && a == self.cfg.data_id)
    }

    /// A request's called party is the site: TSI or its data ID, or IPI with no extended address to upload (the
    /// destination is the datagram's, 6.6.3.3: derived from the DLL address).
    pub(super) fn pd_to_site(&self, r: &Rand) -> bool {
        r.service_kind == SK_IND_PD && (self.pd_site(r.target) || (r.target == IPI && !r.proxy && r.dep == 0))
    }

    /// A data channel open for these parties (either way round for an individual call).
    fn pd_channel(&self, group: bool, a: u32, b: u32) -> Option<usize> {
        (0..CHANNELS).find(|&i| {
            let c = &self.chans[i];
            Self::live(c)
                && c.flags & F_DATA != 0
                && (c.kind != KIND_PRIVATE) == group
                && if group { c.dst == b } else { (c.src == a && c.dst == b) || (c.src == b && c.dst == a) }
        })
    }

    fn pd_free(&self) -> Option<usize> {
        self.pd.pds.iter().position(|p| !p.used)
    }

    /// C_RAND for packet data (6.6.3.1.1): the checks, then the availability check, a channel or the queue.
    pub(super) fn pd_rand(&mut self, r: &Rand) {
        let (ms, dst) = (r.source, r.target);
        let group = r.service_kind == SK_TG_PD;
        if let Some(i) = self.pd.pds.iter().position(|p| p.used && !p.from_site && p.caller == ms && p.called == dst) {
            let reason = if self.pd.pds[i].state == PD_QUEUED { R_QUEUED } else { R_WAIT };
            self.ack(reason, ms, dst); // a repeat: the same answer
            return;
        }
        let Some(prio) = self.admit(r) else { return };
        if let Some(ch) = self.pd_channel(group, ms, dst) {
            self.regrant(ch, ms);
            return;
        }
        let site = self.pd_to_site(r);
        if !group && !site {
            if self.reg_index(dst).is_none() {
                self.refuse(R_NOREG_MSAWAY, ms, dst); // packet data across the network: not built
                return;
            }
            if self.ms_busy(dst) || self.pd_busy(dst) {
                self.refuse(R_CALLED_PARTY_BUSY, ms, dst);
                return;
            }
        }
        let Some(i) = self.pd_free() else {
            self.refuse(R_SYSBUSY, ms, dst);
            return;
        };
        self.pd.pds[i] = Pd {
            used: true,
            group,
            svc: r.service_options,
            prio,
            caller: ms,
            called: dst,
            since_ms: self.now,
            ..Default::default()
        };
        if group || site {
            self.pd_ready(i);
        } else {
            self.pd_check(i);
            self.ack(R_WAIT, ms, dst);
        }
    }

    /// Whether an MS is a party of a data call being set up.
    fn pd_busy(&self, ms: u32) -> bool {
        self.pd.pds.iter().any(|p| p.used && !p.group && (p.caller == ms || p.called == ms))
    }

    /// Starts the called MS's availability check (6.6.3.1.4 a).
    fn pd_check(&mut self, i: usize) {
        let now = self.now;
        let p = &mut self.pd.pds[i];
        p.state = PD_AHOY;
        p.ahoy_left = self.cfg.ahoy_tries.max(1);
        p.next_ahoy_ms = now;
        p.deadline_ms = now + self.cfg.tp_ms as i64;
        p.next_ack_ms = now + WACK_EVERY_MS;
        self.pd_ahoy(i);
    }

    fn pd_ahoy(&mut self, i: usize) {
        let p = self.pd.pds[i];
        if p.ahoy_left == 0 {
            self.pd.pds[i].used = false;
            self.pd_fail(&p, R_MSAWAY);
            return;
        }
        // table 6.49: Service_Options_Mirror the request's, Service_Kind 0010b, source the caller
        let a = Ahoy {
            service_options: p.svc & 0x7F,
            service_kind: SK_IND_PD,
            target: p.called,
            source: p.caller,
            ..Default::default()
        };
        self.push(DT_CSBK, a.encode(), true, None);
        let q = &mut self.pd.pds[i];
        q.ahoy_left -= 1;
        q.next_ahoy_ms = self.now + self.cfg.ahoy_every_ms as i64;
    }

    /// Ready for a channel: one now, or the queue (C_QACKD Queued_for_Resource).
    fn pd_ready(&mut self, i: usize) {
        if let Some(ch) = self.free_channel() {
            self.pd_go(i, ch);
            return;
        }
        let full = self.queue_len() + self.pd_queued() >= self.cfg.queue_max as usize;
        let p = self.pd.pds[i];
        if full {
            self.pd.pds[i].used = false;
            self.pd_fail(&p, R_SYSBUSY);
            return;
        }
        let now = self.now;
        let q = &mut self.pd.pds[i];
        q.state = PD_QUEUED;
        q.deadline_ms = now + self.cfg.queue_ms as i64;
        q.next_ack_ms = now + QACK_EVERY_MS;
        if !p.from_site {
            self.ack(R_QUEUED, p.caller, p.called);
        }
        self.counters.queued += 1;
        self.activity(ACT_QUEUE, QUEUE_IN, p.caller, p.called, 0);
    }

    fn pd_queued(&self) -> usize {
        self.pd.pds.iter().filter(|p| p.used && p.state == PD_QUEUED).count()
    }

    /// The call on channel ch: grants (PD_GRANT or TD_GRANT, single or multi-item as asked), the data hang time.
    fn pd_go(&mut self, i: usize, ch: usize) {
        let p = self.pd.pds[i];
        self.pd.pds[i].used = false;
        let mut flags = F_DATA;
        let kind = if !p.group {
            KIND_PRIVATE
        } else if is_allcall(p.called) {
            flags |= F_GROUP | F_ALLCALL;
            KIND_ALLCALL
        } else {
            flags |= F_GROUP;
            KIND_GROUP
        };
        if p.svc & SO_EMERGENCY != 0 {
            flags |= F_EMERGENCY;
        }
        self.pd.multi[ch] = p.svc & SO_SIMI != 0;
        self.open(ch, kind, p.caller, p.called, flags, 0, false);
        self.chans[ch].idle_until_ms = self.now + self.cfg.start_grace_ms as i64 + self.pd_hang();
        self.pd.calls += 1;
        let site = !p.group && (self.pd_site(p.called) || p.called == IPI || p.from_site);
        let mut e = self.chan_ev(ch, EV_PD);
        e.op = PD_OPEN;
        e.kind = site as u8;
        self.ev(e);
    }

    fn pd_fail(&mut self, p: &Pd, reason: u8) {
        if p.from_site {
            self.ev(Event {
                r#type: EV_PD,
                op: PD_FAILED,
                reason,
                src: p.caller,
                dst: p.called,
                flags: if p.group { F_GROUP } else { 0 },
                ..Default::default()
            });
        } else {
            self.refuse(reason, p.caller, p.called);
        }
    }

    /// Queued data calls get the channels voice leaves free (voice goes first).
    pub(super) fn pd_serve(&mut self) {
        while let Some(ch) = self.free_channel() {
            let best =
                (0..PDS).filter(|&i| self.pd.pds[i].used && self.pd.pds[i].state == PD_QUEUED).min_by_key(|&i| {
                    (self.pd.pds[i].svc & SO_EMERGENCY == 0, 3 - self.pd.pds[i].prio.min(3), self.pd.pds[i].since_ms)
                });
            let Some(i) = best else { return };
            let p = self.pd.pds[i];
            self.activity(ACT_QUEUE, QUEUE_OUT, p.caller, p.called, 0);
            self.pd_go(i, ch);
        }
    }

    /// The called MS's answer to the availability check: true when it was one.
    pub(super) fn pd_ack(&mut self, a: &Ack) -> bool {
        let Some(i) = self
            .pd
            .pds
            .iter()
            .position(|p| p.used && p.state == PD_AHOY && p.called == a.source && p.caller == a.target)
        else {
            return false;
        };
        let p = self.pd.pds[i];
        match (reason_type(a.reason), a.reason) {
            // C_ACKU(Message_Accepted): a channel (6.6.3.1.4, 6.6.3.2.6); MS_Accepted is the same answer with the
            // MS-sent direction bit (table 7.42, 7.2.8.0)
            (1, R_MESSAGE_ACCEPTED | R_MS_ACCEPTED) => self.pd_ready(i),
            // it will call back: no channel, the reason mirrored to the caller (as for voice, 6.6.2.2.6)
            (1, R_CALLBACK) => {
                self.pd.pds[i].used = false;
                self.pd_fail(&p, a.reason);
            }
            // another C_ACKU (MS_Alerting, ...) doesn't accept a data call: the check goes on, its repeats and then
            // no answer
            (1, _) => {}
            _ => {
                self.pd.pds[i].used = false;
                self.pd_fail(&p, a.reason); // C_NACKU: its reason, mirrored
            }
        }
        true
    }

    /// Cancel from a caller whose data call waits: true when it was one.
    pub(super) fn pd_cancel(&mut self, ms: u32) -> bool {
        let Some(i) = self.pd.pds.iter().position(|p| p.used && !p.from_site && p.caller == ms) else {
            return false;
        };
        let p = self.pd.pds[i];
        self.pd.pds[i].used = false;
        self.ack(R_MESSAGE_ACCEPTED, ms, p.called);
        if p.state == PD_QUEUED {
            self.activity(ACT_QUEUE, QUEUE_DROP, p.caller, p.called, 0);
        }
        true
    }

    pub(super) fn pd_timers(&mut self) {
        let now = self.now;
        for i in 0..PDS {
            let p = self.pd.pds[i];
            if !p.used {
                continue;
            }
            if now >= p.deadline_ms {
                self.pd.pds[i].used = false;
                let reason = if p.state == PD_QUEUED { R_SYSBUSY } else { R_MSAWAY };
                if p.state == PD_QUEUED {
                    self.activity(ACT_QUEUE, QUEUE_DROP, p.caller, p.called, reason);
                }
                self.pd_fail(&p, reason);
                continue;
            }
            if p.state == PD_AHOY && now >= p.next_ahoy_ms {
                self.pd_ahoy(i);
                if !self.pd.pds[i].used {
                    continue;
                }
            }
            if !p.from_site && now >= p.next_ack_ms {
                let (reason, every) =
                    if p.state == PD_QUEUED { (R_QUEUED, QACK_EVERY_MS) } else { (R_WAIT, WACK_EVERY_MS) };
                self.ack(reason, p.caller, p.called);
                self.pd.pds[i].next_ack_ms = now + every;
            }
        }
    }

    /// The radio layer heard data on channel ch (6.6.3.3.1.6 b): any burst holds the channel for the data hang
    /// time; the end of an unconfirmed single item clears it once repeated.
    pub fn rx_data(&mut self, ch: usize, what: u8, src: u32) {
        if self.withdrawn || ch >= CHANNELS || !Self::live(&self.chans[ch]) || self.chans[ch].flags & F_DATA == 0 {
            return;
        }
        if src != 0 && !is_gateway(src) && self.reg_index(src).is_some() {
            self.heard(src);
        }
        let now = self.now;
        let hang = self.pd_hang();
        let multi = self.pd.multi[ch];
        let c = &mut self.chans[ch];
        match what {
            DATA_HEADER | DATA_BLOCK => {
                c.r#use = USE_CALL;
                c.last_voice_ms = now;
                c.idle_until_ms = now + hang;
                if src != 0 {
                    c.talker = src;
                }
            }
            DATA_END | DATA_END_CONFIRMED | DATA_RESPONSE => {
                c.r#use = USE_HANG;
                c.idle_until_ms = now + if what == DATA_END && !multi { DRAIN_MS } else { hang };
                if what != DATA_RESPONSE {
                    self.pd.items += 1;
                }
            }
            _ => {}
        }
    }

    /// The site's own data to an MS or a talkgroup (its IP gateway's): a channel to put it on. src is the ID it
    /// comes from (the site's data ID or TSI). An MS gets the availability check first. false: not here or busy.
    pub fn pd_send(&mut self, src: u32, dst: u32, group: bool, now_ms: i64) -> bool {
        self.now = now_ms;
        if self.withdrawn {
            return false;
        }
        if let Some(ch) = self.pd_channel(group, src, dst) {
            let mut e = self.chan_ev(ch, EV_PD);
            e.op = PD_OPEN;
            e.kind = (!group) as u8;
            (e.src, e.dst) = (src, dst); // the send's, for its sender to match
            self.ev(e); // already open: the data can go now
            return true;
        }
        if self.pd.pds.iter().any(|p| p.used && p.from_site && p.called == dst && p.group == group) {
            return true; // on its way
        }
        if group && !is_allcall(dst) && !self.policy.tg.contains(dst) {
            return false;
        }
        if !group && (self.reg_index(dst).is_none() || self.ms_busy(dst) || self.pd_busy(dst)) {
            return false;
        }
        let Some(i) = self.pd_free() else { return false };
        self.pd.pds[i] =
            Pd { used: true, group, from_site: true, caller: src, called: dst, since_ms: now_ms, ..Default::default() };
        if group {
            self.pd_ready(i);
        } else {
            self.pd_check(i);
        }
        true
    }
}

// ── C API (rdof/t3.h) ──────────────────────────────────────────────────────────

#[no_mangle]
pub unsafe extern "C" fn rdof_t3_rx_data(s: *mut Site, ch: c_int, _k: i64, what: u8, src: u32) {
    if ch >= 0 {
        (*s).rx_data(ch as usize, what, src);
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3_pd_send(s: *mut Site, src: u32, dst: u32, group: bool, now_ms: i64) -> bool {
    (*s).pd_send(src, dst, group, now_ms)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn data_header_round_trip() {
        // 361-1 9.2.1/9.2.2/9.2.3
        let h = DataHead {
            dpf: DPF_CONFIRMED,
            group: false,
            response: true,
            sap: SAP_IP,
            pad: 0x13,
            dst: 1001,
            src: 2002,
            full: true,
            blocks: 5,
            ns: 6,
            fsn: 8,
            ..Default::default()
        };
        let e = h.encode();
        assert_eq!(e[0], 0x40 | 0x10 | 3);
        assert_eq!(e[1], 0x43);
        assert_eq!(e[8], 0x85);
        assert_eq!(e[9], 0x68);
        assert_eq!(DataHead::decode(&e), Some(h));
        let r = DataHead {
            dpf: DPF_RESPONSE,
            sap: SAP_IP,
            dst: 2002,
            src: 1001,
            class: 0,
            rtype: 1,
            status: 6,
            ..Default::default()
        };
        assert_eq!(r.encode()[9], 0x0E);
        assert_eq!(DataHead::decode(&r.encode()), Some(r));
        let mut u = [0u8; 10];
        u[0] = 0x0D; // short data defined: not counted here
        assert_eq!(DataHead::decode(&u), None);
    }

    #[test]
    fn messages_and_crcs() {
        let data: [u8; 30] = core::array::from_fn(|i| i as u8 * 7);
        for dpf in [DPF_UNCONFIRMED, DPF_CONFIRMED] {
            let h = DataHead { dpf, sap: SAP_IP, dst: 1, src: 2, ..Default::default() };
            let m = DataMsg::build(h, &data).unwrap();
            assert_eq!(m.n, if dpf == DPF_CONFIRMED { 4 } else { 3 });
            let mut out = [0u8; MSG_OCTETS];
            assert_eq!(m.data(&mut out), Some(30));
            assert_eq!(&out[..30], &data);
            let mut bad = m;
            bad.b[1][5] ^= 1;
            assert_eq!(bad.data(&mut out), None);
            if dpf == DPF_CONFIRMED {
                for j in 0..m.n as usize {
                    let c = u16::from(m.b[j][0] & 1) << 8 | u16::from(m.b[j][1]);
                    assert_eq!(c, crc9(&m.b[j][2..12], m.b[j][0] >> 1, CRC9_RATE12));
                }
            }
        }
        assert!(DataMsg::build(DataHead::default(), &[0; 12 * MSG_BLOCKS]).is_none());
    }

    #[test]
    fn counting_a_message() {
        let mut rx = DataRx::default();
        let h = DataHead { dpf: DPF_UNCONFIRMED, blocks: 2, dst: 9, src: 8, group: true, ..Default::default() };
        assert_eq!(rx.header(&h.encode()), DATA_HEADER);
        assert_eq!((rx.src, rx.dst, rx.group), (8, 9, true));
        assert_eq!(rx.block(), DATA_BLOCK);
        assert_eq!(rx.block(), DATA_END);
        assert_eq!(rx.block(), DATA_BLOCK); // no header: just activity
        let c = DataHead { dpf: DPF_CONFIRMED, blocks: 1, ..h };
        assert_eq!(rx.header(&c.encode()), DATA_HEADER);
        assert_eq!(rx.block(), DATA_END_CONFIRMED);
        let r = DataHead { dpf: DPF_RESPONSE, rtype: 1, ..h };
        assert_eq!(rx.header(&r.encode()), DATA_RESPONSE);
    }

    #[test]
    fn data_grant_layout() {
        // 7.1.1.1.1.4 table 7.13: HI_RATE where the voice grants have Late_Entry
        let g = DataGrant {
            op: OP_PD_GRANT,
            chan: 0x123,
            lcn: 1,
            hi_rate: true,
            emergency: false,
            offset: true,
            target: 1001,
            source: 2002,
        };
        let b = g.encode();
        assert_eq!(b[0], 0x80 | OP_PD_GRANT);
        assert_eq!(b[3], 0x30 | 0x08 | 0x04 | 0x01);
        assert_eq!(DataGrant::decode(&b), Some(g));
        assert!(DataGrant::decode(&Grant { op: OP_PV_GRANT, ..Default::default() }.encode()).is_none());
        assert_eq!(data_op(true, true), OP_TD_GRANT_MI);
    }
}
