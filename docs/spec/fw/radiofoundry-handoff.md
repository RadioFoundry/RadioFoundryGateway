# Notes for RadioFoundry's firmware work

A running list of what RadioFoundry's radio firmware needs to know or change for the gateway's features. It holds
only what isn't already in a spec: requests, choices the gateway made, limits of what the gateway supports now, and
facts found while building. Add to it as things come up; mark items done when RadioFoundry has them.

Status (2026-11-06): nothing sent yet.

## 1. The radio as a split duplex transmitter (TX half)

The gateway drives the TX half through RadioFoundry's split protocol (SET_ROLE, ALIGN, GET_EPOCH, TX_SCHED, SET_AT,
DMR_SHORTLC). Its side is in `common/src/split.c` and `rdof/splitops.h`; the layouts it follows are in
slot-timing.md ("The 0xD6 additions").

### 1.1 Continuous duty (100 %)

The owner modifies radios for continuous transmission (extra heat sinks, fans, mains power). The key limit should be
a choice per radio, not a fixed cap in the firmware.

- Today: SET_ROLE byte 4 (longest key-up, 10 s units) takes 1-60, so 600 s at most; the default is 180 s, then a 20 s
  cool-down (byte 5) in which DMR_DATA is NAKed with 2 and TX_SCHED answers outcome 4.
- Request: byte 4 = 0xFF means no key limit and no cool-down. A capability bit in INFO 10-11 (proposed: bit 6,
  "continuous duty") says the radio accepts it, so the gateway can refuse a setting the radio won't honor instead of
  being clamped silently.
- Keep: the unkey after 2 s of host silence. It's a safety stop, not a duty limit.
- Keep: the power cap (byte 2). With continuous duty the owner sets it on purpose; the default 25 % stays.
- Gateway side: `split.max_key_s` accepts `"none"` (sends 0xFF, needs the bit); numbers stay 0..600.

### 1.2 Slot clock drift on a long key-up

With no key limit the TX half may never unkey. Today it re-aligns to the host's grid only while unkeyed (a keyed
ALIGN reports the phase; 20 ppm drifts 3.6 ms in 180 s). A key-up of hours drifts tens of ms and puts the carrier off
the site's slot grid.

- Request: ALIGN slewing while keyed (capability bit 2) on every radio that offers the TX half, before continuous duty
  is turned on. The gateway re-aligns every few seconds while keyed when bit 2 is set.
- Request: if a radio can't slew, it should refuse 0xFF in byte 4 (1.1), so a never-unkeying carrier can't drift.

### 1.3 Shorter delay

- The FIFO stream's prebuffer (SET_ROLE byte 3, default 3 = 180 ms) is a default, not a floor: 1 (60 ms) is allowed.
  The gateway keeps 3 for FIFO repeaters unless the setting says otherwise.
- The trunked site uses the scheduled stream (TX_SCHED) for its payload carrier, so the prebuffer doesn't apply;
  the delay is the scheduled TX guard (INFO 12: 55 ms on the HR_C7000).
- Request: measure the smallest guard each radio meets reliably and report it in INFO 12; every ms saved shortens the
  repeat delay of every call on that carrier. Say what sets the floor (driver set-up one slot ahead, USB latency).
- Request: in the FIFO stream, a prebuffer in ms or slots below 60 ms isn't needed; no change asked.

### 1.4 What a trunked site needs from the TX half

A trunk site's carrier B (a split radio, payload only) works with the protocol as it is: TX_SCHED for each burst,
DMR_SHORTLC for P_SYS_Parms, SET_AT. A split radio carrying the control channel (TSCC) would need more:

- Short LC per 4-slot cycle, by slot counter. C_SYS_Parms carries the Common_Slot_Counter, which changes every
  120 ms. DMR_SHORTLC applies "from the next 4-CACH boundary", with no tie to the slot counter, so the gateway can't
  say which cycle gets which value. Request: a scheduled short LC (counter of the cycle's first slot + 9 bytes), or a
  TX_SCHED control bit that carries the short LC fragment with the burst.
- AT per slot. The TSCC sets AT = 1 only before the inbound slot where a solicited answer is due, slot by slot. SET_AT
  sets a mask that holds until changed, and lands on air about 1 s late in the split repeater's tests. Request: the AT
  bit per scheduled burst (a TX_SCHED control bit), applied to the CACH before that slot.
- The CACH bits must be the host's. INFO byte 4 = 2 ("the chip makes its own CACH; our AT / short LC unknown") can't
  carry a TSCC. Report which radios send the host's CACH (byte 4 = 1).
- Every slot filled: a TSCC sends a block in every TS1 slot. With TX_SCHED, an empty TS1 slot in a TSCC is a fault;
  the gateway fills every slot, so the TX half's idle fill only covers a late burst. Request: count late bursts per
  key-up (STATUS 34 does) and say so in the reply, as now.

### 1.5 MMDVM protocol (the TX half's standard commands)

- DMR_START 01 / 00, DMR_DATA1/2, DMR_SHORTLC, DMR_ABORT stay as they are.
- Nothing else is asked for MMDVM itself; the additions above are 0xD6 ops.

### 1.6 Facts from the gateway

- The gateway's split repeater lags SET_AT about 1 s behind the inbound activity (seen on the bench): one reason for
  the per-burst AT request above.
- The gateway opens the SDR receive-only (`--split-tx`) when a split radio is the transmitter.

## 2. The radio as a Tier III mobile (MS)

What the gateway's trunked site does now, so RadioFoundry's MS knows what it will meet. The standard (361-4) is the
spec for both; this lists only the gateway's choices and limits. The full design is tier3.md.

### 2.1 What the site supports (phase 1)

- Dedicated TSCC only: carrier A's TS1 is the control channel, all the time. No composite TSCC, no second TSCC, no
  asynchronous (hibernating) TSCC, no C_MOVE. Carrier A's TS2 and both slots of carrier B are payload channels.
- Services:
  - registration and de-registration;
  - group voice (TV_GRANT), all-call (BTV_GRANT for ALLMSID);
  - individual voice with an availability check, OACSU (default) or FOACSU (a site setting);
  - cancel (Service_Kind 1111b);
  - emergency and priority as queue order only (no pre-emption).
- Every other service gets C_NACKD(Not_Supported): data, UDT short data, status, PSTN / PABX / dispatcher gateways,
  duplex calls, ambient listening, stun / revive / kill, DGNA.
- No authentication. No power save (no MS_Sleep handling), no mass re-registration, no Vote Now.
- Talkgroup subscription: one talkgroup in the registration (TRGT_ADR_CNTS 01b). A list subscription (10b, needs a UDT
  upload) is answered "registration accepted, group refused" (reason 0x65); the site then learns the MS's talkgroups
  from its calls.
- Registration: open (any ID) or listed. Refused IDs get Reg_Denied unless the site says Reg_Refused. Reg = 1
  (registration required) by default: a service request from an unregistered ID gets MS_Not_Registered. A call
  request from an unregistered but allowed ID registers it implicitly.

### 2.2 Values the MS will see

| Item | Site default |
|---|---|
| NRand_Wait | 6 (360 ms) |
| Backoff | 3 when quiet, up to 9 under load |
| Service Function | 00 normally; 11 (registration only) while the queue is full |
| Grant repetition | each grant twice in consecutive TSCC slots, then every 1.2 s with Late_Entry = 1 through the call and hang |
| Hang (TV_Hangtime) | 3 s |
| Call end | P_CLEAR to ALLMSI, 4 times, channel 0 (back to the TSCC) |
| Individual call timer | 180 s (emergency 300 s) |
| C_AHOY | up to 3 times, 1 s apart, AT set for the answer |
| While a call is pending | C_WACKD to the caller every 2 s |
| Call across sites | the answer may take up to 5 s (setup over the network); the caller gets C_WACKD meanwhile |
| Payload warm-up | 4 CACH of idles before the first sync opportunity (the site isn't slot-synchronized across carriers: Site Timeslot Synchronization 0) |
| Colour code | the TSCC's CC on every payload channel |
| Logical channels | numbered 1-4094 by the site's settings; LCN 0 = TS1, 1 = TS2 |
| Channel plan | C_BCAST Chan_Freq (MBC, absolute Tx / Rx per channel) for each carrier every 10 s, so an MS can learn it over the air; a programmed plan works too |
| Other broadcasts | Adjacent_Site every 5 s per neighbor (from the network), Local_Time every 6 s when the site's clock is good, Gen_Site_Params every 10 s |
| C_ALOHA | at least one in every 4 TSCC slots |
| Active_Connection | 1 while the site is on the network; 0 alone (local calls still work) |
| Version | 100b (1.11.1) |

### 2.3 What the site expects from the MS

- The MS answers a C_AHOY on the inbound slot after the AT bit.
- At the end of an individual call the initiator sends P_MAINT DISCON; the site clears on that, the hang, or the call
  timer.
- FOACSU: C_ACKU(MS_Alerting), then C_RAND Call Answer (1001b) when the user answers.
- De-registration when the radio is switched off (if the radio does it); otherwise the record expires after 4 h of
  silence.
- An MS that moves to another site of the same network registers there; the site doesn't hand calls over. During a
  call, the MS late-enters from the repeated grant.

### 2.4 Test help on the gateway side

- `rdofradio --sim-air N` runs the site against N simulated MSs (the gateway's own simulator, `rust/src/t3ms.rs`) over
  the full burst path; two of them make a two-site network on one computer. `--sim-script` times registrations and
  calls, so a radio under test can share a network with simulated ones at another site.
- `--sim-ext PATH` puts RadioFoundry's MS engine, built for the host, on that air as an external MS: a unix socket,
  one JSON line each way per slot in lockstep. Out: every carrier's burst (33 bytes), its data type, CC and PDU, and
  the CACH before it (AT, TC, LCSS, the short LC fragment), the AT after the slot. In: the MS's bursts (a PDU, voice
  header / terminator / burst, or raw), each with its carrier, slot, target slot and timing error in µs. Each burst's
  fate comes back (heard, collided, no-slot, timing, loss, corrupted, late, bad) and is logged. Several clients and
  the simulated MSs share the air and collide. Format: `sdr/cli/rdofradio_simext.h`. Not yet: a scripted AT pattern.
- On a device rdofradio runs two carriers: carrier A on the SDR (the TSCC on TS1, a payload channel on TS2) and
  carrier B on a split TX half (both slots payload), the SDR's receiver hearing both inbounds. The first call takes
  carrier A's TS2; a second at the same time goes to carrier B. Carrier B's half counts UTC slots (aligned from the
  host's clock); carrier A's slots follow the SDR's own clock, so the two carriers aren't slot-synchronized
  (`site_sync` 0): a radio moving to carrier B must find its slot timing there (the payload warm-up).
- Adjacent_Site broadcasts list the other trunked sites on the network (from their `info`), active while online.
- Inbound timing: the site gives an inbound burst the slot by its timing against the outbound (aligned timing,
  1.25 ms after the slot's start); a radio that sends late or early by more than half a slot lands in the wrong one.
- The simulator is a second implementation of the MS procedures the site needs: when the radio and the site
  disagree, compare the radio's behavior with it, and check both against the standard.
- First on-air tests: the Pluto on the SDR bench (cabled), on a frequency the owner gives.
