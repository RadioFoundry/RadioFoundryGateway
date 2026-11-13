// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! The TSCC's other C_BCAST announcements and power save (docs/spec/fw/tier3.md section 3.11), ETSI TS 102 361-4:
//! - CallTimer_Parms (6.7.1.2, table 7.70): the site's call timers as the tokens of annex A tables A.2 and A.4;
//! - Vote_Now (6.7.1.3, table 7.71): an adjacent site's TSCC to assess, then VOTE_BLK slots of blocked random
//!   access (C_ALOHA with ADRNULL and Mask 24);
//! - Ann_WD_TSCC (6.7.1.1, table 7.69): the adjacent sites' TSCCs for the MSs' hunt lists, two to a CSBK;
//! - MassReg (6.4.5, tables 7.76 and 6.12): a mass re-registration, on command or at start, repeated in every
//!   power save frame while power save is offered (6.4.5.1 f);
//! - power save (6.4.7): PowerSave_RQ in a registration, the PowerSave_Offset in its C_ACKD's Response_Info, and
//!   the C_AHOYs to a sleeping MS held for its wake frame (PS_Counter, the Common_Slot_Counter's 7 high bits).
//!
//! A child module of t3site: it works on the site's own state.

use super::*;

/// A.2: TSCC slots of blocked random access after a Vote Now Advice (2 to 10 TDMA frames).
const VOTE_BLK: i64 = 4;
/// Mass registration broadcasts without power save, a second apart (the MS ignores repeats in its window).
const MASS_SENDS: u16 = 3;
/// A registration demanded of one MS: within 1 s (table 7.77 Reg_Window 2)
const DEMAND_WINDOW_S: u32 = 1;
/// Slots of a power save frame: 4 Common_Slot_Counter steps of 120 ms (figure 6.29).
const PS_FRAME_K: i64 = 16;
/// The first slots of a wake frame in which a held C_AHOY is let go: it airs within the frame.
const PS_SEND_K: i64 = 8;

/// Table 6.12: Treg_Window in ms for Reg_Window 1-15 (0 cancels).
const REG_WINDOW_MS: [u32; 16] =
    [0, 500, 1000, 2000, 5000, 10_000, 20_000, 30_000, 100_000, 300_000, 1_000_000, 3_000_000, 10_000_000, 30_000_000, 100_000_000, 200_000_000];

/// The Reg_Window of a window in seconds: the smallest that covers it (0: cancel; 15 at most).
pub fn reg_window_of(secs: f64) -> u8 {
    if secs <= 0.0 {
        return 0;
    }
    (1..16).find(|&w| f64::from(REG_WINDOW_MS[w]) >= secs * 1000.0).unwrap_or(15) as u8
}

/// Treg_Window in ms (table 6.12).
pub fn reg_window_ms(w: u8) -> u32 {
    REG_WINDOW_MS[usize::from(w & 15)]
}

/// Table A.2: T_EMERG_TIMER's seconds for a token 1-510.
fn emerg_secs(t: u32) -> u32 {
    match t {
        0..=10 => t,
        11..=20 => 15 + 5 * (t - 11),
        21..=28 => 75 + 15 * (t - 21),
        29..=40 => 210 + 30 * (t - 29),
        41..=51 => 600 + 60 * (t - 41),
        _ => 1500 + 300 * (t - 52),
    }
}

/// Table A.4 (and A.5): T_MS-MS_TIMER's seconds for a token 1-4094.
fn msms_secs(t: u32) -> u32 {
    match t {
        0..=59 => t,
        60..=107 => 60 + 5 * (t - 60),
        108..=138 => 300 + 30 * (t - 108),
        _ => 1260 + 60 * (t - 139),
    }
}

/// The largest token whose time doesn't pass ms (the MS's timer ends first, before the site's), 1 at least;
/// 0 ms is infinity.
fn token(ms: u32, max: u32, secs: fn(u32) -> u32) -> u32 {
    if ms == 0 {
        return max + 1;
    }
    let s = ms / 1000;
    let (mut lo, mut hi) = (1, max);
    while lo < hi {
        let mid = (lo + hi).div_ceil(2);
        if secs(mid) <= s {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    lo
}

/// CallTimer_Parms (table 7.70): T_EMERG_TIMER (9), T_PACKET_TIMER (5) in Parms 1, T_MS-MS_TIMER (12) and
/// T_MS-LINE_TIMER (12) in Parms 2. Packet and line timers 0: the MS's own (the site sets none).
pub fn call_timers(reg: bool, backoff: u8, syscode: u16, emergency_ms: u32, call_ms: u32) -> Bcast {
    let p1 = token(emergency_ms, 510, emerg_secs) << 5;
    let p2 = token(call_ms, 4094, msms_secs) << 12;
    Bcast { lb: true, ann: BC_CALL_TIMERS, parms1: p1 as u16, reg, backoff, syscode, parms2: p2 }
}

/// The emergency and MS-MS call timers' seconds a CallTimer_Parms gives (None: the MS's own; u32::MAX: infinity).
pub fn call_timers_of(b: &Bcast) -> (Option<u32>, Option<u32>) {
    let (e, m) = (u32::from(b.parms1) >> 5 & 0x1FF, b.parms2 >> 12 & 0xFFF);
    let e = match e {
        0 => None,
        511 => Some(u32::MAX),
        t => Some(emerg_secs(t)),
    };
    let m = match m {
        0 => None,
        4095 => Some(u32::MAX),
        t => Some(msms_secs(t)),
    };
    (e, m)
}

/// Vote_Now (table 7.71): the adjacent TSCC's 14 syscode bits, its Active_Connection when known, the priorities
/// (table 6.81) and CH_VOTE, its logical channel (a single CSBK).
pub fn vote_now(reg: bool, backoff: u8, syscode: u16, n: &Neighbor) -> Bcast {
    let p2 = (u32::from(n.active_known != 0) << 23)
        | (u32::from(n.active_known != 0 && n.active != 0) << 22)
        | (u32::from(n.prio_confirmed & 7) << 19)
        | (u32::from(n.prio_adjacent & 7) << 16)
        | u32::from(n.chan & 0xFFF);
    Bcast { lb: true, ann: BC_VOTE_NOW, parms1: n.syscode >> 2, reg, backoff, syscode, parms2: p2 }
}

/// Ann_WD_TSCC as a CSBK (table 7.69): up to two TSCCs, each with its color code (0 for a withdrawal) and AW_FLAG
/// (0 add, 1 withdraw); CHNULL (0) for none.
pub fn ann_wd_tscc(reg: bool, backoff: u8, syscode: u16, ch: [(u16, u8, bool); 2]) -> Bcast {
    let cc = |(c, cc, wd): (u16, u8, bool)| if c == 0 || wd { 0 } else { u16::from(cc & 15) };
    let p1 = cc(ch[0]) << 6 | cc(ch[1]) << 2 | u16::from(ch[0].2) << 1 | u16::from(ch[1].2);
    let p2 = u32::from(ch[0].0 & 0xFFF) << 12 | u32::from(ch[1].0 & 0xFFF);
    Bcast { lb: true, ann: BC_ANN_WD_TSCC, parms1: p1, reg, backoff, syscode, parms2: p2 }
}

/// MassReg (table 7.76): Reg_Window (4) and the Aloha-style Mask (5) in Parms 1, the address in Parms 2.
pub fn mass_reg(reg: bool, backoff: u8, syscode: u16, window: u8, mask: u8, addr: u32) -> Bcast {
    let p1 = u16::from(window & 15) << 5 | u16::from(mask.min(24));
    Bcast { lb: true, ann: BC_MASS_REG, parms1: p1, reg, backoff, syscode, parms2: addr & 0xFF_FFFF }
}

/// A MassReg's Reg_Window, Mask and address.
pub fn mass_reg_of(b: &Bcast) -> (u8, u8, u32) {
    ((b.parms1 >> 5 & 15) as u8, (b.parms1 & 31) as u8, b.parms2)
}

/// Whether a MassReg's Mask and address take in this MS (6.4.5.1, as 6.1.3).
pub fn mass_reg_applies(mask: u8, addr: u32, id: u32) -> bool {
    let m = if mask >= 24 { 0xFF_FFFF } else { (1u32 << mask) - 1 };
    (addr ^ id) & m == 0
}

/// PS_Counter (6.4.7.1): the Common_Slot_Counter (one step a 120 ms short LC) without its 2 low bits, 0-127.
pub fn ps_counter(k: i64) -> u8 {
    (k.div_euclid(PS_FRAME_K).rem_euclid(128)) as u8
}

/// Whether slot k is in the wake frame of PowerSave_RQ rq with PowerSave_Offset off (table 6.14).
pub fn ps_wake(k: i64, rq: u8, off: u8) -> bool {
    let mask = (1u8 << rq.min(7)) - 1;
    ps_counter(k) & mask == off & mask
}

#[derive(Clone, Copy, Default)]
pub(super) struct BcState {
    next_timers_ms: i64,
    next_vote_ms: i64,
    vote_idx: usize,
    vote_blk_k: i64,
    next_ann_ms: i64,
    ann_idx: usize,
    mass_window: u8,
    mass_mask: u8,
    mass_addr: u32,
    mass_left: u16,
    mass_frame: i64,
    next_mass_ms: i64,
    started: bool,
}

impl Site {
    /// 6.4.5.0: demands a registration from one MS (Mask 24, its address) when no mass re-registration is under
    /// way; false when one is (try again later).
    pub(super) fn demand_registration(&mut self, ms: u32) -> bool {
        if self.bc.mass_left > 0 || ms == 0 {
            return false;
        }
        self.mass_registration(DEMAND_WINDOW_S, ms)
    }

    /// A registration from ms: a demand of it alone is answered, its repeats stop.
    pub(super) fn demand_answered(&mut self, ms: u32) {
        if self.bc.mass_left > 0 && self.bc.mass_mask == 24 && self.bc.mass_addr == ms && self.bc.mass_window != 0 {
            self.bc.mass_left = 0;
        }
    }

    /// Starts a mass re-registration: every MS (ms 0) or one MS, within secs (0 cancels one under way).
    pub(super) fn mass_registration(&mut self, secs: u32, ms: u32) -> bool {
        if ms > 0xFF_FFFF {
            return false;
        }
        // the repeats all fall in the window, so an MS that heard one ignores the rest (6.4.5.1 e): with power save
        // one in each frame for the longest cycle, and the window at least that long
        let left = if self.cfg.power_save > 0 { 1u16 << self.cfg.power_save } else { MASS_SENDS };
        let span_s = if self.cfg.power_save > 0 { f64::from(left) * PS_FRAME_K as f64 * 0.03 } else { 0.0 };
        let w = if secs == 0 { 0 } else { reg_window_of(f64::from(secs).max(span_s)) };
        self.bc.mass_window = w;
        self.bc.mass_mask = if ms != 0 { 24 } else { 0 };
        self.bc.mass_addr = ms;
        self.bc.mass_left = left; // a cancel too: every sleeping MS hears it
        self.bc.mass_frame = -1;
        self.bc.next_mass_ms = self.now;
        self.log(format_args!("mass re-registration, window {} s, {}", reg_window_ms(w) / 1000, ms));
        true
    }

    /// The announcements of this module that are due (at most one a call), for due_bcast.
    pub(super) fn due_bcast_more(&mut self, reg: bool, bo: u8, sc: u16) -> Option<([u8; 10], u8, Option<[u8; 10]>)> {
        let now = self.now;
        if !self.bc.started {
            self.bc.started = true;
            if self.cfg.mass_reg_start != 0 {
                let secs = reg_window_ms(self.cfg.mass_reg_start).div_ceil(1000);
                self.mass_registration(secs, 0);
            }
        }
        if self.bc.mass_left > 0 && now >= self.bc.next_mass_ms {
            let frame = self.k.div_euclid(PS_FRAME_K);
            if self.cfg.power_save == 0 || frame != self.bc.mass_frame {
                // with power save: once in each frame, so every wake frame hears one
                self.bc.mass_frame = frame;
                self.bc.mass_left -= 1;
                let every = (i64::from(reg_window_ms(self.bc.mass_window)) / i64::from(MASS_SENDS)).clamp(60, 1000);
                self.bc.next_mass_ms = if self.cfg.power_save > 0 { now } else { now + every };
                let b = mass_reg(reg, bo, sc, self.bc.mass_window, self.bc.mass_mask, self.bc.mass_addr);
                return Some((b.encode(), DT_CSBK, None));
            }
        }
        if self.cfg.call_timers_ms > 0 && now >= self.bc.next_timers_ms {
            self.bc.next_timers_ms = now + self.cfg.call_timers_ms as i64;
            let b = call_timers(reg, bo, sc, self.cfg.emergency_call_ms, self.cfg.call_ms);
            return Some((b.encode(), DT_CSBK, None));
        }
        let nn = self.nneighbors;
        if self.cfg.vote_now_ms > 0 && nn > 0 && now >= self.bc.next_vote_ms {
            let n = self.neighbors[self.bc.vote_idx % nn];
            self.bc.vote_idx = (self.bc.vote_idx + 1) % nn;
            self.bc.next_vote_ms = now + (self.cfg.vote_now_ms as i64 / nn as i64).max(1000);
            self.bc.vote_blk_k = self.k + 2 * (VOTE_BLK + 1); // this block, then VOTE_BLK TSCC slots
            return Some((vote_now(reg, bo, sc, &n).encode(), DT_CSBK, None));
        }
        if self.cfg.ann_tscc_ms > 0 && nn > 0 && now >= self.bc.next_ann_ms {
            let pairs = nn.div_ceil(2);
            let i = self.bc.ann_idx % pairs * 2;
            self.bc.ann_idx = (self.bc.ann_idx + 1) % pairs;
            self.bc.next_ann_ms = now + (self.cfg.ann_tscc_ms as i64 / pairs as i64).max(1000);
            let cc = self.cfg.cc; // one color code for the system (the neighbors' isn't known)
            let one = |n: &Neighbor| (n.chan, cc, n.active_known != 0 && n.active == 0);
            let a = one(&self.neighbors[i]);
            let b = if i + 1 < nn { one(&self.neighbors[i + 1]) } else { (0, 0, false) };
            return Some((ann_wd_tscc(reg, bo, sc, [a, b]).encode(), DT_CSBK, None));
        }
        None
    }

    /// The TSCC's block in the VOTE_BLK slots after a Vote Now Advice: a C_ALOHA that lets no MS in (ADRNULL,
    /// Mask 24), so a voting MS misses nothing addressed to it.
    pub(super) fn vote_blocked(&mut self) -> Option<[u8; 10]> {
        if self.k >= self.bc.vote_blk_k {
            return None;
        }
        let mut a = Aloha::decode(&self.aloha())?;
        a.mask = 24;
        a.addr = ADRNULL;
        Some(a.encode())
    }

    /// The registration's C_ACKD, with PowerSave_Offset in Response_Info (6.4.7.2.1, table 6.13): granted when the
    /// MS asks for 1:2^rq with rq up to services.power_save; 0 (registered, power save not available) otherwise.
    /// The offset spreads the MSs over the frames by subscribed talkgroup (one frame for a talkgroup's members).
    /// Only Reg_Accepted carries it (table 7.41): a talkgroup refused (Reg_Subscription/Attachment) gets
    /// Response_Info 000 0000 (6.4.4.1.13.1) and no power save (the MS may not ask for both: PS_RQ 000b there).
    pub(super) fn reg_ack(&mut self, i: usize, r: &Rand, reason: u8) {
        let rq = (r.service_options & SO_PS_RQ) >> 1;
        let off = if reason == R_REG_ACCEPTED && rq != 0 && rq <= self.cfg.power_save {
            let key = if self.regs[i].rec.sub_tg != 0 { self.regs[i].rec.sub_tg } else { self.regs[i].rec.id };
            let n = (1u32 << rq) - 1; // offsets 1 to 2^rq - 1
            (1 + key.wrapping_mul(2_654_435_761) % n) as u8
        } else {
            0
        };
        let g = &mut self.regs[i];
        (g.ps, g.ps_off) = if off != 0 { (rq, off) } else { (0, 0) };
        g.awake_ms = self.now + self.cfg.awake_ms as i64;
        let a = Ack { op: OP_ACKD, response_info: off, reason, target: r.source, source: REGI };
        self.push(DT_CSBK, a.encode(), false, None);
    }

    /// Whether a C_AHOY to ms may go now: it isn't in power save, or it's awake (heard from or signalled within
    /// services.power_save_awake_s), or this is early in its wake frame. Marks it awake when it may.
    pub(super) fn ps_awake(&mut self, ms: u32) -> bool {
        let Some(i) = self.reg_index(ms) else { return true };
        let (now, k) = (self.now, self.k);
        let g = &mut self.regs[i];
        if g.ps == 0 {
            return true;
        }
        let awake = now < g.awake_ms
            || now - g.rec.at_ms < self.cfg.awake_ms as i64
            || (ps_wake(k, g.ps, g.ps_off) && k.rem_euclid(PS_FRAME_K) < PS_SEND_K);
        if awake {
            g.awake_ms = g.awake_ms.max(now + self.cfg.awake_ms as i64);
        }
        awake
    }

    /// The PowerSave_RQ and PowerSave_Offset the site holds for ms.
    pub fn power_save_of(&self, ms: u32) -> (u8, u8) {
        self.reg_index(ms).map_or((0, 0), |i| (self.regs[i].ps, self.regs[i].ps_off))
    }

    /// The longest wait for an MS's wake frame, in ms (added to a call's set-up time limit while it sleeps).
    pub(super) fn ps_wait_ms(&self, ms: u32) -> i64 {
        let (rq, _) = self.power_save_of(ms);
        if rq == 0 {
            0
        } else {
            (1i64 << rq) * PS_FRAME_K * 30
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn call_timer_tokens() {
        // table A.2 / A.4 end points
        assert_eq!(emerg_secs(510), 2315 * 60);
        assert_eq!(emerg_secs(40), 9 * 60);
        assert_eq!(msms_secs(4094), 3976 * 60);
        assert_eq!(msms_secs(107), 295);
        assert_eq!(msms_secs(138), 20 * 60);
        // the largest that doesn't pass: 180 s is 60 + 5 x 24 (84); 1800 s, 21 min + 9 x 1 min (148)
        assert_eq!(token(180_000, 4094, msms_secs), 84);
        assert_eq!(token(1_800_000, 4094, msms_secs), 148);
        assert_eq!(token(300_000, 510, emerg_secs), 32); // 3.5 min + 3 x 30 s
        assert_eq!(token(0, 4094, msms_secs), 4095);
        assert_eq!(token(500, 4094, msms_secs), 1);
        let b = Bcast::decode(&call_timers(true, 2, 0x1234, 300_000, 180_000).encode()).unwrap();
        assert_eq!(b.ann, BC_CALL_TIMERS);
        assert_eq!(call_timers_of(&b), (Some(300), Some(180)));
        assert_eq!(b.parms1 & 31, 0, "packet timer: the MS's");
        assert_eq!(b.parms2 & 0xFFF, 0, "line timer: the MS's");
    }

    #[test]
    fn mass_reg_fields() {
        let b = Bcast::decode(&mass_reg(false, 0, 1, 9, 24, 3_100_123).encode()).unwrap();
        assert_eq!((b.ann, mass_reg_of(&b)), (BC_MASS_REG, (9, 24, 3_100_123)));
        assert_eq!(b.parms1 >> 9, 0, "5 reserved bits");
        assert!(mass_reg_applies(24, 3_100_123, 3_100_123));
        assert!(!mass_reg_applies(24, 3_100_123, 3_100_124));
        assert!(mass_reg_applies(0, ADRNULL, 3_100_124));
        assert!(mass_reg_applies(1, 0, 3_100_124) && !mass_reg_applies(1, 0, 3_100_123));
        assert_eq!(reg_window_of(0.0), 0);
        assert_eq!(reg_window_of(0.4), 1);
        assert_eq!(reg_window_of(30.0), 7);
        assert_eq!(reg_window_of(31.0), 8);
        assert_eq!(reg_window_of(1e9), 15);
        assert_eq!(reg_window_ms(9), 300_000);
    }

    #[test]
    fn vote_now_and_ann_wd_fields() {
        let n = Neighbor { syscode: 0x2A5D, chan: 77, active_known: 1, active: 1, prio_confirmed: 2, prio_adjacent: 5 };
        let b = Bcast::decode(&vote_now(true, 1, 0x1111, &n).encode()).unwrap();
        assert_eq!((b.ann, b.parms1), (BC_VOTE_NOW, 0x2A5D >> 2));
        assert_eq!(b.parms2, 1 << 23 | 1 << 22 | 2 << 19 | 5 << 16 | 77);
        let b = Bcast::decode(&ann_wd_tscc(true, 1, 0x1111, [(77, 3, false), (0, 0, false)]).encode()).unwrap();
        assert_eq!((b.ann, b.parms1, b.parms2), (BC_ANN_WD_TSCC, 3 << 6, 77 << 12));
        let b = Bcast::decode(&ann_wd_tscc(true, 1, 0x1111, [(77, 3, true), (78, 3, false)]).encode()).unwrap();
        assert_eq!((b.parms1, b.parms2), (3 << 2 | 1 << 1, 77 << 12 | 78), "a withdrawal has color code 0");
        assert_eq!(b.parms1 >> 10, 0, "4 reserved bits");
    }

    #[test]
    fn power_save_frames() {
        // table 6.14: PowerSave_RQ 2 (1:4), offset 2: PS_Counter 66 and 70 wake, 65, 67, 68, 69 sleep
        let k = |ps: i64| ps * 16;
        for (ps, wake) in [(65, false), (66, true), (67, false), (68, false), (69, false), (70, true)] {
            assert_eq!(ps_wake(k(ps), 2, 2), wake, "PS_Counter {ps}");
            assert_eq!(ps_counter(k(ps) + 15), ps as u8);
        }
        // it counts the Common_Slot_Counter's high bits: 512 counts of 4 slots roll over
        assert_eq!(ps_counter(512 * 4), 0);
    }
}
