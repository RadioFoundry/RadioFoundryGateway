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
/// A transmit interrupt across sites (tier3.md 3.20): src the interrupting MS, dst the talker to de-key, value the
/// status (125), data the call's destination (3 octets) and 1 for a talkgroup call (0 an individual call). Only
/// between sites (never from the application); its result's reason is Message_Accepted or a C_NACKD reason.
pub const SD_INTR: u8 = 5;
/// A UDT short data poll across sites (tier3.md 5.9): src the caller, dst the polled MS, value the C_RAND's
/// Service_Options (the Polling Format in its four LSBs), data one octet: the UDTs asked for (1-4). Its result
/// carries the polled MS's data (value its UDT_Format) or the refusal. From the application: a poll from SDMI
/// (6.6.5.1.5) of an MS registered here, value the Polling Format, data the UDTs asked for (none: 4).
pub const SD_UDT_POLL: u8 = 6;
/// A registration's talkgroup list upload (6.4.4.1.13.2): only from an MS here, never an SdMsg
pub(super) const SD_TGLIST: u8 = 0x41;
/// A call diversion's address upload (6.6.7.1): only from an MS
pub(super) const SD_DIVERT: u8 = 0x42;
/// The diverted address to a caller (6.6.7.2): a UDT header with SF 1 and no answer, sent FINAL_REPEATS times
pub(super) const SD_NOTICE: u8 = 0x43;
/// A UDT short data poll from an MS (6.6.5.1): src the caller, dst the polled MS (here). The C_AHOY from the caller
/// asks the polled MS to upload its data (S_UPLOAD, the upload from dst); then the download to the caller (S_DOWN,
/// from dst to src), whose C_ACKU ends it.
pub(super) const SD_UPOLL: u8 = 0x44;
/// The IP address of a packet data call to IPI (6.6.3.2.4 b 2, B.3.7): the C_AHOY from IPI asks the caller for it,
/// the caller uploads C_UDTHU, UDT_Format IP; the data call (t3pdata, PD_ADDR) goes on with it. r#ref: the IPv4
/// address once uploaded.
pub(super) const SD_IPADDR: u8 = 0x45;
/// IP connection advice (6.4.11): a registration with IP_Inform 1 (so: its Service_Options, Reg_Dereg 1 adding the
/// address, 0 deleting it); the C_AHOY from IPI asks for the address as SD_IPADDR does. r#ref: the IPv4 address.
pub(super) const SD_IPADV: u8 = 0x46;

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
    ps_held: bool,  // S_UPLOAD: a poll's C_AHOY held for the polled MS's wake frame (the caller told to wait)
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
            SK_UDT_POLL => SD_UPOLL,
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
        if what == SD_UPOLL {
            self.upoll_rand(r);
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
            // the transmit interrupt statuses (123, 125; 6.6.6.0) aren't delivered: they ask the TS to de-key a
            // talker (6.4.14.3.3, 6.4.14.3.4; t3intr.rs). Not served: extended addresses (the Proxy Flag)
            let st = rand_status(r);
            if matches!(st, STATUS_ALS_END | STATUS_INTERRUPT) {
                self.intr_request(r, st);
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

    /// 6.6.5.1: a UDT short data poll of an individual MS registered here (its diversion is supp_gate's). The answer
    /// is the C_AHOY to the polled MS (6.6.5.1.1 b), or C_QACKD while it's busy. Not served: a poll of an MS at
    /// another site, gateways, talkgroups.
    fn upoll_rand(&mut self, r: &Rand) {
        let (ms, dst) = (r.source, r.target);
        let bad = dst == ADRNULL || dst == ms || dst == TSI || is_gateway(dst) || is_allcall(dst);
        if bad || r.proxy {
            self.refuse(R_NOT_SUPPORTED, ms, dst);
            return;
        }
        if self.reg_index(dst).is_none() && !self.connected {
            self.refuse(R_NOREG_MSAWAY, ms, dst);
            return;
        }
        let Some(i) = self.sd_free() else {
            self.refuse(R_SYSBUSY, ms, dst);
            return;
        };
        if self.reg_index(dst).is_none() {
            // another site may have it: the poll goes there (5.9), the caller waits; its answer comes back as
            // the result's data and is downloaded here
            let stream = self.new_stream();
            let mut d = Sd {
                used: true,
                what: SD_UPOLL,
                origin: O_MS,
                state: S_NET,
                so: r.service_options,
                src: ms,
                dst,
                stream,
                // the far site's own C_AHOYs and upload come first
                deadline_ms: self.now + 2 * self.cfg.tnp_ms as i64,
                next_ack_ms: self.now + ACK_EVERY_MS,
                ..Default::default()
            };
            d.head.blocks = rand_sdata_blocks(r);
            self.sd.sds[i] = d;
            let m = self.sd_msg(&d, SD_SEND);
            self.ev(SdMsg { stream, ..m }.event(EV_NET_SD));
            self.ack(R_WAIT, ms, dst);
            return;
        }
        let busy = self.sd_busy(dst);
        let mut d = Sd {
            used: true,
            what: SD_UPOLL,
            origin: O_MS,
            state: if busy { S_HOLD } else { S_UPLOAD },
            ahoy_due: !busy,
            so: r.service_options,
            src: ms,
            dst,
            tries: self.cfg.ahoy_tries.max(1),
            deadline_ms: self.now + if busy { self.cfg.sd_store_ms } else { self.cfg.tnp_ms } as i64,
            next_ack_ms: self.now + ACK_EVERY_MS,
            ..Default::default()
        };
        d.head.blocks = rand_sdata_blocks(r);
        self.sd.sds[i] = d;
        if busy {
            self.ack(R_QUEUED_BUSY, ms, dst);
        } else if self.sd.upload.active {
            self.ack(R_WAIT, ms, dst); // another MS is uploading: this one's turn comes
        }
    }

    /// A transaction that starts with an upload from ms (C_AHOY from SDMI, DGNAI or TATTSI): what SD_UDT,
    /// SD_DGNA or SD_TGLIST, dst the C_RAND's target, so its Service_Options, blocks the UAB it asked for.
    /// False: none free (SYSbusy sent).
    pub(super) fn sd_upload_new(&mut self, what: u8, ms: u32, dst: u32, group: bool, so: u8, blocks: u8) -> bool {
        let Some(i) = self.sd_free() else {
            self.refuse(R_SYSBUSY, ms, dst);
            return false;
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
        true
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
        if d.what == SD_UPOLL && matches!(d.origin, O_NET | O_APP) {
            self.sd_done(i, R_MESSAGE_ACCEPTED, 0, WHY_OK); // the data back to the caller's site or the application
            return;
        }
        if d.what == SD_UPOLL {
            // 6.6.5.1.3: the polled data down to the caller
            let s = &mut self.sd.sds[i];
            s.state = S_DOWN;
            s.next_ms = self.now;
            s.tries = self.cfg.sd_tries.max(1);
            s.deadline_ms = self.now + self.cfg.tnp_ms as i64;
            return;
        }
        if d.what == SD_IPADDR || d.what == SD_IPADV {
            // B.3.7: IPv4 in octets 0-3 of one block (UAB 0); IPv6 in two (UAB 1), not used by the IP gateway
            let mut buf = [0u8; MAX_DATA];
            let n = d.blocks.data(&mut buf);
            let ok = d.value == FMT_IP && d.head.blocks == 1 && n >= 4;
            self.sd.sds[i].r#ref = if ok { u32::from_be_bytes([buf[0], buf[1], buf[2], buf[3]]) } else { 0 };
            let reason = if ok { R_MESSAGE_ACCEPTED } else { R_NOT_SUPPORTED };
            self.sd_done(i, reason, 0, if ok { WHY_OK } else { WHY_REFUSED });
            return;
        }
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
        if d.what == SD_UPOLL {
            // a request: the C_RAND's Service_Options and the UDTs asked for; a result: the polled data, as UDT
            // short data
            m.what = SD_UDT_POLL;
            if op == SD_SEND {
                m.value = d.so;
                m.len = 1;
                m.data[0] = d.head.blocks.clamp(1, MAX_BLOCKS as u8);
                return m;
            }
        }
        if d.what == SD_UDT || d.what == SD_DGNA || (d.what == SD_UPOLL && d.blocks.n > 0) {
            let mut buf = [0u8; MAX_DATA];
            let mut n = d.blocks.data(&mut buf);
            if d.what != SD_DGNA && pads_nibbles(d.value) {
                // the message, not its blocks: the pad octets stay here (the far site pads it again; an odd pad
                // nibble stays in the last octet and goes as pad_nibble)
                n -= (usize::from(d.head.pad_nibble) / 2).min(n);
            }
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
        if d.what == SD_IPADDR {
            self.pd_addr(d.src, ok.then_some(d.r#ref), reason); // the data call's to answer
            return;
        }
        if d.what == SD_IPADV {
            self.ip_advice_done(d.src, d.so, ok.then_some(d.r#ref)); // the registration's to answer
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
            // a poll: the caller's C_ACKU ends it (6.6.5.1.4); a failure before the download, C_NACKD to it with
            // the polled MS's reason mirrored (6.6.5.1.2 a)
            O_MS if d.what == SD_UPOLL => {
                if !ok && d.state != S_DOWN {
                    let a = Ack { op: OP_ACKD, response_info: 0, reason, target: d.src, source: d.dst };
                    for _ in 0..FINAL_REPEATS {
                        self.push(DT_CSBK, a.encode(), false, None);
                    }
                    self.counters.refused += 1;
                    self.activity(ACT_REFUSE, 0, d.src, d.dst, reason);
                }
            }
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
            O_APP if d.what == SD_UPOLL => {
                // the polled data (value its UDT_Format), or the refusal
                let m = SdMsg { op: SD_RESULT, reason, why, r#ref: d.r#ref, ..self.sd_msg(&d, SD_RESULT) };
                let m = if ok { m } else { SdMsg { value: 0, len: 0, data: [0; 48], ..m } };
                self.ev(m.event(EV_SD));
            }
            O_APP => {
                let m = SdMsg { op: SD_RESULT, reason, why, value: info, r#ref: d.r#ref, ..self.sd_msg(&d, SD_RESULT) };
                self.ev(SdMsg { len: 0, data: [0; 48], ..m }.event(EV_SD));
            }
            O_NET if d.what == SD_UPOLL => {
                // the polled data (value its UDT_Format), or the refusal
                let stream = self.new_stream();
                let m = SdMsg { stream, ..self.sd_msg(&d, SD_RESULT) };
                let m = if ok { m } else { SdMsg { value: 0, len: 0, data: [0; 48], ..m } };
                self.ev(SdMsg { reason, why, r#ref: d.r#ref, ..m }.event(EV_NET_SD));
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
    pub fn sd_pending(&self) -> bool {
        self.sd.sds.iter().any(|d| d.used)
    }

    /// A transaction's next transmission when one is due: an upload's C_AHOY, a download, a status C_AHOY.
    pub(super) fn sd_start(&mut self) -> Option<(u8, [u8; 10], bool)> {
        let now = self.now;
        // power save (6.4.7): a poll waits for the polled MS's wake frame, its time limit stretched to cover it
        let mut ready = [false; SDS];
        for (i, r) in ready.iter_mut().enumerate() {
            let d = self.sd.sds[i];
            *r = d.used && d.state == S_UPLOAD && d.ahoy_due;
            if *r && d.what == SD_UPOLL && !self.ps_awake(d.dst) {
                *r = false;
                let wait = self.ps_wait_ms(d.dst);
                let s = &mut self.sd.sds[i];
                s.deadline_ms = s.deadline_ms.max(now + wait);
                if d.origin == O_MS && !d.ps_held {
                    // the caller waits for up to a power save cycle: C_WACKD, else it gives up on its random
                    // access (6.6.5.1.1: the TSCC may answer with C_WACKD); again every ACK_EVERY_MS (sd_timers)
                    s.ps_held = true;
                    s.next_ack_ms = now + ACK_EVERY_MS;
                    self.ack(R_WAIT, d.src, d.dst);
                }
            }
        }
        if !self.sd.upload.active {
            if let Some(i) = (0..SDS).find(|&i| ready[i]) {
                // 6.6.4.1.1 d): C_AHOY from SDMI to the caller, Service_Kind 0100b whatever the message's target,
                // Appended_Blocks the C_RAND's Appended_UDT_Short_Data; for DGNA from DGNAI, Service_Kind 1101b
                // (6.6.8.2.1 c); for a registration's talkgroup list from TATTSI, Service_Kind 1110b and
                // Service_Kind_Flag 1 (6.4.4.1.13.2 table 6.11)
                let d = &mut self.sd.sds[i];
                d.ahoy_due = false;
                let n = d.head.blocks.clamp(1, MAX_BLOCKS as u8);
                // 6.6.5.1.1 b): a poll's from the caller to the polled MS, Service_Kind 0110b, Appended_Blocks the
                // C_RAND's Appended_Short_Data (the UDTs the caller asked for)
                let (service_kind, source, target) = match d.what {
                    SD_DGNA => (SK_SUPPL, DGNAI, d.src),
                    SD_TGLIST => (SK_REG, TATTSI, d.src),
                    SD_DIVERT => (SK_DIVERT, DIVERTI, d.src), // 6.6.7.1.1.1 b)
                    SD_UPOLL => (SK_UDT_POLL, d.src, d.dst),
                    SD_IPADDR => (SK_IND_PD, IPI, d.src), // 6.6.3.1.2 b): Service_Kind 0010b
                    // 6.4.11.2.1 c) gives no Service_Kind: 0100b, as for a gateway's UDT upload (6.6.1.1 d), table
                    // 6.54); 1110b with flag 0 would read as an authentication challenge
                    SD_IPADV => (SK_IND_UDT, IPI, d.src),
                    _ => (SK_IND_UDT, SDMI, d.src),
                };
                let a = Ahoy {
                    service_options: d.so,
                    service_kind_flag: d.what == SD_TGLIST,
                    appended: n - 1,
                    service_kind,
                    target,
                    source,
                    ..Default::default()
                };
                self.sd.upload = Upload {
                    active: true,
                    sd: i,
                    from: target,
                    until_k: self.k + 2 * (n as i64 + 1) + 8,
                    ..Default::default()
                };
                self.sd.keep = n;
                let (k, kind) = (self.k, a.service_kind);
                self.log(format_args!("short data: C_AHOY {source} -> {target}, kind {kind}, {n} UDTs, slot {k}"));
                return Some(self.train_start(&[(DT_CSBK, pdu12(&a.encode()))], true));
            }
        }
        // power save (6.4.7): one to a sleeping MS waits for its wake frame, its time limit stretched to cover it
        for i in 0..SDS {
            let d = self.sd.sds[i];
            let to = if d.what == SD_UPOLL { d.src } else { d.dst };
            if d.used && d.state == S_DOWN && !d.group && now >= d.next_ms && !self.ps_awake(to) {
                let wait = self.ps_wait_ms(to);
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
        if !matches!(d.what, SD_UDT | SD_DGNA | SD_NOTICE | SD_UPOLL) {
            return Some(self.train_start(&[(DT_CSBK, pdu12(&Self::status_ahoy(&d)))], solicit));
        }
        // 6.6.4.1.4: the header (A = 1 to an MS, 6.5.1.2 table 6.34), then the blocks as they came
        let poll = d.what == SD_UPOLL; // the polled MS's data to the caller (6.6.5.1.3)
        let h = UdtHead {
            op: if d.what == SD_DGNA { OP_DGNAHD } else { OP_UDTHD }, // table 6.69
            group: d.group,
            response: solicit,
            emergency: d.emergency,
            format: d.value,
            target: if poll { d.src } else { d.dst },
            source: if poll { d.dst } else { d.src },
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
        // an IP address: figure 6.34 has source the MS, target IPI; 6.4.11.1.3 the two the other way round
        let ip = u.active && matches!(self.sd.sds[u.sd].what, SD_IPADDR | SD_IPADV);
        let from = if ip && h.source == IPI { h.target } else { h.source };
        if !u.active || from != u.from || (ip && h.source != IPI && h.target != IPI) {
            return; // not asked for (an upload this site didn't ask for): ignored
        }
        let d = &self.sd.sds[u.sd];
        if d.what == SD_UPOLL && (h.target != d.src || h.group || h.op != OP_UDTHU) {
            return; // a poll's data goes to its caller (6.6.5.7 a)
        }
        let u = &mut self.sd.upload;
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
        // a poll the polled MS never answered: MSaway
        let reason = if self.sd.sds[i].what == SD_UPOLL { R_MSAWAY } else { R_UDT_CRC };
        self.sd_done(i, reason, 0, WHY_TIMEOUT);
    }

    /// C_ACKU / C_NACKU for a transaction: true when it was one's.
    pub(super) fn sd_ack(&mut self, a: &Ack) -> bool {
        // the caller refusing to upload (6.6.4.2: it may answer C_AHOY from SDMI with C_NACKU)
        if matches!(a.target, SDMI | DGNAI | TATTSI | DIVERTI | IPI) {
            if let Some(i) = (0..SDS).find(|&i| {
                let d = &self.sd.sds[i];
                d.used && d.state == S_UPLOAD && d.src == a.source
            }) {
                self.sd_done(i, a.reason, 0, WHY_REFUSED);
                return true;
            }
            // else the called MS's answer to a download from the gateway (DGNA from the site, source DGNAI)
        }
        // a poll: the polled MS refusing it (6.6.5.7 b), or the caller's answer to the download (6.6.5.5)
        if let Some(i) = (0..SDS).find(|&i| {
            let d = &self.sd.sds[i];
            d.used
                && d.what == SD_UPOLL
                && ((d.state == S_UPLOAD && d.dst == a.source && d.src == a.target)
                    || (d.state == S_DOWN && d.src == a.source && d.dst == a.target))
        }) {
            let d = self.sd.sds[i];
            if d.state == S_UPLOAD && reason_type(a.reason) == 1 {
                return true; // its data comes as an upload, not an acknowledgement
            }
            let why = if reason_type(a.reason) == 1 { WHY_OK } else { WHY_REFUSED };
            self.sd_done(i, a.reason, 0, why);
            return true;
        }
        let Some(i) = (0..SDS).find(|&i| {
            let d = &self.sd.sds[i];
            d.used && d.state == S_DOWN && d.what != SD_UPOLL && d.dst == a.source && d.src == a.target
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
                    if d.what == SD_UPOLL {
                        s.state = S_UPLOAD; // the poll's C_AHOY now
                        s.ahoy_due = true;
                        s.deadline_ms = now + self.cfg.tnp_ms as i64;
                    } else {
                        s.state = S_DOWN;
                        s.next_ms = now;
                    }
                }
                S_HOLD | S_NET | S_UPLOAD if now >= d.deadline_ms => {
                    let (reason, why) = match d.state {
                        S_HOLD => (R_CALLED_PARTY_BUSY, WHY_BUSY),
                        S_NET => (R_MSAWAY, WHY_TIMEOUT),
                        _ if d.what == SD_UPOLL => (R_MSAWAY, WHY_TIMEOUT),
                        _ => (R_UDT_CRC, WHY_TIMEOUT),
                    };
                    self.sd_done(i, reason, 0, why);
                    continue;
                }
                _ => {}
            }
            let d = self.sd.sds[i];
            let progress = matches!(d.state, S_HOLD | S_NET)
                || (d.state == S_DOWN && d.what != SD_UPOLL)
                || (d.state == S_UPLOAD && d.ps_held && d.ahoy_due);
            if d.origin == O_MS && progress && now >= d.next_ack_ms {
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
        if m.what == SD_UDT_POLL {
            return self.app_upoll(m);
        }
        self.sd_new(m, O_APP)
    }

    /// A UDT short data poll from the site (6.6.5.1.5): C_AHOY from SDMI to the polled MS, Service_Kind 0110b; its
    /// C_NACKU ends it, or its upload (target SDMI) is the result's data. False: not an individual MS, none free.
    fn app_upoll(&mut self, m: &SdMsg) -> bool {
        let dst = m.dst;
        let group = m.flags & SD_F_GROUP != 0;
        if group || dst == ADRNULL || is_gateway(dst) || is_allcall(dst) {
            return false;
        }
        let Some(i) = self.sd_free() else { return false };
        let busy = self.sd_busy(dst);
        let mut d = Sd {
            used: true,
            what: SD_UPOLL,
            origin: O_APP,
            state: if busy { S_HOLD } else { S_UPLOAD },
            ahoy_due: !busy,
            so: m.value & 0x0F,
            src: SDMI,
            dst,
            r#ref: m.r#ref,
            tries: self.cfg.ahoy_tries.max(1),
            deadline_ms: self.now + if busy { self.cfg.sd_store_ms } else { self.cfg.tnp_ms } as i64,
            ..Default::default()
        };
        d.head.blocks = m.data().first().copied().unwrap_or(MAX_BLOCKS as u8).clamp(1, MAX_BLOCKS as u8);
        self.sd.sds[i] = d;
        if self.reg_index(dst).is_none() {
            self.sd_done(i, R_NOREG_MSAWAY, 0, WHY_NOT_FOUND); // not here (the network isn't asked)
        }
        true
    }

    /// A message or a result from the network (mqtt-net `sdata`).
    pub fn net_sd_in(&mut self, m: &SdMsg, now_ms: i64) {
        self.now = self.now.max(now_ms);
        if m.what == SD_INTR {
            self.net_intr_in(m);
            return;
        }
        if m.op == SD_RESULT {
            let Some(i) = (0..SDS).find(|&i| {
                let d = &self.sd.sds[i];
                d.used && d.state == S_NET && d.stream == m.r#ref
            }) else {
                return;
            };
            if self.sd.sds[i].what == SD_UPOLL && reason_type(m.reason) == 1 {
                self.upoll_net_answer(i, m);
                return;
            }
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
        if m.what == SD_UDT_POLL {
            self.upoll_net_in(m);
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

    /// A poll from another site of an MS here (5.9): the C_AHOY from the caller and the upload as for a poll on
    /// this site; the data goes back in the result (sd_route), a refusal as the reason.
    fn upoll_net_in(&mut self, m: &SdMsg) {
        if m.flags & SD_F_GROUP != 0 || self.reg_index(m.dst).is_none() {
            return; // not here: another site may have it
        }
        if (0..SDS).any(|i| {
            let d = &self.sd.sds[i];
            d.used && d.what == SD_UPOLL && d.origin == O_NET && d.r#ref == m.stream
        }) {
            return; // a repeat
        }
        let Some(i) = self.sd_free() else {
            let stream = self.new_stream();
            let r = SdMsg { op: SD_RESULT, reason: R_SYSBUSY, why: WHY_REFUSED, r#ref: m.stream, stream, ..*m };
            self.ev(SdMsg { len: 0, value: 0, ..r }.event(EV_NET_SD));
            return;
        };
        let busy = self.sd_busy(m.dst);
        let mut d = Sd {
            used: true,
            what: SD_UPOLL,
            origin: O_NET,
            state: if busy { S_HOLD } else { S_UPLOAD },
            ahoy_due: !busy,
            so: m.value & 0x7F,
            src: m.src,
            dst: m.dst,
            r#ref: m.stream,
            tries: self.cfg.ahoy_tries.max(1),
            deadline_ms: self.now + if busy { self.cfg.sd_store_ms } else { self.cfg.tnp_ms } as i64,
            ..Default::default()
        };
        d.head.blocks = m.data().first().copied().unwrap_or(MAX_BLOCKS as u8).clamp(1, MAX_BLOCKS as u8);
        self.sd.sds[i] = d;
    }

    /// The polled MS's data came back from its site: down to the caller, as a poll's here (6.6.5.1.3).
    fn upoll_net_answer(&mut self, i: usize, m: &SdMsg) {
        let Some((b, pn)) = Blocks::of_format(m.data(), m.value) else {
            self.sd_done(i, R_NOT_SUPPORTED, 0, WHY_REFUSED);
            return;
        };
        let pad_nibble = if pads_nibbles(m.value) { pn + (m.pad_nibble & 1) } else { m.pad_nibble };
        let s = &mut self.sd.sds[i];
        s.blocks = b;
        s.value = m.value;
        s.head = UdtHead { blocks: b.n, pad_nibble, format: m.value, ..Default::default() };
        s.state = S_DOWN;
        s.next_ms = self.now;
        s.tries = self.cfg.sd_tries.max(1);
        s.deadline_ms = self.now + self.cfg.tnp_ms as i64;
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
            let pad_nibble = if pads_nibbles(m.value) { pn + (m.pad_nibble & 1) } else { m.pad_nibble };
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
