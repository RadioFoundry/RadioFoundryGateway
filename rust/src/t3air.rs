// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! A slot-exact air between a site and N simulated MSs (docs/spec/fw/tier3.md section 11). Each slot k: the site
//! makes its outbound blocks, the MSs transmit (from what they heard before k), the site hears the inbound slot
//! (two PDUs on the TSCC in one slot are a collision: a burst with a bad CRC), then the MSs hear slot k. A
//! solicited answer to a block in slot k goes in slot k + 2 at the earliest, as on air.
//!
//! Packet data with the network as the far party (tier3.md 5.10) goes through a PdNet, as in the unit: what it
//! takes from the MSs waits in `published` for a test's network; net_data gives it what comes from one.

use crate::t3ms::{Ms, MsTx, TX_DATA, TX_PDU, TX_VOICE};
use crate::t3pdnet::{NetData, PdNet};
use crate::t3pdu::{opcode, DT_CSBK};
use crate::t3site::{block_len, DataRx, Event, Site, SlotOut, BLOCK_MAX, CHANNELS, RC_SEND, REV_CALL};
use crate::t3udt::{DT_DATA_HDR, DT_RATE12};

pub const SLOT_MS: i64 = 30;

pub struct Air<'a, const N: usize> {
    pub site: &'a mut Site,
    pub ms: [Ms; N],
    pub k: i64,
    pub t0: i64,
    pub collisions: u32,
    pub last: SlotOut,
    pub inbound: [Option<MsTx>; N],
    pub data: [DataRx; CHANNELS], // the radio layer's part: what each channel's data is
    /// (opcode, n): the next n TSCC blocks (outbound) or MS PDUs (inbound) with that opcode are lost on the air.
    pub lose_out: (u8, u32),
    pub lose_in: (u8, u32),
    /// Packet data blocks the MSs hear damaged: bit j, the j-th block from now (a header doesn't count).
    pub damage: u32,
    /// The site's relays (tier3.md 5.10) and what they took for the network, oldest first.
    pub net: PdNet,
    pub published: [Option<NetData>; 4],
    out_data: [DataRx; CHANNELS], // the relays' own blocks, counted for the controller
}

/// Whether pdu is one to lose, counting it.
fn lose(l: &mut (u8, u32), pdu: &[u8; 10]) -> bool {
    let hit = l.1 > 0 && opcode(pdu) == l.0;
    if hit {
        l.1 -= 1;
    }
    hit
}

impl<'a, const N: usize> Air<'a, N> {
    pub fn new(site: &'a mut Site, ms: [Ms; N], t0: i64) -> Self {
        Air {
            site,
            ms,
            k: 0,
            t0,
            collisions: 0,
            last: SlotOut::default(),
            inbound: [None; N],
            lose_out: (0, 0),
            lose_in: (0, 0),
            damage: 0,
            data: Default::default(),
            net: PdNet::default(),
            published: [None; 4],
            out_data: Default::default(),
        }
    }

    /// A message from the network (its stream from): held for a channel the controller is asked for; false when
    /// it can't go (not here, busy, nothing free).
    pub fn net_data(&mut self, nd: &NetData, from: u32) -> bool {
        let now = self.now();
        if !self.net.hold(nd, from, now) {
            return false;
        }
        let ok = self.site.pd_net_in(nd.head.src, nd.head.dst, nd.head.group, now);
        if !ok {
            self.net.drop_held(from);
        }
        ok // its events go with the next slot's
    }

    /// The next message the relays took, for the network.
    pub fn take_published(&mut self) -> Option<NetData> {
        let i = self.published.iter().position(|p| p.is_some())?;
        let p = self.published[i].take();
        self.published[i..].rotate_left(1);
        p
    }

    fn publish(&mut self, nd: NetData) {
        if let Some(s) = self.published.iter_mut().find(|s| s.is_none()) {
            *s = Some(nd);
        }
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
            self.net.event(&e, now);
            f(&e);
        }
        self.net.tick(now);
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
            } else if t.kind == TX_PDU && t.dt == DT_RATE12 {
                let mut b = [0u8; 12];
                b[..10].copy_from_slice(&t.pdu);
                b[10..].copy_from_slice(&t.ext[..2]);
                self.site.rx_block(ch, t.dt, &b);
            } else if t.kind == TX_PDU {
                if !lose(&mut self.lose_in, &t.pdu) {
                    self.site.rx_pdu(ch, t.dt, &t.pdu);
                }
            } else if t.kind == TX_DATA {
                // the radio layer counts the message for the controller and repeats it on the outbound
                let mut b = [0u8; BLOCK_MAX];
                b[..10].copy_from_slice(&t.pdu);
                b[10..].copy_from_slice(&t.ext);
                let what = if t.dt == DT_DATA_HDR {
                    self.data[ch].header(&t.pdu)
                } else {
                    if self.damage & 1 != 0 {
                        b[block_len(t.dt).max(12) - 3] ^= 0x24; // past its FEC: its CRCs fail
                    }
                    self.damage >>= 1;
                    self.data[ch].block()
                };
                self.site.rx_data(ch, what, t.src);
                for m in self.ms.iter_mut() {
                    m.rx_data(ch, t.dt, &b);
                }
                let mut blk = [0u8; BLOCK_MAX];
                blk.copy_from_slice(&b);
                if let Some(nd) = self.net.rx(ch, t.dt, &blk, now) {
                    self.publish(nd);
                }
            } else if t.kind == TX_VOICE {
                self.site.rx_voice(ch, t.what, t.src);
                let out_ch = self.site.repeat_channel(ch); // a duplex call: the other party's slot
                for m in self.ms.iter_mut() {
                    m.hear(out_ch, t.src, t.dst);
                }
            }
        }
        // the relays' blocks, one a channel in its slot
        for ch in (0..CHANNELS).filter(|&c| c as i64 & 1 == k & 1) {
            let Some((dt, b)) = self.net.next(ch, now) else { continue };
            let what = if dt == DT_DATA_HDR {
                let mut p = [0u8; 10];
                p.copy_from_slice(&b[..10]);
                self.out_data[ch].header(&p)
            } else {
                self.out_data[ch].block()
            };
            let src = self.out_data[ch].src;
            self.site.rx_data(ch, what, src);
            let mut b = b;
            if dt != DT_DATA_HDR {
                if self.damage & 1 != 0 {
                    b[block_len(dt).max(12) - 3] ^= 0x24;
                }
                self.damage >>= 1;
            }
            for m in self.ms.iter_mut() {
                m.rx_data(ch, dt, &b);
            }
        }
        while let Some(e) = self.site.next_event() {
            self.net.event(&e, now);
            f(&e);
        }
        let tscc = self.site.tscc_ch();
        let lost = k & 1 == 0 && out.tscc_dt == DT_CSBK && lose(&mut self.lose_out, &out.tscc_pdu);
        for m in self.ms.iter_mut() {
            if out.has_slc != 0 {
                m.slc(tscc / 2, out.slc_tscc); // the TSCC carrier's CACH
            }
            let ch = m.ch();
            if ch == tscc && k & 1 == 0 && out.rev != REV_CALL {
                let mut b = [0u8; BLOCK_MAX];
                b[..10].copy_from_slice(&out.tscc_pdu);
                b[10..12].copy_from_slice(&out.tscc_ext);
                m.rx_block(ch, k, if lost { 0 } else { out.tscc_dt }, &b, out.at != 0);
            } else if ch < CHANNELS && ch as i64 & 1 == k & 1 {
                m.rx(ch, k, out.ch[ch].dt, &out.ch[ch].pdu, false);
            }
            // a Reverse Channel PDU for it, in its carrier's other outbound slot
            if ch < CHANNELS && (ch ^ 1) as i64 & 1 == k & 1 && out.ch[ch ^ 1].rc & RC_SEND != 0 {
                m.rc(ch, out.ch[ch ^ 1].rc & 0x0F);
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
