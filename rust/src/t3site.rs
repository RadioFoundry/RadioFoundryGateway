// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! The trunked site controller (DMR Tier III, ETSI TS 102 361-4): registration, random access, group and individual
//! calls, the queue, grants, the TSCC schedule and the timers (docs/spec/fw/tier3.md section 3). C API: rdof/t3.h.
//!
//! Sans-IO: no clock, no allocation, no I/O. The caller gives it the inbound PDUs, payload voice activity, network
//! events and the time; per slot it returns the blocks to send, and events say what else to do. Every struct here
//! is valid all zero, so the caller's memory is zeroed and filled in place (the site is about 100 KB).

use core::ffi::{c_char, c_int, CStr};
use core::fmt::Write;

use crate::t3pdu::*;
use crate::t3udt::{DGNAI, DT_DATA_HDR, DT_RATE12, IPI, SK_IND_PD, SK_IND_UDT, SK_STATUS, SK_SUPPL, SK_TG_PD, SK_TG_UDT};

// short data on the TSCC (UDT short data, status, polling): tier3.md 3.15
#[path = "t3sdata.rs"]
mod sdata;
pub use sdata::{
    rdof_t3_net_sd, rdof_t3_sd_of_event, rdof_t3_sd_send, SdMsg, SD_DGNA, SD_F_EMERGENCY, SD_F_GROUP, SD_POLL,
    SD_INTR, SD_RESULT, SD_SEND, SD_STATUS, SD_UDT, SD_UDT_POLL,
};
use sdata::{SD_DIVERT, SD_IPADDR, SD_IPADV, SD_TGLIST};
// talkgroup lists: registration with a list, DGNA (tier3.md 3.17)
#[path = "t3tglist.rs"]
mod tglist;
pub use tglist::{DGNA_TGS, LIST_TGS, TGLISTS};
// supplementary services: stun/revive, ambient listening, call diversion (tier3.md 3.18)
#[path = "t3supp.rs"]
mod supp;
pub use supp::{
    ACT_SUPP, DEP_ALS, DIVERSIONS, DIV_KINDS, DIV_ON, DIV_PD, DIV_SD, DIV_STATUS, DIV_VOICE, STUNNED, STUN_JOBS,
    SUPP_DIVERT, SUPP_DIVERT_CLEAR, SUPP_REVIVE, SUPP_STUN,
};
pub use supp::{Ackvit, OP_ACKVIT, R_AUTH_RESPONSE_TS, SUPP_KILL};
// packet data calls on the payload channels: tier3.md 3.16
#[path = "t3pdata.rs"]
mod pdata;
pub use pdata::{
    block_len, crc32, crc9, crc9_mask, data_op, rdof_t3_pd_send, rdof_t3_rx_data, DataGrant, DataHead, DataMsg, DataRx,
    BLOCK_MAX, CRC9_RATE1, CRC9_RATE12, CRC9_RATE34, DATA_BLOCK, DATA_END, DT_R1, DT_R12, DT_R34, MSG_OCTETS,
    blocks_mask, Mask, DATA_END_CONFIRMED, DATA_HEADER, DATA_RESPONSE, DPF_CONFIRMED, DPF_RESPONSE, DPF_UNCONFIRMED,
    MSG_BLOCKS,
    OP_PD_GRANT, OP_PD_GRANT_MI, OP_TD_GRANT, OP_TD_GRANT_MI, PARTY_NET, PARTY_SITE, PD_FAILED, PD_OPEN, SAP_IP,
    SO_HI_RATE, SO_SIMI, SO_SUPED, rdof_t3_pd_net_in,
};

// features in their own files, inside this module (they reach the site's state)
#[path = "t3emerg.rs"]
mod emerg;
pub use emerg::RC_SEND;
#[path = "t3authsite.rs"]
mod authsite;
pub use authsite::{
    rdof_t3_entropy, AUTH_OFF, AUTH_REGISTRATION, AUTH_REQUIRED, REG_AUTH_FAIL, REG_AUTH_NONE, REG_AUTH_OK,
};
#[path = "t3status.rs"]
mod status;
pub use status::{ahoy_status, status_ahoy, status_of, status_rand, STATUS_CANCEL_ALARM, STATUS_EMERGENCY_ALARM};
#[path = "t3bcast.rs"]
mod bcast;
pub use bcast::{
    call_timers_of, mass_reg_applies, mass_reg_of, ps_counter, ps_wake, reg_window_ms, reg_window_of, vn_ap_of,
};
use bcast::NbChan;
#[path = "t3tscc.rs"]
mod tscc;
#[path = "t3duplex.rs"]
mod duplex;
#[path = "t3composite.rs"]
mod composite;
pub use composite::{REV_CALL, REV_GRANT};
// transmit interrupt: payload interrupt, ALS termination and call timer de-key (tier3.md 3.20)
#[path = "t3intr.rs"]
mod intr;
// P_AHOY on a voice payload channel: radio check and selective clear (tier3.md 3.15)
#[path = "t3pcheck.rs"]
mod pcheck;
use pcheck::PaCheck;
pub use intr::{ACT_INTR, INTR_ACCEPTED, INTR_CEASED, INTR_GAVE_UP, INTR_TALKS, STATUS_ALS_END, STATUS_INTERRUPT};
use duplex::KIND_DUPLEX;
pub use duplex::{DxGrant, OPEN_DUPLEX, OP_PV_GRANT_DX, R_DUPLEX_NOT_SUPPORTED, SK_DUPLEX};
pub use tscc::{bs_dwn_act, bs_dwn_act_of, move_mbc_hdr, move_mbc_of, move_of, move_pdu, mv_ap, mv_ap_of, OP_BS_DWN_ACT};

pub const CARRIERS: usize = 4;
pub const CHANNELS: usize = CARRIERS * 2;
pub const REGS: usize = 1024;
pub const REG_TGS: usize = 8;
pub const PENDING: usize = 32;
pub const STREAMS: usize = 16;
pub const NEIGHBORS: usize = 32;
pub const MS_POLICIES: usize = 256;
pub const TG_POLICIES: usize = 64;
pub const IDSET_RANGES: usize = 16;
const EVENTS: usize = 64;
const RESP: usize = 32;
const SUBS_MAX: usize = 512;

// ── the C structs (rdof/t3.h) ──────────────────────────────────────────────────

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct Carrier {
    pub tx_hz: u32,
    pub rx_hz: u32,
    pub chan: u16,
    pub tscc: u8,
    pub enabled: u8,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct Cfg {
    pub model: u8,
    pub par: u8,
    pub area_bits: u8,
    pub version: u8,
    pub net: u16,
    pub site: u16,
    pub ncarriers: u8,
    pub cc: u8,
    pub site_sync: u8,
    pub reg_required: u8,
    pub carriers: [Carrier; CARRIERS],
    pub foacsu: u8,
    pub nrand_wait: u8,
    pub backoff_min: u8,
    pub backoff_max: u8,
    pub clear_repeats: u8,
    pub warmup_cach: u8,
    pub ahoy_tries: u8,
    pub queue_max: u8,
    pub clock_ok: u8,
    pub pad0: u8,
    pub utc_offset_min: i16,
    pub tp_ms: u32,
    pub tnp_ms: u32,
    pub hang_ms: u32,
    pub call_ms: u32,
    pub emergency_call_ms: u32,
    pub ahoy_every_ms: u32,
    pub grant_every_ms: u32,
    pub queue_ms: u32,
    pub net_queue_ms: u32,
    pub setup_ms: u32,
    pub start_grace_ms: u32,
    pub reg_hold_s: u32,
    pub adjacent_ms: u32,
    pub chan_freq_ms: u32,
    pub local_time_ms: u32,
    pub gen_site_ms: u32,
    pub stream_seed: u32,
    pub preempt: u8, // 1: an emergency with no channel free clears a non-emergency call (361-4 6.4.14.3.1)
    pub power_save: u8,     // the highest PowerSave_RQ granted (1-7: 1:2 to 1:128); 0: not offered (6.4.7)
    pub mass_reg_start: u8, // Reg_Window (table 6.12) of a mass re-registration at start; 0: none (6.4.5)
    pub duplex: u8, // full-duplex MS to MS calls (section 3.13): 0 refused
    pub call_timers_ms: u32, // CallTimer_Parms every; 0: not broadcast
    pub vote_now_ms: u32,    // Vote_Now for each adjacent site in this time; 0: not broadcast
    pub ann_tscc_ms: u32,    // Ann_WD_TSCC for the adjacent sites in this time; 0: not broadcast
    pub awake_ms: u32,       // how long an MS in power save stays awake after a PDU (the site's T_Awake)
    pub hibernate_ms: u32,   // T_BS_Inactive: the TSCC hibernates after this long with no inbound; 0 never
    pub sd_tries: u8,
    pub sd_repeats: u8,
    pub dgna_ms: u8,
    pub als: u8,
    pub sd_store_ms: u32,
    pub data_hang_ms: u32,
    pub data_id: u32,
    pub divert: u8,
    pub auth: u8, // AUTH_*: MS authentication at registration (361-4 6.4.8; t3authsite.rs)
    pub composite: u8, // 1: the TSCC's slot carries a call when no payload channel is free (361-4 5.3.2)
    pub vote_blk: u8,  // VOTE_BLK: TDMA frames of blocked random access after a Vote Now Advice (361-4 A.2: 2-10)
    pub priority: u8,  // the site's priority radiated with its neighbors (361-4 table 6.81: 1 highest, 7 lowest, 0 none)
    pub intr: u8,      // 1: transmit interrupt (361-4 6.4.14.3.2-6.4.14.3.4, 6.4.15; t3intr.rs)
    pub ip_upload: u8, // 1: packet data to IPI asks the caller for the IP address first (6.6.3.2.4 b 2)
    pub pad1: [u8; 1],
    pub ps_hold_ms: u32, // a group call waits up to this long for its sleeping members' wake frame (6.4.7.1); 0 never
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct Range {
    pub lo: u32,
    pub hi: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct IdSet {
    pub all: u8,
    pub n: u8,
    pub pad: [u8; 2],
    pub r: [Range; IDSET_RANGES],
}

impl IdSet {
    pub fn all() -> IdSet {
        IdSet { all: 1, ..Default::default() }
    }
    pub fn contains(&self, id: u32) -> bool {
        self.all != 0 || self.r[..(self.n as usize).min(IDSET_RANGES)].iter().any(|r| r.lo <= id && id <= r.hi)
    }
    pub fn of(ranges: &[(u32, u32)]) -> IdSet {
        let mut s = IdSet::default();
        for (i, &(lo, hi)) in ranges.iter().take(IDSET_RANGES).enumerate() {
            s.r[i] = Range { lo, hi };
            s.n = i as u8 + 1;
        }
        s
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct Policy {
    pub open: u8,
    pub refuse_unlisted: u8,
    pub pad: [u8; 2],
    pub ms: IdSet,
    pub private_ms: IdSet,
    pub tg: IdSet,
    pub wide_tg: IdSet,
    pub static_tg: IdSet,
}

impl Policy {
    /// Registration open to any MS, every talkgroup allowed, none wide or static.
    pub fn default_open() -> Policy {
        Policy { open: 1, ms: IdSet::all(), private_ms: IdSet::all(), tg: IdSet::all(), ..Default::default() }
    }
}

/// A talkgroup's provisioning (cfg/tg): its hang time (0: the site's) and call priority (0-3).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct TgPolicy {
    pub tg: u32,
    pub hang_ms: u32,
    pub priority: u8,
    pub pad: [u8; 3],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct MsPolicy {
    pub enabled: u8,
    pub private_ok: u8,
    pub wide_private: u8,
    pub emergency_ok: u8,
    pub priority: u8,
    pub ntg: u8,
    pub pad: [u8; 2],
    pub tg: [u32; REG_TGS],
    // authentication (361-4 6.4.8.1): the MS's key K and PSN; secret, never logged or published
    pub has_key: u8,
    pub has_psn: u8,
    pub pad2: [u8; 2],
    pub psn: u32,
    pub key: [u8; 16],
}

impl MsPolicy {
    pub fn tgs(&self) -> &[u32] {
        &self.tg[..(self.ntg as usize).min(REG_TGS)]
    }
}

/// An MS with no entry of its own.
pub const MS_POLICY_DEFAULT: MsPolicy = MsPolicy {
    enabled: 1,
    private_ok: 1,
    wide_private: 1,
    emergency_ok: 1,
    priority: 0,
    ntg: 0,
    pad: [0; 2],
    tg: [0; REG_TGS],
    has_key: 0,
    has_psn: 0,
    pad2: [0; 2],
    psn: 0,
    key: [0; 16],
};

pub const HOW_REGISTER: u8 = 1;
pub const HOW_CALL: u8 = 2;
pub const HOW_LOADED: u8 = 3;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Record {
    pub id: u32,
    pub sub_tg: u32,
    pub how: u8,
    pub ntg: u8,
    pub area: u8,
    pub pad: u8,
    pub tg: [u32; REG_TGS],
    pub ps_rq: u8,  // power save granted: PowerSave_RQ (6.4.7); 0 none
    pub ps_off: u8, // and PowerSave_Offset
    pub pad2: [u8; 2],
    pub at_ms: i64,
    pub exp_ms: i64,
}

impl Record {
    pub fn tgs(&self) -> &[u32] {
        &self.tg[..(self.ntg as usize).min(REG_TGS)]
    }
    /// Puts tg first, the others after it in order (most recent first; the oldest falls off a full list).
    fn touch_tg(&mut self, tg: u32) -> bool {
        let n = (self.ntg as usize).min(REG_TGS);
        let i = self.tg[..n].iter().position(|&t| t == tg);
        if i == Some(0) {
            return false;
        }
        let end = match i {
            Some(i) => i,
            None => {
                self.ntg = (n + 1).min(REG_TGS) as u8;
                n.min(REG_TGS - 1)
            }
        };
        for j in (0..end).rev() {
            self.tg[j + 1] = self.tg[j];
        }
        self.tg[0] = tg;
        true
    }
    fn remove_tg(&mut self, tg: u32) {
        let n = (self.ntg as usize).min(REG_TGS);
        if let Some(i) = self.tg[..n].iter().position(|&t| t == tg) {
            for j in i..n - 1 {
                self.tg[j] = self.tg[j + 1];
            }
            self.tg[n - 1] = 0;
            self.ntg -= 1;
        }
    }
}

pub const SETUP_REQUEST: u8 = 1;
pub const SETUP_ACCEPT: u8 = 2;
pub const SETUP_REJECT: u8 = 3;
pub const SETUP_ALERTING: u8 = 4;
pub const SETUP_CANCEL: u8 = 5;
pub const SETUP_ANSWERED: u8 = 6;
pub const WHY_OK: u8 = 0;
pub const WHY_NOT_FOUND: u8 = 1;
pub const WHY_BUSY: u8 = 2;
pub const WHY_NO_ANSWER: u8 = 3;
pub const WHY_REFUSED: u8 = 4;
pub const WHY_NOT_SUPPORTED: u8 = 5;
pub const WHY_CONGESTION: u8 = 6;
pub const WHY_TIMEOUT: u8 = 7;
pub const SETUP_F_FOACSU: u8 = 0x01;
pub const SETUP_F_UNCONFIRMED: u8 = 0x02;
pub const SETUP_F_DATA: u8 = 0x04; // a packet data call (tier3.md 5.10)
pub const SETUP_F_ALS: u8 = 0x08; // an Ambient Listening Service call (361-4 6.6.2.6): the called MS transmits

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Setup {
    pub op: u8,
    pub reason: u8,
    pub flags: u8,
    pub svc: u8,
    pub src: u32,
    pub dst: u32,
    pub stream: u32,
    pub r#ref: u32,
}

/// An adjacent site: its C_SYScode, its TSCC's logical channel, Active_Connection (when known), its priority
/// (table 6.81; the confirmed priority radiated with it is the site's own, Cfg.priority), and its TSCC's color code
/// and frequencies as its own carriers give them (tx_hz its outbound, rx_hz its inbound; 0: not known).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Neighbor {
    pub syscode: u16,
    pub chan: u16,
    pub active_known: u8,
    pub active: u8,
    pub prio_adjacent: u8,
    pub cc: u8,
    pub tx_hz: u32,
    pub rx_hz: u32,
}

pub const EV_CH_OPEN: u8 = 1;
pub const EV_CH_CLOSE: u8 = 2;
pub const EV_CH_STREAM: u8 = 3;
pub const EV_NET_SETUP: u8 = 4;
pub const EV_NET_SUBS: u8 = 5;
pub const EV_RECORD: u8 = 6;
pub const EV_ACTIVITY: u8 = 7;
pub const EV_NET_DROP: u8 = 8;
pub const EV_LOG: u8 = 9;
pub const EV_SD: u8 = 20; // short data for the site, or the result of one it sent (SdMsg::of_event)
pub const EV_NET_SD: u8 = 21; // short data or a result to publish (mqtt-net sdata)
pub const EV_PD: u8 = 22; // a data channel opened (PD_OPEN), or the site's own data call failed (PD_FAILED)

pub const CLOSE_HANG: u8 = 1;
pub const CLOSE_TIMER: u8 = 2;
pub const CLOSE_DISCON: u8 = 3;
pub const CLOSE_CMD: u8 = 4;
pub const CLOSE_WITHDRAW: u8 = 5;
pub const CLOSE_PREEMPT: u8 = 6;
pub const CLOSE_INTR: u8 = 7; // an ALS call ended at its caller's request (361-4 6.4.14.3.4)

pub const ACT_REG: u8 = 1;
pub const ACT_GRANT: u8 = 2;
pub const ACT_QUEUE: u8 = 3;
pub const ACT_SETUP: u8 = 4;
pub const ACT_REFUSE: u8 = 5;
pub const ACT_STATUS: u8 = 6;
pub const REG_REGISTER: u8 = 1;
pub const REG_DEREGISTER: u8 = 2;
pub const REG_EXPIRE: u8 = 3;
/// An expired record whose MS was sent a registration demand: kept this long for its answer
const DEMAND_GRACE_MS: i64 = 30_000;
/// An expired record whose demand waits for a mass re-registration under way
const DEMAND_RETRY_MS: i64 = 5_000;
pub const REG_DENY: u8 = 4;
pub const REG_REFUSE: u8 = 5;
pub const QUEUE_IN: u8 = 1;
pub const QUEUE_OUT: u8 = 2;
pub const QUEUE_DROP: u8 = 3;

pub const F_GROUP: u8 = 0x01;
pub const F_EMERGENCY: u8 = 0x02;
pub const F_NET: u8 = 0x04;
pub const F_WIDE: u8 = 0x08;
pub const F_ALLCALL: u8 = 0x10;
pub const F_LATE: u8 = 0x20;
pub const F_PRIVATE_NET: u8 = 0x40;
pub const F_DATA: u8 = 0x80; // a packet data call (tier3.md 3.16)

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct Event {
    pub r#type: u8,
    pub ch: u8,
    pub why: u8,
    pub flags: u8,
    pub op: u8,
    pub reason: u8,
    pub svc: u8,
    pub kind: u8,
    pub chan: u16,
    pub lcn: u8,
    pub pad: u8,
    pub src: u32,
    pub dst: u32,
    pub stream: u32,
    pub r#ref: u32,
    pub text: [u8; 64],
}

impl Default for Event {
    fn default() -> Event {
        Event {
            r#type: 0,
            ch: 0,
            why: 0,
            flags: 0,
            op: 0,
            reason: 0,
            svc: 0,
            kind: 0,
            chan: 0,
            lcn: 0,
            pad: 0,
            src: 0,
            dst: 0,
            stream: 0,
            r#ref: 0,
            text: [0; 64],
        }
    }
}

impl Event {
    pub fn text(&self) -> &str {
        let n = self.text.iter().position(|&c| c == 0).unwrap_or(self.text.len());
        core::str::from_utf8(&self.text[..n]).unwrap_or("")
    }
}

pub const USE_OFF: u8 = 0;
pub const USE_IDLE: u8 = 1;
pub const USE_TSCC: u8 = 2;
pub const USE_CALL: u8 = 3;
pub const USE_HANG: u8 = 4;
pub const USE_CLEAR: u8 = 5;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct ChOut {
    pub dt: u8,
    pub pdu: [u8; 10],
    pub r#use: u8,
    pub rc: u8, // RC_SEND | RC info: a Reverse Channel PDU in this channel's outbound burst, for its other slot's talker
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct SlotOut {
    pub tscc_dt: u8,
    pub tscc_pdu: [u8; 10],
    pub at: u8,
    pub has_slc: u8,
    pub tscc_ext: [u8; 2],
    pub rev: u8, // REV_CALL / REV_GRANT: a composite TSCC's slot and its call in slot k (t3composite.rs)
    pub slc_tscc: u32,
    pub slc_payload: u32,
    pub ch: [ChOut; CHANNELS],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct ChStatus {
    pub r#use: u8,
    pub lcn: u8,
    pub flags: u8,
    pub pad: u8,
    pub chan: u16,
    pub pad2: u16,
    pub src: u32,
    pub dst: u32,
    pub since_ms: i64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct Status {
    pub connected: u8,
    pub withdrawn: u8,
    pub nch: u8,
    pub pad: u8,
    pub regs: u32,
    pub queue: u32,
    pub aloha: u32,
    pub rand: u32,
    pub collisions: u32,
    pub grants: u32,
    pub refused: u32,
    pub queued: u32,
    pub ch: [ChStatus; CHANNELS],
}

pub const VOICE_HEADER: u8 = 1;
pub const VOICE_FRAME: u8 = 2;
pub const VOICE_TERM: u8 = 3;
pub const VOICE_LOST: u8 = 4;

// ── internal state ──────────────────────────────────────────────────────────────

const KIND_NONE: u8 = 0;
const KIND_GROUP: u8 = 1;
const KIND_PRIVATE: u8 = 2;
const KIND_ALLCALL: u8 = 3;

/// An RF talker heard nothing for this long (no frame, no terminator): the item ended.
const VOICE_LOST_MS: i64 = 1500;
/// C_WACKD and C_QACKD repeats to a caller that waits (it gives up after its own TP or TNP otherwise).
const WACK_EVERY_MS: i64 = 2000;
const QACK_EVERY_MS: i64 = 4000;
/// A record is written again (EV_RECORD) at most this often when only its time changes.
const RECORD_REFRESH_MS: i64 = 60_000;
const LOAD_BUCKET_MS: i64 = 200;
const LOAD_BUCKETS: usize = 10;

#[derive(Clone, Copy, Default)]
struct Chan {
    r#use: u8,
    kind: u8,
    flags: u8,
    clear_left: u8,
    close_why: u8,
    talking: bool,
    talking_net: bool,
    src: u32,
    dst: u32,
    stream: u32,
    talker: u32,
    since_ms: i64,
    call_end_ms: i64,
    idle_until_ms: i64,
    grant_next_ms: i64,
    last_voice_ms: i64,
    preempt: bool, // being cleared for an emergency (t3emerg.rs)
    held: bool,    // cleared over a talker that didn't cease: out of use until its inbound stops (t3emerg.rs)
    preempt_until_ms: i64,
    rc_next_ms: i64,
    als: bool,       // an Ambient Listening Service call (6.6.2.6): the called MS (dst) transmits
    dekey: u8,       // a transmit interrupt de-keys its talker (t3intr.rs DK_*)
    dekey_until_ms: i64,
    intr_ms: u32,    // the interrupting MS
    intr_stream: u32, // the interrupt sent to the talker's site (DK_NET): its stream, a result's ref
    protect_ms: u32, // P_PROTECT(EN_PTT_ONE_MS) to this MS while the channel waits for it (t3intr.rs)
    protect_until_ms: i64,
    pa: PaCheck, // a P_AHOY radio check or selective clear on the channel (t3pcheck.rs)
}

const P_AHOY: u8 = 1; // waiting for the called MS's answer to C_AHOY
const P_ALERTING: u8 = 2; // FOACSU: the called party alerts; waiting for Call Answer
const P_NET: u8 = 3; // waiting for the network's answer to our setup request
const P_QUEUED: u8 = 4; // both parties ready (or a group call): waiting for a payload channel
const P_WAKE: u8 = 5; // a group call waiting for its members' power save wake frame (6.4.7.1)

#[derive(Clone, Copy, Default)]
struct Pend {
    used: bool,
    kind: u8,
    state: u8,
    svc: u8,
    prio: u8,
    ahoy_left: u8,
    caller_local: bool,
    called_local: bool,
    answered: bool,
    caller: u32,
    called: u32,
    stream: u32,     // the setup's stream: our request's (called remote) or the request we answer (caller remote)
    net_stream: u32, // network voice waiting for this call's channel
    since_ms: i64,
    deadline_ms: i64,
    next_ahoy_ms: i64,
    next_ack_ms: i64,
    als: bool, // an Ambient Listening Service call (6.6.2.6)
}

#[derive(Clone, Copy, Default)]
struct NetWait {
    used: bool,
    emergency: bool,
    stream: u32,
    src: u32,
    tg: u32,
    since_ms: i64,
    deadline_ms: i64,
}

#[derive(Clone, Copy, Default)]
struct Reg {
    used: bool,
    rec: Record,
    written_ms: i64,
    awake_ms: i64, // awake until (power save; what was granted is in the record)
    demanded: bool, // expired, and a registration demanded of it (6.4.5.0); deleted if it doesn't answer
    ips: [u32; IPS_PER_MS], // IP connection advice (6.4.11): its IPv4 addresses, 0 unused; not kept in the records
}

/// The IP addresses an MS may advise (6.4.11.0 a): more get IP_Connection_failed.
const IPS_PER_MS: usize = 2;

#[derive(Clone, Copy, Default)]
struct MsPol {
    used: bool,
    id: u32,
    p: MsPolicy,
}

#[derive(Clone, Copy, Default)]
struct Resp {
    dt: u8,
    solicit: bool,
    has_cont: bool,
    pdu: [u8; 10],
    cont: [u8; 10],
    /// the answer to a C_RAND heard in inbound slot rand_k (rx_k_ok then) from rand_src: a line when it goes out
    rand: bool,
    rand_k: i64,
    rand_src: u32,
}

#[derive(Clone, Copy, Default)]
struct Counters {
    aloha: u32,
    rand: u32,
    collisions: u32,
    grants: u32,
    refused: u32,
    queued: u32,
    events_lost: u32,
}

pub struct Site {
    auth: authsite::Auth,
    cfg: Cfg,
    policy: Policy,
    syscode: u16,
    tscc_carrier: usize,
    now: i64,
    k: i64,
    /// the inbound slot of the PDU rx_pdu takes, when its caller gave one (rdof_t3_rx_pdu)
    rx_k: i64,
    rx_k_ok: bool,
    connected: bool,
    withdrawn: bool,
    next_stream: u32,
    // the TSCC
    resp: [Resp; RESP],
    resp_head: usize,
    resp_len: usize,
    cont_pending: bool,
    cont: [u8; 10],
    since_aloha: u32,
    load: [u16; LOAD_BUCKETS],
    load_bucket: i64,
    next_adjacent_ms: i64,
    adjacent_idx: usize,
    next_chan_freq_ms: i64,
    chan_freq_idx: usize,
    next_local_time_ms: i64,
    next_gen_site_ms: i64,
    // calls
    chans: [Chan; CHANNELS],
    pend: [Pend; PENDING],
    net: [NetWait; STREAMS],
    bc: bcast::BcState,
    tscc: tscc::TsccState,
    rev: composite::RevState,
    // registrations and provisioning
    regs: [Reg; REGS],
    reg_scan: usize,
    nregs: u32,
    mspol: [MsPol; MS_POLICIES],
    tgpol: [TgPolicy; TG_POLICIES], // tg 0: unused
    neighbors: [Neighbor; NEIGHBORS],
    nneighbors: usize,
    subs_dirty: bool,
    subs_sum: u64,
    // out
    events: [Event; EVENTS],
    ev_head: usize,
    ev_len: usize,
    counters: Counters,
    sd: sdata::SdState,
    pd: pdata::PdState,
    tgl: tglist::TglState,
    supp: supp::SuppState,
}

/// Text into a fixed buffer, NUL terminated, cut when full (LOG events, errors).
struct Buf<'a> {
    b: &'a mut [u8],
    n: usize,
}

impl Write for Buf<'_> {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        for &c in s.as_bytes() {
            if self.n + 1 < self.b.len() {
                self.b[self.n] = c;
                self.n += 1;
            }
        }
        if self.n < self.b.len() {
            self.b[self.n] = 0;
        }
        Ok(())
    }
}

/// The defaults of tier3.md section 3.8: one composite carrier.
pub fn cfg_default() -> Cfg {
    let mut c = Cfg {
        model: MODEL_SMALL,
        par: 3,
        area_bits: 5,
        version: 4,
        net: 1,
        site: 1,
        ncarriers: 1,
        cc: 1,
        site_sync: 0,
        reg_required: 1,
        foacsu: 0,
        nrand_wait: 6,
        backoff_min: 3,
        backoff_max: 9,
        clear_repeats: 4,
        warmup_cach: 4,
        ahoy_tries: 3,
        queue_max: 16,
        clock_ok: 0,
        tp_ms: 20_000,
        tnp_ms: 10_000,
        hang_ms: 3000,
        call_ms: 180_000,
        emergency_call_ms: 300_000,
        ahoy_every_ms: 1000,
        grant_every_ms: 1200,
        queue_ms: 10_000,
        net_queue_ms: 2000,
        setup_ms: 5000,
        start_grace_ms: 2000,
        reg_hold_s: 14_400,
        adjacent_ms: 5000,
        chan_freq_ms: 10_000,
        local_time_ms: 6000,
        gen_site_ms: 10_000,
        stream_seed: 1,
        awake_ms: 1000,
        sd_tries: 3,
        sd_repeats: 2,
        sd_store_ms: 30_000,
        data_hang_ms: 3000,
        divert: 1,
        vote_blk: 6,
        ps_hold_ms: 2000,
        ..Default::default()
    };
    c.carriers[0] = Carrier { tx_hz: 438_750_000, rx_hz: 438_250_000, chan: 1, tscc: 1, enabled: 1 };
    c
}

/// The settings' checks; the first problem as text.
fn check(c: &Cfg, err: &mut Buf) -> bool {
    macro_rules! no {
        ($($a:tt)*) => {{ let _ = write!(err, $($a)*); return false; }};
    }
    if c.model > 3 {
        no!("system.model: 0 to 3");
    }
    let (nb, sb) = model_bits(c.model);
    if c.net as u32 >= 1 << nb {
        no!("system.net: 0 to {} for this model", (1u32 << nb) - 1);
    }
    // SITE 0 is a site: table 6.5 counts 2^n sites for an n-bit SITE
    if c.site as u32 >= 1 << sb {
        no!("system.site: 0 to {} for this model", (1u32 << sb) - 1);
    }
    if !(1..=3).contains(&c.par) {
        no!("system.par: a, b or ab");
    }
    if c.area_bits == 0 || c.area_bits as u32 > sb {
        no!("system.area_bits: 1 to {}", sb);
    }
    if c.cc > 15 {
        no!("cc: 0 to 15");
    }
    let n = c.ncarriers as usize;
    if n == 0 || n > CARRIERS {
        no!("carriers: 1 to {}", CARRIERS);
    }
    let mut tscc = 0;
    for i in 0..n {
        let a = &c.carriers[i];
        if a.chan == 0 || a.chan > 4094 {
            no!("carriers[{}].chan: 1 to 4094", i);
        }
        if (0..i).any(|j| c.carriers[j].chan == a.chan) {
            no!("carriers[{}].chan: {} is used twice", i, a.chan);
        }
        if a.tscc != 0 {
            tscc += 1;
            if a.enabled == 0 {
                no!("carriers[{}]: the TSCC carrier can't be disabled", i);
            }
        }
    }
    if tscc != 1 {
        no!("carriers: exactly one with tscc (found {})", tscc);
    }
    if c.nrand_wait > 15 {
        no!("timers.nrand_wait: 0 to 15");
    }
    if !(2..=10).contains(&c.vote_blk) {
        no!("broadcast.vote_blk: 2 to 10");
    }
    if c.priority > 7 {
        no!("broadcast.priority: 0 to 7");
    }
    if c.backoff_min == 0 || c.backoff_max > 15 || c.backoff_min > c.backoff_max {
        no!("timers.backoff_min, backoff_max: 1 to 15, min not over max");
    }
    if !(4000..=60_000).contains(&c.tp_ms) {
        no!("timers.tp_s: 4 to 60");
    }
    if !(2000..=60_000).contains(&c.tnp_ms) {
        no!("timers.tnp_s: 2 to 60");
    }
    if !(1000..=60_000).contains(&c.hang_ms) {
        no!("timers.hang_s: 1 to 60");
    }
    if !(1000..=4_094_000).contains(&c.call_ms) {
        no!("timers.call_s: 1 to 4094");
    }
    if !(1000..=510_000).contains(&c.emergency_call_ms) {
        no!("timers.emergency_call_s: 1 to 510");
    }
    if !(1..=10).contains(&c.ahoy_tries) || !(300..=5000).contains(&c.ahoy_every_ms) {
        no!("timers.ahoy_tries 1 to 10, ahoy_every_s 0.3 to 5");
    }
    if !(300..=10_000).contains(&c.grant_every_ms) {
        no!("timers.grant_every_s: 0.3 to 10");
    }
    if !(1..=8).contains(&c.clear_repeats) {
        no!("timers.clear_repeats: 1 to 8");
    }
    if c.warmup_cach != 2 && c.warmup_cach != 4 {
        no!("timers.warmup_cach: 2 or 4");
    }
    if !(1..=16).contains(&c.queue_max) || !(1000..=60_000).contains(&c.queue_ms) {
        no!("timers.queue_max 1 to 16, queue_s 1 to 60");
    }
    if c.net_queue_ms > 10_000 || !(1000..=30_000).contains(&c.setup_ms) {
        no!("timers.net_queue_s 0 to 10, setup_s 1 to 30");
    }
    if c.start_grace_ms > 10_000 {
        no!("timers.start_grace_s: 0 to 10");
    }
    if c.duplex > 1 {
        no!("services.duplex: a bool");
    }
    if c.hibernate_ms != 0 && !(1000..=300_000).contains(&c.hibernate_ms) {
        no!("services.hibernate_s: 0 (never) or 1 to 300");
    }
    if c.power_save > 7 || c.mass_reg_start > 15 || !(100..=60_000).contains(&c.awake_ms) {
        no!("services.power_save 0 to 7, power_save_awake_s 0.1 to 60, mass_reg_at_start_s 0 to 200000");
    }
    if c.sd_tries > 10 || c.sd_repeats > 10 || c.sd_store_ms > 600_000 {
        no!("timers.sd_tries 1 to 10, sd_repeats 1 to 10, sd_store_s 0 to 600");
    }
    if !(500..=30_000).contains(&c.data_hang_ms) {
        no!("timers.data_hang_s: 0.5 to 30");
    }
    if c.data_id > 0xFFFCDF {
        no!("data_id: an individual ID, 1 to 16776415, or 0");
    }
    if !(60..=604_800).contains(&c.reg_hold_s) {
        no!("timers.reg_hold_s: 60 to 604800");
    }
    if c.composite > 1 {
        no!("composite_tscc: true or false");
    }
    if c.preempt > 1 {
        no!("emergency_preempt: true or false");
    }
    if c.intr > 1 {
        no!("transmit_interrupt: true or false");
    }
    if c.ip_upload > 1 {
        no!("ip_address_upload: true or false");
    }
    if c.auth > AUTH_REQUIRED {
        no!("auth: off, registration or required");
    }
    true
}

fn better(key: &(bool, u8, i64), best: &Option<((bool, u8, i64), bool, usize)>) -> bool {
    match best {
        None => true,
        Some((b, _, _)) => (key.0, key.1, -key.2) > (b.0, b.1, -b.2),
    }
}

impl Site {
    // ── setup ──

    /// Fills a zeroed Site.
    fn setup(&mut self, cfg: &Cfg) {
        self.policy = Policy::default_open();
        self.next_stream = cfg.stream_seed.max(1);
        self.apply(cfg);
        self.reset_channels();
    }

    fn apply(&mut self, cfg: &Cfg) {
        self.cfg = *cfg;
        self.syscode = SysCode { model: cfg.model, net: cfg.net, site: cfg.site, par: cfg.par }.encode();
        self.tscc_carrier = (0..cfg.ncarriers as usize).find(|&i| cfg.carriers[i].tscc != 0).unwrap_or(0);
    }

    /// Sets each channel's use from the carriers, leaving calls in progress alone.
    fn reset_channels(&mut self) {
        for ch in 0..CHANNELS {
            let carrier = ch / 2;
            let exists = carrier < self.cfg.ncarriers as usize && self.cfg.carriers[carrier].enabled != 0;
            let c = &mut self.chans[ch];
            if ch == self.tscc_carrier * 2 {
                if !matches!(c.r#use, USE_CALL | USE_HANG | USE_CLEAR) {
                    *c = Chan { r#use: USE_TSCC, ..Default::default() }; // a composite TSCC's call goes on
                }
            } else if !exists {
                if c.kind == KIND_NONE {
                    *c = Chan::default();
                }
            } else if c.r#use == USE_OFF || c.r#use == USE_TSCC {
                *c = Chan { r#use: USE_IDLE, ..Default::default() };
            }
        }
    }

    // ── helpers ──

    fn ev(&mut self, e: Event) {
        if self.ev_len == EVENTS {
            self.counters.events_lost += 1;
            return;
        }
        let i = (self.ev_head + self.ev_len) % EVENTS;
        self.events[i] = e;
        self.ev_len += 1;
    }

    fn log(&mut self, args: core::fmt::Arguments) {
        let mut e = Event { r#type: EV_LOG, ..Default::default() };
        let mut b = Buf { b: &mut e.text, n: 0 };
        let _ = b.write_fmt(args);
        self.ev(e);
    }

    fn chan_ev(&self, ch: usize, ty: u8) -> Event {
        let c = &self.chans[ch];
        Event {
            r#type: ty,
            ch: ch as u8,
            why: if ty == EV_CH_OPEN && c.kind == KIND_DUPLEX { OPEN_DUPLEX } else { 0 },
            flags: c.flags,
            svc: Self::lc_options(c),
            chan: self.cfg.carriers[ch / 2].chan,
            lcn: (ch % 2) as u8,
            src: c.src,
            dst: c.dst,
            stream: c.stream,
            ..Default::default()
        }
    }

    /// The Service Options of a voice LC the site makes for channel c's call (361-2 table 7.11): Emergency (1000
    /// 0000b) for an emergency call, Broadcast (0000 1000b) for an all-call or a broadcast call (361-2 5.3.2.1 and
    /// 5.3.3.1: "the Service Options Broadcast Field of the Grp_V_Ch_Usr PDU shall be set to 1").
    fn lc_options(c: &Chan) -> u8 {
        let e = if c.flags & F_EMERGENCY != 0 { 0x80 } else { 0 };
        e | if c.kind == KIND_ALLCALL { 0x08 } else { 0 }
    }

    fn activity(&mut self, kind: u8, op: u8, src: u32, dst: u32, reason: u8) {
        self.ev(Event { r#type: EV_ACTIVITY, kind, op, src, dst, reason, ..Default::default() });
    }

    fn drop_stream(&mut self, stream: u32, why: u8) {
        self.ev(Event { r#type: EV_NET_DROP, stream, why, ..Default::default() });
    }

    fn new_stream(&mut self) -> u32 {
        let s = self.next_stream;
        self.next_stream = self.next_stream.wrapping_add(1).max(1);
        s
    }

    fn reg_bit(&self) -> bool {
        self.cfg.reg_required != 0
    }

    /// The Backoff code: the floor plus half the random accesses and collisions of the last 2 s.
    fn backoff(&self) -> u8 {
        let load: u32 = self.load.iter().map(|&n| n as u32).sum();
        (self.cfg.backoff_min as u32 + load / 2).clamp(self.cfg.backoff_min as u32, self.cfg.backoff_max as u32) as u8
    }

    fn count_load(&mut self) {
        let i = self.now.div_euclid(LOAD_BUCKET_MS).rem_euclid(LOAD_BUCKETS as i64) as usize;
        self.load[i] = self.load[i].saturating_add(1);
    }

    fn roll_load(&mut self) {
        let b = self.now.div_euclid(LOAD_BUCKET_MS);
        if b != self.load_bucket {
            let steps = (b - self.load_bucket).clamp(0, LOAD_BUCKETS as i64);
            for s in 1..=steps {
                self.load[(self.load_bucket + s).rem_euclid(LOAD_BUCKETS as i64) as usize] = 0;
            }
            self.load_bucket = b;
        }
    }

    fn queue_len(&self) -> usize {
        self.pend.iter().filter(|p| p.used && p.state == P_QUEUED).count() + self.net.iter().filter(|n| n.used).count()
    }

    fn tg_policy(pols: &[TgPolicy], tg: u32) -> Option<&TgPolicy> {
        if tg == 0 {
            return None;
        }
        pols.iter().find(|t| t.tg == tg)
    }

    /// A channel's hang time: none for a broadcast or an all-call (361-2 5.3.3.0, 5.3.3.1: "no call hangtime is
    /// to be generated"; 361-4 6.6.1.7 treats all-calls as broadcasts), its talkgroup's (cfg/tg) for a group call,
    /// else the site's.
    fn hang_of(pols: &[TgPolicy], dflt: u32, kind: u8, dst: u32) -> i64 {
        if kind == KIND_ALLCALL {
            return 0;
        }
        let t = if kind == KIND_GROUP { Self::tg_policy(pols, dst) } else { None };
        t.filter(|t| t.hang_ms > 0).map_or(dflt, |t| t.hang_ms) as i64
    }

    fn ms_policy(&self, ms: u32) -> MsPolicy {
        self.mspol.iter().find(|m| m.used && m.id == ms).map(|m| m.p).unwrap_or(MS_POLICY_DEFAULT)
    }

    fn reg_index(&self, ms: u32) -> Option<usize> {
        self.regs.iter().position(|r| r.used && r.rec.id == ms)
    }

    fn live(c: &Chan) -> bool {
        matches!(c.r#use, USE_CALL | USE_HANG)
    }

    // ── the TSCC ──

    fn push(&mut self, dt: u8, pdu: [u8; 10], solicit: bool, cont: Option<[u8; 10]>) {
        if self.resp_len == RESP {
            self.log(format_args!("tscc queue full"));
            return; // the MS repeats its request
        }
        let i = (self.resp_head + self.resp_len) % RESP;
        self.resp[i] =
            Resp { dt, solicit, has_cont: cont.is_some(), pdu, cont: cont.unwrap_or([0; 10]), ..Default::default() };
        self.resp_len += 1;
    }

    /// A C_RAND just taken: its answer, the first response it queued, is marked to be said when it goes out; none
    /// queued (an answer later, or none), said now. Each line has the inbound slot it was heard in, for timing
    /// random access (6.4.4.1: the MS waits NRand_Wait TSCC slots for an answer before it tries again).
    fn rand_said(&mut self, r: &Rand, before: usize, queued: usize) {
        if !self.rx_k_ok {
            return;
        }
        if self.resp_len > queued {
            let a = &mut self.resp[before];
            a.rand = true;
            a.rand_k = self.rx_k;
            a.rand_src = r.source;
        } else {
            let (k, src, sk) = (self.rx_k.rem_euclid(1_000_000), r.source, r.service_kind);
            self.log(format_args!("C_RAND from {src} in slot {k}: kind {sk}, no answer queued"));
        }
    }

    /// C_ACKD, C_NACKD, C_QACKD or C_WACKD (by the reason) to target, about source.
    fn ack(&mut self, reason: u8, target: u32, source: u32) {
        let ri = match reason {
            R_REG_ACCEPTED | R_REG_SUBSCRIPTION => 0, // no power save, no list pattern
            _ => SysCode::decode(self.syscode).response_check(),
        };
        let a = Ack { op: OP_ACKD, response_info: ri, reason, target, source };
        self.push(DT_CSBK, a.encode(), false, None);
        if reason_type(reason) == 0 {
            self.counters.refused += 1;
        }
    }

    fn refuse(&mut self, reason: u8, target: u32, source: u32) {
        self.ack(reason, target, source);
        self.activity(ACT_REFUSE, 0, target, source, reason);
        if reason == R_MS_NOT_REGISTERED {
            // 6.4.5.0: the TSCC may demand a registration from one MS (Mask 24); an MS whose record this site
            // dropped (reg_hold_s) may not know it must register again
            self.demand_registration(target);
        }
    }

    fn aloha(&mut self) -> [u8; 10] {
        self.counters.aloha += 1;
        let sf = if self.queue_len() >= self.cfg.queue_max as usize { 3 } else { 0 };
        Aloha {
            tsccas: false,
            site_sync: self.cfg.site_sync != 0,
            version: self.cfg.version,
            offset: false,
            active_connection: self.connected,
            mask: 0,
            service_function: sf,
            nrand_wait: self.cfg.nrand_wait,
            reg: self.reg_bit(),
            backoff: self.backoff(),
            syscode: self.syscode,
            addr: ADRNULL,
        }
        .encode()
    }

    fn grant_pdu(&self, ch: usize, late: bool, source: u32) -> [u8; 10] {
        let c = &self.chans[ch];
        if c.kind == KIND_DUPLEX {
            return self.duplex_grant(ch);
        }
        let op = match c.kind {
            KIND_PRIVATE => OP_PV_GRANT,
            KIND_ALLCALL => OP_BTV_GRANT,
            _ => OP_TV_GRANT,
        };
        let data = c.flags & F_DATA != 0;
        let op = if data { data_op(c.kind != KIND_PRIVATE, self.pd.multi_on(ch)) } else { op };
        Grant {
            op,
            chan: self.cfg.carriers[ch / 2].chan,
            lcn: (ch % 2) as u8,
            late_entry: late && op != OP_PV_GRANT && !data, // a data grant's HI_RATE: single slot
            emergency: c.flags & F_EMERGENCY != 0,
            offset: false,
            target: c.dst,
            source,
        }
        .encode()
    }

    /// A grant repeat that is due (Late_Entry, for MSs that missed the start): the oldest one. Through the call and
    /// its hang time, until P_CLEAR (6.6.2.4.1, "while a call is active"; the captured site does the same).
    fn due_grant(&mut self) -> Option<[u8; 10]> {
        self.ps_regrant();
        let now = self.now;
        let ch = (0..CHANNELS)
            .filter(|&i| Self::live(&self.chans[i]) && !matches!(self.chans[i].kind, KIND_PRIVATE | KIND_DUPLEX) && now >= self.chans[i].grant_next_ms)
            .min_by_key(|&i| self.chans[i].grant_next_ms)?;
        self.chans[ch].grant_next_ms = now + self.cfg.grant_every_ms as i64;
        let c = self.chans[ch];
        Some(self.grant_pdu(ch, true, if c.talker != 0 { c.talker } else { c.src }))
    }

    /// A broadcast that is due: the block and its type, and an MBC's continuation.
    fn due_bcast(&mut self) -> Option<([u8; 10], u8, Option<[u8; 10]>)> {
        let now = self.now;
        let (reg, bo, sc) = (self.reg_bit(), self.backoff(), self.syscode);
        if let Some(b) = self.due_bcast_more(reg, bo, sc) {
            return Some(b);
        }
        let nn = self.nneighbors;
        if self.cfg.adjacent_ms > 0 && nn > 0 && now >= self.next_adjacent_ms {
            self.next_adjacent_ms = now + (self.cfg.adjacent_ms as i64 / nn as i64).max(300);
            // CH_ADJ has no absolute form (table 7.79): a neighbor whose number names other frequencies here is
            // left out
            for _ in 0..nn {
                let i = self.adjacent_idx % nn;
                self.adjacent_idx = (i + 1) % nn;
                if self.nb_chan(i) == NbChan::Clash {
                    continue;
                }
                let n = self.neighbors[i];
                let act = if n.active_known != 0 { Some(n.active != 0) } else { None };
                let b = Bcast::adjacent_site(reg, bo, sc, n.syscode, act, self.cfg.priority, n.prio_adjacent, n.chan);
                return Some((b.encode(), DT_CSBK, None));
            }
        }
        if self.cfg.chan_freq_ms > 0 && now >= self.next_chan_freq_ms {
            let m = self.chan_freq_count();
            let (chan, tx_hz, rx_hz) = self.chan_freq_at(self.chan_freq_idx % m);
            self.chan_freq_idx = (self.chan_freq_idx + 1) % m;
            self.next_chan_freq_ms = now + (self.cfg.chan_freq_ms as i64 / m as i64).max(300);
            let h = Bcast::chan_freq(reg, bo, sc, chan);
            // the MS's view: it transmits on the site's receive frequency
            let ap = BcAp { chan, tx_hz: rx_hz, rx_hz: tx_hz };
            return Some((h.encode(), DT_MBC_HDR, Some(ap.encode())));
        }
        if self.cfg.local_time_ms > 0 && self.cfg.clock_ok != 0 && now >= self.next_local_time_ms {
            self.next_local_time_ms = now + self.cfg.local_time_ms as i64;
            let off = self.cfg.utc_offset_min as i64;
            let t = civil_from_ms(now + off * 60_000);
            let b = Bcast::local_time(reg, bo, sc, &t, Some(off as i32));
            return Some((b.encode(), DT_CSBK, None));
        }
        if self.cfg.gen_site_ms > 0 && now >= self.next_gen_site_ms {
            self.next_gen_site_ms = now + self.cfg.gen_site_ms as i64;
            let b = Bcast::gen_site_params(reg, bo, sc, self.about_to_hibernate(), false);
            return Some((b.encode(), DT_CSBK, None));
        }
        None
    }

    /// The TSCC's block for slot k: an MBC continuation, an Aloha every fourth block at least, the answers, grant
    /// repeats, broadcasts, else an Aloha.
    fn tscc_slot(&mut self, out: &mut SlotOut) {
        let (dt, pdu, solicit) = if self.cont_pending {
            self.cont_pending = false;
            (DT_MBC_CONT, self.cont, false)
        } else if let Some(a) = self.vote_blocked() {
            (DT_CSBK, a, false)
        } else if let Some(b) = self.sd_cont() {
            b // a UDT's next block, or a slot kept for an upload
        } else if self.since_aloha >= 3 || (self.since_aloha == 2 && self.resp_len > 0 && self.resp[self.resp_head].has_cont) {
            // an MBC's two blocks must end before the Aloha that's due: it waits a slot
            (DT_CSBK, self.aloha(), false)
        } else if self.resp_len > 0 {
            let r = self.resp[self.resp_head];
            self.resp_head = (self.resp_head + 1) % RESP;
            self.resp_len -= 1;
            if r.rand {
                // the slots' last six digits: the line fits an event's 64 octets
                let (k, n, src) = (r.rand_k.rem_euclid(1_000_000), self.k - r.rand_k, r.rand_src);
                self.log(format_args!("C_RAND from {src} in slot {k}: answered +{n} ({} ms)", n * 30));
            }
            if r.has_cont {
                self.cont_pending = true;
                self.cont = r.cont;
            }
            (r.dt, r.pdu, r.solicit)
        } else if let Some(b) = self.sd_start() {
            b
        } else if let Some(g) = self.due_grant() {
            (DT_CSBK, g, false)
        } else if let Some((b, dt, cont)) = if self.since_aloha < 2 { self.due_bcast() } else { None } {
            if let Some(c) = cont {
                self.cont_pending = true;
                self.cont = c;
            }
            (dt, b, false)
        } else {
            (DT_CSBK, self.aloha(), false)
        };
        if dt == DT_CSBK && opcode(&pdu) == OP_ALOHA {
            self.since_aloha = 0;
        } else {
            self.since_aloha += 1;
        }
        out.tscc_dt = dt;
        out.tscc_pdu = pdu;
        out.tscc_ext = self.sd_ext();
        out.at = solicit as u8;
    }

    // ── payload channels ──

    /// A free payload channel: an idle one, preferring a carrier already keyed (its other slot in use).
    fn free_channel(&self) -> Option<usize> {
        let keyed = |i: usize| matches!(self.chans[i ^ 1].r#use, USE_CALL | USE_HANG | USE_CLEAR | USE_TSCC);
        (0..CHANNELS)
            .filter(|&i| self.chans[i].r#use == USE_IDLE)
            .min_by_key(|&i| (!keyed(i), i))
            .or_else(|| self.composite_channel())
    }

    /// Opens a call on channel ch and queues its grant twice.
    #[allow(clippy::too_many_arguments)]
    fn open(&mut self, ch: usize, kind: u8, src: u32, dst: u32, flags: u8, stream: u32, talking_net: bool) {
        let now = self.now;
        let timer = if flags & F_EMERGENCY != 0 { self.cfg.emergency_call_ms } else { self.cfg.call_ms } as i64;
        self.chans[ch] = Chan {
            r#use: if talking_net { USE_CALL } else { USE_HANG },
            kind,
            flags,
            talking: talking_net,
            talking_net,
            src,
            dst,
            stream,
            talker: if talking_net { src } else { 0 },
            since_ms: now,
            call_end_ms: now + timer,
            idle_until_ms: now + self.cfg.start_grace_ms as i64 + Self::hang_of(&self.tgpol, self.cfg.hang_ms, kind, dst),
            grant_next_ms: now + self.cfg.grant_every_ms as i64,
            last_voice_ms: now,
            ..Default::default()
        };
        let g = self.grant_pdu(ch, false, src);
        if ch == self.tscc_carrier * 2 {
            self.revert(g); // a composite TSCC: the grant on its own slot, then the call
        } else {
            self.push(DT_CSBK, g, false, None);
            self.push(DT_CSBK, g, false, None);
        }
        self.counters.grants += 1;
        let e = self.chan_ev(ch, EV_CH_OPEN);
        self.ev(e);
        let mut a = self.chan_ev(ch, EV_ACTIVITY);
        a.kind = ACT_GRANT;
        self.ev(a);
    }

    /// A grant of an open channel again, twice: a party asked again, or another member asked to talk.
    fn regrant(&mut self, ch: usize, src: u32) {
        let now = self.now;
        let hang = Self::hang_of(&self.tgpol, self.cfg.hang_ms, self.chans[ch].kind, self.chans[ch].dst);
        let c = &mut self.chans[ch];
        if c.r#use == USE_HANG {
            c.idle_until_ms = c.idle_until_ms.max(now + self.cfg.start_grace_ms as i64 + hang);
        }
        let source = if self.chans[ch].kind == KIND_PRIVATE { self.chans[ch].src } else { src };
        let g = self.grant_pdu(ch, false, source);
        self.push(DT_CSBK, g, false, None);
        self.push(DT_CSBK, g, false, None);
        let mut a = self.chan_ev(ch, EV_ACTIVITY);
        a.kind = ACT_GRANT;
        a.src = src;
        a.flags |= F_LATE;
        self.ev(a);
    }

    /// Starts clearing a call: P_CLEAR in the channel's next slots, then the channel closes.
    fn clear(&mut self, ch: usize, why: u8) {
        let c = &mut self.chans[ch];
        if !Self::live(c) {
            return;
        }
        let (stream, net) = (c.stream, c.talking_net);
        c.r#use = USE_CLEAR;
        c.clear_left = self.cfg.clear_repeats;
        c.close_why = why;
        c.talking = false;
        c.talking_net = false;
        if net && stream != 0 {
            self.drop_stream(stream, WHY_CONGESTION);
        }
        self.duplex_clear(ch, why);
    }

    fn close(&mut self, ch: usize) {
        self.pa_closed(ch);
        let mut e = self.chan_ev(ch, EV_CH_CLOSE);
        e.why = self.chans[ch].close_why;
        self.ev(e);
        let carrier = ch / 2;
        let on = carrier < self.cfg.ncarriers as usize && self.cfg.carriers[carrier].enabled != 0;
        self.chans[ch] = Chan { r#use: if on { USE_IDLE } else { USE_OFF }, ..Default::default() };
        if ch == self.tscc_carrier * 2 {
            self.tscc_back();
        }
        self.serve_queue();
        self.pd_serve();
    }

    /// The group call (or all-call) on tg, if one is open.
    fn group_channel(&self, tg: u32) -> Option<usize> {
        (0..CHANNELS).find(|&i| {
            let c = &self.chans[i];
            Self::live(c) && matches!(c.kind, KIND_GROUP | KIND_ALLCALL) && c.dst == tg && c.flags & F_DATA == 0
        })
    }

    fn private_channel(&self, a: u32, b: u32) -> Option<usize> {
        (0..CHANNELS).find(|&i| {
            let c = &self.chans[i];
            Self::live(c)
                && c.kind == KIND_PRIVATE
                && c.flags & F_DATA == 0
                && ((c.src == a && c.dst == b) || (c.src == b && c.dst == a))
        })
    }

    /// Whether an MS is in an individual call, or a party of one being set up.
    fn ms_busy(&self, ms: u32) -> bool {
        let ind = |kind| kind == KIND_PRIVATE || kind == KIND_DUPLEX;
        self.chans.iter().any(|c| Self::live(c) && ind(c.kind) && (c.src == ms || c.dst == ms))
            || self.pend.iter().any(|p| p.used && ind(p.kind) && (p.called == ms || p.caller == ms))
    }

    fn payload_slot(&mut self, out: &mut SlotOut) {
        let lcn = (self.k & 1) as usize;
        for ch in 0..CHANNELS {
            out.ch[ch].r#use = self.chans[ch].r#use;
            if ch % 2 == lcn && self.chans[ch].protect_ms != 0 {
                self.protect_slot(ch, &mut out.ch[ch]);
            }
            if ch % 2 == lcn {
                self.pa_slot(ch, &mut out.ch[ch]);
            }
            if ch % 2 != lcn || self.chans[ch].r#use != USE_CLEAR {
                continue;
            }
            let c = Clear { chan: 0, group: false, target: ALLMSI, source: TSI };
            out.ch[ch].dt = DT_CSBK;
            out.ch[ch].pdu = c.encode();
            self.chans[ch].clear_left = self.chans[ch].clear_left.saturating_sub(1);
            if self.chans[ch].clear_left == 0 && !self.still_keyed(ch) {
                self.close(ch);
            }
        }
    }

    fn channel_timers(&mut self) {
        let now = self.now;
        for ch in 0..CHANNELS {
            let c = self.chans[ch];
            if !Self::live(&c) {
                continue;
            }
            if now >= c.call_end_ms {
                if !self.timer_dekey(ch) {
                    self.clear(ch, CLOSE_TIMER); // or after its talker is de-keyed (6.4.14.3.2, t3intr.rs)
                }
                continue;
            }
            if c.talking && !c.talking_net && now - c.last_voice_ms > VOICE_LOST_MS {
                let hang = Self::hang_of(&self.tgpol, self.cfg.hang_ms, c.kind, c.dst);
                let c = &mut self.chans[ch];
                c.talking = false;
                c.r#use = USE_HANG;
                c.idle_until_ms = now + hang;
            }
            let c = self.chans[ch];
            if !c.talking && now >= c.idle_until_ms && !self.duplex_held(ch) {
                self.clear(ch, CLOSE_HANG);
            }
        }
    }

    // ── the queue ──

    /// Gives free channels to the waiting requests: emergency first, then priority, then age.
    fn serve_queue(&mut self) {
        loop {
            let Some(ch) = self.free_channel() else { return };
            let mut best: Option<((bool, u8, i64), bool, usize)> = None; // key, is_net, index
            for (i, p) in self.pend.iter().enumerate() {
                let key = (p.svc & SO_EMERGENCY != 0, p.prio, p.since_ms);
                if p.used && p.state == P_QUEUED && better(&key, &best) {
                    best = Some((key, false, i));
                }
            }
            for (i, n) in self.net.iter().enumerate() {
                let key = (n.emergency, 0, n.since_ms);
                if n.used && better(&key, &best) {
                    best = Some((key, true, i));
                }
            }
            let Some((_, is_net, i)) = best else { return };
            if is_net {
                let n = self.net[i];
                self.net[i].used = false;
                self.activity(ACT_QUEUE, QUEUE_OUT, n.src, n.tg, 0);
                self.net_group_go(n.stream, n.src, n.tg, n.emergency, Some(ch));
            } else {
                let p = self.pend[i];
                self.pend[i].used = false;
                self.activity(ACT_QUEUE, QUEUE_OUT, p.caller, p.called, 0);
                if p.kind == KIND_PRIVATE {
                    self.private_go(&p, ch);
                } else {
                    self.group_go(p.caller, p.called, p.svc, ch);
                }
            }
        }
    }

    fn free_pend(&self) -> Option<usize> {
        self.pend.iter().position(|p| !p.used)
    }

    /// Queues a request for a channel (C_QACKD to a local caller), or refuses it when the queue is full.
    fn enqueue(&mut self, mut p: Pend) {
        let full = self.queue_len() >= self.cfg.queue_max as usize && !self.preempts(p.svc);
        match (full, self.free_pend()) {
            (false, Some(i)) => {
                p.used = true;
                p.state = P_QUEUED;
                p.deadline_ms = self.now + self.cfg.queue_ms as i64;
                p.next_ack_ms = self.now + QACK_EVERY_MS;
                if p.caller_local {
                    self.ack(R_QUEUED, p.caller, p.called);
                }
                self.pend[i] = p;
                self.counters.queued += 1;
                self.activity(ACT_QUEUE, QUEUE_IN, p.caller, p.called, 0);
            }
            _ => {
                self.fail(&p, R_SYSBUSY, WHY_CONGESTION);
                self.activity(ACT_QUEUE, QUEUE_DROP, p.caller, p.called, R_SYSBUSY);
            }
        }
    }

    /// Ends a request that failed: C_NACKD to a local caller (and a cancel to the network if it was asked), a
    /// reject to a network caller, a drop of the voice waiting for it.
    fn fail(&mut self, p: &Pend, reason: u8, why: u8) {
        if p.caller_local {
            self.refuse(reason, p.caller, p.called);
            if !p.called_local && p.state == P_NET {
                self.setup_out(SETUP_CANCEL, WHY_OK, 0, p.svc, p.caller, p.called, p.stream);
            }
        } else {
            if p.stream != 0 {
                self.setup_out(SETUP_REJECT, why, 0, p.svc, p.caller, p.called, p.stream);
            }
            if p.net_stream != 0 {
                self.drop_stream(p.net_stream, why);
            }
        }
    }

    /// A setup message to publish. A request carries its own stream; the others carry the request's as ref.
    #[allow(clippy::too_many_arguments)]
    fn setup_out(&mut self, op: u8, reason: u8, flags: u8, svc: u8, src: u32, dst: u32, request: u32) {
        let (stream, r#ref) = if op == SETUP_REQUEST { (request, 0) } else { (self.new_stream(), request) };
        self.ev(Event { r#type: EV_NET_SETUP, op, reason, flags, svc, src, dst, stream, r#ref, ..Default::default() });
        self.activity(ACT_SETUP, op, src, dst, reason);
    }

    // ── registration ──

    fn write_record(&mut self, i: usize) {
        self.regs[i].written_ms = self.now;
        let id = self.regs[i].rec.id;
        self.ev(Event { r#type: EV_RECORD, src: id, ..Default::default() });
    }

    fn delete_record(&mut self, i: usize, op: u8) {
        let id = self.regs[i].rec.id;
        self.regs[i] = Reg::default();
        self.tgl_forget(id);
        self.nregs = self.nregs.saturating_sub(1);
        self.subs_dirty = true;
        self.ev(Event { r#type: EV_RECORD, why: 1, src: id, ..Default::default() });
        self.activity(ACT_REG, op, id, 0, 0);
    }

    /// The record of ms, made when there is none; None when the table is full.
    fn record_for(&mut self, ms: u32, how: u8) -> Option<usize> {
        if let Some(i) = self.reg_index(ms) {
            return Some(i);
        }
        let i = self.regs.iter().position(|r| !r.used)?;
        let shift = model_bits(self.cfg.model).1.saturating_sub(self.cfg.area_bits as u32);
        let area = (self.cfg.site >> shift) as u8;
        let exp_ms = self.now + self.cfg.reg_hold_s as i64 * 1000;
        self.regs[i] = Reg {
            used: true,
            rec: Record { id: ms, how, area, at_ms: self.now, exp_ms, ..Default::default() },
            written_ms: i64::MIN / 2,
            ..Default::default()
        };
        self.nregs += 1;
        self.subs_dirty = true;
        Some(i)
    }

    /// Any PDU from an MS keeps its record alive.
    fn heard(&mut self, ms: u32) {
        if let Some(i) = self.reg_index(ms) {
            let hold = self.cfg.reg_hold_s as i64 * 1000;
            let r = &mut self.regs[i];
            r.rec.at_ms = self.now;
            r.rec.exp_ms = self.now + hold;
            r.demanded = false;
            if self.now - r.written_ms >= RECORD_REFRESH_MS {
                self.write_record(i);
            }
        }
    }

    fn allowed(&self, ms: u32) -> bool {
        self.ms_policy(ms).enabled != 0 && (self.policy.open != 0 || self.policy.ms.contains(ms))
    }

    fn registration(&mut self, r: &Rand) {
        let ms = r.source;
        if r.service_options & SO_REG == 0 && r.service_options & SO_IP_INFORM != 0 {
            self.ip_advice(r); // 6.4.11.0 b): deleting an IP connection; it stays registered
            return;
        }
        if r.service_options & SO_REG == 0 {
            // deregistration: the only answer is Reg_Accepted
            self.ack(R_REG_ACCEPTED, ms, REGI);
            if let Some(i) = self.reg_index(ms) {
                self.delete_record(i, REG_DEREGISTER);
            }
            return;
        }
        let cnts = r.trgt_adr_cnts();
        if cnts == 0 && r.target != self.syscode as u32 {
            return; // a registration for another system: discard
        }
        if cnts == 3 {
            self.refuse(R_NOT_SUPPORTED, ms, REGI);
            return;
        }
        let from = if cnts == 2 { TATTSI } else { REGI }; // a list's refusal comes from TATTSI (6.4.4.1.13.2)
        if !self.allowed(ms) {
            let (reason, op) =
                if self.policy.refuse_unlisted != 0 { (R_REG_REFUSED, REG_REFUSE) } else { (R_REG_DENIED, REG_DENY) };
            self.ack(reason, ms, from);
            self.activity(ACT_REG, op, ms, 0, reason);
            if let Some(i) = self.reg_index(ms) {
                self.delete_record(i, op);
            }
            return;
        }
        if self.auth_registration(r) {
            return; // challenged (t3authsite.rs): register() follows a good answer
        }
        self.register(r);
    }

    /// A registration that passed the checks (and the authentication, if any): the record, the subscription, the
    /// answer.
    fn register(&mut self, r: &Rand) {
        let (ms, cnts) = (r.source, r.trgt_adr_cnts());
        let from = if cnts == 2 { TATTSI } else { REGI }; // a list's refusal comes from TATTSI (6.4.4.1.13.2)
        let Some(i) = self.record_for(ms, HOW_REGISTER) else {
            self.ack(R_REG_REFUSED, ms, from);
            self.activity(ACT_REG, REG_REFUSE, ms, 0, R_REG_REFUSED);
            return;
        };
        let hold = self.cfg.reg_hold_s as i64 * 1000;
        self.regs[i].demanded = false;
        self.demand_answered(ms);
        let rec = &mut self.regs[i].rec;
        rec.how = HOW_REGISTER;
        rec.at_ms = self.now;
        rec.exp_ms = self.now + hold;
        let mut reason = R_REG_ACCEPTED;
        match cnts {
            1 if r.target == ADRNULL => {
                // ADRNULL removes the subscription, a list's too
                self.tgl_clear_list(ms);
                let rec = &mut self.regs[i].rec;
                let old = rec.sub_tg;
                rec.sub_tg = 0;
                if old != 0 {
                    rec.remove_tg(old);
                }
            }
            1 => {
                let tg = r.target;
                self.tgl_clear_list(ms); // a single subscription replaces a list
                if self.policy.tg.contains(tg) && !is_gateway(tg) && !is_allcall(tg) {
                    let rec = &mut self.regs[i].rec;
                    let old = rec.sub_tg;
                    if old != 0 && old != tg {
                        rec.remove_tg(old);
                    }
                    rec.sub_tg = tg;
                    rec.touch_tg(tg);
                } else {
                    reason = R_REG_SUBSCRIPTION; // registered, the talkgroup refused
                }
            }
            2 => {
                // the list comes in a UDT upload (C_AHOY from TATTSI); the C_ACK follows it (tier3.md 3.17)
                self.subs_dirty = true;
                self.write_record(i);
                self.tgl_register(r);
                return;
            }
            _ => {}
        }
        self.subs_dirty = true;
        if r.service_options & SO_IP_INFORM != 0 && reason == R_REG_ACCEPTED {
            self.ip_advice(r); // 6.4.11.2: the C_AHOY from IPI answers it, the final C_ACKD comes from IPI
        } else {
            self.reg_ack(i, r, reason); // with PowerSave_Offset
        }
        self.write_record(i);
        let sub = self.regs[i].rec.sub_tg;
        self.activity(ACT_REG, REG_REGISTER, ms, sub, reason);
    }

    // ── IP connection advice (6.4.11) ──

    /// A registration with IP_Inform 1, accepted (Reg_Dereg 1, adding an address), or Reg_Dereg 0 (deleting one:
    /// not a deregistration, 6.4.11.0 b). With services.ip_address_upload, the C_AHOY from IPI asks for the address
    /// (6.4.11.2.1 c): the registration is then taken as done. Without it, or for an MS not registered here when it
    /// deletes, C_NACKD(IP_Connection_failed) from IPI (6.4.11.2.2 c): registered, no IP service.
    fn ip_advice(&mut self, r: &Rand) {
        let ms = r.source;
        if self.sd_again(SD_IPADV, ms) {
            return; // it missed the C_AHOY: again
        }
        let Some(i) = self.reg_index(ms) else {
            self.refuse(R_IP_CONN_FAILED, ms, IPI);
            return;
        };
        if self.cfg.ip_upload == 0 || !self.sd_upload_new(SD_IPADV, ms, IPI, false, r.service_options, 1) {
            if r.service_options & SO_REG != 0 {
                self.ip_no_ps(i); // no Reg_Accepted: no PowerSave_Offset granted
            }
            if self.cfg.ip_upload == 0 {
                self.refuse(R_IP_CONN_FAILED, ms, IPI);
            }
        }
    }

    fn ip_no_ps(&mut self, i: usize) {
        let g = &mut self.regs[i];
        (g.rec.ps_rq, g.rec.ps_off) = (0, 0);
        g.awake_ms = self.now + self.cfg.awake_ms as i64;
    }

    /// An IP connection advice's upload ended (so the C_RAND's Service_Options; ip None when it failed): the final
    /// answer from IPI (6.4.11.2.3). Added: C_ACKD(Reg_Accepted) with PowerSave_Offset, as a registration's; an
    /// address another MS holds, or no room for it, IP_Connection_failed ("the IP address is already in use by
    /// another MS", 6.4.11.1.4). Deleted: C_ACKD(Reg_Accepted) with the offset it has; an address it doesn't hold,
    /// IP_Connection_failed.
    pub(super) fn ip_advice_done(&mut self, ms: u32, so: u8, ip: Option<u32>) {
        let Some(i) = self.reg_index(ms) else { return }; // deregistered meanwhile: nothing to answer
        let add = so & SO_REG != 0;
        let ok = match ip {
            Some(ip) if add => {
                let other = (0..REGS).any(|j| j != i && self.regs[j].used && self.regs[j].ips.contains(&ip));
                let ips = &mut self.regs[i].ips;
                if other {
                    false
                } else if ips.contains(&ip) {
                    true
                } else if let Some(s) = ips.iter_mut().find(|a| **a == 0) {
                    *s = ip;
                    true
                } else {
                    false
                }
            }
            Some(ip) => match self.regs[i].ips.iter_mut().find(|a| **a == ip) {
                Some(s) => {
                    *s = 0;
                    true
                }
                None => false,
            },
            None => false,
        };
        let [a, b, c, d] = ip.unwrap_or(0).to_be_bytes();
        let what = match (ok, add) {
            (true, true) => "added",
            (true, false) => "deleted",
            _ => "refused",
        };
        self.log(format_args!("IP connection advice from {ms}: {a}.{b}.{c}.{d} {what}"));
        if !ok {
            if add {
                self.ip_no_ps(i);
            }
            self.refuse(R_IP_CONN_FAILED, ms, IPI);
            return;
        }
        if add {
            let r = Rand { service_options: so, source: ms, ..Default::default() };
            self.reg_ack_from(i, &r, R_REG_ACCEPTED, IPI);
        } else {
            let off = self.regs[i].rec.ps_off;
            let a = Ack { op: OP_ACKD, response_info: off, reason: R_REG_ACCEPTED, target: ms, source: IPI };
            self.push(DT_CSBK, a.encode(), false, None);
        }
    }

    /// The MS that advised IPv4 address ip here (6.4.11.0: "cross reference the IP address with the MS individual
    /// address").
    pub fn ip_owner(&self, ip: u32) -> Option<u32> {
        if ip == 0 {
            return None;
        }
        (0..REGS).find(|&j| self.regs[j].used && self.regs[j].ips.contains(&ip)).map(|j| self.regs[j].rec.id)
    }

    // ── service requests ──

    fn service(&mut self, r: &Rand) {
        if self.supp_gate(r) {
            return; // stunned, diverted, or a diversion request
        }
        match r.service_kind {
            SK_IND_VOICE | SK_TG_VOICE => self.call_request(r),
            SK_CALL_ANSWER => self.call_answer(r),
            SK_CANCEL => self.cancel(r.source),
            SK_STATUS => self.sd_rand(r), // status delivery and polling (6.6.6; t3sdata.rs, t3status.rs)
            SK_DUPLEX => self.duplex_request(r),
            SK_IND_UDT | SK_TG_UDT | crate::t3udt::SK_UDT_POLL => self.sd_rand(r),
            SK_SUPPL if r.target == DGNAI => self.sd_rand(r),
            SK_IND_PD | SK_TG_PD => self.pd_rand(r),
            _ => self.refuse(R_NOT_SUPPORTED, r.source, r.target),
        }
    }

    /// The checks every call request passes: Some(priority) when it may go on.
    fn admit(&mut self, r: &Rand) -> Option<u8> {
        let ms = r.source;
        let group = matches!(r.service_kind, SK_TG_VOICE | SK_TG_PD);
        let site = self.pd_to_site(r); // data to the site itself
        let not_ms = !group && (r.target == ms || is_allcall(r.target));
        if (is_gateway(r.target) && !site) || r.target == ADRNULL || not_ms {
            self.refuse(R_NOT_SUPPORTED, ms, r.target);
            return None;
        }
        let pol = self.ms_policy(ms);
        if !self.allowed(ms) {
            self.refuse(R_PERM_USER_REFUSED, ms, r.target);
            return None;
        }
        if self.reg_index(ms).is_none() {
            if self.reg_bit() || self.auth_needed(ms) {
                self.refuse(R_MS_NOT_REGISTERED, ms, r.target);
                return None;
            }
            match self.record_for(ms, HOW_CALL) {
                Some(i) => self.write_record(i),
                None => {
                    self.refuse(R_SYSBUSY, ms, r.target);
                    return None;
                }
            }
        }
        let emergency = r.service_options & SO_EMERGENCY != 0;
        if emergency && pol.emergency_ok == 0 {
            self.refuse(R_PERM_USER_REFUSED, ms, r.target);
            return None;
        }
        if group {
            if !is_allcall(r.target) && !self.policy.tg.contains(r.target) {
                self.refuse(R_GROUP_NOT_ALLOWED, ms, r.target);
                return None;
            }
        } else if pol.private_ok == 0 || !self.policy.private_ms.contains(ms) {
            self.refuse(R_PERM_USER_REFUSED, ms, r.target);
            return None;
        }
        let tgp = if group { Self::tg_policy(&self.tgpol, r.target).map_or(0, |t| t.priority) } else { 0 };
        Some(if emergency { 4 } else { (r.service_options & SO_PRIORITY).max(pol.priority.min(3)).max(tgp.min(3)) })
    }

    fn call_request(&mut self, r: &Rand) {
        let (ms, dst) = (r.source, r.target);
        let group = r.service_kind == SK_TG_VOICE;
        // a repeat of a request in progress: answer it again
        if let Some(i) = self.pend.iter().position(|p| p.used && p.caller_local && p.caller == ms && p.called == dst) {
            match self.pend[i].state {
                P_QUEUED => self.ack(R_QUEUED, ms, dst),
                P_ALERTING => self.ack(R_MS_ALERTING, ms, dst),
                _ => self.ack(R_WAIT, ms, dst),
            }
            return;
        }
        let Some(prio) = self.admit(r) else { return };
        if group {
            if !is_allcall(dst) {
                if let Some(i) = self.reg_index(ms) {
                    if self.regs[i].rec.touch_tg(dst) {
                        self.subs_dirty = true;
                        self.write_record(i);
                    }
                }
            }
            let svc = r.service_options;
            if self.group_channel(dst).is_none() && svc & SO_EMERGENCY == 0 {
                if let (Some(d @ 1..), Some(i)) = (self.ps_group_wait(dst), self.free_pend()) {
                    // members asleep: the grant waits for their wake frame (6.4.7.1), C_WACKD meanwhile
                    self.pend[i] = Pend {
                        used: true,
                        kind: KIND_GROUP,
                        state: P_WAKE,
                        svc,
                        prio,
                        caller_local: true,
                        called_local: true,
                        caller: ms,
                        called: dst,
                        since_ms: self.now,
                        deadline_ms: self.now + d * 30,
                        next_ack_ms: self.now + WACK_EVERY_MS,
                        ..Default::default()
                    };
                    self.ack(R_WAIT, ms, dst);
                    let ms = d * 30;
                    self.log(format_args!("power save: the call to {dst} waits {ms} ms for its members' wake frame"));
                    return;
                }
            }
            self.group_start(ms, dst, svc, prio);
            return;
        }
        // individual
        if let Some(ch) = self.private_channel(ms, dst) {
            self.regrant(ch, ms);
            return;
        }
        if self.ms_busy(dst) {
            self.refuse(R_CALLED_PARTY_BUSY, ms, dst);
            return;
        }
        let Some(i) = self.free_pend() else {
            self.refuse(R_SYSBUSY, ms, dst);
            return;
        };
        let mut p = Pend {
            used: true,
            kind: KIND_PRIVATE,
            svc: r.service_options,
            prio,
            caller_local: true,
            caller: ms,
            called: dst,
            since_ms: self.now,
            next_ack_ms: self.now + WACK_EVERY_MS,
            als: r.dep & DEP_ALS != 0,
            ..Default::default()
        };
        if self.reg_index(dst).is_some() {
            p.called_local = true;
            p.state = P_AHOY;
            p.deadline_ms = self.now + self.cfg.tp_ms as i64;
            p.ahoy_left = self.cfg.ahoy_tries;
            p.next_ahoy_ms = self.now;
            self.ack(R_WAIT, ms, dst);
            self.pend[i] = p;
            self.ahoy_tick(i);
        } else if self.connected && self.ms_policy(ms).wide_private != 0 {
            p.state = P_NET;
            p.stream = self.new_stream();
            p.deadline_ms = self.now + self.cfg.setup_ms as i64;
            self.ack(R_WAIT, ms, dst);
            // an ALS call's radio check is OACSU's (table 6.46): no FOACSU
            let flags = if p.als {
                SETUP_F_ALS
            } else if self.cfg.foacsu != 0 {
                SETUP_F_FOACSU
            } else {
                0
            };
            self.pend[i] = p;
            self.setup_out(SETUP_REQUEST, WHY_OK, flags, p.svc, ms, dst, p.stream);
        } else {
            self.refuse(R_NOREG_MSAWAY, ms, dst);
        }
    }

    /// A local group call: its channel (a free one, or the one the talkgroup is on), or the queue.
    fn group_start(&mut self, ms: u32, dst: u32, svc: u8, prio: u8) {
        match self.group_channel(dst).or_else(|| self.free_channel()) {
            Some(ch) => self.group_go(ms, dst, svc, ch),
            None => self.enqueue(Pend {
                kind: KIND_GROUP,
                svc,
                prio,
                caller_local: true,
                called_local: true,
                caller: ms,
                called: dst,
                since_ms: self.now,
                ..Default::default()
            }),
        }
    }

    fn group_go(&mut self, src: u32, tg: u32, svc: u8, ch: usize) {
        if let Some(c) = self.group_channel(tg) {
            if svc & SO_EMERGENCY != 0 {
                self.emergency_upgrade(c);
            }
            self.regrant(c, src);
            return;
        }
        let mut flags = F_GROUP;
        let kind = if is_allcall(tg) {
            flags |= F_ALLCALL;
            KIND_ALLCALL
        } else if svc & SO_BROADCAST != 0 {
            KIND_ALLCALL // a broadcast call to a talkgroup (BCAST_SV, 6.6.2): BTV_GRANT, one way
        } else {
            KIND_GROUP
        };
        if svc & SO_EMERGENCY != 0 {
            flags |= F_EMERGENCY;
        }
        if tg == ALLMSID || (!is_allcall(tg) && self.policy.wide_tg.contains(tg)) {
            flags |= F_WIDE;
        }
        self.open(ch, kind, src, tg, flags, 0, false);
    }

    /// An individual call both parties accepted, on channel ch.
    fn private_go(&mut self, p: &Pend, ch: usize) {
        let mut flags = 0;
        if p.svc & SO_EMERGENCY != 0 {
            flags |= F_EMERGENCY;
        }
        if !p.caller_local || !p.called_local {
            flags |= F_PRIVATE_NET | F_WIDE;
        }
        if !p.caller_local {
            flags |= F_NET;
        }
        let talking = !p.caller_local && p.net_stream != 0; // a network caller already talking
        self.open(ch, KIND_PRIVATE, p.caller, p.called, flags, p.net_stream, talking);
        self.chans[ch].als = p.als;
    }

    /// Both parties ready: a channel now, or the queue.
    fn private_ready(&mut self, i: usize) {
        let p = self.pend[i];
        self.pend[i].used = false;
        if p.kind == KIND_DUPLEX {
            self.duplex_ready(&p);
            return;
        }
        match self.free_channel() {
            Some(ch) => self.private_go(&p, ch),
            None => self.enqueue(p),
        }
    }

    fn ahoy_tick(&mut self, i: usize) {
        let p = self.pend[i];
        if !self.ps_awake(p.called) {
            // asleep (power save): held for its wake frame, the time limit stretched to it
            self.pend[i].deadline_ms = p.deadline_ms.max(self.now + self.ps_wait_ms(p.called));
            return;
        }
        if p.ahoy_left == 0 {
            self.pend[i].used = false;
            self.fail(&p, R_MSAWAY, WHY_NO_ANSWER);
            return;
        }
        // an ALS radio check (table 6.46): Service_Options_Mirror 0, Service_Kind_Flag 0, ALS_SERV 1
        let a = Ahoy {
            service_options: if p.als { 0 } else { p.svc },
            service_kind_flag: self.cfg.foacsu != 0 && !p.als,
            als: p.als,
            group: false,
            appended: 0,
            service_kind: if p.kind == KIND_DUPLEX { SK_DUPLEX } else { SK_IND_VOICE },
            target: p.called,
            source: p.caller,
        };
        self.push(DT_CSBK, a.encode(), true, None);
        let q = &mut self.pend[i];
        q.ahoy_left -= 1;
        q.next_ahoy_ms = self.now + self.cfg.ahoy_every_ms as i64;
    }

    /// C_ACKU or C_NACKU from a called MS: its source is the AHOY's target.
    fn ms_ack(&mut self, a: &Ack) {
        self.heard(a.source);
        if self.sd_ack(a) || self.pd_ack(a) || self.supp_ack(a) {
            return;
        }
        let Some(i) =
            self.pend.iter().position(|p| p.used && p.state == P_AHOY && p.called == a.source && p.caller == a.target)
        else {
            return;
        };
        let p = self.pend[i];
        self.pend[i].answered = true;
        match (reason_type(a.reason), a.reason) {
            (1, R_MS_ALERTING) => {
                let tp = self.cfg.tp_ms as i64;
                let q = &mut self.pend[i];
                q.state = P_ALERTING;
                q.deadline_ms = self.now + tp;
                if p.caller_local {
                    self.ack(R_MS_ALERTING, p.caller, p.called); // mirrored
                } else if p.stream != 0 {
                    self.setup_out(SETUP_ALERTING, WHY_OK, SETUP_F_FOACSU, p.svc, p.caller, p.called, p.stream);
                }
            }
            (1, R_CALLBACK) => {
                self.pend[i].used = false;
                if p.caller_local {
                    self.ack(R_CALLBACK, p.caller, p.called);
                } else {
                    self.fail(&p, R_MSAWAY, WHY_REFUSED);
                }
            }
            (1, _) => {
                if !p.caller_local && p.stream != 0 {
                    self.setup_out(SETUP_ACCEPT, WHY_OK, 0, p.svc, p.caller, p.called, p.stream);
                }
                self.private_ready(i);
            }
            _ => {
                self.pend[i].used = false;
                let why = match a.reason {
                    R_EQUIP_BUSY => WHY_BUSY,
                    crate::t3udt::R_MS_NOT_SUPPORTED => WHY_NOT_SUPPORTED, // an ALS call to an MS without it
                    _ => WHY_REFUSED,
                };
                self.fail(&p, a.reason, why); // the called MS's reason, mirrored
            }
        }
    }

    /// Call Answer (FOACSU) from the called MS.
    fn call_answer(&mut self, r: &Rand) {
        let called = r.source;
        let Some(i) =
            self.pend.iter().position(|p| p.used && p.state == P_ALERTING && p.called == called && p.caller == r.target)
        else {
            self.refuse(R_NOT_SUPPORTED, called, r.target);
            return;
        };
        let p = self.pend[i];
        if r.accepted() {
            if !p.caller_local && p.stream != 0 {
                self.setup_out(SETUP_ANSWERED, WHY_OK, 0, p.svc, p.caller, p.called, p.stream);
            }
            self.private_ready(i);
        } else {
            self.pend[i].used = false;
            self.ack(R_MESSAGE_ACCEPTED, called, p.caller);
            self.fail(&p, R_RECIPIENT_REFUSED, WHY_REFUSED);
        }
    }

    fn cancel(&mut self, ms: u32) {
        if self.sd_cancel(ms) || self.pd_cancel(ms) {
            return;
        }
        if let Some(i) = self.pend.iter().position(|p| p.used && p.caller_local && p.caller == ms) {
            let p = self.pend[i];
            self.pend[i].used = false;
            self.ack(R_MESSAGE_ACCEPTED, ms, p.called);
            if p.called_local && p.answered {
                // tell the called MS (it may be alerting)
                let a = Ahoy { service_kind: SK_CANCEL, target: p.called, source: p.caller, ..Default::default() };
                self.push(DT_CSBK, a.encode(), false, None);
            }
            if !p.called_local && matches!(p.state, P_NET | P_ALERTING) {
                self.setup_out(SETUP_CANCEL, WHY_OK, 0, p.svc, p.caller, p.called, p.stream);
            }
            if p.state == P_QUEUED {
                self.activity(ACT_QUEUE, QUEUE_DROP, p.caller, p.called, 0);
            }
            return;
        }
        if self.chans.iter().any(|c| Self::live(c) && c.src == ms) {
            self.refuse(R_CALL_CANCEL_REFUSED, ms, ADRNULL);
        } else {
            self.ack(R_MESSAGE_ACCEPTED, ms, ADRNULL);
        }
    }

    fn pending_timers(&mut self) {
        let now = self.now;
        for i in 0..PENDING {
            let p = self.pend[i];
            if !p.used {
                continue;
            }
            if p.state == P_WAKE && (now >= p.deadline_ms || self.ps_group_wait(p.called) == Some(0)) {
                self.pend[i].used = false; // the members' wake frame: the grant now
                self.group_start(p.caller, p.called, p.svc, p.prio);
                continue;
            }
            if now >= p.deadline_ms {
                self.pend[i].used = false;
                if p.state == P_QUEUED {
                    self.fail(&p, R_SYSBUSY, WHY_CONGESTION);
                    self.activity(ACT_QUEUE, QUEUE_DROP, p.caller, p.called, R_SYSBUSY);
                } else {
                    self.fail(&p, R_MSAWAY, WHY_TIMEOUT); // no answer within TP or setup_s: abandon
                }
                continue;
            }
            if p.state == P_AHOY && p.called_local && now >= p.next_ahoy_ms {
                self.ahoy_tick(i);
                if !self.pend[i].used {
                    continue;
                }
            }
            if p.caller_local && now >= p.next_ack_ms {
                let (reason, every) = match p.state {
                    P_QUEUED => (R_QUEUED, QACK_EVERY_MS),
                    P_ALERTING => (R_MS_ALERTING, WACK_EVERY_MS),
                    _ => (R_WAIT, WACK_EVERY_MS),
                };
                self.ack(reason, p.caller, p.called);
                self.pend[i].next_ack_ms = now + every;
            }
        }
        for i in 0..STREAMS {
            let n = self.net[i];
            if n.used && now >= n.deadline_ms {
                self.net[i].used = false;
                self.drop_stream(n.stream, WHY_CONGESTION);
                self.activity(ACT_QUEUE, QUEUE_DROP, n.src, n.tg, R_SYSBUSY);
            }
        }
    }

    fn reg_timers(&mut self) {
        // a few records a slot: all 1024 in about 4 s
        for _ in 0..8 {
            let i = self.reg_scan;
            self.reg_scan = (self.reg_scan + 1) % REGS;
            if self.regs[i].used && self.now >= self.regs[i].rec.exp_ms {
                // 6.4.5.0: before a record goes, a registration is demanded of its MS; one that answers keeps it
                let id = self.regs[i].rec.id;
                if self.regs[i].demanded {
                    self.delete_record(i, REG_EXPIRE);
                } else if self.demand_registration(id) {
                    self.regs[i].demanded = true;
                    self.regs[i].rec.exp_ms = self.now + DEMAND_GRACE_MS;
                } else {
                    self.regs[i].rec.exp_ms = self.now + DEMAND_RETRY_MS; // another under way
                }
            }
        }
    }

    // ── the network ──

    /// Whether the site carries tg: an all-call, a static talkgroup, or one a registered MS needs.
    fn wants_tg(&self, tg: u32) -> bool {
        if is_allcall(tg) || self.policy.static_tg.contains(tg) {
            return true;
        }
        self.regs.iter().any(|r| r.used && (r.rec.tgs().contains(&tg) || self.ms_policy(r.rec.id).tgs().contains(&tg)))
            || self.tgl_wants(tg)
    }

    fn net_group_go(&mut self, stream: u32, src: u32, tg: u32, emergency: bool, ch: Option<usize>) {
        let Some(ch) = ch.or_else(|| self.free_channel()) else {
            match self.net.iter().position(|n| !n.used) {
                Some(i) if self.cfg.net_queue_ms > 0 && self.queue_len() < self.cfg.queue_max as usize => {
                    self.net[i] = NetWait {
                        used: true,
                        emergency,
                        stream,
                        src,
                        tg,
                        since_ms: self.now,
                        deadline_ms: self.now + self.cfg.net_queue_ms as i64,
                    };
                    self.activity(ACT_QUEUE, QUEUE_IN, src, tg, 0);
                }
                _ => self.drop_stream(stream, WHY_CONGESTION),
            }
            return;
        };
        let mut flags = F_GROUP | F_NET | F_WIDE;
        let kind = if is_allcall(tg) {
            flags |= F_ALLCALL;
            KIND_ALLCALL
        } else {
            KIND_GROUP
        };
        if emergency {
            flags |= F_EMERGENCY;
        }
        self.open(ch, kind, src, tg, flags, stream, true);
    }

    /// A network stream now feeds open channel ch.
    fn attach(&mut self, ch: usize, stream: u32, src: u32) {
        let now = self.now;
        let c = &mut self.chans[ch];
        c.stream = stream;
        c.talking = true;
        c.talking_net = true;
        c.talker = src;
        c.r#use = USE_CALL;
        c.last_voice_ms = now;
        let mut e = self.chan_ev(ch, EV_CH_STREAM);
        e.src = src;
        self.ev(e);
    }

    fn net_end(&mut self, stream: u32) {
        let now = self.now;
        for c in self.chans.iter_mut() {
            if c.stream == stream && Self::live(c) {
                c.stream = 0;
                if c.talking_net {
                    c.talking = false;
                    c.talking_net = false;
                    c.r#use = USE_HANG;
                    c.idle_until_ms = now + Self::hang_of(&self.tgpol, self.cfg.hang_ms, c.kind, c.dst);
                }
            }
        }
        for n in self.net.iter_mut() {
            if n.used && n.stream == stream {
                n.used = false;
            }
        }
        for p in self.pend.iter_mut() {
            if p.used && p.net_stream == stream {
                p.net_stream = 0;
            }
        }
    }

    fn net_group(&mut self, stream: u32, src: u32, tg: u32, flags: u8) {
        if stream == 0
            || self.withdrawn
            || self.chans.iter().any(|c| c.stream == stream && Self::live(c))
            || self.net.iter().any(|n| n.used && n.stream == stream)
        {
            return;
        }
        if let Some(ch) = self.group_channel(tg) {
            let c = self.chans[ch];
            if c.talking && !c.talking_net {
                self.drop_stream(stream, WHY_BUSY); // RF has the floor
            } else {
                self.attach(ch, stream, src);
            }
        } else if self.wants_tg(tg) {
            self.net_group_go(stream, src, tg, flags & F_EMERGENCY != 0, None);
        } else {
            self.drop_stream(stream, WHY_NOT_FOUND);
        }
    }

    fn net_setup(&mut self, m: &Setup) {
        match m.op {
            SETUP_REQUEST => self.net_request(m),
            SETUP_ACCEPT | SETUP_ANSWERED | SETUP_ALERTING | SETUP_REJECT => {
                if self.pd_net_answer(m) {
                    return;
                }
                let Some(i) =
                    self.pend.iter().position(|p| p.used && p.caller_local && !p.called_local && p.stream == m.r#ref)
                else {
                    if m.op == SETUP_ACCEPT || m.op == SETUP_ANSWERED {
                        // a second acceptor, or one too late: release it
                        self.setup_out(SETUP_CANCEL, WHY_OK, 0, m.svc, m.src, m.dst, m.r#ref);
                    }
                    return;
                };
                let p = self.pend[i];
                match m.op {
                    SETUP_ALERTING => {
                        let tp = self.cfg.tp_ms as i64;
                        let q = &mut self.pend[i];
                        q.state = P_ALERTING;
                        q.answered = true;
                        q.deadline_ms = self.now + tp;
                        self.ack(R_MS_ALERTING, p.caller, p.called);
                    }
                    SETUP_REJECT => {
                        self.pend[i].used = false;
                        self.refuse(Self::reason_of_why(m.reason), p.caller, p.called);
                    }
                    _ => {
                        self.pend[i].state = P_AHOY; // answered: a later failure needs no cancel
                        self.private_ready(i);
                    }
                }
            }
            SETUP_CANCEL => {
                if self.pd_net_cancel(m) {
                    return;
                }
                if let Some(i) = self.pend.iter().position(|p| p.used && !p.caller_local && p.stream == m.r#ref) {
                    let p = self.pend[i];
                    self.pend[i].used = false;
                    if p.answered {
                        let a = Ahoy { service_kind: SK_CANCEL, target: p.called, source: p.caller, ..Default::default() };
                        self.push(DT_CSBK, a.encode(), false, None);
                    }
                    return;
                }
                // the call was granted already: clear it
                if let Some(ch) = self.private_channel(m.src, m.dst) {
                    if self.chans[ch].flags & F_PRIVATE_NET != 0 {
                        self.clear(ch, CLOSE_CMD);
                    }
                }
            }
            _ => {}
        }
    }

    /// The reason a local caller gets for a network reject (mqtt-net.md 9.4 reasons).
    pub(super) fn reason_of_why(why: u8) -> u8 {
        match why {
            WHY_NOT_FOUND => R_NOREG_MSAWAY,
            WHY_BUSY => R_CALLED_PARTY_BUSY,
            WHY_REFUSED => R_RECIPIENT_REFUSED,
            WHY_NOT_SUPPORTED => R_NOT_SUPPORTED,
            WHY_CONGESTION => R_SYSBUSY,
            _ => R_MSAWAY,
        }
    }

    /// A network caller asks for an MS: if it's registered here and free, check it with C_AHOY.
    fn net_request(&mut self, m: &Setup) {
        if m.flags & SETUP_F_DATA != 0 {
            self.pd_net_request(m); // a packet data call (tier3.md 5.10)
            return;
        }
        if self.pend.iter().any(|p| p.used && !p.caller_local && p.stream == m.stream) {
            return; // a repeat
        }
        if self.reg_index(m.dst).is_none() {
            self.setup_out(SETUP_REJECT, WHY_NOT_FOUND, 0, m.svc, m.src, m.dst, m.stream);
            return;
        }
        if self.ms_busy(m.dst) {
            self.setup_out(SETUP_REJECT, WHY_BUSY, 0, m.svc, m.src, m.dst, m.stream);
            return;
        }
        let als = m.flags & SETUP_F_ALS != 0;
        if als && self.cfg.als == 0 {
            self.setup_out(SETUP_REJECT, WHY_NOT_SUPPORTED, 0, m.svc, m.src, m.dst, m.stream);
            return;
        }
        let Some(i) = self.free_pend() else {
            self.setup_out(SETUP_REJECT, WHY_CONGESTION, 0, m.svc, m.src, m.dst, m.stream);
            return;
        };
        self.pend[i] = Pend {
            used: true,
            kind: KIND_PRIVATE,
            state: P_AHOY,
            als,
            svc: m.svc,
            prio: m.svc & SO_PRIORITY,
            ahoy_left: self.cfg.ahoy_tries,
            called_local: true,
            caller: m.src,
            called: m.dst,
            stream: m.stream,
            since_ms: self.now,
            deadline_ms: self.now + self.cfg.tp_ms as i64,
            next_ahoy_ms: self.now,
            ..Default::default()
        };
        self.ahoy_tick(i);
    }

    fn net_private(&mut self, stream: u32, src: u32, dst: u32) {
        if let Some(ch) = self.private_channel(src, dst) {
            if self.chans[ch].stream != stream {
                if self.chans[ch].talking && !self.chans[ch].talking_net {
                    self.drop_stream(stream, WHY_BUSY);
                    return;
                }
                self.attach(ch, stream, src);
            }
            return;
        }
        if let Some(i) = self.pend.iter().position(|p| p.used && p.caller == src && p.called == dst) {
            self.pend[i].net_stream = stream; // carried on the channel once granted
            return;
        }
        if self.reg_index(dst).is_none() {
            self.drop_stream(stream, WHY_NOT_FOUND);
            return;
        }
        if self.ms_busy(dst) {
            self.drop_stream(stream, WHY_BUSY);
            return;
        }
        // a caller that made no setup (Tier II): set one up here
        let Some(i) = self.free_pend() else {
            self.drop_stream(stream, WHY_CONGESTION);
            return;
        };
        self.pend[i] = Pend {
            used: true,
            kind: KIND_PRIVATE,
            state: P_AHOY,
            ahoy_left: self.cfg.ahoy_tries,
            called_local: true,
            caller: src,
            called: dst,
            net_stream: stream,
            since_ms: self.now,
            deadline_ms: self.now + self.cfg.tp_ms as i64,
            next_ahoy_ms: self.now,
            ..Default::default()
        };
        self.ahoy_tick(i);
    }

    fn link(&mut self, connected: bool) {
        if self.connected == connected {
            return;
        }
        self.connected = connected;
        self.log(format_args!("network {}", if connected { "up" } else { "down" }));
        if connected {
            return;
        }
        // what waits on the network fails; local calls go on
        self.pd_link_down();
        for i in 0..PENDING {
            let p = self.pend[i];
            if p.used && (!p.caller_local || !p.called_local) && p.state != P_QUEUED {
                self.pend[i].used = false;
                if p.caller_local {
                    self.refuse(R_MSAWAY, p.caller, p.called);
                }
            }
        }
        for i in 0..STREAMS {
            if self.net[i].used {
                self.net[i].used = false;
                let s = self.net[i].stream;
                self.drop_stream(s, WHY_CONGESTION);
            }
        }
        let now = self.now;
        for c in self.chans.iter_mut() {
            if Self::live(c) && c.talking_net {
                c.talking = false;
                c.talking_net = false;
                c.stream = 0;
                c.r#use = USE_HANG;
                c.idle_until_ms = now + Self::hang_of(&self.tgpol, self.cfg.hang_ms, c.kind, c.dst);
            }
        }
    }

    // ── subscriptions ──

    fn subs_into(&self, out: &mut [u32]) -> usize {
        let mut n = 0;
        let mut add = |tg: u32, out: &mut [u32]| {
            if tg != 0 && n < out.len() && !out[..n].contains(&tg) {
                out[n] = tg;
                n += 1;
            }
        };
        for r in self.regs.iter().filter(|r| r.used) {
            for &tg in r.rec.tgs() {
                add(tg, out);
            }
            for &tg in self.ms_policy(r.rec.id).tgs() {
                add(tg, out);
            }
            for tg in self.tgl_of(r.rec.id) {
                add(tg, out);
            }
        }
        n
    }

    fn check_subs(&mut self) {
        if !self.subs_dirty {
            return;
        }
        self.subs_dirty = false;
        let mut buf = [0u32; SUBS_MAX];
        let n = self.subs_into(&mut buf);
        buf[..n].sort_unstable();
        // a hash is enough to tell a change
        let mut sum: u64 = 0xcbf29ce484222325 ^ n as u64;
        for &t in &buf[..n] {
            sum = (sum ^ t as u64).wrapping_mul(0x100000001b3);
        }
        if sum != self.subs_sum {
            self.subs_sum = sum;
            self.ev(Event { r#type: EV_NET_SUBS, ..Default::default() });
        }
    }

    // ── entry points ──

    pub fn slot(&mut self, k: i64, now_ms: i64) -> SlotOut {
        self.k = k;
        self.now = now_ms;
        self.roll_load();
        self.pending_timers();
        self.auth_timers();
        self.sd_timers();
        self.pd_timers();
        self.supp_timers();
        self.channel_timers();
        self.reg_timers();
        let mut out = SlotOut::default();
        let rc_on_tscc = self.preempt_tick(&mut out) | self.intr_tick(&mut out);
        let rev = self.reverted();
        if !self.withdrawn && (rev || !self.hibernate_tick()) {
            if k & 1 == 0 && rev {
                self.rev_slot(&mut out);
            } else if k & 1 == 0 && !rc_on_tscc {
                self.tscc_slot(&mut out);
            } else {
                // the CACH before the TSCC carrier's next TS1 slot: TS2's inbound in use
                out.at = (self.chans[self.tscc_carrier * 2 + 1].r#use == USE_CALL) as u8;
            }
            if k.rem_euclid(4) == 0 {
                let counter = k.div_euclid(4).rem_euclid(512) as u16;
                out.has_slc = 1;
                // 7.1.2.1, 7.1.2.2: a composite TSCC carrying a call is a payload channel with Payload Channel Type 1
                out.slc_tscc = if rev {
                    p_sys_parms(self.syscode, true, counter)
                } else {
                    c_sys_parms(self.syscode, self.reg_bit(), counter)
                };
                out.slc_payload = p_sys_parms(self.syscode, false, counter);
            }
        }
        self.payload_slot(&mut out);
        self.check_subs();
        out
    }

    pub fn rx_pdu(&mut self, ch: usize, dt: u8, pdu: &[u8; 10]) {
        if self.withdrawn || ch >= CHANNELS {
            return;
        }
        if ch == self.tscc_carrier * 2 && !self.tscc_payload() {
            if dt == DT_DATA_HDR {
                self.sd_rx_head(pdu);
            }
            if dt != DT_CSBK {
                return; // MBC uploads: none is asked for
            }
            self.tscc_heard(); // a BS_Dwn_Act wakes a hibernating TSCC, as any good block does
            if let Some(r) = Rand::decode(pdu) {
                self.counters.rand += 1;
                self.count_load();
                self.heard(r.source);
                let before = (self.resp_head + self.resp_len) % RESP;
                let queued = self.resp_len;
                if r.service_kind == SK_REG {
                    self.registration(&r);
                } else {
                    self.service(&r);
                }
                self.rand_said(&r, before, queued);
            } else if let Some(a) = Ack::decode(pdu) {
                if a.op == OP_ACKU && !self.auth_answer(&a) {
                    self.ms_ack(&a);
                }
            } else if let Some(v) = Ackvit::decode(pdu) {
                self.heard(v.source);
                self.supp_ackvit(&v); // 6.4.9.2, 6.4.10: an MS challenges the TSCC
            }
        } else if let Some(a) = Ack::decode(pdu).filter(|a| a.op == OP_P_ACKU) {
            self.pa_answer(ch, &a); // P_ACKU to a radio check or a selective clear (t3pcheck.rs)
        } else if let Some(m) = Maint::decode(pdu) {
            let c = self.chans[ch];
            if m.kind == MAINT_DISCON
                && Self::live(&c)
                && (m.source == c.src || (c.kind == KIND_PRIVATE && m.source == c.dst))
            {
                self.heard(m.source);
                self.clear(ch, CLOSE_DISCON);
            }
        }
        self.check_subs();
    }

    /// A rate 1/2 data block (12 octets) on the TSCC: a UDT's appended block.
    pub fn rx_block(&mut self, ch: usize, dt: u8, b: &[u8; 12]) {
        if !self.withdrawn && ch == self.tscc_carrier * 2 && !self.tscc_payload() && dt == DT_RATE12 {
            self.sd_rx_block(b);
        }
    }

    pub fn rx_bad(&mut self, ch: usize) {
        if ch == self.tscc_carrier * 2 && !self.withdrawn && !self.tscc_payload() {
            self.counters.collisions += 1;
            self.count_load();
        } else if ch < CHANNELS && !self.withdrawn {
            self.pa_burst(ch); // a talkgroup radio check's answers colliding (t3pcheck.rs)
        }
    }

    pub fn rx_voice(&mut self, ch: usize, what: u8, src: u32) {
        if ch < CHANNELS && self.chans[ch].held {
            self.held_voice(ch, what);
            return;
        }
        if ch >= CHANNELS || !Self::live(&self.chans[ch]) {
            return;
        }
        let now = self.now;
        match what {
            VOICE_HEADER | VOICE_FRAME => {
                let c = self.chans[ch];
                if c.talking_net && c.stream != 0 {
                    // RF beats the network
                    self.drop_stream(c.stream, WHY_BUSY);
                    self.chans[ch].stream = 0;
                }
                let c = &mut self.chans[ch];
                c.talking = true;
                c.talking_net = false;
                c.r#use = USE_CALL;
                c.last_voice_ms = now;
                if src != 0 {
                    c.talker = src;
                    self.heard(src);
                }
            }
            VOICE_TERM | VOICE_LOST => {
                let hang = Self::hang_of(&self.tgpol, self.cfg.hang_ms, self.chans[ch].kind, self.chans[ch].dst);
                let c = &mut self.chans[ch];
                if !c.talking_net {
                    c.talking = false;
                    c.r#use = USE_HANG;
                    c.idle_until_ms = now + hang;
                }
            }
            _ => {}
        }
    }

    pub fn status(&self) -> Status {
        let mut st = Status {
            connected: self.connected as u8,
            withdrawn: self.withdrawn as u8,
            nch: self.cfg.ncarriers * 2,
            regs: self.nregs,
            queue: self.queue_len() as u32,
            aloha: self.counters.aloha,
            rand: self.counters.rand,
            collisions: self.counters.collisions,
            grants: self.counters.grants,
            refused: self.counters.refused,
            queued: self.counters.queued,
            ..Default::default()
        };
        for ch in 0..CHANNELS {
            let c = &self.chans[ch];
            st.ch[ch] = ChStatus {
                r#use: c.r#use,
                lcn: (ch % 2) as u8,
                flags: c.flags,
                chan: self.cfg.carriers[ch / 2].chan,
                src: c.src,
                dst: c.dst,
                since_ms: c.since_ms,
                ..Default::default()
            };
        }
        st
    }

    pub fn next_event(&mut self) -> Option<Event> {
        if self.ev_len == 0 {
            return None;
        }
        let e = self.events[self.ev_head];
        self.ev_head = (self.ev_head + 1) % EVENTS;
        self.ev_len -= 1;
        Some(e)
    }

    pub fn withdraw(&mut self, off: bool) {
        if off && !self.withdrawn {
            for ch in 0..CHANNELS {
                self.clear(ch, CLOSE_WITHDRAW);
            }
            for i in 0..PENDING {
                let p = self.pend[i];
                if p.used {
                    self.pend[i].used = false;
                    self.fail(&p, R_SYSBUSY, WHY_CONGESTION);
                }
            }
            self.resp_len = 0;
            self.cont_pending = false;
        }
        self.withdrawn = off;
    }

    pub fn record(&self, ms: u32) -> Option<Record> {
        self.reg_index(ms).map(|i| self.regs[i].rec)
    }

    pub fn load_record(&mut self, r: &Record) -> bool {
        if r.id == 0 || r.exp_ms <= self.now {
            return false;
        }
        let Some(i) = self.record_for(r.id, HOW_LOADED) else { return false };
        let mut rec = *r;
        rec.ntg = rec.ntg.min(REG_TGS as u8);
        self.regs[i].rec = rec;
        self.regs[i].written_ms = self.now;
        self.subs_dirty = true;
        self.check_subs();
        true
    }

    pub fn command(&mut self, cmd: &str, a1: u32, a2: u32) -> bool {
        match cmd {
            "deregister" => match self.reg_index(a1) {
                Some(i) => {
                    self.delete_record(i, REG_DEREGISTER);
                    self.check_subs();
                    true
                }
                None => false,
            },
            "clear" => {
                let ch = (0..self.cfg.ncarriers as usize)
                    .find(|&c| self.cfg.carriers[c].chan as u32 == a1)
                    .map(|c| c * 2 + (a2 as usize & 1));
                match ch {
                    Some(ch) if Self::live(&self.chans[ch]) => {
                        self.clear(ch, CLOSE_CMD);
                        true
                    }
                    _ => false,
                }
            }
            "withdraw" => {
                self.withdraw(true);
                true
            }
            "restore" => {
                self.withdraw(false);
                true
            }
            "resend_records" => {
                for i in 0..REGS {
                    if self.regs[i].used {
                        if self.ev_len == EVENTS {
                            return false; // the caller drains the events and asks again
                        }
                        self.write_record(i);
                    }
                }
                true
            }
            "status" => true,
            "mass_reg" => self.mass_registration(a1, a2),
            "move" => self.move_cmd(a1, a2),
            "move_site" => self.move_to_site(a1, a2), // C_MOVE to an adjacent site's TSCC, absolute when needed
            "auth" => self.auth_poll(a1),
            // a2 1 for a talkgroup; on its payload channel when it's on one (6.6.2.3.1.1), else the TSCC (6.4.12)
            "radio_check" if self.pa_applies(a1, a2 != 0) => self.pa_start(a1, a2 != 0, false),
            "radio_check" => self.radio_check(a1, a2 != 0),
            "clear_ms" => self.pa_start(a1, a2 != 0, true), // 6.6.2.3.1.8: a2 1 for a talkgroup
            "stun" | "revive" => self.stun(a1, cmd == "revive"), // 6.4.9
            "kill" => self.kill(a1),                               // 6.4.10: only an MS with a key
            _ => false,
        }
    }

    // for the tests and the air (Rust callers)

    pub fn policy_set(&mut self, p: &Policy) {
        self.policy = *p;
        self.subs_dirty = true;
        self.check_subs();
    }
    pub fn net_group_in(&mut self, stream: u32, src: u32, tg: u32, flags: u8, now_ms: i64) {
        self.now = self.now.max(now_ms);
        self.net_group(stream, src, tg, flags);
        self.check_subs();
    }
    pub fn net_end_in(&mut self, stream: u32) {
        self.net_end(stream);
    }
    pub fn net_setup_in(&mut self, m: &Setup, now_ms: i64) {
        self.now = self.now.max(now_ms);
        if !self.withdrawn {
            self.net_setup(m);
        }
    }
    pub fn net_private_in(&mut self, stream: u32, src: u32, dst: u32, now_ms: i64) {
        self.now = self.now.max(now_ms);
        if !self.withdrawn && stream != 0 {
            self.net_private(stream, src, dst);
        }
    }
    pub fn set_link(&mut self, c: bool) {
        self.link(c);
    }
    pub fn set_neighbors(&mut self, n: &[Neighbor]) {
        let (old, nold) = (self.neighbors, self.nneighbors);
        let count = n.len().min(NEIGHBORS);
        self.neighbors[..count].copy_from_slice(&n[..count]);
        self.nneighbors = count;
        self.adjacent_idx = 0;
        self.log_clashes(&old[..nold]);
    }
    /// A talkgroup's provisioning; None removes it. False: the table is full.
    pub fn set_tg_policy(&mut self, tg: u32, p: Option<&TgPolicy>) -> bool {
        if tg == 0 {
            return false;
        }
        let i = self.tgpol.iter().position(|t| t.tg == tg);
        match p {
            None => {
                if let Some(i) = i {
                    self.tgpol[i] = TgPolicy::default();
                }
                true
            }
            Some(p) => match i.or_else(|| self.tgpol.iter().position(|t| t.tg == 0)) {
                Some(i) => {
                    self.tgpol[i] = TgPolicy { tg, ..*p };
                    true
                }
                None => false,
            },
        }
    }

    pub fn set_ms_policy(&mut self, ms: u32, p: Option<&MsPolicy>) -> bool {
        let i = self.mspol.iter().position(|m| m.used && m.id == ms);
        self.subs_dirty = true;
        let ok = match p {
            None => {
                if let Some(i) = i {
                    self.mspol[i] = MsPol::default();
                }
                true
            }
            Some(p) => match i.or_else(|| self.mspol.iter().position(|m| !m.used)) {
                Some(i) => {
                    self.mspol[i] = MsPol { used: true, id: ms, p: *p };
                    if p.enabled == 0 {
                        if let Some(r) = self.reg_index(ms) {
                            self.delete_record(r, REG_DENY);
                        }
                    }
                    true
                }
                None => false,
            },
        };
        self.check_subs();
        ok
    }
    pub fn where_in(&mut self, ms: u32, here: bool, at_ms: i64) {
        if here {
            return;
        }
        if let Some(i) = self.reg_index(ms) {
            if self.regs[i].rec.at_ms < at_ms {
                self.delete_record(i, REG_EXPIRE);
                self.check_subs();
            }
        }
    }
    pub fn reconfigure(&mut self, cfg: &Cfg, err: &mut [u8]) -> bool {
        let mut b = Buf { b: err, n: 0 };
        if !check(cfg, &mut b) {
            return false;
        }
        let tscc = (0..cfg.ncarriers as usize).find(|&i| cfg.carriers[i].tscc != 0);
        if tscc != Some(self.tscc_carrier) {
            for ch in 0..CHANNELS {
                self.clear(ch, CLOSE_CMD);
            }
        }
        self.apply(cfg);
        self.reset_channels();
        true
    }
    pub fn subs(&self, out: &mut [u32]) -> usize {
        self.subs_into(out)
    }
    pub fn tscc_ch(&self) -> usize {
        self.tscc_carrier * 2
    }
    pub fn cfg(&self) -> &Cfg {
        &self.cfg
    }
    pub fn syscode(&self) -> u16 {
        self.syscode
    }
    pub fn events_lost(&self) -> u32 {
        self.counters.events_lost
    }
}

/// Zeroes the memory at s and fills it in place (a Site is valid all zero, and too big for a stack).
///
/// # Safety
/// s points to rdof_t3_site_size() writable bytes, aligned for Site.
pub unsafe fn init_in_place(s: *mut Site, cfg: &Cfg, err: &mut [u8]) -> bool {
    let mut b = Buf { b: err, n: 0 };
    if !check(cfg, &mut b) {
        return false;
    }
    core::ptr::write_bytes(s, 0, 1);
    (*s).setup(cfg);
    true
}

// ── layouts (rdof_t3_layout) ───────────────────────────────────────────────────

#[repr(C)]
pub struct Layout {
    pub name: *const c_char,
    pub size: usize,
    pub align: usize,
}

// SAFETY: the names are static strings, never written
unsafe impl Sync for Layout {}

macro_rules! layouts {
    ($(($name:literal, $t:ty)),* $(,)?) => {
        [$(Layout {
            name: concat!($name, "\0").as_ptr().cast(),
            size: core::mem::size_of::<$t>(),
            align: core::mem::align_of::<$t>(),
        },)*
         Layout { name: core::ptr::null(), size: 0, align: 0 }]
    };
}

static LAYOUTS: [Layout; 19] = layouts![
    ("rdof_t3_carrier_t", Carrier),
    ("rdof_t3_cfg_t", Cfg),
    ("rdof_t3_range_t", Range),
    ("rdof_t3_idset_t", IdSet),
    ("rdof_t3_policy_t", Policy),
    ("rdof_t3_ms_policy_t", MsPolicy),
    ("rdof_t3_record_t", Record),
    ("rdof_t3_setup_t", Setup),
    ("rdof_t3_neighbor_t", Neighbor),
    ("rdof_t3_event_t", Event),
    ("rdof_t3_ch_out_t", ChOut),
    ("rdof_t3_slot_out_t", SlotOut),
    ("rdof_t3_ch_status_t", ChStatus),
    ("rdof_t3_status_t", Status),
    ("rdof_t3ms_tx_t", crate::t3ms::MsTx),
    ("rdof_t3ms_status_t", crate::t3ms::MsStatus),
    ("rdof_t3_tg_policy_t", TgPolicy),
    ("rdof_t3_sd_t", SdMsg),
];

#[no_mangle]
pub extern "C" fn rdof_t3_layout(i: usize) -> *const Layout {
    LAYOUTS.get(i).map_or(core::ptr::null(), |l| l as *const Layout)
}

// ── C API (rdof/t3.h) ──────────────────────────────────────────────────────────

unsafe fn err_buf<'a>(err: *mut c_char, cap: usize) -> &'a mut [u8] {
    if err.is_null() || cap == 0 {
        &mut []
    } else {
        *err = 0;
        core::slice::from_raw_parts_mut(err.cast::<u8>(), cap)
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3_cfg_default(cfg: *mut Cfg) {
    *cfg = cfg_default();
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3_policy_default(p: *mut Policy) {
    *p = Policy::default_open();
}

#[no_mangle]
pub extern "C" fn rdof_t3_site_size() -> usize {
    core::mem::size_of::<Site>()
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3_init(s: *mut Site, cfg: *const Cfg, err: *mut c_char, cap: usize) -> bool {
    init_in_place(s, &*cfg, err_buf(err, cap))
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3_reconfigure(s: *mut Site, cfg: *const Cfg, err: *mut c_char, cap: usize) -> bool {
    (*s).reconfigure(&*cfg, err_buf(err, cap))
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3_slot(s: *mut Site, k: i64, now_ms: i64, out: *mut SlotOut) {
    *out = (*s).slot(k, now_ms);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3_rx_pdu(s: *mut Site, ch: c_int, k: i64, dt: u8, pdu: *const [u8; 10], _rssi: c_int) {
    (*s).rx_k = k;
    (*s).rx_k_ok = true;
    if ch >= 0 && dt == DT_RATE12 {
        (*s).rx_block(ch as usize, dt, &*pdu.cast::<[u8; 12]>()); // a rate 1/2 block: 12 octets
    } else if ch >= 0 {
        (*s).rx_pdu(ch as usize, dt, &*pdu);
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3_rx_bad(s: *mut Site, ch: c_int, _k: i64) {
    if ch >= 0 {
        (*s).rx_bad(ch as usize);
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3_rx_voice(s: *mut Site, ch: c_int, _k: i64, what: u8, src: u32, _dst: u32, _group: bool) {
    if ch >= 0 {
        (*s).rx_voice(ch as usize, what, src);
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3_net_group(s: *mut Site, stream: u32, src: u32, tg: u32, flags: u8, now_ms: i64) {
    (*s).net_group_in(stream, src, tg, flags, now_ms);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3_net_group_end(s: *mut Site, stream: u32) {
    (*s).net_end(stream);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3_net_setup(s: *mut Site, m: *const Setup, now_ms: i64) {
    (*s).net_setup_in(&*m, now_ms);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3_net_private(s: *mut Site, stream: u32, src: u32, dst: u32, now_ms: i64) {
    (*s).net_private_in(stream, src, dst, now_ms);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3_net_private_end(s: *mut Site, stream: u32) {
    (*s).net_end(stream);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3_net_link(s: *mut Site, connected: bool) {
    (*s).link(connected);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3_neighbors(s: *mut Site, n: *const Neighbor, count: c_int) {
    let count = (count.max(0) as usize).min(NEIGHBORS);
    let n = if count == 0 { &[][..] } else { core::slice::from_raw_parts(n, count) };
    (*s).set_neighbors(n);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3_where(s: *mut Site, ms: u32, here: bool, at_ms: i64) {
    (*s).where_in(ms, here, at_ms);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3_policy(s: *mut Site, p: *const Policy) {
    (*s).policy_set(&*p);
}

/// C_SYScode from its parts (model 0-3, par 1-3), 0 when they don't fit the model.
#[no_mangle]
pub extern "C" fn rdof_t3_syscode(model: u8, net: u16, site: u16, par: u8) -> u16 {
    let c = crate::t3pdu::SysCode { model, net, site, par };
    if c.valid() {
        c.encode()
    } else {
        0
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3_tg_policy(s: *mut Site, tg: u32, p: *const TgPolicy) -> bool {
    (*s).set_tg_policy(tg, p.as_ref())
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3_ms_policy(s: *mut Site, ms: u32, p: *const MsPolicy) -> bool {
    (*s).set_ms_policy(ms, p.as_ref())
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3_load_record(s: *mut Site, r: *const Record) -> bool {
    (*s).load_record(&*r)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3_record(s: *const Site, ms: u32, r: *mut Record) -> bool {
    match (*s).record(ms) {
        Some(rec) => {
            *r = rec;
            true
        }
        None => false,
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3_subs(s: *const Site, tg: *mut u32, cap: usize) -> usize {
    if tg.is_null() || cap == 0 {
        let mut buf = [0u32; SUBS_MAX];
        return (*s).subs_into(&mut buf);
    }
    (*s).subs_into(core::slice::from_raw_parts_mut(tg, cap))
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3_withdraw(s: *mut Site, off: bool) {
    (*s).withdraw(off);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3_command(s: *mut Site, cmd: *const c_char, a1: u32, a2: u32) -> bool {
    match CStr::from_ptr(cmd).to_str() {
        Ok(c) => (*s).command(c, a1, a2),
        Err(_) => false,
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3_event(s: *mut Site, ev: *mut Event) -> bool {
    match (*s).next_event() {
        Some(e) => {
            *ev = e;
            true
        }
        None => false,
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3_status(s: *const Site, st: *mut Status) {
    *st = (*s).status();
}

/// A Site on the heap, for the tests.
#[cfg(test)]
pub fn new_site(cfg: &Cfg) -> std::boxed::Box<Site> {
    use std::alloc::{alloc_zeroed, Layout as L};
    unsafe {
        let p = alloc_zeroed(L::new::<Site>()) as *mut Site;
        let mut err = [0u8; 128];
        assert!(init_in_place(p, cfg, &mut err), "{}", std::string::String::from_utf8_lossy(&err));
        std::boxed::Box::from_raw(p)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn err_of(c: &Cfg) -> std::string::String {
        let mut tmp = [0u8; 128];
        let mut b = Buf { b: &mut tmp, n: 0 };
        assert!(!check(c, &mut b));
        let n = b.n;
        std::string::String::from_utf8_lossy(&tmp[..n]).into_owned()
    }

    #[test]
    fn settings_checks() {
        let mut c = cfg_default();
        let mut tmp = [0u8; 128];
        assert!(check(&c, &mut Buf { b: &mut tmp, n: 0 }));
        c.site = 0; // a site (table 6.5)
        assert!(check(&c, &mut Buf { b: &mut tmp, n: 0 }));
        c.site = 1 << model_bits(c.model).1;
        assert!(err_of(&c).starts_with("system.site"));
        c = cfg_default();
        c.ncarriers = 2;
        c.carriers[1] = Carrier { chan: 1, enabled: 1, ..Default::default() };
        assert!(err_of(&c).contains("used twice"));
        c.carriers[1].chan = 2;
        c.carriers[1].tscc = 1;
        assert!(err_of(&c).contains("exactly one"));
        c = cfg_default();
        c.hang_ms = 0;
        assert!(err_of(&c).starts_with("timers.hang_s"));
    }

    #[test]
    fn record_tgs() {
        let mut r = Record::default();
        assert!(r.touch_tg(1));
        assert!(r.touch_tg(2));
        assert!(!r.touch_tg(2));
        assert!(r.touch_tg(1));
        assert_eq!(r.tgs(), &[1, 2]);
        for t in 3..20 {
            r.touch_tg(t);
        }
        assert_eq!(r.tgs(), &[19, 18, 17, 16, 15, 14, 13, 12]);
        r.remove_tg(15);
        assert_eq!(r.tgs(), &[19, 18, 17, 16, 14, 13, 12]);
    }

    #[test]
    fn idle_tscc() {
        let mut s = new_site(&cfg_default());
        let mut alohas = 0;
        for k in 0..200i64 {
            let o = s.slot(k, k * 30);
            if k % 2 == 1 {
                assert_eq!(o.tscc_dt, 0);
                continue;
            }
            assert_ne!(o.tscc_dt, 0);
            if o.tscc_dt == DT_CSBK && Aloha::decode(&o.tscc_pdu).is_some() {
                alohas += 1;
            }
            if k % 4 == 0 {
                let (slco, _, reg, csc) = sys_parms_decode(o.slc_tscc);
                assert_eq!((slco, reg, csc), (SLCO_C_SYS_PARMS, true, (k / 4) as u16));
            }
        }
        assert!(alohas >= 80, "{alohas}");
        assert!(core::mem::size_of::<Site>() < 160 * 1024, "{}", core::mem::size_of::<Site>());
    }
}
