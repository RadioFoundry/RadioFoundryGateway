// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The trunked site controller (DMR Tier III, ETSI TS 102 361-4) and its MS simulator, in Rust (rust/src/t3site.rs,
 * t3ms.rs; docs/spec/fw/tier3.md sections 3 and 8).
 *
 * The controller makes every Tier III decision and does no I/O: the caller feeds it the inbound PDUs, payload voice
 * activity, network events and the time, and per slot it returns what goes on air; events say what else to do
 * (open or close a payload channel, publish a call setup, write a registration record). It allocates nothing: the
 * caller gives it rdof_t3_site_size() bytes, aligned to 8.
 *
 * PDUs are the 10 information bytes of a CSBK, MBC header or MBC continuation block, before the CRC; short LCs the
 * 28 bits before the CRC-8. The radio layer adds the CRCs, BPTC, slot type, sync and CACH.
 *
 * Channels: index = carrier * 2 + LCN (LCN 0 = TDMA channel 1). The TSCC is LCN 0 of the carrier with tscc set.
 * Slots: k counts outbound slots on every carrier (aligned timing): LCN 0 when k is even, LCN 1 when odd. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* capacities (compile time) */
#define RDOF_T3_CARRIERS 4
#define RDOF_T3_CHANNELS (RDOF_T3_CARRIERS * 2)
#define RDOF_T3_REGS 1024        /* registration records */
#define RDOF_T3_REG_TGS 8        /* talkgroups per record */
#define RDOF_T3_PENDING 32       /* call requests in progress or queued */
#define RDOF_T3_STREAMS 16       /* network group streams waiting for or holding a channel */
#define RDOF_T3_NEIGHBORS 32
#define RDOF_T3_MS_POLICIES 256  /* per-MS provisioning entries */
#define RDOF_T3_TG_POLICIES 64   /* per-talkgroup provisioning entries */
#define RDOF_T3_IDSET_RANGES 16

/* data types (361-1 slot type) */
#define RDOF_T3_DT_NONE 0
#define RDOF_T3_DT_CSBK 3
#define RDOF_T3_DT_MBC_HDR 4
#define RDOF_T3_DT_MBC_CONT 5

typedef struct {
    uint32_t tx_hz, rx_hz; /* the site's outbound (MS receives) and inbound (MS transmits) frequencies */
    uint16_t chan;         /* logical physical channel number in grants, 1..4094 */
    uint8_t tscc;          /* 1: LCN 0 is the TSCC (exactly one carrier) */
    uint8_t enabled;       /* 0: out of service (its payload channels aren't granted) */
} rdof_t3_carrier_t;

typedef struct {
    /* C_SYScode */
    uint8_t model, par, area_bits, version; /* model 0 tiny .. 3 huge; par 1 A, 2 B, 3 both; version 4 = 1.11.1 */
    uint16_t net, site;
    uint8_t ncarriers, cc;
    uint8_t site_sync;    /* 1: every carrier slot-synchronized (C_ALOHA); else payload warm-up */
    uint8_t reg_required; /* Reg = 1: service only for registered MSs */
    rdof_t3_carrier_t carriers[RDOF_T3_CARRIERS];
    uint8_t foacsu;       /* individual calls by full off-air call set-up */
    uint8_t nrand_wait, backoff_min, backoff_max;
    uint8_t clear_repeats, warmup_cach, ahoy_tries, queue_max;
    uint8_t clock_ok;     /* 1: now_ms is good wall-clock time (Local_Time broadcasts) */
    uint8_t pad0;
    int16_t utc_offset_min; /* local minus UTC */
    /* timers, ms */
    uint32_t tp_ms, tnp_ms, hang_ms, call_ms, emergency_call_ms, ahoy_every_ms, grant_every_ms, queue_ms;
    uint32_t net_queue_ms, setup_ms, start_grace_ms;
    uint32_t reg_hold_s;
    uint32_t adjacent_ms, chan_freq_ms, local_time_ms, gen_site_ms; /* 0: not broadcast */
    uint32_t stream_seed; /* first stream ID for setups the site starts */
    uint8_t preempt;      /* 1: an emergency with no channel free clears a non-emergency call (361-4 6.4.14.3.1) */
    uint8_t power_save;     /* the highest PowerSave_RQ granted (1-7: 1:2 to 1:128); 0: not offered (6.4.7) */
    uint8_t mass_reg_start; /* Reg_Window (table 6.12) of a mass re-registration at start; 0: none (6.4.5) */
    uint8_t duplex; /* full-duplex MS to MS calls (tier3.md 3.13): 0 refused */
    uint32_t call_timers_ms, vote_now_ms, ann_tscc_ms; /* CallTimer_Parms, Vote_Now, Ann_WD_TSCC; 0: not broadcast */
    uint32_t awake_ms; /* how long an MS in power save stays awake after a PDU (the site's T_Awake) */
    uint32_t hibernate_ms; /* T_BS_Inactive: the TSCC hibernates after this long with no inbound; 0 never */
    /* short data (tier3.md 3.15): downloads to an MS (each waits ahoy_every_ms for C_ACKU), sends to a talkgroup,
     * and how long a message waits for a busy MS; dgna_ms: MSs may assign talkgroups to others (DGNA, 6.6.8);
     * als: MSs may ask for the Ambient Listening Service (6.6.2.6; tier3.md 3.18) */
    uint8_t sd_tries, sd_repeats, dgna_ms, als;
    uint32_t sd_store_ms;
    /* packet data (tier3.md 3.16): how long a data channel waits for more after a message (confirmed or
     * multi-item), and the site's own data ID (0: TSI only) */
    uint32_t data_hang_ms;
    uint32_t data_id;
    uint8_t divert; /* MSs may divert their calls (6.6.7; tier3.md 3.18): 0 refused */
    uint8_t auth; /* RDOF_T3_AUTH_*: MS authentication at registration (361-4 6.4.8; tier3.md 3.14) */
    uint8_t composite; /* 1: the TSCC's slot carries a call when no payload channel is free (361-4 5.3.2; tier3.md
                          3.19) */
    uint8_t vote_blk;  /* VOTE_BLK: TDMA frames of blocked random access after a Vote Now Advice (361-4 A.2: 2-10) */
    uint8_t priority;  /* the site's priority radiated with its neighbors (361-4 table 6.81: 1 highest .. 7 lowest,
                          0 non-preferred) */
    uint8_t intr;      /* 1: transmit interrupt, the payload interrupt and ALS termination de-key and the call timer
                          de-key (361-4 6.4.14.3.2-6.4.14.3.4, 6.4.15; tier3.md 3.20) */
    uint8_t ip_upload; /* 1: packet data to IPI asks the caller for the IP address first (361-4 6.6.3.2.4 b 2) */
    uint8_t composite_le; /* 1: Late_Entry grants on a lent composite TSCC slot in hang time (tier3.md 3.19) */
    /* a group call to a talkgroup with members in power save waits up to this long for a frame they're all awake
     * in (361-4 6.4.7.1); 0: never waits */
    uint32_t ps_hold_ms;
    uint8_t payload_le; /* 1: Late_Entry grants on a payload slot itself in its call's gaps (tier3.md 3.21) */
    uint8_t pad1;
    uint16_t clear_chan; /* P_CLEAR's channel; 0: none named (tier3.md 3.21) */
    uint32_t park_ms;    /* P_PROTECT(ILLEGALLY_PARKED) this long while a new call waits for voice; 0 off (3.21) */
} rdof_t3_cfg_t;

#define RDOF_T3_AUTH_OFF 0          /* none */
#define RDOF_T3_AUTH_REGISTRATION 1 /* MSs with a key are challenged at registration; the others register as before */
#define RDOF_T3_AUTH_REQUIRED 2     /* every MS is challenged; one without a key is refused (Reg_Denied) */

typedef struct {
    uint32_t lo, hi;
} rdof_t3_range_t;

/* an ID set: all, or up to RDOF_T3_IDSET_RANGES ranges (n = 0 and all = 0: empty) */
typedef struct {
    uint8_t all, n, pad[2];
    rdof_t3_range_t r[RDOF_T3_IDSET_RANGES];
} rdof_t3_idset_t;

typedef struct {
    uint8_t open;            /* 1: any ID in ms may register; 0: listed (the same set, but the default is empty) */
    uint8_t refuse_unlisted; /* 0: Reg_Denied, 1: Reg_Refused */
    uint8_t pad[2];
    rdof_t3_idset_t ms, private_ms, tg, wide_tg, static_tg;
} rdof_t3_policy_t;

/* a talkgroup's provisioning (cfg/tg): its hang time (0: the site's hang_ms) and call priority (0-3) */
typedef struct {
    uint32_t tg, hang_ms;
    uint8_t priority, pad[3];
} rdof_t3_tg_policy_t;

typedef struct {
    uint8_t enabled, private_ok, wide_private, emergency_ok;
    uint8_t priority, ntg, pad[2];
    uint32_t tg[RDOF_T3_REG_TGS]; /* static talkgroups for this MS */
    /* authentication (361-4 6.4.8.1): the MS's key K and its PSN (24 bits); secret: never logged or published */
    uint8_t has_key, has_psn, pad2[2];
    uint32_t psn;
    uint8_t key[16];
} rdof_t3_ms_policy_t;

#define RDOF_T3_HOW_REGISTER 1
#define RDOF_T3_HOW_CALL 2
#define RDOF_T3_HOW_LOADED 3

typedef struct {
    uint32_t id, sub_tg; /* sub_tg 0: none */
    uint8_t how, ntg, area, pad;
    uint32_t tg[RDOF_T3_REG_TGS];
    uint8_t ps_rq, ps_off, pad2[2]; /* power save granted: PowerSave_RQ and PowerSave_Offset (6.4.7); 0: none */
    int64_t at_ms, exp_ms;
} rdof_t3_record_t;

/* setup ops and reasons (mqtt-net's setup item, tier3.md 5.4) */
#define RDOF_T3_SETUP_REQUEST 1
#define RDOF_T3_SETUP_ACCEPT 2
#define RDOF_T3_SETUP_REJECT 3
#define RDOF_T3_SETUP_ALERTING 4
#define RDOF_T3_SETUP_CANCEL 5
#define RDOF_T3_SETUP_ANSWERED 6
#define RDOF_T3_WHY_OK 0
#define RDOF_T3_WHY_NOT_FOUND 1
#define RDOF_T3_WHY_BUSY 2
#define RDOF_T3_WHY_NO_ANSWER 3
#define RDOF_T3_WHY_REFUSED 4
#define RDOF_T3_WHY_NOT_SUPPORTED 5
#define RDOF_T3_WHY_CONGESTION 6
#define RDOF_T3_WHY_TIMEOUT 7
#define RDOF_T3_SETUP_F_FOACSU 0x01
#define RDOF_T3_SETUP_F_UNCONFIRMED 0x02
#define RDOF_T3_SETUP_F_DATA 0x04 /* a packet data call (tier3.md 5.10) */
#define RDOF_T3_SETUP_F_ALS 0x08  /* an Ambient Listening Service call (361-4 6.6.2.6) */

typedef struct {
    uint8_t op, reason, flags, svc; /* svc: C_RAND's service options */
    uint32_t src, dst;
    uint32_t stream; /* this message's stream ID */
    uint32_t ref;    /* the request's stream ID this answers (0 in a request) */
} rdof_t3_setup_t;

/* an adjacent site (tier3.md 3.11): its C_SYScode and TSCC channel, Active_Connection, its priority (361-4 table
 * 6.81; the confirmed priority radiated with it is cfg.priority), and its TSCC's color code and frequencies as its
 * carriers give them (tx_hz outbound, rx_hz inbound; 0: not known). With the frequencies, Chan_Freq announces the
 * channel, unless one of the site's carriers (or an earlier neighbor) has the number at other frequencies: then only
 * Vote_Now names it, by its frequencies. */
typedef struct {
    uint16_t syscode, chan;
    uint8_t active_known, active, prio_adjacent, cc;
    uint32_t tx_hz, rx_hz;
} rdof_t3_neighbor_t;

/* events */
#define RDOF_T3_EV_CH_OPEN 1     /* a payload channel's call starts: ch, src, dst, flags, stream (network), chan, lcn,
                                    svc (the Service Options of an LC the site makes: 80h emergency, 08h broadcast) */
#define RDOF_T3_EV_CH_CLOSE 2    /* ch, why (RDOF_T3_CLOSE_*) */
#define RDOF_T3_EV_CH_STREAM 3   /* a network stream now feeds open channel ch: stream, src */
#define RDOF_T3_EV_NET_SETUP 4   /* publish a setup message: op, reason, flags (setup flags), svc, src, dst, stream, ref */
#define RDOF_T3_EV_NET_SUBS 5    /* the talkgroups to subscribe to changed: rdof_t3_subs() */
#define RDOF_T3_EV_RECORD 6      /* write (rdof_t3_record()) or, with why 1, delete the record of src */
#define RDOF_T3_EV_ACTIVITY 7    /* kind (RDOF_T3_ACT_*), op (action), src, dst, ch, chan, lcn, flags, reason */
#define RDOF_T3_EV_NET_DROP 8    /* the network stream won't be carried: stream, why */
#define RDOF_T3_EV_LOG 9         /* text */
#define RDOF_T3_EV_SD 20         /* short data for the site, or the result of one it sent (rdof_t3_sd_of_event) */
#define RDOF_T3_EV_NET_SD 21     /* short data or a result to publish (mqtt-net sdata, tier3.md 5.9) */
#define RDOF_T3_EV_PD 22         /* packet data: op RDOF_T3_PD_OPEN (ch, src, dst; kind RDOF_T3_PARTY_*) or _FAILED */

#define RDOF_T3_CLOSE_HANG 1     /* hang time expired */
#define RDOF_T3_CLOSE_TIMER 2    /* call timer */
#define RDOF_T3_CLOSE_DISCON 3   /* P_MAINT DISCON */
#define RDOF_T3_CLOSE_CMD 4      /* a command or a cancel */
#define RDOF_T3_CLOSE_WITHDRAW 5 /* the site went off the air */
#define RDOF_T3_CLOSE_PREEMPT 6  /* cleared for an emergency call (361-4 6.4.14.3.1) */
#define RDOF_T3_CLOSE_INTR 7     /* an ALS call ended at its caller's request (361-4 6.4.14.3.4) */

#define RDOF_T3_ACT_REG 1   /* op: 1 register, 2 deregister, 3 expire, 4 deny, 5 refuse, 6 auth_ok, 7 auth_fail,
                               8 auth_none (no answer to a challenge, or no support where it isn't required) */
#define RDOF_T3_ACT_GRANT 2 /* flags: group, late, net */
#define RDOF_T3_ACT_QUEUE 3 /* op: 1 in, 2 out, 3 drop; reason: the C_NACKD reason when dropped */
#define RDOF_T3_ACT_SETUP 4 /* op, reason: as the setup message */
#define RDOF_T3_ACT_REFUSE 5 /* reason: the C_NACKD reason */
#define RDOF_T3_ACT_STATUS 6 /* a status delivered or not (361-4 6.6.6): op the status, flags group, reason the caller's ack */
/* a supplementary service's result (tier3.md 3.18): op RDOF_T3_SUPP_*, src the MS, dst STUNI, KILLI or the diverted
 * address (TSI or DIVERTI when cleared), reason the air reason (an ACK reason, 0x60 or 0x44, done; 0x14 the MS
 * refused the site's authentication response; 0x20 no key to answer the MS's challenge; 0x25 no answer) */
#define RDOF_T3_ACT_SUPP 7
#define RDOF_T3_SUPP_STUN 1
#define RDOF_T3_SUPP_REVIVE 2
#define RDOF_T3_SUPP_DIVERT 3
#define RDOF_T3_SUPP_DIVERT_CLEAR 4
#define RDOF_T3_SUPP_KILL 5
/* a transmit interrupt's progress (tier3.md 3.20): op RDOF_T3_INTR_*, src the interrupting MS, dst the call's
 * destination, ch its channel */
#define RDOF_T3_ACT_INTR 8
#define RDOF_T3_INTR_ACCEPTED 1 /* C_ACKD(Message_Accepted) to the interrupting MS */
#define RDOF_T3_INTR_CEASED 2   /* the talker stopped */
#define RDOF_T3_INTR_GAVE_UP 3  /* the talker didn't stop: the call goes on (an ALS call is cleared) */
#define RDOF_T3_INTR_TALKS 4    /* the interrupting MS talks on the channel */

#define RDOF_T3_F_GROUP 0x01
#define RDOF_T3_F_EMERGENCY 0x02
#define RDOF_T3_F_NET 0x04    /* started from the network */
#define RDOF_T3_F_WIDE 0x08   /* carried on the network (publish the channel's RF voice) */
#define RDOF_T3_F_ALLCALL 0x10
#define RDOF_T3_F_LATE 0x20
#define RDOF_T3_F_PRIVATE_NET 0x40 /* an individual call whose other party is on the network */
#define RDOF_T3_F_DATA 0x80        /* a packet data call (tier3.md 3.16) */
/* EV_CH_OPEN why: RDOF_T3_OPEN_DUPLEX, a full-duplex call's channel (offset timing, the inbound repeated on the other slot) */
#define RDOF_T3_OPEN_DUPLEX 1

typedef struct {
    uint8_t type, ch, why, flags;
    uint8_t op, reason, svc, kind;
    uint16_t chan;
    uint8_t lcn, pad;
    uint32_t src, dst, stream, ref;
    char text[128];
} rdof_t3_event_t;

/* channel use */
#define RDOF_T3_USE_OFF 0
#define RDOF_T3_USE_IDLE 1
#define RDOF_T3_USE_TSCC 2
#define RDOF_T3_USE_CALL 3  /* someone talking */
#define RDOF_T3_USE_HANG 4  /* granted, no one talking */
#define RDOF_T3_USE_CLEAR 5 /* sending P_CLEAR */

/* rdof_t3_ch_out_t.rc: RC info (361-4 table 6.32) with this flag */
#define RDOF_T3_RC_SEND 0x10
#define RDOF_T3_RC_CEASE 0x4 /* cease transmission command */

typedef struct {
    uint8_t dt;      /* RDOF_T3_DT_*, 0: nothing (send voice, idle, or nothing as the channel's use says) */
    uint8_t pdu[10];
    uint8_t use;     /* RDOF_T3_USE_* now */
    uint8_t rc;      /* RDOF_T3_RC_SEND | RC info: a Reverse Channel PDU in this outbound burst, for the other slot's
                        talker (361-1 5.1.5); 0 none */
} rdof_t3_ch_out_t;

typedef struct {
    uint8_t tscc_dt; /* the TSCC block for slot k (k even); 0: nothing (k odd, or the site withdrawn) */
    uint8_t tscc_pdu[10];
    uint8_t at;      /* the AT bit for the CACH before slot k + 1 on the TSCC carrier */
    uint8_t has_slc; /* k % 4 == 0: a new short LC for the next 4 CACH */
    uint8_t tscc_ext[2]; /* tscc_dt RDOF_DT_RATE_12 (a UDT's appended block): its octets 10 and 11 */
    uint8_t rev;     /* a composite TSCC's slot in slot k: 1 carries a call (its payload path sends it), 2 still
                        sends the call's grant but hears the call */
    uint32_t slc_tscc;    /* C_SYS_Parms for the TSCC carrier (P_SYS_Parms, composite, while rev) */
    uint32_t slc_payload; /* P_SYS_Parms for the other carriers */
    rdof_t3_ch_out_t ch[RDOF_T3_CHANNELS]; /* only the channels with k's LCN */
} rdof_t3_slot_out_t;

typedef struct {
    uint8_t use, lcn, flags, pad;
    uint16_t chan, pad2;
    uint32_t src, dst;
    int64_t since_ms;
} rdof_t3_ch_status_t;

typedef struct {
    uint8_t connected, withdrawn, nch, pad;
    uint32_t regs, queue;
    uint32_t aloha, rand, collisions, grants, refused, queued;
    uint32_t lends, lent_ms, lent_max_ms; /* a composite TSCC's slot lent to calls: how often, in all and the
                                             longest, ms (the lend going on counted; tier3.md 3.19) */
    rdof_t3_ch_status_t ch[RDOF_T3_CHANNELS];
} rdof_t3_status_t;

/* payload voice activity (rdof_t3_rx_voice) */
#define RDOF_T3_VOICE_HEADER 1
#define RDOF_T3_VOICE_FRAME 2
#define RDOF_T3_VOICE_TERM 3
#define RDOF_T3_VOICE_LOST 4

/* packet data on a payload channel (rdof_t3_rx_data) */
#define RDOF_T3_DATA_HEADER 1
#define RDOF_T3_DATA_BLOCK 2
#define RDOF_T3_DATA_END 3           /* the last block of an unconfirmed message */
#define RDOF_T3_DATA_END_CONFIRMED 4 /* the last block of a confirmed message */
#define RDOF_T3_DATA_RESPONSE 5      /* a response header */
#define RDOF_T3_PD_OPEN 1
#define RDOF_T3_PD_FAILED 2 /* the site's own data call, or one from the network (kind PARTY_NET): reason, src, dst */
#define RDOF_T3_PARTY_SITE 1 /* PD_OPEN kind: the site is a party (its IP gateway) */
#define RDOF_T3_PARTY_NET 2  /* the far party is on the network: the unit relays (tier3.md 5.10) */

typedef struct rdof_t3_site rdof_t3_site_t;

/* the defaults of tier3.md section 3.8, one carrier */
void rdof_t3_cfg_default(rdof_t3_cfg_t *cfg);
void rdof_t3_policy_default(rdof_t3_policy_t *p);
size_t rdof_t3_site_size(void);
bool rdof_t3_init(rdof_t3_site_t *s, const rdof_t3_cfg_t *cfg, char *err, size_t cap);
/* new settings; calls in progress stay */
bool rdof_t3_reconfigure(rdof_t3_site_t *s, const rdof_t3_cfg_t *cfg, char *err, size_t cap);

/* slot k (see above), with the wall clock in ms; called once per k in order, about a slot before air */
void rdof_t3_slot(rdof_t3_site_t *s, int64_t k, int64_t now_ms, rdof_t3_slot_out_t *out);

/* inbound: a block with a good CRC, or (TSCC only) a burst with a good sync and a bad CRC: a collision. pdu is 10
 * octets, 12 when dt is RDOF_DT_RATE_12 (a UDT's appended block, BPTC decoded: its CRC is the whole message's) */
void rdof_t3_rx_pdu(rdof_t3_site_t *s, int ch, int64_t k, uint8_t dt, const uint8_t *pdu, int rssi);
void rdof_t3_rx_bad(rdof_t3_site_t *s, int ch, int64_t k);
/* payload voice: what RDOF_T3_VOICE_*, with the LC's addresses */
void rdof_t3_rx_voice(rdof_t3_site_t *s, int ch, int64_t k, uint8_t what, uint32_t src, uint32_t dst, bool group);
/* payload packet data: what RDOF_T3_DATA_*, src the message's (0: unknown) */
void rdof_t3_rx_data(rdof_t3_site_t *s, int ch, int64_t k, uint8_t what, uint32_t src);
/* The site's own packet data from src (its data ID or TSI) to an MS or a talkgroup: a channel is asked for (an MS's
 * availability check first); RDOF_T3_EV_PD PD_OPEN names it, or PD_FAILED. A response (an ACK) goes only on the data
 * channel it answers. False: not registered here, busy, no room, or a response with no channel. */
bool rdof_t3_pd_send(rdof_t3_site_t *s, uint32_t src, uint32_t dst, bool group, bool response, int64_t now_ms);
/* Packet data from the network (tier3.md 5.10) from src to an MS or a talkgroup here: a channel is asked for (after a
 * setup the far site answered, at once; else the MS's availability check first); PD_OPEN (kind PARTY_NET) names it,
 * or PD_FAILED. False: not here, busy, no room. */
bool rdof_t3_pd_net_in(rdof_t3_site_t *s, uint32_t src, uint32_t dst, bool group, int64_t now_ms);

/* the network */
void rdof_t3_net_group(rdof_t3_site_t *s, uint32_t stream, uint32_t src, uint32_t tg, uint8_t flags, int64_t now_ms);
void rdof_t3_net_group_end(rdof_t3_site_t *s, uint32_t stream);
void rdof_t3_net_setup(rdof_t3_site_t *s, const rdof_t3_setup_t *m, int64_t now_ms);
void rdof_t3_net_private(rdof_t3_site_t *s, uint32_t stream, uint32_t src, uint32_t dst, int64_t now_ms);
void rdof_t3_net_private_end(rdof_t3_site_t *s, uint32_t stream);
void rdof_t3_net_link(rdof_t3_site_t *s, bool connected);
void rdof_t3_neighbors(rdof_t3_site_t *s, const rdof_t3_neighbor_t *n, int count);
/* C_SYScode from its parts (model 0-3, par 1-3); 0 when they don't fit the model */
uint16_t rdof_t3_syscode(uint8_t model, uint16_t net, uint16_t site, uint8_t par);
/* where/{ms}: here false and at_ms newer than this site's record removes it */
void rdof_t3_where(rdof_t3_site_t *s, uint32_t ms, bool here, int64_t at_ms);

/* provisioning and state */
void rdof_t3_policy(rdof_t3_site_t *s, const rdof_t3_policy_t *p);
/* p NULL removes the talkgroup's entry; false: the table is full (tg 0 isn't one) */
bool rdof_t3_tg_policy(rdof_t3_site_t *s, uint32_t tg, const rdof_t3_tg_policy_t *p);
/* p NULL removes the MS's entry */
bool rdof_t3_ms_policy(rdof_t3_site_t *s, uint32_t ms, const rdof_t3_ms_policy_t *p);
bool rdof_t3_load_record(rdof_t3_site_t *s, const rdof_t3_record_t *r);
bool rdof_t3_record(const rdof_t3_site_t *s, uint32_t ms, rdof_t3_record_t *r);
/* the talkgroups the registered MSs need (subscriptions and lists, DGNA, recent calls, provisioning); static_tg
 * isn't included */
size_t rdof_t3_subs(const rdof_t3_site_t *s, uint32_t *tg, size_t cap);
void rdof_t3_withdraw(rdof_t3_site_t *s, bool off);
/* "deregister" (arg1 MS), "clear" (arg1 chan, arg2 LCN), "withdraw", "restore", "resend_records", "status",
   "mass_reg" (arg1 the window in s, 0 cancels; arg2 one MS, 0 all), "move" (C_MOVE: arg1 the TSCC's chan, arg2 one MS,
   0 all), "move_site" (C_MOVE to an adjacent site's TSCC: arg1 its C_SYScode, arg2 one MS, 0 all; by its frequencies,
   the MBC form, when its channel number names other frequencies here), "auth" (arg1 MS: an authentication poll; false when the MS isn't registered here or has no key),
   "stun" / "revive" (arg1 MS; 361-4 6.4.9, with authentication when the MS asks for it), "kill" (arg1 MS; 6.4.10,
   always authenticated: false when the site has no key for the MS); the result an RDOF_T3_ACT_SUPP activity;
   "radio_check" (arg1 an MS, or with arg2 1 a talkgroup; on its voice payload channel when it's on one, 6.6.2.3.1.1,
   else the TSCC, 6.4.12; false when no MS here has it; the outcome logged), "clear_ms" (arg1 an MS, or with arg2 1 a
   talkgroup: a selective clear from its voice payload channel, 6.6.2.3.1.8; false when it's on none) */
bool rdof_t3_command(rdof_t3_site_t *s, const char *cmd, uint32_t arg1, uint32_t arg2);
/* Stirs v into the source of authentication challenges: something unpredictable (the platform's random source),
   at start and now and then. */
void rdof_t3_entropy(rdof_t3_site_t *s, uint64_t v);

/* short data (tier3.md 3.15): UDT short data, a status, or a status poll */
#define RDOF_T3_SD_UDT 1
#define RDOF_T3_SD_STATUS 2
#define RDOF_T3_SD_POLL 3
#define RDOF_T3_SD_DGNA 4 /* DGNA to an MS (tier3.md 3.17): value the UDT_Format (1 address, 10 mixed), data the
                             * appended data; src 0 is DGNAI */
#define RDOF_T3_SD_INTR 5 /* a transmit interrupt between sites (tier3.md 3.20): dst the talker to de-key, value the
                           * status (125), data the call's destination (3 octets) and 1 for a talkgroup; never from the
                           * application (rdof_t3_sd_send refuses it) */
#define RDOF_T3_SD_UPOLL 6 /* a UDT short data poll (tier3.md 3.15, 5.9): from the application, a poll from SDMI of an
                            * MS here, value the Polling Format, data[0] the UDTs asked for (len 0: 4); its result's
                            * data is the polled MS's (value its UDT_Format). Also between sites */
#define RDOF_T3_SD_SUPP 7 /* a voice call's supplementary user data between sites (tier3.md 3.21, 5.9): value the
                           * UDT_Format, data as UDT short data's; never from the application */
#define RDOF_T3_SD_SEND 1   /* op: a message */
#define RDOF_T3_SD_RESULT 2 /* op: its outcome */
#define RDOF_T3_SD_F_GROUP 0x01
#define RDOF_T3_SD_F_EMERGENCY 0x02

typedef struct {
    uint8_t op, what;
    uint8_t reason;     /* a result: the air reason (C_ACKD's: 0x44 MS_Accepted, 0x60 Message_Accepted, ...) */
    uint8_t value;      /* UDT_Format, the status (0-127), or a poll's result: the polled status */
    uint8_t flags;      /* RDOF_T3_SD_F_* */
    uint8_t pad_nibble; /* UDT: the header's Pad Nibble */
    uint8_t len;        /* UDT: data octets, 0-46 */
    uint8_t why;        /* a result: RDOF_T3_WHY_* */
    uint32_t src, dst;  /* src 0 from the site: TSI */
    uint32_t stream;    /* on the network: this message's stream ID */
    uint32_t ref;       /* the caller's tag (rdof_t3_sd_send), echoed in its result; a network result: the send's stream */
    uint8_t data[48];
} rdof_t3_sd_t;

/* A message from the site to an MS or a talkgroup; its outcome comes as an RDOF_T3_EV_SD result with its ref. False:
 * no room (8 in progress), a bad address, or bad data. */
bool rdof_t3_sd_send(rdof_t3_site_t *s, const rdof_t3_sd_t *m, int64_t now_ms);
/* A message or a result from the network (mqtt-net sdata) */
void rdof_t3_net_sd(rdof_t3_site_t *s, const rdof_t3_sd_t *m, int64_t now_ms);
/* An RDOF_T3_EV_SD / EV_NET_SD event's message: op, kind what, reason, svc value, why, flags (RDOF_T3_F_GROUP,
 * F_EMERGENCY), lcn the pad nibble, chan the data length, text the data, src, dst, stream, ref. */
void rdof_t3_sd_of_event(const rdof_t3_event_t *e, rdof_t3_sd_t *m);

/* outputs: drain after each call */
bool rdof_t3_event(rdof_t3_site_t *s, rdof_t3_event_t *ev);
void rdof_t3_status(const rdof_t3_site_t *s, rdof_t3_status_t *st);

/* ── MS simulator (t3ms.rs): a radio that knows one site and follows 361-4 for what the site supports ── */

#define RDOF_T3MS_ANSWER_ACCEPT 0
#define RDOF_T3MS_ANSWER_BUSY 1     /* C_NACKU(EquipBusy_Refused) */
#define RDOF_T3MS_ANSWER_REFUSE 2   /* C_NACKU(Recipient_Refused) */
#define RDOF_T3MS_ANSWER_SILENT 3   /* no answer */
#define RDOF_T3MS_ANSWER_FOACSU 4   /* C_ACKU(MS_Alerting), then Call Answer (accept) after answer_ms */

#define RDOF_T3MS_DO_REGISTER 1    /* arg: the talkgroup to subscribe (0: none) */
#define RDOF_T3MS_DO_DEREGISTER 2
#define RDOF_T3MS_DO_GROUP 3       /* arg: talkgroup; talk_ms; flags RDOF_T3MS_F_* */
#define RDOF_T3MS_DO_PRIVATE 4     /* arg: called MS; talk_ms */
#define RDOF_T3MS_DO_CANCEL 5
#define RDOF_T3MS_DO_STATUS 7     /* arg: an MS, a talkgroup (F_TO_GROUP) or TSI; talk_ms: the status (126 alarm, 127 polls) */
#define RDOF_T3MS_DO_DUPLEX 8     /* arg: called MS; talk_ms (a full-duplex call: both talk) */
#define RDOF_T3MS_DO_SD_STATUS 10 /* as DO_STATUS, by the MS's short data path (flags RDOF_T3MS_F_GROUP) */
#define RDOF_T3MS_DO_INTERRUPT 14 /* from the call it listens to: arg the MS or (F_TO_GROUP) the talkgroup; talk_ms; F_ALS
                                     an ALS call's end (status 123), else a payload interrupt (125, tier3.md 3.20) */
#define RDOF_T3MS_F_ALS 0x20      /* DO_PRIVATE: an ALS call; DO_INTERRUPT: its end */
#define RDOF_T3MS_F_EMERGENCY 0x01
#define RDOF_T3MS_F_TO_GROUP 0x02 /* DO_STATUS to a talkgroup */
#define RDOF_T3MS_F_GROUP 0x02    /* DO_SD_STATUS, rdof_t3ms_sdm, rdof_t3ms_data: to a talkgroup */
#define RDOF_T3MS_F_BROADCAST 0x04 /* DO_GROUP: a broadcast call (BCAST_SV), one way: BTV_GRANT */
#define RDOF_T3MS_F_CONFIRMED 0x40 /* rdof_t3ms_data: confirmed data */
#define RDOF_T3MS_F_MULTI 0x08     /* rdof_t3ms_data: multi-item (P_MAINT after the item) */
#define RDOF_T3MS_F_RATE34 0x10    /* rdof_t3ms_data: rate 3/4 blocks (neither rate flag: rate 1/2) */
#define RDOF_T3MS_F_RATE1 0x20     /* rdof_t3ms_data: rate 1 blocks */

#define RDOF_T3MS_IDLE 0     /* on the TSCC, nothing to do */
#define RDOF_T3MS_ACCESS 1   /* random access in progress */
#define RDOF_T3MS_WAITING 2  /* acknowledged, waiting for a grant or the outcome */
#define RDOF_T3MS_PAYLOAD 3  /* on a payload channel */

/* what an MS sends in inbound slot k */
typedef struct {
    uint8_t ch;      /* channel index */
    uint8_t kind;    /* 1 PDU, 2 voice, 3 packet data (dt 6 a data header; 7, 8, 10 a rate 1/2, 3/4, 1 block) */
    uint8_t dt;      /* PDU, packet data */
    uint8_t what;    /* voice: RDOF_T3_VOICE_* */
    uint8_t pdu[10];
    uint8_t group;
    uint8_t offset;  /* sent in offset timing (a duplex call): in the other inbound slot of ch's carrier */
    uint32_t src, dst;
    uint8_t ext[14]; /* a data block (dt 7, 8, 10) or header: its octets 10 on (to 11, 17 or 23) */
    uint8_t so;      /* voice: the LC's Service Options (361-2 table 7.11: Emergency 80h, Broadcast 08h) */
    uint8_t pad2;
} rdof_t3ms_tx_t;

typedef struct {
    uint8_t state, registered, last_reason, ch;
    uint32_t grants, calls_done, calls_failed, ahoys, clears;
    uint32_t heard_src, heard_dst; /* the last call followed */
} rdof_t3ms_status_t;

typedef struct rdof_t3ms rdof_t3ms_t;

size_t rdof_t3ms_size(void);
/* id, its talkgroups (up to 8), the site's channel plan (chan of each carrier), a random seed */
void rdof_t3ms_init(rdof_t3ms_t *ms, uint32_t id, const uint32_t *tg, int ntg, const uint16_t *chans, int ncarriers,
                    uint32_t seed);
void rdof_t3ms_answer(rdof_t3ms_t *ms, uint8_t answer, uint32_t answer_ms);
/* the status (0-127) it answers a status poll with (6.6.6.2.2.1.5; 0 at start) */
void rdof_t3ms_set_status(rdof_t3ms_t *ms, uint8_t status);
/* a test's: its next n ACKs to its confirmed data go unheard, so it sends the message again */
void rdof_t3ms_miss_acks(rdof_t3ms_t *ms, uint8_t n);
/* Its authentication key K and PSN (361-4 6.4.8.1): it answers a challenge C_ACKU(Authentication Response); k NULL:
   no key, it answers C_NACKU(MSNot_Supported). */
void rdof_t3ms_key(rdof_t3ms_t *ms, const uint8_t k[16], uint32_t psn, bool has_psn);
/* queue an action (up to 8 queued) at or after at_ms */
bool rdof_t3ms_do(rdof_t3ms_t *ms, uint8_t what, uint32_t arg, uint32_t talk_ms, uint8_t flags, int64_t at_ms);
/* what it answers a UDT short data poll with (6.6.5.7): up to 46 octets in UDT_Format format; len 0, it refuses
   polls (C_NACKU, Recipient_Refused; the default) */
bool rdof_t3ms_set_poll(rdof_t3ms_t *ms, uint8_t format, const uint8_t *data, size_t len);
/* a UDT short data poll of MS dst (6.6.5): the data in format, in blocks UDTs (1-4); it comes as short data */
bool rdof_t3ms_upoll(rdof_t3ms_t *ms, uint32_t dst, uint8_t format, uint8_t blocks, int64_t at_ms);
/* supplementary user data its voice calls carry from now on (6.4.13: SUPED_SV; up to 46 octets, len 0 none) */
bool rdof_t3ms_set_supp(rdof_t3ms_t *ms, const uint8_t *data, size_t len);
/* the last supplementary user data it took (from SUPLI): its octets into out (up to cap, *len; trailing zero padding
 * included). Returns how many it has taken (0: none, nothing written). */
uint32_t rdof_t3ms_supp_inbox(const rdof_t3ms_t *ms, uint8_t *out, size_t cap, size_t *len);
/* IP connection advice (6.4.11): a registration with IP_Inform 1 adding IPv4 address ip (host order), or (add false)
   deleting it; it stays registered either way */
bool rdof_t3ms_ip_advice(rdof_t3ms_t *ms, uint32_t ip, bool add, int64_t at_ms);
/* UDT short data (up to 46 octets, UDT_Format format) to dst, a talkgroup with RDOF_T3MS_F_GROUP */
bool rdof_t3ms_sdm(rdof_t3ms_t *ms, uint32_t dst, uint8_t flags, uint8_t format, const uint8_t *data, size_t len,
                   int64_t at_ms);
/* A packet data call with one message (SAP 4; up to 12 blocks: 140 octets unconfirmed, 116 confirmed at rate 1/2,
 * 212 / 188 at rate 3/4, 284 / 260 at rate 1) to dst: an MS, the site (TSI, its data ID, IPI) or, RDOF_T3MS_F_GROUP,
 * a talkgroup; RDOF_T3MS_F_CONFIRMED, RDOF_T3MS_F_MULTI, RDOF_T3MS_F_RATE34 or RDOF_T3MS_F_RATE1. Confirmed: a SACK's
 * blocks are sent again, a NACK for the CRC-32 all of them, up to 4 sends; no response in 3 s ends it. */
bool rdof_t3ms_data(rdof_t3ms_t *ms, uint32_t dst, uint8_t flags, const uint8_t *data, size_t len, int64_t at_ms);
/* The last packet data message it took (CRC good; any rate, confirmed ones answered ACK, SACK or NACK): its
 * length, the data into out (up to cap), its source; -1 none. */
int rdof_t3ms_data_inbox(const rdof_t3ms_t *ms, uint8_t *out, size_t cap, uint32_t *src);
/* The last UDT short data it took: its octets into out (up to cap, *len; a character format less its pad nibbles),
 * its source and UDT_Format. Returns how many it has taken (0: none, nothing written). */
uint32_t rdof_t3ms_sd_inbox(const rdof_t3ms_t *ms, uint8_t *out, size_t cap, size_t *len, uint32_t *src,
                            uint8_t *format);
/* outbound slot k on channel ch: a block (dt 0: voice or idle, nothing to read); at is the CACH's AT bit for the
 * next slot (TSCC carrier). pdu: 10 octets; 12 when dt is RDOF_DT_RATE_12 or RDOF_DT_DATA_HEADER on a payload
 * channel, 18 for RDOF_DT_RATE_34, 24 for RDOF_DT_RATE_1 (packet data: the decoded block, rdof_t3rf_burst_block) */
void rdof_t3ms_rx(rdof_t3ms_t *ms, int ch, int64_t k, uint8_t dt, const uint8_t *pdu, bool at);
/* Carrier c's CACH short LC from now on (28 bits; -1 none: ignored): a composite TSCC's P_SYS_Parms keeps an idle MS
 * from hunting while the TSCC carries a call (361-4 5.3.2). */
void rdof_t3ms_slc(rdof_t3ms_t *ms, int carrier, int64_t slc);
/* A Reverse Channel PDU's info (rdof_t3rf_rc_read) heard in an outbound burst: for the talker on payload channel ch,
 * the burst's carrier's other slot (361-1 5.1.5). The MS ceases on RC 0100b (361-4 6.4.15). */
void rdof_t3ms_rc(rdof_t3ms_t *ms, int ch, int info);
/* inbound slot k: true when the MS transmits, filling *tx */
bool rdof_t3ms_tx(rdof_t3ms_t *ms, int64_t k, int64_t now_ms, rdof_t3ms_tx_t *tx);
void rdof_t3ms_status(const rdof_t3ms_t *ms, rdof_t3ms_status_t *st);

/* the structs above, by name, with their Rust size and alignment (i from 0; NULL name at the end) */
typedef struct {
    const char *name;
    size_t size, align;
} rdof_t3_layout_t;
const rdof_t3_layout_t *rdof_t3_layout(size_t i);

#ifdef __cplusplus
}
#endif
