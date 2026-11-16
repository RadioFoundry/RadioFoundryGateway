# A Tier III site on a duplex MMDVM modem

A trunked site ([tier3.md](tier3.md)) whose one carrier goes out of a stock duplex MMDVM modem (an MMDVM_HS dual
hat) instead of an SDR, with an SDR receiver (an RTL-SDR) beside it hearing the carrier's out and in. It is the
second site of a two-site lab system, when only one SDR can transmit. The logic is Rust (`rust/src/t3hat.rs`, C API
`rdof/t3hat.h`), the device glue C (`sdr/cli/rdofradio_t3.c`, `rdofradio --tier3`).

The stock firmware keeps its own slot clock and its own CACH, and plays the host's bursts from one queue per
timeslot. The host can't say which slot a burst goes in; it can only choose when to write it. Section 2 is what
the stock modem does, as measured and as expected; section 3 the design that places bursts on slots anyway; section 5
what a firmware change would make exact.

## 1. Setup

- `rdofradio DEVICE --allow-tx --mmdvm PORT --config BOX.json --tier3 SITE.json [--hat-lead IDLE_MS,BUSY_MS]`:
  `--config` is the box's own settings (the modem's offsets and levels, the receiver's channels, HBlink, MQTT, the
  bench), `--tier3` a settings file with the `tier3` app and its one carrier. The site takes the modem; the box's
  own unit on it (a transceiver or repeater) doesn't start. The receiver gets channels on the carrier's out and in
  when `--config` doesn't have them. The leads are 132 and 161 ms by default (3.3, H10).
- The site file: the carrier's out transmitted by a `modem` device (`mmdvm_hs` or `mmdvm`, not a radio hotspot),
  its in heard by an `sdr` device. One carrier only (the TSCC; TS2 a payload channel, TS1 too while a composite TSCC
  carries a call, tier3.md 3.19). Its MQTT network is its own (the site's node), beside the box's.
- `--mmdvm sim-hat` (no SDR): the stock modem simulated (section 4), for the host tests.
- The modem is set up duplex with only DMR enabled (`SET_CONFIG`, `SET_FREQ`, `SET_MODE` DMR) and no `DMR_START`:
  the site keys it with its first write. A bench job that needs the modem pauses the site (nothing written, no status
  polls, `DMR_START 0`); after it the modem is set up again and the site places its slots anew.

## 2. The stock modem

### 2.1 Measured

MMDVM_HS_Dual_Hat firmware v1.5.2 on the bench box, 461.2875 MHz, RF level 40, an RTL-SDR at 1.536 MS/s (gain 0,
0.64 ppm set) decoding its out. Bursts were queued by a bench outbound job (a TS2 call from 3 s, 20 superframes, and
a TS1 call from 8 s, 10 superframes), the CACH of each burst read from the receiver. The job keyed with `DMR_START 1`
and didn't poll `GET_STATUS`. Each finding's second paragraph reads it against the expected behavior (2.2).

- **H1 (2026-11-13), keying.** `DMR_START 1` alone keys about 0.2 s (7 of its own idle bursts) and drops. With
  bursts queued it keys and stays keyed; it drops about 180 ms (6 slots) after both queues run empty. Each key-up
  starts its slot grid at a new phase (a -1.94 ms shift seen between two). The first CACH after a key-up is a
  single-fragment short LC (LCSS 0, AT 1, TC 2 twice).

  Read again: the drops are the modem's watchdog (E1), as the job never polled `GET_STATUS`; with polls the modem
  holds its key on its idle fill with nothing queued. The new phase is when the keying command was taken (E2). The
  modem makes no LCSS 0 CACH, so that first CACH is most likely the transmitter's turn-on damaging it.
- **H2 (2026-11-13), idle fill.** A timeslot whose queue is empty gets the modem's own idle burst (data type 9),
  with its CACH. The carrier stays up on that as long as the other timeslot has bursts.
- **H3 (2026-11-13), queues.** One FIFO per timeslot, independent. Bursts queued before the key-up built a backlog
  that aired 0.54-0.63 s (TS2) and 0.09-0.15 s (TS1) after the slots they were meant for: the host can't place a
  burst on a slot by its content, only by when it's written.

  Read again: TS2's 0.54-0.63 s is about the 20-slot hold after `DMR_START 1` (E2: the queues not read for 600 ms),
  TS1's 0.09-0.15 s about the fetch lead (E3). A key-up by a write has no hold.
- **H4 (2026-11-13), the AT.** The AT bit of the CACH whose TC names timeslot x is 1 while the other timeslot's queue
  holds a burst of the host's: set about 4 slots before the first queued burst aired, cleared in the CACH right
  after the last one played. That is this repo's pairing (tier3.md 3.2: the CACH before TS2 carries TS1's inbound
  AT). So a TSCC fed ahead on TS1 shows AT 1 in every random access CACH, and MSs wouldn't send. Written just in
  time (the queue empty when the CACH is built), the AT is 0; a slot earlier, 1.

  Read again: "4 slots before" is the fetch lead (about 106 ms, E3) plus the CACH's 24 bits; E4 gives the windows.
- **H5 (2026-11-13), the short LC.** A 4-CACH cycle (LCSS 1, 3, 3, 2), its first fragment in the CACH before TS1.
  The modem repeats the last `DMR_SHORTLC` it was given (the job's Activity Update stayed on through both calls; the
  modem makes no Activity Update of its own) and goes to the Null short LC about 60 ms after the last burst, idle
  before it unkeys.

  Read again: the cycle count is the modem's own and never reset (E5), so which timeslot its first fragment comes
  before can differ from one key-up to the next. The Null short LC is a cycle latched with both queues empty.
- **H6 (2026-11-13), its clock.** Slot period 46079.86 receiver samples over 9.24 s: -3.0 ppm against the
  receiver; with the receiver's +0.64 ppm setting the modem's clock is about 3.6 ppm fast. Each burst within 0.19 ms
  (8.9 samples rms) of the fitted line.
- **H7 (2026-11-13), RF.** The carrier came out +622 Hz high with the board's tx offset -175 Hz. The receiver
  clipped 3.8% of its samples at gain 0 a few meters away.

  Read again: the slot clock and the RF synthesizer both come from the board's 14.7456 MHz TCXO (E9), so they should
  show the same error. +622 Hz less the -175 Hz offset is +797 Hz, about +1.7 ppm at 461.29 MHz, against H6's
  3.6 ppm: check the receiver's frequency reference and how the offset is applied before trusting either number.

H8-H13 are from the site itself (`--tier3`, this repo's writer polling `GET_STATUS`), same board and receiver, the
TSCC on 461.2875 MHz: lead sweeps with `--hat-sweep` (both leads stepped, no catch-up, both queues flushed at each
step, a 2.5 s window per step) with `--hat-mark`, three runs (185 down to 95 ms by 2, twice; 95 up to 185 by 3). The
leads are host write times against the air time the receiver gives (3.1), so they include the serial line and the
receiver's own latency.

- **H8 (2026-11-13), the fetch and far edges.** Both timeslots on their slot for leads 115-173 ms in all three runs;
  at 113 ms and under, a slot pair late; at 175 ms and over, a slot pair early. The window is 60 ms wide (one slot
  pair), its edges the same within 2 ms in both directions and on both timeslots.

  Read again: E3's fetch is about 110 ms of true lead, a few ms past 106-107 + 3.2; the far edge is the fetch of the
  pair before. The simulated modem's fetch is now 110 ms.
- **H9 (2026-11-13), the AT edge.** The AT heard is 0 for leads up to 145 ms and 1 from 149 ms (147 ms mixed, 6 of
  42), on both timeslots, all three runs. Windows in host lead: AT idle 115-145 ms, AT busy 149-173 ms.
- **H10 (2026-11-13), the short LC.** Written with the cycle's first burst, every cycle aired as written for leads
  119-173 ms; at 115-117 ms and at 175 ms (the window's edges) the cycles were the Null short LC; under 113 ms each
  aired a cycle (120 ms) late. With H9, everything holds for leads 119-145 ms (idle) and 149-173 ms (busy): the
  defaults are now their middles, 132 and 161 ms. With those, over several minutes and four starts: every burst on
  its slot on both timeslots, the AT as the controller's on all, each short LC cycle as written, queues 0-1 deep.
- **H11 (2026-11-13), queue depth.** `GET_STATUS`'s free space reads 28 empty, 27 with one burst queued, 24-25 with a
  backlog of 3-4 (free bytes / 35, E6). In the window the depth is 0 or 1 at each poll, never more.
- **H12 (2026-11-13), the key-up.** On each start the receiver first hears bursts 52-74 ms after the keying write,
  then the grid starts again (a new phase) 88-90 ms later: the modem's first slots after a key-up don't hold their
  phase. The second grid's first slot airs 134-164 ms after the keying write. Bursts written before placing are
  still queued then (TS1 bursts 6 slots late at placing, on all four starts); the placing's `DMR_ABORT` of both queues
  clears them, the timeslot on its slot 8 slots later.
- **H13 (2026-11-13), late and early hold.** A timeslot that aired early (written past the far edge, or after each
  of three starts: about 44 slots a pair early per timeslot, 2.6 s) kept airing early after the lead went back into
  the window, with its queue 0-1 deep, until one slot got the modem's own fill;
  then it was on its slot. Late after a lead under the fetch likewise held for a few seconds with the queue 0-1 deep
  and `DMR_ABORT`s sent. So the modem's fetch-to-air time isn't fixed: it moves by a slot pair and stays there until
  a fill. Leaving one slot of that timeslot unwritten puts it back (one line, "left unwritten ... it aired early";
  back on its slot within a second, once; after four starts with it, no early bursts at all). The simulated modem
  doesn't model this.

  Read again: the "early" TS2 reported on the first deploy wasn't this: the host's idle burst and the modem's own
  fill are the same, and the oldest pending one was matched to the fill before the first TS2 write, putting every
  TS2 match after it one pair early. A match now needs the burst written at least 80 ms before its air time (3.4).

### 2.2 Expected, to confirm on the bench

How the stock modem is expected to behave in duplex DMR, at 9600 b/s (a slot 288 bits, 264 burst and 24 CACH,
30 ms). The design (section 3) and the simulated modem (section 4) follow this; section 2.3 lists what's still to
measure.

- **E1, the watchdog.** Once keyed in duplex DMR the modem runs on, filling empty slots with idle bursts; it stops
  only on `DMR_START 0` or on leaving DMR mode. With no `GET_STATUS` for about 1.0 s it drops to IDLE (a `DMR_DATA`
  write doesn't reset that timer): it stops taking bursts, plays out its TX bit buffer (about 1024 bits, 107 ms) and
  unkeys. A `DMR_DATA` write while IDLE puts it back in DMR.
- **E2, key-up.** Unkeyed, `DMR_START 1` or a `DMR_DATA1/2` write sets it to send TS1 next: the first thing on air
  is a TS1 burst with no CACH before it, its phase set by when the command was taken. `DMR_START 1` also starts a
  20-slot (600 ms) hold: the queues aren't read (idle fill) and the AT is 0. A key-up by a write has no hold.
- **E3, the fetch.** A burst is taken from its timeslot's queue about 105.8-106.6 ms before its first bit reaches the
  modulator (the TX bit buffer kept at 1016-1023 bits), plus the modulator's and transmitter's latency. A burst that
  misses its fetch airs 60 ms later on the same timeslot. On air: TS1 burst, CACH (TC TS2), TS2 burst, CACH (TC
  TS1), and so on; each CACH is built 264 bits after the burst before it was taken, the burst after it 24 bits later.
  A `DMR_DATA` frame (37 bytes) takes about 3.2 ms on the serial line at 115200 baud.
- **E4, the AT.** The AT of each CACH is 1 when the other timeslot's queue holds a burst as the CACH is built (about
  106 ms before it airs), except during E2's hold. For a TSx burst to air at T, by when its queue has it: between
  T - 166 and T - 138.5 ms, the CACH after the TSx burst before it says AT 1; between T - 138.5 and T - 106 ms, AT 0,
  and the burst still makes its fetch. Under T - 106 ms it airs at T + 60 ms.
- **E5, the short LC.** `DMR_SHORTLC` takes 9 bytes (the 68 bits of the Hamming-coded short LC first; the modem
  interleaves). The modem keeps its own 4-CACH count (LCSS 1, 3, 3, 2), never reset, not by `DMR_START` nor a
  key-up. It latches the short LC as it builds the LCSS 1 CACH (about 106 ms before it airs): the last `DMR_SHORTLC`
  it took, or, with both queues empty then, the Null short LC for the whole cycle. A `DMR_SHORTLC` later than that
  lands a cycle (120 ms) late. The burst after that CACH is taken 24 bits after the latch, so a burst written at least
  2.5 ms before its fetch keeps its queue non-empty at the latch.
- **E6, queues and status.** Each timeslot's queue is 1000 bytes, a burst 33 (30 fit; the 31st write is NAKed with
  code 5). A `DMR_DATA` payload is 34 bytes, the first ignored. The v1.5.2 `GET_STATUS` reply is 13 bytes; from the
  frame's 0xE0: byte 4 the state (2 DMR, 0 IDLE: the watchdog fired), byte 5 the flags (bit 0 the transmitter keyed,
  bit 2 RX overflow, bit 3 TX bit buffer overflow), bytes 7 and 8 TS1's and TS2's free space in bursts (free bytes /
  35: 28 empty).
- **E7, `DMR_ABORT`** (payload 1 or 2) sets a flag for the timeslot: its next write empties its queue first. An idle
  fill on that timeslot before then clears the flag without a flush.
- **E8, the inbound.** In duplex the modem's `DMR_DATA1/2` to the host carries only a timeslot tag, no slot number or
  time. While keyed, each received bit is tagged with the outbound timeslot being sent at that moment; the tag moves
  about 9 bytes into each outbound burst, shifted by `SET_CONFIG`'s DMR delay byte. The payload is the control byte
  (the data sync bit and data type, the voice sync flag, or the voice burst's number 1-5), the 33 burst bytes and, in
  some builds, 2 RSSI bytes. A report goes out when the receiver reaches the burst's end, 108 bits (11.25 ms) after
  the end of the sync it found (voice B-F by the timing of the last sync), then a small near-constant receive latency
  and 3.2 ms of serial line. A data sync that fails the colour code check sends nothing; after 13 slots with no sync,
  `DMR_LOST1/2`. Nothing comes while no MS sends; unkeyed, another receiver runs and everything comes as `DMR_DATA1`.
  The modem doesn't hear its own outbound.
- **E9, the clock.** No reference input or calibration: the slot clock and the RF synthesizer both come from the
  board's 14.7456 MHz TCXO.

### 2.3 Still to measure

Each from the site's own reports (section 6) unless said. The fetch, AT and far edges, the short LC latch, the queue
depth and the key-up are in H8-H12.

- **What moves the fetch-to-air time** (H13), and whether anything but a fill moves it back.
- **The watchdog** (E1): its timeout, and whether a write in IDLE restarts it (the simulated modem assumes so), with
  a bench job that stops polling; "N IDLE" and "N unkeyed" in the status line show it under the site.
- **The key-up** (E2): why the first slots after a key-up don't hold their phase (H12), and the transmitter's
  turn-on (the first CACH after a key-up).
- **The inbound tag** (E8): "it heard N inbound: N in the receiver's timeslot, N in the other" and the range "after
  its end" during calls; the share in the other timeslot shows where the tag moves against the receiver's slots.
- **The clock**: "its clock X ppm" over hours, against the RF offset (H7, E9).

## 3. The design

### 3.1 The grid

The receiver hears the carrier's out. Each outbound burst gives its position (48 kS/s, the receiver's sample clock)
and, from its CACH, its timeslot (TC), AT and LCSS. Modem slot m starts at `a_p + (m - a_m) * per`: a least squares
line over the last 128 bursts, its slope the slot length on the receiver's clock. The site keys the modem once and
holds it for the run, so one grid holds too: a gap in what the receiver hears doesn't end it. A new grid is the
exception path: a burst more than 2 ms off the line, or on the wrong parity, three times in a row, 2 s with nothing
heard, or the modem's status saying it unkeyed (3.4). The modem's ppm comes from a least squares fit over the whole
grid. The receiver's clock maps to the host's by the earliest arrival: host time = off + position / 48000, off the
least `now - now48 / 48000` over 2 s windows.

### 3.2 Placing the trunk's slots

The trunk's slot k is the UTC slot number, as on the SDR (tier3.md 9.4), until placed; then trunk k is modem slot
`k - d`, d even (the modem's TS1 is LCN 0), chosen once a grid is known so that the first slot written is late enough
to make. The modem's short LC count is its own (E5), so where its cycle starts is heard: the CACH with LCSS 1, before
TS1 or TS2. d puts that cycle on `k % 4 == 0` when it starts before TS1, on `k % 4 == 1` before TS2. From then on k
only goes up; a new grid places again from the next k.

### 3.3 Writing each slot

A writer thread wakes each millisecond.

- **Status.** It sends `GET_STATUS` every 250 ms while the site runs: that holds the modem in DMR (E1) and gives each
  queue's depth, the state and the transmitter (3.4).
- **Keying.** Until the grid is placed, an idle burst written on TS1 keys the modem (not `DMR_START 1`, whose hold
  would leave the queues unread for 20 slots, E2), again every 0.5 s while its status says it isn't keyed. Keyed, the
  modem holds its carrier on its own idle fill.
- **Each slot.** For the next trunk slot k it asks the controller for the burst (`rdof_t3unit_slot`; an idle burst
  when it has none, so both queues stay the host's and the carrier keyed) and writes it at `air(k) - lead`. The lead
  is `lead_idle` (132 ms) where the controller wants the AT of the CACH after slot k - 2 idle, `lead_busy` (161 ms)
  where it wants it busy: the middles of the windows measured (H8-H10: AT idle and the short LC as written 119-145 ms,
  AT busy 149-173 ms), every write before the fetch. A slot whose time to air has dropped below `lead_idle - 10 ms`
  is skipped.
- **The short LC.** Before the burst of a slot k whose CACH starts the modem's cycle (3.2), the short LC for that
  cycle goes out as `DMR_SHORTLC`, when it has changed (C_SYS_Parms carries the cycle's counter: every cycle; or
  P_SYS_Parms while a composite TSCC carries a call). The value is the controller's cycle from `k & ~3`. Written with
  slot k's burst, it is in before the modem latches it, 2.5 ms before that burst's fetch (E5), and the burst keeps the
  queues non-empty there, so the cycle isn't Null.

### 3.4 Checking where each burst went

Each burst written waits in a FIFO per timeslot. A burst heard matches the written one nearest it in bits (within
24) among those written at least 80 ms before its air time, the oldest of alike ones (a TSCC's repeats; the modem's
own fill is the host's idle burst); the ones before it are lost, and a burst that matches none is the modem's own.
For a match, e = (heard modem slot + d) - k: 0 on its slot. Alike bursts can't show a whole run shifted, so
`--hat-mark` (for measuring) makes each idle burst its own: its last 4 info bytes XORed with a number from k (the
idle message's info is fill that MSs don't read, TS 102 361-1 7.3); the report counts the matches among alike ones.

Placing flushes both queues (`DMR_ABORT` before each timeslot's first write), for what the key-up left (H12). A
timeslot runs behind (its queue holds one too many) when two in a row air a slot late, when its status depth is over
what the leads allow, or when a write to it is NAKed: `DMR_ABORT` for it goes just before its next write, which
empties its queue first (E7); the bursts that flush drops (one per slot pair it ran behind) leave the pending FIFO,
so alike ones heard after aren't matched to them. A timeslot that airs a slot pair early twice in a row gets its next
slot left unwritten, at most once every 2 s: the modem's fill there puts it back (H13). A status that says the
transmitter is off while placed (the watchdog, or a key drop) starts over: the next write keys the modem on a new
grid, placed again. The write-to-air delays of the bursts on time, the matches, the AT heard against the
controller's, the ppm and the status go in a report every 10 s, with a line per timeslot (section 6).

### 3.5 The inbound

The receiver hears the carrier's in. An inbound burst starting at position p is under trunk slot
`round((p + 60 - a_p) / per) + a_m + d` (an MS's burst starts 1.25 ms before the outbound slot's burst, tier3.md 9.1)
and goes to the controller as the SDR site's does. The receiver stays the slot reference. The modem's own reports
(E8) are a cross-check: each is matched to an inbound burst the receiver placed (within 24 bits, the nearest in
time), its tag against the timeslot of that burst's trunk slot, and its time against the burst's end (a coarse
check of the phase during calls). They also still go to the box's tracker, as before.

### 3.6 Exact, approximate, not done

| | On stock firmware |
|---|---|
| Each burst on its slot | yes, from the receiver's view of the grid (3.2-3.4; on the bench H10); a stall of the host over ~10 ms loses or delays slots, and 3.4 catches up within a few slots with one `DMR_ABORT`, or one slot unwritten when it airs early (H13) |
| The AT per slot | yes, by the write lead (E4, H9) |
| The short LC per cycle | yes when written with the cycle's first burst (E5); a write later than the latch lands a cycle late |
| The carrier held | yes, one key-up for the run: `GET_STATUS` every 250 ms and both timeslots written |
| The CACH's own fields | the modem's: TC and LCSS can't be set; its short LC cycle starts where its own count says |
| The slot grid on UTC | no: the modem's clock runs free (3.6 ppm fast, H6); the trunk's slots follow it, not UTC. A second site on another clock is fine (mqtt-net doesn't need slot sync between sites) |
| Inbound timing | approximate: placed by position (3.5), as the SDR site does; the modem's own reports a cross-check |
| The modem's queue depth | from `GET_STATUS` every 250 ms |

## 4. The simulated modem

`rdof_t3hatsim` is the stock modem as section 2.2 has it, for the host tests. Its time is the host's; a frame takes
effect when its last byte is in at 115200 baud.

- Its own clock with a ppm error; keyed by a `DMR_DATA` write or `DMR_START 1` (in DMR mode), TS1 first with no CACH
  before it, the phase at that moment; slots due already are taken at once.
- Each slot taken from its timeslot's queue `fetch_s` (0.110 s as measured, H8, with 0.8 ms of spread) before its air time, its idle
  burst when empty; the CACH before it built 2.5 ms before that: the AT 1 when the other timeslot's queue holds a
  burst, the LCSS from its own never-reset count, the short LC latched at LCSS 1 (Null when both queues are empty).
- The 20-slot hold after `DMR_START 1` only; `DMR_START 0` and IDLE stop the taking, what was taken plays out, then it
  unkeys; a write while still playing out takes slots again on the same grid.
- The watchdog: IDLE 1 s after the later of the last `GET_STATUS` and entering DMR (a write in IDLE enters DMR;
  assumed: E1 doesn't say what the timer counts from then). Leaving DMR empties the queues (assumed).
- Queues of 30 bursts, the 31st NAKed with 5; `GET_STATUS` as E6; `DMR_ABORT` as E7; `GET_VERSION` and ACKs.
- What it hears (the air MSs' inbound, `--air-ms`): reported as `DMR_DATA1/2` 1 ms after the burst ends, tagged with
  the outbound timeslot it was sending 72 bits before the sync's end (TS1 unkeyed).

`--mmdvm sim-hat` runs it on a socket in place of the port, and what it sends goes to the site as heard, each
position dithered by under a sample as a receiver's would be. At the end it prints its key-ups, unkeys, watchdog drops,
Null short LC cycles while written, and NAKs.

## 5. Modem firmware needs

What a change to the modem's firmware would make exact, in order of value. Each is a small addition to its DMR
duplex path; the host side for the scheduled bursts exists for the split TX half (`rdof/splitops.h`: TX_SCHED,
ALIGN, the UTC slot counter) and would be reused. Holding the key isn't among them: polling `GET_STATUS` does it (E1).

1. **Scheduled bursts (TX_SCHED).** `DMR_DATA1/2` with a slot counter: the burst goes in that slot or is dropped
   (with a count of late and early ones in `GET_STATUS`). Removes 3.3's timing and 3.4's catch-up; the host writes
   ahead by any margin.
2. **A per-slot tick, or a report of what aired.** An event per slot (the counter, the host's burst or the fill, the
   AT and LCSS of its CACH), and the counter on each inbound `DMR_DATA` (E8 has only a timeslot tag). The host then
   needs no receiver of its own to find the grid or to place the inbound, and checks placement without hearing its
   own carrier.
3. **The AT per slot from the host.** A bit with each scheduled burst, on 1's counter, that sets the AT of the CACH
   after it, instead of the queue rule (E4).
4. **The short LC per cycle from the host.** `DMR_SHORTLC` with the counter of the cycle it starts (k % 4 == 0), on
   1's counter, applied exactly there, the cycle count tied to that counter; no Null short LC of its own while the host
   has given one.
5. **The slot counter on UTC (ALIGN).** The host gives a boundary time and the counter for it; the modem moves its
   slot clock to it and reports its phase. Puts the site on the UTC grid, so two sites (or a hat and an SDR carrier at
   one site) share slot timing. Last, and coarse: the slot clock comes from integer dividers of the TCXO with no
   trim (E9), so it can only step a whole symbol (208 µs), and at 3.6 ppm it would step once a minute or so.

## 6. On the bench

The site's reports every 10 s, two lines:

- `N written, N on their slot, N late, N early, N lost, N skipped, N its own; N heard out, N in; write to air A-B
  ms; its clock X ppm; AT busy N of M, as the controller's N of M`
- `status N (N IDLE, N unkeyed, N overflow), its queues a / b deep at most; N flushed, N NAKed; it heard N inbound:
  N in the receiver's timeslot, N in the other, N unmatched, A to B ms after its end`

and a third, per timeslot since the last report (each window starts over):

- `TSn aired -4..+4: a b c d e, its own N, write to air A-B ms; AT 0/1 N/M; depth 0..3+ a b c d (space S-T); ...;
  short LC cycles N as written, N a cycle late, N Null, N else; N alike`

"aired" counts the matched bursts two slot pairs or more early, a pair early, on their slot, a pair late, two or more
late; "depth" the status polls by queue depth, with the raw free space; the short LC cycles are joined from the
receiver's CACH rows and checked against the one written for that cycle; "alike" counts matches among alike bursts
(those can't show a run shifted). Lines also say when a timeslot's bursts move off their slot or back (at most 8 a
report) and when a slot is left unwritten (H13).

Bursts on their slot on both timeslots with the write-to-air range at the leads, the AT as the controller's, the
short LC cycles as written, no IDLE or unkeyed, and the queues 1 / 1 deep are the check. `--hat-lead` moves the
leads; `--hat-sweep FROM,TO,STEP,SECONDS[,1]` steps both leads once placed (no catch-up, both queues flushed at each
step unless the 1), a line per step in the form above, then goes back to the leads; `--hat-mark` marks idle bursts.
Both are for measuring, not left on.

## 7. Tests

- `cargo test` (`t3` feature), `t3hat::tests`: the engine on the simulated modem 3.6 ppm fast places every burst on
  its slot with the AT idle, the short LC on each cycle, one key-up held by the status polls; the short LC cycle
  before TS2; written early the AT is busy where the controller wants it; a 70 ms stall makes bursts late and one
  `DMR_ABORT` puts them back (and the bursts it flushed no longer pending, alike ones read on time); placing flushes
  both queues, catch-up on or off; a timeslot airing early gets one slot unwritten at most every 2 s; a lead sweep
  finds the fetch, AT and far edges; a host stall past the watchdog unkeys the modem and the site places again on its next
  key-up; the modem's inbound reports tagged with the trunk's timeslot; an inbound burst's slot from its position.
  The simulated modem alone: the key held while polled and dropped by the watchdog, a write keys it again; the
  20-slot hold after `DMR_START 1`; the fetch and AT edges; the short LC latch, the Null cycle and the count never
  reset; the 30-burst queue, its NAK, its free space and `DMR_ABORT`.
- `host-tests/e2e_t3_hat.sh` (ctest `rdofradio_e2e_t3_hat`): `rdofradio --mmdvm sim-hat --tier3` with two MSs
  beside the air (`--air-ms`): both register, a group call and an individual call; every burst written on its slot,
  none late or lost, the AT as the controller's, the modem's clock measured; one key-up held all run (no unkey, no
  watchdog drop, no Null short LC cycle), the status with no IDLE and queues 1 / 1 deep, the placing's two flushes,
  the per-timeslot line with every burst on its slot and the short LC cycles as written, and the modem's own reports
  of the MSs' inbound all in the trunk's timeslot.
