// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Short data on the TSCC (docs/spec/fw/tier3.md 3.15): UDT short data messages (ETSI TS 102 361-4 6.6.4), status
//! delivery and status polling (6.6.6), store and forward, between the site's MSs, the site itself (TSI) and the
//! network. A part of the site controller (a child module of t3site, for its state); its own state is SdState.
//!
//! One transaction per message. An MS's UDT goes in two phases: the upload (C_AHOY from SDMI, the MS's C_UDTHU and
//! appended blocks in the inbound slots the TSCC keeps for it) and the download (C_UDTHD and the blocks, A = 1 to an
//! MS, which answers C_ACKU). A status goes in the C_AHOY itself. The final C_ACKD to the caller mirrors the called
//! MS's answer, or is Message_Accepted for a talkgroup or the site.

use super::*;
use crate::t3udt::*;

pub const SDS: usize = 8;

// what (rdof_t3_sd_t.what)
pub const SD_UDT: u8 = 1;
pub const SD_STATUS: u8 = 2;
pub const SD_POLL: u8 = 3;
/// Dynamic Group Numbering Assignment (6.6.8): value the UDT_Format (Address or Mixed), data the appended data
pub const SD_DGNA: u8 = 4;
/// A registration's talkgroup list upload (6.4.4.1.13.2): only from an MS here, never an SdMsg
pub(super) const SD_TGLIST: u8 = 5;
/// A call diversion's address upload (6.6.7.1): only from an MS
pub(super) const SD_DIVERT: u8 = 6;
/// The diverted address to a caller (6.6.7.2): a UDT header with SF 1 and no answer, sent FINAL_REPEATS times
pub(super) const SD_NOTICE: u8 = 7;

// op (rdof_t3_sd_t.op)
pub const SD_SEND: u8 = 1;
pub const SD_RESULT: u8 = 2;

// flags (rdof_t3_sd_t.flags)
pub const SD_F_GROUP: u8 = 0x01;
pub const SD_F_EMERGENCY: u8 = 0x02;

/// A message for the site or the network, or a result (rdof_t3_sd_t).
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SdMsg {
    pub op: u8,
    pub what: u8,
    pub reason: u8,
    pub value: u8,
    pub flags: u8,
    pub pad_nibble: u8,
    pub len: u8,
    pub why: u8,
    pub src: u32,
    pub dst: u32,
    pub stream: u32,
    pub r#ref: u32,
    pub data: [u8; 48],
}

impl Default for SdMsg {
    fn default() -> SdMsg {
        SdMsg {
            op: 0,
            what: 0,
            reason: 0,
            value: 0,
            flags: 0,
            pad_nibble: 0,
            len: 0,
            why: 0,
            src: 0,
            dst: 0,
            stream: 0,
            r#ref: 0,
            data: [0; 48],
        }
    }
}

impl SdMsg {
    pub fn data(&self) -> &[u8] {
        &self.data[..(self.len as usize).min(MAX_DATA)]
    }
    /// The message as an event (EV_SD or EV_NET_SD): op, kind what, reason, svc value, why, flags F_GROUP and
    /// F_EMERGENCY, lcn the pad nibble, chan the data length, text the data.
    pub fn event(&self, ty: u8) -> Event {
        let mut e = Event {
            r#type: ty,
            op: self.op,
            kind: self.what,
            reason: self.reason,
            svc: self.value,
            why: self.why,
            flags: if self.flags & SD_F_GROUP != 0 { F_GROUP } else { 0 }
                | if self.flags & SD_F_EMERGENCY != 0 { F_EMERGENCY } else { 0 },
            lcn: self.pad_nibble,
            chan: self.len as u16,
            src: self.src,
            dst: self.dst,
            stream: self.stream,
            r#ref: self.r#ref,
            ..Default::default()
        };
        let d = self.data();
        e.text[..d.len()].copy_from_slice(d);
        e
    }
    /// The message an EV_SD / EV_NET_SD event carries.
    pub fn of_event(e: &Event) -> SdMsg {
        let mut m = SdMsg {
            op: e.op,
            what: e.kind,
            reason: e.reason,
            value: e.svc,
            why: e.why,
            flags: if e.flags & F_GROUP != 0 { SD_F_GROUP } else { 0 }
                | if e.flags & F_EMERGENCY != 0 { SD_F_EMERGENCY } else { 0 },
            pad_nibble: e.lcn,
            len: (e.chan as usize).min(MAX_DATA) as u8,
            src: e.src,
            dst: e.dst,
            stream: e.stream,
            r#ref: e.r#ref,
            ..Default::default()
        };
        let n = m.len as usize;
        m.data[..n].copy_from_slice(&e.text[..n]);
        m
    }
}

// transaction states
const S_UPLOAD: u8 = 1; // the caller is to send its UDT (C_AHOY from SDMI)
const S_HOLD: u8 = 2; // stored: the called MS is busy
const S_DOWN: u8 = 3; // to an MS here: the download or the status C_AHOY, its answer awaited
const S_GROUP: u8 = 4; // to a talkgroup: repeats left
const S_NET: u8 = 5; // sent to the network: its result awaited

// where it came from
const O_MS: u8 = 1; // an MS here: answered on the TSCC
const O_APP: u8 = 2; // the site's application: an EV_SD result
const O_NET: u8 = 3; // another site: an EV_NET_SD result

const ACK_EVERY_MS: i64 = 2000;
const FINAL_REPEATS: usize = 2; // the final acknowledgement is repeated for reliability (figures 6.57, 6.62)
const TRAIN: usize = 1 + MAX_BLOCKS;

#[derive(Clone, Copy, Default)]
struct Sd {
    used: bool,
    what: u8,
    state: u8,
    origin: u8,
    group: bool,
    emergency: bool,
    ahoy_due: bool, // S_UPLOAD: its C_AHOY is to go
    so: u8,         // the C_RAND's Service_Options (the upload C_AHOY's Service_Options_Mirror)
    value: u8,      // UDT_Format, or the status
    tries: u8,
    src: u32,
    dst: u32,
    r#ref: u32,  // the app's tag, or the network send's stream
    stream: u32, // ours on the network (S_NET)
    head: UdtHead,
    blocks: Blocks,
    deadline_ms: i64,
    next_ms: i64,
    next_ack_ms: i64,
}

#[derive(Clone, Copy, Default)]
struct Upload {
    active: bool,
    sd: usize,
    from: u32,
    have_head: bool,
    got: u8,
    head: UdtHead,
    blocks: Blocks,
    until_k: i64,
}

/// The short data part of the site's state (all zero: nothing in progress).
#[derive(Clone, Copy, Default)]
pub(super) struct SdState {
    sds: [Sd; SDS],
    // the blocks that go in consecutive TSCC slots: a UDT, or a C_AHOY that asks for one
    train_dt: [u8; TRAIN],
    train: [[u8; 12]; TRAIN],
    train_len: usize,
    train_pos: usize,
    train_at_last: bool,
    keep: u8, // TSCC slots to send C_ALOHA(mask 24, ADRNULL) and AT busy: the upload's inbound slots
    ext: [u8; 2],
    upload: Upload,
    pub(super) delivered: u32,
    pub(super) failed: u32,
}

fn first10(b: &[u8; 12]) -> [u8; 10] {
    let mut p = [0u8; 10];
    p.copy_from_slice(&b[..10]);
    p
}

fn pdu12(p: &[u8; 10]) -> [u8; 12] {
    let mut b = [0u8; 12];
    b[..10].copy_from_slice(p);
    b
}

impl Site {
    fn sd_free(&self) -> Option<usize> {
        self.sd.sds.iter().position(|d| !d.used)
    }

    /// On a payload channel (a private call, or talking in a group call), or in a call setup.
    fn sd_busy(&self, ms: u32) -> bool {
        self.ms_busy(ms) || self.chans.iter().any(|c| Self::live(c) && (c.src == ms || c.talker == ms))
    }

    /// The service options a status C_AHOY mirrors: G/I and the five MSBs of the status (table 6.57).
    fn status_ahoy(d: &Sd) -> [u8; 10] {
        Ahoy {
            service_options: (d.group as u8) << 6 | (d.value >> 2),
            service_kind_flag: false,
            als: false,
            group: d.group,
            appended: d.value & 3,
            service_kind: SK_STATUS,
            target: d.dst,
            source: d.src,
        }
        .encode()
    }

    // ── requests from MSs ──

    /// The checks of a short data or status request's caller: true when it may go on.
    fn sd_admit(&mut self, r: &Rand) -> bool {
        let ms = r.source;
        if !self.allowed(ms) {
            self.refuse(R_PERM_USER_REFUSED, ms, r.target);
            return false;
        }
        if self.reg_index(ms).is_none() {
            if self.reg_bit() {
                self.refuse(R_MS_NOT_REGISTERED, ms, r.target);
                return false;
            }
            match self.record_for(ms, HOW_CALL) {
                Some(i) => self.write_record(i),
                None => {
                    self.refuse(R_SYSBUSY, ms, r.target);
                    return false;
                }
            }
        }
        true
    }

    /// The target's checks: Some(group) when it may go on. TSI is the site itself; other gateways need the
    /// extended address upload, not built.
    fn sd_target(&mut self, ms: u32, dst: u32, group: bool, poll: bool) -> Option<bool> {
        if dst == TSI && !group && !poll {
            return Some(false);
        }
        if dst == ADRNULL || is_gateway(dst) || (!group && (dst == ms || is_allcall(dst))) || (group && poll) {
            self.refuse(R_NOT_SUPPORTED, ms, dst);
            return None;
        }
        if group && !is_allcall(dst) && !self.policy.tg.contains(dst) {
            self.refuse(R_GROUP_NOT_ALLOWED, ms, dst);
            return None;
        }
        if !group && self.reg_index(dst).is_none() && !self.connected {
            self.refuse(R_NOREG_MSAWAY, ms, dst);
            return None;
        }
        Some(group)
    }

    /// C_RAND for UDT short data (6.6.4.1.1), a status or a status poll (6.6.6.1.1, 6.6.6.2.1).
    pub(super) fn sd_rand(&mut self, r: &Rand) {
        let ms = r.source;
        let what = match r.service_kind {
            SK_IND_UDT | SK_TG_UDT => SD_UDT,
            SK_SUPPL => SD_DGNA,
            _ if rand_status(r) == STATUS_POLL => SD_POLL,
            _ => SD_STATUS,
        };
        // a repeat of a request in progress (the MS missed the answer): answer it again
        if let Some(i) = self.sd.sds.iter().position(|d| d.used && d.origin == O_MS && d.src == ms && d.dst == r.target)
        {
            let d = self.sd.sds[i];
            if d.state == S_UPLOAD && !(self.sd.upload.active && self.sd.upload.sd == i) {
                self.sd.sds[i].ahoy_due = true;
            } else if d.state == S_HOLD {
                self.ack(R_QUEUED_BUSY, ms, d.dst);
            } else {
                self.ack(R_WAIT, ms, d.dst);
            }
            return;
        }
        if !self.sd_admit(r) {
            return;
        }
        if what == SD_DGNA {
            // 6.6.8.2.1: the called MS comes with the upload; DGNA from MSs only when the site allows it
            if self.cfg.dgna_ms == 0 {
                self.refuse(R_NOT_SUPPORTED, ms, DGNAI);
                return;
            }
            let blocks = (r.dep >> 2) + 1; // SUPED_VAL (table 6.70)
            self.sd_upload_new(SD_DGNA, ms, DGNAI, false, r.service_options, blocks);
            return;
        }
        let group = match what {
            SD_UDT => r.service_kind == SK_TG_UDT,
            _ => rand_status_group(r),
        };
        if what == SD_STATUS {
            // not served: the transmit interrupt statuses (123, 125; 6.6.6.4), refused from TSI as 6.4.14.3.3.1 and
            // 6.4.14.3.4.2 ask ("If Transmit Interrupt is not available the TSCC shall respond C_NACKD (Source = TSI,
            // Target = Interrupting MS)"), and extended addresses (the Proxy Flag)
            let st = rand_status(r);
            if matches!(st, 123 | 125) {
                self.refuse(R_NOT_SUPPORTED, ms, TSI);
                return;
            }
            if r.proxy {
                self.refuse(R_NOT_SUPPORTED, ms, r.target);
                return;
            }
            // the emergency alarm (6.6.6.3) only from an MS whose provisioning allows emergencies
            if st == STATUS_EMERGENCY_ALARM && self.ms_policy(ms).emergency_ok == 0 {
                self.refuse(R_PERM_USER_REFUSED, ms, r.target);
                return;
            }
        }
        let Some(group) = self.sd_target(ms, r.target, group, what == SD_POLL) else { return };
        // Supplementary data (SUPED_SV) isn't taken: the call goes on without it (6.6.6.1.2.1.2 a)
        if what == SD_UDT {
            self.sd_upload_new(SD_UDT, ms, r.target, group, r.service_options, rand_sdata_blocks(r));
            return;
        }
        let Some(i) = self.sd_free() else {
            self.refuse(R_SYSBUSY, ms, r.target);
            return;
        };
        let mut d = Sd {
            used: true,
            what,
            origin: O_MS,
            group,
            src: ms,
            dst: r.target,
            tries: self.cfg.ahoy_tries.max(1),
            deadline_ms: self.now + self.cfg.tnp_ms as i64,
            next_ack_ms: self.now + ACK_EVERY_MS,
            ..Default::default()
        };
        d.value = rand_status(r);
        self.sd.sds[i] = d;
        self.sd_route(i);
    }

    /// A transaction that starts with an upload from ms (C_AHOY from SDMI, DGNAI or TATTSI): what SD_UDT,
    /// SD_DGNA or SD_TGLIST, dst the C_RAND's target, so its Service_Options, blocks the UAB it asked for.
    pub(super) fn sd_upload_new(&mut self, what: u8, ms: u32, dst: u32, group: bool, so: u8, blocks: u8) {
        let Some(i) = self.sd_free() else {
            self.refuse(R_SYSBUSY, ms, dst);
            return;
        };
        let mut d = Sd {
            used: true,
            what,
            origin: O_MS,
            group,
            state: S_UPLOAD,
            ahoy_due: true,
            so,
            src: ms,
            dst,
            tries: self.cfg.ahoy_tries.max(1),
            deadline_ms: self.now + self.cfg.tnp_ms as i64,
            next_ack_ms: self.now + ACK_EVERY_MS,
            ..Default::default()
        };
        d.head.blocks = blocks.clamp(1, MAX_BLOCKS as u8);
        self.sd.sds[i] = d;
        if self.sd.upload.active {
            self.ack(R_WAIT, ms, dst); // another MS is uploading: this one's turn comes
        }
    }

    /// An upload of kind what (SD_TGLIST, SD_DIVERT) in progress for ms: asks again when it waits for it (the MS
    /// repeated its C_RAND); true when there is one.
    pub(super) fn sd_again(&mut self, what: u8, ms: u32) -> bool {
        let Some(i) = (0..SDS).find(|&i| {
            let d = &self.sd.sds[i];
            d.used && d.what == what && d.src == ms
        }) else {
            return false;
        };
        if self.sd.sds[i].state == S_UPLOAD && !(self.sd.upload.active && self.sd.upload.sd == i) {
            self.sd.sds[i].ahoy_due = true;
        }
        true
    }

    /// The message is whole (uploaded, or a status): on to the site, the talkgroup, the MS or the network.
    fn sd_route(&mut self, i: usize) {
        let d = self.sd.sds[i];
        if d.what == SD_TGLIST {
            let mut buf = [0u8; MAX_DATA];
            let n = d.blocks.data(&mut buf);
            let (reason, info) = self.tgl_apply(d.src, &AddrList::of(&buf[..n]));
            self.sd_done(i, reason, info, WHY_OK);
            return;
        }
        if d.what == SD_DIVERT {
            let reason = self.div_set(d.src, d.dst, d.so, d.value, &d.blocks);
            let why = if reason == R_MESSAGE_ACCEPTED { WHY_OK } else { WHY_REFUSED };
            self.sd_done(i, reason, 0, why); // C_ACKD from MSI or GPI (6.6.7.1.1.2)
            return;
        }
        if d.what == SD_DGNA {
            // the called MS: an MS (here or elsewhere), and talkgroups this site allows
            let not_ms = d.dst == ADRNULL || is_gateway(d.dst) || is_allcall(d.dst) || d.head.group;
            let why = if not_ms || d.dst == TSI || d.dst == d.src {
                R_NOT_SUPPORTED
            } else {
                self.dgna_refusal(d.value, &d.blocks)
            };
            if why != 0 {
                self.sd_done(i, why, 0, WHY_REFUSED);
                return;
            }
            // 6.4.9.0: a stunned MS may not "receive any user initiated services" (as supp_gate for the others)
            if d.origin == O_MS && self.stunned(d.dst) {
                self.sd_done(i, R_MSAWAY, 0, WHY_REFUSED);
                return;
            }
        }
        if d.dst == TSI && !d.group {
            let m = self.sd_msg(&d, SD_SEND);
            self.ev(m.event(EV_SD));
            self.sd_done(i, R_MESSAGE_ACCEPTED, 0, WHY_OK);
            return;
        }
        let s = &mut self.sd.sds[i];
        s.next_ms = self.now;
        s.tries = if d.group { self.cfg.sd_repeats.max(1) } else { self.cfg.sd_tries.max(1) };
        // the caller hears how it goes (6.6.4.1.3, 6.6.6.1.1.3): C_WACKD, or C_QACKD while the called MS is busy
        let busy = !d.group && self.reg_index(d.dst).is_some() && self.sd_busy(d.dst);
        let found = d.group || self.reg_index(d.dst).is_some() || (d.origin != O_NET && self.connected);
        if d.origin == O_MS && found {
            self.ack(if busy { R_QUEUED_BUSY } else { R_WAIT }, d.src, d.dst);
        }
        let s = &mut self.sd.sds[i];
        if d.group {
            s.state = S_GROUP;
            if d.origin != O_NET && self.connected && (d.dst == ALLMSID || self.policy.wide_tg.contains(d.dst)) {
                let m = self.sd_msg(&d, SD_SEND);
                let stream = self.new_stream();
                self.ev(SdMsg { stream, ..m }.event(EV_NET_SD));
            }
            return;
        }
        if self.reg_index(d.dst).is_some() {
            if busy {
                let s = &mut self.sd.sds[i];
                s.state = S_HOLD;
                s.deadline_ms = self.now + self.cfg.sd_store_ms as i64;
            } else {
                self.sd.sds[i].state = S_DOWN;
            }
            return;
        }
        if d.origin == O_NET || !self.connected {
            self.sd_done(i, R_NOREG_MSAWAY, 0, WHY_NOT_FOUND);
            return;
        }
        let stream = self.new_stream();
        let s = &mut self.sd.sds[i];
        s.state = S_NET;
        s.stream = stream;
        s.deadline_ms = self.now + self.cfg.tnp_ms as i64;
        let m = self.sd_msg(&self.sd.sds[i], SD_SEND);
        self.ev(SdMsg { stream, ..m }.event(EV_NET_SD));
    }

    fn sd_msg(&self, d: &Sd, op: u8) -> SdMsg {
        let mut m = SdMsg {
            op,
            what: d.what,
            value: d.value,
            flags: if d.group { SD_F_GROUP } else { 0 } | if d.emergency { SD_F_EMERGENCY } else { 0 },
            pad_nibble: d.head.pad_nibble,
            src: d.src,
            dst: d.dst,
            ..Default::default()
        };
        if d.what == SD_UDT || d.what == SD_DGNA {
            let mut buf = [0u8; MAX_DATA];
            let n = d.blocks.data(&mut buf);
            m.len = n as u8;
            m.data[..n].copy_from_slice(&buf[..n]);
        }
        m
    }

    /// The end of transaction i: the final answer to where it came from (reason the air reason, info the
    /// Response_Info of a poll's answer).
    fn sd_done(&mut self, i: usize, reason: u8, info: u8, why: u8) {
        let d = self.sd.sds[i];
        self.sd.sds[i].used = false;
        if self.sd.upload.active && self.sd.upload.sd == i {
            self.sd.upload.active = false;
        }
        let ok = reason_type(reason) == 1;
        if d.what == SD_NOTICE {
            return;
        }
        if d.what == SD_TGLIST {
            // 6.4.4.1.13.2: registered either way; a list that didn't come is a list refused (Response_Info 0)
            let (reason, ri) = if ok { (reason, info) } else { (R_REG_SUBSCRIPTION, 0) };
            let a = Ack { op: OP_ACKD, response_info: ri, reason, target: d.src, source: TATTSI };
            self.push(DT_CSBK, a.encode(), false, None); // once, as a registration's (figure 6.25)
            let sub = self.reg_index(d.src).map_or(0, |r| self.regs[r].rec.sub_tg);
            self.activity(ACT_REG, REG_REGISTER, d.src, sub, reason);
            return;
        }
        if d.what == SD_STATUS && d.origin == O_MS {
            self.status_done(d.value, d.group, d.src, d.dst, reason);
        }
        if d.what != SD_DIVERT {
            if ok {
                self.sd.delivered += 1;
            } else {
                self.sd.failed += 1;
            }
        }
        match d.origin {
            O_MS => {
                let ri = if d.what == SD_POLL && ok { info } else { SysCode::decode(self.syscode).response_check() };
                let a = Ack { op: OP_ACKD, response_info: ri, reason, target: d.src, source: d.dst };
                for _ in 0..FINAL_REPEATS {
                    self.push(DT_CSBK, a.encode(), false, None);
                }
                if !ok {
                    self.counters.refused += 1;
                    self.activity(ACT_REFUSE, 0, d.src, d.dst, reason);
                }
            }
            O_APP => {
                let m = SdMsg { op: SD_RESULT, reason, why, value: info, r#ref: d.r#ref, ..self.sd_msg(&d, SD_RESULT) };
                self.ev(SdMsg { len: 0, data: [0; 48], ..m }.event(EV_SD));
            }
            O_NET if !d.group => {
                let stream = self.new_stream();
                let m = SdMsg { stream, ..self.sd_msg(&d, SD_RESULT) };
                let m = SdMsg { reason, why, value: info, r#ref: d.r#ref, len: 0, data: [0; 48], ..m };
                self.ev(m.event(EV_NET_SD));
            }
            _ => {}
        }
    }

    // ── the TSCC: blocks in a row ──

    /// The next block of a train in progress, or a slot kept for an upload: (dt, the first 10 octets, AT busy).
    pub(super) fn sd_cont(&mut self) -> Option<(u8, [u8; 10], bool)> {
        let s = &mut self.sd;
        if s.train_pos < s.train_len {
            let p = s.train_pos;
            s.train_pos += 1;
            let last = s.train_pos == s.train_len;
            s.ext = [s.train[p][10], s.train[p][11]];
            let at = last && s.train_at_last;
            return Some((s.train_dt[p], first10(&s.train[p]), at));
        }
        if s.keep > 0 {
            s.keep -= 1;
            let mut a = Aloha::decode(&self.aloha()).unwrap_or_default();
            a.mask = 24; // the next inbound slot is no one's to random access (figure 6.43)
            a.addr = ADRNULL;
            return Some((DT_CSBK, a.encode(), true));
        }
        None
    }

    fn train_start(&mut self, blocks: &[(u8, [u8; 12])], at_last: bool) -> (u8, [u8; 10], bool) {
        let s = &mut self.sd;
        for (j, (dt, b)) in blocks.iter().enumerate() {
            s.train_dt[j] = *dt;
            s.train[j] = *b;
        }
        s.train_len = blocks.len();
        s.train_pos = 0;
        s.train_at_last = at_last;
        self.sd_cont().unwrap_or((DT_CSBK, [0; 10], false))
    }

    /// Something in progress (for the hibernating TSCC: it stays awake).
    pub(super) fn sd_pending(&self) -> bool {
        self.sd.sds.iter().any(|d| d.used)
    }

    /// A transaction's next transmission when one is due: an upload's C_AHOY, a download, a status C_AHOY.
    pub(super) fn sd_start(&mut self) -> Option<(u8, [u8; 10], bool)> {
        let now = self.now;
        if !self.sd.upload.active {
            if let Some(i) = self.sd.sds.iter().position(|d| d.used && d.state == S_UPLOAD && d.ahoy_due) {
                // 6.6.4.1.1 d): C_AHOY from SDMI to the caller, Service_Kind 0100b whatever the message's target,
                // Appended_Blocks the C_RAND's Appended_UDT_Short_Data; for DGNA from DGNAI, Service_Kind 1101b
                // (6.6.8.2.1 c); for a registration's talkgroup list from TATTSI, Service_Kind 1110b and
                // Service_Kind_Flag 1 (6.4.4.1.13.2 table 6.11)
                let d = &mut self.sd.sds[i];
                d.ahoy_due = false;
                let n = d.head.blocks.clamp(1, MAX_BLOCKS as u8);
                let (service_kind, source) = match d.what {
                    SD_DGNA => (SK_SUPPL, DGNAI),
                    SD_TGLIST => (SK_REG, TATTSI),
                    SD_DIVERT => (SK_DIVERT, DIVERTI), // 6.6.7.1.1.1 b)
                    _ => (SK_IND_UDT, SDMI),
                };
                let a = Ahoy {
                    service_options: d.so,
                    service_kind_flag: d.what == SD_TGLIST,
                    appended: n - 1,
                    service_kind,
                    target: d.src,
                    source,
                    ..Default::default()
                };
                self.sd.upload = Upload {
                    active: true,
                    sd: i,
                    from: d.src,
                    until_k: self.k + 2 * (n as i64 + 1) + 8,
                    ..Default::default()
                };
                self.sd.keep = n;
                return Some(self.train_start(&[(DT_CSBK, pdu12(&a.encode()))], true));
            }
        }
        // power save (6.4.7): one to a sleeping MS waits for its wake frame, its time limit stretched to cover it
        for i in 0..SDS {
            let d = self.sd.sds[i];
            if d.used && d.state == S_DOWN && !d.group && now >= d.next_ms && !self.ps_awake(d.dst) {
                let wait = self.ps_wait_ms(d.dst);
                let s = &mut self.sd.sds[i];
                s.deadline_ms = s.deadline_ms.max(now + wait);
                s.next_ms = now + 60; // look again next frame
            }
        }
        let i = (0..SDS).find(|&i| {
            let d = &self.sd.sds[i];
            d.used && matches!(d.state, S_DOWN | S_GROUP) && now >= d.next_ms && d.tries > 0
        })?;
        let d = self.sd.sds[i];
        let s = &mut self.sd.sds[i];
        s.tries -= 1;
        s.next_ms = now + self.cfg.ahoy_every_ms as i64;
        let notice = d.what == SD_NOTICE;
        let solicit = !d.group && !notice;
        if !matches!(d.what, SD_UDT | SD_DGNA | SD_NOTICE) {
            return Some(self.train_start(&[(DT_CSBK, pdu12(&Self::status_ahoy(&d)))], solicit));
        }
        // 6.6.4.1.4: the header (A = 1 to an MS, 6.5.1.2 table 6.34), then the blocks as they came
        let h = UdtHead {
            op: if d.what == SD_DGNA { OP_DGNAHD } else { OP_UDTHD }, // table 6.69
            group: d.group,
            response: solicit,
            emergency: d.emergency,
            format: d.value,
            target: d.dst,
            source: d.src,
            pad_nibble: d.head.pad_nibble,
            blocks: d.blocks.n,
            sf: notice, // 6.6.7.2: Supplementary_Flag 1, UDT Response 0
            ..Default::default()
        };
        let mut t = [(0u8, [0u8; 12]); TRAIN];
        t[0] = (DT_DATA_HDR, pdu12(&h.encode()));
        let n = d.blocks.n as usize;
        for j in 0..n {
            t[1 + j] = (DT_RATE12, d.blocks.b[j]);
        }
        Some(self.train_start(&t[..1 + n], solicit))
    }

    /// The octets 10 and 11 of this slot's block (a rate 1/2 block's), taken.
    pub(super) fn sd_ext(&mut self) -> [u8; 2] {
        core::mem::take(&mut self.sd.ext)
    }

    // ── what MSs send back ──

    /// A UDT header on the TSCC (C_UDTHU).
    pub(super) fn sd_rx_head(&mut self, pdu: &[u8; 10]) {
        let Some(h) = UdtHead::decode(pdu) else { return };
        if h.op != OP_UDTHU && h.op != OP_DGNAHU {
            return;
        }
        self.heard(h.source);
        let u = &mut self.sd.upload;
        if !u.active || h.source != u.from {
            return; // not asked for (an upload this site didn't ask for): ignored
        }
        u.have_head = true;
        u.got = 0;
        u.head = h;
        u.blocks = Blocks { n: h.blocks, ..Default::default() };
        u.until_k = u.until_k.max(self.k + 2 * h.blocks as i64 + 6);
    }

    /// A rate 1/2 block on the TSCC: an appended block of the upload in progress.
    pub(super) fn sd_rx_block(&mut self, b: &[u8; 12]) {
        let u = &mut self.sd.upload;
        if !u.active || !u.have_head || u.got >= u.blocks.n {
            return;
        }
        u.blocks.b[u.got as usize] = *b;
        u.got += 1;
        if u.got < u.blocks.n {
            return;
        }
        let u = *u;
        self.sd.upload.active = false;
        let i = u.sd;
        if !u.blocks.crc_ok() {
            self.sd_upload_failed(i);
            return;
        }
        let d = &mut self.sd.sds[i];
        d.head = u.head;
        d.blocks = u.blocks;
        d.value = u.head.format;
        d.emergency = false;
        if d.what == SD_DGNA {
            d.dst = u.head.target; // the called MS comes with the upload (6.6.8.3.1)
        }
        self.sd_route(i);
    }

    /// No upload, or a bad one: ask again while tries last, then C_NACKD(CRC error in the UDT upload phase).
    fn sd_upload_failed(&mut self, i: usize) {
        let d = &mut self.sd.sds[i];
        d.tries = d.tries.saturating_sub(1);
        if d.tries > 0 {
            d.ahoy_due = true;
            return;
        }
        self.sd_done(i, R_UDT_CRC, 0, WHY_TIMEOUT);
    }

    /// C_ACKU / C_NACKU for a transaction: true when it was one's.
    pub(super) fn sd_ack(&mut self, a: &Ack) -> bool {
        // the caller refusing to upload (6.6.4.2: it may answer C_AHOY from SDMI with C_NACKU)
        if matches!(a.target, SDMI | DGNAI | TATTSI | DIVERTI) {
            if let Some(i) = (0..SDS).find(|&i| {
                let d = &self.sd.sds[i];
                d.used && d.state == S_UPLOAD && d.src == a.source
            }) {
                self.sd_done(i, a.reason, 0, WHY_REFUSED);
                return true;
            }
            // else the called MS's answer to a download from the gateway (DGNA from the site, source DGNAI)
        }
        let Some(i) = (0..SDS).find(|&i| {
            let d = &self.sd.sds[i];
            d.used && d.state == S_DOWN && d.dst == a.source && d.src == a.target
        }) else {
            return false;
        };
        let d = self.sd.sds[i];
        if reason_type(a.reason) == 1 {
            // 6.6.4.1.5, 6.6.6.1.1.4, 6.6.6.2.1.3: the called MS's acknowledgement, mirrored
            let info = if d.what == SD_POLL { a.response_info } else { 0 };
            if d.what == SD_DGNA {
                self.tgl_dgna(d.dst, &d.blocks, d.value); // 6.6.8.1.1: the MS now holds these
            }
            self.sd_done(i, a.reason, info, WHY_OK);
        } else {
            let why = if a.reason == R_EQUIP_BUSY { WHY_BUSY } else { WHY_REFUSED };
            self.sd_done(i, a.reason, 0, why);
        }
        true
    }

    /// Cancel from the caller (6.4.6): true when it was a transaction's.
    pub(super) fn sd_cancel(&mut self, ms: u32) -> bool {
        let Some(i) =
            (0..SDS).find(|&i| self.sd.sds[i].used && self.sd.sds[i].origin == O_MS && self.sd.sds[i].src == ms)
        else {
            return false;
        };
        let d = self.sd.sds[i];
        if matches!(d.state, S_UPLOAD | S_HOLD | S_NET) {
            self.sd.sds[i].used = false;
            if self.sd.upload.active && self.sd.upload.sd == i {
                self.sd.upload.active = false;
            }
            self.ack(R_MESSAGE_ACCEPTED, ms, d.dst);
        } else {
            self.refuse(R_CALL_CANCEL_REFUSED, ms, d.dst);
        }
        true
    }

    // ── the timers ──

    pub(super) fn sd_timers(&mut self) {
        let now = self.now;
        if self.sd.upload.active && self.k > self.sd.upload.until_k {
            let i = self.sd.upload.sd;
            self.sd.upload.active = false;
            self.sd_upload_failed(i);
        }
        for i in 0..SDS {
            let d = self.sd.sds[i];
            if !d.used {
                continue;
            }
            let busy_train = self.sd.train_pos < self.sd.train_len;
            match d.state {
                S_GROUP if d.tries == 0 && !busy_train => {
                    // the last outbound phase is out: Message_Accepted (6.6.4.1.5, 6.6.6.1.1.4)
                    self.sd_done(i, R_MESSAGE_ACCEPTED, 0, WHY_OK);
                    continue;
                }
                S_DOWN if d.tries == 0 && now >= d.next_ms => {
                    self.sd_done(i, R_MSAWAY, 0, WHY_NO_ANSWER);
                    continue;
                }
                S_HOLD if !self.sd_busy(d.dst) => {
                    let s = &mut self.sd.sds[i];
                    s.state = S_DOWN;
                    s.next_ms = now;
                }
                S_HOLD | S_NET | S_UPLOAD if now >= d.deadline_ms => {
                    let (reason, why) = match d.state {
                        S_HOLD => (R_CALLED_PARTY_BUSY, WHY_BUSY),
                        S_NET => (R_MSAWAY, WHY_TIMEOUT),
                        _ => (R_UDT_CRC, WHY_TIMEOUT),
                    };
                    self.sd_done(i, reason, 0, why);
                    continue;
                }
                _ => {}
            }
            let d = self.sd.sds[i];
            if d.origin == O_MS && matches!(d.state, S_HOLD | S_DOWN | S_NET) && now >= d.next_ack_ms {
                let reason = if d.state == S_HOLD { R_QUEUED_BUSY } else { R_WAIT };
                self.ack(reason, d.src, d.dst);
                self.sd.sds[i].next_ack_ms = now + ACK_EVERY_MS;
            }
        }
    }

    /// A diverted address to a caller (SD_NOTICE): source MSI or GPI, blocks the Address Format. False when
    /// no transaction is free.
    pub(super) fn sd_notice(&mut self, caller: u32, source: u32, blocks: &Blocks) -> bool {
        let Some(i) = self.sd_free() else { return false };
        self.sd.sds[i] = Sd {
            used: true,
            what: SD_NOTICE,
            state: S_GROUP,
            value: FMT_ADDRESS,
            tries: FINAL_REPEATS as u8,
            src: source,
            dst: caller,
            blocks: *blocks,
            head: UdtHead { blocks: blocks.n, ..Default::default() },
            next_ms: self.now,
            ..Default::default()
        };
        true
    }

    // ── the application and the network ──

    /// A message from the site's application (or a status poll) to an MS or a talkgroup; false when it can't
    /// go (no room, bad address or data). Its outcome comes as an EV_SD result with its ref.
    pub fn sd_send(&mut self, m: &SdMsg, now_ms: i64) -> bool {
        self.now = self.now.max(now_ms);
        self.sd_new(m, O_APP)
    }

    /// A message or a result from the network (mqtt-net `sdata`).
    pub fn net_sd_in(&mut self, m: &SdMsg, now_ms: i64) {
        self.now = self.now.max(now_ms);
        if m.op == SD_RESULT {
            let Some(i) = (0..SDS).find(|&i| {
                let d = &self.sd.sds[i];
                d.used && d.state == S_NET && d.stream == m.r#ref
            }) else {
                return;
            };
            let why = if reason_type(m.reason) == 1 {
                WHY_OK
            } else if m.why != 0 {
                m.why
            } else {
                WHY_REFUSED
            };
            self.sd_done(i, m.reason, m.value, why);
            return;
        }
        let group = m.flags & SD_F_GROUP != 0;
        if group && !self.wants_tg(m.dst) && !is_allcall(m.dst) {
            return;
        }
        if !group && self.reg_index(m.dst).is_none() {
            return; // not here: another site may have it
        }
        self.sd_new(&SdMsg { r#ref: m.stream, ..*m }, O_NET);
    }

    fn sd_new(&mut self, m: &SdMsg, origin: u8) -> bool {
        let group = m.flags & SD_F_GROUP != 0;
        if m.len as usize > MAX_DATA {
            return false;
        }
        let bad_dst = m.dst == ADRNULL || is_gateway(m.dst) || (!group && is_allcall(m.dst));
        if bad_dst || (group && m.what == SD_POLL) || !(SD_UDT..=SD_DGNA).contains(&m.what) {
            return false;
        }
        let dgna = m.what == SD_DGNA;
        if dgna && (group || !matches!(m.value, FMT_ADDRESS | FMT_MIXED)) {
            return false; // 6.6.8.0: to an individual MS only, the Address or Mixed Format
        }
        let Some(i) = self.sd_free() else { return false };
        let mut d = Sd {
            used: true,
            what: m.what,
            origin,
            group,
            emergency: m.flags & SD_F_EMERGENCY != 0,
            value: if m.what == SD_POLL { STATUS_POLL } else { m.value },
            src: if m.src != 0 {
                m.src
            } else if dgna {
                DGNAI
            } else {
                TSI
            },
            dst: m.dst,
            r#ref: m.r#ref,
            deadline_ms: self.now + self.cfg.tnp_ms as i64,
            ..Default::default()
        };
        if m.what == SD_UDT || dgna {
            // a character format padded as annex B says; the others as the application gave them
            let Some((b, pn)) = Blocks::of_format(m.data(), m.value) else { return false };
            d.blocks = b;
            let pad_nibble = if pads_nibbles(m.value) { pn } else { m.pad_nibble };
            d.head = UdtHead { blocks: b.n, pad_nibble, format: m.value, ..Default::default() };
        } else if m.what == SD_STATUS && m.value >= STATUS_POLL {
            return false;
        }
        self.sd.sds[i] = d;
        self.sd_route(i);
        true
    }
}

// ── C API (rdof/t3.h) ──────────────────────────────────────────────────────────

#[no_mangle]
pub unsafe extern "C" fn rdof_t3_sd_send(s: *mut Site, m: *const SdMsg, now_ms: i64) -> bool {
    !m.is_null() && (*s).sd_send(&*m, now_ms)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3_sd_of_event(e: *const Event, m: *mut SdMsg) {
    *m = SdMsg::of_event(&*e);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3_net_sd(s: *mut Site, m: *const SdMsg, now_ms: i64) {
    if !m.is_null() {
        (*s).net_sd_in(&*m, now_ms);
    }
}
