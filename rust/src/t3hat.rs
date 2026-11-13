// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! A Tier III carrier on a stock duplex MMDVM modem (docs/spec/fw/tier3-hat.md; C header rdof/t3hat.h).
//!
//! The modem keeps its own slot clock, sends its own CACH and plays the host's bursts from a queue per timeslot,
//! filling a slot whose queue is empty with its own idle burst. Once keyed it stays keyed on that fill as long as the
//! host polls GET_STATUS (its watchdog drops it to IDLE after about 1 s without one). The host can't name the slot a
//! burst goes in: it can only choose when to write it. This places the writes: the receiver (an SDR beside the
//! modem) hears the carrier, its bursts give the modem's slot grid on the receiver's sample clock, and each burst is
//! written a lead before its slot's air time, after the modem has taken the burst before it on that timeslot and
//! before it takes this one (about 106 ms ahead of the air). Written late in that window, its queue is empty when the
//! modem builds the CACH before the other timeslot's slot, so the AT there says idle; written early in it, the queue
//! holds it then and the AT says busy. Each burst heard is matched to the one written for it, which checks where it
//! went; a timeslot that runs behind (its queue holds one too many) is flushed with DMR_ABORT before its next write.
//!
//! `Sim` is the modem as expected (tier3-hat.md section 4), for the host tests.
//!
//! No I/O, no allocation; the platform serializes the calls.

use core::ffi::c_int;

pub const BURST: usize = 33;
const PEND: usize = 16; // bursts written and not yet heard, per timeslot
const FIT: usize = 128; // the bursts heard that the grid's line is fitted to
const EVQ: usize = 512;
const INB: usize = 16; // inbound bursts placed, for the modem's own reports to match
const SLOT48: f64 = 1440.0; // a 30 ms slot at 48 kS/s
const RESTART_GAP48: i64 = 96000; // 2 s with nothing heard: start the grid over (the modem's status says first)
const OUTLIER48: f64 = 96.0; // 2 ms off the line
const MATCH_BITS: u32 = 24; // a heard burst is a written one with at most this many bits different
const INBOUND_OFF48: f64 = 60.0; // an inbound burst starts 1.25 ms before its outbound slot's burst
const INBOUND_END_S: f64 = 0.02625; // and ends 26.25 ms after that burst starts
const FETCH_S: f64 = 0.106; // the modem takes a burst about this long before it airs (expected; the bench measures)
const SLOT_S: f64 = 0.03;
const KEEP_S: f64 = 0.5; // unplaced and not keyed: a write to key it at most this often

// events (rdof_t3hat_event)
pub const EV_WRITE: c_int = 1; // k, ts; v: written at, lead, air time
pub const EV_AIR: c_int = 2; // k, m, ts; v: e (slots late), air time less written at, lead, AT of the CACH before it
pub const EV_FILL: c_int = 3; // m, ts: the modem's own burst (none of ours matched); v: AT before it
pub const EV_LOST: c_int = 4; // k, ts: written, never heard
pub const EV_GRID: c_int = 5; // m: the grid (re)started; v: position, slot length (48 kS/s samples)
pub const EV_PLACE: c_int = 6; // k, m: trunk slot k is modem slot m from now; v: d, the short LC's cycle start (k % 4)
pub const EV_SKIP: c_int = 7; // k, ts: not written (too late for it)
pub const EV_REKEY: c_int = 8; // the modem unkeyed under the site (its status): placed again on its next key-up
pub const EV_ABORT: c_int = 9; // k, ts: DMR_ABORT before slot k's write (its timeslot ran behind); v[0]: why (0 heard, 1 status, 2 NAK)

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
    pub ppm: f64, // the modem's slot clock against the receiver's sample clock (+: slots shorter), 0 unknown
    pub delay_min: f64, // air time less write time of the bursts on time, s
    pub delay_max: f64,
    pub delay_sum: f64,
    // GET_STATUS
    pub polls: u32,          // replies
    pub idle: u32,           // replies with the modem in IDLE (its watchdog fired)
    pub unkeyed: u32,        // replies with the transmitter off while placed (each a re-key: placed again)
    pub overflows: u32,      // replies with an overflow flag (RX or TX bit buffer)
    pub depth_max: [u32; 2], // the most bursts a timeslot's queue held (from its free space)
    pub aborts: u32,         // DMR_ABORT sent
    pub naks: u32,           // writes NAKed (a queue full)
    // the modem's own inbound reports against the receiver's (tier3-hat.md 3.5)
    pub rx_n: u32,
    pub rx_same: u32,      // matched an inbound burst placed by the receiver, tagged with its timeslot
    pub rx_other: u32,     // matched, tagged with the other timeslot
    pub rx_unmatched: u32, // the receiver placed none like it
    pub rx_delay_min: f64, // report time less the inbound burst's end (host s), matched ones
    pub rx_delay_max: f64,
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
fn maxf(a: f64, b: f64) -> f64 {
    if a > b {
        a
    } else {
        b
    }
}

pub fn bit_distance(a: &[u8; BURST], b: &[u8; BURST]) -> u32 {
    a.iter().zip(b.iter()).map(|(x, y)| (x ^ y).count_ones()).sum()
}

pub struct Hat {
    lead: [f64; 2], // by the AT wanted in the CACH before the other timeslot's next slot: 0 idle, 1 busy
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
    lc_phase: i64,  // m % 4 of the slots whose CACH carries LCSS 1 (the modem's own count), -1 unknown
    // the trunk's slots: trunk k is modem slot k - d
    placed: bool,
    d: i64,
    next_k: i64,
    pend: [[Pending; PEND]; 2],
    np: [usize; 2],
    slip: [i64; 2],
    slip_n: [u32; 2],
    abort: [c_int; 2], // a DMR_ABORT due before the timeslot's next write: 0 none, else 1 + why
    // the modem's status
    keyed: i8, // the transmitter, as the last GET_STATUS said: -1 not known
    last_key: f64,
    // inbound bursts placed by the receiver: trunk slot, burst
    inb_k: [i64; INB],
    inb_b: [[u8; BURST]; INB],
    inb_i: usize,
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
            abort: [0; 2],
            keyed: -1,
            last_key: f64::NEG_INFINITY,
            inb_k: [-1; INB],
            inb_b: [[0; BURST]; INB],
            inb_i: 0,
            ev: [Ev::default(); EVQ],
            ev_h: 0,
            ev_n: 0,
            st: Stats {
                delay_min: f64::INFINITY,
                rx_delay_min: f64::INFINITY,
                rx_delay_max: f64::NEG_INFINITY,
                ..Stats::default()
            },
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
            self.abort[ts] = 0;
        }
    }

    /// Start over (a bench job took the modem, or it unkeyed): a new grid and a new placing.
    pub fn reset(&mut self) {
        self.lose_all();
        self.grid = false;
        self.placed = false;
        self.st.placed = false;
        self.nfit = 0;
        self.lc_phase = -1;
        self.keyed = -1;
        self.last_key = f64::NEG_INFINITY;
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
        // one key-up holds for the run: a gap in what's heard is the receiver's, and the grid carries over it
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
            self.restart(pos48, slot); // the exception: the modem keyed again at a new phase
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
        // the modem's short LC count runs on its own (never reset): where its cycle starts is heard, either timeslot
        if lcss == 1 {
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
            // two in a row a slot behind: the queue holds one too many; DMR_ABORT before the next write flushes it
            if e > 0 && self.slip_n[ts] >= 2 && self.abort[ts] == 0 {
                self.abort[ts] = 1;
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

    /// An inbound burst placed under trunk slot k (the receiver's), for the modem's own report of it to match.
    pub fn inbound(&mut self, k: i64, burst: &[u8; BURST]) {
        self.inb_k[self.inb_i] = k;
        self.inb_b[self.inb_i] = *burst;
        self.inb_i = (self.inb_i + 1) % INB;
    }

    /// The modem's own report of an inbound burst (DMR_DATA1/2 from it, slot its tag), at now: matched to one the
    /// receiver placed, a cross-check of the timeslot and a coarse check of the phase. Returns that one's trunk slot.
    pub fn modem_rx(&mut self, now: f64, slot: c_int, burst: &[u8; BURST]) -> Option<i64> {
        self.st.rx_n += 1;
        // the same burst may come again (a call's repeats): the one whose end is nearest now
        let mut hit = None;
        let mut best = f64::INFINITY;
        for i in 0..INB {
            if self.inb_k[i] < 0 || bit_distance(&self.inb_b[i], burst) > MATCH_BITS {
                continue;
            }
            let dt = self.air(self.inb_k[i]).map_or(0.0, |a| absf(now - (a + INBOUND_END_S)));
            if hit.is_none() || dt < best {
                hit = Some(i);
                best = dt;
            }
        }
        let Some(i) = hit else {
            self.st.rx_unmatched += 1;
            return None;
        };
        let k = self.inb_k[i];
        self.inb_k[i] = -1;
        if i64::from(slot) == (k & 1) + 1 {
            self.st.rx_same += 1;
        } else {
            self.st.rx_other += 1;
        }
        if let Some(a) = self.air(k) {
            let delay = now - (a + INBOUND_END_S);
            if delay < self.st.rx_delay_min {
                self.st.rx_delay_min = delay;
            }
            if delay > self.st.rx_delay_max {
                self.st.rx_delay_max = delay;
            }
        }
        Some(k)
    }

    /// A GET_STATUS reply at now: the modem's state (2 DMR, 0 IDLE, -1 not known), its flags (bit 0 the
    /// transmitter on, 2 and 3 an overflow), each timeslot's free space (bursts). A queue deeper than the leads
    /// allow is a backlog: flushed before its next write. The transmitter off while placed is a re-key: placed
    /// again on the grid its next key-up starts.
    pub fn status(&mut self, state: c_int, flags: c_int, space: [c_int; 2]) {
        self.st.polls += 1;
        if state == 0 {
            self.st.idle += 1;
        }
        if flags & 0x0C != 0 {
            self.st.overflows += 1;
        }
        // the queue holds a burst from its write to its fetch: one at most while every lead is under a slot pair
        let lead = maxf(self.lead[0], self.lead[1]);
        let allowed = ceil((lead - FETCH_S) / (2.0 * SLOT_S)).max(1);
        for (ts, sp) in space.iter().enumerate() {
            let depth = (28 - sp).max(0) as u32; // empty reads 28 (its free bytes / 35)
            if depth > self.st.depth_max[ts] {
                self.st.depth_max[ts] = depth;
            }
            if self.placed && i64::from(depth) > allowed && self.abort[ts] == 0 {
                self.abort[ts] = 2;
            }
        }
        let keyed = flags & 1 != 0;
        if self.placed && !keyed && self.keyed == 1 {
            self.st.unkeyed += 1;
            self.reset();
            self.event(EV_REKEY, 0, 0, 0, [0.0; 4]);
        }
        self.keyed = i8::from(keyed);
    }

    /// A write to timeslot slot NAKed (its queue full): flushed before its next write.
    pub fn nak(&mut self, slot: c_int) {
        self.st.naks += 1;
        if (slot == 1 || slot == 2) && self.placed {
            self.abort[(slot - 1) as usize] = 3;
        }
    }

    /// Whether to write an idle burst on TS1 now to key the modem: not placed and not keyed (as its status says),
    /// at most every 0.5 s. Placed, the slots written hold the key.
    pub fn keep(&mut self, now: f64) -> bool {
        if self.placed || self.keyed == 1 || now - self.last_key < KEEP_S {
            return false;
        }
        self.last_key = now;
        true
    }

    /// The next slot to write and its air time; None while the grid isn't known. k_min: the first trunk slot it
    /// may place on (the next after the last the controller gave). Slots too late to make are skipped here.
    pub fn next(&mut self, now: f64, k_min: i64) -> Option<(i64, f64)> {
        // wait for the short LC's cycle to place on it (a modem that sends none: 40 slots)
        if !self.grid || self.nfit < 6 || !self.off_ok || (self.lc_phase < 0 && self.nfit < 40) {
            return None;
        }
        if !self.placed {
            let lead = maxf(self.lead[0], self.lead[1]);
            let want48 = (now + lead + 0.01 - self.off) * 48000.0;
            let mut m = self.a_m + ceil((want48 - self.a_p) / self.per);
            m += m & 1; // TS1
            let mut k0 = k_min;
            k0 += k0 & 1;
            let mut d = k0 - m;
            // the modem's short LC cycle on trunk slots k % 4 == 0 when it starts before TS1, else k % 4 == 1
            if self.lc_phase >= 0 && (self.lc_phase + d).rem_euclid(4) >= 2 {
                d += 2;
            }
            self.d = d;
            self.next_k = m + d;
            self.placed = true;
            self.st.placed = true;
            self.st.d = d;
            let c = if self.lc_phase >= 0 { (self.lc_phase + d).rem_euclid(4) as f64 } else { -1.0 };
            self.event(EV_PLACE, 1, m + d, m, [d as f64, c, 0.0, 0.0]);
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

    /// Whether the CACH before trunk slot k starts the modem's short LC cycle (LCSS 1): DMR_SHORTLC for that cycle
    /// goes with slot k's write, before the modem latches it.
    pub fn lc_start(&self, k: i64) -> bool {
        self.placed && self.lc_phase >= 0 && (k - self.d - self.lc_phase).rem_euclid(4) == 0
    }

    /// Whether DMR_ABORT for slot k's timeslot goes just before its write (that timeslot ran behind): true once.
    pub fn abort(&mut self, k: i64) -> bool {
        let ts = (k & 1) as usize;
        let why = self.abort[ts];
        if why == 0 {
            return false;
        }
        self.abort[ts] = 0;
        self.st.aborts += 1;
        self.event(EV_ABORT, ts as c_int + 1, k, 0, [f64::from(why - 1), 0.0, 0.0, 0.0]);
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
//
// The stock modem as expected (tier3-hat.md section 4; what isn't measured yet is marked so there). Its time is the
// host's: a frame from the host takes effect when its last byte is in (115200 baud); a slot is fetched from its
// timeslot's queue `fetch` before its air time (the TX bit buffer kept full, a few bits' jitter), and the CACH before
// it is built 24 bits before that.

const SIM_FIFO_BYTES: usize = 1000; // each timeslot's queue; a burst takes 33
const SIM_CAP_MAX: usize = SIM_FIFO_BYTES / BURST;
const SIM_AIRQ: usize = 16;
const SIM_RXQ: usize = 8;
const SIM_BYTE_S: f64 = 10.0 / 115200.0;
const SIM_BIT_S: f64 = 1.0 / 9600.0;
const SIM_CACH_LEAD: f64 = 24.0 * SIM_BIT_S; // the CACH before a slot is built this long before its burst's fetch
const SIM_BURST_S: f64 = 264.0 * SIM_BIT_S;
const SIM_TAG_S: f64 = 72.0 * SIM_BIT_S; // the inbound tag moves to the outbound timeslot 9 bytes into its burst
const SIM_RX_LATENCY: f64 = 0.001;
const SIM_WATCHDOG: f64 = 1.0;
const SIM_HOLD: u32 = 20; // slots after a key-up by DMR_START 1: the queues not read, the AT 0
const SIM_JITTER: f64 = 0.0008; // the fetch lead's spread (the bit buffer 1016-1023 bits)
const IDLE_BURST: [u8; BURST] = [0x55; BURST]; // the sim's own fill: not a valid burst, never one of the host's

/// A slot the simulated modem sent.
#[derive(Clone, Copy)]
pub struct SimAir {
    pub m: i64,
    pub slot: c_int,
    pub t: f64,
    pub burst: [u8; BURST],
    pub host: bool, // the host's burst (false: its idle fill)
    pub at: c_int,  // the CACH before it (-1: none, the first slot of a key-up)
    pub lcss: c_int,
    pub frag: c_int, // which of the short LC's 4 rows
    pub slc: [u8; 9],
}

const NO_AIR: SimAir =
    SimAir { m: 0, slot: 0, t: 0.0, burst: [0; BURST], host: false, at: -1, lcss: -1, frag: -1, slc: [0; 9] };

#[derive(Clone, Copy)]
struct SimRx {
    t: f64, // its sync's end
    control: u8,
    burst: [u8; BURST],
}

pub struct Sim {
    ppm: f64,
    fetch: f64,
    cap: usize,
    dmr: bool,     // in DMR mode (else IDLE)
    dmr_t: f64,    // when it went to DMR
    status_t: f64, // the last GET_STATUS
    keyed: bool,
    feeding: bool,  // taking slots from the queues (false: draining what it took, then unkeyed)
    key_t: f64,     // slots due to be fetched before this are fetched at it (a key-up, a resume)
    drain_end: f64, // the end of the last burst it took
    t0: f64,
    m_next: i64,
    m_base: i64,
    jit: f64, // m_next's fetch lead
    cach_built: bool,
    cur_at: c_int,
    cur_lcss: c_int,
    cur_frag: c_int,
    hold: u32,
    q: [[[u8; BURST]; SIM_CAP_MAX]; 2],
    qn: [usize; 2],
    abort: [bool; 2],
    cach_n: u64, // its short LC count: never reset
    slc: [u8; 9],
    slc_cycle: [u8; 9],
    rng: u32,
    uart_t: f64,
    out: [u8; 1024],
    nout: usize,
    air: [SimAir; SIM_AIRQ],
    air_h: usize,
    air_n: usize,
    rx: [SimRx; SIM_RXQ],
    rx_n: usize,
    rd: [u8; 256],
    nrd: usize,
    pub naks: u32,
    pub keyups: u32,
    pub unkeys: u32,
    pub drops: u32,       // the watchdog fired while keyed
    pub idles: u32,       // the watchdog fired
    pub nulls: u32,       // short LC cycles latched Null (both queues empty) while the host writes
    last_write: [f64; 2], // the host's last two writes
}

const ACK: u8 = 0x70;
const NAK: u8 = 0x7F;

impl Sim {
    pub fn new(ppm: f64, fetch: f64, cap: usize) -> Sim {
        Sim {
            ppm,
            fetch,
            cap: if cap == 0 || cap > SIM_CAP_MAX { SIM_CAP_MAX } else { cap },
            dmr: false,
            dmr_t: 0.0,
            status_t: 0.0,
            keyed: false,
            feeding: false,
            key_t: 0.0,
            drain_end: 0.0,
            t0: 0.0,
            m_next: 0,
            m_base: 0,
            jit: fetch,
            cach_built: false,
            cur_at: -1,
            cur_lcss: -1,
            cur_frag: -1,
            hold: 0,
            q: [[[0; BURST]; SIM_CAP_MAX]; 2],
            qn: [0; 2],
            abort: [false; 2],
            cach_n: 0,
            slc: [0; 9],
            slc_cycle: [0; 9],
            rng: 12345,
            uart_t: f64::NEG_INFINITY,
            out: [0; 1024],
            nout: 0,
            air: [NO_AIR; SIM_AIRQ],
            air_h: 0,
            air_n: 0,
            rx: [SimRx { t: 0.0, control: 0, burst: [0; BURST] }; SIM_RXQ],
            rx_n: 0,
            rd: [0; 256],
            nrd: 0,
            naks: 0,
            keyups: 0,
            unkeys: 0,
            drops: 0,
            idles: 0,
            nulls: 0,
            last_write: [f64::NEG_INFINITY; 2],
        }
    }

    /// Its short LC count, so a test can start a key-up at another phase of it.
    pub fn set_lc_count(&mut self, n: u64) {
        self.cach_n = n;
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

    fn slot_len(&self) -> f64 {
        SLOT_S / (1.0 + self.ppm * 1e-6)
    }

    fn slot_t(&self, m: i64) -> f64 {
        self.t0 + m as f64 * self.slot_len()
    }

    fn new_jitter(&mut self) {
        self.rng = self.rng.wrapping_mul(1103515245).wrapping_add(12345);
        let u = f64::from(self.rng >> 8 & 0xffff) / 65536.0;
        self.jit = self.fetch + (u - 0.5) * SIM_JITTER;
    }

    fn key(&mut self, t: f64, hold: u32) {
        self.keyed = true;
        self.feeding = true;
        self.key_t = t;
        self.t0 = t; // TS1 first, no CACH before it (the transmitter's own turn-on delay isn't modeled)
        self.m_base += 1000;
        self.m_next = 0;
        self.cach_built = false;
        self.hold = hold;
        self.keyups += 1;
        self.new_jitter();
    }

    /// Stop taking slots: what it took plays out, then it unkeys.
    fn stop(&mut self) {
        self.feeding = false;
        self.qn = [0; 2];
    }

    fn command(&mut self, cmd: u8, p: &[u8], t: f64) {
        match cmd {
            0x00 => {
                let mut v = [0u8; 24];
                v[0] = 1;
                v[1..21].copy_from_slice(b"MMDVM_HS sim duplex ");
                self.reply(0x00, &v[..21]);
            }
            0x01 => {
                self.status_t = t;
                let flags = u8::from(self.keyed);
                let space = |n: usize| ((SIM_FIFO_BYTES - n * BURST) / 35) as u8;
                let s =
                    [0x02, if self.dmr { 2 } else { 0 }, flags, 0, space(self.qn[0]), space(self.qn[1]), 0, 0, 0, 0];
                self.reply(0x01, &s);
            }
            0x03 => {
                if p.first().copied() == Some(2) {
                    if !self.dmr {
                        self.dmr = true;
                        self.dmr_t = t;
                    }
                } else if self.dmr {
                    self.dmr = false;
                    self.stop();
                }
                self.reply(ACK, &[cmd]);
            }
            0x18 | 0x1A => {
                let ts = if cmd == 0x18 { 0 } else { 1 };
                if !self.dmr {
                    self.dmr = true; // a write in IDLE goes back to DMR
                    self.dmr_t = t;
                }
                if self.abort[ts] {
                    self.abort[ts] = false;
                    self.qn[ts] = 0;
                }
                if p.len() < 1 + BURST || self.qn[ts] >= self.cap {
                    self.naks += 1;
                    self.reply(NAK, &[cmd, 5]);
                    return;
                }
                let n = self.qn[ts];
                self.q[ts][n].copy_from_slice(&p[1..1 + BURST]);
                self.qn[ts] += 1;
                self.last_write = [self.last_write[1], t];
                if !self.keyed {
                    self.key(t, 0);
                } else if !self.feeding {
                    self.feeding = true; // still draining: it takes slots again, on the same grid
                    self.key_t = t;
                }
            }
            0x1C => {
                if p.len() >= 9 {
                    self.slc.copy_from_slice(&p[..9]);
                }
            }
            0x1D => {
                if p.first().copied().unwrap_or(0) != 0 {
                    if self.dmr && !self.keyed {
                        self.key(t, SIM_HOLD);
                    }
                } else if self.keyed {
                    self.stop();
                }
            }
            0x1E => {
                let ts = if p.first().copied().unwrap_or(1) == 2 { 1 } else { 0 };
                self.abort[ts] = true;
            }
            _ => self.reply(ACK, &[cmd]),
        }
    }

    /// Bytes from the host, sent at now.
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
                // in when its last byte is: the serial line one frame after another
                let t = maxf(self.uart_t, now) + n as f64 * SIM_BYTE_S;
                self.uart_t = t;
                self.run(t);
                self.command(cmd, &p[..n - 3], t);
            } else if self.nrd >= 2 && (self.rd[1] < 3) {
                self.nrd = 0;
            }
        }
    }

    /// Bytes to the host (replies, NAKs, what it heard), up to out.len(); returns how many.
    pub fn output(&mut self, out: &mut [u8]) -> usize {
        let n = if self.nout < out.len() { self.nout } else { out.len() };
        out[..n].copy_from_slice(&self.out[..n]);
        self.out.copy_within(n..self.nout, 0);
        self.nout -= n;
        n
    }

    /// An MS's burst on the inbound, its sync ending at t_sync (host s): reported as DMR_DATA1/2 when the burst ends,
    /// tagged with the outbound timeslot it was sending then (TS1 unkeyed).
    pub fn hear(&mut self, t_sync: f64, control: u8, burst: &[u8; BURST]) {
        if self.rx_n < SIM_RXQ {
            self.rx[self.rx_n] = SimRx { t: t_sync, control, burst: *burst };
            self.rx_n += 1;
        }
    }

    fn next_fetch(&self) -> f64 {
        maxf(self.key_t, self.slot_t(self.m_next) - self.jit)
    }

    fn next_step(&self) -> f64 {
        let f = self.next_fetch();
        if self.m_next > 0 && !self.cach_built {
            maxf(self.key_t, f - SIM_CACH_LEAD)
        } else {
            f
        }
    }

    fn step(&mut self, t: f64) {
        let m = self.m_next;
        let ts = (m & 1) as usize;
        if m > 0 && !self.cach_built {
            // the CACH before slot m: TC its timeslot, the AT the other timeslot's queue, the short LC's count
            let pos = (self.cach_n % 4) as usize;
            if pos == 0 {
                if self.qn[0] + self.qn[1] == 0 {
                    self.slc_cycle = [0; 9];
                    if t - self.last_write[0] < 0.1 {
                        self.nulls += 1;
                    }
                } else {
                    self.slc_cycle = self.slc;
                }
            }
            self.cur_at = if self.hold > 0 { 0 } else { c_int::from(self.qn[1 - ts] > 0) };
            self.cur_lcss = [1, 3, 3, 2][pos];
            self.cur_frag = pos as c_int;
            self.cach_n += 1;
            self.cach_built = true;
            return;
        }
        let mut a =
            SimAir { m: self.m_base + m, slot: ts as c_int + 1, t: self.slot_t(m), slc: self.slc_cycle, ..NO_AIR };
        if m > 0 {
            a.at = self.cur_at;
            a.lcss = self.cur_lcss;
            a.frag = self.cur_frag;
        }
        if self.hold > 0 {
            self.hold -= 1;
            a.burst = IDLE_BURST;
        } else if self.qn[ts] > 0 {
            a.burst = self.q[ts][0];
            a.host = true;
            let n = self.qn[ts];
            self.q[ts].copy_within(1..n, 0);
            self.qn[ts] -= 1;
        } else {
            a.burst = IDLE_BURST;
            self.abort[ts] = false; // an idle fill clears a DMR_ABORT, nothing flushed
        }
        self.air[(self.air_h + self.air_n) % SIM_AIRQ] = a;
        self.air_n += 1;
        self.drain_end = a.t + SIM_BURST_S;
        self.m_next += 1;
        self.cach_built = false;
        self.new_jitter();
    }

    /// The modem run to t: its watchdog, each CACH built and each slot taken from its queue (or filled), the key
    /// dropped once it stops taking and what it took has aired, its reports of what it heard.
    pub fn run(&mut self, t: f64) {
        loop {
            let ev = if self.keyed && self.feeding { self.next_step() } else { f64::INFINITY };
            if self.dmr {
                let wd = maxf(self.status_t, self.dmr_t) + SIM_WATCHDOG;
                if wd <= t && wd <= ev {
                    self.dmr = false;
                    self.idles += 1;
                    if self.keyed && self.feeding {
                        self.drops += 1;
                    }
                    self.stop(); // IDLE: it stops taking slots (its queues emptied: assumed)
                    continue;
                }
            }
            if !self.keyed {
                break;
            }
            if !self.feeding {
                if self.drain_end <= t {
                    self.keyed = false;
                    self.unkeys += 1;
                }
                break;
            }
            if ev > t || self.air_n == SIM_AIRQ {
                break;
            }
            self.step(ev);
        }
        // what it heard, out when the burst has ended (108 bits after its sync) and a little latency
        let mut i = 0;
        while i < self.rx_n {
            let r = self.rx[i];
            let t_out = r.t + 108.0 * SIM_BIT_S + SIM_RX_LATENCY;
            if t_out > t {
                i += 1;
                continue;
            }
            let mut slot = 1;
            if self.keyed {
                let m = floor((r.t - SIM_TAG_S - self.t0) / self.slot_len()) as i64;
                if m >= 0 {
                    slot = (m & 1) + 1;
                }
            }
            let mut p = [0u8; 1 + BURST];
            p[0] = r.control;
            p[1..].copy_from_slice(&r.burst);
            self.reply(if slot == 1 { 0x18 } else { 0x1A }, &p);
            self.rx.copy_within(i + 1..self.rx_n, i);
            self.rx_n -= 1;
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
pub unsafe extern "C" fn rdof_t3hat_inbound(h: *mut Hat, k: i64, burst: *const u8) {
    (*h).inbound(k, &*(burst as *const [u8; BURST]));
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3hat_modem_rx(h: *mut Hat, now: f64, slot: c_int, burst: *const u8) -> i64 {
    (*h).modem_rx(now, slot, &*(burst as *const [u8; BURST])).unwrap_or(-1)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3hat_status(h: *mut Hat, state: c_int, flags: c_int, space1: c_int, space2: c_int) {
    (*h).status(state, flags, [space1, space2]);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3hat_nak(h: *mut Hat, slot: c_int) {
    (*h).nak(slot);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3hat_keep(h: *mut Hat, now: f64) -> bool {
    (*h).keep(now)
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
pub unsafe extern "C" fn rdof_t3hat_lc_start(h: *const Hat, k: i64) -> bool {
    (*h).lc_start(k)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3hat_abort(h: *mut Hat, k: i64) -> bool {
    (*h).abort(k)
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

#[no_mangle]
pub unsafe extern "C" fn rdof_t3hatsim_hear(s: *mut Sim, t_sync: f64, control: u8, burst: *const u8) {
    (*s).hear(t_sync, control, &*(burst as *const [u8; BURST]));
}

/// Its counts: key-ups, unkeys, watchdog drops (while keyed), short LC cycles latched Null, writes NAKed.
#[no_mangle]
pub unsafe extern "C" fn rdof_t3hatsim_counts(s: *const Sim, out: *mut u32) {
    let s = &*s;
    let c = [s.keyups, s.unkeys, s.drops, s.nulls, s.naks];
    core::ptr::copy_nonoverlapping(c.as_ptr(), out, c.len());
}

/// The next slot sent by now: its modem slot, timeslot, air time, burst, whether the host's, the AT and LCSS of the
/// CACH before it (-1: none), the short LC (9 bytes, as DMR_SHORTLC took it) and which of its rows that CACH carries.
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

    fn send(s: &mut Sim, t: f64, cmd: u8, p: &[u8]) {
        let (f, n) = frame(cmd, p);
        s.input(t, &f[..n]);
    }

    fn burst_for(k: i64) -> [u8; BURST] {
        let mut b = [0u8; BURST];
        for (i, x) in b.iter_mut().enumerate() {
            *x = (k as u8).wrapping_mul(31).wrapping_add(i as u8 * 7) ^ ((k >> 8) as u8);
        }
        b
    }

    fn data(b: &[u8; BURST]) -> [u8; 34] {
        let mut p = [0u8; 34];
        p[0] = 0x40;
        p[1..].copy_from_slice(b);
        p
    }

    fn slc_for(k: i64) -> [u8; 9] {
        [((k >> 2) as u8) | 0x80; 9]
    }

    const KEY_BURST: [u8; BURST] = [0x40; BURST];
    // a frame's time on the serial line: DMR_DATA 37 bytes
    const DATA_S: f64 = 37.0 * SIM_BYTE_S;

    /// The host and the simulated modem: the receiver hears every slot (position on a 48 kS/s clock that is the
    /// host's, plus a constant receive delay), the writer polls GET_STATUS and writes each slot lead before its air
    /// time, the short LC with the slot whose CACH starts its cycle.
    struct Bench {
        h: Hat,
        s: Sim,
        now: f64,
        aired_ok: u32,
        aired_at: [[u32; 2]; 2], // [ts][at]
        fills: u32,
        ts1_at: [u32; 2],
        slc_ok: u32,
        strict: bool,
        polling: bool,
        next_poll: f64,
        rx_k: [i64; 64],
        rx_n: usize,
    }

    impl Bench {
        fn new(ppm: f64, lc_count: u64) -> Bench {
            let mut s = Sim::new(ppm, 0.106, 0);
            s.set_lc_count(lc_count);
            send(&mut s, 0.0, 0x03, &[2]); // DMR
            Bench {
                h: Hat::new(0.125, 0.155),
                s,
                now: 0.0,
                aired_ok: 0,
                aired_at: [[0; 2]; 2],
                fills: 0,
                ts1_at: [0; 2],
                slc_ok: 0,
                strict: true,
                polling: true,
                next_poll: 0.0,
                rx_k: [0; 64],
                rx_n: 0,
            }
        }

        /// the modem's replies: status, NAKs, what it heard
        fn replies(&mut self) {
            let mut out = [0u8; 1024];
            let n = self.s.output(&mut out);
            let mut i = 0;
            while i + 3 <= n {
                let len = out[i + 1] as usize;
                let (cmd, p) = (out[i + 2], &out[i + 3..i + len]);
                match cmd {
                    0x01 => self.h.status(c_int::from(p[1]), c_int::from(p[2]), [p[4].into(), p[5].into()]),
                    0x7F => self.h.nak(if p[0] == 0x18 { 1 } else { 2 }),
                    0x18 | 0x1A => {
                        let b: [u8; BURST] = p[1..1 + BURST].try_into().unwrap();
                        if let Some(k) = self.h.modem_rx(self.now, if cmd == 0x18 { 1 } else { 2 }, &b) {
                            self.rx_k[self.rx_n % 64] = k;
                            self.rx_n += 1;
                        }
                    }
                    _ => {}
                }
                i += len;
            }
        }

        /// one ms of time; at_of: the AT wanted after slot k
        fn step(&mut self, k_min: &mut i64, at_of: &dyn Fn(i64) -> c_int) {
            self.now += 0.001;
            let now = self.now;
            let rx_delay = 0.004;
            self.h.clock(now, ((now - rx_delay) * 48000.0) as i64);
            self.s.run(now);
            self.replies();
            while let Some(a) = self.s.aired(now - rx_delay - 0.0001) {
                let pos = (a.t * 48000.0) as i64;
                let m = self.h.heard(pos, a.slot, a.at, a.lcss, &a.burst);
                let (Some(m), true) = (m, self.h.placed()) else { continue };
                let k = m + self.h.d();
                if self.strict && a.lcss == 1 {
                    assert!(self.h.lc_start(k), "the short LC's cycle starts before slot {k}");
                    assert!(k.rem_euclid(4) <= 1, "the cycle starts on slot {k}");
                    if a.host {
                        assert_eq!(a.slc, slc_for(k), "the short LC from slot {k}");
                        self.slc_ok += 1;
                    }
                }
                if a.host && a.burst == KEY_BURST {
                    // the burst that keyed the modem before the placing
                } else if a.host {
                    if self.strict {
                        assert_eq!(a.burst, burst_for(k), "slot {k} aired someone else's burst");
                    }
                    self.aired_ok += 1;
                    if a.slot == 2 && a.at >= 0 {
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
            // the writer, as the platform's thread runs it
            if self.polling && now >= self.next_poll {
                send(&mut self.s, now, 0x01, &[]);
                self.next_poll = now + 0.25;
            }
            match self.h.next(now, *k_min) {
                None => {
                    if self.h.keep(now) {
                        send(&mut self.s, now, 0x18, &data(&KEY_BURST));
                    }
                }
                Some((k, air)) => {
                    let lead = self.h.lead(at_of(k - 2));
                    if now >= air - lead {
                        if self.h.abort(k) {
                            send(&mut self.s, now, 0x1E, &[(k & 1) as u8 + 1]);
                        }
                        if self.h.lc_start(k) {
                            send(&mut self.s, now, 0x1C, &slc_for(k));
                        }
                        let b = burst_for(k);
                        send(&mut self.s, now, if k & 1 == 0 { 0x18 } else { 0x1A }, &data(&b));
                        self.h.wrote(k, &b, now, air - now);
                        *k_min = k + 1;
                    }
                }
            }
        }

        fn run(&mut self, ms: u32, k_min: &mut i64, at_of: &dyn Fn(i64) -> c_int) {
            for _ in 0..ms {
                self.step(k_min, at_of);
            }
        }
    }

    #[test]
    fn places_and_keeps_every_burst_on_its_slot() {
        let mut b = Bench::new(3.6, 3); // the short LC's cycle before TS1, as on the bench (H5)
        let mut k_min = 100;
        b.run(20000, &mut k_min, &|_| 0);
        let st = b.h.stats();
        assert!(st.placed);
        assert!(b.aired_ok > 600, "aired {}", b.aired_ok);
        assert_eq!(st.late + st.early, 0, "late {} early {}", st.late, st.early);
        assert!(st.on_time + 10 >= st.written, "on time {} written {}", st.on_time, st.written);
        // all in the idle window: the AT after each TS1 slot idle
        assert!(b.aired_at[1][1] <= 2, "AT busy {}", b.aired_at[1][1]);
        assert!(st.ppm > 3.4 && st.ppm < 3.8, "ppm {}", st.ppm);
        assert!(b.slc_ok > 100, "short LCs {}", b.slc_ok);
        // one key-up, held all the way by the status polls
        assert_eq!((b.s.keyups, b.s.unkeys, b.s.drops, b.s.nulls), (1, 0, 0, 0));
        assert!(st.polls > 70 && st.idle == 0 && st.aborts == 0, "polls {} idle {}", st.polls, st.idle);
        assert!(st.depth_max[0] <= 1 && st.depth_max[1] <= 1);
        assert!(st.delay_min > 0.11 && st.delay_max < 0.14, "{} {}", st.delay_min, st.delay_max);
    }

    #[test]
    fn the_short_lc_cycle_may_start_before_ts2() {
        let mut b = Bench::new(-1.0, 0); // the count at a key-up puts LCSS 1 before TS2
        let mut k_min = 0;
        b.run(8000, &mut k_min, &|_| 0);
        let st = b.h.stats();
        assert_eq!(st.late + st.early, 0);
        assert!(b.slc_ok > 40, "short LCs {}", b.slc_ok);
        assert_eq!(b.s.nulls, 0);
    }

    #[test]
    fn a_burst_written_early_sets_the_at() {
        let mut b = Bench::new(-2.0, 3);
        let mut k_min = 0;
        // the AT busy after every 4th TS1 slot (a soliciting PDU), idle otherwise
        let at = |k: i64| if k & 1 == 0 && k % 8 == 4 { 1 } else { 0 };
        b.run(20000, &mut k_min, &at);
        let st = b.h.stats();
        assert_eq!(st.late + st.early, 0);
        assert!(b.ts1_at[1] > 50 && b.ts1_at[0] > 150, "AT busy {} idle {}", b.ts1_at[1], b.ts1_at[0]);
        assert_eq!((b.s.keyups, b.s.unkeys), (1, 0));
    }

    #[test]
    fn a_late_write_is_caught_up() {
        let mut b = Bench::new(0.0, 3);
        b.strict = false;
        let mut k_min = 0;
        b.run(8000, &mut k_min, &|_| 0);
        // a stall: the writer misses 70 ms, then writes the slots it can still make, some past their fetch
        b.h.set_leads(0.125, 0.155, -1.0); // as a writer that doesn't check how late it is
        for _ in 0..70 {
            b.now += 0.001;
        }
        b.run(200, &mut k_min, &|_| 0);
        b.h.set_leads(0.125, 0.155, 0.115);
        b.run(8000, &mut k_min, &|_| 0);
        let st = b.h.stats();
        assert!(st.late >= 2, "late {}", st.late);
        assert!(st.aborts >= 1, "aborts {}", st.aborts);
        // caught up: the last second all on time, on the same key-up
        let before = st.on_time;
        b.run(1000, &mut k_min, &|_| 0);
        let st2 = b.h.stats();
        assert_eq!(st2.late, st.late, "still late after the catch-up");
        assert!(st2.on_time > before + 25);
        assert_eq!(b.s.keyups, 1);
    }

    #[test]
    fn a_host_stall_past_the_watchdog_places_again() {
        let mut b = Bench::new(2.0, 3);
        b.strict = false;
        let mut k_min = 0;
        b.run(5000, &mut k_min, &|_| 0);
        // 1.5 s with nothing written and no status: the modem goes IDLE, drains, unkeys
        for _ in 0..1500 {
            b.now += 0.001;
            b.s.run(b.now);
            while b.s.aired(b.now).is_some() {}
        }
        assert!(!b.s.keyed() && b.s.drops == 1);
        b.run(8000, &mut k_min, &|_| 0);
        let st = b.h.stats();
        assert!(st.grids >= 2, "grids {}", st.grids);
        assert!(st.placed);
        let before = (st.on_time, st.late, st.early);
        b.run(2000, &mut k_min, &|_| 0);
        let st2 = b.h.stats();
        assert_eq!((st2.late, st2.early), (before.1, before.2), "placed again, all on time");
        assert!(st2.on_time > before.0 + 50);
        assert_eq!(b.s.keyups, 2);
    }

    #[test]
    fn the_modem_tags_an_inbound_burst_with_its_timeslot() {
        let mut b = Bench::new(1.0, 3);
        let mut k_min = 0;
        b.run(3000, &mut k_min, &|_| 0);
        assert!(b.h.placed());
        let k0 = k_min + 8;
        for i in 0..20 {
            let k = k0 + 3 * i;
            let mut x = burst_for(k);
            x[0] ^= 0xA5; // not one of the outbound's
            b.h.inbound(k, &x);
            // the MS's burst starts 1.25 ms before slot k's, its sync ends 156 bits in
            let t_sync = b.h.air(k).unwrap() - 0.004 - 0.00125 + 156.0 * SIM_BIT_S;
            b.s.hear(t_sync, 0x40 | 6, &x);
            b.run(90, &mut k_min, &|_| 0);
        }
        b.run(500, &mut k_min, &|_| 0);
        let st = b.h.stats();
        assert_eq!((st.rx_n, st.rx_same, st.rx_other, st.rx_unmatched), (20, 20, 0, 0));
        // reported 1 ms after the burst ends (and a ms step); the receiver's 4 ms delay puts its end that late
        assert!(st.rx_delay_min > -0.0035 && st.rx_delay_max < -0.001, "{} {}", st.rx_delay_min, st.rx_delay_max);
        assert_eq!(b.rx_k[0], k0);
    }

    #[test]
    fn inbound_slot_from_its_position() {
        let mut b = Bench::new(1.0, 3);
        let mut k_min = 10;
        b.run(3000, &mut k_min, &|_| 0);
        let k = k_min + 4;
        let air = b.h.air(k).unwrap();
        // an inbound burst under slot k starts 1.25 ms before its outbound burst, 1 ms of timing error either way
        for err in [-0.001, 0.0, 0.001] {
            let pos = ((air - b.h.off + err - 0.00125) * 48000.0) as i64;
            assert_eq!(b.h.inbound_k(pos), Some(k));
        }
    }

    // ── the simulated modem alone ──

    /// Runs the sim to t in 1 ms steps, polling GET_STATUS every 250 ms when poll; collects what aired.
    fn sim_to(s: &mut Sim, t0: f64, t1: f64, poll: bool, aired: &mut [SimAir; 512], n: &mut usize) {
        let mut t = t0;
        while t < t1 {
            t += 0.001;
            if poll && ((t * 1000.0).round() as i64) % 250 == 0 {
                send(s, t, 0x01, &[]);
            }
            s.run(t);
            let mut out = [0u8; 1024];
            s.output(&mut out);
            while let Some(a) = s.aired(t) {
                if *n < aired.len() {
                    aired[*n] = a;
                }
                *n += 1;
            }
        }
    }

    #[test]
    fn sim_holds_the_key_while_polled_and_drops_it_on_the_watchdog() {
        let mut s = Sim::new(0.0, 0.106, 0);
        send(&mut s, 0.0, 0x03, &[2]);
        send(&mut s, 0.0, 0x18, &data(&KEY_BURST)); // a write keys it: TS1 first, no hold
        let mut a = [NO_AIR; 512];
        let mut n = 0;
        sim_to(&mut s, 0.0, 3.0, true, &mut a, &mut n);
        assert!(s.keyed());
        assert_eq!((s.keyups, s.unkeys), (1, 0));
        assert!(a[0].host && a[0].slot == 1 && a[0].at < 0, "the written burst first, on TS1, no CACH before it");
        assert!(n > 95 && a[1..n].iter().all(|x| !x.host), "idle filled after: {n}");
        // the last poll at 3.0: IDLE at 4.0, then what it took (about 106 ms) plays out and it unkeys
        let mut t = 3.0;
        while s.keyed() {
            t += 0.001;
            s.run(t);
            while s.aired(t).is_some() {}
        }
        assert!(t > 4.1 && t < 4.16, "unkeyed at {t}");
        assert_eq!((s.drops, s.unkeys), (1, 1));
        // a write keys it again, a new grid, TS1 first
        send(&mut s, 5.0, 0x1A, &data(&burst_for(1)));
        n = 0;
        sim_to(&mut s, 5.0, 5.2, true, &mut a, &mut n);
        assert_eq!(s.keyups, 2);
        assert!(a[0].slot == 1 && !a[0].host && a[1].slot == 2 && a[1].host);
        assert!(a[0].t > 5.0 && a[0].t < 5.01);
    }

    #[test]
    fn sim_dmr_start_holds_twenty_slots() {
        let mut s = Sim::new(0.0, 0.106, 0);
        send(&mut s, 0.0, 0x03, &[2]);
        send(&mut s, 0.0, 0x1D, &[1]);
        let mut a = [NO_AIR; 512];
        let mut n = 0;
        let mut t = 0.0;
        for i in 0..40 {
            // a burst a slot on each timeslot from the start
            send(&mut s, t, if i & 1 == 0 { 0x18 } else { 0x1A }, &data(&burst_for(i)));
            sim_to(&mut s, t, t + 0.03, true, &mut a, &mut n);
            t += 0.03;
        }
        assert!(a[..20].iter().all(|x| !x.host && x.at <= 0), "the hold: idle fill, AT 0");
        assert!(a[20].host && a[20].burst == burst_for(0), "then the backlog plays");
        assert!(a[21].at == 1, "the AT from the queues again");
    }

    #[test]
    fn sim_fetch_and_at_edges() {
        let mut s = Sim::new(0.0, 0.106, 0);
        send(&mut s, 0.0, 0x03, &[2]);
        send(&mut s, 0.0, 0x18, &data(&KEY_BURST));
        let mut a = [NO_AIR; 512];
        let mut n = 0;
        sim_to(&mut s, 0.0, 0.5, true, &mut a, &mut n);
        // TS1 slots m (air T): the burst in the queue (its last byte) at T - lead
        let mut t = 0.5;
        let cases = [(40, 0.100), (48, 0.112), (56, 0.135), (64, 0.142), (72, 0.160), (80, 0.170)];
        for &(m, lead) in &cases {
            let at = s.slot_t(m) - lead - DATA_S;
            sim_to(&mut s, t, at, true, &mut a, &mut n);
            send(&mut s, at, 0x18, &data(&burst_for(m)));
            t = at;
        }
        sim_to(&mut s, t, 3.0, true, &mut a, &mut n);
        let find = |m: i64| a[..n].iter().position(|x| x.m == 1000 + m).unwrap();
        // under 106 ms: misses its fetch, airs on the next TS1 slot
        assert!(!a[find(40)].host && a[find(42)].burst == burst_for(40));
        // 106-138.5 ms: on its slot, the AT before the slot ahead of it idle
        for m in [48, 56] {
            assert!(a[find(m)].burst == burst_for(m) && a[find(m - 1)].at == 0, "slot {m}");
        }
        // 138.5-166 ms: on its slot, that AT busy
        for m in [64, 72] {
            assert!(a[find(m)].burst == burst_for(m) && a[find(m - 1)].at == 1, "slot {m}");
        }
        // over 166 ms: taken a slot pair early
        assert!(a[find(78)].burst == burst_for(80));
    }

    #[test]
    fn sim_latches_the_short_lc_and_never_resets_its_count() {
        let mut s = Sim::new(0.0, 0.106, 0);
        send(&mut s, 0.0, 0x03, &[2]);
        send(&mut s, 0.0, 0x1C, &[7; 9]);
        send(&mut s, 0.0, 0x1A, &data(&burst_for(1))); // keys it: TS1 first, its idle fill
        let mut a = [NO_AIR; 512];
        let mut n = 0;
        sim_to(&mut s, 0.0, 0.6, true, &mut a, &mut n);
        // the first cycle: TS2's burst queued at the latch; the next, both queues empty: Null
        assert!(a[1].lcss == 1 && a[1].slc == [7; 9] && a[1].slot == 2);
        assert!(a[5].lcss == 1 && a[5].slc == [0; 9]);
        // a burst a slot from now on: the short LC latched each cycle
        let mut t = 0.6;
        let m0 = (s.m_next + 8) & !1;
        for m in m0..m0 + 16 {
            let at = s.slot_t(m) - 0.125;
            sim_to(&mut s, t, at, true, &mut a, &mut n);
            send(&mut s, at, if m & 1 == 0 { 0x18 } else { 0x1A }, &data(&burst_for(m)));
            t = at;
        }
        sim_to(&mut s, t, 1.6, true, &mut a, &mut n);
        let lc1 = a[..n].iter().filter(|x| x.lcss == 1 && x.m >= 1000 + m0 + 1 && x.m < 1000 + m0 + 16);
        assert!(lc1.clone().count() >= 3 && lc1.clone().all(|x| x.slc == [7; 9]));
        // unkeyed (DMR_START 0) after an odd number of CACH, keyed again: its cycle starts before the other timeslot
        let count = s.cach_n;
        send(&mut s, t, 0x1D, &[0]);
        sim_to(&mut s, t, 2.0, true, &mut a, &mut n);
        assert!(!s.keyed());
        s.set_lc_count(count | 1); // (the count there as if one more CACH had gone)
        send(&mut s, 2.0, 0x18, &data(&KEY_BURST));
        n = 0;
        sim_to(&mut s, 2.0, 2.3, true, &mut a, &mut n);
        let first = a[..n].iter().find(|x| x.lcss == 1).unwrap();
        assert_eq!(first.slot, if (count | 1) % 4 == 3 { 2 } else { 1 });
    }

    #[test]
    fn sim_queue_is_thirty_bursts_and_status_reads_its_space() {
        let mut s = Sim::new(0.0, 0.106, 0);
        send(&mut s, 0.0, 0x03, &[2]);
        send(&mut s, 0.0, 0x1D, &[1]); // the hold: nothing taken from the queues for 20 slots
        s.output(&mut [0u8; 64]);
        let space = |s: &mut Sim, t: f64| {
            send(s, t, 0x01, &[]);
            let mut out = [0u8; 64];
            let n = s.output(&mut out);
            assert_eq!(n, 13);
            (out[4], out[5], out[7], out[8])
        };
        assert_eq!(space(&mut s, 0.0), (2, 1, 28, 28));
        for i in 0..31 {
            send(&mut s, 0.001, 0x18, &data(&burst_for(i)));
        }
        let mut out = [0u8; 64];
        let n = s.output(&mut out);
        assert_eq!(&out[..n], &[0xE0, 5, 0x7F, 0x18, 5], "the 31st NAKed");
        assert_eq!(s.queued(0), 30);
        assert_eq!(space(&mut s, 0.2), (2, 1, 0, 28));
        // DMR_ABORT: the next write to that timeslot empties its queue first
        send(&mut s, 0.2, 0x1E, &[1]);
        assert_eq!(s.queued(0), 30);
        send(&mut s, 0.2, 0x18, &data(&burst_for(99)));
        assert_eq!(s.queued(0), 1);
        assert_eq!(space(&mut s, 0.21).2, 27);
    }
}
