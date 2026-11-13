# A Tier III site on a duplex MMDVM modem

A trunked site ([tier3.md](tier3.md)) whose one carrier goes out of a stock duplex MMDVM modem (an MMDVM_HS dual
hat) instead of an SDR, with an SDR receiver (an RTL-SDR) beside it hearing the carrier's out and in. It is the
second site of a two-site lab system, when only one SDR can transmit. The logic is Rust (`rust/src/t3hat.rs`, C API
`rdof/t3hat.h`), the device glue C (`sdr/cli/rdofradio_t3.c`, `rdofradio --tier3`).

The stock firmware keeps its own slot clock and its own CACH, and plays the host's bursts from one queue per
timeslot. The host can't say which slot a burst goes in; it can only choose when to write it. Section 2 is what
the stock modem does, as measured; section 3 the design that places bursts on slots anyway; section 5 what a firmware
change would make exact.

## 1. Setup

- `rdofradio DEVICE --allow-tx --mmdvm PORT --config BOX.json --tier3 SITE.json [--hat-lead IDLE_MS,BUSY_MS]`:
  `--config` is the box's own settings (the modem's offsets and levels, the receiver's channels, HBlink, MQTT, the
  bench), `--tier3` a settings file with the `tier3` app and its one carrier. The site takes the modem; the box's
  own unit on it (a transceiver or repeater) doesn't start. The receiver gets channels on the carrier's out and in
  when `--config` doesn't have them.
- The site file: the carrier's out transmitted by a `modem` device (`mmdvm_hs` or `mmdvm`, not a radio hotspot),
  its in heard by an `sdr` device. One carrier only (the TSCC; TS2 a payload channel, TS1 too while a composite TSCC
  carries a call, tier3.md 3.19). Its MQTT network is its own (the site's node), beside the box's.
- `--mmdvm sim-hat` (no SDR): the stock modem simulated (section 4), for the host tests.
- A bench job that needs the modem pauses the site (nothing written, `DMR_START` off); after it the modem is set up
  again and the site places its slots anew.

## 2. The stock modem, measured

MMDVM_HS_Dual_Hat firmware v1.5.2 on the bench box, 461.2875 MHz, RF level 40, an RTL-SDR at 1.536 MS/s (gain 0,
0.64 ppm set) decoding its out. Bursts were queued by a bench outbound job (a TS2 call from 3 s, 20 superframes, and
a TS1 call from 8 s, 10 superframes), the CACH of each burst read from the receiver.

- **H1 (2026-11-13), keying.** `DMR_START 1` alone keys about 0.2 s (7 of its own idle bursts) and drops. With
  bursts queued it keys and stays keyed; it drops about 180 ms (6 slots) after both queues run empty. Each key-up
  starts its slot grid at a new phase (a -1.94 ms shift seen between two). The first CACH after a key-up is a
  single-fragment short LC (LCSS 0, AT 1, TC 2 twice).
- **H2 (2026-11-13), idle fill.** A timeslot whose queue is empty gets the modem's own idle burst (data type 9),
  with its CACH. The carrier stays up on that as long as the other timeslot has bursts.
- **H3 (2026-11-13), queues.** One FIFO per timeslot, independent. Bursts queued before the key-up built a backlog
  that aired 0.54-0.63 s (TS2) and 0.09-0.15 s (TS1) after the slots they were meant for: the host can't place a
  burst on a slot by its content, only by when it's written.
- **H4 (2026-11-13), the AT.** The AT bit of the CACH whose TC names timeslot x is 1 while the other timeslot's queue
  holds a burst of the host's: set about 4 slots before the first queued burst aired, cleared in the CACH right
  after the last one played. That is this repo's pairing (tier3.md 3.2: the CACH before TS2 carries TS1's inbound
  AT). So a TSCC fed ahead on TS1 shows AT 1 in every random access CACH, and MSs wouldn't send. Written just in
  time (the queue empty when the CACH is built), the AT is 0; a slot earlier, 1.
- **H5 (2026-11-13), the short LC.** A 4-CACH cycle (LCSS 1, 3, 3, 2), its first fragment in the CACH before TS1.
  The modem repeats the last `DMR_SHORTLC` it was given (the job's Activity Update stayed on through both calls; the
  modem makes no Activity Update of its own) and goes to the Null short LC about 60 ms after the last burst, idle
  before it unkeys.
- **H6 (2026-11-13), its clock.** Slot period 46079.86 receiver samples over 9.24 s: -3.0 ppm against the
  receiver; with the receiver's +0.64 ppm setting the modem's clock is about 3.6 ppm fast. Each burst within 0.19 ms
  (8.9 samples rms) of the fitted line.
- **H7 (2026-11-13), RF.** The carrier came out +622 Hz high with the board's tx offset -175 Hz. The receiver
  clipped 3.8% of its samples at gain 0 a few meters away.

Still to measure, each from the site's own 10 s report on the bench (section 6): the window of write leads that put
a burst on its slot with the AT idle (the fetch time), and with it busy; whether the modem unkeys after 6 empty
slots or 6 idle-filled slots (the design keeps both timeslots written, so either holds); how late a `DMR_SHORTLC`
may come and still land on the next cycle; the modem's ppm over hours; `GET_STATUS`'s free space per timeslot.

## 3. The design

### 3.1 The grid

The receiver hears the carrier's out. Each outbound burst gives its position (48 kS/s, the receiver's sample clock)
and, from its CACH, its timeslot (TC), AT and LCSS. Modem slot m starts at `a_p + (m - a_m) * per`: a least squares
line over the last 128 bursts, its slope the slot length on the receiver's clock. A burst more than 4 ms off the
line, or on the wrong parity, three times in a row, or 400 ms with nothing heard, starts a new grid (the modem keyed
up again, H1). The modem's ppm comes from a least squares fit over the whole grid. The receiver's clock maps to the
host's by the earliest arrival: host time = off + position / 48000, off the least `now - now48 / 48000` over 2 s
windows.

### 3.2 Placing the trunk's slots

The trunk's slot k is the UTC slot number, as on the SDR (tier3.md 9.4), until placed; then trunk k is modem slot
`k - d`, d even (the modem's TS1 is LCN 0), chosen once a grid is known so that the first slot written is late enough
to make and `k % 4 == 0` falls where the modem starts its short LC cycle (H5: the LCSS 1 CACH before TS1). From then
on k only goes up: a new grid (a re-key) places again from the next k.

### 3.3 Writing each slot

A writer thread wakes each millisecond. For the next trunk slot k it asks the controller for the burst
(`rdof_t3unit_slot`; an idle burst when it has none, so both queues stay the host's) and writes it at
`air(k) - lead`. The lead is `lead_idle` (25 ms by default) where the controller wants the AT of the CACH after slot
k - 2 idle, `lead_busy` (55 ms) where it wants it busy (H4: a burst queued when the modem builds the CACH before the
other timeslot's slot sets that AT). A slot whose time to air has dropped below `lead_idle - 10 ms` is skipped. At
`k % 4 == 0` the short LC for the next cycle (the controller's C_SYS_Parms, or P_SYS_Parms while a composite TSCC
carries a call) goes out as `DMR_SHORTLC` before the slot's burst, when it has changed: the modem latches it as the
cycle starts (the simulated modem does; to confirm on the bench).

Until the grid is placed, an idle burst on TS1 every 58 ms keys the modem and holds it (a slow backlog of one burst
every 29 slots, taken out by 3.4).

### 3.4 Checking where each burst went

Each burst written waits in a FIFO per timeslot. A burst heard matches the oldest written one within 24 bits of it
written before its air time; the ones before it are lost, and a burst that matches none is the modem's own. For a
match, e = (heard modem slot + d) - k: 0 on its slot. Two in a row a slot late (e = 2, the queue holds one too many)
and the next write on that timeslot is dropped. The write-to-air delays of the bursts on time, the matches, the AT
heard against the controller's, and the ppm go in a report every 10 s.

### 3.5 The inbound

The receiver hears the carrier's in. An inbound burst starting at position p is under trunk slot
`round((p + 60 - a_p) / per) + a_m + d` (an MS's burst starts 1.25 ms before the outbound slot's burst, tier3.md 9.1)
and goes to the controller as the SDR site's does. The modem's own receiver hears it too (the box's tracker, as
before); the site doesn't use it.

### 3.6 Exact, approximate, not done

| | On stock firmware |
|---|---|
| Each burst on its slot | yes, from the receiver's view of the grid (3.2-3.4); a stall of the host over ~20 ms loses or delays slots, and 3.4 catches up within a few slots |
| The AT per slot | yes, by the write lead (H4), if the fetch window measured on the bench leaves room for both leads |
| The short LC per cycle | approximate: the modem latches the last `DMR_SHORTLC` at its own cycle start, so a change written late lands a cycle later |
| The CACH's own fields | the modem's: TC and LCSS can't be set; a key-up's first CACH is its own (H1) |
| The slot grid on UTC | no: the modem's clock runs free (3.6 ppm fast, H6); the trunk's slots follow it, not UTC. A second site on another clock is fine (mqtt-net doesn't need slot sync between sites) |
| Inbound timing | approximate: placed by position (3.5), as the SDR site does |
| The modem's queue depth | not read (`GET_STATUS` isn't polled while the site runs) |

## 4. The simulated modem

`rdof_t3hatsim` is the stock modem as section 2 has it, for the host tests: its own clock with a ppm error; a FIFO
per timeslot taken `fetch_s` (10 ms) before each slot's air time, its idle burst when empty; the AT of each CACH 1
while the other timeslot's queue holds a burst at that moment; `DMR_SHORTLC` latched at the start of each 4-slot
cycle, sent LCSS 1, 3, 3, 2; keyed by a burst after `DMR_START 1`, dropped after 6 idle-filled slots in a row, a new
phase at each key-up; `GET_VERSION`, `GET_STATUS` (version 1, the free space per timeslot) and ACKs. `--mmdvm
sim-hat` runs it on a socket in place of the port, and what it sends goes to the site as heard, each position
dithered by under a sample as a receiver's would be.

## 5. Modem firmware needs

What a change to the modem's firmware would make exact, in order of value. Each is a small addition to its DMR
duplex path; the host side for the first two exists for the split TX half (`rdof/splitops.h`: TX_SCHED, ALIGN, the
UTC slot counter) and would be reused.

1. **Scheduled bursts (TX_SCHED).** `DMR_DATA1/2` with a slot counter: the burst goes in that slot or is dropped
   (with a count of late and early ones in `GET_STATUS`). Removes 3.3's timing and 3.4's catch-up; the host writes
   ahead by any margin.
2. **The slot counter on UTC (ALIGN).** The host gives a boundary time and the counter for it; the modem steers its
   slot clock to it (slews, never jumps while keyed) and reports its phase. Puts the site on the UTC grid, so two
   sites (or a hat and an SDR carrier at one site) share slot timing.
3. **The AT per slot from the host.** A bit with each scheduled burst (or a separate per-slot command) that sets the
   AT of the CACH after it, instead of the modem's queue rule (H4).
4. **The short LC per cycle from the host.** `DMR_SHORTLC` with the slot counter of the cycle it starts (k % 4 == 0),
   applied exactly there; no Null short LC of its own while the host has given one.
5. **Hold the key.** `DMR_START` holding the carrier with its idle fill until `DMR_START 0` (no 6-slot hang), and no
   single-fragment CACH of its own at key-up.
6. **Report what aired.** Optional: an event per slot (counter, host burst or fill) so the host needn't hear its own
   carrier to check placement.

## 6. On the bench

The site's report every 10 s: `N written, N on their slot, N late, N early, N lost, N skipped, N its own; N heard out,
N in; write to air A-B ms; its clock X ppm; AT busy N of M, as the controller's N of M`. Bursts on their slot with
the write-to-air range at the leads, and the AT as the controller's, are the check; `--hat-lead` moves the leads to
find the window (H4).

## 7. Tests

- `cargo test` (`t3` feature), `t3hat::tests`: the engine on the simulated modem 3.6 ppm fast places every burst on
  its slot with the AT idle; written a slot ahead the AT is busy where the controller wants it; a 70 ms stall makes
  bursts late and the catch-up puts them back; an inbound burst's slot from its position; the simulated modem latches
  a short LC at its cycle start.
- `host-tests/e2e_t3_hat.sh` (ctest `rdofradio_e2e_t3_hat`): `rdofradio --mmdvm sim-hat --tier3` with two MSs
  beside the air (`--air-ms`): both register, a group call and an individual call; every burst written on its slot,
  none late or lost, the AT as the controller's, the modem's clock measured.
