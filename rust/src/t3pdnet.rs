// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Packet data across sites (docs/spec/fw/tier3.md 3.16 and 5.10): the site's part of a data call whose far party
//! is on the network. The controller grants the channel (PD_GRANT or TD_GRANT, EV_PD PD_OPEN with kind PARTY_NET);
//! this takes the message the MS sends there, answers it on the air when it is confirmed, and gives it whole to the
//! caller to publish; and it sends a message from the network to the MS, as confirmed data with its retries when it
//! came confirmed. Store and forward, a whole message at a time: the confirmed-data procedure stays on each air link
//! (ETSI TS 102 361-3 5.4: the response goes within T_RspnsWait, too soon for a round trip through the network; 361-3
//! clause 5 leaves relaying between base stations out of scope), as mqtt-net.md 4.4 has it for every box.
//!
//! Blocks here are octets (a header's 10, a block's 12, 18 or 24); the caller makes bursts of them (the unit, with
//! the radio layer) or hands them to MSs (the simulated air). No allocation, no I/O.

use crate::t3pdrate::{rdata_retry, response, DataAsm, Taken, CLASS_ACK, CLASS_NACK, CLASS_SACK, TRIES, TYPE_ACK};
use crate::t3pdrate::TYPE_NACK_CRC;
use crate::t3site::{
    crc32, DataHead, DataMsg, Event, BLOCK_MAX, CHANNELS, DPF_CONFIRMED, DPF_RESPONSE, DT_R1, DT_R12, DT_R34,
    EV_CH_CLOSE, EV_PD, F_GROUP, MSG_OCTETS, PARTY_NET, PD_FAILED, PD_OPEN,
};
use crate::t3udt::DT_DATA_HDR;

/// How long a sender waits for the response after its last block is queued (the queue ahead of the air, the MS's
/// T_RspnsWait and its answer), before it sends the message again.
pub const RESP_WAIT_MS: i64 = 3000;
/// A message goes this long after its grant: the MS has it on the TSCC (twice) and moves to the channel.
pub const GRANT_LEAD_MS: i64 = 360;
/// How long a message from the network waits for its channel.
pub const HOLD_MS: i64 = 30_000;
/// Messages from the network waiting for a channel, or for their channel's sender.
pub const HELD: usize = 4;
const RESULTS: usize = 4;

/// What became of a message sent to an MS (mqtt-net.md 7, `data_result`).
pub const RESULT_DELIVERED: u8 = 1;
pub const RESULT_FAILED: u8 = 2;
pub const RESULT_NO_RESPONSE: u8 = 3;
pub const RESULT_UNCONFIRMED: u8 = 4;

/// A packet data message as it goes over the network (the data_pdu item, mqtt-net.md 4.2, 4.4): the data header
/// as it came (F = 1), the blocks' data type and the user data, without serial numbers, CRCs or pad.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct NetData {
    pub head: DataHead,
    pub dt: u8,
    pub len: usize,
    pub data: [u8; MSG_OCTETS],
}

impl Default for NetData {
    fn default() -> Self {
        NetData { head: DataHead::default(), dt: DT_R12, len: 0, data: [0; MSG_OCTETS] }
    }
}

impl NetData {
    /// A whole message's (its CRC-32 good).
    pub fn of_msg(m: &DataMsg) -> Option<NetData> {
        let mut data = [0u8; MSG_OCTETS];
        let len = m.data(&mut data)?;
        Some(NetData { head: DataHead { full: true, ..m.head }, dt: m.rate(), len, data })
    }

    /// The message again, its blocks, pad, serial numbers and CRCs made for its rate (mqtt-net.md 4.4).
    pub fn msg(&self) -> Option<DataMsg> {
        DataMsg::build_rate(DataHead { full: true, ..self.head }, &self.data[..self.len], self.dt)
    }

    /// The data_pdu rate code: 0 rate 1/2, 1 rate 3/4, 2 rate 1.
    pub fn rate(&self) -> u8 {
        match self.dt {
            DT_R34 => 1,
            DT_R1 => 2,
            _ => 0,
        }
    }

    pub fn dt_of_rate(rate: u8) -> Option<u8> {
        [DT_R12, DT_R34, DT_R1].get(usize::from(rate)).copied()
    }
}

/// A block to send: its data type (DT_DATA_HDR for a header) and its octets.
pub type Block = (u8, [u8; BLOCK_MAX]);

/// One channel's relay: the message the MS sends to the far party, taken and answered; a message from the far
/// party, sent with the confirmed procedure's retries.
#[derive(Clone, Copy, Debug, Default)]
pub struct Relay {
    pub on: bool,
    pub src: u32,
    pub dst: u32,
    pub group: bool,
    asm: DataAsm,
    taking: bool,
    last: Option<(u32, u8, u32)>, // the last confirmed message passed on: source, N(S), CRC-32 of its data
    resp: Option<[u8; 10]>,       // a response it owes the MS, and a SACK's C_RDATA
    resp_blk: Option<[u8; 12]>,
    // its message to the MS
    out: DataMsg,
    sending: bool,
    pos: u8,
    send: u16,
    pub tries: u8,
    waiting: bool,
    rdata_next: bool,
    until_ms: i64,
    pub from: u32, // the network stream it came in (for its result)
    result: Option<u8>,
    start_ms: i64, // its message not before (the grant's lead)
}

impl Relay {
    fn open(&mut self, src: u32, dst: u32, group: bool) {
        *self = Relay { on: true, src, dst, group, ..Default::default() };
    }

    /// A message of the channel's call: its talkgroup, or between its two parties (either way).
    fn ours(&self, h: &DataHead) -> bool {
        if self.group {
            h.group && h.dst == self.dst
        } else {
            !h.group && ((h.src == self.src && h.dst == self.dst) || (h.src == self.dst && h.dst == self.src))
        }
    }

    /// Whether it is sending a message, or waiting for its response.
    pub fn busy(&self) -> bool {
        self.sending || self.waiting
    }

    fn all(&self) -> u16 {
        (1u16 << self.out.n) - 1
    }

    /// A header or block an MS sent on the channel. Some: a whole message for the far party.
    pub fn rx(&mut self, dt: u8, blk: &[u8; BLOCK_MAX], now: i64) -> Option<NetData> {
        if dt == DT_DATA_HDR {
            let mut p = [0u8; 10];
            p.copy_from_slice(&blk[..10]);
            self.rdata_next = false;
            self.taking = false;
            let h = DataHead::decode(&p)?;
            if h.dpf == DPF_RESPONSE {
                if self.waiting && h.dst == self.out.head.src && h.src == self.out.head.dst {
                    self.on_response(&h, now);
                }
                return None;
            }
            self.taking = self.ours(&h) && self.asm.header(&h);
            return None;
        }
        if self.rdata_next && dt == DT_R12 {
            // a SACK's C_RDATA: the blocks to send again (361-3 5.4.1)
            self.rdata_next = false;
            if let Some(retry) = rdata_retry(&blk[..12], self.out.n) {
                self.resend(if retry == 0 { self.all() } else { retry }, RESULT_FAILED);
            }
            return None;
        }
        if !self.taking {
            return None;
        }
        let t = self.asm.block(dt, blk)?;
        self.taking = false;
        let h = self.asm.m.head;
        if h.dpf == DPF_CONFIRMED && h.response && !h.group {
            // answered here, as the called party would (361-1 8.2.2.3, table 8.3): the network has it
            let (r, rd) = response(&h, h.dst, t, self.asm.m.n);
            self.resp = Some(r.encode());
            self.resp_blk = rd;
        }
        if t != Taken::Ok {
            return None;
        }
        let nd = NetData::of_msg(&self.asm.m)?;
        if h.dpf == DPF_CONFIRMED {
            // the same N(S) again (its response lost): answered again, passed on once (361-3 5.4.2.2)
            let key = (h.src, h.ns, crc32(&nd.data[..nd.len]));
            if self.last == Some(key) {
                return None;
            }
            self.last = Some(key);
        }
        Some(nd)
    }

    /// Starts sending a message from the network; false when it can't be built.
    pub fn send(&mut self, nd: &NetData, from: u32) -> bool {
        let Some(m) = nd.msg() else { return false };
        self.out = m;
        self.sending = true;
        self.pos = 0;
        self.send = self.all();
        self.tries = 1;
        self.waiting = false;
        self.rdata_next = false;
        self.from = from;
        self.result = None;
        true
    }

    /// The next block to send on the channel: a response it owes, else its message's.
    pub fn next(&mut self, now: i64) -> Option<Block> {
        let mut b = [0u8; BLOCK_MAX];
        if let Some(r) = self.resp.take() {
            b[..10].copy_from_slice(&r);
            return Some((DT_DATA_HDR, b));
        }
        if let Some(r) = self.resp_blk.take() {
            b[..12].copy_from_slice(&r);
            return Some((DT_R12, b)); // C_RDATA: rate 1/2 (361-3 5.4.1.3)
        }
        if !self.sending || now < self.start_ms {
            return None;
        }
        let n = self.out.n as usize;
        if self.pos == 0 {
            // C_HEAD: F = 1 with every block, F = 0 with a SACK's (361-3 5.4.1)
            self.pos = 1;
            let h = DataHead { full: self.send == self.all(), blocks: self.send.count_ones() as u8, ..self.out.head };
            b[..10].copy_from_slice(&h.encode());
            return Some((DT_DATA_HDR, b));
        }
        let mut j = (self.pos - 1) as usize;
        while j < n && self.send >> j & 1 == 0 {
            j += 1;
        }
        if j >= n {
            self.sending = false;
            return None;
        }
        self.pos = j as u8 + 2;
        if (j + 1..n).all(|i| self.send >> i & 1 == 0) {
            self.sending = false;
            if self.out.head.dpf == DPF_CONFIRMED && self.out.head.response && !self.out.head.group {
                self.waiting = true;
                self.until_ms = now + RESP_WAIT_MS;
            } else {
                self.result = Some(RESULT_UNCONFIRMED);
            }
        }
        Some((self.out.rate(), self.out.b[j]))
    }

    /// The MS's response to its message (361-1 table 8.3): ACK; SACK, its C_RDATA next; NACK for the message's
    /// CRC, all again; anything else ends it.
    fn on_response(&mut self, h: &DataHead, now: i64) {
        self.waiting = false;
        match (h.class, h.rtype) {
            (CLASS_ACK, TYPE_ACK) => self.result = Some(RESULT_DELIVERED),
            (CLASS_SACK, _) if h.blocks > 0 => {
                self.waiting = true;
                self.rdata_next = true;
                self.until_ms = self.until_ms.max(now + 1000);
            }
            (CLASS_NACK, TYPE_NACK_CRC) => self.resend(self.all(), RESULT_FAILED),
            _ => self.result = Some(RESULT_FAILED),
        }
    }

    /// The blocks in mask again (all: a C_HEAD with F = 1; some: F = 0), up to TRIES sends; then the result.
    fn resend(&mut self, mask: u16, last: u8) {
        self.waiting = false;
        if self.tries >= TRIES {
            self.result = Some(last);
            return;
        }
        self.tries += 1;
        self.send = mask & self.all();
        self.pos = 0;
        self.sending = true;
    }

    /// No response in time: the message again (361-3 5.4.1: T_RspnsWait, then a retry).
    pub fn tick(&mut self, now: i64) {
        if self.waiting && now >= self.until_ms {
            self.resend(self.all(), RESULT_NO_RESPONSE);
        }
    }
}

/// A message from the network for an MS or a talkgroup here, before its channel.
#[derive(Clone, Copy, Debug, Default)]
pub struct Held {
    pub on: bool,
    pub src: u32,
    pub dst: u32,
    pub group: bool,
    pub from: u32, // its network stream
    pub until_ms: i64,
    pub nd: NetData,
}

/// What became of a message from the network: (source, destination, its stream, result, sends).
pub type NetResult = (u32, u32, u32, u8, u8);

/// The site's relays and the messages waiting for them.
#[derive(Clone, Copy, Debug, Default)]
pub struct PdNet {
    pub relay: [Relay; CHANNELS],
    pub held: [Held; HELD],
    results: [Option<NetResult>; RESULTS],
}

impl PdNet {
    /// The controller's events: a channel with the network as the far party opens (its message goes if one waits),
    /// one closes, a call from the network fails.
    pub fn event(&mut self, e: &Event, now: i64) {
        let ch = usize::from(e.ch);
        let group = e.flags & F_GROUP != 0;
        match e.r#type {
            EV_PD if e.op == PD_OPEN && e.kind == PARTY_NET && ch < CHANNELS => {
                let r = &self.relay[ch];
                let same = r.on && r.group == group && r.ours(&DataHead { group, src: e.src, dst: e.dst, ..Default::default() });
                if !same {
                    self.relay[ch].open(e.src, e.dst, group);
                }
                if !self.relay[ch].busy() {
                    self.relay[ch].start_ms = now + GRANT_LEAD_MS; // granted (again): the MS comes to the channel
                }
                self.feed(now);
            }
            EV_PD if e.op == PD_FAILED && e.kind == PARTY_NET => {
                for i in 0..HELD {
                    let h = self.held[i];
                    if h.on && h.src == e.src && h.dst == e.dst && h.group == group {
                        self.held[i].on = false;
                        self.result((h.src, h.dst, h.from, RESULT_FAILED, 0));
                    }
                }
            }
            EV_CH_CLOSE if ch < CHANNELS => {
                let r = self.relay[ch];
                if r.on && r.busy() {
                    self.result((r.out.head.src, r.out.head.dst, r.from, RESULT_FAILED, r.tries));
                }
                self.relay[ch] = Relay::default();
            }
            _ => {}
        }
    }

    /// A message from the network, held for its channel: false when none is free (the caller then asks the
    /// controller for a channel, and drops it with drop_held if it can't).
    pub fn hold(&mut self, nd: &NetData, from: u32, now: i64) -> bool {
        let Some(i) = self.held.iter().position(|h| !h.on) else { return false };
        let (src, dst, group) = (nd.head.src, nd.head.dst, nd.head.group);
        self.held[i] = Held { on: true, src, dst, group, from, until_ms: now + HOLD_MS, nd: *nd };
        self.feed(now);
        true
    }

    pub fn drop_held(&mut self, from: u32) {
        for h in self.held.iter_mut().filter(|h| h.on && h.from == from) {
            h.on = false;
        }
    }

    /// Held messages to relays of their call that are free, oldest first.
    fn feed(&mut self, now: i64) {
        for ch in 0..CHANNELS {
            if !self.relay[ch].on || self.relay[ch].busy() {
                continue;
            }
            let r = self.relay[ch];
            let best = (0..HELD)
                .filter(|&i| {
                    let h = &self.held[i];
                    h.on && h.group == r.group && r.ours(&h.nd.head)
                })
                .min_by_key(|&i| self.held[i].until_ms);
            if let Some(i) = best {
                let h = self.held[i];
                self.held[i].on = false;
                if !self.relay[ch].send(&h.nd, h.from) {
                    self.result((h.src, h.dst, h.from, RESULT_FAILED, 0));
                }
            }
        }
        let _ = now;
    }

    /// A header or block an MS sent on channel ch; Some: a message to publish.
    pub fn rx(&mut self, ch: usize, dt: u8, blk: &[u8; BLOCK_MAX], now: i64) -> Option<NetData> {
        let r = self.relay.get_mut(ch).filter(|r| r.on)?;
        let nd = r.rx(dt, blk, now);
        self.collect(ch);
        nd
    }

    /// The next block to send on channel ch.
    pub fn next(&mut self, ch: usize, now: i64) -> Option<Block> {
        let r = self.relay.get_mut(ch).filter(|r| r.on)?;
        let b = r.next(now);
        self.collect(ch);
        b
    }

    /// Timers: responses not come, messages held too long; a relay done goes on to the next message held.
    pub fn tick(&mut self, now: i64) {
        for ch in 0..CHANNELS {
            if self.relay[ch].on {
                self.relay[ch].tick(now);
                self.collect(ch);
            }
        }
        for i in 0..HELD {
            let h = self.held[i];
            if h.on && now >= h.until_ms {
                self.held[i].on = false;
                self.result((h.src, h.dst, h.from, RESULT_FAILED, 0));
            }
        }
        self.feed(now);
    }

    /// Whether a channel's relay has something to send.
    pub fn sending(&self, ch: usize) -> bool {
        self.relay.get(ch).is_some_and(|r| r.on && (r.sending || r.resp.is_some() || r.resp_blk.is_some()))
    }

    fn collect(&mut self, ch: usize) {
        let r = &mut self.relay[ch];
        if let Some(res) = r.result.take() {
            let x = (r.out.head.src, r.out.head.dst, r.from, res, r.tries);
            self.result(x);
        }
    }

    fn result(&mut self, x: NetResult) {
        if let Some(s) = self.results.iter_mut().find(|s| s.is_none()) {
            *s = Some(x);
        } else {
            self.results.rotate_left(1);
            self.results[RESULTS - 1] = Some(x);
        }
    }

    /// The next result to report.
    pub fn take_result(&mut self) -> Option<NetResult> {
        let i = self.results.iter().position(|s| s.is_some())?;
        self.results[i].take()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::t3pdrate::rdata;
    use crate::t3site::{DPF_UNCONFIRMED, SAP_IP};

    const A: u32 = 3100001;
    const B: u32 = 3100101;

    fn head(dpf: u8) -> DataHead {
        DataHead { dpf, response: dpf == DPF_CONFIRMED, sap: SAP_IP, dst: B, src: A, ns: 3, ..Default::default() }
    }

    fn open(p: &mut PdNet, ch: u8, src: u32, dst: u32) {
        let e = Event { r#type: EV_PD, op: PD_OPEN, kind: PARTY_NET, ch, src, dst, ..Default::default() };
        p.event(&e, 0);
    }

    /// Every block a relay sends until it stops.
    fn drain(p: &mut PdNet, ch: usize, now: i64) -> Vec<Block> {
        core::iter::from_fn(|| p.next(ch, now)).collect()
    }

    fn hdr(b: &Block) -> DataHead {
        let mut p = [0u8; 10];
        p.copy_from_slice(&b.1[..10]);
        DataHead::decode(&p).unwrap()
    }

    const TEXT: &[u8] = b"across the network, a confirmed message of some length";

    #[test]
    fn takes_and_answers_a_confirmed_message() {
        let mut p = PdNet::default();
        open(&mut p, 2, A, B);
        let m = DataMsg::build(head(DPF_CONFIRMED), TEXT).unwrap();
        let mut hb = [0u8; BLOCK_MAX];
        hb[..10].copy_from_slice(&m.head.encode());
        assert_eq!(p.rx(2, DT_DATA_HDR, &hb, 0), None);
        // block 1 damaged: a SACK listing it
        let mut got = None;
        for j in 0..m.n as usize {
            let mut b = m.b[j];
            if j == 1 {
                b[5] ^= 0x10;
            }
            got = got.or(p.rx(2, DT_R12, &b, 0));
        }
        assert_eq!(got, None);
        let r = drain(&mut p, 2, 0);
        let rh = hdr(&r[0]);
        assert_eq!((rh.dpf, rh.class, rh.src, rh.dst, rh.status), (DPF_RESPONSE, CLASS_SACK, B, A, 3));
        assert_eq!(rdata_retry(&r[1].1[..12], m.n), Some(0b10));
        // the retry: C_HEAD F = 0, one block; then the ACK and the whole message, once
        let h0 = DataHead { full: false, blocks: 1, ..m.head };
        hb[..10].copy_from_slice(&h0.encode());
        p.rx(2, DT_DATA_HDR, &hb, 0);
        let nd = p.rx(2, DT_R12, &m.b[1], 0).expect("whole");
        assert_eq!((&nd.data[..nd.len], nd.head.src, nd.head.dst, nd.dt), (TEXT, A, B, DT_R12));
        let r = drain(&mut p, 2, 0);
        assert_eq!((r.len(), hdr(&r[0]).class), (1, CLASS_ACK));
        // the whole message again (its ACK lost): answered, not passed on twice
        hb[..10].copy_from_slice(&m.head.encode());
        p.rx(2, DT_DATA_HDR, &hb, 0);
        let again: Vec<_> = (0..m.n as usize).filter_map(|j| p.rx(2, DT_R12, &m.b[j], 0)).collect();
        assert!(again.is_empty());
        assert_eq!(hdr(&drain(&mut p, 2, 0)[0]).class, CLASS_ACK);
        // another call's data on the channel is not taken
        let other = DataMsg::build(DataHead { src: 42, ..head(DPF_UNCONFIRMED) }, TEXT).unwrap();
        hb[..10].copy_from_slice(&other.head.encode());
        p.rx(2, DT_DATA_HDR, &hb, 0);
        assert!((0..other.n as usize).all(|j| p.rx(2, DT_R12, &other.b[j], 0).is_none()));
    }

    #[test]
    fn net_data_round_trip_at_each_rate() {
        for dt in [DT_R12, DT_R34, DT_R1] {
            for dpf in [DPF_UNCONFIRMED, DPF_CONFIRMED] {
                let m = DataMsg::build_rate(head(dpf), TEXT, dt).unwrap();
                let nd = NetData::of_msg(&m).unwrap();
                assert_eq!((nd.len, NetData::dt_of_rate(nd.rate())), (TEXT.len(), Some(dt)));
                assert_eq!(nd.msg(), Some(m));
            }
        }
    }

    #[test]
    fn sends_with_retries_and_reports() {
        let mut p = PdNet::default();
        let nd = NetData::of_msg(&DataMsg::build(head(DPF_CONFIRMED), TEXT).unwrap()).unwrap();
        assert!(p.hold(&nd, 77, 0));
        assert!(p.next(1, 0).is_none(), "no channel yet");
        open(&mut p, 1, A, B);
        assert!(p.next(1, GRANT_LEAD_MS - 1).is_none(), "the MS comes to the channel first");
        let first = drain(&mut p, 1, GRANT_LEAD_MS);
        let n = nd.msg().unwrap().n as usize;
        assert_eq!(first.len(), 1 + n);
        assert!(hdr(&first[0]).full);
        // a SACK for block 0: F = 0 and that block
        let sack = DataHead { dpf: DPF_RESPONSE, sap: SAP_IP, src: B, dst: A, class: CLASS_SACK, blocks: 1, status: 3, ..Default::default() };
        let mut b = [0u8; BLOCK_MAX];
        b[..10].copy_from_slice(&sack.encode());
        p.rx(1, DT_DATA_HDR, &b, 500);
        b[..12].copy_from_slice(&rdata(!1 & 0xFFFF, n as u8));
        p.rx(1, DT_R12, &b, 500);
        let retry = drain(&mut p, 1, 500);
        assert_eq!(retry.len(), 2);
        assert_eq!((hdr(&retry[0]).full, hdr(&retry[0]).blocks), (false, 1));
        assert_eq!(retry[1].1, first[1].1);
        // no answer: sent whole again, up to TRIES, then no_response
        let mut t = 500;
        for _ in 0..TRIES {
            t += RESP_WAIT_MS;
            p.tick(t);
            drain(&mut p, 1, t);
        }
        assert_eq!(p.take_result(), Some((A, B, 77, RESULT_NO_RESPONSE, TRIES)));
        // an ACK after one send
        assert!(p.hold(&nd, 78, t));
        p.tick(t);
        drain(&mut p, 1, t);
        let ack = DataHead { class: CLASS_ACK, rtype: TYPE_ACK, blocks: 0, ..sack };
        b[..10].copy_from_slice(&ack.encode());
        p.rx(1, DT_DATA_HDR, &b, t);
        assert_eq!(p.take_result(), Some((A, B, 78, RESULT_DELIVERED, 1)));
        // unconfirmed: done once sent; failed when the call can't be made
        let u = NetData { head: head(DPF_UNCONFIRMED), ..nd };
        assert!(p.hold(&u, 79, t));
        drain(&mut p, 1, t);
        assert_eq!(p.take_result(), Some((A, B, 79, RESULT_UNCONFIRMED, 1)));
        assert!(p.hold(&NetData { head: DataHead { dst: 5, ..u.head }, ..u }, 80, t));
        let f = Event { r#type: EV_PD, op: PD_FAILED, kind: PARTY_NET, src: A, dst: 5, ..Default::default() };
        p.event(&f, t);
        assert_eq!(p.take_result(), Some((A, 5, 80, RESULT_FAILED, 0)));
        assert_eq!(p.take_result(), None);
    }
}
