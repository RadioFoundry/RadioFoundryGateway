// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! An MS simulator for the trunked site (docs/spec/fw/tier3.md section 11): a radio that knows one site and follows
//! ETSI TS 102 361-4 for the services the site supports. It hunts the TSCC, waits for C_ALOHA, makes slotted random
//! accesses with the site's backoff and NRand_Wait, answers C_AHOY, follows grants to the payload channel, talks,
//! and returns on P_CLEAR. Sans-IO like the site: the caller (t3air, or C) moves the blocks and the time.

use core::ffi::c_int;

use crate::t3pdu::*;
use crate::t3rc::RC_CEASE;
use crate::t3site::{CARRIERS, VOICE_FRAME, VOICE_HEADER, VOICE_TERM};

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
pub const F_EMERGENCY: u8 = 0x01;

pub const IDLE: u8 = 0;
pub const ACCESS: u8 = 1;
pub const WAITING: u8 = 2;
pub const PAYLOAD: u8 = 3;

pub const TX_PDU: u8 = 1;
pub const TX_VOICE: u8 = 2;

const ACTIONS: usize = 8;
const TRIES: u8 = 6; // random access attempts before giving up
const WAIT_MS: i64 = 30_000; // after C_WACKD / C_QACKD / MS_Alerting, the longest wait for the outcome
const HUNT_SLOTS: u32 = 100; // TSCC slots without C_ALOHA before trying the next carrier
const MAINT_REPEATS: u8 = 4;

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
        if self.has_cur && matches!(self.cur.what, DO_GROUP | DO_PRIVATE) {
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
        if dt != DT_CSBK {
            return;
        }
        if let Some(a) = Ack::decode(pdu) {
            if a.op == OP_ACKD && a.target == self.id {
                self.on_ack(&a);
            }
        } else if let Some(a) = Ahoy::decode(pdu) {
            if a.target == self.id {
                self.on_ahoy(&a);
            }
        } else if let Some(g) = Grant::decode(pdu) {
            self.on_grant(&g);
        }
    }

    fn on_ack(&mut self, a: &Ack) {
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
        self.ahoys += 1;
        let ack = |reason, s: &Ms| Ack { op: OP_ACKU, response_info: 0, reason, target: a.source, source: s.id }.encode();
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
        self.ceasing = false;
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
        let group = self.pay_group as u8;
        let dst = if self.mine { self.cur.arg } else { self.pay_dst };
        let voice = |what, s: &Ms| MsTx { ch: s.ch as u8, kind: TX_VOICE, what, group, src: s.id, dst, ..Default::default() };
        if self.talking {
            if !self.header_sent {
                self.header_sent = true;
                self.talk_end = self.now + self.cur.talk_ms as i64;
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
    matches!(what, DO_REGISTER..=DO_CANCEL) && (*ms).act(what, arg, talk_ms, flags, at_ms)
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
