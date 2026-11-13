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
    uint8_t pad1;
    uint32_t call_timers_ms, vote_now_ms, ann_tscc_ms; /* CallTimer_Parms, Vote_Now, Ann_WD_TSCC; 0: not broadcast */
    uint32_t awake_ms; /* how long an MS in power save stays awake after a PDU (the site's T_Awake) */
} rdof_t3_cfg_t;

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
} rdof_t3_ms_policy_t;

#define RDOF_T3_HOW_REGISTER 1
#define RDOF_T3_HOW_CALL 2
#define RDOF_T3_HOW_LOADED 3

typedef struct {
    uint32_t id, sub_tg; /* sub_tg 0: none */
    uint8_t how, ntg, area, pad;
    uint32_t tg[RDOF_T3_REG_TGS];
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

typedef struct {
    uint8_t op, reason, flags, svc; /* svc: C_RAND's service options */
    uint32_t src, dst;
    uint32_t stream; /* this message's stream ID */
    uint32_t ref;    /* the request's stream ID this answers (0 in a request) */
} rdof_t3_setup_t;

typedef struct {
    uint16_t syscode, chan; /* the neighbor's C_SYScode and TSCC channel */
    uint8_t active_known, active, prio_confirmed, prio_adjacent;
} rdof_t3_neighbor_t;

/* events */
#define RDOF_T3_EV_CH_OPEN 1     /* a payload channel's call starts: ch, src, dst, flags, stream (network), chan, lcn */
#define RDOF_T3_EV_CH_CLOSE 2    /* ch, why (RDOF_T3_CLOSE_*) */
#define RDOF_T3_EV_CH_STREAM 3   /* a network stream now feeds open channel ch: stream, src */
#define RDOF_T3_EV_NET_SETUP 4   /* publish a setup message: op, reason, flags (setup flags), svc, src, dst, stream, ref */
#define RDOF_T3_EV_NET_SUBS 5    /* the talkgroups to subscribe to changed: rdof_t3_subs() */
#define RDOF_T3_EV_RECORD 6      /* write (rdof_t3_record()) or, with why 1, delete the record of src */
#define RDOF_T3_EV_ACTIVITY 7    /* kind (RDOF_T3_ACT_*), op (action), src, dst, ch, chan, lcn, flags, reason */
#define RDOF_T3_EV_NET_DROP 8    /* the network stream won't be carried: stream, why */
#define RDOF_T3_EV_LOG 9         /* text */

#define RDOF_T3_CLOSE_HANG 1     /* hang time expired */
#define RDOF_T3_CLOSE_TIMER 2    /* call timer */
#define RDOF_T3_CLOSE_DISCON 3   /* P_MAINT DISCON */
#define RDOF_T3_CLOSE_CMD 4      /* a command or a cancel */
#define RDOF_T3_CLOSE_WITHDRAW 5 /* the site went off the air */
#define RDOF_T3_CLOSE_PREEMPT 6  /* cleared for an emergency call (361-4 6.4.14.3.1) */

#define RDOF_T3_ACT_REG 1   /* op: 1 register, 2 deregister, 3 expire, 4 deny, 5 refuse */
#define RDOF_T3_ACT_GRANT 2 /* flags: group, late, net */
#define RDOF_T3_ACT_QUEUE 3 /* op: 1 in, 2 out, 3 drop; reason: the C_NACKD reason when dropped */
#define RDOF_T3_ACT_SETUP 4 /* op, reason: as the setup message */
#define RDOF_T3_ACT_REFUSE 5 /* reason: the C_NACKD reason */
#define RDOF_T3_ACT_STATUS 6 /* a status delivered or not (361-4 6.6.6): op the status, flags group, reason the caller's ack */

#define RDOF_T3_F_GROUP 0x01
#define RDOF_T3_F_EMERGENCY 0x02
#define RDOF_T3_F_NET 0x04    /* started from the network */
#define RDOF_T3_F_WIDE 0x08   /* carried on the network (publish the channel's RF voice) */
#define RDOF_T3_F_ALLCALL 0x10
#define RDOF_T3_F_LATE 0x20
#define RDOF_T3_F_PRIVATE_NET 0x40 /* an individual call whose other party is on the network */

typedef struct {
    uint8_t type, ch, why, flags;
    uint8_t op, reason, svc, kind;
    uint16_t chan;
    uint8_t lcn, pad;
    uint32_t src, dst, stream, ref;
    char text[64];
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
    uint8_t pad[3];
    uint32_t slc_tscc;    /* C_SYS_Parms for the TSCC carrier */
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
    rdof_t3_ch_status_t ch[RDOF_T3_CHANNELS];
} rdof_t3_status_t;

/* payload voice activity (rdof_t3_rx_voice) */
#define RDOF_T3_VOICE_HEADER 1
#define RDOF_T3_VOICE_FRAME 2
#define RDOF_T3_VOICE_TERM 3
#define RDOF_T3_VOICE_LOST 4

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

/* inbound: a block with a good CRC, or (TSCC only) a burst with a good sync and a bad CRC: a collision */
void rdof_t3_rx_pdu(rdof_t3_site_t *s, int ch, int64_t k, uint8_t dt, const uint8_t pdu[10], int rssi);
void rdof_t3_rx_bad(rdof_t3_site_t *s, int ch, int64_t k);
/* payload voice: what RDOF_T3_VOICE_*, with the LC's addresses */
void rdof_t3_rx_voice(rdof_t3_site_t *s, int ch, int64_t k, uint8_t what, uint32_t src, uint32_t dst, bool group);

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
/* the talkgroups the registered MSs need (subscriptions, recent calls, provisioning); static_tg isn't included */
size_t rdof_t3_subs(const rdof_t3_site_t *s, uint32_t *tg, size_t cap);
void rdof_t3_withdraw(rdof_t3_site_t *s, bool off);
/* "deregister" (arg1 MS), "clear" (arg1 chan, arg2 LCN), "withdraw", "restore", "resend_records", "status",
   "mass_reg" (arg1 the window in s, 0 cancels; arg2 one MS, 0 all) */
bool rdof_t3_command(rdof_t3_site_t *s, const char *cmd, uint32_t arg1, uint32_t arg2);

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
#define RDOF_T3MS_DO_STATUS 7     /* arg: an MS, a talkgroup (F_TO_GROUP) or TSI; talk_ms: the status (126 alarm) */
#define RDOF_T3MS_F_EMERGENCY 0x01
#define RDOF_T3MS_F_TO_GROUP 0x02 /* DO_STATUS to a talkgroup */

#define RDOF_T3MS_IDLE 0     /* on the TSCC, nothing to do */
#define RDOF_T3MS_ACCESS 1   /* random access in progress */
#define RDOF_T3MS_WAITING 2  /* acknowledged, waiting for a grant or the outcome */
#define RDOF_T3MS_PAYLOAD 3  /* on a payload channel */

/* what an MS sends in inbound slot k */
typedef struct {
    uint8_t ch;      /* channel index */
    uint8_t kind;    /* 1 PDU, 2 voice */
    uint8_t dt;      /* PDU */
    uint8_t what;    /* voice: RDOF_T3_VOICE_* */
    uint8_t pdu[10];
    uint8_t group, pad;
    uint32_t src, dst;
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
/* queue an action (up to 8 queued) at or after at_ms */
bool rdof_t3ms_do(rdof_t3ms_t *ms, uint8_t what, uint32_t arg, uint32_t talk_ms, uint8_t flags, int64_t at_ms);
/* outbound slot k on channel ch: a block (dt 0: voice or idle, nothing to read); at is the CACH's AT bit for the
 * next slot (TSCC carrier) */
void rdof_t3ms_rx(rdof_t3ms_t *ms, int ch, int64_t k, uint8_t dt, const uint8_t pdu[10], bool at);
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
