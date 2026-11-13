// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! An MS simulator for the trunked site (docs/spec/fw/tier3.md section 11): a radio that knows one site and follows
//! ETSI TS 102 361-4 for the services the site supports. It hunts the TSCC, waits for C_ALOHA, makes slotted random
//! accesses with the site's backoff and NRand_Wait, answers C_AHOY, follows grants to the payload channel, talks,
//! and returns on P_CLEAR. Sans-IO like the site: the caller (t3air, or C) moves the blocks and the time.

use core::ffi::c_int;

use crate::t3pdu::*;
use crate::t3site::{
    DataGrant, DataHead, DataMsg, CARRIERS, DPF_CONFIRMED, DPF_RESPONSE, DPF_UNCONFIRMED, MSG_BLOCKS, SAP_IP, SO_SIMI,
    VOICE_FRAME, VOICE_HEADER, VOICE_TERM,
};
use crate::t3udt::*;

pub const ANSWER_ACCEPT: u8 = 0;
pub const ANSWER_BUSY: u8 = 1;
pub const ANSWER_REFUSE: u8 = 2;
pub const ANSWER_SILENT: u8 = 3;
pub const ANSWER_FOACSU: u8 = 4;

pub const DO_REGISTER: u8 = 1;
pub const DO_DEREGISTER: u8 = 2;
pub const DO_GROUP: u8 = 3;
pub const DO_PRIVATE: u8 = 4;
pub const DO_CANCEL: u8 = 5;
const DO_ANSWER: u8 = 6; // FOACSU Call Answer, queued by the MS itself
pub const DO_SDM: u8 = 7; // UDT short data (send_sdm): arg the MS or talkgroup
pub const DO_STATUS: u8 = 8; // a status (talk_ms: the value; 127 polls): arg the MS or talkgroup
pub const F_EMERGENCY: u8 = 0x01;
pub const F_GROUP: u8 = 0x02; // DO_SDM, DO_STATUS, DO_DATA: arg is a talkgroup
pub const DO_DATA: u8 = 9; // a packet data call (send_data): arg the MS, talkgroup or gateway
pub const F_CONFIRMED: u8 = 0x04; // DO_DATA: confirmed data
pub const F_MULTI: u8 = 0x08; // DO_DATA: multi-item (P_MAINT after the item)
pub const F_LIST: u8 = 0x10; // DO_REGISTER: with the talkgroup list set by register_list
pub const DO_DGNA: u8 = 10; // assign talkgroups to an MS (send_dgna): arg the MS

pub const IDLE: u8 = 0;
pub const ACCESS: u8 = 1;
pub const WAITING: u8 = 2;
pub const PAYLOAD: u8 = 3;

pub const TX_PDU: u8 = 1;
pub const TX_VOICE: u8 = 2;
pub const TX_DATA: u8 = 3; // packet data on a payload channel: dt 6 a header (pdu), dt 7 a rate 1/2 block (pdu, ext)

const ACTIONS: usize = 8;
const TRIES: u8 = 6; // random access attempts before giving up
const WAIT_MS: i64 = 30_000; // after C_WACKD / C_QACKD / MS_Alerting, the longest wait for the outcome
const HUNT_SLOTS: u32 = 100; // TSCC slots without C_ALOHA before trying the next carrier
const MAINT_REPEATS: u8 = 4;
const DATA_ACK_MS: i64 = 3000; // confirmed data: the longest wait for the response

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct MsTx {
    pub ch: u8,
    pub kind: u8,
    pub dt: u8,
    pub what: u8,
    pub pdu: [u8; 10],
    pub group: u8,
    pub pad: u8,
    pub src: u32,
    pub dst: u32,
    pub ext: [u8; 2], // dt 7 (rate 1/2 data): the block's octets 10 and 11
    pub pad2: [u8; 2],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct MsStatus {
    pub state: u8,
    pub registered: u8,
    pub last_reason: u8,
    pub ch: u8,
    pub grants: u32,
    pub calls_done: u32,
    pub calls_failed: u32,
    pub ahoys: u32,
    pub clears: u32,
    pub heard_src: u32,
    pub heard_dst: u32,
}

#[derive(Clone, Copy, Default, Debug)]
struct Action {
    what: u8,
    flags: u8,
    arg: u32,
    talk_ms: u32,
    at_ms: i64,
}

#[derive(Clone, Copy, Default)]
pub struct Ms {
    id: u32,
    tg: [u32; 8],
    ntg: usize,
    chans: [u16; CARRIERS],
    ncarriers: usize,
    rng: u32,
    answer: u8,
    answer_ms: u32,
    actions: [Action; ACTIONS],
    nact: usize,
    now: i64,
    // the site, from C_ALOHA
    tscc_ch: usize,
    have_aloha: bool,
    syscode: u16,
    nrand: u8,
    backoff: u8,
    no_aloha: u32,
    at: bool, // the AT bit for the next TSCC inbound slot
    // what it does
    state: u8,
    ch: usize,
    registered: bool,
    cur: Action,
    has_cur: bool,
    pdu: [u8; 10],
    tries: u8,
    wait: u32,      // TSCC inbound slots before the next attempt
    resp_wait: u32, // TSCC slots left to wait for an answer
    wait_until: i64,
    reply: Option<[u8; 10]>,
    alert_caller: u32,
    // the payload channel
    mine: bool, // the call is this MS's request (it talks)
    talking: bool,
    header_sent: bool,
    talk_end: i64,
    maint_left: u8,
    pay_group: bool,
    pay_src: u32,
    pay_dst: u32,
    // counters
    last_reason: u8,
    grants: u32,
    calls_done: u32,
    calls_failed: u32,
    ahoys: u32,
    clears: u32,
    // short data
    sd_out: Blocks,
    sd_fmt: u8,
    up: [(u8, [u8; 12]); 1 + MAX_BLOCKS],
    up_len: usize,
    up_pos: usize,
    in_head: UdtHead,
    in_on: bool,
    in_got: u8,
    in_blocks: Blocks,
    inbox: Blocks,
    inbox_head: UdtHead,
    inbox_n: u32,
    status_reply: u8,
    last_status: u8,
    last_status_src: u32,
    statuses: u32,
    poll_result: u8,
    // packet data
    pd_out: DataMsg,
    pd_pos: u8, // 0: the header next, then the blocks
    pd_sending: bool,
    pd_wait_ack: bool,
    pd_until: i64,
    pd_multi: bool, // the channel's grant is multi-item
    pd_in: DataMsg,
    pd_in_on: bool,
    pd_in_got: u8,
    pd_inbox: DataMsg,
    pd_inbox_n: u32,
    pd_resp: Option<[u8; 10]>,
    pd_leave: bool, // single item: back to the TSCC after the response goes
    // talkgroup lists
    reg_list: Blocks, // the subscription list a DO_REGISTER with F_LIST uploads
    reg_info: u8,     // the Response_Info of the last registration's C_ACK
    dgna: [u32; 16],  // the talkgroups DGNA gave it (fifteen by address, the sixteenth by alias)
    dgna_n: u32,
}

impl Ms {
    pub fn new(id: u32, tg: &[u32], chans: &[u16], seed: u32) -> Ms {
        let mut m = Ms { id, rng: seed | 1, ..Default::default() };
        m.ntg = tg.len().min(8);
        m.tg[..m.ntg].copy_from_slice(&tg[..m.ntg]);
        m.ncarriers = chans.len().clamp(1, CARRIERS);
        m.chans[..m.ncarriers].copy_from_slice(&chans[..m.ncarriers]);
        m
    }

    pub fn id(&self) -> u32 {
        self.id
    }

    /// The channel it listens to (and transmits on).
    pub fn ch(&self) -> usize {
        self.ch
    }

    pub fn state(&self) -> u8 {
        self.state
    }

    pub fn set_answer(&mut self, answer: u8, answer_ms: u32) {
        self.answer = answer;
        self.answer_ms = answer_ms;
    }

    fn rand(&mut self) -> u32 {
        // xorshift32
        let mut x = self.rng;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        self.rng = x;
        x
    }

    pub fn act(&mut self, what: u8, arg: u32, talk_ms: u32, flags: u8, at_ms: i64) -> bool {
        if self.nact == ACTIONS {
            return false;
        }
        self.actions[self.nact] = Action { what, flags, arg, talk_ms, at_ms };
        self.nact += 1;
        true
    }

    fn act_front(&mut self, a: Action) {
        if self.nact == ACTIONS {
            self.nact -= 1;
        }
        for i in (0..self.nact).rev() {
            self.actions[i + 1] = self.actions[i];
        }
        self.actions[0] = a;
        self.nact += 1;
    }

    /// Queues UDT short data (up to 46 octets) to dst, a talkgroup with F_GROUP; false when it doesn't fit.
    pub fn send_sdm(&mut self, dst: u32, flags: u8, format: u8, data: &[u8], at_ms: i64) -> bool {
        let Some(b) = Blocks::of(data, 0) else { return false };
        self.sd_out = b;
        self.sd_fmt = format;
        self.act(DO_SDM, dst, 0, flags, at_ms)
    }

    /// Queues a packet data call with one message (unconfirmed, or confirmed with F_CONFIRMED; SAP 4) to dst;
    /// false when the data doesn't fit.
    pub fn send_data(&mut self, dst: u32, flags: u8, data: &[u8], at_ms: i64) -> bool {
        let head = DataHead {
            dpf: if flags & F_CONFIRMED != 0 { DPF_CONFIRMED } else { DPF_UNCONFIRMED },
            group: flags & F_GROUP != 0,
            response: flags & F_CONFIRMED != 0,
            sap: SAP_IP,
            dst,
            src: self.id,
            ..Default::default()
        };
        let Some(m) = DataMsg::build(head, data) else { return false };
        self.pd_out = m;
        self.act(DO_DATA, dst, 0, flags, at_ms)
    }

    /// The last packet data message it took: the header, the data and its length, and how many it took.
    pub fn data_inbox(&self) -> Option<(DataHead, [u8; 12 * MSG_BLOCKS], usize, u32)> {
        if self.pd_inbox_n == 0 {
            return None;
        }
        let mut d = [0u8; 12 * MSG_BLOCKS];
        let n = self.pd_inbox.data(&mut d)?;
        Some((self.pd_inbox.head, d, n, self.pd_inbox_n))
    }

    /// Queues a registration with a talkgroup subscription/attachment list (6.4.4.1.13.2: 2 to 7 talkgroups,
    /// ADRNULL removes); false when it doesn't fit.
    pub fn register_list(&mut self, tgs: &[u32], at_ms: i64) -> bool {
        if tgs.len() > 7 {
            return false;
        }
        let Some(b) = AddrList::blocks(false, tgs) else { return false };
        self.reg_list = b;
        self.act(DO_REGISTER, 0, 0, F_LIST, at_ms)
    }

    /// The Response_Info of the last registration's acknowledgement (a list's: one bit per talkgroup).
    pub fn reg_info(&self) -> u8 {
        self.reg_info
    }

    /// Queues a DGNA transfer to dst (6.6.8): the Address Format (format FMT_ADDRESS, up to 15 talkgroups, ok the
    /// One-key flag) or the Mixed Format (FMT_MIXED: data the appended data as is); false when it doesn't fit.
    pub fn send_dgna(&mut self, dst: u32, format: u8, tgs: &[u32], ok: bool, data: &[u8], at_ms: i64) -> bool {
        let b = if format == FMT_ADDRESS { AddrList::blocks(ok, tgs) } else { Blocks::of(data, 0) };
        let Some(b) = b else { return false };
        self.sd_out = b;
        self.sd_fmt = format;
        self.act(DO_DGNA, dst, 0, 0, at_ms)
    }

    /// The talkgroups DGNA gave it, and how many transfers it took.
    pub fn dgna_tgs(&self) -> ([u32; 16], u32) {
        (self.dgna, self.dgna_n)
    }

    /// The status it answers a poll with.
    pub fn set_status(&mut self, status: u8) {
        self.status_reply = status;
    }

    /// The last UDT short data it took: the header and the data (the count of them in MsStatus-free form).
    pub fn inbox(&self) -> Option<(UdtHead, [u8; MAX_DATA], u32)> {
        if self.inbox_n == 0 {
            return None;
        }
        let mut d = [0u8; MAX_DATA];
        self.inbox.data(&mut d);
        Some((self.inbox_head, d, self.inbox_n))
    }

    /// The last status it was sent (source, value, count), and the status a poll of its own got back.
    pub fn statuses(&self) -> (u32, u8, u32, u8) {
        (self.last_status_src, self.last_status, self.statuses, self.poll_result)
    }

    fn take_due(&mut self, pred: impl Fn(&Action) -> bool) -> Option<Action> {
        let now = self.now;
        let i = (0..self.nact).find(|&i| self.actions[i].at_ms <= now && pred(&self.actions[i]))?;
        let a = self.actions[i];
        for j in i..self.nact - 1 {
            self.actions[j] = self.actions[j + 1];
        }
        self.nact -= 1;
        Some(a)
    }

    fn my_tg(&self, tg: u32) -> bool {
        self.tg[..self.ntg].contains(&tg) || (tg != 0 && self.dgna.contains(&tg))
    }

    fn chan_index(&self, chan: u16, lcn: u8) -> Option<usize> {
        (0..self.ncarriers).find(|&i| self.chans[i] == chan).map(|i| i * 2 + (lcn as usize & 1))
    }

    fn request_pdu(&self, a: &Action) -> [u8; 10] {
        let emergency = if a.flags & F_EMERGENCY != 0 { SO_EMERGENCY } else { 0 };
        let r = match a.what {
            // 6.4.4.1.13.2: a list, Trgt_Adr_Cnts 10b to TATTSI with the blocks it needs
            DO_REGISTER if a.flags & F_LIST != 0 => Rand {
                service_options: SO_REG,
                dep: 2 << 2 | (self.reg_list.n.max(1) - 1),
                service_kind: SK_REG,
                target: TATTSI,
                source: self.id,
                ..Default::default()
            },
            DO_REGISTER => Rand {
                service_options: SO_REG,
                dep: if a.arg != 0 { 1 << 2 } else { 0 },
                service_kind: SK_REG,
                target: if a.arg != 0 { a.arg } else { self.syscode as u32 },
                source: self.id,
                ..Default::default()
            },
            DO_DEREGISTER => Rand {
                service_options: 0,
                service_kind: SK_REG,
                target: self.syscode as u32,
                source: self.id,
                ..Default::default()
            },
            DO_GROUP => Rand {
                service_options: emergency,
                service_kind: SK_TG_VOICE,
                target: a.arg,
                source: self.id,
                ..Default::default()
            },
            DO_PRIVATE => Rand {
                service_options: emergency,
                service_kind: SK_IND_VOICE,
                target: a.arg,
                source: self.id,
                ..Default::default()
            },
            DO_ANSWER => Rand { service_kind: SK_CALL_ANSWER, target: a.arg, source: self.id, ..Default::default() },
            // 6.6.4.2 table 6.55: Appended_UDT_Short_Data the blocks less one
            DO_SDM => Rand {
                dep: self.sd_out.n.max(1) - 1,
                service_kind: if a.flags & F_GROUP != 0 { SK_TG_UDT } else { SK_IND_UDT },
                target: a.arg,
                source: self.id,
                ..Default::default()
            },
            // 6.6.6.1.2 table 6.57: G/I, then the status's five MSBs; its two LSBs in the dependent bits
            DO_STATUS => {
                let st = (a.talk_ms & 0x7F) as u8;
                Rand {
                    service_options: ((a.flags & F_GROUP != 0) as u8) << 6 | st >> 2,
                    dep: st & 3,
                    service_kind: SK_STATUS,
                    target: a.arg,
                    source: self.id,
                    ..Default::default()
                }
            }
            // 6.6.8.3 table 6.70: to DGNAI, SUPED_VAL the blocks less one
            DO_DGNA => Rand {
                dep: (self.sd_out.n.max(1) - 1) << 2,
                service_kind: SK_SUPPL,
                target: DGNAI,
                source: self.id,
                ..Default::default()
            },
            // 6.6.3.2.1 table 6.50
            DO_DATA => Rand {
                service_options: emergency | if a.flags & F_MULTI != 0 { SO_SIMI } else { 0 },
                service_kind: if a.flags & F_GROUP != 0 { SK_TG_PD } else { SK_IND_PD },
                target: a.arg,
                source: self.id,
                ..Default::default()
            },
            _ => Rand { service_kind: SK_CANCEL, target: a.arg, source: self.id, ..Default::default() },
        };
        r.encode()
    }

    fn start(&mut self, a: Action) {
        if self.has_cur && a.what == DO_CANCEL {
            self.calls_failed += 1; // the call it cancels
        }
        self.cur = a;
        self.has_cur = true;
        self.up_len = 0;
        self.up_pos = 0;
        self.pdu = self.request_pdu(&a);
        self.tries = 0;
        self.wait = 1;
        self.resp_wait = 0;
        self.state = ACCESS;
    }

    fn done(&mut self, ok: bool) {
        if self.has_cur && matches!(self.cur.what, DO_GROUP | DO_PRIVATE | DO_SDM | DO_STATUS | DO_DATA | DO_DGNA) {
            if ok {
                self.calls_done += 1;
            } else {
                self.calls_failed += 1;
            }
        }
        self.has_cur = false;
        self.state = IDLE;
        self.ch = self.tscc_ch;
    }

    fn back_to_tscc(&mut self) {
        self.state = IDLE;
        self.ch = self.tscc_ch;
        self.talking = false;
        self.maint_left = 0;
        self.pd_sending = false;
        self.pd_wait_ack = false;
        self.pd_in_on = false;
        self.pd_resp = None;
        self.pd_leave = false;
    }

    /// Outbound slot k on channel ch (a block when dt isn't 0); at: the CACH's AT bit for the next slot.
    pub fn rx(&mut self, ch: usize, k: i64, dt: u8, pdu: &[u8; 10], at: bool) {
        let mut b = [0u8; 12];
        b[..10].copy_from_slice(pdu);
        self.rx_block(ch, k, dt, &b, at);
    }

    /// rx with a whole block (12 octets: a rate 1/2 block's; a PDU's first 10).
    pub fn rx_block(&mut self, ch: usize, k: i64, dt: u8, blk: &[u8; 12], at: bool) {
        let mut p = [0u8; 10];
        p.copy_from_slice(&blk[..10]);
        let pdu = &p;
        if ch != self.ch {
            return;
        }
        if self.state == PAYLOAD {
            if dt == DT_DATA_HDR || dt == DT_RATE12 {
                self.rx_data(ch, dt, blk);
                return;
            }
            if dt == DT_CSBK && Clear::decode(pdu).is_some() {
                self.clears += 1;
                let mine = self.mine && self.has_cur;
                self.mine = false;
                if mine {
                    let ok = !(self.cur.what == DO_DATA && (self.pd_sending || self.pd_wait_ack));
                    self.done(ok);
                }
                self.back_to_tscc();
            }
            return;
        }
        if k & 1 != (self.tscc_ch & 1) as i64 {
            return;
        }
        self.at = at;
        match (dt == DT_CSBK).then(|| Aloha::decode(pdu)).flatten() {
            Some(a) => {
                self.have_aloha = true;
                self.no_aloha = 0;
                self.syscode = a.syscode;
                self.nrand = a.nrand_wait;
                self.backoff = a.backoff;
            }
            None => self.no_aloha += 1,
        }
        if self.no_aloha > HUNT_SLOTS && self.state == IDLE {
            // no TSCC here: the next carrier
            let c = (self.tscc_ch / 2 + 1) % self.ncarriers;
            self.tscc_ch = c * 2;
            self.ch = self.tscc_ch;
            self.no_aloha = 0;
            self.have_aloha = false;
            return;
        }
        if dt == DT_DATA_HDR || dt == DT_RATE12 {
            self.on_udt(dt, blk);
            return;
        }
        if dt != DT_CSBK {
            return;
        }
        if let Some(a) = Ack::decode(pdu) {
            if a.op == OP_ACKD && a.target == self.id {
                self.on_ack(&a);
            }
        } else if let Some(a) = Ahoy::decode(pdu) {
            if a.target == self.id || (a.group && a.service_kind == SK_STATUS && self.my_tg(a.target)) {
                self.on_ahoy(&a);
            }
        } else if let Some(g) = Grant::decode(pdu) {
            self.on_grant(&g);
        } else if let Some(g) = DataGrant::decode(pdu) {
            self.on_data_grant(&g);
        }
    }

    /// A data channel grant (6.6.3.2.7): its own call, a call to it, or (idle) one to its talkgroup.
    fn on_data_grant(&mut self, g: &DataGrant) {
        let asked = self.has_cur && matches!(self.state, ACCESS | WAITING);
        let mine = asked && self.cur.what == DO_DATA && g.source == self.id && g.target == self.cur.arg;
        let to_me = if g.group() { self.my_tg(g.target) } else { g.target == self.id };
        let follow = mine || ((self.state == IDLE || !self.has_cur) && to_me);
        if !follow || (self.state == PAYLOAD && !mine) {
            return;
        }
        let Some(ch) = self.chan_index(g.chan, g.lcn) else { return };
        self.grants += 1;
        self.state = PAYLOAD;
        self.ch = ch;
        self.mine = mine;
        self.talking = false;
        self.maint_left = 0;
        self.pd_sending = mine;
        self.pd_pos = 0;
        self.pd_wait_ack = false;
        self.pd_in_on = false;
        self.pd_resp = None;
        self.pd_leave = false;
        self.pd_multi = matches!(g.op, crate::t3site::OP_PD_GRANT_MI | crate::t3site::OP_TD_GRANT_MI);
        self.pay_group = g.group();
        self.pay_src = g.source;
        self.pay_dst = g.target;
    }

    /// A data header or rate 1/2 block on its payload channel (the site's repeat): its own call's response, or a
    /// message for it or its talkgroup (answered when confirmed: 361-1 9.2.3).
    pub fn rx_data(&mut self, ch: usize, dt: u8, blk: &[u8; 12]) {
        if self.state != PAYLOAD || ch != self.ch {
            return;
        }
        if dt == DT_DATA_HDR {
            let mut p = [0u8; 10];
            p.copy_from_slice(&blk[..10]);
            self.pd_in_on = false;
            let Some(h) = DataHead::decode(&p) else { return };
            if h.src == self.id {
                return; // its own, repeated
            }
            if h.dpf == DPF_RESPONSE {
                if h.dst == self.id && self.pd_wait_ack && (h.class, h.rtype) == (0, 1) {
                    self.pd_wait_ack = false;
                    self.pay_src = h.src;
                    self.item_done();
                }
                return;
            }
            if h.dst == self.id || (h.group && self.my_tg(h.dst)) {
                self.pd_in = DataMsg { head: h, n: h.blocks.min(MSG_BLOCKS as u8), ..Default::default() };
                self.pd_in_got = 0;
                self.pd_in_on = h.blocks > 0 && h.blocks as usize <= MSG_BLOCKS;
                self.pay_src = h.src;
            }
            return;
        }
        if !self.pd_in_on {
            return;
        }
        self.pd_in.b[self.pd_in_got as usize] = *blk;
        self.pd_in_got += 1;
        if self.pd_in_got < self.pd_in.n {
            return;
        }
        self.pd_in_on = false;
        let mut d = [0u8; 12 * MSG_BLOCKS];
        if self.pd_in.data(&mut d).is_none() {
            return; // confirmed: no response, the sender tries again
        }
        let h = self.pd_in.head;
        if !(self.pd_inbox_n > 0 && self.pd_inbox == self.pd_in) {
            self.pd_inbox = self.pd_in;
            self.pd_inbox_n += 1;
        }
        if h.dpf == DPF_CONFIRMED && h.response && !h.group {
            let (dpf, sap, dst, src) = (DPF_RESPONSE, h.sap, h.src, self.id);
            let r = DataHead { dpf, sap, dst, src, rtype: 1, status: h.ns, ..Default::default() };
            self.pd_resp = Some(r.encode());
            self.pd_leave = !self.pd_multi && !self.mine;
        } else if !self.pd_multi && !self.mine {
            self.back_to_tscc(); // 6.6.3.3.2.5 b): a single item taken
        }
    }

    /// Its message went (unconfirmed) or was acknowledged: P_MAINT after a multi-item call, else back to the TSCC
    /// (6.6.3.3.2.5 a, c).
    fn item_done(&mut self) {
        if self.pd_multi {
            self.maint_left = MAINT_REPEATS;
            return;
        }
        self.mine = false;
        self.done(true);
        self.back_to_tscc();
    }

    /// A UDT header or appended block on the TSCC (6.6.4.7): data for it or its talkgroup is taken, and an
    /// individual one with A set answered.
    fn on_udt(&mut self, dt: u8, blk: &[u8; 12]) {
        if dt == DT_DATA_HDR {
            let mut p = [0u8; 10];
            p.copy_from_slice(&blk[..10]);
            self.in_on = false;
            if let Some(h) = UdtHead::decode(&p) {
                let dgna = h.op == OP_DGNAHD && h.target == self.id && !h.group;
                if dgna || (h.op == OP_UDTHD && (h.target == self.id || (h.group && self.my_tg(h.target)))) {
                    self.in_on = true;
                    self.in_head = h;
                    self.in_got = 0;
                    self.in_blocks = Blocks { n: h.blocks, ..Default::default() };
                }
            }
            return;
        }
        if !self.in_on || self.in_got >= self.in_blocks.n {
            return;
        }
        self.in_blocks.b[self.in_got as usize] = *blk;
        self.in_got += 1;
        if self.in_got < self.in_blocks.n {
            return;
        }
        self.in_on = false;
        if !self.in_blocks.crc_ok() {
            return; // no answer: the TSCC sends it again
        }
        let h = self.in_head;
        if !h.group && h.response {
            let reason = match self.answer {
                ANSWER_SILENT => return,
                ANSWER_BUSY => R_EQUIP_BUSY,
                ANSWER_REFUSE => R_RECIPIENT_REFUSED,
                _ => R_MS_ACCEPTED,
            };
            let a = Ack { op: OP_ACKU, response_info: 0, reason, target: h.source, source: self.id };
            self.reply = Some(a.encode());
            if reason != R_MS_ACCEPTED {
                return;
            }
        }
        if h.op == OP_DGNAHD {
            // 6.6.8.1: the fifteen replaced, or (alias) the sixteenth set unless it's one of them
            let mut d = [0u8; MAX_DATA];
            let n = self.in_blocks.data(&mut d);
            let l = AddrList::of(&d[..n]);
            if h.format == FMT_ADDRESS {
                self.dgna = [0; 16];
                self.dgna[..l.n.min(15)].copy_from_slice(&l.addrs()[..l.n.min(15)]);
            } else if l.a[0] == ADRNULL || !self.dgna[..15].contains(&l.a[0]) {
                self.dgna[15] = l.a[0];
            }
            self.dgna_n += 1;
            return;
        }
        if self.inbox_n > 0 && self.inbox_head == h && self.inbox == self.in_blocks {
            return; // a repeat of the same message
        }
        self.inbox = self.in_blocks;
        self.inbox_head = h;
        self.inbox_n += 1;
    }

    fn on_ack(&mut self, a: &Ack) {
        self.last_reason = a.reason;
        if !self.has_cur {
            return; // e.g. the called party told of its own refusal
        }
        if matches!(self.cur.what, DO_SDM | DO_STATUS) && a.source != self.cur.arg {
            return; // not about this request (a repeat of an earlier one's final acknowledgement)
        }
        if self.cur.what == DO_DGNA && a.source != self.cur.arg && a.source != DGNAI {
            return;
        }
        if matches!(self.cur.what, DO_SDM | DO_DGNA) && reason_type(a.reason) == 1 && self.up_len == 0 {
            return; // a final answer before its upload: a repeat of the last one's
        }
        if a.reason == R_POLL_MS && self.cur.what == DO_STATUS {
            self.poll_result = a.response_info;
        }
        match reason_type(a.reason) {
            2 | 3 => {
                self.state = WAITING;
                self.wait_until = self.now + WAIT_MS;
            }
            1 => match a.reason {
                R_MS_ALERTING => {
                    self.state = WAITING;
                    self.wait_until = self.now + WAIT_MS;
                }
                R_REG_ACCEPTED | R_REG_SUBSCRIPTION => {
                    self.reg_info = a.response_info;
                    self.registered = self.cur.what == DO_REGISTER;
                    self.done(true);
                }
                R_CALLBACK => self.done(false),
                _ => self.done(true),
            },
            _ => {
                if a.reason == R_MS_NOT_REGISTERED || a.reason == R_REG_DENIED || a.reason == R_REG_REFUSED {
                    self.registered = false;
                }
                self.done(false);
            }
        }
    }

    fn on_ahoy(&mut self, a: &Ahoy) {
        if a.service_kind == SK_CANCEL {
            self.alert_caller = 0;
            let caller = a.source;
            let n = self.nact;
            for i in (0..n).rev() {
                if self.actions[i].what == DO_ANSWER && self.actions[i].arg == caller {
                    for j in i..self.nact - 1 {
                        self.actions[j] = self.actions[j + 1];
                    }
                    self.nact -= 1;
                }
            }
            return;
        }
        // an upload asked for: short data (6.6.4.4 c, table 6.35), DGNA (6.6.8.3.3, table 6.71) or a
        // registration's talkgroup list (6.4.4.1.13.2: to TATTSI, A = 0, SF = 1, the Address Format)
        let upload = match (a.source, a.service_kind) {
            (SDMI, SK_IND_UDT | SK_TG_UDT) => Some(DO_SDM),
            (DGNAI, SK_SUPPL) => Some(DO_DGNA),
            (TATTSI, SK_REG) if a.service_kind_flag => Some(DO_REGISTER),
            _ => None,
        };
        if let Some(what) = upload {
            let mine = self.has_cur && self.cur.what == what && matches!(self.state, ACCESS | WAITING);
            if !mine || (what == DO_REGISTER && self.cur.flags & F_LIST == 0) {
                let r = Ack {
                    op: OP_ACKU,
                    reason: R_MS_NOT_SUPPORTED,
                    target: a.source,
                    source: self.id,
                    ..Default::default()
                };
                self.reply = Some(r.encode());
                return;
            }
            let (h, b) = match what {
                DO_REGISTER => {
                    let h = UdtHead {
                        op: OP_UDTHU,
                        format: FMT_ADDRESS,
                        target: TATTSI,
                        source: self.id,
                        blocks: self.reg_list.n,
                        sf: true,
                        ..Default::default()
                    };
                    (h, self.reg_list)
                }
                _ => {
                    let h = UdtHead {
                        op: if what == DO_DGNA { OP_DGNAHU } else { OP_UDTHU },
                        group: self.cur.flags & F_GROUP != 0,
                        response: what == DO_DGNA,
                        format: self.sd_fmt,
                        target: self.cur.arg,
                        source: self.id,
                        blocks: self.sd_out.n,
                        ..Default::default()
                    };
                    (h, self.sd_out)
                }
            };
            let mut hb = [0u8; 12];
            hb[..10].copy_from_slice(&h.encode());
            self.up[0] = (DT_DATA_HDR, hb);
            for j in 0..b.n as usize {
                self.up[1 + j] = (DT_RATE12, b.b[j]);
            }
            self.up_len = 1 + b.n as usize;
            self.up_pos = 0;
            self.state = WAITING;
            self.wait_until = self.now + WAIT_MS;
            return;
        }
        self.ahoys += 1;
        let ack = |reason, s: &Ms| Ack { op: OP_ACKU, response_info: 0, reason, target: a.source, source: s.id }.encode();
        // a packet data call's availability check is answered Message_Accepted (6.6.3.2.6)
        let accepted = if a.service_kind == SK_IND_PD { R_MESSAGE_ACCEPTED } else { R_MS_ACCEPTED };
        if a.service_kind == SK_STATUS {
            // 6.6.6.1.2.1.5, 6.6.6.2.2.1.5: Service_Options_Mirror the five MSBs, Appended_Blocks the two LSBs
            let st = (a.service_options & 0x1F) << 2 | a.appended;
            if st == STATUS_POLL {
                let info = self.status_reply;
                let r = Ack { op: OP_ACKU, response_info: info, reason: R_POLL_MS, target: a.source, source: self.id };
                self.reply = Some(r.encode());
                return;
            }
            self.last_status = st;
            self.last_status_src = a.source;
            self.statuses += 1;
            if a.group {
                return; // a talkgroup's: no answer (6.6.6.1.2.2.5)
            }
        }
        let status = matches!(a.service_kind, SK_STATUS | SK_IND_PD);
        let answer = if status && self.answer == ANSWER_FOACSU { ANSWER_ACCEPT } else { self.answer };
        self.reply = match answer {
            ANSWER_ACCEPT => Some(ack(accepted, self)),
            ANSWER_BUSY => Some(ack(R_EQUIP_BUSY, self)),
            ANSWER_REFUSE => Some(ack(R_RECIPIENT_REFUSED, self)),
            ANSWER_FOACSU => {
                if self.alert_caller != a.source {
                    self.alert_caller = a.source;
                    let at_ms = self.now + self.answer_ms as i64;
                    self.act_front(Action { what: DO_ANSWER, arg: a.source, at_ms, ..Default::default() });
                }
                Some(ack(R_MS_ALERTING, self))
            }
            _ => None,
        };
    }

    fn on_grant(&mut self, g: &Grant) {
        let asked = self.has_cur && matches!(self.state, ACCESS | WAITING);
        let (mine, follow) = match g.op {
            OP_PV_GRANT => {
                let mine = asked && self.cur.what == DO_PRIVATE && g.source == self.id && g.target == self.cur.arg;
                (mine, mine || g.target == self.id)
            }
            OP_TV_GRANT => {
                let mine = asked && self.cur.what == DO_GROUP && g.target == self.cur.arg;
                (mine, mine || (self.state == IDLE && self.my_tg(g.target)))
            }
            OP_BTV_GRANT => {
                let mine = asked && self.cur.what == DO_GROUP && g.target == self.cur.arg;
                (mine, mine || self.state == IDLE)
            }
            _ => (false, false),
        };
        if !follow {
            return;
        }
        let Some(ch) = self.chan_index(g.chan, g.lcn) else { return };
        let answered = g.op == OP_PV_GRANT && self.has_cur && self.cur.what == DO_ANSWER && g.target == self.id;
        if answered {
            self.has_cur = false; // the grant answers its Call Answer
        } else if !mine && self.has_cur && self.state != IDLE {
            return; // busy with its own request
        }
        if g.op == OP_PV_GRANT && !mine {
            self.alert_caller = 0;
        }
        self.grants += 1;
        self.state = PAYLOAD;
        self.ch = ch;
        self.mine = mine;
        self.talking = mine;
        self.header_sent = false;
        self.maint_left = 0;
        self.pay_group = g.op != OP_PV_GRANT;
        self.pay_src = g.source;
        self.pay_dst = g.target;
    }

    /// A voice burst heard on its payload channel (from the air's repeat of another MS).
    pub fn hear(&mut self, ch: usize, src: u32, dst: u32) {
        if self.state == PAYLOAD && ch == self.ch && src != self.id {
            self.pay_src = src;
            self.pay_dst = dst;
        }
    }

    /// Inbound slot k: Some when it transmits.
    pub fn tx(&mut self, k: i64, now_ms: i64) -> Option<MsTx> {
        self.now = now_ms;
        if self.state == PAYLOAD {
            return self.tx_payload(k);
        }
        if k & 1 != (self.tscc_ch & 1) as i64 {
            return None;
        }
        let pdu_tx = |pdu: [u8; 10], s: &Ms| MsTx { ch: s.ch as u8, kind: TX_PDU, dt: DT_CSBK, pdu, ..Default::default() };
        if self.up_pos < self.up_len {
            // the UDT upload: header and blocks in consecutive inbound slots (6.5.0)
            let (dt, b) = self.up[self.up_pos];
            self.up_pos += 1;
            let mut pdu = [0u8; 10];
            pdu.copy_from_slice(&b[..10]);
            return Some(MsTx { ch: self.ch as u8, kind: TX_PDU, dt, pdu, ext: [b[10], b[11]], ..Default::default() });
        }
        if let Some(r) = self.reply.take() {
            return Some(pdu_tx(r, self)); // in the slot the AT bit kept for it
        }
        if self.state == WAITING && now_ms >= self.wait_until {
            self.last_reason = 0;
            self.done(false);
        }
        if matches!(self.state, ACCESS | WAITING) {
            if let Some(a) = self.take_due(|a| a.what == DO_CANCEL) {
                self.start(a);
            }
        }
        if self.state == IDLE && self.have_aloha {
            if let Some(a) = self.take_due(|_| true) {
                self.start(a);
            }
        }
        if self.state != ACCESS {
            return None;
        }
        if self.resp_wait > 0 {
            self.resp_wait -= 1;
            if self.resp_wait > 0 {
                return None;
            }
            // no answer: back off and try again
            self.tries += 1;
            if self.tries >= TRIES {
                self.last_reason = 0;
                self.done(false);
                return None;
            }
            let n = backoff_frames(self.backoff).max(1);
            self.wait = 1 + self.rand() % n;
        }
        if self.wait > 1 {
            self.wait -= 1;
            return None;
        }
        if self.at {
            return None; // the slot is kept for a solicited answer
        }
        self.wait = 0;
        self.resp_wait = nwait_frames(self.nrand) + 2;
        Some(pdu_tx(self.pdu, self))
    }

    fn tx_payload(&mut self, k: i64) -> Option<MsTx> {
        if k & 1 != (self.ch & 1) as i64 {
            return None;
        }
        if let Some(t) = self.tx_data() {
            return Some(t);
        }
        let group = self.pay_group as u8;
        let dst = if self.mine { self.cur.arg } else { self.pay_dst };
        let voice = |what, s: &Ms| MsTx { ch: s.ch as u8, kind: TX_VOICE, what, group, src: s.id, dst, ..Default::default() };
        if self.talking {
            if !self.header_sent {
                self.header_sent = true;
                self.talk_end = self.now + self.cur.talk_ms as i64;
                return Some(voice(VOICE_HEADER, self));
            }
            if self.now >= self.talk_end {
                self.talking = false;
                if !self.pay_group {
                    self.maint_left = MAINT_REPEATS;
                }
                return Some(voice(VOICE_TERM, self));
            }
            return Some(voice(VOICE_FRAME, self));
        }
        if self.maint_left > 0 {
            self.maint_left -= 1;
            let m = Maint { kind: MAINT_DISCON, target: dst, source: self.id };
            return Some(MsTx { ch: self.ch as u8, kind: TX_PDU, dt: DT_CSBK, pdu: m.encode(), ..Default::default() });
        }
        None
    }

    /// Packet data on the payload channel: its message, a response it owes, the wait for its own response.
    fn tx_data(&mut self) -> Option<MsTx> {
        let data = |dt, b: &[u8; 12], s: &Ms| {
            let mut pdu = [0u8; 10];
            pdu.copy_from_slice(&b[..10]);
            let (src, dst, group) = (s.id, s.pd_out.head.dst, s.pay_group as u8);
            MsTx { ch: s.ch as u8, kind: TX_DATA, dt, pdu, ext: [b[10], b[11]], group, src, dst, ..Default::default() }
        };
        if let Some(r) = self.pd_resp.take() {
            let mut b = [0u8; 12];
            b[..10].copy_from_slice(&r);
            let mut t = data(DT_DATA_HDR, &b, self);
            t.dst = self.pay_src;
            t.group = 0;
            if self.pd_leave {
                self.back_to_tscc(); // 6.6.3.3.2.5 d): it answered a single item
            }
            return Some(t);
        }
        if self.pd_sending {
            if self.pd_pos == 0 {
                self.pd_pos = 1;
                let mut b = [0u8; 12];
                b[..10].copy_from_slice(&self.pd_out.head.encode());
                return Some(data(DT_DATA_HDR, &b, self));
            }
            let j = (self.pd_pos - 1) as usize;
            let b = self.pd_out.b[j];
            self.pd_pos += 1;
            if self.pd_pos as usize > self.pd_out.n as usize {
                self.pd_sending = false;
                if self.pd_out.head.dpf == DPF_CONFIRMED {
                    self.pd_wait_ack = true;
                    self.pd_until = self.now + DATA_ACK_MS;
                } else {
                    let t = data(DT_RATE12, &b, self);
                    self.item_done();
                    return Some(t);
                }
            }
            return Some(data(DT_RATE12, &b, self));
        }
        if self.pd_wait_ack && self.now >= self.pd_until {
            self.pd_wait_ack = false;
            self.mine = false;
            self.done(false);
            self.back_to_tscc();
        }
        None
    }

    pub fn status(&self) -> MsStatus {
        MsStatus {
            state: self.state,
            registered: self.registered as u8,
            last_reason: self.last_reason,
            ch: self.ch as u8,
            grants: self.grants,
            calls_done: self.calls_done,
            calls_failed: self.calls_failed,
            ahoys: self.ahoys,
            clears: self.clears,
            heard_src: self.pay_src,
            heard_dst: self.pay_dst,
        }
    }
}

// ── C API (rdof/t3.h) ──────────────────────────────────────────────────────────

#[no_mangle]
pub extern "C" fn rdof_t3ms_size() -> usize {
    core::mem::size_of::<Ms>()
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3ms_init(
    ms: *mut Ms,
    id: u32,
    tg: *const u32,
    ntg: c_int,
    chans: *const u16,
    ncarriers: c_int,
    seed: u32,
) {
    let ntg = (ntg.max(0) as usize).min(8);
    let tg = if ntg == 0 || tg.is_null() { &[][..] } else { core::slice::from_raw_parts(tg, ntg) };
    let nc = (ncarriers.max(1) as usize).min(CARRIERS);
    let chans = core::slice::from_raw_parts(chans, nc);
    ms.write(Ms::new(id, tg, chans, seed));
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3ms_answer(ms: *mut Ms, answer: u8, answer_ms: u32) {
    (*ms).set_answer(answer, answer_ms);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3ms_do(ms: *mut Ms, what: u8, arg: u32, talk_ms: u32, flags: u8, at_ms: i64) -> bool {
    matches!(what, DO_REGISTER..=DO_CANCEL | DO_STATUS) && (*ms).act(what, arg, talk_ms, flags, at_ms)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3ms_data_inbox(ms: *const Ms, out: *mut u8, cap: usize, src: *mut u32) -> c_int {
    let Some((h, d, n, _)) = (*ms).data_inbox() else { return -1 };
    if !src.is_null() {
        *src = h.src;
    }
    if !out.is_null() {
        core::ptr::copy_nonoverlapping(d.as_ptr(), out, n.min(cap));
    }
    n as c_int
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3ms_data(
    ms: *mut Ms,
    dst: u32,
    flags: u8,
    data: *const u8,
    len: usize,
    at_ms: i64,
) -> bool {
    let d = if data.is_null() || len == 0 { &[][..] } else { core::slice::from_raw_parts(data, len) };
    (*ms).send_data(dst, flags, d, at_ms)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3ms_sdm(
    ms: *mut Ms,
    dst: u32,
    flags: u8,
    format: u8,
    data: *const u8,
    len: usize,
    at_ms: i64,
) -> bool {
    let d = if data.is_null() || len == 0 { &[][..] } else { core::slice::from_raw_parts(data, len) };
    (*ms).send_sdm(dst, flags, format, d, at_ms)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3ms_rx(ms: *mut Ms, ch: c_int, k: i64, dt: u8, pdu: *const [u8; 10], at: bool) {
    if ch >= 0 {
        let mut b = [0u8; 12];
        if !pdu.is_null() {
            // a rate 1/2 block: 12 octets; a data header on a payload channel too
            let n = if dt == DT_RATE12 || (dt == DT_DATA_HDR && ch as usize != (*ms).tscc_ch) { 12 } else { 10 };
            core::ptr::copy_nonoverlapping(pdu.cast::<u8>(), b.as_mut_ptr(), n);
        }
        (*ms).rx_block(ch as usize, k, dt, &b, at);
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3ms_tx(ms: *mut Ms, k: i64, now_ms: i64, tx: *mut MsTx) -> bool {
    match (*ms).tx(k, now_ms) {
        Some(t) => {
            *tx = t;
            true
        }
        None => false,
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3ms_status(ms: *const Ms, st: *mut MsStatus) {
    *st = (*ms).status();
}
