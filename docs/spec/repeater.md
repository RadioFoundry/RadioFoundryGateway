# Base station behavior: duplex repeater, single frequency repeater, simplex hotspot

What the gateway does on air and toward HBlink4 when it drives an MMDVM modem as a base station:

- **duplex repeater**: two frequencies, both slots, an MMDVM duplex modem (MMDVM_HS dual hat, MMDVM board);
- **SFR**: a single frequency repeater, a RadioFoundry radio in SFR mode that repeats by itself;
- **simplex hotspot**: one frequency, one slot (the roamer, and any simplex MMDVM modem);
- **DCDM gateway**: one frequency, both slots in direct mode, two simplex calls at once (a DCDM modem).

This document is the source of truth for that behavior. The code that must follow it is in `common/src/`
(`repeater.c`, `duplex.c`, `mmdvm.c`, `txvr.c`). Related: [slot-timing.md](slot-timing.md) (UTC slots, split
halves), [fw/roamer.md](fw/roamer.md) (the roamer), [fw/radio.md](fw/radio.md) (the modem side). Change this spec
first when behavior changes, then the code.

## 0. Sources, and how rules are justified

Written from these sources only:

- ETSI TS 102 361-1 V2.6.1 (2023-05), "-1", and ETSI TS 102 361-2 V2.4.1 (2017-10), "-2". Clauses are cited as
  "-1 5.2.1.4".
- RadioFoundry (MIT), commit 1ce780d: `docs/spec/mmdvm-protocol.md` ("MP"), `docs/spec/split-duplex.md`
  ("SDS"), `docs/spec/sfr.md`, and `core/dmr/bsOutbound.c` (BS idle and underrun handling).
- The MMDVM serial protocol, for interoperability only, as MP describes it.
- This project's own operational needs.

No MMDVMHost, MMDVM, MMDVM_HS, DMRGateway, hblink3, dmr_utils3, ok-dmrlib or other GPL or AGPL source was opened
to write it. Where MP reports what some host program does, that is interop context, never the reason for a rule.

Each rule has an id (R4.2) and a basis:

- **ETSI**: required or defined by the standard (clause given).
- **MMDVM**: the modem protocol's contract; interop.
- **RadioFoundry**: taken from a RadioFoundry spec (section given).
- **Design**: this project's choice, with the reason in one line.

"Must" is required. "Should" is recommended; code that does otherwise says why.

## 1. Terms and time base

- **Slot**: 30 ms. On the outbound each slot is a 27.5 ms burst plus a 2.5 ms CACH; each logical slot (TS1, TS2) gets
  one burst every 60 ms (-1 4.2, 4.5).
- **Superframe**: six voice bursts A-F of one slot, 360 ms. A carries the voice sync; B-E carry the embedded LC; F
  carries RC, privacy information or the Null embedded message (-1 5.1.2.1, 7.1.3).
- **Data types** (slot type, -1 9.3.6): 0000 PI header, 0001 voice LC header, 0010 terminator with LC, 0011 CSBK,
  0100 MBC header, 0101 MBC continuation, 0110 data header, 0111 rate 1/2 data, 1000 rate 3/4 data, 1001 idle,
  1010 rate 1 data, 1011 USBD (unified single block data).
- **Slot numbering**: what is heard on inbound slot N is repeated on outbound slot N (offset channel timing, -1
  5.1.1.2, table 5.1). The modem labels each inbound burst with its slot (DMR_DATA1 / DMR_DATA2).
- **Host**: the gateway (the ESP32 firmware). **Modem**: the MMDVM-protocol device. **Network**:
  HBlink4, which delivers calls as DMRD streams (a stream id per call) and receives what the BS hears.
- **Traffic**: a call's own bursts. **Filler**: bursts the BS makes up: call hang terminators, headers waiting for
  voice, idle, silence fill.
- **Open call**: a voice or data call on a slot that has started and not yet ended.

## 2. Host and modem

R2.1. The modem frames the outbound. While keyed (DMR_START 1) it transmits continuously on both slots, a CACH before
every burst with the slot number (TC), the inbound access type (AT) and the short LC rows the host gave it
(DMR_SHORTLC). *Basis: MMDVM (MP 2.10; SDS 4.1, 5.1, 5.2); ETSI -1 4.5, 4.6.1.*

R2.2. The modem sends the Idle message (-1 D.2) in any slot it has nothing queued for while keyed. The host relies on
this for the activated BS and for channel hang, and never writes idle bursts just to keep the carrier up. *Basis:
MMDVM (SDS 5.1 describes the same for RadioFoundry's TX half); ETSI -2 5.1.1.1.3 (after activation the BS sends Idle on
both slots). To verify on each modem type: open question 2.*

R2.3. The modem sets AT from its own receiver. MMDVM has no command for AT. ETSI wants AT "busy" while a slot repeats
and during its call hang (-2 5.1.1.2, 5.1.1.3); the host can't enforce that on an MMDVM board. A RadioFoundry radio as the
modem marks a slot busy while its outbound carries a call, the hang included (SDS 3.6). *Basis: MMDVM; open
question 1.*

R2.4. The host decides everything else: which bursts go out, in what order, and their content (sync, slot type, EMB,
LC, embedded signalling), the call hang, and keying. It writes complete 264-bit bursts with the control byte set
for what the burst is (never 00). *Basis: MMDVM (MP 2.8: a modem trusts a non-zero control byte); Design: the
host knows what each burst is, the modem shouldn't have to guess.*

R2.5. The modem receives with the configured color code (SET_CONFIG). Where the host reads a color code itself (the
EMB, for late entry) it must match ours. *Basis: ETSI -1 5.2.1.4 note (another color code is co-channel
interference); MMDVM.*

## 3. Activation, hang and deactivation (duplex)

The states follow -1 G.2 and -2 5.1.1: the BS is **hibernating** (transmitter off) or **active** (keyed). Each slot
of an active BS is **repeating**, in **call hang**, or in **channel hang**.

R3.1. **Wakeup.** While hibernating the BS listens for the BS outbound activation CSBK (BS_Dwn_Act, CSBKO 111000,
-2 table 5.1, 7.1.2) with our color code and a good CRC. On one it keys, starts T_MSInactiv, and both slots go to
channel hang. The wakeup itself is not repeated. *Basis: ETSI -2 5.1.1.1.1, 5.1.1.1.3. The minimum ETSI check is
color code plus CSBK; checking the CSBKO and CRC as well is allowed ("manufacturers may also validate ... CSBKO").*

R3.2. **Network activation.** A network stream admitted for an enabled slot (R7.1) also keys the BS. *Basis: Design:
ETSI has no network-side activation; the outbound must be up for radios to hear the call.*

R3.3. **Key-up lead.** When the BS keys from hibernating for a network call, the outbound must run at least 360 ms (6
bursts per slot) before the first header. R7.5 gets this for free by keying at the stream's first burst, while it
prebuffers. When the BS is already keyed, no lead is needed. *Basis: Design from ETSI timing: receivers have to
frame-sync to an outbound that just appeared before they can take the header; -1 F.1 bounds an MS's wait for an
outbound at T_SyncWu ≤ 360 ms and its sync attempt at T_ChSyncTo ≥ 390 ms.*

R3.4. **T_MSInactiv** (BS inactivity, default 5 s, configurable). It restarts on the wakeup, on every inbound burst
repeated, and at the expected air time of the last network burst written (write time plus that slot's modem
backlog). Filler (call hang terminators, idle, silence) does not restart it. When it expires and nothing but
filler is queued, the BS unkeys (DMR_START 0). *Basis: ETSI -1 F.1 (default 5 s), -1 G.2.1, -2 5.1.1.5.*

R3.5. **Call hang** (T_CallHt, per slot, default 3 s, configurable, 0 allowed). When a voice call on a slot ends
(terminator, lost, or the network stream ends), the slot sends Terminator with LC bursts carrying that call's LC until
T_CallHt expires, then goes to channel hang. With T_CallHt = 0 the slot sends exactly one terminator. Data and CSBK
transmissions have no call hang. *Basis: ETSI -1 5.2.1.4, -2 5.1.1.3, -1 F.1 (default 3 s); -1 5.1.2.3 (a voice
transmission ends with a terminator).*

R3.6. **Channel hang** (T_ChHt = T_MSInactiv − T_CallHt, so 2 s by default): the slot carries idle (R2.2). If
T_MSInactiv is configured below T_CallHt, the BS uses T_CallHt for it: a call hang always completes. *Basis: ETSI
-1 F.1.*

R3.7. Any new call on a slot (RF or network) ends that slot's call hang or channel hang at once; call hang filler not
yet on air is dropped. *Basis: ETSI -1 G.2.2 (BOR in Call_Hangtime goes to Repeating_Slot).*

R3.8. The call hang is a timer, not a queue: its terminators are written just in time, at most two ahead in the
modem (R10.3). *Basis: Design: lets R3.7 happen without DMR_ABORT and without delaying the next call.*

R3.9. **CW ID** (optional, `cwid`): after the BS unkeys, if it sent anything since the last ID and at least
`cwid_every_s` (default 900 s) has passed, the host waits for the modem to stop transmitting, puts it in idle mode,
sends SEND_CWID, waits again, and returns it to DMR mode. *Basis: Design (station identification where the license
needs it); MMDVM (CW only from idle, MP 2.11).*

## 4. RF to RF: the local repeat (duplex)

Only enabled slots are repeated. Everything heard still goes up (section 7).

| Inbound burst | What the BS does | Rule |
|---|---|---|
| Voice LC header, no open call | starts an RF voice call | R4.1, R4.2 |
| Voice LC header, same LC as the open call | absorbed (the BS makes its own headers) | R4.2 |
| Voice LC header, different LC | ends the open call (as lost), starts the new one | R4.3 |
| PI header | repeated inside an RF voice call, after the headers, before A | R4.4 |
| Voice A-F | repeated, regenerated (section 5), positions kept whole (section 6) | R4.5 |
| Voice B-E with no open call | late entry | R4.6 |
| Terminator with LC | ends the call; the call hang replaces the radio's terminators | R4.7 |
| CSBK | repeated if its CRC is good, except BS_Dwn_Act and during a network voice call | R4.9, R8.3 |
| Data header | starts an RF data call if its CRC is good; repeated | R4.8 |
| Rate 1/2, 3/4, 1 data | repeated while the data call expects blocks | R4.8 |
| MBC header / continuation, USBD | should be repeated, CRC-checked | R4.10 |
| Idle | never repeated | R4.11 |

R4.1. An RF voice call starts on a voice LC header whose full LC decodes (BPTC(196,96), RS(12,9) with the header CRC
mask) and has a source id. Whatever the slot carried before is dealt with by R8.1 (network traffic) or R3.7 (hang
filler). *Basis: ETSI -2 5.1.1.2 (BOR on a voice LC header); -1 7.1.1, B.3.6, B.3.12.*

R4.2. **Headers.** From the start of a voice call until its first voice burst A goes out, the outbound slot carries
the call's voice LC header, at least H = 3 of them, and more as long as voice hasn't arrived. Headers the radio
repeats are absorbed into these. *Basis: ETSI -1 5.1.2.2 (burst A is immediately preceded by a voice LC header or a
PI header), -2 5.1.1.6.1.1 (a single site BS repeats the voice header until the authorized MS's bursts arrive).
H = 3 is Design: 180 ms, so a receiver that loses two header bursts to a fade still gets the LC before voice; the
embedded LC repeats every 360 ms after that. Each header beyond what the radio sent delays the whole call by 60 ms,
so not more.*

R4.3. A voice LC header with a different LC (source or destination) while an RF voice call is open ends that call as
if lost (its terminator and call hang, R4.7) and starts the new one. *Basis: Design: a new header is a new speech
item; repeating it under the old LC would show the wrong talker.*

R4.4. A PI header is repeated only inside an RF voice call, after the voice LC headers and before burst A. *Basis:
ETSI -1 5.1.2.2 (the voice LC header precedes the PI header; A immediately follows either).*

R4.5. Voice bursts of the open call are repeated as they are heard, regenerated per section 5, under the voice
stream rules of section 6. *Basis: ETSI -2 5.1.1.2.*

R4.6. **Late entry.** Voice heard on a slot with no open call: the BS collects the embedded LC fragments of bursts B,
C, D and E of one superframe (their EMB color code ours, their LCSS first, continuation, continuation, last). When the
four decode (-1 B.2.1, with the 5-bit checksum, -1 B.3.11) to a full LC with a source id, the call starts: headers per
R4.2, then voice from the next burst A. Bursts heard between the decode and that A (E and F) are not repeated. A voice
sync burst before the four are in hand starts the collection over. *Basis: ETSI -1 7.1.3 (late entry from the
embedded LC), -1 5.1.2.2 (A follows a header). Design: dropping at most two bursts (120 ms) of a call that already
lost its start costs less than a superframe of silence.*

R4.7. **End of an RF voice call.** A terminator heard ends it. So does DMR_LOST from the modem, or no burst on the slot
for 1.0 s (a backstop). Either way the BS sends the call's own terminator and the call hang (R3.5). *Basis: ETSI
-2 5.1.1.3. Design for loss: a lost call must still end cleanly on the outbound. 1.0 s is longer than the modem's own
lost timer (about 0.5-0.8 s, MP 2.9), so DMR_LOST normally comes first.*

R4.8. **RF data.** A data header with a good CRC (-1 B.3.8, mask -1 B.3.12) opens a data call that expects the header's
blocks to follow (BF, -1 9.3.21: every block after the first header, additional header blocks included). Each block is
repeated (regenerated per R5.6) and counted; an additional header block counts as a block and is repeated. The call
ends after the last block, at once if BF is 0, or after 1.0 s without a block. A data header while a data call is open
on the slot: an additional header block (proprietary, DPF 15) is one of its blocks (counted); a header that doesn't
check is dropped; any other header with a good CRC is the next packet: the open call ends there (a block of it was
lost) and the header opens a data call of its own. One after the call ended opens a new data call too, so a chain
(several packets to one destination in one transmission, the headers back to back after one set of preambles and
before one terminator, as RadioFoundry radios send unconfirmed data) is repeated packet by packet, keyed throughout.
*Basis: ETSI -1 5.1.3, 8.2, 9.3.21; Design for the 1.0 s, as R4.7, and for the next packet's header ending the call
(the old rule, every header in the call counted as a block, lost the next packet after a lost block).*

R4.9. **RF CSBK.** A CSBK with a good CRC is repeated (BPTC regenerated), whether the slot is in channel hang or call
hang, except: BS_Dwn_Act (R3.1), and while the slot carries a network voice call (R8.3). Its source and destination
are remembered for echo suppression (R8.4). A CSBK is not a call: no hang, and no change to the activity unless it is
a preamble (R9.2). *Basis: ETSI -2 5.1.1.4.*

R4.10. **MBC and USBD** should be repeated: an MBC header and a USBD only with a good CRC (CRC-CCITT with their
masks, -1 B.3.8, B.3.12, -1 8.2.3), MBC continuations after a good header until the block marked last, all BPTC
regenerated. *Basis: ETSI -2 5.1.1.2 (a BS repeats what it receives); -1 6.2, 8.2.3. Design: none of the radios in
use is known to send them, so this is low priority; see open question 4.*

R4.11. Idle bursts are never repeated: idle is BS filler (-1 D.2), and the BS makes its own (R2.2). *Basis: ETSI.*

## 5. Regeneration: what the BS rebuilds

Every burst the BS transmits is rebuilt for this repeater. Nothing goes out with the sender's framing.

R5.1. **Sync**: BS sourced, voice sync (755FD7DF75F7) on burst A, data sync (DFF57D75DF5D) on every data-sync burst.
*Basis: ETSI -1 9.1.1, table 9.2.* (Simplex: section 11.)

R5.2. **Slot type** on every data-sync burst: our color code and the burst's data type, Golay(20,8). *Basis: ETSI
-1 9.1.3, B.3.1.*

R5.3. **EMB** on bursts B-F: our color code; the PI flag as received (0 when the inbound EMB doesn't decode); LCSS by
position: B first fragment (01), C and D continuation (11), E last fragment (10), F as received (single fragment, 00,
when it doesn't decode). QR(16,7). *Basis: ETSI -1 9.1.2, 7.1.3, B.3.2.*

R5.4. **Voice LC header and terminator** are rebuilt from the call's 9-byte LC: RS(12,9) parity with the header or
terminator CRC mask, BPTC(196,96), slot type, BS data sync. The same LC is used for the whole call, hang included.
The radio's own headers and terminators are not passed. *Basis: ETSI -1 7.1.1, 7.1.2, B.1.1, B.3.6, B.3.12;
-1 5.2.1.4 (hang messages carry the call's IDs).*

R5.5. **CSBK, data header, PI header**: BPTC(196,96) decoded (corrected) and encoded again. CSBK and data header go
out only with a good CRC. The PI header's CRC is not checked: a receiver of an encrypted call checks it itself, and a
corrected PI header is more use to it than none. **USBD and MBC** blocks are passed as heard (slot type and sync
rebuilt, R5.1): they're rare on the networks this serves, and regenerating them is left for later. *Basis: ETSI -1
B.1.1, B.3.8, B.3.12. Design for the PI header and for USBD / MBC.*

R5.6. **Data blocks.** Rate 1/2: BPTC corrected and encoded again when the decode succeeds, else the payload as heard.
Rate 3/4: payload as heard (no trellis decode and encode). Rate 1: as heard (it has no FEC). Slot type and sync are
always rebuilt. *Basis: ETSI -1 B.1.1, B.2.4, B.2.5. Design for rate 3/4: our trellis decoder can't say when it
failed; re-encoding a wrong decode would hand the receiver a clean but wrong block, while bits passed as heard still
get the receiver's own trellis decode and its CRC.*

R5.7. **AMBE FEC.** For calls whose LC has FID 0x00 (standard) or 0x10 (Motorola), each burst's three 72-bit AMBE+2
frames are FEC decoded (corrected) and encoded again. Other FIDs pass as heard. *Basis: Design: correcting at the
repeater stops bit errors adding up over two hops; the two FIDs are the ones known to carry standard AMBE+2 frames in
the -1 6.1 vocoder socket. Encrypted voice is safe: the FEC sits outside the encryption.*

R5.8. **Embedded LC.** Bursts B-E of each outbound superframe carry the embedded LC that the previous inbound
superframe's B-E decoded to; when that didn't decode (and for the first superframe), the call's own LC. All four
fragments of one outbound superframe come from the same LC. *Basis: ETSI -1 7.1.3 (all four fragments within one
superframe, starting at B), B.2.1. Design: passing on what the radio put there keeps the other full LC PDUs radios
send in the embedded field (talker alias, GPS info), one superframe late.*

R5.9. **Burst F** keeps its embedded field as heard: on the inbound it is the Null embedded message or privacy
information, and the outbound F may carry "repeated inbound information related to Privacy". *Basis: ETSI -1
7.1.3.1, 7.1.3.2.*

R5.10. **Silence burst** (R6.2, R6.3): three AMBE+2 silence frames; sync, EMB and embedded field for its position
(R5.1, R5.3, R5.8); the Null embedded message (-1 D.1) on F. *Basis: ETSI -1 D.1; Design.*

R5.11. **Idle burst**, when the host writes one (simplex lead-ins never do; see R2.2): the Idle message of -1 D.2,
BPTC(196,96), slot type with our color code and data type 1001, BS data sync. *Basis: ETSI -1 7.3,
D.2.*

## 6. Voice stream rules (RF repeat and network, all modes)

These keep every superframe on air whole, whether the bursts come from the inbound or from the network.

R6.1. Outbound positions follow the source: a voice sync burst is A, a burst numbered n (1-5) is B-F. *Basis: ETSI
-1 5.1.2.1.*

R6.2. **Gap fill.** When a burst arrives at a position that isn't the next one, the missing positions are first filled
with silence bursts (R5.10). *Basis: ETSI -1 5.1.2.1 (complete superframes); MP 2.8 (fill a missed burst at its
position rather than skip it).*

R6.3. **Underrun fill.** While a voice call is open on a slot, no burst of it is in hand, and the modem holds at most
one burst for that slot (from its space, R10.2), the host writes a silence burst for the next position. This goes on
until the call ends (terminator, R4.7 loss, R7.8 watchdog). *Basis: RadioFoundry (`bsOutbound.c`: a stream that underruns mid
superframe gets silence at the next position). Design: an idle burst in the middle of a superframe makes receivers
drop the call and re-enter late; silence keeps them in it.*

R6.4. **Late bursts.** After silence fill, a burst whose position was already filled is dropped, and the stream goes
on from the next position. A burst whose position comes after the filled ones is gap filled (R6.2). *Basis: Design:
the repeat delay stays fixed; jitter beyond the prebuffer is lost, as on any real-time link.* With the auto jitter
buffer (R7.4), a network call's underrun silence under the maximum instead moves the stream back a position, so the
late burst still goes out; a gap is filled only up to the target, and the stream gives the extra back later at silence
or, after 2 s ahead, one voice burst per 2 s. A moved burst keeps its AMBE and is re-framed for its new position
(sync, or EMB with the position's LCSS and the call's LC fragment, R7.3).

R6.5. After headers, the first voice burst sent is an A. *Basis: ETSI -1 5.1.2.2.*

## 7. The network: up and down (duplex)

### Up

R7.0. Everything the modem hears with the transmitter keyed (not the bursts it flags as heard while the transmitter
was off, and not wakeups) goes to the receiver path: the call tracker, HBlink4 as this peer on the slot it was heard
on, and MQTT, with RSSI. This includes slots not enabled for repeat. The BS never sends up what it transmits.
*Basis: Design: the repeater is also a receiver for the gateway's subscribers; it can't hear its own output on a
duplex pair, and network calls it sends are reported as TX events, not as heard calls.*

### Down

R7.1. **Admission.** A DMRD stream for slot N goes out on slot N if: N is enabled; the stream id isn't in the slot's
done list; no other network stream holds the slot (R8.5); no RF call holds it (R8.2); and it isn't our own repeat
coming back (R8.4). A refused stream goes in the done list, and none of its later bursts are used, even once the slot
frees. The done list keeps at least the last 64 stream ids per slot. *Basis: Design: a call goes out whole or not at
all; picking it up mid-way would put a fragment on air.*

R7.2. **Its LC.** A voice LC header whose LC decodes with a source id gives the call its LC. A stream whose header
doesn't decode, or whose voice comes without a header, gets an LC built from the DMRD addressing: FLCO group voice
(000000) or unit to unit (000011), FID 0, service options 0, destination, source. Later headers in the stream are
ignored. *Basis: ETSI -1 7.1.1 (a voice call needs an LC), -2 7.1.1 (Grp_V_Ch_Usr, UU_V_Ch_Usr). Design for the
fallback.*

R7.3. A network call's LC never changes mid-call: headers, the embedded LC fallback (R5.8) and the terminators all
use it. *Basis: ETSI -1 5.2.1.4; Design.*

R7.4. **Prebuffer.** The BS holds P bursts of a stream (fixed: default 6, 360 ms; auto, the box config's default:
sized per call from the measured network lateness, 180 to 480 ms, [fw/roamer.md](fw/roamer.md) "Jitter buffer") before any of it goes out; then all it holds
goes to the slot's queue and later bursts follow as they come. A stream that ends before P (terminator, last data
block, a lone CSBK) or is ended by the watchdog goes out at once. *Basis: Design: one superframe of cushion absorbs
the network jitter of HBlink4 over field uplinks (LTE, satellite); SDS 5.5 uses 180 ms over USB, and a WAN needs
more.*

R7.5. **Keying.** When hibernating, the BS keys at the stream's first admitted burst, while it prebuffers. The modem
sends idle meanwhile (R2.2), which gives the key-up lead of R3.3 with no added delay. *Basis: Design, from R3.3.*

R7.6. **On air.** A voice call goes out as: headers (H = 3, R4.2), the PI header if the stream has one, then voice
from the first burst A. Bursts of a stream that starts mid-superframe are dropped until its first A. *Basis: ETSI
-1 5.1.2.2.*

R7.7. Voice follows section 6 (positions, gap fill, underrun fill, late bursts). *Basis: as there.*

R7.8. **End.** The stream's terminator ends the call: the call's terminator and the call hang (R3.5). The **watchdog**
ends a stream with no burst for W = 1.5 s the same way. *Basis: ETSI -2 5.1.1.3 for the hang. W is Design: it bridges
network gaps of up to about four superframes (silence filled, R6.3, so receivers stay in the call); a longer gap is
more likely a lost stream than jitter, and the slot should be freed.*

R7.9. **Data and CSBK streams.** A data header opens a data call (BF blocks if its CRC is good, else none). Blocks pass
regenerated (R5.6). The data call ends after its last block, but the stream stays open (not in the done list) for
another packet until the watchdog. A CSBK the network marks as sent on its own (a radio check, a call alert) goes out
and ends its stream. Preamble CSBKs go out one for one. *Basis: Design: HBlink4 can carry several packets in one
stream; the network already sent the preambles the source wanted.*

R7.10. **Back to back.** A new network call on a slot whose previous call is still going out waits for that call's
own bursts and its first terminator; only filler of the previous call (call hang terminators) is dropped. *Basis:
MP 3.7 (call 1 ends with its terminator before call 2 keys up); Design: cutting the end off the previous call
loses the last words, which are often the answer.*

## 8. Contention, priority and echo

R8.1. **RF has the slot.** When an RF voice or data call starts on slot N while the network holds it (streaming,
prebuffering, or bursts still queued here or in the modem): the network stream goes in the done list; its bursts
are dropped here and in the modem (DMR_ABORT N); its TX event ends. *Basis: Design: radios in the repeater's
coverage are local users who can't hear the network caller; ETSI politeness and hang rules govern the inbound and
say nothing about a network path, so the gateway chooses local first.*

R8.2. While an RF call holds slot N (from its start until its terminator or loss), network streams for N are refused.
*Basis: Design, as R8.1.*

R8.3. An RF CSBK heard during a network voice call on that slot goes up but is not repeated: repeating it would cut
a data burst into the voice superframe. In call hang or channel hang it is repeated (R4.9). *Basis: ETSI -1 5.1.2.1
(superframes are continuous); -2 5.1.1.4.*

R8.4. **Echo.** A network stream for slot N with the same source and destination as the last RF transmission on N
(voice call, data call or CSBK), starting within E = 2 s of its end, is refused. *Basis: Design: the network reflects
our own uplink back, through another receiver that heard the repeat or a second copy of the call; within 2 s the same
radio can't be talking somewhere else, and 2 s covers HBlink4's routing delay.*

R8.5. **Network against network.** The first stream holds the slot until it ends; any other stream for that slot
meanwhile is refused. *Basis: Design: HBlink4 decides routing; the BS keeps the call whole.*

R8.6. The two slots are independent: a call on one never affects the other, except that both share the transmitter
(R3.4). *Basis: ETSI -1 5.2.1.5 (2:1 mode).*

R8.7. A network stream may start in a slot's call hang, whatever its talkgroup; it ends the hang (R3.7). *Basis:
Design: HBlink4 already arbitrated the network; -1 5.2.1.4 restricts which MSs may transmit in call hang, not what the
BS sends. The echo rule (R8.4) still applies.*

## 9. The CACH short LC (duplex)

R9.1. The host gives the modem the Activity Update short LC (Act_Updt, SLCO 0001): TS1 activity (4 bits), TS2 activity
(4 bits), the hashed destination of TS1, then of TS2 (8 bits each), in that order. The hash is the CRC-8 of the 24-bit
destination (-1 B.3.7). The 28 bits get their CRC-8 and Hamming(17,12,3) rows with column parity (-1 B.2.3, 7.1.4)
and go to the modem as DMR_SHORTLC (9 bytes, SDS 5.3). *Basis: ETSI -2 7.1.3.2 table 7.10.*

R9.2. **Activity codes** (-2 table 7.10): group voice 1000, individual voice 1001, emergency group voice 1100 and
emergency individual voice 1101 (the LC's service options have the emergency bit), group data 1011, individual data
1010. After a CSBK preamble the slot announces what follows: data activity when the preamble says data follows,
CSBK activity (0010 group, 0011 individual) when it says CSBK. No activity is 0000. *Basis: ETSI -2 7.1.3.2 and its
note 2.*

R9.3. A slot's activity lasts through its call hang and clears when channel hang starts. *Basis: ETSI -1 5.2.1.4 (the
BS keeps the channel busy for the call during call hang).*

R9.4. The host sends a new short LC whenever either slot's activity changes. When both become idle it sends the Null
short LC (SLCO 0000, -2 7.1.3.1) itself; it doesn't count on the modem to fall back to it. *Basis: ETSI -2 7.1.3.1;
SDS 5.2, 5.3 (a RadioFoundry TX half repeats the last short LC until replaced).*

R9.5. No short LC in simplex or SFR: a direct mode transmission has no CACH, and the SFR radio makes its own.
*Basis: MMDVM (MP 2.10, 6.2).*

R9.6. **Interop option** `short_lc_ts1_low` (default off): the Act_Updt fields the other way round, TS2's activity
in the high nibble and its hash first, TS1's in the low nibble and second. Off is R9.1, ETSI's order. *Basis: Design:
many MMDVMHost-based hotspots send TS1 in the low nibble (interop context only); if radios in the field turn out to
depend on that order, the option matches it without a code change.*

## 10. Modem flow control (all modes)

R10.1. **Status polling.** GET_STATUS every 60 ms (one burst time per slot) while anything is queued for the modem,
every 2 s otherwise. *Basis: Design: at one poll per burst time the host's copy of space follows the modem's drain
rate; 2 s when idle keeps a radio in hotspot mode (it leaves after 20 s without host traffic, MP 3.2) at little
cost.*

R10.2. **Space.** Per slot, the host writes a DMR_DATA frame only while its copy of that slot's space is above 1,
subtracts 1 per frame, and replaces its copy with each status reply. A frame the modem refuses as full (NAK 5) is
lost; it is not resent. *Basis: MMDVM (MP 2.4: the last free place is never used, and a modem reports space
assuming this).*

R10.3. **How much to queue.** Traffic goes to the modem as far as space allows. Filler (call hang terminators, headers
waiting for voice, silence fill) is written just in time: at most two bursts of filler in the modem per slot. *Basis:
Design: filler written ahead would hold up the next call, or need DMR_ABORT, which drops traffic too; two bursts
cover one 60 ms status interval plus the host loop's latency (about 20 ms), with one to spare.*

R10.4. DMR_ABORT is used only for RF priority (R8.1). *Basis: Design, from R10.3.*

R10.5. **A dead modem.** A missed status reply is retried at the next poll. Two seconds without any reply while
traffic is queued is a modem fault: everything queued is dropped, network calls going out end, the slots return to
idle, and the modem is reopened (backoff from 1 s, doubling, up to 30 s). *Basis: Design: a modem answers within tens
of milliseconds (MP 2.4), so 2 s is some 30 missed polls; the modem may miss one or two while it keys up.*

## 11. Simplex hotspot

What changes from the duplex rules when the modem is simplex (one frequency, direct mode). Tuning, DMRT / DMRK, the
answer window and the idle channel are the roamer's and are in [fw/roamer.md](fw/roamer.md).

R11.1. One slot on air: the host writes DMR_DATA2 only, and reads slot 2's space (slot 1's is a constant).
*Basis: MMDVM (MP 2.4, 3.6).*

R11.2. **Framing.** MS-sourced sync (voice 7F7D5DD57DFD, data D5D7F77FD757), slot type and EMB with the channel's
color code. LC headers, terminators and the embedded field are the network stream's own; nothing is rebuilt from an
LC. *Basis: ETSI -1 9.1.1 table 9.2, 4.4.3 (direct mode). Design: one stream at a time is relayed, so there's no
second LC to keep consistent.*

R11.3. No CACH, no short LC, no DMR_START, no DMR_ABORT. The modem keys for the traffic it is given. *Basis: MMDVM
(MP 2.10, 3.6).*

R11.4. **Headers and terminators per call**: Hs = 8 and Ts = 3 for MMDVM and MMDVM_HS boards, 1 and 1 for a
radio_hotspot (the radio builds its own lead and terminator, MP 2.8). The counts are per call: repeated headers or
terminators in the stream don't multiply them. *Basis: Design from ETSI: in simplex the carrier starts with the
call, so the headers are also the sync lead; 8 × 60 ms = 480 ms covers a receiver's sync attempt (-1 F.1
T_ChSyncTo ≥ 390 ms) with a header to spare. -1 5.1.2.3 needs one terminator; three (180 ms) let a receiver that
loses one end the call at once instead of on its own fade timer.*

R11.5. Prebuffer P (R7.4; the repeated headers not counted), the stream timeout W = 1.5 s, the voice stream rules of
section 6, and the space rule R10.2 (write while above 1) are as for duplex. *Basis: as there.* A voice stream W ends
gets its Ts terminators (R7.8), with the LC of its header, or one built from the stream's source and destination when
no header decoded.

R11.6. **A CSBK on its own** goes out after two preamble CSBKs (Pre_CSBK, CSBKO 111101, "CSBK content follows", the
blocks to follow counting down to the real one), and the transmitter drops straight after it. *Basis: ETSI -2
7.1.2.5 (preambles help scanning and battery-saving radios). Two (120 ms) is Design: enough for a synced target to
wake, short enough not to hold the channel.*

R11.7. **Listen before talk.** Before keying on a channel the hotspot listens for L = 0.3 s; anything heard there
means busy, and the call is refused. *Basis: ETSI -1 5.2.1.6 (polite channel access). L is Design: five frame times;
any DMR transmission on the channel shows several bursts in that time.*

R11.8. No call hang and no channel hang: there is no BS. *Basis: ETSI -1 4.4.3 (direct mode).*

## 12. Single frequency repeater (a RadioFoundry radio in SFR mode)

The radio repeats its inbound slot on its outbound slot by itself, frames the outbound, keys and runs its own hang
(RadioFoundry `sfr.md`; ETSI -1 5.1.4.2). The host is a receiver for the inbound slot and a transmitter for the network on the
outbound slot.

R12.1. **Setup.** SET_FREQ with RX = TX, SET_CONFIG with the simplex flag clear. The modem's description must announce
SFR with our inbound slot (` SFR TS<in>>TS<out> <framing>`); the host refuses to start otherwise, and warns unless the
framing is `TS` (the only one that marks the slot on air). *Basis: MMDVM (MP 6.1, 6.2).*

R12.2. **Up.** What the radio sends up for the inbound slot goes to the receiver path (R7.0). The host never repeats
it: the radio does. *Basis: MP 6.2.*

R12.3. **Down.** Every admitted network stream goes out on the outbound slot, whatever slot HBlink4 sent it on, as that
slot's DMR_DATA. No DMR_START, no DMR_SHORTLC. The radio frames it (R11.2 does not apply; the radio rebuilds). The
call hang terminators of R3.5 are sent for what the network sends. *Basis: MP 6.2; Design for the hang (the
outbound looks the same to radios as a duplex repeater's).*

R12.4. **Inbound busy.** The inbound is busy when a burst was heard on it within Q = 0.3 s, when the status has flag
0x80, or after the radio NAKs an outbound frame with 6 (RF busy). It is free after the inbound call's terminator or
DMR_LOST, or Q after the last inbound burst with the flag clear. When it is busy only by the flag and Q has passed,
the host reads the status again before refusing a stream. *Basis: MMDVM (MP 6.2: busy until the inbound
terminator has been repeated). Q is Design: five frame times with nothing on the inbound means the call is gone even
if its terminator was missed; the flag and the NAK cover the rest.*

R12.5. **RF has the slot.** While the inbound is busy a new stream is refused; a stream going out when it becomes busy
is dropped (done list, DMR_ABORT). A stream that comes during the radio's own hang goes straight out. *Basis: MP
6.2; Design as R8.1.*

## 13. Dual capacity direct mode (a gateway, mode `dcdm`)

Two simplex calls at once on one 12.5 kHz frequency, one in each TDMA slot (ETSI -1 5.1.4, "TDMA direct mode"): a
radio sends a burst every 60 ms, TS2 30 ms after TS1. The gateway passes both slots between RF and the network. It is
not a repeater. Radios hear each other directly, and the gateway only carries calls to and from HBlink4. The modem is
a DCDM modem (an SDR in DCDM, or a radio with the DCDM contract in [fw/radio.md](fw/radio.md)).

R13.1. **Setup.** SET_FREQ with RX = TX, SET_CONFIG with the simplex flag (0x80) and the DCDM flag (0x40). The
modem's description must contain the word `DCDM`; the host refuses to start otherwise. A split pair and UTC slot
timing are refused: the modem times its slots from the channel. *Basis: MMDVM (MP 2.4, 3.6) plus the DCDM extension
([fw/radio.md](fw/radio.md)).*

R13.2. **Both slots.** The host writes DMR_DATA1 for TS1 and DMR_DATA2 for TS2, reads both slots' space, and aborts
one slot at a time (DMR_ABORT N). The modem keys burst by burst in each slot's window and sends TS2 30 ms after TS1.
It takes the slot timing from TS1 / TS2 bursts it hears, so its bursts fall in the slots the radios use. With
nothing heard it keeps a timing of its own. *Basis: ETSI -1 5.1.4, 9.1.1.*

R13.3. **Nothing repeated.** What is heard on a slot goes up to the network on that slot (R7.0) and is never sent back
on air: the radios hear each other. RF calls are followed only so the host knows who has the slot (R8.1, R8.2). Heard
TS1 bursts are slot 1; TS2 and MS-sourced bursts are slot 2. *Basis: ETSI -1 4.4.3 (direct mode has no BS); Design
for MS sync (a single-slot radio's call goes to slot 2, as on a simplex hotspot).*

R13.4. **Framing.** Each burst carries its slot's direct mode sync: TS1 voice 5D577F7757FF / data F7FDD5DDFD55, TS2
voice 7DFFD5F55D5F / data D7557F5FF7F5. The host writes the sync over whatever the stream had. Slot type and EMB use
the channel's color code. Everything else is as R11.2. *Basis: ETSI -1 9.1.1 table 9.2.*

R13.5. **Calls.** Headers and terminators per call as R11.4 (Hs and Ts from the radio's profile). No CACH, no short
LC, no DMR_START. No call hang and no channel hang: a network call ends with its Ts terminators. *Basis: ETSI -1
4.4.3, as R11.3 and R11.8.*

R13.6. **Per slot.** The network rules (prebuffer, watchdog, voice stream rules: R11.5) and contention (section 8:
RF has the slot, echo, network against network) apply to each slot by itself. The two slots are independent (R8.6).
A network call on TS1 can go out while a radio talks on TS2, and two network calls can go out at once. *Basis: ETSI
-1 5.1.4; Design as section 8.*

R13.7. **The radio's side.** Listen before talk (R11.7) does not apply: a slot heard busy is RF's (R8.2), and the
other slot stays free. Radios must be set for dual capacity direct mode, with each talkgroup on its slot, to
receive the slot they're on. *Basis: ETSI -1 5.2.1.6 (politeness per channel; a DCDM channel is one per slot).*

## 14. Values

| Value | Default | Config | Basis |
|---|---|---|---|
| T_CallHt, call hang | 3 s | `call_hang_s` | ETSI -1 F.1 default |
| T_MSInactiv, BS inactivity | 5 s | `hang_s` | ETSI -1 F.1 default |
| T_ChHt, channel hang | T_MSInactiv − T_CallHt = 2 s | derived | ETSI -1 F.1 |
| H, duplex headers | 3, more while voice hasn't come | | Design (R4.2), ETSI -2 5.1.1.6.1.1 |
| Key-up lead | ≥ 360 ms | | Design from ETSI -1 F.1 (R3.3) |
| P, network prebuffer | auto 180–480 ms (6 bursts until the link is known); fixed 6 bursts (360 ms) | `jitter_buffer`, `jitter_min_ms`, `jitter_max_ms`, `prebuffer` | Design (R7.4) |
| W, network watchdog | 1.5 s | | Design (R7.8) |
| RF loss backstop | 1.0 s | | Design (R4.7) |
| E, echo window | 2 s | | Design (R8.4) |
| Q, SFR inbound quiet | 0.3 s | | Design (R12.4) |
| Status poll | 60 ms busy, 2 s idle | | Design (R10.1) |
| Dead modem | 2 s without a status reply | | Design (R10.5) |
| Space threshold | write while above 1 | | MMDVM (R10.2) |
| Filler in the modem | ≤ 2 bursts per slot | | Design (R10.3) |
| Hs / Ts, simplex | 8 / 3 (MMDVM boards), 1 / 1 (radio_hotspot) | `headers`, `terminators` | Design from ETSI (R11.4) |
| Hs / Ts, DCDM | as simplex, per the radio's profile | `headers`, `terminators` | Design (R13.5) |
| DCDM slot spacing | TS2 30 ms after TS1, each slot every 60 ms | | ETSI (R13.2) |
| CSBK preambles, simplex | 2 | `csbk_preambles` | Design (R11.6) |
| L, listen before talk | 0.3 s | `listen_s` | Design (R11.7) |
| CW ID interval | 900 s | `cwid_every_s` | Design (R3.9) |
| Short LC slot order | ETSI (TS1 high) | `short_lc_ts1_low` | ETSI (R9.1); Design for the option (R9.6) |

## 15. Open questions

1. **AT during call hang on MMDVM boards.** ETSI wants AT busy during call hang (R2.3); the host can't set it. Check
   on air with an SDR (CACH decode) whether MMDVM_HS and MMDVM boards mark the inbound busy while they send hang
   terminators. If not, polite radios may start other calls in our call hang.
2. **Idle fill when keyed.** R2.2 assumes every duplex modem fills empty slots with idle while keyed. Confirm on each
   board type with an SDR, keyed by a wakeup with nothing queued.
3. **Values to settle in the field**: P (6) and W (1.5 s) on real field uplinks; whether T_CallHt 3 s and
   T_MSInactiv 5 s suit the users' radios (a longer T_MSInactiv saves wakeups between exchanges).
4. **MBC and USBD.** Does any radio in use send them? If none does, R4.10 can stay unimplemented.
5. **Wake on voice.** ETSI allows "MS established timing" (-1 4.4.2, -2 5.1.1.1.0): a BS that keys on any MS-sourced
   burst with our color code. The gateway wakes only on BS_Dwn_Act. Worth it for radios that don't send the wakeup?
6. **Delay growth.** With R6.4 the repeat delay is fixed per call. Is a cap needed for very long calls on a drifting
   network clock, or does the end of each call reset it often enough?
