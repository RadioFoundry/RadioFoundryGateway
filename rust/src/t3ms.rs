// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! An MS simulator for the trunked site (docs/spec/fw/tier3.md section 11): a radio that knows one site and follows
//! ETSI TS 102 361-4 for the services the site supports. It hunts the TSCC, waits for C_ALOHA, makes slotted random
//! accesses with the site's backoff and NRand_Wait, answers C_AHOY, follows grants to the payload channel, talks,
//! and returns on P_CLEAR. Sans-IO like the site: the caller (t3air, or C) moves the blocks and the time.

use core::ffi::c_int;

use crate::t3auth::{response, R_AUTH_RESPONSE, R_MS_NOT_SUPPORTED};
use crate::t3pdu::*;
use crate::t3rc::RC_CEASE;
use crate::t3site::{
    ahoy_status, bs_dwn_act, mass_reg_applies, DxGrant, R_DUPLEX_NOT_SUPPORTED, SK_DUPLEX, mass_reg_of, move_of, ps_wake, reg_window_ms, status_rand, CARRIERS,
    VOICE_FRAME, VOICE_HEADER, VOICE_TERM,
};

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

pub const IDLE: u8 = 0;
pub const ACCESS: u8 = 1;
pub const WAITING: u8 = 2;
pub const PAYLOAD: u8 = 3;

pub const TX_PDU: u8 = 1;
pub const TX_VOICE: u8 = 2;

const ACTIONS: usize = 8;
const TRIES: u8 = 6; // random access attempts before giving up: NRand_NR (table A.6)
const TRIES_EMERGENCY: u8 = 10; // NRand_NE
const WAIT_MS: i64 = 30_000; // after C_WACKD / C_QACKD / MS_Alerting, the longest wait for the outcome
const HUNT_SLOTS: u32 = 100; // TSCC slots without C_ALOHA before trying the next carrier
const MAINT_REPEATS: u8 = 4;
const TNP_MS: i64 = 10_000; // TNP_Timer: after answering a registration's challenge, the wait for the outcome

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
    next_activation_ms: i64,
    activations: u32,
    moves: u32,
    // full-duplex calls (6.6.9)
    no_duplex: bool, // answers a duplex radio check C_NACKU(MS_Duplex_Not_Supported)
    dx: bool,        // in a duplex call: offset timing, talks while it listens
    dx_peer: u32,
    voice_heard: u32,
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
        self.tg[..self.ntg].contains(&tg)
    }

    fn chan_index(&self, chan: u16, lcn: u8) -> Option<usize> {
        (0..self.ncarriers).find(|&i| self.chans[i] == chan).map(|i| i * 2 + (lcn as usize & 1))
    }

    fn request_pdu(&self, a: &Action) -> [u8; 10] {
        let emergency = if a.flags & F_EMERGENCY != 0 { SO_EMERGENCY } else { 0 };
        let r = match a.what {
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
        self.pdu = self.request_pdu(&a);
        self.tries = 0;
        self.wait = 1;
        self.resp_wait = 0;
        self.state = ACCESS;
    }

    fn done(&mut self, ok: bool) {
        if self.has_cur && matches!(self.cur.what, DO_GROUP | DO_PRIVATE | DO_STATUS | DO_DUPLEX) {
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
        self.dx = false;
        self.state = IDLE;
        self.ch = self.tscc_ch;
        self.talking = false;
        self.maint_left = 0;
    }

    /// Outbound slot k on channel ch (a block when dt isn't 0); at: the CACH's AT bit for the next slot.
    pub fn rx(&mut self, ch: usize, k: i64, dt: u8, pdu: &[u8; 10], at: bool) {
        if ch != self.ch {
            return;
        }
        if self.state == PAYLOAD {
            if dt == DT_CSBK && Clear::decode(pdu).is_some() {
                self.clears += 1;
                let mine = self.mine && self.has_cur;
                self.mine = false;
                if mine {
                    self.done(true);
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
        if self.no_aloha > HUNT_SLOTS && self.state == IDLE && !self.site_hibernates {
            // no TSCC here: the next carrier
            let c = (self.tscc_ch / 2 + 1) % self.ncarriers;
            self.tscc_ch = c * 2;
            self.ch = self.tscc_ch;
            self.no_aloha = 0;
            self.have_aloha = false;
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

    fn on_ack(&mut self, a: &Ack) {
        self.awake_until = self.now + T_AWAKE_MS;
        self.last_reason = a.reason;
        if !self.has_cur {
            return; // e.g. the called party told of its own refusal
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
                self.done(false);
            }
        }
    }

    fn on_ahoy(&mut self, a: &Ahoy) {
        self.awake_until = self.now + T_AWAKE_MS;
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
        let ack = |reason, s: &Ms| Ack { op: OP_ACKU, response_info: 0, reason, target: a.source, source: s.id }.encode();
        if a.service_kind == SK_STATUS {
            // 6.6.6.1.2.1.5 / 6.6.6.1.2.2.5: store it; an individual one is answered, a talkgroup's isn't
            let st = (a.source, a.target, ahoy_status(a));
            if st != self.last_status || self.now - self.last_status_ms > 2000 {
                self.statuses += 1; // not the same one repeated
            }
            self.last_status = st;
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
        self.reply = match self.answer {
            ANSWER_ACCEPT => Some(ack(R_MS_ACCEPTED, self)),
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

    /// A Reverse Channel PDU heard on the alternate slot of its payload channel ch (361-1 5.1.5). Cease
    /// transmission (361-4 6.4.15): it finishes its superframe, sends a terminator and stops talking.
    pub fn rc(&mut self, ch: usize, info: u8) {
        if self.state == PAYLOAD && ch == self.ch && self.talking && !self.ceasing && info == RC_CEASE {
            self.ceasing = true;
            self.cease_left = 5;
        }
    }

    /// Times it stopped talking for an RC cease.
    pub fn ceased(&self) -> u32 {
        self.ceased
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
        let group = self.pay_group as u8;
        let dst = if self.dx {
            self.dx_peer
        } else if self.mine {
            self.cur.arg
        } else {
            self.pay_dst
        };
        let offset = u8::from(self.dx);
        let voice =
            |what, s: &Ms| MsTx { ch: s.ch as u8, kind: TX_VOICE, what, group, offset, src: s.id, dst, ..Default::default() };
        if self.talking {
            if !self.header_sent {
                self.header_sent = true;
                let talk = if self.dx && !self.mine { DX_CALLED_TALK_MS } else { self.cur.talk_ms as i64 };
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
                self.ceased += 1;
                return Some(voice(VOICE_TERM, self));
            }
            if self.now >= self.talk_end {
                self.talking = false;
                if !self.pay_group && (!self.dx || self.mine) {
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
pub unsafe extern "C" fn rdof_t3ms_key(ms: *mut Ms, k: *const [u8; 16], psn: u32, has_psn: bool) {
    let k = if k.is_null() { None } else { Some(&*k) };
    (*ms).set_key(k, has_psn.then_some(psn & 0xFF_FFFF));
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3ms_do(ms: *mut Ms, what: u8, arg: u32, talk_ms: u32, flags: u8, at_ms: i64) -> bool {
    matches!(what, DO_REGISTER..=DO_CANCEL | DO_STATUS | DO_DUPLEX) && (*ms).act(what, arg, talk_ms, flags, at_ms)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3ms_rx(ms: *mut Ms, ch: c_int, k: i64, dt: u8, pdu: *const [u8; 10], at: bool) {
    if ch >= 0 {
        let zero = [0u8; 10];
        let pdu = if pdu.is_null() { &zero } else { &*pdu };
        (*ms).rx(ch as usize, k, dt, pdu, at);
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
