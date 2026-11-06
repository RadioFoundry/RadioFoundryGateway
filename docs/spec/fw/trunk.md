# Trunked site (DMR Tier III): design and plan

Covers: what the `trunk` app is, how the site controller, the radio layer, the MQTT network and the network
services split the work, the protocol subset, the settings, the network messages, failure and isolation behavior,
the road to multi-site roaming, what is built in this round and what comes later, and how it is tested.

Status (2026-11-05): design. Branch `tier3-trunk`, from `mqtt-net-rebase`.

Sources: ETSI TS 102 361-1 (air interface), -2 (voice), -4 (trunking, V1.12.1). Clause numbers below are 361-4's
unless marked. Clean room: the ETSI text and our own MIT code only. Authentication (361-4 6.4.8, 6.4.9.2, 6.4.10) is
out of scope and needs the owner's go-ahead before any work.

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
| RF layout | 2 carriers: A (SDR TX + SDR RX) carries the dedicated TSCC on TS1 and a payload channel on TS2; B (split radio TX + SDR RX) carries two payload channels. 1 control slot, 3 payload slots. The code takes N carriers. |
| Control channel | C_SYS_Parms short LC with a counting Common_Slot_Counter; C_ALOHA; C_BCAST Adjacent_Site (from the network), Chan_Freq (the site's own channel plan), Local_Time (when the clock is good), Gen_Site_Params; CACH AT driven by the controller |
| Random access | Slotted Aloha, Mask 0, Service Function from load, adaptive Backoff, NRand_Wait from settings; a response to every random access |
| Registration | Open or listed (ID sets); register, de-register, implicit registration from call requests; single-talkgroup subscription (TRGT_ADR_CNTS 01b); Reg_Accepted / Reg_Refused / Reg_Denied; MS_Not_Registered |
| Group voice | C_RAND, C_WACKD/C_QACKD, TV_GRANT (repeated; Late_Entry grants through the call), BTV_GRANT for ALLMSID calls, quasi-transmission trunking (hang time), P_CLEAR on hang expiry, call timer or P_MAINT DISCON |
| Individual voice | C_AHOY availability check (OACSU or FOACSU, a setting), C_ACKU/C_NACKU, Call Answer, PV_GRANT, cancel (Service_Kind 1111b) |
| Queueing | One queue by priority (emergency first, then PRIORTY_SV, then age); C_QACKD; SYSbusy_Refused when full |
| Refusals | C_NACKD(Not_Supported) for every other Service_Kind; Called_Party_Busy, Called_Group_Not_Allowed, Perm_User_Refused as provisioning says |
| Network | Wide-area group calls both ways; individual calls across sites with an availability check over the network; Tier II to Tier III and back; registration records; site description; provisioning from the manager, cached at the site |
| Services | The site controller (Rust, in RFG), the radio layer (C, in RFG), the router's additions, the manager and its dashboard (Python, network services), the MS simulator |
| Tests | Rust unit and scenario tests with the simulated air, C host tests of the radio layer and the network messages, a whole-system run with simulated radios over a real broker |

### 1.2 Later (the design leaves room; nothing here is built)

Packet data and UDT short data, talkgroup list subscription (UDT upload), power save, mass re-registration,
Vote Now, C_MOVE and a second TSCC, composite TSCC, TSCCAS, the asynchronous (hibernating) TSCC, DGNA,
stun/revive/kill, gateways (PSTN, PABX, dispatcher), duplex calls, ambient listening, emergency pre-emption of a
running call, a standby controller, trunking on the W12 (the LR2021 can't stay keyed: slot-timing.md), two carriers
out of one SDR, authentication (owner's go-ahead first), encryption beyond pass-through.

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
| Radio layer | C, `common/src/trunkrf.c` (portable) + `sdr/cli/rdofradio_trunk.c` | Burst building and parsing (BPTC, CRC masks, slot type, sync, CACH, short LC), the per-slot hook into the SDR modem, the payload channels (local repeat, network voice in, idle, the controller's payload PDUs), the split radio for carrier B, the receive path from the SDR's channels |
| Network side of the site | C, `common/src/mqttnet.c` and rdofradio | mqtt-net as today, plus the setup item, registration records, the trunk part of `info` and `status`, provisioning topics and their cache |
| MS simulator | Rust, `rust/src/t3ms.rs`, `t3air.rs` | Simulated radios that follow 361-4 (hunt omitted, access, registration, calls, answers), and a slot-exact air model with collisions. Used by the Rust tests, the C tests, and rdofradio's `--sim-air` |
| Router | Python, network services | As today, plus location from registration records and an early refusal for an unknown called party |
| Manager | Python, network services | The provisioning source of truth: network identity, sites, talkgroups, radios, permissions, timers. Publishes retained `cfg` topics; sends commands; serves the dashboard and a small REST API |
| Dashboard | HTML + JS served by the manager | Nodes, sites and their channels live, registrations, calls, last heard, router counters, provisioning editor |

Why Rust for the controller: the owner's choice for new logic in RFG; the controller is pure state machines over
fixed tables, which `no_std` Rust does well, and the same crate holds the MS simulator so the PDU codec exists once.
The radio layer stays C because the FEC, the SDR modem and the split code it drives are C.

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
| Channel plan | The MS must know it: programmed in the radio, or learned from the C_BCAST Chan_Freq broadcasts (MBC + BC_AP with absolute Tx/Rx per channel), sent for each of the site's carriers every `chan_freq_s` (default 10 s) |
| Colour code | One CC for the site (strategy (c), 6.2.1.3: payload channels use the TSCC's CC). Inbound payload PDUs with another CC are dropped |
| Timing | Aligned TSCC (Offset 0). Site Timeslot Synchronization 0: carrier B's split radio is aligned only to 1-2 ms, so payload channels send idles for 4 CACH before the MS's first sync opportunity (6.6.1.6.2.2) |
| Version | 100b (1.11.1, the latest code; V1.12.1 has none of its own) |
| Active_Connection | 1 while the backbone (mqtt-net) is connected, else 0 |

### 3.2 The control channel schedule

Carrier A TS1 sends a CSBK or MBC block every slot (every 60 ms). The controller picks, in order:

1. A response that is due: an acknowledgment, a C_AHOY, an initial grant (each initial grant twice, in consecutive
   TSCC slots).
2. The continuation block of an MBC already started.
3. A grant repeat that is due: every `grant_every_s` (default 1.2 s) per active call, with Late_Entry = 1, through the
   call and its hang time (as the RadioMax reference does).
4. A broadcast that is due: Adjacent_Site per neighbor every `adjacent_s` (default 5 s), Chan_Freq every
   `chan_freq_s`, Local_Time every `local_time_s` (default 6 s) when the clock is synchronized, Gen_Site_Params
   every 10 s.
5. C_ALOHA, Mask 0, address ADRNULL.

At least one C_ALOHA goes out every 4 TSCC slots whatever else is due (an MS waits for an Aloha before it accesses,
6.2.1.3).

CACH on carrier A: TC alternates; the short LC is C_SYS_Parms (SLCO 0010b) with the Common_Slot_Counter, which
advances every 4 slots; Reg matches C_ALOHA. AT = 1 in the CACH before the inbound slot where a solicited response
is expected (after C_AHOY, after a response that solicits), else 0. Carrier B, payload only, sends P_SYS_Parms
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
  C_NACKD(MS_Not_Registered).
- List subscription (TRGT_ADR_CNTS 10b) needs a UDT upload, which isn't built: the site answers C_ACKD with reason
  0110 0101b (registration accepted, group refused, 6.4.4.1.13.1), and the MS's talkgroups come from its calls and
  the provisioning.

### 3.5 Calls

Group call (talkgroup, ALLMSID):

1. C_RAND (Service_Kind 0001b). Checks: registered (if required), allowed on this talkgroup (provisioning), the
   talkgroup allowed at this site.
2. A free payload slot: TV_GRANT (BTV_GRANT for ALLMSID/ALLMSIDL/ALLMSIDZ) twice, then repeats. None free: C_QACKD and
   into the queue; queue full: C_NACKD(SYSbusy_Refused).
3. The payload channel starts sending idles at once, then repeats the inbound voice of whoever talks, and plays
   network voice for the talkgroup.
4. Hang (`hang_s`, default 3 s, TV_Hangtime) after each item; any member may talk again during it. The call ends on
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

Every C_RAND Service_Kind other than 0000b, 0001b, 1001b (Call Answer), 1110b and 1111b gets
C_NACKD(Not_Supported) (4.2). Gateway addresses (PSTNI and the rest) as targets also get Not_Supported. Data,
status and UDT requests likewise.

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
| `queue_max`, `queue_s` | queue size, longest wait | 16, 10 | 1-32, 1-60 |
| `net_queue_s` | network call wait for a slot | 2 | 0-10 |

## 4. Settings (config-v3 `trunk` app)

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
    {"name": "site", "type": "trunk",
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
| `timers` | object | section 3.8 | |
| `broadcast` | object | section 3.2 | `adjacent_s`, `chan_freq_s`, `local_time_s` (0 = off) |

Checks (refusals name the key): exactly one `tscc` carrier; every carrier's `out` on a device that transmits and its
`in` on a device that receives; the TSCC carrier's `out` on a full-duplex SDR (a split radio can't hold a control
channel: its key limit is 600 s); `chan` unique; the `system` numbers inside the model's ranges; at most 4 carriers;
a frequency used by a `trunk` app can't be used by another app or a talk path.

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
          "services": ["reg", "group", "private", "allcall"], "reg": true}
```

Neighbors: each site builds its Adjacent_Site list from the `info` of the other online nodes with a `trunk` object of
the same MODEL and NET, unless `cfg/node/{node}` gives `neighbors` explicitly.

`status` (retained, every 10 s and on change), for all nodes; a trunk site adds:

```json
"trunk": {"connected": true, "regs": 41, "queue": 0,
          "channels": [{"chan": 1, "lcn": 0, "use": "tscc"},
                       {"chan": 1, "lcn": 1, "use": "call", "src": 3100123, "dst": 91, "group": true, "since": "..."},
                       {"chan": 2, "lcn": 0, "use": "idle"}, {"chan": 2, "lcn": 1, "use": "hang", "dst": 3100}],
          "counters": {"aloha": 0, "rand": 0, "collisions": 0, "grants": 0, "refused": 0, "queued": 0}}
```

`activity` events added: `reg` (`src`, `action` register/deregister/expire/deny/refuse, `tg`), `grant` (`src`,
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
"priority": 0, "emergency": true, "tg": [91, 3100]}`. `enabled: false` = Reg_Denied at every site, and an existing
registration is removed. `tg` adds static talkgroups for that MS.

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

### 5.8 Security

- Broker: TLS on the backbone (`transport: mqtt/tls`), a username and password per node, ACLs per role (mqtt-net.md
  section 11 plus section 5.2 here). Only the manager writes `cfg/#` and `cmd`; only the router writes `dl/#` and
  `where/#`.
- A node trusts `cfg` because of the ACL. Signed provisioning (a manager key, verified on the node) is a later step;
  the `rev`/`at` fields leave room for a `sig` field.
- The dashboard and REST API need a token (manager setting); TLS by a reverse proxy or the manager's own.
- Tier III authentication is out of scope (owner's rule).

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
| A radio finds a neighbor | Adjacent_Site broadcasts | each site, from the others' `info` |
| A radio moves | it registers at the new site (new SYS_AREA) | the new site writes `node/{new}/ms/{id}`; the router's `where/{id}` moves; the old site sees `where` and deletes its record |
| A group call reaches the new site | the record's talkgroups | the new site subscribes; the broker delivers |
| A private call reaches the moved radio | the setup request | the router places it by the newest record |
| A call in progress | Tier III has no handover | the radio re-registers and late-enters from the repeated grant |
| The network splits | | each part serves its own; on rejoin the newest record wins |

Later steps, none needing a redesign: registration areas spanning several sites (`area_bits` smaller than SITE), a
home registry and per-radio permissions checked across sites (the manager already holds them), C_MOVE for load
sharing, Vote Now with priorities from the manager.

## 8. Site controller API (`rdof/t3.h`)

The Rust exports a C API (the repo's convention: the header is the contract; structs mirrored `#[repr(C)]` and listed
in `rust/src/layout.rs` and `rust/layout.c`).

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
bool   rdof_t3_command(rdof_t3_site_t *s, const char *cmd, uint32_t arg1, uint32_t arg2);

/* outputs: drained after each call */
bool   rdof_t3_event(rdof_t3_site_t *s, rdof_t3_event_t *ev);
void   rdof_t3_status(const rdof_t3_site_t *s, rdof_t3_status_t *st);
```

`rdof_t3_slot_out_t` holds, for slot k: the TSCC PDU (`dt`, 10 bytes) when k is a TSCC slot, the AT bit for the
CACH at k+1, the 28-bit short LC for the 4-slot cycle when k % 4 == 0, and for each payload channel a control PDU to
send instead of voice or idle (P_CLEAR, P_GRANT) if any.

Events (`rdof_t3_event_t`, a tagged struct): `CH_OPEN` (ch, call: src, dst, group, kind, emergency, source rf/net,
stream), `CH_CLOSE` (ch, why), `NET_PUBLISH_SETUP` (setup message), `NET_SUBS` (the talkgroup set changed),
`RECORD` (write or delete a registration record), `ACTIVITY` (reg/grant/queue/setup event fields), `NET_DROP` (a
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

`rdof_trunkrf_ch_t` (portable C): per payload channel (carrier, LCN):

- inbound bursts (from the receiver, MS sync) on an open channel: regenerated (`rdof_duplex_data`, `rdof_voice_regen_*`)
  and queued for the outbound slot one frame later; voice activity to `rdof_t3_rx_voice`; CSBKs (P_MAINT) to
  `rdof_t3_rx_pdu`; the frames to the mqtt-net uplink (with the site's path) when the call is wide-area;
- network bursts for the channel's call (from the mqtt-net downlink, after a 240 ms prebuffer) queued the same way,
  unless RF holds the channel;
- the controller's payload PDUs (P_CLEAR) take precedence over voice;
- idle bursts fill.

Outbound sinks: carrier A's channels are read by the slot source (9.1); carrier B's are written to the split radio
as an MMDVM duplex modem (`rdof_split_t`): DMR_START on the first open channel, DMR_DATA1/2 per slot in order,
DMR_SHORTLC with P_SYS_Parms, DMR_START 0 when both channels close.

### 9.3 Receive

The SDR's receiver channels A-in and B-in feed the trunk layer straight from `rdof_sdr_rx_t.on_burst` (with the
burst's family, CC, slot and position), not through the modem's 34-byte DMR_DATA path:

- A-in TS1 (the TSCC inbound): CSBK / MBC / UDT with a good CRC to `rdof_t3_rx_pdu(ch 0)`; a data-sync burst with a
  bad CRC to `rdof_t3_rx_bad`.
- A-in TS2, B-in TS1/TS2: to their payload channel (9.2).
- The slot of a carrier A inbound burst comes from the modem's own outbound timing (inbound slot n is under outbound
  slot n, aligned timing); carrier B's from the split radio's epoch, as the split repeater does.

### 9.4 rdofradio

- `--config` with a `trunk` app: rdofradio opens the SDR full duplex for carrier A, the split radio's serial port for
  carrier B (`port` on its device, as a split TX half), receiver channels for every carrier's `in`.
- The trunk unit replaces the repeater/transceiver unit in the main loop: `trunk_poll(now)` every 20 ms drains
  events, runs the payload channels, and feeds mqtt-net.
- `--state-dir DIR`: the `cfg` cache.
- `--sim-air N`: no SDR, no split radio: the trunk unit runs against the MS simulator (N radios, scripted by
  `--sim-script FILE` or a default mix), through the full burst path (the simulator's PDUs are built into bursts and
  parsed back), with real mqtt-net. Two rdofradios with `--sim-air` and different `site` numbers make a two-site
  system on one computer.
- Rust in the Linux build: `rust.cmake` gains `FEATURES`; the Linux tools build `rdof_rust` with only the `t3`
  feature (so the C modules stay C), and the Pluto container installs rustup with `armv7-unknown-linux-gnueabihf`.

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
- registration (and de-registration), with a subscribed talkgroup;
- group and individual call requests; follows grants to the payload channel; talks for a set time (voice header,
  voice bursts, terminator), P_MAINT DISCON at the end of an individual call; returns on P_CLEAR or TV_Inactive;
- answers C_AHOY: accept, busy, no answer, or FOACSU alerting then answer, as configured;
- emergency and priority options; cancel.

`t3air`: a slot-exact air between one site controller and N MSs: TSCC outbound to every MS on the TSCC; inbound
collisions (two MSs in one inbound slot: neither is heard, and the site gets `rx_bad`); payload channels with their
members; optional loss rate and delay. Scenarios script MS actions by time and check outcomes.

Used by: `cargo test` (scenarios), the C host tests (through the C API), rdofradio `--sim-air`.

## 12. Tests

| Level | What | Where |
|---|---|---|
| PDU codec | Every PDU the site sends or reads: field packing against the 361-4 tables, round trips, known vectors from a standard site's captures (decoded values, not checks) | Rust unit tests |
| Controller | Scenarios over `t3air`: registration open/listed/denied, collisions and backoff, group call, queue and grant on release, late entry repeats, hang and P_CLEAR, call timer, individual OACSU and FOACSU, no answer, cancel, busy called party, Not_Supported, network group in and out, network setup both ways, Tier II implicit setup, records reload, provisioning changes, link loss | Rust tests (`cargo test`, run by ctest) |
| C API | Init, a short scenario through the FFI, layouts | C host test `rdof_t3_tests` |
| Radio layer | Burst building (CSBK, MBC, CACH with C_SYS_Parms and AT), parsing back, the slot source in the SDR modem (timing, short LC, AT), payload channel regeneration and P_CLEAR, split output frames | C host tests |
| Settings | the `trunk` app: good and bad files, schema | C host tests, `test_schema.js` |
| Network | `setup` item vectors, Tier II auto-answer, records, `cfg` parsing and precedence, cache | C host tests (`rdof_mqttnet_tests`), Python tests in the network services |
| System | Two `rdofradio --sim-air` sites + router + manager over mosquitto: registration, local and wide-area group calls, a private call across sites, a move from site to site, link loss and recovery | network services' `tests/test_e2e_t3.py` (skipped without mosquitto) |
| Bench | The Pluto sends a TSCC on the bench (cabled), our receiver decodes it (C_SYS_Parms counter, Aloha, broadcast timing, AT), the bench's decoder confirms the CSBK checks; then a registration and calls from a Tier III radio | owner's go-ahead, a licensed frequency, lowest power |

## 13. Work order

1. This document; the mqtt-net additions in mqtt-net.md; the `trunk` app in config-v3.md.
2. Rust: PDU codec (`t3pdu`), controller (`t3site`), simulator (`t3ms`, `t3air`), C API, cargo tests.
3. Build: `rust.cmake` features, host test target, Linux build of the `t3` feature, Pluto container.
4. Settings: `trunk` app parse, schema, fixtures.
5. Radio layer: burst helpers, SDR modem slot source, payload channels, split output, receive path; host tests.
6. mqtt-net: `setup` item, Tier II answer, records, status, `cfg` and cache; host tests.
7. rdofradio: trunk unit, `--sim-air`, `--state-dir`.
8. Network services: library, router, manager, dashboard; tests; the system test.
9. Docs: reference.md (layout, limits), overview.md roles, rxtx-combinations.md, transmitters.md.
10. Pluto bundle build; bench run when the owner says.

## 14. Open questions for the owner

1. A licensed frequency pair and an OK for the first bench TSCC (cabled), and later on air.
2. Whether a third-party Tier III radio is available for the interop test, and which.
3. The network identity to use for tests (MODEL/NET), and whether DMR IDs for sites follow the existing 999xxxx test
   range.
