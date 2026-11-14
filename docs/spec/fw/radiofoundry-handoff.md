# Notes for RadioFoundry's firmware work

A running list of what RadioFoundry's radio firmware needs to know or change for the gateway's features. It holds
only what isn't already in a spec: requests, choices the gateway made, limits of what the gateway supports now, and
facts found while building. Add to it as things come up; mark items done when RadioFoundry has them.

Status (2026-11-13): RadioFoundry's MS engine has run against the site on air (tier3.md, phase 7). Section 2.1 is
the phase 1 scope; the site does more now (tier3.md section 1). Section 2.5 lists the site's changes from the
2026-11-13 audit that an MS sees.

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
- The gateway's side is built (2026-11-13) as one op covering the short LC, AT and host CACH asks above: INFO
  capability bit 0x0040 (`RDOF_OPS_CACH`); TX_SCHED may then carry 3 more octets after the burst, the 24-bit CACH
  sent just before slot n's burst (TACT and payload, air order, the first bit in bit 7 of octet 0). The half sends
  them as given in place of its own AT, TC and short LC fragment. An idle burst with a CACH is sent, not a clear,
  so the gateway schedules every slot of the TSCC's carrier. Without the bit the gateway sends TX_SCHED as before.
  Tested against the host's fake half (`rdof_t3split2_tests`). The radio built the same op (its tier3-radio branch,
  host-tested only, 2026-11-13).
- Before the half is on the UTC grid, the gateway doesn't key it. A TSCC's half that is held keyed off the grid (a
  step in the host's clock) is unkeyed so it can snap back.
- Every slot filled: a TSCC sends a block in every TS1 slot. With TX_SCHED, an empty TS1 slot in a TSCC is a fault;
  the gateway fills every slot, so the TX half's idle fill only covers a late burst. Request: count late bursts per
  key-up (STATUS 34 does) and say so in the reply, as now.

### 1.4a The radio as a trunked site's RX half

A carrier on two radios and no SDR (a split transmitter and a split receiver): the gateway asks the RX half for
RX_META (SET_ROLE byte 7 bit 0) when its INFO offers capability bit 3, and takes each burst's UTC slot from the
event's slot number (bytes 14-15, flag 80) and the TX half's counter at the SET_EPOCH E it sent (GET_EPOCH's counter,
plus one when that slot was TS2). It reads the number as signed: a burst from the slot just before a new E, reported
after the radio took the new SET_EPOCH, comes as -1 (65535), as the radio sends it (counted from the E in force when
the burst is reported, never the old E; the radio's own anchor moves are counted in). Without RX_META the gateway
takes the slot of the label's parity just over when the burst comes, so a report later than one slot after its slot
ends lands two slots off. Tested against the host's fake
halves (`rdof_t3split3_tests`, both ways).

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
- No authentication. No power save (no MS_Sleep handling), no mass re-registration. (The current site has all three
  and Vote Now: tier3.md 3.11, 3.14.)
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
| Grant repetition | each grant twice in consecutive TSCC slots, then every 1.2 s with Late_Entry = 1 through the call and its hang, up to P_CLEAR (6.6.2.4.1, "while a call is active"; the captured site does the same). The MS should count the hang as activity (2.5) |
| Hang (TV_Hangtime) | 3 s |
| Call end | P_CLEAR to ALLMSI, 4 times, channel 0 (back to the TSCC) |
| Individual call timer | 180 s (emergency 300 s) |
| C_AHOY | up to 3 times, 1 s apart, AT set for the answer |
| While a call is pending | C_WACKD to the caller every 2 s |
| Call across sites | the answer may take up to 5 s (setup over the network); the caller gets C_WACKD meanwhile |
| Payload warm-up | 4 CACH of idles before the first sync opportunity (the site isn't slot-synchronized across carriers: Site Timeslot Synchronization 0) |
| Colour code | the TSCC's CC on every payload channel |
| Logical channels | numbered 1-4094 by the site's settings; LCN 0 = TS1, 1 = TS2 |
| Channel plan | C_BCAST Chan_Freq (MBC, absolute Tx / Rx per channel, the MS's view) every 10 s for each carrier and each neighbor's TSCC channel, so an MS can learn it over the air and resolve CH_ADJ / CH_VOTE; a programmed plan works too. A neighbor channel whose number clashes with one of the site's carriers isn't announced; Vote_Now names it with CH_VOTE 4095 and a VN_AP block (tier3.md 3.11) |
| Other broadcasts | Adjacent_Site every 5 s per neighbor (from the network), Local_Time every 6 s when the site's clock is good, Gen_Site_Params every 10 s; Vote_Now per neighbor every `vote_now_s` when set (off by default) |
| Site priorities | Adjacent_Site and Vote_Now Parms2 bits 21-19 = the sending site's `broadcast.priority`, bits 18-16 = the named neighbor's (table 6.81: 1 highest, 7 lowest, 0 non-preferred; default 0 at every site) |
| VOTE_BLK | `broadcast.vote_blk` TDMA frames after a Vote_Now (default 6 = 360 ms), one TSCC slot each, only C_ALOHA ADRNULL Mask 24 |
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
- Adjacent_Site broadcasts list the other trunked sites on the network (from their `info`), active while online,
  with Chan_Freq for each one's TSCC channel and the sites' priorities (tier3.md 3.11).
- Inbound timing: the site gives an inbound burst the slot by its timing against the outbound (aligned timing,
  1.25 ms after the slot's start); a radio that sends late or early by more than half a slot lands in the wrong one.
- The simulator is a second implementation of the MS procedures the site needs: when the radio and the site
  disagree, compare the radio's behavior with it, and check both against the standard.
- First on-air tests: the Pluto on the SDR bench (cabled), on a frequency the owner gives.

### 2.5 Site changes an MS sees (2026-11-13 audit)

- Hang time: the payload channel sends the last item's Terminator with LC in every slot of the hang (361-1 5.2.1.4,
  361-2 5.1.1.3; the captured site does too), not idle bursts. An MS should count any burst with a good sync on its
  slot as call activity (idles too), so it stays to P_CLEAR. Late_Entry grants go out in the hang too: an MS that
  has just left a call may discard Late_Entry = 1 grants for it (6.6.2.4.1), but not a Late_Entry = 0 grant.
- The LC the site makes (late entry, network voice) carries the call's Service Options: Emergency 80h, Broadcast
  08h for all-calls and broadcast calls (361-2 5.3.2.1, 5.3.3.1, table 7.11).
- Talkgroup list registration (6.4.4.1.13.2, table 6.10): all accepted, C_ACKD(Reg_Accepted) with Response_Info
  1111111b; else C_ACKD(65h) with a bit per accepted address (ADDRESS1 the MSB), 0 for ADRNULL and unsent
  positions; a full talkgroup table, 65h with 0000000b.
- A single talkgroup refused (TRGT_ADR_CNTS 01b): C_ACKD(65h) with Response_Info 0000000b even when the MS asked
  for power save; PowerSave_Offset only comes with Reg_Accepted (table 7.41).
- Packet data availability check: the MS answers C_ACKU(Message_Accepted) (6.6.3.2.6; MS_Accepted works too) or
  C_NACKU when its data terminal isn't ready. CallBack is mirrored to the caller; MS_Alerting isn't an answer to a
  data check.

### 2.6 Supplementary services, checked against 361-4 (2026-11-13)

Each value below was settled from the 361-4 text, not from either implementation (tier3.md 3.17, 3.18 have the
choices where the text is open).

- Stun / revive / kill: C_AHOY, Service_Options_Mirror 0, Service_Kind_Flag 0 stun or kill / 1 revive, ALS 0, G/I 0,
  UAB 00, Service_Kind 1101b, target the MS, source STUNI FFFECCh or KILLI FFFECFh (tables 6.19, 6.23); up to 3
  times, 1 s apart, AT set.
- An MS that authenticates the site answers C_ACKVIT (opcode 30): Service_Options_Mirror 0, the C_AHOY's
  Service_Kind_Flag (0 for a kill), reserved 00, UAB 00, Service_Kind 1101b, target its challenge (000000h-FFFCDFh),
  source itself (tables 6.21, 6.25, 7.26). The site then sends C_ACKD: Response_Info G/I 0 + Response_Check, reason
  0x64 (Authentication_Response), target the MS, source the response, AT set (tables 6.20, 6.24). The response is
  6.4.8's algorithm with the MS's K from the site's provisioning (`cfg/ms/{id}` `auth_k`), XORed with its `psn` when
  one is provisioned: leave `psn` unset for an MS that doesn't use one. The site repeats that C_ACKD up to 3 times
  while no final answer comes; a repeated C_ACKVIT gets the response again.
- The final answer: C_ACKU to STUNI / KILLI, source the MS. The site takes any ACK reason, MS_Accepted 0x44 (tables
  6.22, 6.26) or Message_Accepted 0x60 (the figure text). C_NACKU(Recipient_Refused 0x14) after a wrong response;
  C_NACKU(MSNot_Supported 0x00) for a service it doesn't support.
- Kill is always authenticated: the site sends it only for an MS it has a key for. After the final C_ACKU the site
  removes the MS's registration. If the site has no key for an MS that sends C_ACKVIT to a stun, it sends nothing
  more (the MS's own timer ends it).
- DGNA (from the site, MQTT `dgna`): C_DGNAHD (opcode 36), G/I 0, A 1, DPF 0, SAP 0, UDT_Format 0001b, target the
  MS, source DGNAI FFFED6h, UAB the blocks less one, SF 0, then the Address Format blocks (OK 0); the MS answers
  C_ACKU(MS_Accepted), target DGNAI. Up to `sd_tries` times. From an MS (only with `dgna_from_ms`): C_AHOY from
  DGNAI asks for the upload (C_DGNAHU, opcode 37, UAB from the C_RAND's SUPED_VAL); the acknowledgements after the
  upload come from the called MS's ID, not DGNAI (the text doesn't say which: an MS should take either).
- Call diversion: the C_AHOY from DIVERTI asking for the address has UAB 00 (01 only with the C_RAND's Proxy Flag
  set, 6.6.7.1.1.1), whatever SDATA_VAL says. The diversion notice (C_UDTHD, SF 1, A 0) comes from MSI or GPI
  (table 6.65), not DIVERTI as the call procedures say; its block's One Key flag is 0.
- ALS: the called MS gets the radio check C_AHOY (Service_Options_Mirror 0, Service_Kind_Flag 0, ALS 1, Service_Kind
  0000b, source the caller) and answers C_ACKU (0x60 or 0x44) or C_NACKU(MSNot_Supported). Then PV_GRANT, target
  the called MS, source the caller, as an OACSU call: the called MS transmits. Status 123 (end the ALS call) and 125
  (transmit interrupt): see 2.7.
- Packet data payload channel: the site never sends a P_AHOY there (no radio check, selective clear or payload
  authentication), so a P_ACKU (opcode 35) can't be asked for; one sent anyway is ignored. If that check is built,
  the site will take P_ACKU with Message_Accepted or MS_Accepted.

### 2.7 Transmit interrupt, power save, large data (2026-11-13)

- Transmit interrupt (tier3.md 3.20), with `services.transmit_interrupt` on (off: C_NACKD(Not_Supported) from TSI):
  status 125 to the talkgroup (G/I 1) or to the talker or the other party (G/I 0), status 123 from an ALS caller to
  the called MS. Every answer comes from TSI: C_ACKD(Message_Accepted), C_NACKD(Not_Supported) or
  C_NACKD(Perm_User_Refused). The talker gets the RC cease command (0100b) on the other slot of its carrier every
  360 ms; once it stops, the channel sends P_PROTECT(EN_PTT_ONE_MS), source TSI, target the interrupter, in every
  other hang burst for up to 2 s. An MS should obey the cease (finish the superframe, terminator, stop) and keep
  its PTT disabled while P_PROTECTs for another MS come. A talker at another site is de-keyed by its own site the
  same way: an MS sees no difference.
- Power save (6.4.7): PowerSave_Offset comes from the MS's subscribed talkgroup (else its ID), so a talkgroup's
  members with one PowerSave_RQ share a wake frame. A group call to members asleep waits (C_WACKD to the caller,
  up to 2 s) for that frame; else its Late_Entry grant repeat goes out early in each sleeper's frame. A sleeping
  MS needs only to wake in its frame.
- Packet data up to 127 blocks (Blocks to Follow's limit), at each rate; a confirmed message's SACK takes two
  C_RDATA blocks past 64 (each with its own CRC-32). Across sites the message crosses whole.
