// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! An MS simulator for the trunked site (docs/spec/fw/tier3.md section 11): a radio that knows one site and follows
//! ETSI TS 102 361-4 for the services the site supports. It hunts the TSCC, waits for C_ALOHA, makes slotted random
//! accesses with the site's backoff and NRand_Wait, answers C_AHOY, follows grants to the payload channel, talks,
//! and returns on P_CLEAR. Sans-IO like the site: the caller (t3air, or C) moves the blocks and the time.

use core::ffi::c_int;

use crate::t3auth::{response, CHALLENGE_MAX, R_AUTH_RESPONSE, R_MS_NOT_SUPPORTED};
use crate::t3pdrate::{self as pdrate, DataAsm, Taken, CLASS_ACK, CLASS_NACK, CLASS_SACK, TYPE_ACK, TYPE_NACK_CRC};
use crate::t3pdu::*;
use crate::t3rc::RC_CEASE;
use crate::t3site::{
    ahoy_status, bs_dwn_act, mass_reg_applies, Ackvit, R_AUTH_RESPONSE_TS, mass_reg_of, move_of, ps_wake, reg_window_ms, status_rand, DataGrant, DataHead,
    DataMsg, DxGrant, CARRIERS, DEP_ALS, DIV_ON, DPF_CONFIRMED, DPF_RESPONSE, DPF_UNCONFIRMED, MSG_BLOCKS, R_DUPLEX_NOT_SUPPORTED,
    SAP_IP, SK_DUPLEX, SO_SIMI, STATUS_ALS_END, STATUS_INTERRUPT, VOICE_FRAME, VOICE_HEADER, VOICE_TERM,
};
use crate::t3site::{block_len, Mask, BLOCK_MAX, DT_R1, DT_R12, DT_R34, MSG_OCTETS};
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
pub const DO_STATUS: u8 = 7; // a status delivery (act_status)
pub const DO_DUPLEX: u8 = 8; // a full-duplex individual call (6.6.9): both parties talk
/// How long a called MS talks in a duplex call.
const DX_CALLED_TALK_MS: i64 = 2000;
pub const F_EMERGENCY: u8 = 0x01;
pub const F_TO_GROUP: u8 = 0x02; // DO_STATUS: the target is a talkgroup
pub const F_BROADCAST: u8 = 0x04; // DO_GROUP: a broadcast call (BCAST_SV): one way
const F_MASS: u8 = 0x80; // DO_REGISTER: a mass re-registration's (cancelled by a Reg_Window 0)
/// T_Awake (annex A.1): how long it stays awake after a PDU addressed to it, in power save.
const T_AWAKE_MS: i64 = 2000;
pub const F_GROUP: u8 = 0x02; // DO_SDM, DO_SD_STATUS, DO_DATA: arg is a talkgroup (F_TO_GROUP's bit)
pub const DO_SDM: u8 = 9; // UDT short data (send_sdm): arg the MS or talkgroup
pub const DO_SD_STATUS: u8 = 10; // a status by the short data path (talk_ms: the value; 127 polls): arg the MS or talkgroup
pub const DO_DATA: u8 = 11; // a packet data call (send_data): arg the MS, talkgroup or gateway
pub const F_CONFIRMED: u8 = 0x40; // DO_DATA: confirmed data
pub const F_MULTI: u8 = 0x08; // DO_DATA: multi-item (P_MAINT after the item)
pub const F_RATE34: u8 = 0x10; // DO_DATA: rate 3/4 blocks (361-1 8.2.2); neither rate flag: rate 1/2
pub const F_RATE1: u8 = 0x20; // DO_DATA: rate 1 blocks
pub const F_LIST: u8 = 0x10; // DO_REGISTER: with the talkgroup list set by register_list
pub const DO_DGNA: u8 = 12; // assign talkgroups to an MS (send_dgna): arg the MS
pub const DO_DIVERT: u8 = 13; // set or clear a call diversion (divert, divert_clear): arg MSI, GPI, TSI or DIVERTI
pub const F_ALS: u8 = 0x20; // DO_PRIVATE: an Ambient Listening Service call (it listens, the called MS talks)
/// A transmit interrupt from the payload channel it listens on (interrupt, als_end): arg the MS or, F_TO_GROUP, the
/// talkgroup; F_ALS ends an ALS call (status 123), else a payload interrupt (status 125) and it talks talk_ms.
pub const DO_INTERRUPT: u8 = 14;

pub const IDLE: u8 = 0;
pub const ACCESS: u8 = 1;
pub const WAITING: u8 = 2;
pub const PAYLOAD: u8 = 3;

pub const TX_PDU: u8 = 1;
pub const TX_VOICE: u8 = 2;
pub const TX_DATA: u8 = 3; // packet data on a payload channel: dt 6 a header (pdu), 7, 8 or 10 a block (pdu, ext)

const ACTIONS: usize = 8;
/// How soon after a final C_ACKD the same one again is taken as its repeat.
const FINAL_REPEAT_MS: i64 = 300;
const TRIES: u8 = 6; // random access attempts before giving up: NRand_NR (table A.6)
const TRIES_EMERGENCY: u8 = 10; // NRand_NE
const WAIT_MS: i64 = 30_000; // after C_WACKD / C_QACKD / MS_Alerting, the longest wait for the outcome
const HUNT_SLOTS: u32 = 100; // TSCC slots without C_ALOHA before trying the next carrier
const MAINT_REPEATS: u8 = 4;
const TNP_MS: i64 = 10_000; // TNP_Timer: after answering a registration's challenge, the wait for the outcome
const DATA_ACK_MS: i64 = 3000; // confirmed data: the longest wait for the response
const ALS_LIFE_MS: i64 = 10_000; // T_ALS_REQUEST_LIFE_SPAN (table A.1: 1 s to 10 s)
const ALS_MS: i64 = 10_000; // T_ALS, how long an ALS call's called MS transmits (table A.1: 10 s to 300 s)
/// After an accepted payload interrupt, the longest wait for the outbound PDU that says it may transmit (6.4.14.3.3)
const INTR_WAIT_MS: i64 = 6000;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct MsTx {
    pub ch: u8,
    pub kind: u8,
    pub dt: u8,
    pub what: u8,
    pub pdu: [u8; 10],
    pub group: u8,
    pub offset: u8, // offset timing (a duplex call): sent in the other inbound slot of ch's carrier
    pub src: u32,
    pub dst: u32,
    pub ext: [u8; 14], // a data block (dt 7, 8, 10): its octets 10 on (to 11, 17 or 23)
    pub so: u8,        // voice: the LC's Service Options (361-2 table 7.11)
    pub pad2: u8,
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
    mask: u8,         // the population subdivision (6.2.1.1.1)
    aloha_addr: u32,  // its address
    service_fn: u8,   // the services invited (6.2.1.1.2, table 6.1)
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
    rand_sent: bool, // the current request's C_RAND has gone at least once
    last_final: (u8, u32, u8), // the last request's final C_ACKD: reason, source, Response_Info
    last_final_ms: i64,
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
    cease_left: u8, // told to cease (RC): bursts left of its superframe; 0 none
    ceasing: bool,
    // counters
    last_reason: u8,
    grants: u32,
    calls_done: u32,
    calls_failed: u32,
    ahoys: u32,
    clears: u32,
    ceased: u32,
    // authentication (361-4 6.4.8): its key K and PSN
    has_key: bool,
    key: [u8; 16],
    psn: Option<u32>,
    challenges: u32,
    statuses: u32,          // statuses delivered to it (C_AHOY, Service_Kind Status)
    last_status: (u32, u32, u8), // the last one's source, target and value
    last_status_ms: i64,
    // power save (6.4.7) and mass re-registration (6.4.5)
    ps_rq: u8,
    ps_off: u8, // PowerSave_Offset granted; 0: awake all the time
    awake_until: i64,
    mass_until: i64,
    mass_regs: u32,
    bcasts: [u32; 8], // C_BCASTs heard, by Announcement_type
    // the asynchronous TSCC (6.1.1.2) and C_MOVE (6.3.3.1 e)
    site_hibernates: bool, // Gen_Site_Params said so: no C_ALOHA is no reason to hunt
    tscc_rev: bool, // its TSCC's CACH says P_SYS_Parms, composite (5.3.2): the TSCC carries a call, it comes back
    next_activation_ms: i64,
    activations: u32,
    moves: u32,
    // full-duplex calls (6.6.9)
    no_duplex: bool, // answers a duplex radio check C_NACKU(MS_Duplex_Not_Supported)
    dx: bool,        // in a duplex call: offset timing, talks while it listens
    dx_peer: u32,
    voice_heard: u32,
    // short data
    sd_out: Blocks,
    sd_fmt: u8,
    sd_pn: u8, // its Pad_Nibble
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
    sd_status: u8,
    sd_status_src: u32,
    sd_statuses: u32,
    poll_result: u8,
    // packet data
    pd_out: DataMsg,
    pd_pos: u8, // 0: the header next, then the blocks
    pd_sending: bool,
    pd_wait_ack: bool,
    pd_until: i64,
    pd_multi: bool, // the channel's grant is multi-item
    /// A SACK's C_RDATA block, after pd_resp.
    pd_resp_blk: Option<pdrate::RdataOut>,
    pd_send: Mask,    // the blocks of pd_out this transmission sends (all, or a SACK's)
    pd_tries: u8,    // its sends so far
    pd_rdata: pdrate::RdataIn, // a SACK's C_RDATA blocks being taken
    pd_asm: DataAsm, // the message it takes
    pd_inbox: DataMsg,
    pd_inbox_n: u32,
    pd_resp: Option<[u8; 10]>,
    pd_leave: bool, // single item: back to the TSCC after the response goes
    // talkgroup lists
    reg_list: Blocks, // the subscription list a DO_REGISTER with F_LIST uploads
    reg_info: u8,     // the Response_Info of the last registration's C_ACK
    dgna: [u32; 16],  // the talkgroups DGNA gave it (fifteen by address, the sixteenth by alias)
    dgna_n: u32,
    // supplementary services
    stunned: bool,
    no_stun: bool, // answers stun/revive MSNot_Supported
    stun_auth: bool, // authenticates the TSCC before a stun or revive (6.4.9.2); kill always does (6.4.10)
    vit: u8,         // the C_ACKVIT it sent: 0 none, 1 stun, 2 revive, 3 kill
    vit_rand: u32,   // its challenge
    vits: u32,       // C_ACKVITs sent
    killed: bool,    // 6.4.10: no DMR functionality at all
    no_als: bool,  // answers an ALS radio check MSNot_Supported
    als_from: u32, // the caller of an ALS radio check it accepted
    als_until: i64,
    als_on: bool,       // on the payload channel of an ALS call, transmitting for the caller
    diverted: u32,      // the last diverted address a request of its own got (6.6.7.2)
    diverted_src: u32,  // MSI or GPI
    diverted_n: u32,
    // transmit interrupt (361-4 6.4.14.3.3, 6.4.14.3.4)
    intr_ch: usize,     // the payload channel it left to ask
    intr_away: bool,    // on the TSCC for an interrupt request, back to intr_ch after the answer
    intr_wait: bool,    // accepted: waits for P_PROTECT(EN_PTT_ONE_MS) to it
    intr_until: i64,
    intr_talk: u32,     // then talks this long
    intr_heard: u32,    // voice_heard when it came back
    intr_talking: bool, // talks after an interrupt (not its own call: no P_MAINT DISCON after)
    intr_ok: u32,       // interrupts that got it the channel
    intr_lost: u32,     // accepted ones that didn't
    protects: u32,      // P_PROTECTs heard on its payload channel
    protects_other: u32, // of them EN_PTT_ONE_MS to another MS: its PTT disabled meanwhile (6.6.2.3.2.3)
    rc_deaf: bool,
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

    /// Its authentication key and PSN (6.4.8.1); None: no key (it answers a challenge MSNot_Supported).
    pub fn set_key(&mut self, key: Option<&[u8; 16]>, psn: Option<u32>) {
        self.has_key = key.is_some();
        self.key = key.copied().unwrap_or_default();
        self.psn = psn;
    }

    /// Authentication challenges it answered.
    pub fn challenges(&self) -> u32 {
        self.challenges
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

    /// A status delivery (361-4 6.6.6.1.2) to an MS or, with group, a talkgroup; 126 is the emergency alarm.
    pub fn act_status(&mut self, status: u8, group: bool, target: u32, at_ms: i64) -> bool {
        let flags = if group { F_TO_GROUP } else { 0 };
        self.act(DO_STATUS, target, u32::from(status & 0x7F), flags, at_ms)
    }

    /// A payload interrupt (361-4 6.4.14.3.3) of the call it listens to: status 125 to target (the talkgroup with
    /// group, or the MS to interrupt), then back on the payload channel it talks talk_ms once the site lets it.
    pub fn interrupt(&mut self, target: u32, group: bool, talk_ms: u32, at_ms: i64) -> bool {
        self.act(DO_INTERRUPT, target, talk_ms, if group { F_TO_GROUP } else { 0 }, at_ms)
    }

    /// The end of the ALS call it listens to (361-4 6.4.14.3.4): status 123 to the called MS; it stays on the TSCC.
    pub fn als_end(&mut self, target: u32, at_ms: i64) -> bool {
        self.act(DO_INTERRUPT, target, 0, F_ALS, at_ms)
    }

    /// Payload interrupts: (got the channel, accepted but didn't), and the P_PROTECTs heard (all, to another MS).
    pub fn interrupts(&self) -> (u32, u32, u32, u32) {
        (self.intr_ok, self.intr_lost, self.protects, self.protects_other)
    }

    /// Statuses delivered to it, and the last one (source, target, value).
    pub fn statuses(&self) -> (u32, (u32, u32, u8)) {
        (self.statuses, self.last_status)
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
        let Some((b, pn)) = Blocks::of_format(data, format) else { return false };
        self.sd_out = b;
        self.sd_fmt = format;
        self.sd_pn = pn;
        self.act(DO_SDM, dst, 0, flags, at_ms)
    }

    /// Queues a packet data call with one message (unconfirmed, or confirmed with F_CONFIRMED; SAP 4) to dst, in
    /// rate 1/2 blocks or, F_RATE34 / F_RATE1, rate 3/4 or rate 1; false when the data doesn't fit.
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
        let dt = if flags & F_RATE34 != 0 {
            DT_R34
        } else if flags & F_RATE1 != 0 {
            DT_R1
        } else {
            DT_R12
        };
        let Some(m) = DataMsg::build_rate(head, data, dt) else { return false };
        self.pd_out = m;
        self.act(DO_DATA, dst, 0, flags, at_ms)
    }

    /// The last packet data message it took: the header, the data and its length, and how many it took.
    pub fn data_inbox(&self) -> Option<(DataHead, [u8; MSG_OCTETS], usize, u32)> {
        if self.pd_inbox_n == 0 {
            return None;
        }
        let mut d = [0u8; MSG_OCTETS];
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
        let b = if format == FMT_ADDRESS {
            AddrList::blocks(ok, tgs).map(|b| (b, 0))
        } else {
            Blocks::of_format(data, format)
        };
        let Some((b, pn)) = b else { return false };
        self.sd_out = b;
        self.sd_fmt = format;
        self.sd_pn = pn;
        self.act(DO_DGNA, dst, 0, 0, at_ms)
    }

    /// The talkgroups DGNA gave it, and how many transfers it took.
    pub fn dgna_tgs(&self) -> ([u32; 16], u32) {
        (self.dgna, self.dgna_n)
    }

    /// Which supplementary services it doesn't support (it answers C_NACKU MSNot_Supported): stun/revive, ALS.
    pub fn refuse_supp(&mut self, stun: bool, als: bool) {
        self.no_stun = stun;
        self.no_als = als;
    }

    /// Stunned by the site (6.4.9): no user services until revived.
    pub fn stunned(&self) -> bool {
        self.stunned
    }

    /// Whether it authenticates the TSCC before a stun or revive (6.4.9.2; needs a key). Kill always does.
    pub fn set_stun_auth(&mut self, on: bool) {
        self.stun_auth = on;
    }

    /// Killed by the site (6.4.10): it hears and sends nothing more.
    pub fn killed(&self) -> bool {
        self.killed
    }

    /// C_ACKVITs it sent (its challenges to the TSCC).
    pub fn ackvits(&self) -> u32 {
        self.vits
    }

    /// Queues a call diversion of the kinds (DIV_* bits) to an MS, or a talkgroup with group (6.6.7.1).
    pub fn divert(&mut self, to: u32, group: bool, kinds: u8, at_ms: i64) -> bool {
        let Some(b) = AddrList::blocks(false, &[to]) else { return false }; // OK 0: DGNA only (7.2.36)
        self.sd_out = b;
        self.sd_fmt = FMT_ADDRESS;
        self.sd_pn = 0;
        let gw = if group { GPI } else { MSI };
        self.act(DO_DIVERT, gw, u32::from(DIV_ON | (kinds & 0x0F)), 0, at_ms)
    }

    /// Queues clearing its own diversions of the kinds, or (incoming) the ones that point at it (6.6.7.1.2).
    pub fn divert_clear(&mut self, incoming: bool, kinds: u8, at_ms: i64) -> bool {
        self.act(DO_DIVERT, if incoming { DIVERTI } else { TSI }, u32::from(kinds & 0x0F), 0, at_ms)
    }

    /// The last diverted address its requests got: (MSI or GPI, the address, how many).
    pub fn diverted(&self) -> (u32, u32, u32) {
        (self.diverted_src, self.diverted, self.diverted_n)
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
    pub fn sd_statuses(&self) -> (u32, u8, u32, u8) {
        (self.sd_status_src, self.sd_status, self.sd_statuses, self.poll_result)
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
                service_options: SO_REG | (self.ps_rq & 7) << 1,
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
                service_options: emergency | if a.flags & F_BROADCAST != 0 { SO_BROADCAST } else { 0 },
                service_kind: SK_TG_VOICE,
                target: a.arg,
                source: self.id,
                ..Default::default()
            },
            DO_PRIVATE => Rand {
                service_options: emergency,
                dep: if a.flags & F_ALS != 0 { DEP_ALS } else { 0 }, // table 6.40: ALS_SERV
                service_kind: SK_IND_VOICE,
                target: a.arg,
                source: self.id,
                ..Default::default()
            },
            DO_DUPLEX => Rand {
                service_options: emergency,
                service_kind: SK_DUPLEX,
                target: a.arg,
                source: self.id,
                ..Default::default()
            },
            DO_ANSWER => Rand { service_kind: SK_CALL_ANSWER, target: a.arg, source: self.id, ..Default::default() },
            DO_STATUS => status_rand(a.talk_ms as u8, a.flags & F_TO_GROUP != 0, a.arg, self.id),
            // 6.4.14.3.3.2, 6.4.14.3.4.3: a status call, 125 (payload interrupt) or 123 (ALS termination)
            DO_INTERRUPT => {
                let st = if a.flags & F_ALS != 0 { STATUS_ALS_END } else { STATUS_INTERRUPT };
                status_rand(st, a.flags & F_TO_GROUP != 0, a.arg, self.id)
            }
            // 6.6.4.2 table 6.55: Appended_UDT_Short_Data the blocks less one
            DO_SDM => Rand {
                dep: self.sd_out.n.max(1) - 1,
                service_kind: if a.flags & F_GROUP != 0 { SK_TG_UDT } else { SK_IND_UDT },
                target: a.arg,
                source: self.id,
                ..Default::default()
            },
            // 6.6.6.1.2 table 6.57: G/I, then the status's five MSBs; its two LSBs in the dependent bits
            DO_SD_STATUS => {
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
            // 6.6.7.1.2 tables 6.61, 6.64: Service_Options DIVONOFF and the kinds; SDATA_VAL the blocks less one
            DO_DIVERT => {
                let so = a.talk_ms as u8;
                let set = so & DIV_ON != 0;
                Rand {
                    service_options: so,
                    dep: if set { self.sd_out.n.max(1) - 1 } else { 0 },
                    service_kind: SK_DIVERT,
                    target: a.arg,
                    source: self.id,
                    ..Default::default()
                }
            }
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
        if self.stunned && !matches!(a.what, DO_REGISTER | DO_DEREGISTER) {
            self.calls_failed += 1; // 6.4.9: no user services while stunned
            return;
        }
        if self.has_cur && a.what == DO_CANCEL {
            self.calls_failed += 1; // the call it cancels
        }
        self.cur = a;
        self.has_cur = true;
        self.up_len = 0;
        self.up_pos = 0;
        self.pdu = self.request_pdu(&a);
        self.tries = 0;
        self.rand_sent = false;
        self.wait = 1;
        self.resp_wait = 0;
        self.state = ACCESS;
    }

    fn done(&mut self, ok: bool) {
        if self.has_cur && matches!(self.cur.what, DO_GROUP | DO_PRIVATE | DO_STATUS | DO_DUPLEX | DO_SDM | DO_SD_STATUS | DO_DATA | DO_DGNA | DO_DIVERT | DO_INTERRUPT) {
            if ok {
                self.calls_done += 1;
            } else {
                self.calls_failed += 1;
            }
        }
        let intr = self.has_cur && self.cur.what == DO_INTERRUPT;
        self.has_cur = false;
        self.state = IDLE;
        self.ch = self.tscc_ch;
        if intr && self.intr_away {
            self.intr_back(ok);
        }
    }

    /// The TSCC answered its interrupt request (or didn't). A payload interrupt: "the interrupting MS shall then
    /// return to the payload channel in either case" (6.4.14.3.3); accepted, it waits for leave to talk. An ALS
    /// termination: "the terminating MS shall remain on the TSCC" (6.4.14.3.4.3).
    fn intr_back(&mut self, ok: bool) {
        self.intr_away = false;
        if self.cur.flags & F_ALS != 0 {
            return;
        }
        self.state = PAYLOAD;
        self.ch = self.intr_ch;
        self.talking = false;
        self.header_sent = false;
        self.intr_wait = ok;
        self.intr_until = self.now + INTR_WAIT_MS;
        self.intr_talk = self.cur.talk_ms;
        self.intr_heard = self.voice_heard;
    }

    /// In a call on a payload channel, listening: an interrupt request that is due takes it to the TSCC
    /// (6.4.14.3.3.2: "The interrupting MS shall return to the TSCC").
    fn intr_leave(&mut self) {
        if self.intr_wait && self.now >= self.intr_until {
            // no leave to talk: the talker kept the channel (6.4.15.1), or the call ended while it was away
            self.intr_wait = false;
            self.intr_lost += 1;
            if self.voice_heard == self.intr_heard {
                self.back_to_tscc();
                return;
            }
        }
        let listening = !self.talking && self.maint_left == 0 && !self.dx && !self.pd_sending && !self.pd_wait_ack;
        if !listening || self.intr_wait || self.stunned {
            return;
        }
        let Some(a) = self.take_due(|a| a.what == DO_INTERRUPT) else { return };
        if self.mine && self.has_cur {
            self.calls_done += 1; // its own call went; it now asks as a listener
        }
        self.mine = false;
        self.has_cur = false;
        self.intr_ch = self.ch;
        self.intr_away = true;
        self.state = IDLE;
        self.ch = self.tscc_ch;
        self.start(a);
    }

    /// P_PROTECT on its payload channel (6.6.2.3.2.3). EN_PTT_ONE_MS to it after an accepted interrupt: it talks.
    fn on_protect(&mut self, p: &Protect) {
        self.protects += 1;
        if p.kind != PROTECT_EN_PTT_ONE_MS {
            return;
        }
        if p.target != self.id {
            self.protects_other += 1;
            return;
        }
        if self.intr_wait && !self.talking {
            self.intr_wait = false;
            self.intr_ok += 1;
            self.talking = true;
            self.intr_talking = true;
            self.header_sent = false;
            self.ceasing = false;
        }
    }

    fn back_to_tscc(&mut self) {
        self.dx = false;
        self.state = IDLE;
        self.ch = self.tscc_ch;
        self.talking = false;
        self.maint_left = 0;
        self.pd_sending = false;
        self.pd_wait_ack = false;
        self.pd_rdata.stop();
        self.pd_asm = DataAsm::default();
        self.pd_resp = None;
        self.pd_resp_blk = None;
        self.pd_leave = false;
        self.als_on = false;
        if self.intr_wait {
            self.intr_lost += 1;
        }
        self.intr_wait = false;
        self.intr_talking = false;
    }

    /// Outbound slot k on channel ch (a block when dt isn't 0); at: the CACH's AT bit for the next slot.
    pub fn rx(&mut self, ch: usize, k: i64, dt: u8, pdu: &[u8; 10], at: bool) {
        let mut b = [0u8; BLOCK_MAX];
        b[..10].copy_from_slice(pdu);
        self.rx_block(ch, k, dt, &b, at);
    }

    /// rx with a whole block (a data block's 12, 18 or 24 octets as its data type; a PDU's first 10).
    pub fn rx_block(&mut self, ch: usize, k: i64, dt: u8, blk: &[u8; BLOCK_MAX], at: bool) {
        let mut b12 = [0u8; 12];
        b12.copy_from_slice(&blk[..12]);
        let mut p = [0u8; 10];
        p.copy_from_slice(&blk[..10]);
        let pdu = &p;
        if ch != self.ch || self.killed {
            return;
        }
        if self.state == PAYLOAD {
            if dt == DT_DATA_HDR || block_len(dt) > 0 {
                self.rx_data(ch, dt, blk);
                return;
            }
            if dt == DT_CSBK {
                if let Some(p) = Protect::decode(pdu) {
                    self.on_protect(&p);
                    return;
                }
            }
            if dt == DT_CSBK && Clear::decode(pdu).is_some() {
                if self.rc_deaf && self.talking && !self.dx {
                    // a real MS hears only the other slot while it transmits (the RC's, 361-1 5.1.5); the
                    // simulated one hears its own too, except one modeled without the RC
                    return;
                }
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
        if self.asleep(k) {
            return; // power save: hears nothing outside its wake frame
        }
        self.at = at;
        match (dt == DT_CSBK).then(|| Aloha::decode(pdu)).flatten() {
            Some(a) => {
                self.have_aloha = true;
                self.no_aloha = 0;
                self.syscode = a.syscode;
                self.nrand = a.nrand_wait;
                self.backoff = a.backoff;
                self.mask = a.mask;
                self.aloha_addr = a.addr;
                self.service_fn = a.service_function;
            }
            None => self.no_aloha += 1,
        }
        if self.no_aloha > HUNT_SLOTS && self.state == IDLE && !self.site_hibernates && !self.tscc_rev {
            // no TSCC here: the next carrier
            let c = (self.tscc_ch / 2 + 1) % self.ncarriers;
            self.tscc_ch = c * 2;
            self.ch = self.tscc_ch;
            self.no_aloha = 0;
            self.have_aloha = false;
            return;
        }
        if dt == DT_DATA_HDR || dt == DT_RATE12 {
            self.on_udt(dt, &b12);
            return;
        }
        if dt != DT_CSBK {
            return;
        }
        if let Some(a) = Ack::decode(pdu) {
            if a.op == OP_ACKD && a.target == self.id && a.reason == R_AUTH_RESPONSE_TS && self.vit != 0 {
                self.on_vit_response(&a);
            } else if a.op == OP_ACKD && a.target == self.id {
                self.on_ack(&a);
            }
        } else if let Some(a) = Ahoy::decode(pdu) {
            if a.target == self.id || (a.group && a.service_kind == SK_STATUS && self.my_tg(a.target)) {
                self.on_ahoy(&a);
            }
        } else if let Some(g) = Grant::decode(pdu) {
            self.on_grant(&g);
        } else if let Some(g) = DxGrant::decode(pdu) {
            self.on_dx_grant(&g);
        } else if let Some(b) = Bcast::decode(pdu) {
            self.on_bcast(&b);
        } else if let Some((mask, chan, addr)) = move_of(pdu) {
            // 6.3.3.1 e): leave for the TSCC on chan (one it knows; LCN 0)
            if self.state == IDLE && mass_reg_applies(mask, addr, self.id) {
                if let Some(c) = self.chan_index(chan, 0) {
                    self.moves += 1;
                    self.tscc_ch = c;
                    self.ch = c;
                    self.no_aloha = 0;
                    self.have_aloha = false;
                }
            }
        } else if let Some(g) = DataGrant::decode(pdu) {
            self.on_data_grant(&g);
        }
    }

    /// Power save: idle, not within T_Awake of a PDU for it, and outside its wake frame.
    fn asleep(&self, k: i64) -> bool {
        self.ps_off != 0
            && self.state == IDLE
            && !self.has_cur
            && self.now >= self.awake_until
            && !ps_wake(k, self.ps_rq, self.ps_off)
    }

    fn on_bcast(&mut self, b: &Bcast) {
        self.bcasts[usize::from(b.ann & 7)] += 1;
        if b.ann == BC_GEN_SITE_PARAMS {
            self.site_hibernates = b.parms2 >> 22 & 1 != 0; // Hibernating_Flag (7.2.40)
        }
        if b.ann != BC_MASS_REG {
            return;
        }
        // 6.4.5.1: a random time within Treg_Window, then a registration; Reg_Window 0 cancels
        let (w, mask, addr) = mass_reg_of(b);
        if !mass_reg_applies(mask, addr, self.id) {
            return;
        }
        if w == 0 {
            self.mass_until = 0;
            let n = self.nact;
            for i in (0..n).rev() {
                if self.actions[i].what == DO_REGISTER && self.actions[i].flags & F_MASS != 0 {
                    for j in i..self.nact - 1 {
                        self.actions[j] = self.actions[j + 1];
                    }
                    self.nact -= 1;
                }
            }
            return;
        }
        if self.now < self.mass_until {
            return; // e): one in its window is ignored
        }
        let t = i64::from(reg_window_ms(w));
        let at = self.now + i64::from(self.rand()) % (t + 1);
        self.mass_until = self.now + t;
        self.mass_regs += 1;
        if self.cur.what == DO_REGISTER && self.state != IDLE
            || self.actions[..self.nact].iter().any(|x| x.what == DO_REGISTER)
        {
            return; // a registration on its way answers it (its window still set: the repeats are ignored)
        }
        self.act(DO_REGISTER, 0, 0, F_MASS, at); // its subscription stays
    }

    /// Asks for power save 1:2^rq (1-7; 0 none) in its next registrations.
    pub fn set_power_save(&mut self, rq: u8) {
        self.ps_rq = rq.min(7);
    }

    /// PowerSave_RQ asked and the PowerSave_Offset granted (0: none).
    pub fn power_save(&self) -> (u8, u8) {
        (self.ps_rq, self.ps_off)
    }

    /// Mass re-registrations it has taken part in, and the C_BCASTs it heard of one Announcement_type.
    pub fn mass_regs(&self) -> u32 {
        self.mass_regs
    }
    pub fn bcasts(&self, ann: u8) -> u32 {
        self.bcasts[usize::from(ann & 7)]
    }

    /// BS_Dwn_Acts sent to a hibernating TSCC, and C_MOVEs followed.
    pub fn activations(&self) -> (u32, u32) {
        (self.activations, self.moves)
    }

    /// A data channel grant (6.6.3.2.7): its own call, a call to it, or (idle) one to its talkgroup.
    fn on_data_grant(&mut self, g: &DataGrant) {
        if self.stunned {
            return;
        }
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
        self.pd_send = Mask::of(self.pd_all());
        self.pd_tries = 0;
        self.pd_wait_ack = false;
        self.pd_rdata.stop();
        self.pd_asm = DataAsm::default();
        self.pd_resp = None;
        self.pd_resp_blk = None;
        self.pd_leave = false;
        self.pd_multi = matches!(g.op, crate::t3site::OP_PD_GRANT_MI | crate::t3site::OP_TD_GRANT_MI);
        self.pay_group = g.group();
        self.pay_src = g.source;
        self.pay_dst = g.target;
    }

    /// A data header or block (rate 1/2, 3/4 or 1) on its payload channel (the site's repeat): its own call's
    /// response, or a message for it or its talkgroup (a confirmed one answered: 361-1 8.2.2.3, 361-3 5.4.2.2).
    pub fn rx_data(&mut self, ch: usize, dt: u8, blk: &[u8; BLOCK_MAX]) {
        if self.state != PAYLOAD || ch != self.ch {
            return;
        }
        if dt == DT_DATA_HDR {
            let mut p = [0u8; 10];
            p.copy_from_slice(&blk[..10]);
            self.pd_rdata.stop();
            let Some(h) = DataHead::decode(&p) else { return };
            if h.src == self.id {
                return; // its own, repeated
            }
            if h.dpf == DPF_RESPONSE {
                if h.dst == self.id && self.pd_wait_ack {
                    self.pay_src = h.src;
                    self.pay_dst = h.dst;
                    self.on_response(&h);
                }
                return;
            }
            if h.dst == self.id || (h.group && self.my_tg(h.dst)) {
                if self.pd_asm.header(&h) {
                    self.pay_src = h.src;
                }
            } else {
                self.pd_asm.left = 0;
            }
            return;
        }
        if self.pd_rdata.on() && dt == DT_R12 {
            // a SACK's C_RDATA (one or two): the blocks to send again (361-3 5.4.1)
            match self.pd_rdata.block(&blk[..12], self.pd_out.n) {
                Some(0) => self.pd_rdata.stop(),
                Some(retry) => self.resend(retry),
                None => {}
            }
            return;
        }
        let Some(taken) = self.pd_asm.block(dt, blk) else { return };
        let h = self.pd_asm.m.head;
        let answer = h.dpf == DPF_CONFIRMED && h.response && !h.group;
        if taken == Taken::Ok && !(self.pd_inbox_n > 0 && self.pd_inbox == self.pd_asm.m) {
            self.pd_inbox = self.pd_asm.m;
            self.pd_inbox_n += 1;
        }
        if answer {
            let (r, rd) = pdrate::response(&h, self.id, taken, self.pd_asm.m.n);
            self.pd_resp = Some(r.encode());
            self.pd_resp_blk = rd;
            self.pd_leave = taken == Taken::Ok && !self.pd_multi && !self.mine;
        } else if taken == Taken::Ok && !self.pd_multi && !self.mine {
            self.back_to_tscc(); // 6.6.3.3.2.5 b): a single item taken
        }
    }

    fn pd_all(&self) -> u128 {
        crate::t3site::blocks_mask(self.pd_out.n)
    }

    /// The response to its confirmed message (361-1 table 8.3): ACK; SACK, its C_RDATA next; NACK for the message's
    /// CRC, all again; another NACK ends it.
    fn on_response(&mut self, h: &DataHead) {
        match (h.class, h.rtype) {
            (CLASS_ACK, TYPE_ACK) => {
                self.pd_wait_ack = false;
                self.item_done();
            }
            (CLASS_SACK, _) if h.blocks > 0 => self.pd_rdata.start(h.blocks),
            (CLASS_NACK, TYPE_NACK_CRC) => self.resend(self.pd_all()),
            _ => {
                self.pd_wait_ack = false;
                self.mine = false;
                self.done(false);
                self.back_to_tscc();
            }
        }
    }

    /// Sends the blocks in mask again (all of them: a C_HEAD with F = 1; some: F = 0), up to pdrate::TRIES sends.
    fn resend(&mut self, mask: u128) {
        if self.pd_tries + 1 >= pdrate::TRIES {
            self.pd_wait_ack = false;
            self.mine = false;
            self.done(false);
            self.back_to_tscc();
            return;
        }
        self.pd_tries += 1;
        self.pd_send = Mask::of(mask & self.pd_all());
        self.pd_pos = 0;
        self.pd_wait_ack = false;
        self.pd_sending = true;
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
        if self.stunned {
            return;
        }
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
        if h.sf && !h.group && h.op == OP_UDTHD && matches!(h.source, MSI | GPI) {
            // 6.6.7.2: the called MS diverted what it asked for; the request ends
            let mut d = [0u8; MAX_DATA];
            let n = self.in_blocks.data(&mut d);
            let asked = self.has_cur && matches!(self.state, ACCESS | WAITING);
            if asked && matches!(self.cur.what, DO_PRIVATE | DO_SDM | DO_STATUS | DO_SD_STATUS | DO_DATA) {
                self.diverted = AddrList::of(&d[..n]).a[0];
                self.diverted_src = h.source;
                self.diverted_n += 1;
                self.done(false);
            }
            return;
        }
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
                self.dgna[..15].fill(0); // 6.6.8.1.1: the fifteen; the sixteenth (6.6.8.1.2) stays
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
        self.awake_until = self.now + T_AWAKE_MS;
        if self.has_cur && self.state == ACCESS && !self.rand_sent {
            return; // a repeat of the last request's final acknowledgement: this one hasn't been asked yet
        }
        if (a.reason, a.source, a.response_info) == self.last_final && self.now - self.last_final_ms < FINAL_REPEAT_MS {
            return; // the site repeats a final acknowledgement (361-4 figures 6.57, 6.62): taken once
        }
        if reason_type(a.reason) != 2 && reason_type(a.reason) != 3 && a.reason != R_MS_ALERTING {
            self.last_final = (a.reason, a.source, a.response_info);
            self.last_final_ms = self.now;
        }
        self.last_reason = a.reason;
        if !self.has_cur {
            return; // e.g. the called party told of its own refusal
        }
        if matches!(self.cur.what, DO_SDM | DO_SD_STATUS) && a.source != self.cur.arg {
            return; // not about this request (a repeat of an earlier one's final acknowledgement)
        }
        if self.cur.what == DO_INTERRUPT && a.source != TSI {
            return; // 6.4.14.3.3: the answer comes from TSI
        }
        if self.cur.what == DO_DGNA && a.source != self.cur.arg && a.source != DGNAI {
            return;
        }
        if self.cur.what == DO_DIVERT && a.source != self.cur.arg {
            return; // C_ACKD comes from the C_RAND's target (6.6.7.1.1.2)
        }
        let upload = matches!(self.cur.what, DO_SDM | DO_DGNA)
            || (self.cur.what == DO_DIVERT && self.cur.talk_ms as u8 & DIV_ON != 0);
        if upload && reason_type(a.reason) == 1 && self.up_len == 0 {
            return; // a final answer before its upload: a repeat of the last one's
        }
        if a.reason == R_POLL_MS && matches!(self.cur.what, DO_STATUS | DO_SD_STATUS) {
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
                    // 6.4.7.2.1: the offset in Response_Info; 0, power save not available
                    self.ps_off = if self.registered && self.ps_rq != 0 { a.response_info & 0x7F } else { 0 };
                    self.done(true);
                }
                R_CALLBACK => self.done(false),
                _ => self.done(true),
            },
            _ => {
                if a.reason == R_MS_NOT_REGISTERED || a.reason == R_REG_DENIED || a.reason == R_REG_REFUSED {
                    self.registered = false;
                }
                // 6.4.4.1.0: told it must register (MS_Not_Registered, 7.2.6), it registers by random access
                let pending = self.actions[..self.nact].iter().any(|x| x.what == DO_REGISTER);
                if a.reason == R_MS_NOT_REGISTERED && self.cur.what != DO_REGISTER && !pending {
                    self.act(DO_REGISTER, 0, 0, 0, self.now);
                }
                self.done(false);
            }
        }
    }

    fn on_ahoy(&mut self, a: &Ahoy) {
        self.awake_until = self.now + T_AWAKE_MS;
        if (a.source == STUNI || a.source == KILLI) && a.service_kind == SK_SUPPL {
            let kill = a.source == KILLI;
            if !self.no_stun && self.has_key && (kill || self.stun_auth) {
                // 6.4.9.2.2 table 6.21, 6.4.10.2 table 6.25: it challenges the TSCC with C_ACKVIT
                let rand = self.rand() % (CHALLENGE_MAX + 1);
                self.vit = if kill { 3 } else if a.service_kind_flag { 2 } else { 1 };
                self.vit_rand = rand;
                self.vits += 1;
                let flag = a.service_kind_flag && !kill;
                let v = Ackvit { service_kind_flag: flag, service_kind: SK_SUPPL, target: rand, source: self.id, ..Default::default() };
                self.reply = Some(v.encode());
                return;
            }
            // 6.4.9.1 table 6.19: Service_Kind_Flag 0 stun, 1 revive; Message_Accepted, or MSNot_Supported (a kill
            // it can't authenticate too: 6.4.10.2)
            let reason = if self.no_stun || kill { R_MS_NOT_SUPPORTED } else { R_MESSAGE_ACCEPTED };
            if reason == R_MESSAGE_ACCEPTED {
                self.stunned = !a.service_kind_flag;
            }
            let r = Ack { op: OP_ACKU, reason, target: a.source, source: self.id, ..Default::default() };
            self.reply = Some(r.encode());
            return;
        }
        if self.stunned {
            return; // it takes no user services (6.4.9)
        }
        if a.als && a.service_kind == SK_IND_VOICE && !a.group {
            // 6.6.2.6.2 table 6.47: an ALS radio check; accepted, it transmits on a matching grant in time
            let reason = if self.no_als { R_MS_NOT_SUPPORTED } else { R_MESSAGE_ACCEPTED };
            if !self.no_als {
                self.als_from = a.source;
                self.als_until = self.now + ALS_LIFE_MS;
            }
            self.ahoys += 1;
            let r = Ack { op: OP_ACKU, reason, target: a.source, source: self.id, ..Default::default() };
            self.reply = Some(r.encode());
            return;
        }
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
        if a.service_kind == SK_REG && !a.service_kind_flag {
            self.on_challenge(a);
            return;
        }
        // an upload asked for: short data (6.6.4.4 c, table 6.35), DGNA (6.6.8.3.3, table 6.71) or a
        // registration's talkgroup list (6.4.4.1.13.2: to TATTSI, A = 0, SF = 1, the Address Format)
        let upload = match (a.source, a.service_kind) {
            (SDMI, SK_IND_UDT | SK_TG_UDT) => Some(DO_SDM),
            (DGNAI, SK_SUPPL) => Some(DO_DGNA),
            (TATTSI, SK_REG) if a.service_kind_flag => Some(DO_REGISTER),
            (DIVERTI, SK_DIVERT) => Some(DO_DIVERT),
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
                DO_DIVERT => {
                    // table 6.63: to MSI or GPI, the Address Format, A 0, SF 0, UDT_DIV 1
                    let h = UdtHead {
                        op: OP_UDTHU,
                        option: true,
                        format: FMT_ADDRESS,
                        target: self.cur.arg,
                        source: self.id,
                        blocks: self.sd_out.n,
                        ..Default::default()
                    };
                    (h, self.sd_out)
                }
                _ => {
                    let h = UdtHead {
                        op: if what == DO_DGNA { OP_DGNAHU } else { OP_UDTHU },
                        group: self.cur.flags & F_GROUP != 0,
                        response: what == DO_DGNA,
                        format: self.sd_fmt,
                        pad_nibble: self.sd_pn,
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
        let ack = |reason, s: &Ms| Ack { op: OP_ACKU, response_info: 0, reason, target: a.source, source: s.id }.encode();
        if a.service_kind == SK_STATUS {
            if ahoy_status(a) == STATUS_POLL {
                // 6.6.6.2.2.1.5: a status poll, answered with its status in Response_Info
                let info = self.status_reply;
                let r = Ack { op: OP_ACKU, response_info: info, reason: R_POLL_MS, target: a.source, source: self.id };
                self.reply = Some(r.encode());
                return;
            }
            // 6.6.6.1.2.1.5 / 6.6.6.1.2.2.5: store it; an individual one is answered, a talkgroup's isn't
            let st = (a.source, a.target, ahoy_status(a));
            if st != self.last_status || self.now - self.last_status_ms > 2000 {
                self.statuses += 1; // not the same one repeated
                self.sd_statuses += 1;
            }
            self.last_status = st;
            self.sd_status = st.2;
            self.sd_status_src = a.source;
            self.last_status_ms = self.now;
            if !a.group {
                self.reply = match self.answer {
                    ANSWER_SILENT => None,
                    ANSWER_BUSY => Some(ack(R_EQUIP_BUSY, self)),
                    ANSWER_REFUSE => Some(ack(R_RECIPIENT_REFUSED, self)),
                    _ => Some(ack(R_MS_ACCEPTED, self)),
                };
            }
            return;
        }
        self.ahoys += 1;
        if a.service_kind == SK_DUPLEX && self.no_duplex {
            self.reply = Some(ack(R_DUPLEX_NOT_SUPPORTED, self)); // 6.6.9.2.5
            return;
        }
        // a packet data call's availability check is answered Message_Accepted (6.6.3.2.6)
        let accepted = if a.service_kind == SK_IND_PD { R_MESSAGE_ACCEPTED } else { R_MS_ACCEPTED };
        let answer = if a.service_kind == SK_IND_PD && self.answer == ANSWER_FOACSU { ANSWER_ACCEPT } else { self.answer };
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

    /// The TSCC's answer to its C_ACKVIT (tables 6.20, 6.24): checked with its key; the final acknowledgement to
    /// STUNI or KILLI is C_ACKU(MS_Accepted) or C_NACKU(Recipient_Refused) (tables 6.22, 6.26), and on a good one
    /// it stuns, revives or dies. A repeated C_ACKD gets the same answer again.
    fn on_vit_response(&mut self, a: &Ack) {
        let ok = a.source & 0xFF_FFFF == response(self.vit_rand, &self.key, self.psn);
        let target = if self.vit == 3 { KILLI } else { STUNI };
        let reason = if ok { R_MS_ACCEPTED } else { R_RECIPIENT_REFUSED };
        self.reply = Some(Ack { op: OP_ACKU, response_info: 0, reason, target, source: self.id }.encode());
        if ok {
            match self.vit {
                1 => self.stunned = true,
                2 => self.stunned = false,
                _ => self.killed = true, // after this answer goes (6.4.10.0 d)
            }
        }
    }

    /// An authentication challenge (6.4.8.3): C_ACKU(Authentication Response) with the response as its target
    /// (table 6.18), or C_NACKU(MSNot_Supported) without a key, in the slot the AT bit keeps. During its registration
    /// it then waits TNP_Timer for the outcome (figure 6.23).
    fn on_challenge(&mut self, a: &Ahoy) {
        self.challenges += 1;
        let (reason, target) = if self.has_key {
            (R_AUTH_RESPONSE, response(a.source, &self.key, self.psn))
        } else {
            (R_MS_NOT_SUPPORTED, a.source)
        };
        self.reply = Some(Ack { op: OP_ACKU, response_info: 0, reason, target, source: self.id }.encode());
        if self.has_cur && self.cur.what == DO_REGISTER && matches!(self.state, ACCESS | WAITING) {
            self.state = WAITING;
            self.wait_until = self.now + TNP_MS;
        }
    }

    fn on_grant(&mut self, g: &Grant) {
        if self.stunned {
            return;
        }
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
                // an all-call, or a broadcast call to one of its talkgroups
                let mine = asked && self.cur.what == DO_GROUP && g.target == self.cur.arg;
                (mine, mine || (self.state == IDLE && (is_allcall(g.target) || self.my_tg(g.target))))
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
        self.ceasing = false;
        self.header_sent = false;
        self.maint_left = 0;
        self.pay_group = g.op != OP_PV_GRANT;
        self.pay_src = g.source;
        self.pay_dst = g.target;
        if mine && self.cur.flags & F_ALS != 0 {
            self.talking = false; // it listens
        }
        // 6.6.2.6.2: the grant that follows an accepted ALS radio check, in time: it transmits (as OACSU)
        let als = g.op == OP_PV_GRANT && g.target == self.id && g.source == self.als_from && self.now < self.als_until;
        if als && !mine {
            self.als_from = 0;
            self.als_on = true;
            self.talking = true;
        }
    }

    /// PV_GRANT_DX (6.6.9.2.6): applicable when it's the target; its own slot of the carrier, offset timing.
    fn on_dx_grant(&mut self, g: &DxGrant) {
        if g.target != self.id {
            return;
        }
        let asked = self.has_cur && matches!(self.state, ACCESS | WAITING);
        let mine = asked && self.cur.what == DO_DUPLEX && g.source == self.cur.arg;
        let Some(ch) = self.chan_index(g.chan, g.lcn) else { return };
        if self.has_cur && self.cur.what == DO_ANSWER {
            self.has_cur = false; // the grant answers its Call Answer
        } else if !mine && self.has_cur && self.state != IDLE {
            return;
        }
        self.alert_caller = 0;
        self.grants += 1;
        self.state = PAYLOAD;
        self.ch = ch;
        self.dx = true;
        self.dx_peer = g.source;
        self.mine = mine;
        self.talking = true; // both talk
        self.ceasing = false;
        self.header_sent = false;
        self.maint_left = 0;
        self.pay_group = false;
        self.pay_src = g.source;
        self.pay_dst = g.target;
    }

    /// Doesn't support full-duplex calls: answers their radio check C_NACKU(MS_Duplex_Not_Supported).
    pub fn set_no_duplex(&mut self, no: bool) {
        self.no_duplex = no;
    }

    /// Voice bursts heard from another MS on its payload channel.
    pub fn voice_heard(&self) -> u32 {
        self.voice_heard
    }

    /// A voice burst heard on its payload channel (from the air's repeat of another MS).
    pub fn hear(&mut self, ch: usize, src: u32, dst: u32) {
        if self.state == PAYLOAD && ch == self.ch && src != self.id {
            self.voice_heard += 1;
            self.pay_src = src;
            self.pay_dst = dst;
        }
    }

    /// The CACH short LC of carrier c from now on (k % 4 == 0; 361-4 7.1.2). On its TSCC's carrier, P_SYS_Parms with
    /// Payload Channel Type 1 (table 7.34) says the TSCC has reverted to a payload channel for a while (5.3.2): it
    /// waits for it rather than hunting. C_SYS_Parms: the TSCC is back.
    pub fn slc(&mut self, carrier: usize, slc: u32) {
        if carrier != self.tscc_ch / 2 {
            return;
        }
        let (slco, _, flag, _) = sys_parms_decode(slc);
        if slco == SLCO_P_SYS_PARMS {
            self.tscc_rev = flag;
        } else if slco == SLCO_C_SYS_PARMS {
            self.tscc_rev = false;
        }
    }

    /// A Reverse Channel PDU heard on the alternate slot of its payload channel ch (361-1 5.1.5). Cease
    /// transmission (361-4 6.4.15): it finishes its superframe, sends a terminator and stops talking.
    pub fn rc(&mut self, ch: usize, info: u8) {
        if self.rc_deaf {
            return;
        }
        if self.state == PAYLOAD && ch == self.ch && self.talking && !self.ceasing && info == RC_CEASE {
            self.ceasing = true;
            self.cease_left = 5;
        }
    }

    /// Deaf to the Reverse Channel (a radio that doesn't obey it), for tests.
    pub fn rc_deaf(&mut self, deaf: bool) {
        self.rc_deaf = deaf;
    }

    /// Times it stopped talking for an RC cease.
    pub fn ceased(&self) -> u32 {
        self.ceased
    }

    /// Inbound slot k: Some when it transmits.
    pub fn tx(&mut self, k: i64, now_ms: i64) -> Option<MsTx> {
        self.now = now_ms;
        if self.state == PAYLOAD {
            self.intr_leave();
        }
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
            let mut ext = [0u8; 14];
            ext[..2].copy_from_slice(&b[10..]);
            return Some(MsTx { ch: self.ch as u8, kind: TX_PDU, dt, pdu, ext, ..Default::default() });
        }
        if let Some(r) = self.reply.take() {
            return Some(pdu_tx(r, self)); // in the slot the AT bit kept for it
        }
        if self.killed {
            return None;
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
        let due = (0..self.nact).any(|i| self.actions[i].at_ms <= now_ms);
        if self.state == IDLE && due && self.site_hibernates && self.no_aloha > 4 {
            // the TSCC sleeps: wake it (BS_Dwn_Act), then access once C_ALOHA is back
            if now_ms >= self.next_activation_ms {
                self.next_activation_ms = now_ms + 600;
                self.activations += 1;
                return Some(pdu_tx(bs_dwn_act(0, self.id), self));
            }
            return None;
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
            let tries = if self.cur.flags & F_EMERGENCY != 0 { TRIES_EMERGENCY } else { TRIES };
            if self.tries >= tries {
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
        if !self.invited() {
            return None; // not this MS's subset or service this time: a later Aloha may invite it
        }
        self.wait = 0;
        self.resp_wait = nwait_frames(self.nrand) + 2;
        self.rand_sent = true;
        Some(pdu_tx(self.pdu, self))
    }

    /// Whether the last C_ALOHA invites this access: its Mask (6.2.1.1.1: the Mask least significant bits of the
    /// Aloha's address against the MS's; an emergency request ignores it, except Mask 24) and its Service Function
    /// (6.2.1.1.2, table 6.1; not checked for an emergency request).
    fn invited(&self) -> bool {
        let emergency = self.cur.flags & F_EMERGENCY != 0;
        let m = self.mask.min(24) as u32;
        let bits = if m == 0 { 0 } else { (1u32 << m) - 1 };
        if (!emergency || m == 24) && (self.id ^ self.aloha_addr) & bits != 0 {
            return false;
        }
        if emergency {
            return true;
        }
        let registration = matches!(self.cur.what, DO_REGISTER | DO_DEREGISTER);
        let payload = matches!(self.cur.what, DO_GROUP | DO_PRIVATE | DO_ANSWER);
        match self.service_fn & 3 {
            0 => true,
            1 => registration || payload,
            2 => registration || !payload,
            _ => registration,
        }
    }

    fn tx_payload(&mut self, k: i64) -> Option<MsTx> {
        // offset timing in a duplex call: its inbound is the carrier's other slot (361-1 5.1.1.2)
        if k & 1 != ((self.ch & 1) ^ usize::from(self.dx)) as i64 {
            return None;
        }
        if let Some(t) = self.tx_data() {
            return Some(t);
        }
        let group = self.pay_group as u8;
        let dst = if self.dx {
            self.dx_peer
        } else if self.mine {
            self.cur.arg
        } else if self.als_on || (!self.pay_group && self.pay_dst == self.id) {
            self.pay_src // the other party
        } else {
            self.pay_dst
        };
        let offset = u8::from(self.dx);
        // 361-2 table 7.11: Emergency in its own emergency call; Broadcast in its all-call or broadcast call
        // (5.3.2.1, 5.3.3.1)
        let so = if self.mine && !self.dx {
            let e = if self.cur.flags & F_EMERGENCY != 0 { 0x80 } else { 0 };
            let b = self.pay_group && (is_allcall(dst) || self.cur.flags & F_BROADCAST != 0);
            e | if b { 0x08 } else { 0 }
        } else {
            0
        };
        let voice = |what, s: &Ms| MsTx {
            ch: s.ch as u8,
            kind: TX_VOICE,
            what,
            group,
            offset,
            src: s.id,
            dst,
            so,
            ..Default::default()
        };
        if self.talking {
            if !self.header_sent {
                self.header_sent = true;
                let talk = if self.dx && !self.mine {
                    DX_CALLED_TALK_MS
                } else if self.als_on {
                    ALS_MS
                } else if self.intr_talking {
                    self.intr_talk as i64
                } else {
                    self.cur.talk_ms as i64
                };
                self.talk_end = self.now + talk;
                return Some(voice(VOICE_HEADER, self));
            }
            if self.ceasing {
                if self.cease_left > 0 {
                    self.cease_left -= 1;
                    return Some(voice(VOICE_FRAME, self));
                }
                self.ceasing = false;
                self.talking = false;
                self.intr_talking = false;
                self.ceased += 1;
                return Some(voice(VOICE_TERM, self));
            }
            if self.now >= self.talk_end {
                self.talking = false;
                let interrupter = core::mem::take(&mut self.intr_talking);
                if !self.pay_group && (!self.dx || self.mine) && !interrupter {
                    // a duplex call: the caller ends it
                    self.maint_left = MAINT_REPEATS;
                }
                return Some(voice(VOICE_TERM, self));
            }
            return Some(voice(VOICE_FRAME, self));
        }
        if self.maint_left > 0 {
            self.maint_left -= 1;
            let m = Maint { kind: MAINT_DISCON, target: TSI, source: self.id }; // table 7.32
            return Some(MsTx { ch: self.ch as u8, kind: TX_PDU, dt: DT_CSBK, pdu: m.encode(), offset, ..Default::default() });
        }
        None
    }

    /// Packet data on the payload channel: its message, a response it owes, the wait for its own response.
    fn tx_data(&mut self) -> Option<MsTx> {
        let data = |dt, b: &[u8], s: &Ms| {
            let mut pdu = [0u8; 10];
            pdu.copy_from_slice(&b[..10]);
            let mut ext = [0u8; 14];
            ext[..b.len() - 10].copy_from_slice(&b[10..]);
            let (src, dst, group) = (s.id, s.pd_out.head.dst, s.pay_group as u8);
            MsTx { ch: s.ch as u8, kind: TX_DATA, dt, pdu, ext, group, src, dst, ..Default::default() }
        };
        if let Some(r) = self.pd_resp.take() {
            let mut t = data(DT_DATA_HDR, &r, self);
            t.dst = self.pay_src;
            t.group = 0;
            if self.pd_leave && self.pd_resp_blk.is_none() {
                self.back_to_tscc(); // 6.6.3.3.2.5 d): it answered a single item
            }
            return Some(t);
        }
        if let Some(b) = self.pd_resp_blk.as_mut().and_then(|o| o.take()) {
            if !self.pd_resp_blk.is_some_and(|o| o.left()) {
                self.pd_resp_blk = None;
            }
            let mut t = data(DT_R12, &b, self); // C_RDATA: rate 1/2 (361-3 5.4.1.3)
            t.dst = self.pay_src;
            t.group = 0;
            return Some(t);
        }
        if self.pd_sending {
            let n = self.pd_out.n as usize;
            if self.pd_pos == 0 {
                // C_HEAD: F = 1 with every block, F = 0 with a SACK's (361-3 5.4.1)
                self.pd_pos = 1;
                let send = self.pd_send.get();
                let (all, count) = (send == self.pd_all(), send.count_ones() as u8);
                let h = DataHead { full: all, blocks: count, ..self.pd_out.head };
                return Some(data(DT_DATA_HDR, &h.encode(), self));
            }
            let mut j = (self.pd_pos - 1) as usize;
            while j < n && self.pd_send.get() >> j & 1 == 0 {
                j += 1;
            }
            let (dt, len) = (self.pd_out.rate(), block_len(self.pd_out.rate()));
            let b = self.pd_out.b[j.min(MSG_BLOCKS - 1)];
            self.pd_pos = j as u8 + 2;
            let last = (j + 1..n).all(|i| self.pd_send.get() >> i & 1 == 0);
            if last {
                self.pd_sending = false;
                if self.pd_out.head.dpf == DPF_CONFIRMED {
                    self.pd_wait_ack = true;
                    self.pd_until = self.now + DATA_ACK_MS;
                } else {
                    let t = data(dt, &b[..len], self);
                    self.item_done();
                    return Some(t);
                }
            }
            return Some(data(dt, &b[..len], self));
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
pub unsafe extern "C" fn rdof_t3ms_set_status(ms: *mut Ms, status: u8) {
    (*ms).set_status(status & 0x7F);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3ms_key(ms: *mut Ms, k: *const [u8; 16], psn: u32, has_psn: bool) {
    let k = if k.is_null() { None } else { Some(&*k) };
    (*ms).set_key(k, has_psn.then_some(psn & 0xFF_FFFF));
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3ms_do(ms: *mut Ms, what: u8, arg: u32, talk_ms: u32, flags: u8, at_ms: i64) -> bool {
    matches!(what, DO_REGISTER..=DO_CANCEL | DO_STATUS | DO_DUPLEX | DO_SD_STATUS | DO_INTERRUPT) && (*ms).act(what, arg, talk_ms, flags, at_ms)
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

/// The last UDT short data it took: its octets into out (up to cap; character formats less their pad nibbles),
/// the source and the format; the count taken so far (0: none, nothing written).
#[no_mangle]
pub unsafe extern "C" fn rdof_t3ms_sd_inbox(
    ms: *const Ms,
    out: *mut u8,
    cap: usize,
    len: *mut usize,
    src: *mut u32,
    format: *mut u8,
) -> u32 {
    let Some((h, d, count)) = (*ms).inbox() else { return 0 };
    let mut n = (*ms).inbox.data(&mut [0u8; MAX_DATA]);
    if crate::t3udt::pads_nibbles(h.format) {
        n = n.saturating_sub(h.pad_nibble as usize / 2);
    }
    let n = n.min(cap);
    if !out.is_null() {
        core::ptr::copy_nonoverlapping(d.as_ptr(), out, n);
    }
    if !len.is_null() {
        *len = n;
    }
    if !src.is_null() {
        *src = h.source;
    }
    if !format.is_null() {
        *format = h.format;
    }
    count
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
pub unsafe extern "C" fn rdof_t3ms_slc(ms: *mut Ms, carrier: c_int, slc: i64) {
    if carrier >= 0 && (0..1i64 << 28).contains(&slc) {
        (*ms).slc(carrier as usize, slc as u32);
    }
}

/// A Reverse Channel PDU (its info, 4 bits) heard on the alternate slot of payload channel ch (rdof_t3rf_rc_read on
/// the carrier's outbound burst of the other slot).
#[no_mangle]
pub unsafe extern "C" fn rdof_t3ms_rc(ms: *mut Ms, ch: c_int, info: c_int) {
    if ch >= 0 && (0..16).contains(&info) {
        (*ms).rc(ch as usize, info as u8);
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3ms_rx(ms: *mut Ms, ch: c_int, k: i64, dt: u8, pdu: *const [u8; 10], at: bool) {
    if ch >= 0 {
        let mut b = [0u8; BLOCK_MAX];
        if !pdu.is_null() {
            // a data block: 12, 18 or 24 octets as its rate; a data header on a payload channel 12; a PDU 10
            let n = match block_len(dt) {
                0 if dt == DT_DATA_HDR && ((*ms).state == PAYLOAD || ch as usize != (*ms).tscc_ch) => 12,
                0 => 10,
                n => n,
            };
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
