# Trunked site (DMR Tier III): design and plan

Covers: what the `tier3` app is, how the site controller, the radio layer, the MQTT network and the network
services split the work, the protocol subset, the settings, the network messages, failure and isolation behavior,
the road to multi-site roaming, what is built in this round and what comes later, and how it is tested.

Status (2026-11-12): round 1 built and run on simulated air only (two sites over a broker, external MSs through
`--sim-ext`); never on air. Round 2 under way (section 13): the site in Rust only, then the bench. Branch
`tier3-trunk`, on main as of 2026-11-12.

Sources: ETSI TS 102 361-1 (air interface), -2 (voice), -4 (trunking, V1.12.1). Clause numbers below are 361-4's
unless marked. Clean room: the ETSI text and our own MIT code only. MS authentication (6.4.8, 6.4.4.1.5) is built
(section 3.14, owner's go-ahead 2026-11-12); authentication of the site by the MS (6.4.9.2) and 6.4.10 are not.

## 1. Goals and the line for this round

The gateway becomes a standard Tier III trunked station (TS): a control channel (TSCC), payload channels, random
access, registration, group and individual calls. Standard third-party Tier III radios and, later, RadioFoundry
radios (the MS side) use it. Sites link over the MQTT network (mqtt-net), the same network Tier II repeaters and
hotspots use; HBlink is not used.

Principles:

- **A site runs on its own.** Registration, grants and local calls need only the site's own box. The network adds
  wide-area calls, roaming and central management; losing it costs those and nothing else.
- **One network for Tier II and Tier III.** Same call messages, same topics, same router, same node presence. Tier III
  adds what it needs (registration records, call setup, the site description, provisioning) as small extensions that
  Tier II nodes can use or ignore.
- **Ready for more.** Multi-site roaming, a standby controller, more carriers and more services fit the structure
  without a rewrite (section 12).

### 1.1 Built in this round

| Area | Built |
|---|---|
| RF layout | 2 carriers: A (SDR TX + SDR RX) carries the dedicated TSCC on TS1 and a payload channel on TS2; B (TX from the same SDR, summed into its transmit stream with each carrier at half amplitude, or from a split radio; SDR RX) carries two payload channels. 1 control slot, 3 payload slots. The controller, the radio layer, the unit and the simulator take 1-4 carriers (up to 7 payload channels), the TSCC on the first; tested with 3 and 4 on simulated air. rdofradio drives 2 at most (hardware) |
| Control channel | C_SYS_Parms short LC with a counting Common_Slot_Counter; C_ALOHA; C_BCAST Adjacent_Site (from the network), Chan_Freq (the site's own channels and its neighbors' TSCCs), Local_Time (when the clock is good), Gen_Site_Params, CallTimer_Parms, Vote_Now (with the VOTE_BLK block), Ann_WD_TSCC, MassReg (section 3.11); CACH AT driven by the controller |
| Random access | Slotted Aloha, Mask 0, Service Function from load, adaptive Backoff, NRand_Wait from settings; a response to every random access |
| Registration | Open or listed (ID sets); register, de-register, implicit registration from call requests; single-talkgroup subscription (TRGT_ADR_CNTS 01b) and a list of up to 7 by UDT upload (10b, 3.17); Reg_Accepted / Reg_Refused / Reg_Denied; MS_Not_Registered |
| Group voice | C_RAND, C_WACKD/C_QACKD, TV_GRANT (repeated; Late_Entry grants through the call and its hang time), BTV_GRANT for all-calls (ALLMSID, ALLMSIDL, ALLMSIDZ) and broadcast calls to a talkgroup (BCAST_SV), quasi-transmission trunking (hang time; none for broadcasts and all-calls), P_CLEAR on hang expiry, call timer or P_MAINT DISCON |
| Individual voice | C_AHOY availability check (OACSU or FOACSU, a setting), C_ACKU/C_NACKU, Call Answer, PV_GRANT, cancel (Service_Kind 1111b) |
| Full-duplex individual voice | Service_Kind 1010b (6.6.9): radio check, a whole carrier in offset timing, a PV_GRANT_DX to each party, each party's inbound repeated on the other's slot; local calls; a setting, off by default (section 3.13) |
| Queueing | One queue by priority (emergency first, then PRIORTY_SV, then age); C_QACKD; SYSbusy_Refused when full |
| Authentication | MS authentication (6.4.8): a challenge at registration (6.4.4.1.5) for MSs with a key, or every MS; an authentication poll on command; the 6.4.8 algorithm with the PSN; keys from `cfg/ms` (section 3.14). The MS authenticating the site isn't built |
| Emergency | Emergency group calls (the emergency call timer, the Emergency flag in grants; an emergency request on an open call upgrades it); pre-emption of a running call when no channel is free (6.4.14.3.1), with the Reverse Channel cease command (6.4.15, table 6.32) to its talker; a setting, off by default (section 3.9). The emergency alarm and its cancel (status 126 and 124, 6.6.6.3) through status delivery |
| Status and the emergency alarm | The Status Transport Service (6.6.6.1, 6.6.6.2) delivered as short data (section 3.15): to an MS (C_AHOY, its C_ACKU or C_NACKU mirrored), a talkgroup or the TSCC, stored while the called MS is busy, polling; the emergency alarm and its cancel (6.6.6.3) under the MS's emergency provisioning; an `activity` `status` event (section 3.10) |
| Mass re-registration | MassReg (6.4.5): all MSs or one, on command (`mass_reg`) or at start, repeated inside its Reg_Window (section 3.11) |
| TSCC modes | C_MOVE on command (all MSs or one, to another TSCC's logical channel, or to an adjacent site's TSCC by its frequencies in the MBC form); the asynchronous (hibernating) TSCC, woken by BS_Dwn_Act or any good inbound block; a setting, off by default (section 3.12); the composite TSCC (its slot carries a call when every payload channel is busy, P_SYS_Parms with Payload Channel Type 1 meanwhile; a setting, off by default, section 3.19) |
| Power save | PowerSave_RQ in registration, PowerSave_Offset in the C_ACKD (6.4.7.2.1); C_AHOYs to a sleeping MS held for its wake frame; a setting, off by default (section 3.11) |
| Short data | UDT short data on the TSCC both ways (C_UDTHD/C_UDTHU, 1-4 rate 1/2 blocks, up to 46 octets), stored and forwarded MS to MS, MS to talkgroup, MS to site and site to MS; status messages and status polling; across sites in mqtt-net's `sdata` item (3.15, 5.9) |
| Packet data | Individual and talkgroup packet data calls (C_RAND 0010b/0011b): the availability check (C_AHOY 0010b), PD_GRANT/TD_GRANT single or multi-item, the payload channel in data mode (each data burst repeated), confirmed and unconfirmed data at rate 1/2, 3/4 and 1 (the MS simulator's selective retry: SACK, C_RDATA, F = 0 resends), data hang time and clearing; MS to MS, MS to talkgroup, MS to the site's IP gateway (TSI, its data ID or IPI) and the gateway's replies down (3.16); across sites over mqtt-net to an MS registered at another site (the check there through the setup) and to the sites that carry a `wide_tg` talkgroup, a whole message at a time, and the gateway's replies to an MS at another site (5.10) |
| Talkgroup lists | Registration with a talkgroup subscription/attachment list (C_AHOY from TATTSI, the Address Format upload, one Response_Info bit per talkgroup); DGNA from an MS (C_AHOY from DGNAI, C_DGNAHU then C_DGNAHD) or from the site, both modes; the site carries the talkgroups either list holds (3.17) |
| Supplementary services | Stun and revive from the site (C_AHOY from STUNI; a stunned MS gets no user services), with or without the MS authenticating the TSCC (C_ACKVIT, C_ACKD Authentication_Response); kill (from KILLI, always authenticated); the Ambient Listening Service between MSs at the site (a setting); call diversion of voice, packet data, short data and status to an MS or a talkgroup, set, cleared and cancelled by the recipient, the caller told the diverted address (3.18) |
| Transmit interrupt | A listener's payload interrupt of a group or individual call (status 125), the ALS caller's end of the call (status 123) and the call timer de-key (6.4.14.3.2-6.4.14.3.4): C_ACKD or C_NACKD from TSI, the RC cease command to the talker, then P_PROTECT(EN_PTT_ONE_MS) to the interrupter or P_CLEAR; a setting, off by default (section 3.20) |
| Refusals | C_NACKD(Not_Supported) for every other Service_Kind; Called_Party_Busy, Called_Group_Not_Allowed, Perm_User_Refused as provisioning says |
| Network | Wide-area group calls both ways; individual calls across sites with an availability check over the network; Tier II to Tier III and back; registration records; site description; provisioning from the manager, cached at the site |
| Services | The site controller (Rust, in RFG), the radio layer (C, in RFG), the router's additions, the manager and its dashboard (Python, network services), the MS simulator |
| Tests | Rust unit and scenario tests with the simulated air, C host tests of the radio layer and the network messages, a whole-system run with simulated radios over a real broker |

### 1.2 Later (the design leaves room; nothing here is built)

Multi-part packet data (extended addresses through gateways), dual-slot (HI_RATE) data,
short data to gateways (extended addresses, supplementary data), a second TSCC, TSCCAS (section 3.12
says why), DGNA across sites for the site's records (a roaming MS's dynamic talkgroups), ALS across sites or from
the site, transmit interrupt from a gateway or the network (3.20), call
diversion to gateways or across sites, gateways (PSTN, PABX, dispatcher), duplex calls over the network and duplex
packet data, a standby controller, trunking on the W12 (the LR2021 can't stay keyed: slot-timing.md),
encryption beyond pass-through.

Two carriers out of one SDR, now built (9.4), were tried first on the Pluto (2026-11-06, with fixed test signals, not
the site): two DMR BS
carriers 125 kHz apart, each idle bursts with the CACH on both slots, summed in software and sent on TX1 (2.4 MS/s,
the LO between them, each at half amplitude), both decoded on the SDR bench with no bit errors beyond the single
carrier's, 6 dB down each; intermodulation stayed under the bench receiver's floor, about 22 dB below the carriers,
so the true level is unmeasured. The AD9363's second chain (TX2, same LO) took its own stream (DMA channels 2 and 3)
in 2R2T mode, but its port isn't cabled to the bench, so it wasn't heard. Either way works for a two-carrier site:
rdofradio uses one chain, the simplest; two chains would keep each carrier on its own amplifier.

## 2. Architecture

```
              site (one box: Pluto + split radio)                     network services (anywhere)
 +-------------------------------------------------------+       +-------------------------------+
 |  radio layer (C)              site controller (Rust)  |       |  router (pc routing, where)   |
 |  SDR modem slot hook  <---->  rdof_t3 (sans-IO):      |       |  manager (provisioning, cmd)  |
 |  payload channels             registration, access,   |       |  dashboard (web)              |
 |  split radio (carrier B)      calls, queue, grants,   |       +---------------^---------------+
 |  burst/CACH/FEC               TSCC schedule, timers   |                       |
 |        |                              |               |              backbone broker
 |        +------ mqtt-net (C) ----------+               |                       |
 |                    |                                  |                       |
 |              site broker (optional, bridged) ---------+-----------------------+
 +-------------------------------------------------------+
```

| Part | Language, where | Does |
|---|---|---|
| Site controller | Rust, `rust/src/t3*.rs` (the `t3` feature of `rdof_rust`), C API `rdof/t3.h` | Every Tier III decision. No I/O, no allocation, no clock of its own: the caller feeds it PDUs, voice activity, network events and time; it returns what to send per slot and events to act on. Runs in rdofradio now; built `no_std` so a board can run it later |
| Radio layer | Rust, `rust/src/t3rf.rs` (portable) and `t3split.rs` (carrier B's split), + `sdr/cli/rdofradio_t3.c` | Burst building and parsing (BPTC, CRC masks, slot type, sync, CACH, short LC), the per-slot hook into the SDR modem, the payload channels (local repeat, network voice in, idle, the controller's payload PDUs), the split radio for carrier B, the receive path from the SDR's channels |
| Network side of the site | C, `common/src/mqttnet.c` and rdofradio; the unit that joins it to the controller and the radio layer is Rust, `rust/src/t3unit.rs` (C API `rdof/t3unit.h`) | mqtt-net as today, plus the setup item, registration records, the trunk part of `info` and `status`, provisioning topics and their cache |
| MS simulator | Rust, `rust/src/t3ms.rs`, `t3air.rs` | Simulated radios that follow 361-4 (hunt omitted, access, registration, calls, answers), and a slot-exact air model with collisions. Used by the Rust tests, the C tests, and rdofradio's `--sim-air` |
| Router | Python, network services | As today, plus location from registration records and an early refusal for an unknown called party |
| Manager | Python, network services | The provisioning source of truth: network identity, sites, talkgroups, radios, permissions, timers. Publishes retained `cfg` topics; sends commands; serves the dashboard and a small REST API |
| Dashboard | HTML + JS served by the manager | Nodes, sites and their channels live, registrations, calls, last heard, router counters, provisioning editor |

Why Rust for the controller: the owner's choice for new logic in RFG; the controller is pure state machines over
fixed tables, which `no_std` Rust does well, and the same crate holds the MS simulator so the PDU codec exists once.
The radio layer is Rust too; the FEC, the voice regeneration and the split code it drives stay C, and it calls them.

The controller works on PDU bodies (the 10 info bytes of a CSBK, MBC or UDT block, before the CRC) and short LC
values (28 bits, before the CRC-8). The radio layer adds and checks CRCs, BPTC, slot types, syncs and the CACH. So
the Rust has no C dependencies and its tests run with `cargo test`.

## 3. Protocol subset and choices

### 3.1 Identity and channels

| Item | Choice |
|---|---|
| C_SYScode | MODEL, NET, SITE, PAR from the provisioning (manager, else settings). One network = one MODEL + NET; each site its SITE. PAR 11b (A and B) by default |
| DMRLA / SYS_AREA | Setting `area_bits` (default: all SITE bits, so every site is its own registration area) |
| Logical channel numbers | Each carrier's `chan` (1..4094) in the settings; grants use single-block CSBKs with that number. LCN 0 = TS1, 1 = TS2 |
| Channel plan | One plan for the network: each logical number names one frequency pair at every site. The MS must know it: programmed in the radio, or learned from the C_BCAST Chan_Freq broadcasts (MBC + BC_AP with absolute Tx/Rx per channel), sent every `chan_freq_s` (default 10 s) for each of the site's carriers and each neighbor's TSCC channel (section 3.11) |
| Colour code | One CC for the site (strategy (c), 6.2.1.3: payload channels use the TSCC's CC). Inbound payload PDUs with another CC are dropped |
| Timing | Aligned TSCC (Offset 0). Site Timeslot Synchronization 0: carrier B's split radio is aligned only to 1-2 ms, so payload channels send idles for 4 CACH before the MS's first sync opportunity (6.6.1.6.2.2) |
| Version | 100b (1.11.1, the latest code; V1.12.1 has none of its own) |
| Active_Connection | 1 while the backbone (mqtt-net) is connected, else 0 |

### 3.2 The control channel schedule

Carrier A TS1 sends a CSBK or MBC block every slot (every 60 ms). The controller picks, in order:

1. A response that is due: an acknowledgment, a C_AHOY, an initial grant (each initial grant twice, in consecutive
   TSCC slots).
2. The continuation block of an MBC already started.
3. A grant repeat that is due: every `grant_every_s` (default 1.2 s) per active call, with Late_Entry = 1, through
   the call and its hang time, until P_CLEAR (6.6.2.4.1, "while a call is active"; the captured site sends them up
   to P_CLEAR). For a while (69c77db6) none went out in the hang: a radio on the bench re-followed a finished call
   from one (2026-11-13). The likely cause is on the radio's side: it took the idle bursts of the hang for a dead
   channel, left early, and then followed the call's next Late_Entry grant back (inferred from its code, not seen on
   air). The hang now carries terminators (3.5), and an MS that counts them as activity stays to P_CLEAR.
4. A broadcast that is due: MassReg while one runs, CallTimer_Parms every `call_timers_s`, Vote_Now per neighbor
   every `vote_now_s`, Ann_WD_TSCC every `ann_tscc_s` (section 3.11; all off by default), Adjacent_Site per neighbor
   every `adjacent_s` (default 5 s), Chan_Freq every `chan_freq_s` (the site's carriers and the neighbors' TSCC
   channels in one rotation), Local_Time every `local_time_s` (default 6 s)
   when the clock is synchronized, Gen_Site_Params every 10 s.
5. C_ALOHA, Mask 0, address ADRNULL.

At least one C_ALOHA goes out every 4 TSCC slots whatever else is due (an MS waits for an Aloha before it accesses,
6.2.1.3).

CACH on carrier A: TC alternates; the short LC is C_SYS_Parms (SLCO 0010b) with the Common_Slot_Counter, which
advances every 4 slots; Reg matches C_ALOHA. AT = 1 in the CACH before the inbound slot where a solicited response
is expected (after C_AHOY, after a response that solicits), else 0. With aligned timing that is the CACH right after
the TSCC's TS1 burst (its TC names TS2): 361-1 table 5.1 pairs outbound TS1 with inbound TS2 in one logical channel,
and 4.5 / figure 4.8 tie a CACH to the inbound slot its TC names; 361-4 6.2.1.1.3 puts the AT in "the CACH
following" the Aloha or the soliciting PDU. A standard site's TSCC on air agrees (RadioMax capture, 80 k bursts:
after an Aloha that CACH said idle 97% of the time; after a soliciting PDU, busy). The CACH before TS1 carries TS2's
inbound: busy while a call holds TS2. Carrier B, payload only, sends P_SYS_Parms
(SLCO 0011b) while either slot is active and Activity Update otherwise; when both slots are idle and the split
radio's call ends, carrier B unkeys.

Carrier A TS2 when not a payload channel: idle bursts (4.8: the other slot may be idle; an MS takes nothing from it).

### 3.3 Random access

- NRand_Wait from settings (default 6, as the RadioMax reference: 6 frames, 360 ms). The site controller is in the
  same process as the radio layer, so it answers within 3-5 frames (inbound decode about 60 ms, outbound lead about
  1 slot plus the front end's buffer, about 100 ms on the Pluto). A request it can't settle at once gets C_WACKD.
- Backoff adapts: the controller counts inbound random accesses and collisions (a burst with a good sync and a bad
  CRC on the TSCC inbound slot counts as one) over the last 2 s; Backoff 3 when quiet, up to 9 under load.
- Service Function: 00b normally; 11b (registration only) while the queue is full; 01b is not used yet.

### 3.4 Registration

- Policy (provisioning): `open` (any ID that asks) or `listed` (an ID set). An unlisted ID gets Reg_Denied (the
  standard's preferred refusal, 6.4.2) unless `refuse_unlisted: "refused"`.
- Requests whose REG_ADDR doesn't carry this site's C_SYScode are discarded (6.4.4.1.0).
- A registration record holds: ID, time, expiry (`reg_hold_s`, default 4 h, refreshed by any PDU from the MS),
  subscribed talkgroup (single), talkgroups learned from its calls (up to 8, most recent first). Records survive a
  restart (section 6.2).
- De-registration is answered C_ACKD(Reg_Accepted) at once (6.4.6), then the record goes.
- Implicit registration: a call request from an ID with no record makes one (4.10.2) when the policy allows the ID.
- With `require_registration` (default true) Reg = 1, and service requests from unregistered IDs get
  C_NACKD(MS_Not_Registered), and a registration demand (MassReg, Mask 24, its address, Reg_Window 1 s; 6.4.5.0)
  when no mass re-registration is under way: a standard MS needn't re-register on the NACK alone.
- A record that reaches its expiry isn't dropped at once: the site demands a registration of its MS (as above) and
  keeps the record 30 s for the answer (5 s more each time while another mass re-registration is under way). An MS
  that answers keeps its record and its subscription; one that doesn't loses it. An MS idle on the TSCC longer than
  `reg_hold_s` sends nothing the site hears, so without this its record would go and its next call would be
  refused. A registration from the MS a demand names stops that demand's repeats.
- The simulated MS (t3ms) registers by random access when a C_NACKD(MS_Not_Registered) answers it (6.4.4.1.0), and
  takes a MassReg that comes while that registration is pending as answered by it.
- List subscription (TRGT_ADR_CNTS 10b, target TATTSI): the registration is checked first, then the list comes in
  a UDT upload and the C_ACK follows it (3.17). The C_ACK, or a refusal's C_NACKD, comes from TATTSI (6.4.4.1.13.2).

### 3.5 Calls

Group call (talkgroup, ALLMSID):

1. C_RAND (Service_Kind 0001b). Checks: registered (if required), allowed on this talkgroup (provisioning), the
   talkgroup allowed at this site.
2. A free payload slot: TV_GRANT (BTV_GRANT for ALLMSID/ALLMSIDL/ALLMSIDZ, and for a broadcast call, BCAST_SV
   in the request: one way, 6.6.2) twice, then repeats. None free: C_QACKD and
   into the queue; queue full: C_NACKD(SYSbusy_Refused).
3. The payload channel starts sending idles at once, then repeats the inbound voice of whoever talks, and plays
   network voice for the talkgroup.
4. Hang (`hang_s`, default 3 s, TV_Hangtime) after each item; any member may talk again during it. The payload
   channel keeps sending the item's Terminator with LC (its source and destination) in every slot of the hang, AT
   busy (361-1 5.2.1.4, 361-2 5.1.1.3; the captured site sends them too); a talker that went without a terminator
   gets one from the site first. A broadcast call and an all-call have no hang: P_CLEAR follows the talker's
   terminator (361-2 5.3.3.0, 5.3.3.1 "no call hangtime is to be generated"; 361-4 6.6.1.7 makes all-calls
   broadcasts). The call ends on
   hang expiry, the call timer (`call_s`, default 180 s, T_MS-MS_TIMER), or P_MAINT DISCON from the initiator: the
   payload channel sends P_CLEAR (target ALLMSI, source TSI, channel 0 = back to the TSCC) 4 times, then frees.
5. A new request for a talkgroup that already has a call in progress or in hang gets the same channel (a fresh
   TV_GRANT with Late_Entry 0).

Individual call:

1. C_RAND (Service_Kind 0000b). Checks as above, plus the caller's `private` permission.
2. The called ID is registered here: C_AHOY to it (source = caller, Service_Kind_Flag 0 for OACSU, 1 for FOACSU),
   repeated every 1 s up to 3 times, AT set. Meanwhile C_WACKD to the caller every 2 s (keeps its TP_Timer alive).
   - C_ACKU(Message_Accepted) (OACSU), or C_ACKU(MS_Alerting) then C_RAND(Call Answer) (FOACSU): PV_GRANT to both,
     twice, then repeats as for group calls.
   - C_NACKU: C_NACKD to the caller with the mirrored reason. No answer: C_NACKD(MSaway_Refused).
3. The called ID is registered elsewhere or not at all: a setup request goes to the network (section 5.4); the
   answer from there decides as above. Not found: C_NACKD(MSaway_Refused).
4. Cancel (C_RAND Service_Kind 1111b): C_ACKD(Message_Accepted) to the caller; C_AHOY(Service_Kind 1111b) to the
   called MS if it had answered (6.6.1.2); a network setup in flight gets a cancel.

Timers kept like the MS's: TP_Timer per pending call (`tp_s`, default 20 s), TNP_Timer for registrations (`tnp_s`,
default 10 s). On expiry the controller abandons the request.

### 3.6 Calls from the network

- Group: a talkgroup message arrives (the site subscribes to the talkgroups of its registered MSs and its static
  ones). If the talkgroup has a call on a slot, its voice goes there. Otherwise the controller grants a free slot
  (TV_GRANT, source = the network caller) and the voice plays after a 240 ms prebuffer. No free slot: queued for up
  to `net_queue_s` (default 2 s), then dropped and reported.
- Individual: a setup request for a registered MS runs the availability check and answers over the network. Plain
  private voice with no setup (from a Tier II node) for a registered MS starts an implicit setup: the AHOY and grant,
  with the voice held in the prebuffer up to 1.5 s; a failed check drops it.
- RF on a slot beats the network, as on a Tier II repeater: a network stream for a talkgroup whose local member is
  talking waits for the hang or is dropped.

### 3.7 Refusals

Every C_RAND Service_Kind other than 0000b, 0001b, 0010b, 0011b (packet data, 3.16), 0100b, 0101b (short data,
3.15), 0111b (status and polling, 3.15 and 3.10), 1000b (call diversion, 3.18), 1001b (Call Answer), 1010b (duplex,
3.13), 1101b to DGNAI (3.17), 1110b and 1111b gets C_NACKD(Not_Supported) (4.2). Statuses 123 and 125 (transmit
interrupt) are answered from TSI as section 3.20 says: C_NACKD(Not_Supported) with the setting off (6.4.14.3.3.1,
6.4.14.3.4.2). A stunned MS's requests get
Perm_User_Refused, and requests to it MSaway_Refused (3.18). Gateway addresses (PSTNI and the rest) as targets also
get Not_Supported, TSI excepted for short data, status and packet data, and IPI for packet data with nothing to
upload. UDT polling (0110b) likewise.

### 3.8 Timers and constants (settings, under the app's `timers`)

| Key | 361-4 | Default | Range |
|---|---|---|---|
| `nrand_wait` | NRand_Wait code | 6 | 0-15 |
| `backoff_min`, `backoff_max` | Backoff codes | 3, 9 | 1-15 |
| `tp_s` | TP_Timer | 20 | 4-60 |
| `tnp_s` | TNP_Timer | 10 | 2-60 |
| `hang_s` | TV_Hangtime | 3 | 1-60 |
| `call_s` | T_MS-MS_TIMER | 180 | 1-4094 |
| `emergency_call_s` | T_EMERG_TIMER | 300 | 1-510 |
| `ahoy_tries`, `ahoy_every_s` | | 3, 1.0 | 1-10, 0.3-5 |
| `grant_every_s` | grant repetition | 1.2 | 0.3-10 |
| `clear_repeats` | P_CLEAR repetition | 4 | 1-8 |
| `warmup_cach` | payload warm-up | 4 | 2 or 4 |
| `reg_hold_s` | registration record life | 14400 | 60-604800 |
| `queue_max`, `queue_s` | queue size, longest wait | 16, 10 | 1-16, 1-60 |
| `net_queue_s` | network call wait for a slot | 2 | 0-10 |
| `setup_s` | wait for a network setup answer | 5 | 1-30 |
| `start_grace_s` | a granted channel waits this long, past the hang, for the first voice | 2 | 0-10 |
| `sd_tries` | short data or a status to an MS: sends (each `ahoy_every_s` apart) before MSaway | 3 | 1-10 |
| `sd_repeats` | short data or a status to a talkgroup: sends | 2 | 1-10 |
| `sd_store_s` | how long short data waits for a busy MS (6.6.6.1.1.5) | 30 | 0-600 |
| `data_hang_s` | data hang time: a data channel waits this long for more after a message (6.6.3.3.1.6) | 3 | 0.5-30 |

The parser is `rust/src/t3cfg.rs` (`rdof/t3cfg.h`); it takes the same keys from the manager's `cfg/net` and
`cfg/node/{node}` (section 5.7).

### 3.9 Emergency calls and pre-emption

An emergency request (C_RAND with the Emergency service option) is queued ahead of everything else. Its call runs
under `emergency_call_s` (T_EMERG_TIMER) and its grants carry the Emergency flag. An emergency request for a
talkgroup whose call is already open upgrades that call: the flag and the emergency timer from the call's start.

Pre-emption (6.4.14.3.1, "Emergency Pre-emption MS De-key"; `services.emergency_preempt`, off by default): when an
emergency call waits and no payload channel is free or already being freed, the site picks a non-emergency call and
clears it:

1. The call to clear: one in hang time first (nobody to de-key), then one whose talker is on a payload carrier, then
   the oldest. One call per waiting emergency.
2. Its talker on RF gets the Reverse Channel "cease transmission" command (RC info 0100b, table 6.32) on the other
   slot of its carrier, every 360 ms (the least the air interface allows, 361-1 5.1.5.1). The MS finishes its
   superframe, sends a terminator and stops (6.4.15).
3. Once the inbound stops, the channel sends P_CLEAR and frees; the queue gives it to the emergency call. A call
   with nobody talking, or with network voice only, is cleared at once. A talker still keyed after 3 s (an MS
   without the RC, which 6.4.14.0 asks the TS to deal with) gets P_CLEAR too, so the listeners leave, but the
   channel stays held, sending P_CLEAR, until its inbound has been quiet for 1 s (at most 60 s): 6.4.14.3.1
   connects the emergency call there only "following the cessation of inbound PDUs". While one is held, another
   call may be pre-empted for the emergency.
4. When nothing waits any more (the emergency got another channel, or was cancelled), the pre-emption stops and the
   call goes on.

With pre-emption on, an emergency request is queued even when the queue is full.

The RC burst (361-1 9.1.5, B.2.2.2, B.3.12, B.3.13): 4 info bits and a CRC-7 masked with 7Ah, one Hamming
(16,11,4) row over an odd-parity row, interleaved into the 32 bits of the embedded field, with PI = 1 in the EMB.
The radio layer puts it where the alternate slot's outbound has room: voice burst F's embedded field (an embedded
RC), or an idle burst in place of its sync (a dedicated RC); never in a header, never in the burst right after a
terminator. When the talker is on the TSCC carrier's payload slot, the alternate slot is the TSCC: the controller
leaves that slot's block out and the radio sends the RC in an idle burst (a few control blocks lost a second, only
while a talker is being de-keyed). Choice: the cease command (0100b), not the cease request (0101b): 6.4.14.3.1 is a
de-key, and a request may be ignored by the MS.

### 3.10 Status and the emergency alarm

The Status Transport Service (Service_Kind 0111b, 6.6.6.1) carries a 7-bit status on the control channel. The
C_RAND has it in Service_Options (G/I, SUPED_SV, the high 5 bits) and the two bits before the Service_Kind (table
6.57); the C_AHOY to the called party has it in Service_Options_Mirror and Appended_Blocks (table 7.22, note 2).
Status 126 is the emergency alarm and 124 its cancel (6.6.6.3): an emergency without a voice call. 127 is a poll
(6.6.6.2).

A status is delivered as short data with no upload (section 3.15: to an MS here or on the network, a talkgroup, the
TSCC; stored while the called MS is busy or asleep; polling). What this section adds, for a status from an MS:

- The emergency alarm (126) only from an MS whose provisioning allows emergencies (`emergency`), else
  C_NACKD(Perm_User_Refused): the C_AHOY doesn't carry the voice call's emergency check.
- Not delivered: the transmit interrupt statuses (123, 125; 6.6.6.0), requests to the site (section 3.20). Not
  served (C_NACKD(Not_Supported)): extended addresses (the Proxy Flag). A request for supplementary data is ignored (6.6.6.1.2.1.2 a).
- Each delivered or failed status is an `activity` `status` event (mqtt-net.md section 7) with `alarm` on 126 and
  124, and the alarm goes in the log (`rust/src/t3status.rs`).

### 3.11 Announcements, mass re-registration and power save

C_BCAST, each Announcement_type (table 7.68) and what the site does with it:

| Type | Built | How |
|---|---|---|
| 0 Ann_WD_TSCC (6.7.1.1, table 7.69) | yes, off by default | the adjacent sites' TSCC channels for the MSs' hunt lists, two to a CSBK (CHNULL in the second when odd), every `ann_tscc_s` over them all; a neighbor known to be off line is withdrawn (AW_FLAG 1, color code 0). The color code is the neighbor's (its `info`'s `tscc[0].cc`; the site's own when not given). A neighbor whose channel clashes (below) is left out: the MBC form with BC_AP isn't built |
| 1 CallTimer_Parms (6.7.1.2, table 7.70) | yes, off by default | `emergency_call_s` and `call_s` as the tokens of annex A tables A.2 and A.4, the largest that doesn't pass the site's timer (so the MS's ends first); the packet and line timers 0 (the MS's own: the site has neither) |
| 2 Vote_Now (6.7.1.3, table 7.71) | yes, off by default | each neighbor in turn within `vote_now_s`: its 14 syscode bits, Active_Connection when known, the priorities (below), CH_VOTE its TSCC's logical channel. A neighbor whose channel clashes goes as the MBC: CH_VOTE 4095 and a VN_AP block (7.2.19.3.1, table 7.72) with its color code and CdefParms (its number and frequencies, the MS's view). Then `broadcast.vote_blk` (default 6) TDMA frames, one TSCC slot each, of C_ALOHA with ADRNULL and Mask 24, which lets no MS in, so a voting MS misses nothing |
| 3 Local_Time (6.7.1.4) | yes (section 3.2) | |
| 4 MassReg (6.4.5, table 7.76) | yes | below |
| 5 Chan_Freq (6.7.1.6, table 7.78) | yes (section 3.2) | each carrier, then each neighbor's TSCC channel, in turn (below) |
| 6 Adjacent_Site (6.7.1.7, table 7.79) | yes (section 3.2) | from the network, with the priorities (below); a neighbor whose channel clashes is left out (CH_ADJ has no absolute form) |
| 7 Gen_Site_Params (6.7.1.8) | yes (section 3.2) | the hibernating flag 1 just before an asynchronous TSCC goes quiet (section 3.12); the "Reg and TG Subscription" bit 0 |
| 30, 31 manufacturer specific | no | none defined |

Neighbors' channels. An MS resolves a logical channel through its programmed plan or the Chan_Freq it has heard.
361-4 6.1.2 allows a plan that grows over the air: "new physical/logical relationships that adds to or modifies the
existing channel plan stored in MS may be broadcast on the TSCC", and 4.7.1 and annex C.1.1.1 give one plan of up to
4094 numbers. Nothing ties Chan_Freq to the sending site's own carriers (table 7.78: "Physical Channel Number of the
Channel being Announced"). So the site sends Chan_Freq for each neighbor's TSCC channel too, from the neighbor's
`info` (`tscc[0]`: `chan`, `tx_hz`, `rx_hz`), as the MS uses it (TX on the neighbor's inbound), and the MS can resolve
CH_ADJ and CH_VOTE with no plan programmed. The other way, absolute frequencies in the PDU itself (6.1.2 b), exists
for Vote_Now (CH_VOTE 4095 with VN_AP, 7.2.19.3 c) and Ann_WD_TSCC (MBC with BC_AP, 7.2.19.1), not for Adjacent_Site
(table 7.79 defines no appended block; table 7.1 lists it as a CSBK only). The site uses both, by case:

| Neighbor's TSCC channel | Chan_Freq | Adjacent_Site, Ann_WD_TSCC | Vote_Now |
|---|---|---|---|
| frequencies known, number free here | announced | logical | logical |
| frequencies not in its `info` | not announced (the MS's plan) | logical | logical |
| the number is a carrier here, or an earlier neighbor's, at the same frequencies | not announced again | logical | logical |
| clash: the number is a carrier here, or an earlier neighbor's, at other frequencies | not announced (it would change the MS's plan for the other channel); logged | left out | CH_VOTE 4095 + VN_AP |

A clash means the network's numbering isn't one plan; give each site's carriers numbers no other site uses.

Priorities (the Radiated Preference Strategy, 6.7.1.3): Vote_Now and Adjacent_Site carry a confirmed channel
priority and an adjacent channel priority. 6.7.1.3: "These fields convey the priority of the current (confirmed)
site and the priority of the adjacent site to be sampled", values per table 6.81: 1 the highest, 7 the lowest,
0 "Non-Preferred Site" (0 in both is the "No Radiated Preference Strategy"). Table 7.79 has the same two fields for
Adjacent_Site; 6.7.1.7 doesn't describe them, so the site gives them the same meaning. Confirmed: the sending site's
`broadcast.priority`; adjacent: the neighbor's, from its `info` (`trunk.priority`, 0 when not given). 6.7.1.3: "a TS
will either radiate all Vote Now Advice broadcasts with preference priorities or it shall radiate all [...] with no
preference priority": give every site of a network 1-7, or leave all at 0. Example, pluto-t3 (site 1, chan 1 TSCC at
461.4625 / 461.0875, `priority` 0) and hat-t3 (site 2, chan 3 TSCC at 461.2875 / 461.1375, `priority` 3):

| Sent by | Chan_Freq (CH_ADJ: MS TX / RX MHz) | Adjacent_Site and Vote_Now: syscode MSBs, confirmed, adjacent, CH |
|---|---|---|
| pluto-t3 | 1: 461.0875 / 461.4625; 2: 461.2375 / 461.3375; 3: 461.1375 / 461.2875 | hat-t3's, 0, 3, 3 |
| hat-t3 | 3: 461.1375 / 461.2875; 1: 461.0875 / 461.4625 | pluto-t3's, 3, 0, 1 |

VOTE_BLK (annex A.2: "Number of TDMA frames that the TSCC withdraws random access activity after a Vote Now
Advice", 2 to 10): `broadcast.vote_blk`, default 6. A TDMA frame is 60 ms, two slots, and the TSCC has one slot of
it, so the site counts TSCC slots: 6 frames are 360 ms, as long as an MS that samples the named TSCC for 360 ms is
away. 6.7.1.3 lets only "An Aloha PDU with MS Address = ADRNULL and Mask = 24" and C_WACKD go in those slots; the site
sends the Aloha. After an MBC Vote_Now the frames start after the VN_AP block.

Mass re-registration (6.4.5): the `mass_reg` command (mqtt-net, section 5.7; `rdof_t3_command(s, "mass_reg",
window_s, ms)`) or `services.mass_reg_at_start_s` (a restarted site has lost the records it didn't load). MassReg
with Mask 0 for all MSs, or Mask 24 and the MS's address for one, and the smallest Reg_Window (table 6.12) that
covers the time asked. Each MS registers by random access at a random time within it (the C_RAND isn't marked: an
ordinary registration). The site repeats the broadcast inside the window, so an MS that heard one ignores the
others (6.4.5.1 e): 3 times without power save; with power save once in each power save frame for the longest
cycle offered, and the window at least that long, so every sleeping MS hears one in its wake frame (6.4.5.1 f).
Window 0 cancels a mass re-registration (the pending attempts with it), repeated the same way.

Power save (6.4.7), off unless `services.power_save` is set: an MS that asks for 1:2^n (PowerSave_RQ n, 1-7) with n
up to the setting gets a PowerSave_Offset (1 to 2^n - 1, table 6.13) in Response_Info of its registration's
C_ACKD; a larger n or none gets 0 (registered, power save not available: the site doesn't refuse it). Only
Reg_Accepted carries the offset (table 7.41): a registration whose single talkgroup is refused gets 0110 0101b with
Response_Info 000 0000b (6.4.4.1.13.1) and no power save, even if it asked (the clause has PS_RQ 000b there). The offset
comes from the MS's subscribed talkgroup (else its address), so a talkgroup's members share a wake frame. PS_Counter
is the Common_Slot_Counter's 7 high bits (one power save frame is 4 counts, 480 ms; figure 6.29); the MS wakes when
PS_Counter masked to n bits equals its offset (table 6.14). A C_AHOY to a sleeping MS (an individual call, a
status) waits for the first 8 slots of its wake frame, and the call's set-up time limit stretches by one cycle; the
MS is taken as awake for `services.power_save_awake_s` (the site's T_Awake, 1 s) after a PDU to or from it, so the
following C_AHOYs and the grant go at once.

A group call is a transaction to a "group of MSs" (6.4.7.1: it "shall be queued on the TSCC until the designated
power save frame has been reached"). A local group call to a talkgroup with members asleep (in power save, not
awake as above; for an all call, every MS) waits, the caller answered C_WACKD, for the first frame that all of them
wake in, early enough in it (its first 4 slots) for the grant: up to `services.power_save_group_hold_s` (default
2 s; 0 never waits). Members subscribed to one talkgroup with one PowerSave_RQ share that frame. With no such frame
in that time (members with different ratios or offsets), an emergency, a call from another site (its talker is
already talking) or a talkgroup already on a channel, the grant goes at once, and in each power save frame a call
whose talkgroup has a member asleep that wakes in it sends its Late_Entry grant repeat early in the frame, whatever
`timers.grant_every_s` is. The registration record keeps the grant (PowerSave_RQ and PowerSave_Offset; mqtt-net
`ps_rq`, `ps_off`), so a restarted site that reads its records back pages a sleeping MS in its wake frame.

### 3.12 TSCC modes: C_MOVE, the asynchronous TSCC, and what isn't built

C_MOVE (6.3.3.1 e, table 7.17): the `move` command (`rdof_t3_command(s, "move", chan, ms)`, mqtt-net `move`) sends
C_MOVE three times on the TSCC: Mask 0 for every MS, or Mask 24 and the address for one; the Reg and Backoff of
C_ALOHA; the target TSCC's logical channel (1-4094). A listening MS leaves for that channel; nothing answers. Use:
send MSs to a neighbor's TSCC before taking this one down, or one MS to another site.

To an adjacent site by its C_SYScode (`rdof_t3_command(s, "move_site", syscode, ms)`, mqtt-net `move` with `site`):
the neighbor's TSCC by its logical channel when that number names it here (section 3.11: announced in Chan_Freq,
or not a clash); by its frequencies when the number names other frequencies here, or when the neighbor has
frequencies but no number: the MBC form (7.1.1.1.3 c), Physical Channel Number 4095 in the header and the MV_AP
block (7.1.1.1.3.1, table 7.18) with the neighbor's color code and CdefParms (the MS's transmit and receive
frequencies, as in VN_AP). Both blocks go together, three times, never split by a due C_ALOHA. Not a neighbor, or
neither number nor frequencies known: refused.

The asynchronous TSCC (5.3.3, 6.1.1.2), off unless `services.hibernate_s` (T_BS_Inactive, 1-300 s) is set: with no
good inbound block on the TSCC for that long and nothing under way (no call, no request waiting, nothing to send),
the TSCC sends Gen_Site_Params with the Hibernating_Flag twice, then goes quiet: no C_ALOHA, no C_SYS_Parms in the
CACH. An MS that heard the flag doesn't hunt for want of C_ALOHA; with something to do it sends BS_Dwn_Act (361-2
7.1.2.1, table 7.5a; BS address 0: 361-4 doesn't say what a TSCC's is, so the site takes any) on the TSCC's
inbound. Any good inbound block wakes the TSCC, and so does anything the site has to send (a call from the
network, a status). The radio layer still keys carrier A while the TSCC is quiet (idle bursts): keying the
transmitter down and back on the TSCC's slot grid is later work.

Not built, and why:

- A second TSCC (6.1.1.1, 6.3.2.2.1.2-4: two TSCCs split by PAR and control category): the controller keeps one
  control channel's state (its response queue, Aloha schedule and load); a second needs a second of each and a
  rule for which MS is served where. Two sites with `par` A and B on separate carriers come close today, each its
  own controller.
- Moving the site's own TSCC to another carrier: the radio layer and the unit treat carrier A's first slot as the
  TSCC's home (its CACH, its short LC, its idle path). The composite TSCC (section 3.19) only lends that slot to a
  call and takes it back.
- TSCCAS (6.1.1.3): its content is USBD polling, which belongs to the data services (with LIP). C_ALOHA's TSCCAS
  flag stays 0.

### 3.13 Full-duplex individual calls

6.6.9, off unless `services.duplex` is true (off: C_NACKD(Not_Supported), as before). An MS asks with C_RAND,
Service_Kind 1010b (table 6.73), to another MS registered at this site:

1. The checks of an individual call (section 3.5); the called party must be local (a duplex call over mqtt-net
   would need a stream each way: not built; C_NACKD(NoReg_MSaway)), not busy, and not asked by proxy.
2. C_WACKD to the caller, then the radio check: C_AHOY with Service_Kind 1010b (6.6.9.1.4; FOACSU as set). The
   called MS's C_NACKU is mirrored, MS_Duplex_Not_Supported (0x16) among them (the caller may then try an
   ordinary individual call); C_ACKU(CallBack) gives the caller C_ACKD(CallBack).
3. Accepted: the call needs a whole carrier, both slots free, so never the TSCC's. None free: C_NACKD(SYSbusy):
   duplex requests aren't queued, and a duplex emergency doesn't pre-empt.
4. A PV_GRANT_DX (table 7.10) to each party, twice: LCN 1 to the caller (Call Direction 1), LCN 2 to the called
   MS, the partner as source. The carrier runs offset timing (361-1 5.1.1.2, table 5.1): an MS on LCN n
   transmits in inbound slot n, which is the other slot to the one it hears, so it talks and listens at once. The
   radio layer (`rdof_t3rf_duplex`, set on both channels from the channel's `RDOF_T3_F_DUPLEX` open event) reads
   the carrier's inbound slots the other way round and repeats each party's inbound on the other's outbound.
5. Each channel keeps its own talker and hang time; the call ends when both are quiet past hang time, on the
   call timer, or on either party's P_MAINT DISCON: both slots send P_CLEAR. A duplex call that pre-emption
   picks is cleared at once (its parties don't listen on the other slot, where the RC would go).

### 3.14 MS authentication

The setting `auth` (section 4): `off` (default), `registration` (an MS with a key is challenged when it registers;
one without registers as before) or `required` (every MS is challenged; one without a key is refused). Keys come
from provisioning: `cfg/ms/{id}` `auth_k` (K, 128 bits) and `psn` (24 bits, optional; section 5.7).

The algorithm (6.4.8, figure 6.30): RC4 keyed with the challenge RAND (3 bytes, most significant first) and then K;
the first 256 bytes of keystream are discarded, the next 3 are the response, XORed with the PSN when the MS has one.
Checked against 6.4.8's vectors: RAND 7A17C0h, K 0102...10h gives E8CA1Dh; with PSN 010203h, E9C81Eh. The site's
challenges are in 000000h-FFFCDFh (table 6.17), from a mixer stirred with the platform's random source at start and
the time of each challenge.

Registration (6.4.4.1.5, figure 6.20): a registration C_RAND that passes the policy checks (section 3.4) is answered
with a C_AHOY with Service_Kind 1110b, Service_Kind_Flag 0, Service_Options_Mirror = the C_RAND's Service_Options,
target the MS and source the challenge (table 6.17), with AT set for the answer. The MS answers C_ACKU with reason
Authentication Response (0100 1000b; tables 6.18, 7.42) and the response in its target address.

- A matching response: the registration goes on as without authentication (Reg_Accepted, the record); activity
  `auth_ok`.
- A wrong response: C_NACKD(Reg_Denied), an earlier record removed; activity `auth_fail`, a log line.
- C_NACKU(MSNot_Supported): under `required`, Reg_Denied as a wrong response; under `registration`, the MS
  registers; activity `auth_none`.
- No answer: the C_AHOY is sent `ahoy_tries` times, `ahoy_every_ms` apart (section 3.8); then the site gives up
  silently (the MS's TNP_Timer ends its attempt, 6.4.4.1.6); activity `auth_none`.

An authentication poll (6.4.8.2, Service_Options_Mirror 0): the command `auth` (section 5.7) challenges a
registered MS with a key. A wrong response, or MSNot_Supported under `required`, removes its registration (record
action `auth_fail`, a log line); the site sends it nothing more and the MS registers again on its next access.
A good answer is `auth_ok`, no answer or MSNot_Supported otherwise `auth_none`.

Choices:

- Up to 8 challenges run at once; with the table full, a registration gets no answer and the MS repeats it after
  its random access wait.
- A C_RAND repeated while its challenge runs (the C_AHOY or the answer was lost) gets the same challenge again.
- An MS that must be challenged can't register implicitly through a call request: it gets MS_Not_Registered.
- A poll works with `auth` `off` too, for an MS with a key.
- `auth` is a local setting: the manager's `cfg` doesn't carry it, like `services.emergency_preempt`.
- K never appears in a log, an activity event, a record or `status`. It is in the retained `cfg/ms` documents and
  in the node's provisioning cache (rdofradio writes `cfg-cache.json` with mode 0600); see section 5.8.

The MS simulator (section 11) answers SK 1110b C_AHOYs with its key (C_ACKU 0x48) or, without one,
C_NACKU(MSNot_Supported), and during a registration waits TNP_Timer for the outcome. rdofradio's `--air-ms` MSs take
their keys from the `cfg/ms` documents, at start and whenever provisioning changes.
### 3.15 Short data on the TSCC (UDT short data, status, polling)

Status delivery and polling go this way too; section 3.10 adds the emergency alarm's policy, the statuses not
served and the `status` activity.

The site stores and forwards (361-4 6.6.4, 6.6.6); the code is `rust/src/t3sdata.rs` (procedures, a part of the
controller) and `rust/src/t3udt.rs` (the UDT header and blocks). Up to 8 messages are in progress at once.

The UDT (6.5, 361-1 9.3.x): a data header (data type 6, CRC mask CCCCh) with DPF 0000b and the opcode in octet 9
(C_UDTHD 26 outbound, C_UDTHU 27 inbound), then 1-4 rate 1/2 blocks (data type 7, 12 octets each, no CRC of their
own); the last block's two last octets are the CRC-CCITT over all the data octets (361-1 B.3.8, no mask), so a
message holds up to 12n - 2 octets: 10, 22, 34 or 46. Octet 8's UAB is the blocks less one, its Pad Nibble the
format's. All blocks of a UDT go in consecutive TSCC slots, ahead of the C_ALOHA-every-4th rule.

UDT short data from an MS (C_RAND Service_Kind 0100b individual, 0101b talkgroup):

1. Checks as a call (registered or an implicit record, `ms`), then the target: an MS ID, a talkgroup the site allows,
   or TSI (the site itself). Other gateways (PSTNI, PABXI, ...; extended addresses) get Not_Supported; an MS not
   registered here with the network down gets NoregMSaway; itself, ALLMSI as an individual target, or a poll to a
   talkgroup, Not_Supported. 8 in progress: SYSbusy.
2. The upload: C_AHOY (source SDMI, target the caller, Service_Kind 0100b, Appended_Blocks the C_RAND's
   Appended_UDT_Short_Data) with AT busy; then, for each block, C_ALOHA with mask 24 and address NULL and AT busy, so
   the caller's C_UDTHU and blocks have the inbound slots to themselves (figure 6.43). One upload at a time; a
   second caller hears C_WACKD and goes next. A CRC failure or no upload: C_AHOY again (`ahoy_tries` in all), then
   C_NACKD(CRC error, 30h). A C_NACKU to the C_AHOY ends it with that reason mirrored. Supplementary data
   (SUPED_SV) isn't taken: the message goes on without it (6.6.6.1.2.1.2 a).
3. The caller hears C_WACKD (C_QACKD(Queued_busy) while the called MS is in a call) every 2 s until the end.
4. To an MS here: C_UDTHD (A = 1, source the caller, UDT_Format as uploaded) and the blocks, AT busy after the last;
   repeated every `ahoy_every_s` up to `sd_tries` times. Its C_ACKU or C_NACKU goes to the caller mirrored in C_ACKD /
   C_NACKD; no answer: C_NACKD(MSaway). No availability check first (the download with A = 1 is the check; 6.6.4.1.2
   makes it optional). A called MS in a call: the message waits up to `sd_store_s`, then C_NACKD(Called_Party_Busy).
5. To a talkgroup: C_UDTHD (A = 0) and blocks, sent `sd_repeats` times; then C_ACKD(Message_Accepted) to the caller.
   A `wide_tg` talkgroup (or ALLMSID) also goes to the network.
6. To TSI: the message is the site's (event `EV_SD`; rdofradio logs it), C_ACKD(Message_Accepted).
7. To an MS not here, the network up: mqtt-net (5.9); the far site's result comes back mirrored, none in `tnp_s`:
   C_NACKD(MSaway).
8. Cancel (C_RAND Service_Kind 1111b) while the message waits (upload, stored, on the network): C_ACKD(Message_Accepted)
   and it's dropped; during a download: Call_Cancel_Refused.

The final C_ACKD / C_NACKD goes twice (figures 6.57, 6.62).

Status (C_RAND Service_Kind 0111b; Service_Options G/I, SUPED_SV, the status's 5 MSBs; the 2 LSBs in the dependent
bits; 6.6.6.1): as short data with no upload. Delivery is C_AHOY (Service_Kind 0111b, Service_Options_Mirror G/I and
the 5 MSBs, Appended_Blocks the 2 LSBs, source the caller). An MS answers C_ACKU(MS_Accepted), mirrored; a talkgroup
doesn't answer: `sd_repeats` sends, then C_ACKD(Message_Accepted). Status 127 is a poll (6.6.6.2): the polled MS's
C_ACKU(Accepted for Status Polling, 47h) carries its status in Response_Info; the caller gets C_ACKD(47h) with it.
A status 124 or 126 from an MS is checked and reported as section 3.10 says; 123 and 125 from an MS go to transmit
interrupt (section 3.20).

From the site (`rdof_t3_sd_send`, rdof/t3.h): UDT short data, a status or a poll to an MS or talkgroup, source TSI;
the result comes as an `EV_SD` event with the caller's tag.

Not built: short data to gateways and extended addresses (the multi-part addressing), supplementary data, UDT
polling (0110b: Not_Supported; Div_Cause_Fail when the polled MS diverted its short data, 3.18), the optional
availability check before an individual download.

### 3.16 Packet data calls (361-4 6.6.3)

The code is `rust/src/t3pdata.rs` (a part of the controller, with the data header codec, the message CRCs and a
block counter the radio layer shares); the MS side is in `t3ms`. Up to 8 data calls are being set up at once.

Request (C_RAND Service_Kind 0010b individual, 0011b talkgroup; Service_Options table 6.50: EMERG, Privacy,
SUPED_SV, HI_RATE, SIMI, priority):

1. Checks as a call (registered or an implicit record, `ms`, `tg`). The called party: an MS, a talkgroup, or the
   site: TSI, the `data_gateway` ID, or IPI with no extended address to upload (the IP destination is the datagram's:
   6.6.3.3, "derived from the DLL address"). Other gateways (multi-part addressing, 6.6.3.2.3) get Not_Supported;
   an MS not registered here goes to the network (5.10) when the site is connected and the caller's `wide_private`
   allows, else NoregMSaway; one in a call or a data call,
   Called_Party_Busy; itself, Not_Supported. A repeat of a request being set up gets the same answer again.
2. An individual call to an MS: the availability check (6.6.3.1.4), C_AHOY (Service_Options_Mirror, Service_Kind
   0010b, source the caller, target the called), repeated every `ahoy_every_s` up to `ahoy_tries`; the caller hears
   C_WACKD every 2 s. C_ACKU(Message_Accepted) leads on (6.6.3.1.4, 6.6.3.2.6), and so does MS_Accepted (the same
   answer with the MS-sent direction bit, table 7.42); C_ACKU(CallBack) is mirrored to the caller with no channel,
   as for voice (6.6.2.2.6); another C_ACKU (MS_Alerting) doesn't accept a data call, so the check runs on to no
   answer; C_NACKU is mirrored; no answer, MSaway.
3. A channel: PD_GRANT (individual) or TD_GRANT (talkgroup), opcodes 51/52, or 55/56 when SIMI asked for a
   multi-item call (7.1.1.1.1.4, 7.1.1.1.1.5), sent twice like the voice grants; HI_RATE is always 0 (single slot:
   dual-slot data isn't built, a HI_RATE request gets a single-slot grant). None free: C_QACKD(Queued_for_Resource)
   every 4 s; queued data calls take the channels queued voice leaves (voice first), `queue_s` at most, then
   SYSbusy. A channel already open between the same parties is granted again.
4. On the channel (361-1 9.2, 361-3): the site repeats every data header and block (data types 6, 7, 8, 10),
   regenerated with the BS sync; voice LC and late entry don't apply. The radio layer counts each message (its
   header says how many blocks follow) for the controller, which holds the channel `data_hang_s` after any burst.
5. Clearing (6.6.3.3.1.6 b): an unconfirmed single-item message ends the call once repeated (P_CLEAR about 0.5 s
   after its last block); a confirmed one, `data_hang_s` after the response (the called MS's ACK goes back through
   the repeat); a multi-item call, `data_hang_s` after its last burst, or at once on P_MAINT DISCON; any data call
   at `call_s` (T_PACKET_TIMER).

The site as a party: data to TSI, the data ID or IPI goes, burst by burst, to the unit's `data` hook (rdofradio:
the IP gateway app, when the document has one: [ipgw.md](ipgw.md)). What the gateway
sends (its replies, the ACK of a confirmed message) goes down through `rdof_t3unit_data_tx`: on the open channel at
once, else the site asks for one (`rdof_t3_pd_send`: the availability check to an MS, or TD_GRANT for a talkgroup)
and sends it when granted, source the site's ID; 30 s at most, one packet at a time. A packet to an MS not
registered here goes to the network as a `data_pdu` (5.10) when the site is connected; the site that has the MS
delivers it.

The MS simulator's side of the data (`rust/src/t3pdrate.rs`, 361-1 8.2.2, 9.2; 361-3 5.3.1, 5.4): a message in
blocks of one rate, rate 1/2 (data type 7: 12 octets a block, BPTC), rate 3/4 (8: 18, the B.2 trellis) or rate 1
(10: 24, uncoded); unconfirmed the block is all user data, confirmed its first two octets are the 7-bit serial
number and the CRC-9 (B.3.10, the rate's mask of table B.21: 0F0h, 1FFh, 10Fh), the rest data (10, 16, 22 octets);
the last block ends with the message's CRC-32 (B.3.9). The header's Blocks to Follow, pad octet count and F as 9.2.1
/ 9.2.2. Up to 127 blocks a message, the most Blocks to Follow's 7 bits allow (9.3.21): 1520 / 1266 octets at rate
1/2, 2282 / 2028 at rate 3/4, 3044 / 2790 at rate 1, unconfirmed / confirmed. Taking a confirmed message with A set
(361-3 5.4.2.2), the MS answers C_RHEAD, NI its N(S) (stop and wait): ACK (class 00b, type 001b) when every block's
CRC-9 and the CRC-32 hold; NACK (01b, 001b) when the blocks hold and the CRC-32 doesn't; SACK (10b) with C_RDATA
blocks (rate 1/2: 8 octets of retry flags, block j in octet j / 8 bit j % 8, 0 to retry, then a CRC-32; 9.2.5,
figure 8.17) when a block is bad or missing, and stays on the channel. A SACK has one C_RDATA for a message of up to
64 blocks, two for more (8.2.2.3: "flags for up to 127 blocks"), its header's Blocks to Follow 1 or 2; the sender
takes both before it sends again. 361-1 doesn't say how two split the flags or what the second's CRC covers. Choice:
the second holds blocks 64 to 126, each its own CRC-32 over its 8 flag octets (table 9.14 gives every C_RDATA a
CRC); a second whose CRC covers both blocks' 16 flag octets is taken too. The masks are two u64 (a u128 field would
make the site's, the unit's and the MSs' structs 16-aligned; their C callers give 8). The sender sends a SACK's
blocks again after a C_HEAD with F = 0, Blocks to Follow their count and the same N(S), and all of them (F = 1)
after the NACK; the receiver places each block by its serial number. Choices: N_RtryLmt (no value in 361-3) is 4
sends; with no response in 3 s (T_RspnsWait) the sender gives up rather than sending again; a C_RDATA whose CRC
fails is ignored (the sender then times out); another NACK ends the call. The site repeats every rate alike (data
types 7, 8, 10 regenerated); its own IP gateway's handling of the rates is [ipgw.md](ipgw.md)'s.

Choices where 361-4 leaves them open: the TS doesn't poll MSs on the channel (P_AHOY radio check, 6.6.3.3.1.1) and
doesn't share a data channel between calls; supplementary data (SUPED_SV) is abandoned (6.6.3.2.2 a); a new data
call to an MS on a data channel waits as busy rather than joining it.

Not built: multi-part calls (extended addresses through PSTNI, PABXI, LINEI, DISPATI, IPI with an upload),
supplementary data, dual-slot data, the TS's own P_AHOY radio check and selective clear.

### 3.17 Talkgroup lists: subscription by upload and DGNA (361-4 6.4.4.1.13.2, 6.6.8)

The code is `rust/src/t3tglist.rs` (what the lists leave) on top of the short data transactions of 3.15 (the upload,
the download, the final acknowledgement); the MS side is in `t3ms`. Both carry the Address Format (B.3.2): octet 0
reserved and the One-key flag, then 24-bit addresses, 3 in one block, 7 in two, 11 in three, 15 in four, the
unused ones ADRNULL.

Registration with a list (6.4.4.1.13.2, figure 6.25): C_RAND Registration, TRGT_ADR_CNTS 10b, target TATTSI,
Appended_UDT_Short_Data the blocks less one.

1. The registration is checked as any (3.4); refused, C_NACKD(Reg_Denied or Reg_Refused) from TATTSI.
2. Accepted: C_AHOY (table 6.11: Service_Options_Mirror, Service_Kind_Flag 1, ALS 0, G/I 0, Appended_Blocks as
   asked, Service_Kind 1110b, source TATTSI), then the inbound slots kept for the upload as for short data (C_ALOHA
   mask 24, AT busy). The MS sends C_UDTHU (target TATTSI, A = 0, SF = 1, UDT_Format 0001b) and the blocks.
3. Each of ADDRESS1-7 is checked against `tg` (not a gateway or an all-call). C_ACKD once, source TATTSI:
   Reg_Accepted with Response_Info 1111111b when every address sent (ADRNULL aside) is taken ("This is true
   regardless of the number of Subscription/Attachment TGs the MS has sent"), else 0110 0101b (registration accepted,
   subscription) with one bit per address, ADDRESS1 the MSB (table 6.10), 1 accepted, 0 refused. An ADRNULL's bit is
   0 ("For an address where MS sent an ADRNULL the response shall be 0"), and so is the bit of a position the upload
   doesn't carry (a one-block list holds three; the text doesn't say, 0 matches the ADRNULL rule). The clause allows
   Reg_Accepted or 0110 0101b with 1111111b for all accepted; the site sends Reg_Accepted, the reason 6.4.4.1.2
   names as success. Its 1111111b is the pattern 6.4.4.1.13.2 asks of either reason; table 7.41's PowerSave_Offset
   for Reg_Accepted applies to a request that invokes power save, which a list registration can't (PS_RQ 000b).
4. The list replaces the MS's last list and its single subscription; a single subscription (01b) or ADRNULL
   replaces the list; deregistration or the record's expiry drops it. No upload (`ahoy_tries` asks, `tnp_s` at
   most) or a refused one: the registration stands, Response_Info 0 (the list refused).

DGNA (6.6.8, figure 6.67), only to an individual MS:

1. From an MS: C_RAND Service_Kind 1101b to DGNAI, SUPED_VAL the blocks less one. Without `dgna_from_ms`,
   C_NACKD(Not_Supported) from DGNAI. Else C_AHOY from DGNAI (Service_Kind 1101b, 6.6.8.2.1 c), the upload slots,
   and C_DGNAHU (opcode 37, target the called MS, A = 1, UDT_Format 0001b DGNA_Address or 1010b DGNA_Alias).
2. Checked: the called party an MS (not a talkgroup, a gateway, the site or the caller: Not_Supported), every
   address ADRNULL or in `tg` (else Called_Group_Not_Allowed), another format Not_Supported, a stunned called MS
   MSaway_Refused (6.4.9.0: it may not receive user services). Then as UDT short data
   to an MS (3.15): C_WACKD to the caller, stored while the called MS is busy, C_DGNAHD (opcode 36, table 6.69,
   A = 1, the blocks as they came) repeated up to `sd_tries`, the called MS's C_ACKU mirrored in C_ACKD to the
   caller (MS_Accepted, 6.6.8.2.3); not registered here, through the network (`sdata`, what 4) or NoregMSaway.
3. From the site (`rdof_t3_sd_send`, what `RDOF_T3_SD_DGNA`): only the outbound phase (6.6.8.3.1), source DGNAI
   unless the app names one; the result comes as an EV_SD result.
4. When the called MS takes it (MS_Accepted), the site keeps what it now holds: DGNA_Address replaces the fifteen,
   DGNA_Alias sets the sixteenth unless its address is one of the fifteen (6.6.8.1); all ADRNULL clears them.

What the site keeps: a table of 64 MSs (the list's 7 and DGNA's 16 talkgroups each), apart from the registration
record (its format and the network's records don't change). The site carries every talkgroup a registered MS
holds in either (its network subscriptions, 5.5). Full table: a new list is refused (0110 0101b, Response_Info
000 0000b: "The TSCC may accept the registration but send Response_Info = 000 0000"), a DGNA
result isn't kept (logged). An MS that roams re-registers with its list at the new site; its dynamic talkgroups
aren't carried with its record (1.2).

Choices where 361-4 leaves them open: the registration is validated before the list is asked for; the final C_ACK's
source is TATTSI, as the refusal's; the One-key flag and alias text pass through unread.

### 3.18 Supplementary services: stun/revive/kill, ambient listening, call diversion (361-4 6.4.9, 6.4.10, 6.6.2.6, 6.6.7)

The code is `rust/src/t3supp.rs` (a child of t3site; the diversion upload and the notice go through the short data
transactions of 3.15); the MS side is in `t3ms`. Results come as `EV_ACTIVITY` kind `ACT_SUPP` (7): op 1 stun,
2 revive, 3 divert, 4 divert cleared, 5 kill; src the MS, dst STUNI, KILLI or the address, reason the air reason
(an ACK reason done: 0x60 or 0x44; 0x14 the MS refused the site's authentication response; 0x20 no key to answer
its challenge; 0x25 no answer; the MS's NACK reason otherwise).

Stun, revive and kill, from the site only (the `stun` / `revive` / `kill` commands, `rdof_t3_command`; in 361-4 only
the TSCC's gateways STUNI and KILLI do it):

1. C_AHOY (table 6.19): Service_Options_Mirror 0, Service_Kind_Flag 0 stun / 1 revive, ALS 0, G/I 0, no blocks,
   Service_Kind 1101b, target the MS, source STUNI. Kill (table 6.23): the same with Service_Kind_Flag 0, source
   KILLI. Sent every `ahoy_every_ms` up to `ahoy_tries` times, AT set.
2. Without authentication (6.4.9.1): C_ACKU to STUNI with any ACK reason (Message_Accepted in the text, 0x60;
   MS_Accepted, 0x44, taken too): done. C_NACKU (MSNot_Supported, or any refusal): reported, nothing kept. No
   answer: reported as MSaway_Refused.
3. With authentication (6.4.9.2, 6.4.10; kill always): the MS answers C_ACKVIT (opcode 30, table 7.26;
   tables 6.21 / 6.25: Service_Options_Mirror 0, the C_AHOY's Service_Kind_Flag, Service_Kind 1101b, target its
   challenge, source the MS). The site computes the response with the registration's algorithm (3.14: RC4 keyed
   with the challenge and the MS's K, XORed with its PSN when provisioned) and sends C_ACKD (tables 6.20 / 6.24):
   Response_Info as any C_ACKD (G/I 0, Response_Check), reason Authentication_Response 0110 0100b, target the MS,
   source the response; AT set. It repeats that C_ACKD every `ahoy_every_ms` up to `ahoy_tries` times while no
   final answer comes (6.4.9.2.0 c: "The TSCC may repeat step C"); a repeated C_ACKVIT gets the response at once.
   The final answer, to STUNI or KILLI: C_ACKU (MS_Accepted per tables 6.22 / 6.26; Message_Accepted per the
   text; both taken) done; C_NACKU(Recipient_Refused) the MS refused the response; none: MSaway_Refused (for a
   kill, logged as "may be killed", 6.4.10.0 note). No key for the MS at the site: no C_ACKD, the job ends
   (Not_Supported). `kill` needs a key for the MS (false without one).
4. The site keeps a stunned MS (32 at most) until revived. A stunned MS "may not request nor receive any user
   initiated services" (6.4.9): its requests get C_NACKD(Perm_User_Refused), cancel excepted; individual calls,
   short data, statuses, packet data and DGNA from an MS to it get MSaway_Refused (it wouldn't answer the check).
   Registration stays. The state is in RAM only: a restart forgets it (the MS stays stunned until revived).
5. A killed MS's registration record is removed (deregistration) and it is no longer kept stunned.

Ambient Listening Service (6.6.2.6), only with `ambient_listening`:

1. C_RAND individual voice with ALS_SERV 1 (table 6.40: the dependent bits are SUPED_VAL, ALS_SERV, reserved).
   Without the setting, C_NACKD(Not_Supported). The called MS not at this site: Not_Supported (not carried across).
2. As an OACSU individual call (3.5), but the radio check is table 6.46: Service_Options_Mirror 0, Service_Kind_Flag
   0, ALS_SERV 1, Service_Kind 0000b, source the caller. C_ACKU(Message_Accepted) goes on to the grant;
   C_NACKU(MSNot_Supported) is mirrored to the caller. The grant carries nothing of ALS (6.6.2.6.2 note).
3. On the payload channel the called MS transmits for T_ALS (an MS timer) and ends with P_MAINT DISCON; the site
   clears the call as any. The caller may end it sooner with status 123 to the called MS (6.4.14.3.4) when
   `services.transmit_interrupt` is on (section 3.20); off, it gets C_NACKD(Not_Supported) from TSI ("If Transmit
   Interrupt is not available the TSCC shall respond C_NACKD (Source = TSI, Target = Interrupting MS)",
   6.4.14.3.4.2), and the call goes on. The called MS resuming by itself after a payload interrupt (6.6.2.6.6) isn't
   built.

Call diversion (6.6.7), on unless `call_diversion` is false (then C_NACKD(Not_Supported) from the C_RAND's target):

1. Set (DIVONOFF 1, table 6.61): C_RAND 1000b to MSI (to an MS) or GPI (to a talkgroup), Divert Kind the services
   (voice, packet data, UDT short data, status), SDATA_VAL 00b. C_AHOY from DIVERTI (Service_Kind 1000b,
   Service_Options_Mirror the C_RAND's, Appended_Blocks 00b, or 01b when the C_RAND's Proxy Flag is 1:
   6.6.7.1.1.1), the upload slots, C_UDTHU (table 6.63: target MSI or GPI, the Address Format,
   A 0, SF 0), then C_ACKD from the C_RAND's target (table 6.60): Message_Accepted, or Transient_Sys_Refused for
   an address that can't be (the MS itself, a gateway, an all-call, a talkgroup outside `tg`, another format).
   PSTNI, PABXI, LINEI, DISPATI and IPI get Transient_Sys_Refused at once: no such gateway here.
2. Clear (DIVONOFF 0, single part): to TSI clears the MS's own diversions of the kinds given; to DIVERTI clears the
   other MSs' diversions to this MS (table 6.64; talkgroup diversions only their MS clears). C_ACKD(Message_Accepted)
   from TSI or DIVERTI.
3. Diverting (6.6.7.2): an individual voice call (not ALS), packet data, UDT short data or status request to a
   diverted MS gets a UDT header instead (C_UDTHD: SF 1, A 0, target the caller, source MSI or GPI by table 6.65,
   the Address Format with the diverted address, One Key flag 0: 7.2.36 gives it to DGNA only), sent twice; the
   request ends there. A UDT poll gets
   C_NACKD(Div_Cause_Fail) (6.6.5.1.1). A caller that is the diverted address gets the call as usual.
4. The site keeps 32 MSs' diversions, one address per kind, in RAM, apart from registration: they last until
   cleared (or a restart), registered or not. Requests from the network to a diverted MS aren't diverted.

Choices where 361-4 leaves them open or contradicts itself (checked against the text on 2026-11-13):

- The final acknowledgement of a stun, revive or kill: the figure text and 6.4.9.1 / 6.4.9.2.1 say
  Message_Accepted (0x60, whose direction bit means "TS to MS"), tables 6.22 / 6.26 say MS_Accepted (0x44). The site
  takes any ACK reason; its simulator sends Message_Accepted without authentication (6.4.9.1.2 has no table) and
  MS_Accepted with it (the tables). 6.4.9.2.0 c) also names Authentication_Response for that answer; taken too.
- The response to the MS's challenge: 361-4 doesn't say how the TSCC computes it. The site uses the registration's
  algorithm (6.4.8, figure 6.30) with the MS's K, and its PSN when one is provisioned (figure 6.30's XOR). With no
  PSN both readings agree.
- An MS's C_ACKVIT with a challenge outside 000000h-FFFCDFh is answered anyway.
- The diversion notice's source is MSI or GPI (table 6.65), though the call procedures say DIVERTI (6.6.2.2.5 and
  others). A status diversion to a talkgroup is kept, though table 6.65 has no talkgroup row for status.
- The diversion upload's header target: MSI or GPI (tables 6.59, 6.63) or DIVERTI (table 6.35): the site takes the
  C_RAND's target and ignores the header's.
- DGNA from an MS: the acknowledgements after the upload (C_WACKD / C_QACKD, the mirrored final C_ACKD, refusals)
  come from the called MS's address (table 7.23: "the request's destination"); before the upload, from DGNAI. The
  text doesn't say which; diversion's final acknowledgement comes from the C_RAND's target (6.6.7.1.1.2).
- A stunned MS's requests get Perm_User_Refused; requests to it MSaway_Refused; ALS is off by default, diversion on.
- The ALS termination: the interrupting MS "shall then return to the payload channel in either case"
  (6.4.14.3.4.1) but "shall remain on the TSCC" (6.4.14.3.4.3). The simulated MS stays on the TSCC; the site doesn't
  care (section 3.20).

### 3.19 The composite TSCC (361-4 5.3.2)

`services.composite_tscc` (off by default) makes the TSCC composite: "A composite TSCC may suspend its control
channel function and revert to a payload mode" when "all other payload channels within a particular TS are busy"
(5.3.0, 5.3.2). It is for a site with few channels: one carrier (TS1 the TSCC, TS2 the only payload channel), such
as a hotspot modem, gets a second call at once.

- When a call finds no payload channel free (a request, the queue, a call from the network, packet data), the
  controller gives it the TSCC's own slot, if the TSCC isn't mid-way through an MBC or a short data transfer. The
  grant (TV_GRANT, PV_GRANT, a data grant) names the TSCC's physical channel and LCN 0. It goes on the TSCC twice,
  ahead of anything queued; then the slot is the call's. Pre-emption (section 3.9) still applies; a free payload
  channel is always used first.
- While the slot carries the call, the TSCC sends nothing: no C_ALOHA, no answers, no broadcasts. Random access,
  registration and short data wait (5.3.2 a), b)). Blocks heard on the slot go to the call (P_MAINT); a C_RAND there
  isn't answered. The CACH on the TSCC's carrier sends P_SYS_Parms with Payload Channel Type 1, "Payload Channel is
  Composite Control Channel" (7.1.2.2, table 7.34; 7.2.34), in place of C_SYS_Parms (7.1.2.1: "never transmitted
  when neither of the timeslots of the physical channel are a TSCC"). The other carriers keep Payload Channel Type
  0. The AT bit for TS1 says whether the call's inbound is in use.
- The call runs and clears as any other: hang time, call timer, P_MAINT DISCON, then P_CLEAR on the slot (channel 0:
  back to the control channel). Then the TSCC returns ("When the payload call is completed, the channel returns to
  its control channel function", 5.3.2 b)) with a C_ALOHA first. What it had queued before or during the call
  (grants for other calls, answers) is dropped as stale; MSs ask again.
- The MS simulator: an idle MS whose TSCC's CACH says P_SYS_Parms with Payload Channel Type 1 waits for the TSCC
  rather than hunting; C_SYS_Parms ends that (`rdof_t3ms_slc`, fed by the air paths from `rdof_t3unit_slc`).
- The radio layer: with a TSCC, channel 0 is also a payload channel (`rdof_t3rf_t.composite`), opened only when
  the controller opens it. Each slot the unit sets `rev` from the controller's `rdof_t3_slot_out_t.rev`: 1, channel
  0's outbound comes from its payload path (warm-up, repeat, P_CLEAR) and its inbound goes to it, not to the TSCC;
  2, while the TSCC still sends the grant, the outbound is the TSCC's but the inbound is already the call's. The MS
  that asked sends its voice header as soon as it reads the first grant: on the hat (2026-11-13) that header went to
  the TSCC and was lost, and the call aired without one.

Not built: an MS that reads the short LC on air (the simulator's air MSs take it from the unit), and the 5.3.2 note
that the throughput "shall be assessed" (no metric beyond the log line and the slot counts).

### 3.20 Transmit interrupt (361-4 6.4.14, 6.4.15)

`services.transmit_interrupt` (off by default) turns it on. 6.4.14.0: "If a Tier III TS or MS supports this feature,
the feature shall be implemented as described in this clause and in clause 6.4.15": optional, so off by default, and
off a request gets C_NACKD(Not_Supported) from TSI as 6.4.14.3.3.1 and 6.4.14.3.4.2 ask. On, the site serves the
three de-key cases a TS starts besides the emergency pre-emption (3.9, 6.4.14.3.1); the code is
`rust/src/t3intr.rs`.

What 361-4 asks of the TS, and what is built:

| Clause | The TS | Built |
|---|---|---|
| 6.4.14.3.3.1 | Match a payload interrupt (status 125 to a talkgroup, or to an MS ID: "matched with the MS IDs engaged in voice calls on the payload channels") with a call; answer C_ACKD(Message_Accepted) or C_NACKD (Not_Supported, Perm_User_Refused) from TSI to the interrupting MS; de-key the talker | yes |
| 6.4.14.3.4.2 | ALS termination: status 123 from the ALS caller to the called MS; answer from TSI; de-key the called MS; "Following the cessation of inbound PDUs, the payload channel shall send additional P_Clear PDUs" | yes |
| 6.4.14.3.2 | Call timer expiration: "If a party is transmitting the TS shall use the Reverse Channel Signalling ... 0100b"; then P_Clear | yes, under the same setting |
| 6.4.15.1 | The de-key: RC 0100b (command) or 0101b (request, the MS decides); "If the MS continues to transmit the TS shall continue to service the ongoing voice call"; may send P_PROTECT(EN_PTT_ONE_MS) to the interrupter, limited in number | 0100b; the call goes on; P_PROTECT, at most 17 |
| 6.4.14.3.3, 6.4.14.3.4 | The same, started by a gateway (a line connected party, a dispatcher) | no: no gateways (1.2) |
| 6.4.14.2 | Power control in a de-key | no: the site sends no power control |

The payload interrupt (6.4.14.3.3):

1. A listening MS leaves the payload channel and sends a status call with status 125 (the Status Transport Service,
   3.10), G/I 1 to the talkgroup, G/I 0 to an MS: either party of an individual call, or the talker of a group call.
2. The TSCC finds the voice call: the talkgroup's (a group call, an all-call or a broadcast call), or an individual
   call the target is a party of (a duplex or data call isn't one), or a group call the target talks in. It answers
   from TSI:
   - no such call, a call being cleared or de-keyed for something else, a network talker while the network link is
     down, or the interrupter itself talking: C_NACKD(Not_Supported);
   - an all-call or a broadcast call (one way: its parties don't transmit, 6.6.2), an individual call it isn't a
     party of, a talkgroup outside `tg`, or another MS's interrupt of the call under way: C_NACKD(Perm_User_Refused);
   - else C_ACKD(Message_Accepted). A repeat request is answered again.
3. The MS goes back to the payload channel whatever the answer (6.4.14.3.3).
4. A talker on RF gets the RC cease command (0100b, table 6.32) on the other slot of its carrier every 360 ms, as
   in 3.9 (the TSCC's block gives way when that slot is the TSCC). The MS finishes its superframe, sends a
   terminator and stops (6.4.15.2).
5. Once it stops (or with nobody talking: an interrupt in hang time), the channel sends
   P_PROTECT(EN_PTT_ONE_MS) (table 7.31: Protect_Kind 011b, G/I 0, target the interrupter, source TSI) in place of
   every other hang burst of its slot, for up to 2 s (at most 17 PDUs; the hang lasts at least that long). The
   addressed MS "shall enable the PTT"; the others "shall disable the PTT" (6.6.2.3.1.3). It stops when anyone
   talks; the interrupter's talk is the call's next talk, under the call's own timer.
6. A talker that doesn't stop within 3 s keeps the call; the interrupt ends there.

A talker at another site (the call's voice comes from the network): the site accepts as above, sends no RC, and
asks the talker's site with an `sdata` interrupt message (5.9) to the talker. That site finds the talker in a call
with that destination on one of its channels, talking on RF, and de-keys it as in step 4 (the setting on there too),
then sends P_PROTECT to the interrupter on its channel as in step 5, so its own MSs keep off; it answers
Message_Accepted, or Not_Supported (the setting off, the talker not talking in that call) or Perm_User_Refused
(being cleared, or another interrupt under way). Here, once the stream ends the channel sends P_PROTECT to the
interrupter (step 5), and the interrupter's talk goes to the other site as any talk. A refusal, or a stream that
goes on for 4.5 s (3 s plus the network's trips), ends the interrupt: the call goes on. With the link down a network
talker can't be reached: Not_Supported. Not a duplex call's (one isn't across sites).
ALS termination (6.4.14.3.4): status 123 to the called MS of an ALS call (3.18), from its caller. A group G/I or no
such call: Not_Supported; another MS: Perm_User_Refused. Accepted, the called MS is de-keyed as above, then the call is
cleared (P_CLEAR, close reason 7, `CLOSE_INTR`); one that doesn't stop within 3 s is cleared anyway. Not transmitting
at the time: cleared at once.

Call timer (6.4.14.3.2): with the setting on, a call whose timer runs out while an MS talks on RF gets the RC cease
first and P_CLEAR once the inbound stops (after 3 s P_CLEAR regardless, the channel held until the inbound stops); off, P_CLEAR at once as before. A network talker, a duplex
call or a call being pre-empted is cleared at once.

Each step is an `EV_ACTIVITY` event of kind `ACT_INTR` (8; op 1 accepted, 2 the talker ceased, 3 it didn't, 4 the
interrupter talks; src the interrupter, dst the call's destination) and a log line. mqtt-net's `activity` doesn't
carry them yet.

The simulated MS (`RDOF_T3MS_DO_INTERRUPT`, `Ms::interrupt`, `Ms::als_end`; rdofradio scripts `interrupt`,
`ginterrupt`, `als`, `alsend`): listening on a payload channel when the action is due, it goes to the TSCC, makes the status
call, takes the answer only from TSI, and goes back to the channel; accepted, it waits up to 6 s for
P_PROTECT(EN_PTT_ONE_MS) to it and then talks to the talkgroup or the other party (no P_MAINT DISCON after: it isn't
its call). It counts P_PROTECTs to other MSs (its PTT is disabled meanwhile; it doesn't key there anyway). An ALS
caller stays on the TSCC.

Choices where 361-4 leaves them open:

- The cease command (0100b), not the request (0101b): the interrupter asked for the channel and the TSCC accepted.
- P_PROTECT from TSI (table 7.31 doesn't say), only after the talker stopped (the text allows it alongside the RC),
  and every other burst for 2 s ("limit the number"). The capture of a commercial site at hand has no
  EN_PTT_ONE_MS, only ILLEGALLY_PARKED.
- A call that can't be matched gets Not_Supported, a call the MS may not interrupt Perm_User_Refused (6.4.14.3.3.2
  names both without saying when).
- Any party may interrupt; nothing is reserved to a priority. An emergency call may be interrupted: the talker's
  own radio decides by obeying the RC.
- A network talker is de-keyed by its own site, which the interrupter's site asks over mqtt-net: the site that
  sends the RC must be the one the talker hears. The interrupter's site answers C_ACKD at once, as for a local
  talker, rather than holding the MS for the other site's answer: a de-key isn't sure either way (6.4.15.1).
- The call timer de-key goes with the same setting: it is in 6.4.14 and needs the RC as the rest.
- The ALS termination's MS "shall then return to the payload channel" (6.4.14.3.4.1) but "shall remain on the TSCC"
  (6.4.14.3.4.3): the site doesn't depend on it.

Not built: the gateway-started forms, power control in a de-key (6.4.14.2), the ALS
called MS resuming after an interrupt (6.6.2.6.6), ALS termination across sites (its called MS at another site:
Not_Supported), and mqtt-net events. Not run on a radio.

## 4. Settings (config-v3 `tier3` app)

The app takes the place reserved for it in config-v3.md section 6 ("a control frequency and a pool; it assigns its own
paths").

```json
{
  "frequencies": [
    {"name": "A-out", "freq": 438.75, "devices": ["pluto"]},
    {"name": "A-in",  "freq": 438.25, "devices": ["pluto"], "tx": false},
    {"name": "B-out", "freq": 438.6,  "devices": ["radio1"]},
    {"name": "B-in",  "freq": 438.5,  "devices": ["pluto"], "tx": false}
  ],
  "apps": [
    {"name": "site", "type": "tier3",
     "system": {"model": "small", "net": 5, "site": 3, "par": "ab"},
     "carriers": [
       {"out": "A-out", "in": "A-in", "chan": 1, "tscc": true},
       {"out": "B-out", "in": "B-in", "chan": 2}
     ],
     "registration": "open",
     "ms": "*",
     "private": "*",
     "tg": "1-999999",
     "wide_tg": "1-99999",
     "foacsu": false,
     "timers": {"hang_s": 3}}
  ]
}
```

| Key | Type | Default | Meaning |
|---|---|---|---|
| `system` | object | | `model` (`tiny`/`small`/`large`/`huge`), `net`, `site` within the model's ranges (table 1.5 of the digest: tiny 9/3 bits ... huge 2/10), `par` (`a`, `b`, `ab`), `area_bits` (1..SITE bits, default all). The manager's `cfg` overrides it |
| `carriers` | array, 1-4 | | each: `out`, `in` (frequency names), `chan` (1-4094, the logical channel number in grants), `tscc` (exactly one carrier) |
| `registration` | `open`, `listed` | `open` | |
| `ms` | ID set | `*` | IDs that may register (with `listed`) and be served |
| `require_registration` | bool | true | Reg = 1 |
| `refuse_unlisted` | `denied`, `refused` | `denied` | |
| `private` | ID set | `*` | IDs that may make individual calls |
| `tg` | ID set | `*` | talkgroups usable at the site |
| `wide_tg` | ID set | `""` | talkgroups carried on the network (wide area); others stay local |
| `static_tg` | ID set | `""` | talkgroups the site always subscribes to on the network (as a Tier II repeater's `tg`) |
| `foacsu` | bool | false | full off-air call setup for individual calls |
| `dgna_from_ms` | bool | false | MSs may assign talkgroups to other MSs (DGNA, 3.17); off: Not_Supported. The site's own DGNA always goes |
| `ambient_listening` | bool | false | MSs may ask for the Ambient Listening Service (3.18); off: Not_Supported |
| `call_diversion` | bool | true | MSs may divert their calls to an MS or a talkgroup (3.18); off: Not_Supported |
| `data_gateway` | integer | 0 | the site's own data ID besides TSI (1-16776415): packet data to it, to TSI or to IPI goes to the site's IP gateway (3.16) |
| `timers` | object | section 3.8 | |
| `broadcast` | object | section 3.2 | `adjacent_s`, `chan_freq_s`, `local_time_s`, `call_timers_s`, `vote_now_s`, `ann_tscc_s` (0 = off, else 1-600 s; the last three off by default, section 3.11); `vote_blk` (2-10 TDMA frames, 6): VOTE_BLK after a Vote_Now; `priority` (0-7, 0): the site's priority for the Radiated Preference Strategy (table 6.81: 1 highest, 7 lowest, 0 non-preferred), sent as the confirmed priority with each neighbor and in `info` (section 3.11). Local settings: the manager's `cfg` doesn't carry them |
| `services` | object | | `emergency_preempt` (bool, false): clear a running call for a waiting emergency (section 3.9); `power_save` (0-7, 0): the largest PowerSave_RQ granted, 0 none; `power_save_awake_s` (0.1-60, 1); `power_save_group_hold_s` (0-10, 2): how long a group call waits for its sleeping members' wake frame (section 3.11); `mass_reg_at_start_s` (0-200000, 0): a mass re-registration at start within this time (section 3.11); `hibernate_s` (0 or 1-300, 0): T_BS_Inactive of the asynchronous TSCC, 0 never (section 3.12); `duplex` (bool, false): full-duplex individual calls (section 3.13); `composite_tscc` (bool, false): the TSCC's slot carries a call when no payload channel is free (section 3.19); `transmit_interrupt` (bool, false): payload interrupt, ALS termination and the call timer de-key (section 3.20). Local settings only for now; the manager's `cfg` doesn't carry them |
| `auth` | `off`, `registration`, `required` | `off` | MS authentication (section 3.14): challenge MSs with a key at registration, or every MS. Local settings only; the manager's `cfg` doesn't carry it |

Checks (refusals name the key): exactly one `tscc` carrier; every carrier's `out` on a device that transmits and its
`in` on a device that receives; the TSCC carrier's `in` and `out` on one full-duplex SDR (a split TX half
can't carry the TSCC yet: radiofoundry-handoff.md 1.4); every carrier's `in` on that SDR; another carrier's `out` on
an SDR or a split TX half (one carrier a radio). Carrier B out of the TSCC's SDR needs its `tx.carriers` at 2, which a
host's I/Q driver (`iio`, `soapy`, `sim`) takes when the site is all that SDR sends (the board's drivers send one);
rdofradio then wants both outs within its transmit stream's reach of one TX LO (9.4); `chan` unique; the `system` numbers inside the model's ranges; at most 4 carriers;
a frequency used by a `tier3` app can't be used by another app or a talk path.

Paths: the app offers one talk path, named after the app, with `trunk: true` in `info`. The slots are internal: the
network never addresses a slot, only the site.

## 5. The MQTT network (mqtt-net) for Tier III

mqtt-net format 3 is a draft, not released; the additions below go into it (docs/spec/fw/mqtt-net.md), not into a
format 4. Tier II nodes are unchanged except where marked "all nodes".

### 5.1 What is shared, what is added

| Concern | Tier II (today) | Tier III | Shared |
|---|---|---|---|
| Call messages | `tg/{tg}/{node}`, `pc/{dst}/{node}`, `dl/{node}` | the same; a trunk site publishes its payload channels' calls as any box does | yes, all of it |
| Which talkgroups reach a node | its paths' `tg` sets + ptt-held | its registered MSs' talkgroups + `static_tg` | yes: one subscription mechanism (`rdof_mn_sub_tgs`), a new source of talkgroups |
| Private call location | the router's last heard (passive) | registration records (explicit) | yes: one router table, two sources |
| Private call setup | none (voice just goes) | availability check over the network | the setup item; Tier II nodes answer it at once (section 5.4) |
| Node presence and description | `online`, `info` | `info` gains `trunk` | yes |
| Status | (legacy topic) | `node/{node}/status` with `trunk` | yes: the topic is defined for all nodes |
| Activity events | call_start, call_end, csbk, data ... | adds `reg`, `grant`, `queue`, `setup` | yes |
| Provisioning | none | `cfg/...` | yes: the topics are per node and per network, any node may read them |
| Commands | `node/{node}/cmd` (undefined) | defined (section 5.7) | yes |

### 5.2 Topics added

| Topic | Payload | QoS, retained | Publisher | Readers |
|---|---|---|---|---|
| `{root}/node/{node}/ms/{id}` | JSON registration record; empty payload deletes | 1, yes | the site holding the registration | router, dashboard, the site itself after a restart |
| `{root}/node/{node}/status` | JSON, every 10 s and on change | 1, yes | any node | manager, dashboard |
| `{root}/node/{node}/cmd` | JSON command | 1, no | manager | that node |
| `{root}/node/{node}/cmd/res` | JSON result | 1, no | that node | manager |
| `{root}/cfg/net` | JSON network provisioning | 1, yes | manager | every node |
| `{root}/cfg/node/{node}` | JSON node provisioning | 1, yes | manager | that node |
| `{root}/cfg/tg/{tg}` | JSON talkgroup definition | 1, yes | manager | nodes, router, dashboard |
| `{root}/cfg/ms/{id}` | JSON radio definition | 1, yes | manager | nodes, router, dashboard |
| `{root}/manager/{id}/online` | `1` / `0` (will) | 1, yes | manager | nodes, dashboard |
| `{root}/where/{id}` | (as today) gains `source`: `reg` or `heard` | 1, yes | router | sites (to drop a record a newer one replaced), dashboard |

Subscriptions added: a trunk site subscribes `cfg/net`, `cfg/node/{node}`, `cfg/tg/+`, `cfg/ms/+`,
`node/{node}/ms/+` (its own records, read back at start), `node/{node}/cmd`, and `where/+`. The router adds
`node/+/ms/+` and `cfg/ms/+`.

ACLs added: only the manager publishes `cfg/#` and `node/+/cmd`; a node publishes `node/{node}/#` (including its `ms`
records and `cmd/res`) and nothing under another node.

### 5.3 Registration record (`node/{node}/ms/{id}`)

```json
{"id": 3100123, "at": "2026-11-05T21:04:11.250Z", "exp": "2026-11-06T01:04:11Z",
 "how": "register", "tg": [91, 3100], "sub": 91, "area": 3}
```

- `how`: `register` (explicit), `call` (implicit, from a call request).
- `tg`: talkgroups the site will carry for it (subscription first, then its recent calls, then provisioning).
- Written on every change, and at most once a minute for a refresh; deleted (empty retained) on de-registration,
  expiry, Reg_Denied after a policy change, or when `where/{id}` names another node with a newer time.
- On a restart the site reads its own records back (the broker keeps them) and serves those MSs at once.

The router treats a record as a sighting with `source: "reg"`, at the record's time, that doesn't expire before
`exp`. A newer sighting anywhere (registration or heard traffic) wins; ties go to the registration.

### 5.4 Call setup across the network (the `setup` item)

New item type 0x0C, `setup`, 8 bytes:

| Offset | Field | Meaning |
|---|---|---|
| 0 | op | 1 `request`, 2 `accept`, 3 `reject`, 4 `alerting`, 5 `cancel`, 6 `answered` |
| 1 | reason | 0 ok; 1 `not_found`; 2 `busy`; 3 `no_answer`; 4 `refused` (by the MS or policy); 5 `not_supported`; 6 `congestion` (no channel); 7 `timeout` |
| 2 | flags | bit0 `foacsu`, bit1 `unconfirmed` (the answerer couldn't check: Tier II), bits 2-7 zero |
| 3 | service options | as C_RAND's (emergency, privacy, priority) |
| 4 | ref | u32 LE: the stream ID of the `request` this message answers (0 in a request) |

A setup message is a call message with no voice: header (PC, src, dst) plus one `setup` item, on `pc/{dst}/{node}`.
The router routes it like any private message (no router change for routing).

Flow, MS 3100123 at site A calls MS 3100456 at site B:

```
A: C_RAND(ind, 3100456) from 3100123   -> C_WACKD to 3100123
A -> pc/3100456/A   setup request, stream S, src 3100123, dst 3100456
router -> dl/B      (3100456 is registered at B)
B: C_AHOY to 3100456 ... C_ACKU(Message_Accepted)
B -> pc/3100123/B   setup accept, ref S        (B grants 3100456 a slot now)
router -> dl/A
A: PV_GRANT to 3100123                         (voice then flows as ordinary pc streams both ways)
```

Rules:

- The caller's site waits up to `setup_s` (5 s) for an answer, sending C_WACKD to its MS meanwhile; no answer:
  C_NACKD(MSaway_Refused) and a `cancel` out.
- `alerting` (FOACSU) keeps the wait going until `answered` or `tp_s`.
- A `reject not_found` doesn't end the wait (with a flood, another node may still accept); the timer then ends the
  call as `not_found`. Any other reject ends it at once ([mqtt-net.md](mqtt-net.md) §9.4).
- **All nodes:** a Tier II box that gets a `request` on `dl/{node}` for an ID in its last-heard table on a path that
  can send answers `accept` with `unconfirmed` at once; without one it answers `reject not_found`. So a Tier III MS
  can call a radio on a Tier II repeater, and the voice goes as today.
- **Router:** a `request` for an ID it can't place gets `reject not_found` on `dl/{publisher}` from the router (the
  "early refusal"), unless its flood policy sends the request on, in which case the first `accept` wins and the
  caller's site sends `cancel` to any later acceptor.
- Voice after an accept: each site publishes its own MS's speech as an ordinary private stream; the router places it
  by the called ID as today. The setup's stream ID isn't reused for voice.
- A call from a Tier II radio (no setup) to a Tier III MS: section 3.6.

### 5.5 Group calls across sites

Nothing new on the wire. A trunk site publishes its payload channel's group call on `tg/{tg}/{node}` when the
talkgroup is in `wide_tg`; it subscribes to `tg/{tg}/+` for each talkgroup in its registration records and
`static_tg`. The broker delivers a call only to sites that subscribed, so a call reaches exactly the sites with
members, the Tier III subscription model, with no central table. Tier II repeaters carrying the same talkgroup take
part unchanged.

### 5.6 The trunk part of `info` and `status`

`info` (retained, schema 1 extended): the trunk path has `"trunk": true`; a `trunk` object:

```json
"trunk": {"system": {"model": "small", "net": 5, "site": 3, "par": "ab", "area_bits": 5},
          "tscc": [{"chan": 1, "tx_hz": 438750000, "rx_hz": 438250000, "cc": 1}],
          "carriers": [{"chan": 1, "tx_hz": 438750000, "rx_hz": 438250000},
                       {"chan": 2, "tx_hz": 438600000, "rx_hz": 438500000}],
          "services": ["reg", "group", "private", "allcall"], "reg": true, "priority": 0}
```

`priority`: the site's `broadcast.priority` (0-7, table 6.81).

Neighbors: each site builds its Adjacent_Site list from the `info` of the other online nodes with a `trunk` object of
the same MODEL and NET, unless `cfg/node/{node}` gives `neighbors` explicitly. From each it takes the C_SYScode
(`system`), the TSCC (`tscc[0]`: `chan`; `tx_hz` and `rx_hz`, 50-999.999875 MHz, for Chan_Freq and VN_AP; `cc`) and
`priority` (the adjacent priority it radiates). Older `info` without them: no Chan_Freq for that neighbor, its color
code taken as the site's, priority 0 (section 3.11).

`status` (retained, every 10 s and on change), for all nodes; a trunk site adds:

```json
"trunk": {"connected": true, "regs": 41, "queue": 0,
          "channels": [{"chan": 1, "lcn": 0, "use": "tscc"},
                       {"chan": 1, "lcn": 1, "use": "call", "src": 3100123, "dst": 91, "group": true, "since": "..."},
                       {"chan": 2, "lcn": 0, "use": "idle"}, {"chan": 2, "lcn": 1, "use": "hang", "dst": 3100}],
          "counters": {"aloha": 0, "rand": 0, "collisions": 0, "grants": 0, "refused": 0, "queued": 0}}
```

`activity` events added: `reg` (`src`, `action` register/deregister/expire/deny/refuse/auth_ok/auth_fail/auth_none,
`tg`; section 3.14), `grant` (`src`,
`dst`, `group`, `chan`, `lcn`, `late`, `source` rf/net), `queue` (`src`, `dst`, `action` in/out/drop, `why`),
`setup` (`src`, `dst`, `op`, `reason`, `peer`). call_start / call_end on a trunk site carry `chan` and `lcn`.

### 5.7 Provisioning (`cfg/...`) and commands

Every document has `rev` (an integer the manager increases on each change) and `at`. A node applies a document only
when its `rev` is newer than the one it holds, stores it in its cache (rdofradio: `--state-dir`, file
`cfg-cache.json`; a board: NVS), and uses the cache when the network is down at start. Settings in the node's own
file are the base; `cfg` overrides them key by key; a key `cfg` can't change (frequencies, devices) is ignored and
reported in `cmd/res`.

`cfg/net`:

```json
{"rev": 12, "at": "...", "system": {"model": "small", "net": 5},
 "registration": "listed", "ms": "3100000-3199999", "refuse_unlisted": "denied",
 "private": "*", "wide_tg": "1-99999", "timers": {"hang_s": 3}}
```

`cfg/node/{node}`: `{"rev", "at", "site": 3, "area_bits": 5, "neighbors": [4, 7], "tg": "...", "static_tg": "...",
"timers": {...}, "enabled": true}`. `enabled: false` takes the site off the air (it withdraws the TSCC).

`cfg/tg/{tg}`: `{"rev", "at", "name": "County ops", "wide": true, "sites": "*", "ms": "*", "priority": 0,
"hang_s": 3}`. `ms` limits who may call or join it; `sites` where it may be used.

`cfg/ms/{id}`: `{"rev", "at", "name": "Unit 12", "enabled": true, "private": true, "wide_private": true,
"priority": 0, "emergency": true, "tg": [91, 3100], "auth_k": "0102030405060708090A0B0C0D0E0F10", "psn": "010203"}`.
`enabled: false` = Reg_Denied at every site, and an existing registration is removed. `tg` adds static talkgroups
for that MS. `auth_k` (32 hex digits) is its authentication key K and `psn` (6 hex digits, optional) its PSN
(section 3.14); a bad value refuses the document without quoting it.

Precedence: `cfg/ms` over `cfg/tg` over `cfg/node` over `cfg/net` over the settings file.

Commands (`node/{node}/cmd`, JSON `{"id": "c-123", "cmd": "...", ...}`; the result on `cmd/res` with the same `id`,
`ok`, and `error`):

| `cmd` | Arguments | Does |
|---|---|---|
| `deregister` | `ms` | removes the record; the MS registers again on its next access |
| `clear` | `chan`, `lcn` | ends the call on that slot (P_CLEAR) |
| `withdraw` / `restore` | | takes the TSCC off the air / back |
| `resend_records` | | republishes every registration record |
| `status` | | publishes `status` now |
| `mass_reg` | `window_s` (0-200000, 0 cancels), `ms` (optional: one MS) | a mass re-registration (section 3.11) |
| `move` | `chan` (1-4094) or `site` (an adjacent site's C_SYScode), `ms` (optional: one MS) | C_MOVE: the MSs leave for the TSCC on that channel, or for that adjacent site's TSCC (section 3.12) |
| `auth` | `ms` | an authentication poll of a registered MS with a key (section 3.14); a failure removes its registration |
| `stun` / `revive` | `ms` | stun or revive an MS from the site; authenticated when the MS challenges the site (section 3.18) |
| `kill` | `ms` | kill an MS (always authenticated: refused without a key for it; section 3.18) |
| `dgna` | `ms`, `tg` (a list of 0-15 talkgroups) | DGNA_Address from DGNAI to the MS (section 3.17); `[]` sends ADRNULL, which clears what DGNA gave it |

### 5.8 Security

- Broker: TLS on the backbone (`transport: mqtt/tls`), a username and password per node, ACLs per role (mqtt-net.md
  section 11 plus section 5.2 here). Only the manager writes `cfg/#` and `cmd`; only the router writes `dl/#` and
  `where/#`.
- A node trusts `cfg` because of the ACL. Signed provisioning (a manager key, verified on the node) is a later step;
  the `rev`/`at` fields leave room for a `sig` field.
- The dashboard and REST API need a token (manager setting); TLS by a reverse proxy or the manager's own.
- MS authentication keys (`auth_k`) travel in the retained `cfg/ms` documents, which every node and the router read:
  the ACL and TLS are what keep them secret. A node keeps them in its provisioning cache. Per-node key documents
  (only the sites that serve an MS) are a later step.

### 5.9 Short data across the network (the `sdata` item)

New item type 0x0D, `sdata`, 12 bytes plus the data, the only item of its message:

| Octet | Field |
|---|---|
| 0 | op: 1 a message, 2 its result |
| 1 | what: 1 UDT short data, 2 a status, 3 a status poll, 4 DGNA (3.17), 5 a transmit interrupt (3.20) |
| 2 | reason: a result's air reason (44h MS_Accepted, 47h a poll's answer, 60h Message_Accepted, or a C_NACKD reason) |
| 3 | value: UDT_Format, the status, or a poll's result: the polled status |
| 4 | flags: bit 0 talkgroup, bit 1 emergency |
| 5 | the UDT's Pad Nibble |
| 6 | why: a result's setup reason (5.4: not_found, busy, no_answer, refused, timeout) |
| 7 | 0 |
| 8-11 | ref u32 (little endian): a result's message's stream ID |
| 12- | the data, 0-46 octets (UDT short data and DGNA only) |

For a padded UDT format (BCD, ISO 7- and 8-bit, UTF-16, mixed: annex B) the data is the message without its pad
octets; the receiving site pads it again for its blocks. An odd pad nibble (BCD) stays in the last octet and in
octet 5. The other formats go as their blocks hold them.

A message to an MS goes on `pc/{dst}/{node}` (the router places it by the newest record, as a setup request); to a
talkgroup on `tg/{tg}/{node}`, only for a `wide_tg` talkgroup or ALLMSID. The header has the caller as src, the
called as dst, a fresh stream ID, the group and emergency flags. The site that has the called MS delivers it
(3.15) and answers with a result on `pc/{caller}/{node}`: src and dst swapped, as a setup answer, ref the
message's stream. A talkgroup's message gets no result. A message for an MS not registered at the site is ignored
(another site may have it). Nodes without Tier III drop the item (never a stream).

A transmit interrupt (what 5, section 3.20) goes from the interrupter's site to the talker's on `pc/{talker}/{node}`:
src the interrupting MS, dst the talker, value the status (125), flags 0, data 4 octets: the call's destination
(u24 big endian: the talkgroup, or the individual call's other party) and 1 for a talkgroup call, 0 for an
individual call. The talker's site answers with a result (reason Message_Accepted, Not_Supported or
Perm_User_Refused), src and dst as in the message, ref its stream. The application can't send one.

### 5.10 Packet data across sites (the `data_pdu` item)

The code: `rust/src/t3pdnet.rs` (a relay per payload channel), the controller's part in `t3pdata.rs`, the network's
in `t3unit.rs`. Packet data to an MS registered at another site, and to a `wide_tg` talkgroup, crosses mqtt-net in
the `data_pdu` item every box already uses ([mqtt-net.md](mqtt-net.md) §4.2, §4.4): one message, part 0, the data
header as the MS sent it (F = 1) and the user data without serial numbers, CRCs or pad, on `pc/{dst}/{node}` or
`tg/{tg}/{node}`. The message header has the caller as src, the called party as dst, the group flag, and a fresh
stream ID. The only new field is the setup's flag bit 2, `data`.

Design: store and forward, a whole message at a time, with the confirmed-data procedure on each air link (hop by
hop), not block streaming with the far MS's answer carried back:
- 361-3 5.4: the sender waits T_RspnsWait for the response (annex A.1: 180 ms recommended), too short for a round
  trip through the broker to an MS on another site, its grant and its answer.
- 361-3 clause 5 leaves relaying between base stations out of scope, and mqtt-net.md 4.4 already answers confirmed
  data hop by hop for every box; a Tier III site does the same.
- 361-4 6.6.3.1.4: the TSCC checks the called MS before it gives the caller a channel. Across sites that check is
  the setup (5.4), so a channel is only granted for an MS that answered.
- 361-4 6.6.3.3.2.5: an MS goes back to the TSCC after a single item, so the two air links don't overlap in time
  anyway; the calling site has the whole message before the far site's MS is on its channel.
- The caller's ACK then means "the network has it" (Message_Accepted), as mqtt-net.md 4.4 says; what became of the
  message at the far site goes in `activity` as `data_result`.

Individual data, MS 3100123 at site A to MS 3100456 at site B:

```
A: C_RAND(0010b, 3100456) from 3100123    -> C_WACKD
A -> pc/3100456/A   setup request, flags data, stream S
B: C_AHOY(0010b, source 3100123) to 3100456 ... C_ACKU(Message_Accepted)
B -> pc/3100123/B   setup accept, flags data, ref S   (B holds the answer 30 s)
A: PD_GRANT to 3100123; takes the message on the channel, answers it (ACK, or SACK / NACK and the resends)
A -> pc/3100456/A   data_pdu, stream T        (the router places it by the record, as the setup)
B: PD_GRANT to 3100456 (no second check); sends the message, confirmed if it came confirmed
B: activity data_result (delivered, failed, no_response or unconfirmed; tries)
```

Rules:
- The calling site (3.16 step 1): a called MS not registered here goes out as a setup request when the site is
  connected and the caller's `wide_private` allows; the caller hears C_WACKD meanwhile. No answer in `setup_s`:
  MSaway and a `cancel` out. A reject's reason maps to the air's as for voice (`not_found` NoregMSaway, `busy`
  Called_Party_Busy, `refused` Recipient_Refused, `not_supported` Not_Supported, `congestion` SYSbusy, the rest
  MSaway). Every setup message of a data call has the data flag. Data needs no alerting; an `alerting` answer is
  ignored.
- On the channel the calling site takes the message itself: a confirmed one is answered on its air as 3.16 has it
  (the site is the receiver: ACK, SACK with C_RDATA, NACK), a repeat after a lost ACK (same source, N(S) and CRC-32,
  361-3 5.4.2.2) is answered again and not published twice. A message whose CRC-32 fails is not published.
- The far site answers a setup request with the data flag through the availability check (C_AHOY Service_Kind
  0010b, source the remote caller): `accept` (flags data) after C_ACKU, else `reject` with the reason; a busy or
  unknown MS, or no free data call, is refused at once. The accepted call waits up to 30 s for its data; a `cancel`
  ends it. The data then gets PD_GRANT at once (the check is done), or the open channel between the same two
  parties again.
- The far site sends the message on the channel after the grant's lead (360 ms: the grant twice on the TSCC, the
  MS's move), in blocks of the rate it came at, rebuilt with its own serial numbers, CRCs and pad. A confirmed
  message is sent as one, A set, and the site is the sender of 361-3 5.4: the SACK's blocks again after a C_HEAD with
  F = 0, all of them after a NACK or after 3 s with no response, 4 sends at most. An unconfirmed message goes once.
- Data with no setup before it (the IP gateway's replies, a Tier II box's data, a site whose setup was lost) gets
  the availability check at the far site first, then PD_GRANT. A data message for an MS not registered at the site
  is ignored (another site may have it).
- Talkgroup data from an MS on a `wide_tg` talkgroup: the calling site grants TD_GRANT as for any talkgroup, takes
  the message (no response: a talkgroup's data is unconfirmed) and publishes it on `tg/{tg}/{node}`. Each site that
  has the talkgroup (an MS registered with it, or ALLMSID) grants its own TD_GRANT and sends the message there.
- `data_result` (mqtt-net.md §7) goes in `activity` for each message sent from the network: `from` the publisher's
  node, `tries` its sends. The unit counts `data_out` (published), `data_in` (taken) and `data_dropped` (messages it
  couldn't take: parts, a header other than unconfirmed or confirmed, no room).
- The site's IP gateway (3.16) answers an MS at another site: `rdof_t3unit_data_tx` finds no record here and
  publishes the packet, from the gateway's source, as a `data_pdu` on `pc/{dst}/{node}`, with no setup.

A message crosses whole, up to 127 blocks (3044 octets at rate 1): `data_pdu`'s length field takes 65535 octets.

Not built: messages in parts (`data_pdu` part above 0 or the more bit: a data call's single item is one message, so
parts aren't needed for it), multi-item calls across sites (not tested: each item would cross as its own
message), talkgroup data from the site's own gateway to other sites, and a result back to the calling site (end to
end delivery is the application's: 361-3 5.2).

Network services: the router forwards the setup and `data_pdu` messages as raw bytes, so routing needs no change.
Their setup codecs read only flags bits 0-1 and refuse to encode bit 2; a service that decodes a setup and encodes
it again (none does today) would drop the data flag, and the far site would then take the request for a voice call.
The codecs should keep bit 2 (`data`).

## 6. Failure and isolation

### 6.1 What each failure costs

| Failure | Local registrations and calls | Wide-area calls | Recovers |
|---|---|---|---|
| Backbone link down (site broker up, or none) | keep working from the cache | stop; Active_Connection = 0; other sites see the node's will (`online` 0), stop routing to it, and drop its records from their routing | on reconnect: `info`, `status`, every registration record again |
| Manager down | keep working on the cached `cfg` | keep working | on return it republishes; nodes compare `rev` |
| Router down | keep working | group calls keep working (broker fan-out); private calls across sites fail with `no_answer` | a second router (`$share`) takes over (mqtt-net.md 9.1) |
| Site controller (rdofradio) restarts | the TSCC stops for the restart (a few seconds; MSs hunt and come back) | | reads its records back (6.2) |
| Split radio (carrier B) lost | the site runs on carrier A's payload slot; B's slots are marked out of service | | when the radio returns |

### 6.2 State and the standby controller (built later, prepared now)

- All state worth keeping is in the retained registration records (one per MS) and the `cfg` cache. Calls in progress
  are not kept: a restart ends them (the MSs return to the TSCC on TV_Inactive).
- A standby controller is a second box with the same settings: it holds a lock topic (`node/{node}/lock`, retained,
  with the holder's ID and the will clearing it), takes over when the active one's will fires, reads the records and
  keys up. The controller API (section 8) has the hooks: `rdof_t3_load_record`, `rdof_t3_withdraw`.
- A site broker: optional. With one at the site (bridged to the backbone, `DMR/#` both ways except `cfg/#` and
  `node/+/cmd` inbound only), the site's own controller, records and provisioning cache keep working with the
  backbone down, and a standby box at the site works too.

## 7. The road to roaming (multi-site)

Built this round: everything a second site needs, because each record, topic and message is already global.

| Step | What 361-4 provides | Where it lives |
|---|---|---|
| A radio finds a neighbor | Adjacent_Site and Vote_Now broadcasts, Chan_Freq for the neighbor's TSCC channel, the sites' priorities (section 3.11) | each site, from the others' `info` |
| A radio moves | it registers at the new site (new SYS_AREA) | the new site writes `node/{new}/ms/{id}`; the router's `where/{id}` moves; the old site sees `where` and deletes its record |
| A group call reaches the new site | the record's talkgroups | the new site subscribes; the broker delivers |
| A private call reaches the moved radio | the setup request | the router places it by the newest record |
| A call in progress | Tier III has no handover | the radio re-registers and late-enters from the repeated grant |
| The network splits | | each part serves its own; on rejoin the newest record wins |

Later steps, none needing a redesign: registration areas spanning several sites (`area_bits` smaller than SITE), a
home registry and per-radio permissions checked across sites (the manager already holds them), C_MOVE for load
sharing, the priorities from the manager (`broadcast.priority` is a local setting now).

## 8. Site controller API (`rdof/t3.h`)

The Rust exports a C API (the repo's convention: the header is the contract; structs mirrored `#[repr(C)]`). The
t3 structs aren't in `rust/src/layout.rs` (a legacy-only module): `rdof_t3_layout()` reports their sizes and
alignments and `host-tests/test_t3.c` compares them with the header's. The exact API is `common/include/rdof/t3.h`;
the sketch below is the design, which the header extends (`rdof_t3_net_private_end`, `rdof_t3_record`,
`rdof_t3_subs`, `rdof_t3_entropy` (the challenges' random source, section 3.14), short data's `rdof_t3_sd_*`,
packet data's `rdof_t3_rx_data` and `rdof_t3_pd_send`, the MS simulator's `rdof_t3ms_*`).

```c
typedef struct rdof_t3_site rdof_t3_site_t;          /* opaque, caller allocates rdof_t3_site_size() bytes */

size_t rdof_t3_site_size(void);
bool   rdof_t3_init(rdof_t3_site_t *s, const rdof_t3_cfg_t *cfg, char *err, size_t cap);
bool   rdof_t3_reconfigure(rdof_t3_site_t *s, const rdof_t3_cfg_t *cfg, char *err, size_t cap);

/* time: the TSCC's outbound slot counter k (TS1 = even), and the wall clock in ms */
void   rdof_t3_slot(rdof_t3_site_t *s, int64_t k, int64_t now_ms, rdof_t3_slot_out_t *out);

/* inbound: a PDU with a good CRC (10 bytes, dt CSBK / MBC header / MBC continuation / UDT), or a burst with a good
 * sync and a bad CRC on the TSCC (counts a collision) */
void   rdof_t3_rx_pdu(rdof_t3_site_t *s, int ch, int64_t k, uint8_t dt, const uint8_t pdu[10], int rssi);
void   rdof_t3_rx_bad(rdof_t3_site_t *s, int ch, int64_t k);
/* payload channel activity: voice header, voice, terminator, lost, with the addresses the LC gave */
void   rdof_t3_rx_voice(rdof_t3_site_t *s, int ch, int64_t k, uint8_t what, uint32_t src, uint32_t dst, bool group);

/* network */
void   rdof_t3_net_group(rdof_t3_site_t *s, uint32_t stream, uint32_t src, uint32_t tg, uint8_t flags, int64_t now_ms);
void   rdof_t3_net_group_end(rdof_t3_site_t *s, uint32_t stream);
void   rdof_t3_net_setup(rdof_t3_site_t *s, const rdof_t3_setup_t *m, int64_t now_ms);
void   rdof_t3_net_private(rdof_t3_site_t *s, uint32_t stream, uint32_t src, uint32_t dst, int64_t now_ms);
void   rdof_t3_net_link(rdof_t3_site_t *s, bool connected);
void   rdof_t3_neighbors(rdof_t3_site_t *s, const rdof_t3_neighbor_t *n, int count);
void   rdof_t3_where(rdof_t3_site_t *s, uint32_t ms, bool here, int64_t at_ms);

/* provisioning and state */
void   rdof_t3_policy(rdof_t3_site_t *s, const rdof_t3_policy_t *p);   /* ID sets compiled by the caller */
void   rdof_t3_ms_policy(rdof_t3_site_t *s, uint32_t ms, const rdof_t3_ms_policy_t *p);
bool   rdof_t3_load_record(rdof_t3_site_t *s, const rdof_t3_record_t *r);
void   rdof_t3_withdraw(rdof_t3_site_t *s, bool off);
bool   rdof_t3_command(rdof_t3_site_t *s, const char *cmd, uint32_t arg1, uint32_t arg2); /* also stun, revive, kill */

/* outputs: drained after each call */
bool   rdof_t3_event(rdof_t3_site_t *s, rdof_t3_event_t *ev);
void   rdof_t3_status(const rdof_t3_site_t *s, rdof_t3_status_t *st);
```

`rdof_t3_slot_out_t` holds, for slot k: the TSCC PDU (`dt`, 10 bytes) when k is a TSCC slot, the AT bit for the
CACH at k+1, the 28-bit short LC for the 4-slot cycle when k % 4 == 0, and for each payload channel a control PDU to
send instead of voice or idle (P_CLEAR, P_GRANT) if any.

Events (`rdof_t3_event_t`, a tagged struct): `CH_OPEN` (ch, call: src, dst, group, kind, emergency, source rf/net,
stream), `CH_CLOSE` (ch, why), `NET_PUBLISH_SETUP` (setup message), `NET_SUBS` (the talkgroup set changed),
`RECORD` (write or delete a registration record), `ACTIVITY` (reg/grant/queue/setup event fields; `ACT_SUPP`, a
supplementary service's result, 3.18), `NET_DROP` (a
network stream won't be carried), `LOG` (a short text line).

Capacities (compile-time constants, `rdof_t3_limits`): 1024 registration records, 8 talkgroups each, 8 payload
channels, 16 queued requests, 16 pending setups, 16 network streams, 32 neighbors. About 80 KB.

Determinism: the controller's randomness (backoff choice is the MS's, not the TS's; the TS has none today) is
seeded by the caller, so tests are repeatable.

## 9. Radio layer (C)

### 9.1 SDR modem: the slot source

A new hook on `rdof_sdrmodem_t` for role 0 (duplex):

```c
typedef struct {
    /* fill slot k's burst (33 bytes) and its CACH fields; false = use the modem's own queue/idle for that slot */
    bool (*slot)(void *ctx, int64_t k, uint8_t burst[33], int *at, uint32_t *short_lc28_or_neg);
    void *ctx;
} rdof_sdrmodem_source_t;
void rdof_sdrmodem_set_source(rdof_sdrmodem_t *m, const rdof_sdrmodem_source_t *src);
void rdof_sdrmodem_hold_key(rdof_sdrmodem_t *m, bool on);   /* stay keyed with no traffic (a TSCC) */
```

`queue_duplex` asks the source for each slot k before it looks at the FIFO; the short LC from the source replaces
the latch at k % 4 == 0; AT comes from the source instead of the busy mask. The modem keeps its own timing; k is its
slot counter (the same numbering its inbound timing uses, so an inbound burst's slot maps back to k). The source is
called about one slot plus the TX lead ahead of air: the controller decides a slot about 100 ms before it goes out.

### 9.2 Payload channels

`rdof_t3rf_ch_t` (portable; Rust, `rust/src/t3rf.rs`): per payload channel (carrier, LCN):

- inbound bursts (from the receiver, MS sync) on an open channel: regenerated (`rdof_duplex_data`, `rdof_voice_regen_*`)
  and queued for the outbound slot one frame later; voice activity to `rdof_t3_rx_voice`; CSBKs (P_MAINT) to
  `rdof_t3_rx_pdu`; the frames to the mqtt-net uplink (with the site's path) when the call is wide-area;
- packet data (3.16): data headers and blocks repeated the same way; the unit counts each message
  (`rdof_t3_rx_data`) and, when the site is a party, hands the bursts to its `data` hook instead of the uplink; the
  site's own data goes on the queue with `rdof_t3rf_data` (`rdof_t3rf_room` says how much fits);
- network bursts for the channel's call (from the mqtt-net downlink, after a 240 ms prebuffer) queued the same way,
  unless RF holds the channel;
- an LC the layer makes itself (voice joined late from RF, with no header heard; network voice without an LC) has
  the call's addresses and its Service Options from `rdof_t3rf_ch_t.so`: Emergency (80h) for an emergency call,
  Broadcast (08h) for an all-call or a broadcast call (361-2 5.3.2.1, 5.3.3.1, table 7.11). The unit sets `so` from
  the controller's EV_CH_OPEN `svc`, and again from a grant's activity event (an emergency request makes an open call
  an emergency call, 3.9). An LC heard from the talker goes out as heard;
- the controller's payload PDUs (P_CLEAR) take precedence over voice;
- after a voice call's terminator, the call hang time: the terminator's LC again in each free slot
  (`RDOF_T3RF_HANG`, `hang` and `hang_lc`; 361-1 5.2.1.4), until voice comes again or the channel closes. When RF
  voice is lost (no terminator) or the network's stream ends without one, the layer queues a terminator with the
  stream's LC first. A full-duplex call has none;
- idle bursts fill otherwise;
- voice activity to the controller: the header, each voice sync burst (once a superframe, so a long call isn't taken
  for a lost one), the terminator, or lost;
- a Reverse Channel PDU for the other slot's talker (`rdof_t3rf_rc`, from `rdof_t3_ch_out_t.rc`) goes once into the
  next voice burst F or idle burst, 360 ms apart, not right after a terminator (section 3.9); in hang time an idle
  burst carrying it takes one hang time terminator's place (361-1 5.1.5.2);
- a full-duplex call's channels (`rdof_t3rf_duplex`, section 3.13): the carrier's inbound slots read in offset
  timing, each channel's regenerated inbound queued on the other's outbound.

Outbound sinks: carrier A's channels are read by the slot source (9.1); carrier B's by its own modem's slot source
when the SDR sends it (9.4), else they go to the split radio's TX half
(`rdof_split_t`) in the scheduled stream (SET_ROLE stream 1, TX_SCHED): each burst tagged with its slot counter,
sent at least the half's guard (INFO 12, 55 ms on an HR_C7000) before air, so there is no FIFO prebuffer and the
repeat delay is the same as carrier A's; DMR_SHORTLC with P_SYS_Parms. A half without the scheduled stream (INFO
caps bit 0 clear) falls back to the FIFO (DMR_DATA1/2, `split.prebuffer`, default 3 = 180 ms).

### 9.3 Receive

The SDR's receiver channels A-in and B-in feed the trunk layer straight from `rdof_sdr_rx_t.on_burst` (with the
burst's family, CC, slot and position), not through the modem's 34-byte DMR_DATA path:

- A-in TS1 (the TSCC inbound): CSBK / MBC / UDT with a good CRC to `rdof_t3_rx_pdu(ch 0)`; a data-sync burst with a
  bad CRC to `rdof_t3_rx_bad`.
- A-in TS2, B-in TS1/TS2: to their payload channel (9.2).
- The slot of a carrier A inbound burst comes from the modem's own outbound timing (inbound slot n is under outbound
  slot n, aligned timing); carrier B's from its own modem's outbound timing when the SDR sends it, else from the
  split radio's epoch, as the split repeater does.

### 9.4 rdofradio

- The core is `rdof/t3unit.h` (Rust, `rust/src/t3unit.rs`, in `rdof_common` with the `tier3` feature): the
  controller run ahead of the air into a ring of slots, the radio layer, and mqtt-net, with no I/O of its own
  (publish, subscribe, log, cache and random numbers are callbacks). `rdof_t3unit_t` is opaque: the caller allocates
  `rdof_t3unit_size()` bytes, which hold mqtt-net's uplinks and downlink at the sizes C gives
  (`rdof_mn_up_size`, `rdof_mn_down_size`), and reads it through accessors (`rdof_t3unit_cfg`, `_mc`, `_rf`,
  `_counts`, `_peers`, `_neighbors`, `_tg_policies`). `sdr/cli/rdofradio_t3.c` gives it libmosquitto, the clock and the device; the host tests give it an
  in-process broker (`host-tests/test_t3unit.c`).
- `--config` with a `tier3` app: rdofradio opens the SDR full duplex for carrier A. The Tier III unit is the SDR
  modem's slot source (`rdof_sdrmodem_set_source`) and the modem stays keyed (`rdof_sdrmodem_hold_key`); the repeater
  still runs as the modem's client to set it up, its output shadowed by the source. What the SDR hears on carrier
  A's `in` goes to the controller with the slot its timing gives (`rdof_sdrmodem_inbound_k`).
- The trunk's slot k is the UTC slot number, floor(UTC ms / 30). Carrier A's modem slots are moved onto it by an even
  offset taken at the first slot the modem asks for (again if its slots restart), so the modem's TS1 is LCN 0. The
  carriers aren't slot-synchronized (`site_sync` 0: the payload warm-up).
- `--tier3 FILE` with `--mmdvm PORT`: a one-carrier site out of a stock duplex MMDVM modem, an SDR receiver hearing
  its out and in; the trunk's slots follow the modem's own clock, placed by when each burst is written (inside the
  lead windows that set each CACH's AT), the modem keyed once by a write and held by `GET_STATUS` every 250 ms
  ([tier3-hat.md](tier3-hat.md)).
- Carrier B, the first build's second carrier, is a split TX half on its radio's port (`rdof/sdr/t3split.h`,
  `rust/src/t3split.rs`): one split of `rdof_sdrsplits` with the SDR's receiver as its RX half. Before the receiver
  starts, rdofradio brings the half up and sends the carrier's settings (`SET_CONFIG`, `SET_MODE`, `SET_FREQ`; no
  answer in 8 s, or a NAK, and the site doesn't start), then aligns the half's slot counter to UTC from the host's
  clock (`rdof_split_set_utc`, again each minute) with the scheduled stream. The main loop's 20 ms poll pumps it
  (`rdof_t3unit_split_pump`: the controller 4 slots ahead, then `rdof_t3rf_split_pump`): keyed while the
  carrier has a call, `TX_SCHED` for each traffic or control burst on its UTC slot, the short LC when it changes. The
  receiver gets a channel on carrier B's `in` when the settings have none; its bursts go to the split's RX half, which
  labels each with its slot, and come up with the UTC slot of that parity nearest the burst's position. Three
  carriers or more are refused, and so is carrier B's `out` on another SDR.
- Carrier B on the SDR (its `out` among the SDR's `transmits`, `tx.carriers` 2): a second duplex `rdof_sdrmodem_t`,
  set up as the repeater sets carrier A's (`SET_CONFIG`, `SET_FREQ`, `SET_MODE`, no role) with the unit's carrier 2
  (`rdof_t3unit_slot(u, 1, k, ...)`) as its slot source. It gets the same slot grid as carrier A before each pull
  (`rdofradio_utc_apply`), carrier A's TX delay and ppm, and is keyed with it, so both modems' slot k is the same UTC
  slot (off the grid both take the same offset, being keyed on the same sample). Its I/Q for the same positions is
  added to carrier A's and the sum halved (`rdof_sdrmodem_pull_mix`): each carrier at half amplitude, 6 dB under one
  alone, and the sum can't pass full scale (each modulator's output is at most 1). The TX LO sits halfway between the
  two outs (`--tx-lo` unless given), fixed for the run; carrier B's tune never moves it. Both outs must lie within
  0.4 of the stream's rate less 12.5 kHz of it, else the start is refused with the span, the LO, the reach and the
  rate. A bench job that moves the LO silences carrier B until it's back. What the receiver hears on carrier B's
  `in` goes to the unit's carrier 2 with the slot carrier B's modem's timing gives (`rdof_sdrmodem_inbound_k`); the
  modem hears it too (its AT). The status page names carrier 2's sender and frequencies and counts its bursts; the
  stop line gives its pull time.
- With `--fpga-duc` and carrier B on the SDR the transmit stream is 384 kS/s (the device's rate a multiple of it, R 17
  at least: 6.528 MS/s or more), and the outs within a sixth of it (64 kHz) of the LO. The interpolator is a plain
  order-3 CIC (no compensation): a carrier f off the LO comes out down by about 3 · 20 log10 |sinc(f / fs)| (fs the
  stream's rate) and its first image, at f less fs, by the same at that offset. At 384 kS/s, 62.5 kHz off: the carrier
  1.1 dB down (0.9 to 1.4 across a DMR signal's ±6 kHz), the image 43 dB under it (40 to 46). At 192 kS/s the carrier
  would be 4.7 dB down and the image only 19 dB under it (15 at the far edge), 129.5 kHz from the LO on the other
  side: so not 192. The droop is the same for both
  carriers (equal offsets), so they stay level with each other. Without the interpolator the ARM's stream runs at the
  device's rate and the reach is the wider 0.4 rule.
- CPU on the host (an Apple M-series, `test_two_carriers`, 4096-sample pulls): at 192 kS/s about 0.5 ms a pull for
  carrier A's modem and 0.6 ms for carrier B's with the sum, 2.4 and 2.9% of real time; at 384 kS/s 0.46 and 0.57 ms,
  4.3 and 5.4%. A Zynq-7010's A9 at 667 MHz is several times slower; not yet measured there.
- `--state-dir DIR`: the `cfg` cache (`cfg-cache.json`, one document a line, rewritten on each change).
- `--sim-air N`: no SDR, no split radio: the Tier III unit runs against the MS simulator (`rdof/t3sim.h`, in Rust
  `rust/src/t3sim.rs`: N radios, `--sim-id`, `--sim-tg`, `--sim-talk S`, timed actions from `--sim-script FILE`),
  through the full burst path (the simulator's PDUs are built into bursts and parsed back), with real mqtt-net, slot
  k = wall clock ms / 30. Two rdofradios with `--sim-air` and different `site` numbers make a two-site system on one
  computer.
- `--air-ms N` (on a device): N simulated MSs (`--sim-id` on, `--sim-tg`, `--sim-talk S`, `--sim-script FILE` as
  `--sim-air`) beside the real radios on carrier 1. Each slot they hear the outbound burst the site sends, decoded as a
  radio would, and what they send goes to the site as if heard on carrier 1's inbound (one a slot; no RF). A real radio
  then has partners on a one-radio bench: it follows their group calls, takes their individual calls (availability
  check included) and calls them. With mqtt-net they also take commands on `ROOT/node/NODE/air-ms`, a script line
  without its time (`1000001 private 3105738 5000`, `1000001 group 1 3000`, `1000002 register 1`, `1000001 status
  3105738 5`, `1000001 gstatus 1 5`, `1000001 sdm 3105738 hello`, `1000002 gsdm 1 hello all`: a status 0-127, short
  data as ISO 8-bit text up to 46 characters; `1000001 data 3105738 hello`, `cdata` (confirmed), `1000001 gdata 1
  hello all`: a packet data call whose one message is a text message (361-3 5.7.1: UTF-16BE, up to 64 characters,
  or `*N` for N generated ones, up to 746: one 1520-octet datagram; the receiving MS logs the first 80 and whether
  all of it is the pattern)
  in a UDP/IPv4 datagram from port 5016 to 5016, 12.x.y.z the MS's address (its ID in the low 24 bits) and 225.x.y.z
  a talkgroup's, sent as rate 1/2 blocks; `data34`, `cdata34`, `gdata34` the same in rate 3/4 blocks, `data1`,
  `cdata1`, `gdata1` in rate 1; `1000001 setstatus 12`: the status it answers a poll with, else the last it sent, 0
  at start). Their state changes go to the log (`air MS ID: ...`), and so does the short data and packet data they
  take (the text; a UDP datagram's addresses and payload, decoded when it's a text message on port 5016). They take
  data at any rate, as a radio sends it (a real radio's confirmed IP data is often rate 3/4): the outbound's data
  blocks decoded (BPTC, the trellis, or as is at rate 1), the CRCs checked, a confirmed message answered with an
  ACK, a NACK or a SACK and its retry taken (3.16). Test:
  `rdofradio_e2e_t3_airms`.
- `--sim-ext PATH`: external MSs (other programs, such as a radio's MS engine built for the host) on the simulated
  air, through a unix socket, beside the N simulated ones (N may be 0). JSON lines in lockstep with the slots: after
  slot k each client gets what every carrier aired (the burst, its data type and CC, the PDU when its CRC passed, the
  CACH before it with AT, TC, LCSS and the short LC fragment) and the AT after it on the TSCC, plus what became of its
  own bursts; it answers with its bursts for coming slots (a PDU, a voice header, terminator or burst, or 33 bytes as
  given), each with a timing error in µs. The site waits up to 2 s for each answer, then drops the client. External
  bursts join the collision count with the simulated MSs'; one on a channel not on air in its slot, or more than half a
  slot off, is lost. `--sim-ext-loss P` and `--sim-ext-err P` lose or corrupt each one at random; `--sim-fast` runs
  the slots as fast as the clients answer. Each inbound burst gets a log line. The format is in
  `sdr/cli/rdofradio_simext.h`.
- Rust in the Linux build: `rust.cmake` has `FEATURES`; `sdr/cli/tools.cmake` builds `rdof_rust` with only the `t3`
  feature (so the C modules stay C) when cargo is found and `RDOF_TIER3` is on, for `RDOF_RUST_TARGET` when cross
  building; the Pluto container installs rustup with `armv7-unknown-linux-gnueabihf`.
- Provisioning: each `cfg/tg` document's `hang_s` (1-60 s; else the site's) and `priority` (0-3) go to the controller
  (`rdof_t3_tg_policy`, 64 talkgroups): a group call's channel hangs for its talkgroup's time, and a group request's
  priority is the most of its own, the MS's and the talkgroup's. A deleted document's entry goes.
- Neighbors: the unit subscribes `node/+/info` and `node/+/online`. Another node whose `info` has a `trunk` object of
  the same MODEL and NET is an adjacent site (its C_SYScode from its system, `rdof_t3_syscode`, its TSCC channel,
  frequencies and color code, and its `priority`), active while its `online` is `1`; `cfg/node`'s `neighbors`, when
  given, keeps only the sites it lists.
- Tests: `rdof_t3split_tests` (a two-carrier site, carrier B on a fake split TX half: bring-up, UTC alignment,
  a call on carrier B as `TX_SCHED` on the UTC slots followed by a simulated MS from what the half airs, a heard burst
  labeled by slot, unkeyed after the hang); `rdof_t3unit_tests` (two sites over a broker, `cfg/tg` hang, the
  neighbor list; a four-carrier site on the burst path: every payload channel in use, a pre-emption with the RC read
  back from the air, an emergency alarm).
- Carrier timing: both carriers count the host's UTC slots. Carrier B's split half follows the host's clock
  (`rdof_t3split_utc`, measured each minute). Carrier A's modem sits on the same grid through rdofradio's slot timing
  (`sdr/cli/rdofradio_utc.c`): each read of the receive stream stamped with its arrival on the host's clock, the
  sample clock on the earliest arrival of every 2 s and its rate from anchors two minutes apart, the map handed to the
  modem before every pull. The site keys carrier A once the map exists (the modem snaps to the grid at key-up, then
  steers a sample a slot at most), or after 10 s without one, on its own count as before. The host's clock is the
  reference even when nothing disciplines it ("own time": a PlutoSDR runs no NTP client); one site needs its carriers
  on one clock, not on UTC. A step in the host's clock moves both carriers' grids; carrier A steers there at 21 us a
  slot, carrier B jumps at its next measurement.
- Tests: `rdofradio_e2e_t3_car2` (both carriers out of a file device: calls granted on carrier B with air MSs on
  both carriers; rdofsdr decodes the transmit file at both outs: the same slots at the same sample, CC, carrier B's
  voice, equal levels); `test_two_carriers` in `rdof_sdrmodem_tests` (the sum within full scale, each carrier 6 dB under
  one alone, an inbound burst on the same k under either modem). An RF inbound burst on carrier B's `in` is checked
  through that k mapping, not end to end (the air MSs' inbound to carrier 2 is).
- Not done: run on a device (carrier A on the Pluto, carrier B's half on air; carrier B on the SDR); the carriers'
  timing checked on air; the second modulator's CPU on the Pluto.

## 10. Network services

In the network services' own repository (with the router, the Python library and the legacy adapter).

- `rfnet` library: the `setup` item in `callmsg`; topic helpers for `cfg`, `ms`, `status`, `cmd`.
- Router: location from `node/+/ms/+` records; `where` gains `source`; early `reject not_found`; `cfg/ms` `enabled:
  false` blocks routing to and from that ID; status counters for setups.
- Manager (`rfn-manager`): reads a provisioning directory (`net.json`, `nodes/*.json`, `tg/*.json`, `ms/*.json` or
  one `provisioning.json`), validates it, publishes the retained `cfg` topics (deleting ones no longer present), bumps
  `rev` on change, watches the directory, serves the REST API (`GET/PUT/DELETE /api/cfg/...`, `POST /api/cmd/{node}`)
  and the dashboard, and keeps an in-memory picture of the network from `node/+/#`, `where/+`, `router/+/#` for the
  dashboard.
- Dashboard: one page, live over Server-Sent Events from the manager: nodes (online, kind, firmware), trunk sites
  (channel grid with each slot's use, queue, counters), registrations (by site, with talkgroups), calls (live and the
  last 200), last heard, router status, and the provisioning documents with an editor that writes through the API.

## 11. MS simulator

`t3ms` (Rust, `no_std`): an MS that knows one site (no hunt) and follows 361-4 for what the site supports:

- waits for C_ALOHA; random access with the Aloha's Backoff and NRand_Wait; retries up to NRand_NR (6);
- registration (and de-registration), with a subscribed talkgroup or a list of up to 7 (`register_list`: the
  upload when the TSCC asks, the Response_Info bits kept);
- DGNA (`send_dgna`): sends a transfer (Address or Mixed Format) to an MS; takes one addressed to it, answers
  MS_Accepted and takes calls to the talkgroups it got;
- group and individual call requests; follows grants to the payload channel; talks for a set time (voice header,
  voice bursts, terminator), P_MAINT DISCON at the end of an individual call; returns on P_CLEAR or TV_Inactive;
- answers C_AHOY: accept, busy, no answer, or FOACSU alerting then answer, as configured; an authentication
  challenge with its key and PSN (`rdof_t3ms_key`), or MSNot_Supported without one (section 3.14);
- the Reverse Channel cease command: finishes its superframe, sends a terminator and stops talking (6.4.15);
- status delivery (`RDOF_T3MS_DO_STATUS`, the status in `talk_ms`): sends one to an MS, a talkgroup or TSI; stores
  the ones delivered to it or its talkgroup and answers an individual one as configured.
- power save (`set_power_save`): asks for it in its registrations, hears the TSCC only in its wake frame and for
  T_Awake (2 s) after a PDU to it; mass re-registration: registers at a random time in the Reg_Window, ignores
  repeats inside it, a Reg_Window 0 cancels; counts the C_BCASTs it hears by type.
- C_MOVE: an idle MS it applies to moves to that channel's LCN 0 (one in its list); a TSCC that said it
  hibernates: no hunting, and BS_Dwn_Act when it has something to do.
- full-duplex calls (`RDOF_T3MS_DO_DUPLEX`): follows a PV_GRANT_DX to it, transmits in offset timing
  (`rdof_t3ms_tx_t.offset`), both parties talk (the called MS 2 s), the caller disconnects; `set_no_duplex` answers
  the radio check C_NACKU(MS_Duplex_Not_Supported). `RDOF_T3MS_F_BROADCAST` makes a group call a broadcast call;
  a BTV_GRANT is followed by the talkgroup's members (any MS for an all-call).
- packet data (`send_data`, `rdof_t3ms_data`): one message (SAP 4, confirmed or not, single or multi-item, rate 1/2
  or with `RDOF_T3MS_F_RATE34` / `RDOF_T3MS_F_RATE1` rate 3/4 or 1) to an MS, a talkgroup or the site; it takes
  messages at any rate addressed to it or its talkgroup (CRCs checked), answers confirmed ones with an ACK, NACK or
  SACK, sends a SACK's blocks again (3.16) and leaves the channel as 6.6.3.3.2.5 says;
- stun, revive and kill: answers C_AHOY from STUNI with Message_Accepted (or MSNot_Supported, `refuse_supp`); with a
  key and `set_stun_auth`, and always for a C_AHOY from KILLI, challenges the site with C_ACKVIT, checks the
  C_ACKD(Authentication_Response) and answers MS_Accepted or Recipient_Refused (a kill without a key:
  MSNot_Supported). Stunned, it makes no requests and answers no checks, grants or downloads; killed, nothing at all;
- ALS (`RDOF_T3MS_F_ALS`): asks for an ALS call and listens; called, answers the ALS radio check Message_Accepted (or
  MSNot_Supported), then transmits on the matching grant for T_ALS (10 s);
- call diversion (`divert`, `divert_clear`): sets one (the address upload), clears its own or the incoming ones;
  takes a diverted address in answer to its request (`diverted`) and ends the request;
- transmit interrupt (`RDOF_T3MS_DO_INTERRUPT`, `interrupt`, `als_end`): interrupts the call it listens to and talks
  on P_PROTECT(EN_PTT_ONE_MS) to it, or ends its ALS call (section 3.20); `rc_deaf` makes it ignore the RC;
- emergency and priority options; cancel.

`t3air`: a slot-exact air between one site controller and N MSs: TSCC outbound to every MS on the TSCC; inbound
collisions (two MSs in one inbound slot: neither is heard, and the site gets `rx_bad`); payload channels with their
members; the Reverse Channel to the MSs on the other slot; optional loss rate and delay. Scenarios script MS actions
by time and check outcomes. The burst-path simulator (`t3sim`) reads the RC out of the aired bursts the same way.
Its MSs' voice is near-silence that never repeats a frame (the silence frame's parameters with a counter in the 8
lowest of the 49 bits, FEC coded again), each burst B-E with the call's LC embedded: a radio may take a frame equal to
the one before for a stale read and drop the call (seen on a radio on the bench, 2026-11-13).

Used by: `cargo test` (scenarios), the C host tests (through the C API), rdofradio `--sim-air`. External MSs join
the same air through `--sim-ext` (section 9.4), so another MS implementation can be checked against the site
and against `t3ms` on one air.

## 12. Tests

| Level | What | Where |
|---|---|---|
| PDU codec | Every PDU the site sends or reads: field packing against the 361-4 tables, round trips, known vectors from a standard site's captures (decoded values, not checks) | Rust unit tests |
| Controller | Scenarios over `t3air`: registration open/listed/denied, collisions and backoff, group call, queue and grant on release, late entry repeats (through the hang time), hang and P_CLEAR, call timer, individual OACSU and FOACSU, no answer, cancel, busy called party, Not_Supported, network group in and out, network setup both ways, Tier II implicit setup, records reload, provisioning changes, link loss; short data (`t3sdtests.rs`): UDT MS to MS, MS, talkgroup and site, 1 to 4 blocks, refusals, store while busy and its timeout, cancel, the kept upload slots, CRC errors and a missing upload, status to MS, talkgroup and site, polling, the site's own sends, a full table, the network both ways; packet data (`t3pdtests.rs`): MS to MS unconfirmed and confirmed, rate 3/4 and rate 1 (unconfirmed to an MS and a talkgroup, a damaged block lost; confirmed with a damaged block: SACK, C_RDATA, the F = 0 retry, the ACK; retries to N_RtryLmt; `t3pdrate.rs`: the blocks at each rate, CRC-9 and CRC-32 against the B.3.9 / B.3.10 formulas), the availability check, PD/TD_GRANT single and multi-item, data hang and clearing, to a talkgroup, to the site (TSI, data ID, IPI), refusals (NACKU mirrored, no answer, not here, multi-part, itself, talkgroup not allowed, busy), queued and cancelled, the site's own sends; packet data across sites (`t3pdnettests.rs`, two sites; `t3pdnet.rs`): confirmed MS to MS with a damaged block on each air link (SACK at both), unconfirmed at rate 3/4, refusals (not found, no answer, a lost setup, the link down), talkgroup data to the site that carries it, data with no setup checked first, the network form at each rate, the relay's retries and results; talkgroup lists (`t3tgtests.rs`): registration with a list (the
C_AHOY's fields, the Response_Info bits, a talkgroup refused), all accepted, replaced, replaced by a single
subscription, dropped on deregistration, a refused registration; DGNA MS to MS (address mode, a call on the new
talkgroup, alias mode, delete all), refusals (off, talkgroup not allowed, itself, the site, not here, the called MS
refuses), the site's own DGNA; supplementary services (`t3sutests.rs`): stun (the C_AHOY's fields, requests from
and to a stunned MS refused, registration kept) and revive, stun not supported, no answer, kill without a key
refused; stun and revive with authentication (C_ACKVIT, the C_ACKD's fields and response, MS_Accepted), a wrong
response (Recipient_Refused), a lost final answer (the C_ACKD repeated), no key at the site, no response heard;
kill (KILLI, the record removed, no answer after), refused, not supported; DGNA to a stunned MS; an ALS call
(the radio check's fields, the called MS transmits, the caller listens), ALS not supported by the MS, not offered,
the de-key statuses 123 and 125 refused from TSI with transmit interrupt off; call diversion set, used (voice and short data diverted with the
notice's fields, status not), cleared by TSI and DIVERTI, to a talkgroup, the upload's blocks from the Proxy Flag,
refusals (off, PSTNI, itself, a talkgroup not allowed), a UDT poll's Div_Cause_Fail | Rust tests (`cargo test`, run by ctest) |
| Control channel features | Scenarios over `t3air` (`rust/src/t3ctltests.rs`): pre-emption and the RC cease, status delivery and alarms, 3 and 4 carriers, the C_BCASTs, mass re-registration, power save, C_MOVE, the asynchronous TSCC, full-duplex and broadcast calls; through the radio layer and `t3sim`: 4 carriers with pre-emption, a duplex call | Rust tests; C host test `rdof_t3unit_tests` |
| Transmit interrupt | Scenarios over `t3air` (`rust/src/t3intests.rs`): a group call interrupted (the request and its C_ACKD, the RC in the TSCC's slot, the talker ceasing, P_PROTECT's fields and count, the interrupter talking, the others hearing it), the RC on a payload carrier, an individual call interrupted, in hang time, a talker that won't cease, an ALS call ended by its caller, the refusals (not the caller, G/I, no call, not a party, a broadcast call, the talker itself, a second interrupter), off by default, the call timer de-key on and off; P_PROTECT against table 7.31 (`t3spec_pdu.rs`); through the radio layer and `t3sim`: a group call interrupted, the RC and P_PROTECT read back from the air, the interrupter's voice header | Rust tests; C host test `rdof_t3unit_tests` |
| C API | Init, a short scenario through the FFI, layouts | C host test `rdof_t3_tests` |
| Packet data on bursts | MS to MS confirmed data and MS to the site's IP gateway (NAT, the gateway's ACK sent down by the site) over the burst path; across the two sites through the broker: confirmed MS to MS (the setup request with its data flag and the `data_pdu` message checked byte for byte but the stream ID), and the gateway's reply to an MS at the other site | `rdof_t3unit_tests` |
| Radio layer | Burst building (CSBK, MBC, CACH with C_SYS_Parms and AT), parsing back, the slot source in the SDR modem (timing, short LC, AT), payload channel regeneration and P_CLEAR, split output frames | C host tests |
| Settings | the `tier3` app: good and bad files, schema | C host tests, `test_schema.js` |
| Network | `setup` item vectors, Tier II auto-answer, records, `cfg` parsing and precedence, cache | C host tests (`rdof_mqttnet_tests`), Python tests in the network services |
| System | Two `rdofradio --sim-air` sites + router + manager over mosquitto: registration, local and wide-area group calls, a private call across sites, a move from site to site, link loss and recovery | network services' `tests/test_e2e_t3_sim.py` (skipped without mosquitto or rdofradio); in-process: `rdof_t3unit_tests` |
| Bench | The Pluto sends a TSCC on the bench (cabled), our receiver decodes it (C_SYS_Parms counter, Aloha, broadcast timing, AT), the bench's decoder confirms the CSBK checks; then a registration and calls from a Tier III radio | owner's go-ahead, a licensed frequency, lowest power |

## 13. Work order

### 13.1 Round 1 (2026-11-05 to 11-06): built

This document; the mqtt-net additions; the `tier3` app in config-v3.md. Rust: the PDU codec, controller and MS
simulator with cargo tests. C: the radio layer, settings, Tier III unit, simulated air, the split carrier, rdofradio's
site, `--sim-air` and `--sim-ext`. Network services: library, router, manager, dashboard, the two-site system test.
Docs. Not run on a device.

### 13.2 Round 2 (from 2026-11-12)

The owner's direction: Rust is the code priority, and the Tier III site is Rust only. Its name is Tier III: the app
type `tier3` (was `trunk`), the build option `RDOF_TIER3`, the modules `t3*`.

| Step | What | Done when |
|---|---|---|
| 1 | The branch on main (merged 2026-11-12) and the Tier III names | host tests pass (done) |
| 2 | Rust only: `t3rf`, `t3cfg`, `t3unit`, `t3sim`, `t3split` ported from C to rust/src (cargo feature `tier3` = `legacy` + `t3`, in rdof_common's one Rust library); the C files removed; the C headers stay the API for rdofradio, whose device, MQTT and socket glue (`rdofradio_t3.c`, `rdofradio_simext.c`) stays C with the rest of that program. A C-modules build (`RDOF_RUST` off) has no Tier III site | done 2026-11-12: host tests (74, and 68 with `RDOF_RUST` off), the cargo tests, the two-site system test |
| 3 | After the port: the modules call each other by Rust path, not through their C API; one copy of each mirror | done 2026-11-12, as step 2 |
| 4 | Carrier timing: carrier A's slots followed the SDR's sample clock and carrier B's the host's, with the offset taken once, so they drifted apart by the SDR's clock error (20 ppm: one slot in 25 minutes). Now carrier A sits on the host's slot grid too, through the stream's sample clock timed against the host's (9.4) | done on the host: an SDR 20 ppm fast for 3 hours stays within 69 us, 2 us of drift (`rdof_radio_utc_tests`); on a file device the site keys on the grid. On the bench: carrier B's bursts stay on carrier A's grid |
| 5 | The Pluto bundle with `tier3` (tools/pluto/build.sh): rdofradio.gz still fits the settings flash's 896 KB with the controller in it | done 2026-11-12: gzip gave 746 KB, too big; the bundle now carries rdofradio.xz (628 KB) |
| 6 | Bench, step 1 (owner's go, a bench TX frequency with a lease, cabled or lowest power): the Pluto runs a one-carrier site (TSCC on TS1, a payload channel on TS2) and a bench receiver checks the TSCC: C_SYS_Parms and its counter, C_ALOHA at least every 4th TSCC slot, the broadcasts' cadence, CRCs, the CACH (TC, AT) | an hour of TSCC decoded with no bad block ; first run 2026-11-12: 30 s from a W12, 997 bursts, every CRC good, an Aloha at least every 4th TSCC slot. The run found the grid moved when netclock stepped the clock after key-up (fixed: carrier 1 keys once the clock settles). Second run, the fixed image: 12 min, 24,519 bursts, no gap, every FEC and CRC good (12,113 CSBKs, 73 MBCs: header residue AAAA, last block 0000), never more than two TSCC blocks between Alohas, the grid within 3 ms. Then simulated MSs (`--air-ms 2`) beside the carrier: both registered, and a group call and an individual call (with its availability check) were granted and decoded on TS2 from the air, header to terminator. The capture showed their voice had no embedded LC (fixed) |
| 7 | Bench, step 2: an MS against the site. In order of reach: a standard Tier III radio, if one is to hand (registration, a group call, a private call with the availability check); RadioFoundry's MS engine once it exists (handoff notes section 2) | registration accepted, a group call heard on the payload channel ; done 2026-11-13 with a commercial portable running RadioFoundry's MS engine (1 W, the Pluto site, simulated MSs beside it with `--air-ms 2`), each checked from both ends (the radio's log, a W12 decoding the outbound and at times the inbound): registration, with one talkgroup and with a talkgroup list (3.17; one talkgroup refused by `tg`: C_ACKD 65h, Response_Info 1101111b, which was wrong: the ADRNULL positions are 0, 1101000b, fixed since); group and private calls both ways, the radio following the grant, playing 5 s of voice and returning (the simulated voice had to vary frame to frame, section 11); a group call to the refused talkgroup refused (2Fh); status to an MS and to a talkgroup and polls both ways (3.10, 3.15); UDT short data both ways, to an MS and to a talkgroup, the blocks byte for byte (the run found the site's own padding wrong for character formats: fixed, annex B). Found on the way: a returned radio went back to the channel on a Late_Entry grant repeat in hang time; repeats in the hang were stopped for a while; the likely cause is the radio leaving early on the idle-filled hang, and they're back (3.2) |
| 8 | Bench, step 3: carrier B on a split radio's TX half (RadioFoundry's split protocol), two payload slots on it | a call granted on carrier B heard there |
| 8a | Bench, two sites: the Pluto's two-carrier site (site 1) and a one-carrier site on a stock duplex MMDVM modem (site 2, tier3-hat.md), on one mqtt-net; Vote_Now every 30 s, VOTE_BLK 6, site priorities (3.11) | done 2026-11-13 with a commercial portable running RadioFoundry's MS engine (50 mW). Each site's Adjacent_Site, Vote_Now and Chan_Freq (its own carriers and the neighbor's TSCC) decoded from the air on the modem site and taken by the radio from both. Equal priorities (2/2): the radio moved to the site 12 dB stronger and stayed. Site 1 at 4 naming site 2 at 2: the radio sampled site 2 on the first Vote_Now (-59 dBm against -50), moved on priority, registered there (area 2) and stayed through four more votes. A site's priority change reached its neighbor's broadcasts over mqtt-net with no restart. The radio's own fixes on the way: its allowed-site list, and re-registration on a move with DMRLA unset |
| 8b | Bench: the composite TSCC (3.19) on the one-carrier modem site, the site's air MSs, a bench receiver on its outbound | done 2026-11-13. An individual call held TS2; a group call then got TV_GRANT on the TSCC (twice) and ran on TS1, both at once on one carrier. The CACH sent P_SYS_Parms with Payload Channel Type 1 from the grant to the P_CLEAR after the hang, then C_SYS_Parms; C_ALOHA came back. The first run aired the TS1 call without its voice header (heard by the TSCC during the grant slots); fixed (3.19, `rev` 2) and seen on air in the next run |
| 8c | Bench: transmit interrupt (3.20) on the one-carrier modem site, the site's air MSs, a bench receiver on its outbound | done 2026-11-13. A talker on TS2, an MS in the call sent status 125 to the talkgroup: the RC cease command took one TSCC slot, C_ACKD followed, the talker ended its superframe with a terminator 0.3 s later, P_PROTECT(EN_PTT_ONE_MS) went out on TS2 and the interrupter's voice header followed 120 ms after it; then its voice, the hang and P_CLEAR. The first run found rdofradio's air MSs didn't read the RC (fixed); the site gave up after 3 s and kept the call, as 6.4.15.1 asks |
| 8d | Bench: packet data across sites (5.10), the SDR site (site 1) and the modem site (site 2) on one mqtt-net, air MSs on both | done 2026-11-13. Confirmed individual data, site 2 to site 1: the far site checked the MS, granted a payload channel, delivered and the caller's call ended done; then site 1 to site 2 the same way. Unconfirmed group data, site 2 to talkgroup 1: every air MS on both sites got the text. The first try failed with MSaway (0x25): no private-call router ran on the broker, so the request on `pc/{dst}/{node}` reached nobody. With the router, the network services' flags fix (bit 2 kept) and a voice individual call checked first, all passed |
| 8e | Bench: the real radio (a commercial portable with RadioFoundry's MS engine, 50 mW) against both sites, the bench offset image merged in (1e2549fe) | done 2026-11-13. Site 1: registration by syscode, by talkgroup and with a list, group and individual calls, status, poll and short data to an air MS (12 of 12); a broadcast and an all-call cleared right after the terminator (radio back on the TSCC 0.5 and 0.7 s after release), a group call after its 3 s hang; IP text messages (UDP 5016) unconfirmed and confirmed. Site 2: the same calls to an air MS on site 1 through the private-call router, and the text messages across sites. Found: UDT short data crossed sites with its pad octets (24 characters arrived as 34 octets), fixed in 12379fa8. Open: on site 2 the radio sends each registration twice although the site answers the first within 180 ms; the radio decodes few of the stock modem's CSBKs (1.4 a second against 8 on site 1) with no CRC errors |
| 9 | Merge into main | the owner's go |

An ETSI conformance suite (`rust/src/t3spec_*.rs`, `host-tests/test_t3spec_rf.c`), written from the standards
apart from the code, found seven deviations, all fixed (2026-11-12):
- An MBC last block's CRC had the header's mask 0xAAAA; it takes none (361-1 7.4.0, B.3.12). A standard radio would
  have dropped every Chan_Freq broadcast.
- An MBC could start in the slot before a due C_ALOHA and push it to the fifth slot (this document's 3.2).
- The simulated MS ignored the Aloha's Mask and Service Function (6.2.1.1.1, 6.2.1.1.2), made 6 attempts for an
  emergency request instead of NRand_NE (10), and sent P_MAINT to the called MS instead of TSI (table 7.32).
- The settings refused SITE 0, which table 6.5 counts (a Small network's 5-bit SITE gives 32 sites). Nothing used
  0 as "no site" (a peer's info is marked by a nonzero syscode, and PAR is never 0), so 0 is now allowed.

Out of round 2, unchanged from section 1.2: dedicated multi-carrier sites beyond two, the standby controller. MS authentication was added on 2026-11-12 (section 3.14), transmit interrupt and packet data across sites on 2026-11-13 (sections 3.20, 5.10).

## 14. Open questions for the owner

1. A licensed frequency pair and an OK for the first bench TSCC (cabled), and later on air.
2. Whether a third-party Tier III radio is available for the interop test, and which.
3. The network identity to use for tests (MODEL/NET), and whether DMR IDs for sites follow the existing 999xxxx test
   range.
4. Whether mqtt-net's keys follow the new name too: info's `trunk` object and a path's `"trunk": true`, status's
   `trunk` (shared with the network services; unchanged so far).
