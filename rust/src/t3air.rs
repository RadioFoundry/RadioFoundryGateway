// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! A slot-exact air between a site and N simulated MSs (docs/spec/fw/trunk.md section 11). Each slot k: the site
//! makes its outbound blocks, the MSs transmit (from what they heard before k), the site hears the inbound slot
//! (two PDUs on the TSCC in one slot are a collision: a burst with a bad CRC), then the MSs hear slot k. A
//! solicited answer to a block in slot k goes in slot k + 2 at the earliest, as on air.

use crate::t3ms::{Ms, MsTx, TX_PDU, TX_VOICE};
use crate::t3site::{Event, Site, SlotOut, CHANNELS};

pub const SLOT_MS: i64 = 30;

pub struct Air<'a, const N: usize> {
    pub site: &'a mut Site,
    pub ms: [Ms; N],
    pub k: i64,
    pub t0: i64,
    pub collisions: u32,
    pub last: SlotOut,
    pub inbound: [Option<MsTx>; N],
}

impl<'a, const N: usize> Air<'a, N> {
    pub fn new(site: &'a mut Site, ms: [Ms; N], t0: i64) -> Self {
        Air { site, ms, k: 0, t0, collisions: 0, last: SlotOut::default(), inbound: [None; N] }
    }

    pub fn now(&self) -> i64 {
        self.t0 + self.k * SLOT_MS
    }

    /// One slot; the site's events go to f.
    pub fn step(&mut self, f: &mut impl FnMut(&Event)) {
        let k = self.k;
        let now = self.now();
        let out = self.site.slot(k, now);
        while let Some(e) = self.site.next_event() {
            f(&e);
        }
        for i in 0..N {
            self.inbound[i] = self.ms[i].tx(k, now);
        }
        for ch in 0..CHANNELS {
            let mut pdus = 0;
            let mut first: Option<MsTx> = None;
            for t in self.inbound.iter().flatten().filter(|t| t.ch as usize == ch) {
                if t.kind == TX_PDU {
                    pdus += 1;
                }
                first.get_or_insert(*t);
            }
            let Some(t) = first else { continue };
            if pdus > 1 && ch == self.site.tscc_ch() {
                self.collisions += 1;
                self.site.rx_bad(ch);
            } else if t.kind == TX_PDU {
                self.site.rx_pdu(ch, t.dt, &t.pdu);
            } else if t.kind == TX_VOICE {
                self.site.rx_voice(ch, t.what, t.src);
                for m in self.ms.iter_mut() {
                    m.hear(ch, t.src, t.dst);
                }
            }
        }
        while let Some(e) = self.site.next_event() {
            f(&e);
        }
        let tscc = self.site.tscc_ch();
        for m in self.ms.iter_mut() {
            let ch = m.ch();
            if ch == tscc && k & 1 == 0 {
                m.rx(ch, k, out.tscc_dt, &out.tscc_pdu, out.at != 0);
            } else if ch < CHANNELS && ch as i64 & 1 == k & 1 {
                m.rx(ch, k, out.ch[ch].dt, &out.ch[ch].pdu, false);
            }
        }
        self.last = out;
        self.k += 1;
    }

    /// Runs for ms milliseconds.
    pub fn run(&mut self, ms: i64, f: &mut impl FnMut(&Event)) {
        let end = self.now() + ms;
        while self.now() < end {
            self.step(f);
        }
    }

    /// Runs until cond holds (true) or ms pass (false).
    pub fn until(&mut self, ms: i64, f: &mut impl FnMut(&Event), mut cond: impl FnMut(&Self) -> bool) -> bool {
        let end = self.now() + ms;
        while self.now() < end {
            self.step(f);
            if cond(self) {
                return true;
            }
        }
        false
    }
}
