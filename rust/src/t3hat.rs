// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! A Tier III carrier on a stock duplex MMDVM modem (docs/spec/fw/tier3-hat.md; C header rdof/t3hat.h).
//!
//! The modem keeps its own slot clock, sends its own CACH and plays the host's bursts from a queue per timeslot,
//! filling a slot whose queue is empty with its own idle burst. The host can't name the slot a burst goes in: it
//! can only choose when to write it. This places the writes: the receiver (an SDR beside the modem) hears the
//! carrier, its bursts give the modem's slot grid on the receiver's sample clock, and each burst is written a lead
//! before its slot's air time, inside the window where the modem takes it for that slot and no earlier one. A
//! burst written that late leaves its queue empty when the modem builds the CACH before the other timeslot, so the
//! AT there says idle; written one slot earlier, it is queued then and the AT says busy (the modem's own rule,
//! measured: tier3-hat.md section 2). Each burst heard is matched to the one written for it, which checks where it
//! went; a timeslot that runs behind (its queue holds one too many) drops a write to catch up.
//!
//! `Sim` is the modem as measured, for the host tests: its own clock with a ppm error, a queue per timeslot, the
//! idle fill, its CACH (TC, the AT from the other timeslot's queue, the short LC from DMR_SHORTLC latched at the
//! start of each 4-slot cycle), and the drop of its carrier when both queues have been empty a while.
//!
//! No I/O, no allocation; the platform serializes the calls.

use core::ffi::c_int;

pub const BURST: usize = 33;
const PEND: usize = 16; // bursts written and not yet heard, per timeslot
const FIT: usize = 128; // the bursts heard that the grid's line is fitted to
const EVQ: usize = 512;
const SLOT48: f64 = 1440.0; // a 30 ms slot at 48 kS/s
const RESTART_GAP48: i64 = 19200; // 400 ms with nothing heard: the modem may have keyed again (a new grid)
const OUTLIER48: f64 = 192.0; // 4 ms off the line
const MATCH_BITS: u32 = 24; // a heard burst is a written one with at most this many bits different
const INBOUND_OFF48: f64 = 60.0; // an inbound burst starts 1.25 ms before its outbound slot's burst

// events (rdof_t3hat_event)
pub const EV_WRITE: c_int = 1; // k, ts; v: written at, lead, air time
pub const EV_AIR: c_int = 2; // k, m, ts; v: e (slots late), air time less written at, lead, AT of the CACH before it
pub const EV_FILL: c_int = 3; // m, ts: the modem's own burst (none of ours matched); v: AT before it
pub const EV_LOST: c_int = 4; // k, ts: written, never heard
pub const EV_GRID: c_int = 5; // m: the grid (re)started; v: position, slot length (48 kS/s samples)
pub const EV_PLACE: c_int = 6; // k, m: trunk slot k is modem slot m from now; v: d
pub const EV_SKIP: c_int = 7; // k, ts: not written (too late for it, or its timeslot runs behind)

/// rdof_t3hat_stats_t
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Stats {
    pub written: u32,
    pub matched: u32,
    pub on_time: u32,
    pub late: u32,
    pub early: u32,
    pub lost: u32,
    pub fills: u32,
    pub skipped: u32,
    pub grids: u32,
    pub placed: bool,
    pub d: i64,
    pub ppm: f64,      // the modem's slot clock against the receiver's sample clock (+: slots shorter), 0 unknown
    pub delay_min: f64, // air time less write time of the bursts on time, s
    pub delay_max: f64,
    pub delay_sum: f64,
}

#[derive(Clone, Copy)]
struct Pending {
    k: i64,
    t: f64,
    lead: f64,
    burst: [u8; BURST],
}

const NO_PENDING: Pending = Pending { k: 0, t: 0.0, lead: 0.0, burst: [0; BURST] };

#[derive(Clone, Copy, Default)]
struct Ev {
    kind: c_int,
    ts: c_int,
    k: i64,
    m: i64,
    v: [f64; 4],
}

fn floor(x: f64) -> f64 {
    let i = x as i64 as f64;
    if i > x {
        i - 1.0
    } else {
        i
    }
}
fn round(x: f64) -> i64 {
    floor(x + 0.5) as i64
}
fn ceil(x: f64) -> i64 {
    -(floor(-x) as i64)
}
fn absf(x: f64) -> f64 {
    if x < 0.0 {
        -x
    } else {
        x
    }
}

pub fn bit_distance(a: &[u8; BURST], b: &[u8; BURST]) -> u32 {
    a.iter().zip(b.iter()).map(|(x, y)| (x ^ y).count_ones()).sum()
}

pub struct Hat {
    lead: [f64; 2], // by the AT wanted in the CACH before the other timeslot's next slot: 0 just in time, 1 a slot ahead
    min_lead: f64,  // a burst due with less than this left is skipped
    // the receiver's clock: host time = off + position / 48000 (the earliest arrival, over 2 s windows)
    off: f64,
    off_ok: bool,
    win_min: f64,
    win_t: f64,
    // the modem's slot grid: modem slot m's burst starts at position a_p + (m - a_m) * per
    grid: bool,
    fit_m: [i64; FIT],
    fit_p: [i64; FIT],
    nfit: usize,
    fit_i: usize,
    a_m: i64,
    a_p: f64,
    per: f64,
    last_m: i64,
    last_pos: i64,
    bad: u32,
    first_m: i64,
    first_p: i64,
    long: [f64; 5], // the slope over the whole grid: n, sum x, sum y, sum xx, sum xy (from the first burst)
    lc_phase: i64, // m % 4 of the slots whose CACH starts a short LC, -1 unknown
    // the trunk's slots: trunk k is modem slot k - d
    placed: bool,
    d: i64,
    next_k: i64,
    pend: [[Pending; PEND]; 2],
    np: [usize; 2],
    slip: [i64; 2],
    slip_n: [u32; 2],
    drop: [u32; 2],
    ev: [Ev; EVQ],
    ev_h: usize,
    ev_n: usize,
    st: Stats,
}

impl Hat {
    pub fn new(lead_idle: f64, lead_busy: f64) -> Hat {
        Hat {
            lead: [lead_idle, lead_busy],
            min_lead: lead_idle - 0.01,
            off: 0.0,
            off_ok: false,
            win_min: f64::INFINITY,
            win_t: 0.0,
            grid: false,
            fit_m: [0; FIT],
            fit_p: [0; FIT],
            nfit: 0,
            fit_i: 0,
            a_m: 0,
            a_p: 0.0,
            per: SLOT48,
            last_m: 0,
            last_pos: 0,
            bad: 0,
            first_m: 0,
            first_p: 0,
            long: [0.0; 5],
            lc_phase: -1,
            placed: false,
            d: 0,
            next_k: 0,
            pend: [[NO_PENDING; PEND]; 2],
            np: [0; 2],
            slip: [0; 2],
            slip_n: [0; 2],
            drop: [0; 2],
            ev: [Ev::default(); EVQ],
            ev_h: 0,
            ev_n: 0,
            st: Stats { delay_min: f64::INFINITY, ..Stats::default() },
        }
    }

    pub fn set_leads(&mut self, lead_idle: f64, lead_busy: f64, min_lead: f64) {
        self.lead = [lead_idle, lead_busy];
        self.min_lead = min_lead;
    }

    pub fn lead(&self, at: c_int) -> f64 {
        self.lead[if at > 0 { 1 } else { 0 }]
    }

    fn event(&mut self, kind: c_int, ts: c_int, k: i64, m: i64, v: [f64; 4]) {
        if self.ev_n == EVQ {
            self.ev_h = (self.ev_h + 1) % EVQ; // the oldest goes
            self.ev_n -= 1;
        }
        self.ev[(self.ev_h + self.ev_n) % EVQ] = Ev { kind, ts, k, m, v };
        self.ev_n += 1;
    }

    pub fn next_event(&mut self) -> Option<(c_int, c_int, i64, i64, [f64; 4])> {
        if self.ev_n == 0 {
            return None;
        }
        let e = self.ev[self.ev_h];
        self.ev_h = (self.ev_h + 1) % EVQ;
        self.ev_n -= 1;
        Some((e.kind, e.ts, e.k, e.m, e.v))
    }

    /// The receiver's position now48 is the host's now (each turn of the loop that reads it).
    pub fn clock(&mut self, now: f64, now48: i64) {
        let v = now - now48 as f64 / 48000.0;
        if !self.off_ok {
            self.off = v;
            self.off_ok = true;
            self.win_min = v;
            self.win_t = now;
            return;
        }
        if v < self.off {
            self.off = v;
        }
        if v < self.win_min {
            self.win_min = v;
        }
        if now - self.win_t >= 2.0 {
            self.off = self.win_min;
            self.win_min = f64::INFINITY;
            self.win_t = now;
        }
    }

    pub fn host_of(&self, pos48: f64) -> f64 {
        self.off + pos48 / 48000.0
    }

    fn pos_of_m(&self, m: i64) -> f64 {
        self.a_p + (m - self.a_m) as f64 * self.per
    }

    /// The host time trunk slot k's burst goes on air; None before the grid is placed.
    pub fn air(&self, k: i64) -> Option<f64> {
        if !self.placed {
            return None;
        }
        Some(self.host_of(self.pos_of_m(k - self.d)))
    }

    fn lose_all(&mut self) {
        for ts in 0..2 {
            for i in 0..self.np[ts] {
                let k = self.pend[ts][i].k;
                self.st.lost += 1;
                self.event(EV_LOST, ts as c_int + 1, k, 0, [0.0; 4]);
            }
            self.np[ts] = 0;
            self.slip_n[ts] = 0;
            self.drop[ts] = 0;
        }
    }

    /// Start over (the modem unkeyed, or a bench job took it): a new grid and a new placing.
    pub fn reset(&mut self) {
        self.lose_all();
        self.grid = false;
        self.placed = false;
        self.st.placed = false;
        self.nfit = 0;
        self.lc_phase = -1;
    }

    fn fit(&mut self) {
        let n = self.nfit;
        let (rm, rp) = (self.last_m, self.last_pos);
        let (mut sx, mut sy, mut sxx, mut sxy) = (0.0, 0.0, 0.0, 0.0);
        for i in 0..n {
            let x = (self.fit_m[i] - rm) as f64;
            let y = (self.fit_p[i] - rp) as f64;
            sx += x;
            sy += y;
            sxx += x * x;
            sxy += x * y;
        }
        let nf = n as f64;
        let den = nf * sxx - sx * sx;
        if n >= 8 && den > 0.0 {
            let per = (nf * sxy - sx * sy) / den;
            if absf(per / SLOT48 - 1.0) < 200e-6 {
                self.per = per;
            }
        }
        // the line through the points' mean with that slope
        let b = (sy - self.per * sx) / nf;
        self.a_m = rm;
        self.a_p = rp as f64 + b;
    }

    fn restart(&mut self, pos48: i64, slot: c_int) {
        let had = self.grid;
        self.lose_all();
        let mut m = if had { self.last_m + 1000 } else { 0 };
        if (m & 1) != i64::from(slot - 1) {
            m += 1;
        }
        self.grid = true;
        self.placed = false;
        self.st.placed = false;
        self.nfit = 0;
        self.fit_i = 0;
        self.a_m = m;
        self.a_p = pos48 as f64;
        self.last_m = m;
        self.last_pos = pos48;
        self.first_m = m;
        self.first_p = pos48;
        self.long = [0.0; 5];
        self.bad = 0;
        self.lc_phase = -1;
        self.st.grids += 1;
        let per = self.per;
        self.event(EV_GRID, slot, 0, m, [pos48 as f64, per, 0.0, 0.0]);
    }

    /// An outbound burst the receiver heard on the carrier: its position, timeslot (from the CACH's TC), the CACH's
    /// AT and LCSS (-1: none read), the burst. Returns its modem slot (None: not on the grid).
    pub fn heard(&mut self, pos48: i64, slot: c_int, at: c_int, lcss: c_int, burst: &[u8; BURST]) -> Option<i64> {
        if slot != 1 && slot != 2 {
            return None;
        }
        if !self.grid || pos48 - self.last_pos > RESTART_GAP48 || pos48 < self.last_pos - RESTART_GAP48 {
            self.restart(pos48, slot);
        }
        let mut m = self.a_m + round((pos48 as f64 - self.a_p) / self.per);
        let r = pos48 as f64 - self.pos_of_m(m);
        if (m & 1) != i64::from(slot - 1) || absf(r) > OUTLIER48 {
            self.bad += 1;
            if self.bad < 3 {
                return None;
            }
            self.restart(pos48, slot);
            m = self.a_m;
        }
        self.bad = 0;
        if m <= self.last_m && self.nfit > 0 {
            return None; // a repeat
        }
        self.fit_m[self.fit_i] = m;
        self.fit_p[self.fit_i] = pos48;
        self.fit_i = (self.fit_i + 1) % FIT;
        if self.nfit < FIT {
            self.nfit += 1;
        }
        self.last_m = m;
        self.last_pos = pos48;
        self.fit();
        let (x, y) = ((m - self.first_m) as f64, (pos48 - self.first_p) as f64);
        let l = &mut self.long;
        l[0] += 1.0;
        l[1] += x;
        l[2] += y;
        l[3] += x * x;
        l[4] += x * y;
        let den = l[0] * l[3] - l[1] * l[1];
        if x > 300.0 && den > 0.0 {
            let per = (l[0] * l[4] - l[1] * l[2]) / den;
            self.st.ppm = (SLOT48 / per - 1.0) * 1e6;
        }
        if lcss == 1 && slot == 1 {
            self.lc_phase = m.rem_euclid(4);
        }
        if self.placed {
            self.match_heard(m, slot, at, burst);
        }
        Some(m)
    }

    fn match_heard(&mut self, m: i64, slot: c_int, at: c_int, burst: &[u8; BURST]) {
        let ts = (slot - 1) as usize;
        let air = self.host_of(self.pos_of_m(m));
        let n = if self.np[ts] < 6 { self.np[ts] } else { 6 };
        let mut hit = None;
        for i in 0..n {
            let p = &self.pend[ts][i];
            if p.t < air && bit_distance(&p.burst, burst) <= MATCH_BITS {
                hit = Some(i);
                break;
            }
        }
        let Some(i) = hit else {
            self.st.fills += 1;
            self.event(EV_FILL, slot, m + self.d, m, [f64::from(at), 0.0, 0.0, 0.0]);
            return;
        };
        for j in 0..i {
            let k = self.pend[ts][j].k;
            self.st.lost += 1;
            self.event(EV_LOST, slot, k, 0, [0.0; 4]);
        }
        let p = self.pend[ts][i];
        let np = self.np[ts];
        self.pend[ts].copy_within(i + 1..np, 0);
        self.np[ts] = np - i - 1;
        let e = m + self.d - p.k;
        let delay = air - p.t;
        self.st.matched += 1;
        if e == 0 {
            self.st.on_time += 1;
            if delay < self.st.delay_min {
                self.st.delay_min = delay;
            }
            if delay > self.st.delay_max {
                self.st.delay_max = delay;
            }
            self.st.delay_sum += delay;
            self.slip_n[ts] = 0;
        } else {
            if e > 0 {
                self.st.late += 1;
            } else {
                self.st.early += 1;
            }
            if e == self.slip[ts] {
                self.slip_n[ts] += 1;
            } else {
                self.slip[ts] = e;
                self.slip_n[ts] = 1;
            }
            // two in a row a slot behind: the queue holds one too many; a write dropped takes it out
            if e > 0 && self.slip_n[ts] >= 2 && self.drop[ts] == 0 {
                self.drop[ts] = (e / 2) as u32;
                self.slip_n[ts] = 0;
            }
        }
        self.event(EV_AIR, slot, p.k, m, [e as f64, delay, p.lead, f64::from(at)]);
    }

    /// The trunk slot an inbound burst at pos48 is under (aligned timing), None before the grid is placed.
    pub fn inbound_k(&self, pos48: i64) -> Option<i64> {
        if !self.placed {
            return None;
        }
        let m = self.a_m + round((pos48 as f64 + INBOUND_OFF48 - self.a_p) / self.per);
        Some(m + self.d)
    }

    /// The next slot to write and its air time; None while the grid isn't known. k_min: the first trunk slot it
    /// may place on (the next after the last the controller gave). Slots too late to make are skipped here.
    pub fn next(&mut self, now: f64, k_min: i64) -> Option<(i64, f64)> {
        // wait for the short LC's cycle to place on it (a modem that sends none: 40 slots)
        if !self.grid || self.nfit < 6 || !self.off_ok || (self.lc_phase < 0 && self.nfit < 40) {
            return None;
        }
        if !self.placed {
            let lead = if self.lead[1] > self.lead[0] { self.lead[1] } else { self.lead[0] };
            let want48 = (now + lead + 0.01 - self.off) * 48000.0;
            let mut m = self.a_m + ceil((want48 - self.a_p) / self.per);
            m += m & 1; // TS1
            let mut k0 = k_min;
            k0 += k0 & 1;
            let mut d = k0 - m;
            if self.lc_phase >= 0 && (self.lc_phase + d).rem_euclid(4) != 0 {
                d += 2; // the modem's short LC cycle starts on trunk slots k % 4 == 0
            }
            self.d = d;
            self.next_k = m + d;
            self.placed = true;
            self.st.placed = true;
            self.st.d = d;
            self.event(EV_PLACE, 1, m + d, m, [d as f64, 0.0, 0.0, 0.0]);
        }
        loop {
            let k = self.next_k;
            let air = self.air(k)?;
            if air - now >= self.min_lead {
                return Some((k, air));
            }
            self.skip(k);
        }
    }

    fn skip(&mut self, k: i64) {
        self.st.skipped += 1;
        self.event(EV_SKIP, (k & 1) as c_int + 1, k, 0, [0.0; 4]);
        self.next_k = k + 1;
    }

    /// Whether slot k's burst should go unwritten: its timeslot's queue holds one too many.
    pub fn dropping(&mut self, k: i64) -> bool {
        let ts = (k & 1) as usize;
        if self.drop[ts] == 0 {
            return false;
        }
        self.drop[ts] -= 1;
        self.skip(k);
        true
    }

    /// Slot k's burst written at now, lead before its air time.
    pub fn wrote(&mut self, k: i64, burst: &[u8; BURST], now: f64, lead: f64) {
        let ts = (k & 1) as usize;
        if self.np[ts] == PEND {
            let old = self.pend[ts][0].k;
            self.pend[ts].copy_within(1..PEND, 0);
            self.np[ts] -= 1;
            self.st.lost += 1;
            self.event(EV_LOST, ts as c_int + 1, old, 0, [0.0; 4]);
        }
        self.pend[ts][self.np[ts]] = Pending { k, t: now, lead, burst: *burst };
        self.np[ts] += 1;
        self.st.written += 1;
        self.next_k = k + 1;
        let air = self.air(k).unwrap_or(0.0);
        self.event(EV_WRITE, ts as c_int + 1, k, k - self.d, [now, lead, air, 0.0]);
    }

    pub fn stats(&self) -> Stats {
        self.st
    }
    pub fn placed(&self) -> bool {
        self.placed
    }
    pub fn d(&self) -> i64 {
        self.d
    }
}

// ── the modem, simulated ────────────────────────────────────────────────────────────────────────────────────

const SIM_CAP_MAX: usize = 16;
const SIM_AIRQ: usize = 16;
const IDLE_BURST: [u8; BURST] = [0x55; BURST]; // the sim's own fill: not a valid burst, never one of the host's

/// A slot the simulated modem sent.
#[derive(Clone, Copy)]
pub struct SimAir {
    pub m: i64,
    pub slot: c_int,
    pub t: f64,
    pub burst: [u8; BURST],
    pub host: bool, // the host's burst (false: its idle fill)
    pub at: c_int,
    pub lcss: c_int,
    pub frag: c_int, // which of the short LC's 4 rows
    pub slc: [u8; 9],
}

const NO_AIR: SimAir =
    SimAir { m: 0, slot: 0, t: 0.0, burst: [0; BURST], host: false, at: 0, lcss: 0, frag: 0, slc: [0; 9] };

pub struct Sim {
    ppm: f64,
    fetch: f64,
    cap: usize,
    hang: u32,
    key_delay: f64,
    on: bool,
    keyed: bool,
    t0: f64,
    m_next: i64,
    m_base: i64,
    q: [[[u8; BURST]; SIM_CAP_MAX]; 2],
    qn: [usize; 2],
    empty: u32,
    slc: [u8; 9],
    slc_pend: [u8; 9],
    has_pend: bool,
    out: [u8; 512],
    nout: usize,
    air: [SimAir; SIM_AIRQ],
    air_h: usize,
    air_n: usize,
    rd: [u8; 256],
    nrd: usize,
    pub naks: u32,
    pub keyups: u32,
}

const ACK: u8 = 0x70;
const NAK: u8 = 0x7F;

impl Sim {
    pub fn new(ppm: f64, fetch: f64, cap: usize) -> Sim {
        Sim {
            ppm,
            fetch,
            cap: if cap == 0 || cap > SIM_CAP_MAX { SIM_CAP_MAX } else { cap },
            hang: 6,
            key_delay: 0.02,
            on: false,
            keyed: false,
            t0: 0.0,
            m_next: 0,
            m_base: 0,
            q: [[[0; BURST]; SIM_CAP_MAX]; 2],
            qn: [0; 2],
            empty: 0,
            slc: [0; 9],
            slc_pend: [0; 9],
            has_pend: false,
            out: [0; 512],
            nout: 0,
            air: [NO_AIR; SIM_AIRQ],
            air_h: 0,
            air_n: 0,
            rd: [0; 256],
            nrd: 0,
            naks: 0,
            keyups: 0,
        }
    }

    fn reply(&mut self, cmd: u8, p: &[u8]) {
        let n = p.len() + 3;
        if self.nout + n > self.out.len() {
            return;
        }
        self.out[self.nout..self.nout + 3].copy_from_slice(&[0xE0, n as u8, cmd]);
        self.out[self.nout + 3..self.nout + n].copy_from_slice(p);
        self.nout += n;
    }

    fn slot_t(&self, m: i64) -> f64 {
        self.t0 + m as f64 * 0.03 / (1.0 + self.ppm * 1e-6)
    }

    fn command(&mut self, cmd: u8, p: &[u8], now: f64) {
        match cmd {
            0x00 => {
                let mut v = [0u8; 24];
                v[0] = 1;
                v[1..21].copy_from_slice(b"MMDVM_HS sim duplex ");
                self.reply(0x00, &v[..21]);
            }
            0x01 => {
                let flags = if self.keyed { 0x01 } else { 0 };
                let s = [0x02, 0x02, flags, 0, (self.cap - self.qn[0]) as u8, (self.cap - self.qn[1]) as u8, 0, 0];
                self.reply(0x01, &s);
            }
            0x18 | 0x1A => {
                let ts = if cmd == 0x18 { 0 } else { 1 };
                if p.len() < 1 + BURST || self.qn[ts] >= self.cap {
                    self.naks += 1;
                    self.reply(NAK, &[cmd, 5]);
                    return;
                }
                let n = self.qn[ts];
                self.q[ts][n].copy_from_slice(&p[1..1 + BURST]);
                self.qn[ts] += 1;
                self.key(now);
            }
            0x1C => {
                if p.len() >= 9 {
                    self.slc_pend.copy_from_slice(&p[..9]);
                    self.has_pend = true;
                }
            }
            0x1D => {
                self.on = p.first().copied().unwrap_or(0) != 0;
                if !self.on {
                    self.keyed = false;
                    self.qn = [0; 2];
                }
            }
            0x1E => {
                let ts = if p.first().copied().unwrap_or(1) == 2 { 1 } else { 0 };
                self.qn[ts] = 0;
            }
            _ => self.reply(ACK, &[cmd]),
        }
    }

    fn key(&mut self, now: f64) {
        if self.on && !self.keyed && self.qn[0] + self.qn[1] > 0 {
            self.keyed = true;
            self.keyups += 1;
            self.t0 = now + self.key_delay + self.fetch;
            self.m_base += 1000;
            self.m_next = 0;
            self.empty = 0;
        }
    }

    /// Bytes from the host.
    pub fn input(&mut self, now: f64, d: &[u8]) {
        for &b in d {
            if self.nrd == 0 && b != 0xE0 {
                continue;
            }
            if self.nrd < self.rd.len() {
                self.rd[self.nrd] = b;
                self.nrd += 1;
            }
            if self.nrd >= 3 && self.nrd == self.rd[1] as usize {
                let (cmd, n) = (self.rd[2], self.nrd);
                let mut p = [0u8; 256];
                p[..n - 3].copy_from_slice(&self.rd[3..n]);
                self.nrd = 0;
                self.command(cmd, &p[..n - 3], now);
            } else if self.nrd >= 2 && (self.rd[1] < 3) {
                self.nrd = 0;
            }
        }
    }

    /// Bytes to the host (replies, NAKs), up to out.len(); returns how many.
    pub fn output(&mut self, out: &mut [u8]) -> usize {
        let n = if self.nout < out.len() { self.nout } else { out.len() };
        out[..n].copy_from_slice(&self.out[..n]);
        self.out.copy_within(n..self.nout, 0);
        self.nout -= n;
        n
    }

    /// The modem's clock run to now: each slot taken from its queue (or filled) fetch before its air time.
    pub fn run(&mut self, now: f64) {
        while self.keyed && self.slot_t(self.m_next) - self.fetch <= now && self.air_n < SIM_AIRQ {
            let m = self.m_next;
            let ts = (m & 1) as usize;
            let mut a = SimAir { m: self.m_base + m, slot: ts as c_int + 1, t: self.slot_t(m), ..NO_AIR };
            if self.qn[ts] > 0 {
                a.burst = self.q[ts][0];
                a.host = true;
                let n = self.qn[ts];
                self.q[ts].copy_within(1..n, 0);
                self.qn[ts] -= 1;
            } else {
                a.burst = IDLE_BURST;
            }
            // the CACH before it: the AT says whether the other timeslot has a burst queued
            a.at = c_int::from(self.qn[1 - ts] > 0);
            let c = m.rem_euclid(4);
            if c == 0 && self.has_pend {
                self.slc = self.slc_pend;
                self.has_pend = false;
            }
            a.frag = c as c_int;
            a.lcss = [1, 3, 3, 2][c as usize];
            a.slc = self.slc;
            self.air[(self.air_h + self.air_n) % SIM_AIRQ] = a;
            self.air_n += 1;
            self.m_next += 1;
            // slots it fills in a row (assumed: the measurement can't tell this from both queues empty at fetch)
            if a.host {
                self.empty = 0;
            } else {
                self.empty += 1;
                if self.empty >= self.hang && self.qn[0] + self.qn[1] == 0 {
                    self.keyed = false; // the carrier drops; the next burst keys it again on a new grid
                }
            }
        }
    }

    /// The next slot sent by now (its air time passed), in order.
    pub fn aired(&mut self, now: f64) -> Option<SimAir> {
        if self.air_n == 0 || self.air[self.air_h].t > now {
            return None;
        }
        let a = self.air[self.air_h];
        self.air_h = (self.air_h + 1) % SIM_AIRQ;
        self.air_n -= 1;
        Some(a)
    }

    pub fn keyed(&self) -> bool {
        self.keyed
    }
    pub fn queued(&self, ts: usize) -> usize {
        self.qn[ts]
    }
}

// ── C ───────────────────────────────────────────────────────────────────────────────────────────────────────

#[no_mangle]
pub extern "C" fn rdof_t3hat_size() -> usize {
    core::mem::size_of::<Hat>()
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3hat_init(h: *mut Hat, lead_idle_s: f64, lead_busy_s: f64) {
    h.write(Hat::new(lead_idle_s, lead_busy_s));
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3hat_set_leads(h: *mut Hat, lead_idle_s: f64, lead_busy_s: f64, min_lead_s: f64) {
    (*h).set_leads(lead_idle_s, lead_busy_s, min_lead_s);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3hat_lead(h: *const Hat, at: c_int) -> f64 {
    (*h).lead(at)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3hat_clock(h: *mut Hat, now: f64, now48: i64) {
    (*h).clock(now, now48);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3hat_heard(
    h: *mut Hat,
    pos48: i64,
    slot: c_int,
    at: c_int,
    lcss: c_int,
    burst: *const u8,
    m: *mut i64,
) -> i64 {
    let b = &*(burst as *const [u8; BURST]);
    match (*h).heard(pos48, slot, at, lcss, b) {
        Some(mm) => {
            if !m.is_null() {
                *m = mm;
            }
            if (*h).placed {
                mm + (*h).d
            } else {
                -1
            }
        }
        None => -1,
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3hat_inbound_k(h: *const Hat, pos48: i64) -> i64 {
    (*h).inbound_k(pos48).unwrap_or(-1)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3hat_next(h: *mut Hat, now: f64, k_min: i64, k: *mut i64, air: *mut f64) -> bool {
    match (*h).next(now, k_min) {
        Some((kk, a)) => {
            *k = kk;
            *air = a;
            true
        }
        None => false,
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3hat_dropping(h: *mut Hat, k: i64) -> bool {
    (*h).dropping(k)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3hat_wrote(h: *mut Hat, k: i64, burst: *const u8, now: f64, lead: f64) {
    (*h).wrote(k, &*(burst as *const [u8; BURST]), now, lead);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3hat_air(h: *const Hat, k: i64) -> f64 {
    (*h).air(k).unwrap_or(f64::NAN)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3hat_reset(h: *mut Hat) {
    (*h).reset();
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3hat_stats(h: *const Hat, out: *mut Stats) {
    *out = (*h).stats();
}

#[no_mangle]
pub extern "C" fn rdof_t3hat_stats_size() -> usize {
    core::mem::size_of::<Stats>()
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3hat_event(
    h: *mut Hat,
    kind: *mut c_int,
    ts: *mut c_int,
    k: *mut i64,
    m: *mut i64,
    v: *mut f64,
) -> bool {
    match (*h).next_event() {
        Some((a, b, c, d, e)) => {
            *kind = a;
            *ts = b;
            *k = c;
            *m = d;
            core::ptr::copy_nonoverlapping(e.as_ptr(), v, 4);
            true
        }
        None => false,
    }
}

#[no_mangle]
pub extern "C" fn rdof_t3hatsim_size() -> usize {
    core::mem::size_of::<Sim>()
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3hatsim_init(s: *mut Sim, ppm: f64, fetch_s: f64, cap: c_int) {
    s.write(Sim::new(ppm, fetch_s, cap.max(0) as usize));
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3hatsim_input(s: *mut Sim, now: f64, d: *const u8, n: usize) {
    (*s).input(now, core::slice::from_raw_parts(d, n));
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3hatsim_output(s: *mut Sim, out: *mut u8, cap: usize) -> usize {
    (*s).output(core::slice::from_raw_parts_mut(out, cap))
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3hatsim_run(s: *mut Sim, now: f64) {
    (*s).run(now);
}

/// The next slot sent by now: its modem slot, timeslot, air time, burst, whether the host's, the AT and LCSS of the
/// CACH before it, the short LC (9 bytes, as DMR_SHORTLC took it) and which of its rows that CACH carries.
#[no_mangle]
pub unsafe extern "C" fn rdof_t3hatsim_aired(
    s: *mut Sim,
    now: f64,
    m: *mut i64,
    slot: *mut c_int,
    t: *mut f64,
    burst: *mut u8,
    host: *mut bool,
    at: *mut c_int,
    lcss: *mut c_int,
    slc9: *mut u8,
    frag: *mut c_int,
) -> bool {
    let Some(a) = (*s).aired(now) else { return false };
    *m = a.m;
    *slot = a.slot;
    *t = a.t;
    core::ptr::copy_nonoverlapping(a.burst.as_ptr(), burst, BURST);
    *host = a.host;
    *at = a.at;
    *lcss = a.lcss;
    core::ptr::copy_nonoverlapping(a.slc.as_ptr(), slc9, 9);
    *frag = a.frag;
    true
}

#[cfg(test)]
mod tests {
    use super::*;

    fn frame(cmd: u8, p: &[u8]) -> ([u8; 64], usize) {
        let mut f = [0u8; 64];
        f[0] = 0xE0;
        f[1] = (p.len() + 3) as u8;
        f[2] = cmd;
        f[3..3 + p.len()].copy_from_slice(p);
        (f, p.len() + 3)
    }

    fn burst_for(k: i64) -> [u8; BURST] {
        let mut b = [0u8; BURST];
        for (i, x) in b.iter_mut().enumerate() {
            *x = (k as u8).wrapping_mul(31).wrapping_add(i as u8 * 7) ^ ((k >> 8) as u8);
        }
        b
    }

    /// The host and the simulated modem: the receiver hears every slot (position on a 48 kS/s clock that is the
    /// host's, plus a constant receive delay), the writer writes each slot lead before its air time.
    struct Bench {
        h: Hat,
        s: Sim,
        now: f64,
        written: [Option<i64>; 4096], // by modem slot index into the trunk: which k was aired where
        aired_ok: u32,
        aired_at: [[u32; 2]; 2],      // [ts][at]
        fills: u32,
        ts1_at: [u32; 2],
        strict: bool,
    }

    impl Bench {
        fn new(ppm: f64, fetch: f64) -> Bench {
            let mut s = Sim::new(ppm, fetch, 8);
            let (f, n) = frame(0x1D, &[1]);
            s.input(0.0, &f[..n]);
            Bench {
                h: Hat::new(0.025, 0.055),
                s,
                now: 0.0,
                written: [None; 4096],
                aired_ok: 0,
                aired_at: [[0; 2]; 2],
                fills: 0,
                ts1_at: [0; 2],
                strict: true,
            }
        }

        /// one ms of time; at_of: the AT wanted after slot k
        fn step(&mut self, k_min: &mut i64, at_of: &dyn Fn(i64) -> c_int) {
            self.now += 0.001;
            let now = self.now;
            let rx_delay = 0.004;
            self.h.clock(now, ((now - rx_delay) * 48000.0) as i64);
            self.s.run(now);
            while let Some(a) = self.s.aired(now - rx_delay - 0.0001) {
                let pos = (a.t * 48000.0) as i64;
                let lcss = a.lcss;
                let m = self.h.heard(pos, a.slot, a.at, lcss, &a.burst);
                if let Some(m) = m {
                    if self.h.placed() {
                        let k = m + self.h.d();
                        if self.strict && a.lcss == 1 {
                            assert_eq!(k.rem_euclid(4), 0, "the short LC starts on slot {k}");
                        }
                        if a.host && a.burst == [0x40; BURST] {
                            // a burst that held the carrier before the placing
                        } else if a.host {
                            if self.strict {
                                assert_eq!(a.burst, burst_for(k), "slot {k} aired someone else's burst");
                            }
                            self.aired_ok += 1;
                            if a.slot == 2 {
                                // the CACH before this TS2 slot carries the AT after TS1 slot k - 1
                                self.aired_at[1][a.at as usize] += 1;
                                let want = at_of(k - 1);
                                if want >= 0 && self.strict {
                                    assert_eq!(a.at, want, "the AT after TS1 slot {}", k - 1);
                                    self.ts1_at[a.at as usize] += 1;
                                }
                            }
                        } else {
                            self.fills += 1;
                        }
                    }
                }
            }
            // the writer, as the platform's thread runs it
            if !self.h.placed() && self.s.queued(0) + self.s.queued(1) == 0 {
                // not placed yet: a burst at a time keys the modem and holds it
                let (f, n) = frame(0x18, &[0x40; 34]);
                self.s.input(now, &f[..n]);
            }
            if let Some((k, air)) = self.h.next(now, *k_min) {
                let lead = self.h.lead(at_of(k - 2));
                if now >= air - lead {
                    if !self.h.dropping(k) {
                        let b = burst_for(k);
                        let mut p = [0u8; 34];
                        p[0] = 0x40;
                        p[1..].copy_from_slice(&b);
                        let (f, n) = frame(if k & 1 == 0 { 0x18 } else { 0x1A }, &p);
                        self.s.input(now, &f[..n]);
                        self.h.wrote(k, &b, now, lead);
                        if (k as usize) < self.written.len() {
                            self.written[k as usize] = Some(k);
                        }
                    }
                    *k_min = k + 1;
                }
            }
        }
    }

    #[test]
    fn places_and_keeps_every_burst_on_its_slot() {
        let mut b = Bench::new(3.6, 0.010);
        let mut k_min = 100;
        for _ in 0..20000 {
            b.step(&mut k_min, &|_| 0);
        }
        let st = b.h.stats();
        assert!(st.placed);
        assert!(b.aired_ok > 600, "aired {}", b.aired_ok);
        assert_eq!(st.late + st.early, 0, "late {} early {}", st.late, st.early);
        assert!(st.on_time + 10 >= st.written, "on time {} written {}", st.on_time, st.written);
        // all just in time: the AT after each TS1 slot idle
        assert!(b.aired_at[1][1] <= 2, "AT busy {}", b.aired_at[1][1]);
        assert!(st.ppm > 3.4 && st.ppm < 3.8, "ppm {}", st.ppm);
        assert_eq!(b.s.keyups, 1);
    }

    #[test]
    fn a_burst_written_a_slot_ahead_sets_the_at() {
        let mut b = Bench::new(-2.0, 0.012);
        let mut k_min = 0;
        // the AT busy after every 4th TS1 slot (a soliciting PDU), idle otherwise
        let at = |k: i64| if k & 1 == 0 && k % 8 == 4 { 1 } else { 0 };
        for _ in 0..20000 {
            b.step(&mut k_min, &at);
        }
        let st = b.h.stats();
        assert_eq!(st.late + st.early, 0);
        assert!(b.ts1_at[1] > 50 && b.ts1_at[0] > 150, "AT busy {} idle {}", b.ts1_at[1], b.ts1_at[0]);
    }

    #[test]
    fn a_late_write_is_caught_up() {
        let mut b = Bench::new(0.0, 0.010);
        b.strict = false;
        let mut k_min = 0;
        for _ in 0..8000 {
            b.step(&mut k_min, &|_| 0);
        }
        // a stall: the writer misses 70 ms, then writes the slots it can still make; the modem fills the gap
        b.h.set_leads(0.025, 0.055, -1.0); // as a writer that doesn't check how late it is
        let k_stall = k_min;
        for _ in 0..70 {
            b.now += 0.001;
        }
        for _ in 0..200 {
            b.step(&mut k_min, &|_| 0);
        }
        b.h.set_leads(0.025, 0.055, 0.015);
        for _ in 0..8000 {
            b.step(&mut k_min, &|_| 0);
        }
        let st = b.h.stats();
        assert!(st.late >= 2, "late {}", st.late);
        assert!(st.skipped >= 1);
        // caught up: the last second all on time
        let before = st.on_time;
        for _ in 0..1000 {
            b.step(&mut k_min, &|_| 0);
        }
        let st2 = b.h.stats();
        assert_eq!(st2.late, st.late, "still late after the catch-up");
        assert!(st2.on_time > before + 25);
        assert!(k_stall > 0);
    }

    #[test]
    fn inbound_slot_from_its_position() {
        let mut b = Bench::new(1.0, 0.010);
        let mut k_min = 10;
        for _ in 0..3000 {
            b.step(&mut k_min, &|_| 0);
        }
        let k = k_min + 4;
        let air = b.h.air(k).unwrap();
        // an inbound burst under slot k starts 1.25 ms before its outbound burst, 1 ms of timing error either way
        for err in [-0.001, 0.0, 0.001] {
            let pos = ((air - b.h.off + err - 0.00125) * 48000.0) as i64;
            assert_eq!(b.h.inbound_k(pos), Some(k));
        }
    }

    #[test]
    fn sim_latches_the_short_lc_at_its_cycle_start() {
        let mut s = Sim::new(0.0, 0.01, 8);
        let (f, n) = frame(0x1D, &[1]);
        s.input(0.0, &f[..n]);
        let (f, n) = frame(0x18, &[0x40; 34]);
        s.input(0.0, &f[..n]);
        let (f, n) = frame(0x1C, &[7; 9]);
        let mut t = 0.0;
        let mut seen = 0;
        for i in 0..400 {
            t += 0.001;
            if i == 45 {
                s.input(t, &f[..n]);
            }
            if i % 30 == 0 {
                let (g, gn) = frame(0x1A, &[0x40; 34]);
                s.input(t, &g[..gn]);
            }
            s.run(t);
            while let Some(a) = s.aired(t) {
                if a.slc == [7; 9] {
                    if seen == 0 {
                        assert_eq!(a.frag, 0, "a new short LC starts its cycle");
                        assert_eq!(a.lcss, 1);
                    }
                    seen += 1;
                }
            }
        }
        assert!(seen > 4);
        let (f, n) = frame(0x01, &[]);
        s.input(t, &f[..n]);
        let mut out = [0u8; 64];
        let k = s.output(&mut out);
        assert!(k >= 11 && out[2] == 0x01);
    }
}

