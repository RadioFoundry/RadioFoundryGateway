# UTC slot timing

DMR's 30 ms slots laid on UTC, so every site that keeps the grid agrees which slot is which and when it
starts. It's groundwork for multi-site setups: voting receivers line up copies of one burst by slot
number, and simulcast transmitters start each slot together. The default is `free`: each transmitter
keeps its own slot clock, as before.

The code is `common/src/slotgrid.c`, `split.c`, `splitops.c` and `sdr/src/sdrmodem.c`, checked by the
host tests against the golden vectors in `host-tests/vectors.h`.

## The grid

Slot n's boundary is **b(n) = n × 30 ms + offset**, in UTC since the Unix epoch. **Outbound TS1 is even
n.** The offset is the site's `offset_us` (within ±15 000 µs). Simulcast sites use it to even out launch
delays.

POSIX time has no leap seconds. At a leap second every site steps together, so the grid stays common, and
every site loses or doubles one slot at that instant.

| Burst | Where it is |
|---|---|
| outbound (BS) slot n | the CACH starts at b(n), the burst at b(n) + 2.5 ms, its sync center at b(n) + 16.25 ms |
| inbound (MS) slot n | starts at b(n) + 1.25 ms, sync center at b(n) + 15 ms; **TS2 when n is even** (inbound TSx is a slot after outbound TSx) |

Internally n is 64-bit (it's about 5.9 × 10^10 now). At a radio's interface it is a 32-bit counter, n mod
2^32. That keeps the parity, so TS1 is still even. Counters compare by signed 32-bit difference
(`rdof_slot_counter`, `rdof_n_of_counter`).

## The setting

`slot_timing` goes on a box, a repeater, a split's radio entry or an SDR receiver. A radio's own setting
is merged key by key over the box's. It can be `free`, `utc`, or a map:

```yaml
slot_timing:
  mode: utc              # free (default) or utc
  offset_us: 0           # ±15000
  max_err_us: {pps: 20, ntp: 2000}   # or one number for both
  on_loss: holdover      # holdover, free or stop
  holdover_s: 60
```

Validation refuses unknown keys, out-of-range values, and `utc` where the front end keeps its own slot
clock: a single-frequency repeater, or a repeater on an MMDVM hat or board.

## The gate

`rdof_slot_gate_t` decides whether the grid can be used now:

- **aligned:** the clock is synced from a source that has a bound (PPS or NTP), and its error estimate is
  under that bound. The defaults are PPS 20 µs and NTP 2 ms. NTP is enough to label and vote. It is not
  enough for simulcast.
- When that is lost, `on_loss` decides:
  - **holdover:** keep to the grid on the last rate for `holdover_s`, then go free;
  - **free:** free-run at once;
  - **stop:** no transmitting until the clock is back. This is also the state before the clock was ever
    good, and it's what simulcast wants. In stop, the split unkeys its TX half and NAKs DMR data with
    2; the SDR modem unkeys and NAKs too.

The gate's input on the board is netclock: `netclock_quality()` (fit rms + slew left + rate error × time
since the last reference). A Linux build would take the same from the system clock's discipline (for
example chrony's source and error estimate).

The board's netclock PPS input exists, but no real GNSS pulse has checked its bound yet.

Status (the board's `slot_timing` JSON in /api/status, and the page's row) shows:

- the setting, mode, state, source, err_us, holdover_s and why;
- for the SDR: the sample clock's error and windows, the modem's phase against the grid, slips, snaps and
  labeled bursts.

## The sample clock (SDR)

`rdof_sample_clock_t` maps a stream's sample index to UTC. Each chunk of samples is stamped
with its arrival time and the line is fitted through the **earliest** arrivals, because USB and scheduling
jitter only make arrivals late. The line doesn't learn a ppm. When an arrival comes earlier than the line
allows, it re-anchors, and it reports its error (`err_ns`).

On the board the DSP task stamps each chunk on netclock's UTC (`slot_timing.c`). The modem gets the map
and the gate before every pull.

## Transmitting on the grid

### The board's SDR modem (`sdrmodem.c`)

- **Unkeyed** (or at a key-up), the slot clock snaps to the grid. `grid_n0 = n & ~3` keeps the short
  LC's four-slot cycle on n mod 4.
- **Keyed**, it is steered by at most one sample a slot: a sample is slipped or inserted before a slot.
  The air never jumps.
- The carrier is corrected by the crystal's ppm: offset = tune_off_hz − ppm × 10⁻⁶ × tx_hz.
- Heard bursts are labeled from the grid (n and slot). This takes off the receiver's delay: 40 samples at
  48 kS/s, or 48 above that.
- GET_EPOCH's counter comes from the grid.

In the host tests, on a front end running 25 ppm fast with 0–2 ms of arrival jitter, 30 s keyed stays
within 97 µs of the grid (the bound is 120 µs), with steps of at most one sample. A 2 ms bias in the map
converges in about 4 s. The floor is the sample clock's jitter, 20–70 µs.

### A split's TX half (ALIGN)

The host carries the grid into the TX half's clock through the TIME fit (`split.c`'s `align`):

- The target is the next even grid boundary, minus the half's slot-to-air delay (INFO 14–15).
- It is sent with ALIGN every 10 s (every 2 s for the first 30 s), under the same trust rules as the
  epoch (`rdof_epoch_relay_t`, after RadioFoundry #109): the bound must be under 5 ms, and a target that disagrees
  is held back until three agree.
- **Unkeyed,** the half replies 04 (snapping). The host polls STATUS every 30 ms until 46 drops 04:
  - 01 aligned with 20 (counter base): `locked`;
  - only 20: the half landed off the grid, so the host aligns again after the next TIME round.

  A jump over 100 µs makes the RX half's epoch be fetched again.
- **Keyed,** under 1 ms and on parity, only the numbering moves. Otherwise the reply is result 0 with
  flag 10 and the phase. The host notes `held`, and re-aligns at the unkey.
- After a retune the half snaps back to the last grid by itself, and the host aligns again too.

Over USB, the TIME fit is good to 1–2 ms. That is enough for sites to agree on slot numbers, not for
simulcast.

Measured by RadioFoundry: the DM-1801 Pro lands 22–74 µs from the host's grid after the snap. The DM-1701 and the
DM-32UV are not measured, and no on-air TX_SCHED check has run.

### Scheduled TX (TX_SCHED)

In stream mode 1 the TX half takes bursts by slot number instead of a FIFO. Each one is queued, late
(already committed, less than the guard ahead: dropped, never moved), early (past the horizon), replaced,
or full / cool-down. A slot with nothing gets what the stream rules give: idle, silence inside a voice
stream, or a terminator.

Keying is the same as in the FIFO mode, plus two additions:
- a queued burst keys the half up about 150 ms before its slot;
- a key-up that only the queue caused ends 1 s after the last burst.

This is what a remote TX site would use (below). The host side has the encodings (`rdof_ops_schedule`), and the simulated halves and the SDR TX half take it. The repeater doesn't feed it
yet: the split runs its TX half in mode 0.

## The 0xD6 additions (RadioFoundry's layouts)

RadioFoundry owns these wire formats. They are on its feature/split_utc_slots (e83857b2, a21110b3), in its
docs/spec/split-duplex.md 3.1, 3.2, 3.6, 3.8 and 3.9. Every byte is in one place here:
`rdof/splitops.h`. The simulated halves (`host-tests/fake_radio.c`) speak them through it too.

The extension version stays 1. A half has an op only if its capability bit says so.

| Where | Bytes |
|---|---|
| INFO 10–11 | capabilities u16:<br>bit 0 scheduled TX, bit 1 ALIGN;<br>bits 2–5 (slew while keyed, RX metadata, carrier trim, PPS) reserved and read as 0 |
| INFO 12 | the scheduled TX guard, ms: 55 on the HR_C7000. Read it, never assume it. The SDR modem says 40 |
| INFO 13 | the horizon, slots: 32 |
| INFO 14–15 | slot boundary to air, µs s16 (0 = unknown):<br>4600 on HR_C7000 radios, 2200 on the HR_C6000 / DM-1701, 0 on the SDR modem |
| SET_ROLE 6 | the TX stream mode: 0 FIFO, 1 scheduled |
| ALIGN (sub 7) | request: t_us u32, period_ns u32 (0: 30 ms), counter u32 [, max_slew_us u16]<br>reply: phase_us s32, flags (01 aligned, 02 slewing, 04 snapping, 08 period applied (never set by RadioFoundry yet; the SDR modem sets it), 10 not applied)<br>either stream mode; result 2: not the TX half, or no slot clock; result 4: too short, period out of range, or bit 1 clear |
| TX_SCHED (sub 8) | request: counter u32, slot u8, control u8, burst 33<br>reply: counter, outcome (0 queued, 1 late, 2 early, 3 replaced, 4 full or cool-down)<br>result 2: not the TX half, stream 0, or no counter base<br>an idle burst or a repeated terminator clears that slot's place |
| STATUS 32–47 | 32 sent, 34 late, 36 early or full, 38 replaced, 40 cool-down refusals (u16 each)<br>42 the live phase (s32)<br>46 the ALIGN flags now (01 aligned, 04 snapping, 20 counter base); 47 reserved |

**0xD6 never crosses a network.** In RadioFoundry's tests, 9 of 12 calls were mislabeled when the serial link was
carried over IP. Each half sits on its own board's USB or UART. Between sites, only UTC terms travel.

SET_EPOCH, sent from UTC, expires after 120 s on RadioFoundry's side, so the host refreshes it every 10 s.

Not built in RadioFoundry yet, in its order:
1. slewing while keyed, through the HR_C7000's slot-length trim register;
2. RX metadata;
3. carrier trim.

Trim and RX metadata have no ops here yet. The HR_C7000's register 0x428 (`gps_pps_en`) is a future
path: a PPS straight into the radio.

## Which front ends can

| Front end | UTC slots | Why |
|---|---|---|
| the board's SDR (LR2021, SX1255) | yes | its modem places every sample |
| a split TX half with ALIGN (RadioFoundry SPLIT1) | yes, to the TIME fit (1–2 ms over USB) | ALIGN |
| a split TX half without ALIGN | no: `align_state` unsupported, own slot clock | nothing to move its clock with |
| MMDVM_HS hats, MMDVM boards | no (refused) | the firmware owns slot timing, and the MMDVM protocol has no time command |
| a single-frequency repeater (radio hotspot) | no (refused) | the radio times its own slots |
| the LR2021 keyed continuously | not yet | I/Q playback stops at 32 767 samples (0.9 s), and gapless chaining hasn't been tried; until then a long key-up can't stay sample exact |
| RX: the board's MQTT raw bursts | yes: version 2 with `utc_slot`, `utc_us` (the same map the modem labels from) | the sample clock |

## Remote sites (design; not built)

This is how sites fit together on the grid. None of it is built yet, beyond the encodings and the tests
of the single-site parts.

**Roles.**
- A **controller** runs the repeater's logic (`repeater.c`, or HBlink4) and a voter.
- A **tx_site** is a TX-capable front end with its own clock on the grid: an SDR modem, or a RadioFoundry TX
  half in scheduled mode on that site's USB.
- An **rx_site** is any receiver that stamps bursts with UTC n.

Any RX-capable device pairs with any TX-capable one. Nothing assumes one of each, so a controller can
have several of either.

**Transport.** Bursts go over UDP; control and status go over tinysctp (or MQTT).
- A burst is useless once its slot has passed. It must never wait behind a lost one, which is what
  SCTP's ordered, reliable delivery would do.
- Over UDP each datagram stands alone, carrying the site id, a sequence number and n (64-bit). A lost
  burst is just a missing slot, and the radio fills it.
- Duplicates are harmless, since TX_SCHED replaces.
- Control (settings, keying, the epoch, status) wants order and delivery, which tinysctp gives on the
  same link the board already uses for HBlink4.

**Scheduled TX.**
- The controller sends slot n's burst early by lead = network latency (p99) + the half's guard (INFO 12)
  + margin. The default lead would be 200 ms, settable from 100 to 500.
- A late burst is dropped at the site and counted, never moved.
- Empty slots get idle bursts from the site.
- The site unkeys N s after its last burst, or 1 s after the last queued one when the queue keyed it.

**AT / CACH lag.**
- The AT bit in slot n's outbound CACH tells radios whether the next inbound slot is busy, and they use
  it to decide when to transmit.
- With a lead L, the controller decides slot n's CACH L before air, from inbound news that is itself a
  network latency old. At 200 ms that's 7 frames or more.
- During that time a radio sees "idle" for a slot that is in fact busy. Collisions on a busy channel get
  likelier, and channel access for data takes longer.
- **Direct busy:** a tx_site with a co-located receiver sets AT from that receiver (SET_AT to its own TX
  half, which drops a mask older than 2 s). The controller only sets AT where the site has no receiver.

**Inbound and voting.**
- Each rx_site sends every burst with n, the slot, RSSI, BER (from the FEC) and utc_us.
- The voter groups copies by (n, slot) and waits about a slot past the latest expected arrival. It takes
  the copy with the fewest bit errors, then the highest RSSI, and passes it to the repeater as if one
  receiver heard it. The hook is a function per (n, slot): copies in, one out.

**Status per link.** For each link: latency (arrival against utc_us), loss (sequence gaps), late drops
(STATUS 34), the clock's state and error at the site, and the last ALIGN phase.

**Gaps.** None of the following is built:
- the site roles;
- the UDP burst link and its framing (RadioFoundry may define its own; keep it behind an interface);
- the controller feeding TX_SCHED from the repeater;
- the voter;
- per-link status;
- direct busy;
- the simulated controller + TX site + RX site test with network impairments.

The single-site pieces under them are built and tested: the grid, the gate, scheduled TX against the
simulated halves and the SDR modem, and n stamping in the SDR receiver.

## Tests

| What | Where |
|---|---|
| grid, setting, gate, sample clock: golden vectors | `host-tests/vectors.h` → `host-tests/test_main.c` |
| ALIGN host side, snap and STATUS poll, held while keyed, air delay, retune snap-back, gate stop, no caps | `host-tests/test_split.c` |
| TX_SCHED outcomes, counters, idle clears, wrap, refusals, keying, snap-back | the same, through the simulated half directly |
| SDR modem on the UTC grid: keyed steering, map bias, stop / free, RX labeling at two offsets, as a TX half | `host-tests/test_sdrmodem.c` (`test_utc_*`, `test_split_tx_utc`, `test_pair_utc`) |
| netclock_quality | `components/netclock/test` |
| on hardware | not yet: no radio or SDR half has been aligned on a bench by this project |
