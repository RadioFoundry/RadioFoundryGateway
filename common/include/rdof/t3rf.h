// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * A trunked site's radio layer (docs/spec/fw/tier3.md section 9): the bursts and CACH of a Tier III site's carriers,
 * its payload channels, and the routing of what its receivers hear. The site controller (rdof/t3.h) makes every
 * decision; this builds and reads the air for it through the callbacks in rdof_t3rf_io_t, so it has no tie to the
 * controller itself and runs the same against a test's.
 *
 * Numbering: carrier c (0 .. ncarriers - 1) has channels 2c (its TS1) and 2c + 1 (its TS2). The TSCC is carrier 0's
 * TS1 when carrier 0 has it: channel 0. Every other channel is a payload channel. A slot k is an outbound slot, TS1
 * even: the site's slot count (the UTC grid slot n on a site on the grid); an inbound burst is given the k of the
 * outbound slot it sits under (aligned timing).
 *
 * The PDU level: a control block is its 10 info octets before the CRC (the controller's), with its data type: CSBK,
 * MBC header, MBC continuation (the last block of an MBC: 10 octets and a CRC, as the header), or a data header (a UDT
 * header). This adds the CRC-16 with the data type's mask, BPTC(196,96), the slot type and the sync, and the reverse.
 *
 * No I/O, no allocation, no clock: the platform gives the slot k and serializes the calls. In Rust (rust/src/t3rf.rs).
 */
#ifndef RDOF_T3RF_H
#define RDOF_T3RF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/duplex.h"
#include "rdof/features.h"
#include "rdof/fec.h"
#include "rdof/layer2.h"
#include "rdof/tracker.h"

#ifdef __cplusplus
extern "C" {
#endif

#if RDOF_FEATURE_REPEATER

#define RDOF_T3RF_CARRIERS 4
#define RDOF_T3RF_CHANNELS (2 * RDOF_T3RF_CARRIERS)
#define RDOF_T3RF_QUEUE 24        /* outbound bursts a payload channel holds (1.4 s) */
#define RDOF_T3RF_NET_PREBUFFER 4 /* a network stream's bursts held before it plays (240 ms) */
#define RDOF_T3RF_WARMUP 2        /* idle bursts a payload channel sends first: 4 CACH (tier3.md 3.1) */
#define RDOF_T3RF_LOST_K 17       /* an RF call with no burst for this many slots (about 0.5 s) is lost */

/* ── control blocks ───────────────────────────────────────────────────────── */

/* The CRC-16 mask of a control block's data type (CSBK, MBC header, MBC continuation, data header), -1 for others. */
int rdof_t3rf_mask(int dt);
/* A control block's burst: pdu (10 octets) with its CRC, BPTC, the slot type (cc, dt) and the data sync of family
 * (RDOF_FAMILY_BS outbound, RDOF_FAMILY_MS a radio's inbound). False for a data type without a mask. */
bool rdof_t3rf_pdu_burst(int dt, const uint8_t pdu[10], int cc, rdof_family_t family, uint8_t out[RDOF_BURST_BYTES]);

typedef enum {
    RDOF_T3RF_NOT_PDU = 0, /* not a data sync burst, or not a control block's data type */
    RDOF_T3RF_PDU_OK,      /* pdu holds the 10 octets: BPTC decoded and the CRC checks */
    RDOF_T3RF_PDU_BAD,     /* a control block whose BPTC or CRC fails (on the TSCC inbound: a collision) */
} rdof_trunk_pdu_t;
/* A burst read back: its data type and color code from the slot type (*dt, *cc: -1 when unreadable), and the PDU. The
 * sync is checked against the data syncs of every family (BS, MS, TS1, TS2) with up to 4 bits wrong. */
rdof_trunk_pdu_t rdof_t3rf_burst_pdu(const uint8_t burst[RDOF_BURST_BYTES], int *dt, int *cc, uint8_t pdu[10]);
/* A block's burst: a control block (as rdof_t3rf_pdu_burst; octets 10 and 11 ignored) or a data block as given, no
 * CRC of its own (a message's last block holds the message's): RDOF_DT_RATE_12 12 octets (BPTC), RDOF_DT_RATE_34
 * 18 (the rate 3/4 trellis), RDOF_DT_RATE_1 24 (uncoded). blk: 12 octets, 18 or 24 for those. */
bool rdof_t3rf_block_burst(int dt, const uint8_t *blk, int cc, rdof_family_t family, uint8_t out[RDOF_BURST_BYTES]);
/* rdof_t3rf_burst_pdu, and a data block too: rate 1/2 (OK when its BPTC decodes), rate 3/4 and rate 1 (OK: the
 * trellis always finds a path; the block's CRCs decide). blk: the block, 12, 18 or 24 octets as *dt. */
rdof_trunk_pdu_t rdof_t3rf_burst_block(const uint8_t burst[RDOF_BURST_BYTES], int *dt, int *cc, uint8_t blk[24]);

/* The 4 CACH of slots k .. k + 3 (k % 4 == 0) carrying short LC slc28 (SLCO in bits 27-24), TC from each slot's
 * parity, AT at[0..3]: 24 bits each, first on air in the top bit of byte 0. */
void rdof_t3rf_cach4(uint32_t slc28, const int at[4], int64_t k, uint8_t out[4][3]);
/* One CACH back: AT, TC (the slot after it: 1, 2), LCSS and the 17-bit payload; false when the TACT is unreadable.
 * The payloads of four in a row (LCSS first, continuation, continuation, last) join to the short LC
 * (rdof_t3rf_slc_join). */
bool rdof_t3rf_cach_read(const uint8_t c3[3], int *at, int *slot, int *lcss, uint32_t *payload17);
bool rdof_t3rf_slc_join(const uint32_t rows[4], uint32_t *slc28);

/* ── payload channels ─────────────────────────────────────────────────────── */

/* What rdof_t3rf_io_t's activity reports. */
enum {
    RDOF_T3RF_HEADER = 1, /* a voice LC header (the addresses its LC gives) */
    RDOF_T3RF_VOICE,      /* voice with no header first (late entry; the channel's addresses), then each voice sync
                             burst (once a superframe: the talker is still there) */
    RDOF_T3RF_TERM,       /* a terminator with LC */
    RDOF_T3RF_LOST,       /* RF voice stopped without a terminator */
    RDOF_T3RF_DATA,       /* a packet data block repeated (the header goes to pdu; tier3.md 3.16) */
};
/* What a payload channel's slot carries (rdof_t3rf_ch_next). */
typedef enum {
    RDOF_T3RF_IDLE = 0,
    RDOF_T3RF_TRAFFIC, /* voice (or its header / terminator), repeated from RF or from the network */
    RDOF_T3RF_CONTROL, /* the controller's PDU (P_CLEAR, a grant) */
} rdof_t3rf_out_t;

typedef struct {
    uint8_t control; /* the MMDVM control byte: RDOF_SYNC_AUDIO, a voice burst's number, RDOF_SYNC_DATA | dt */
    bool net;        /* from the network (else RF) */
    uint8_t b[RDOF_BURST_BYTES];
} rdof_t3rf_burst_t;

typedef struct {
    int index, cc;
    bool open;
    uint32_t src, dst; /* the call the controller opened it for */
    bool group;
    int warmup;        /* idle bursts still to send before anything else */
    rdof_t3rf_burst_t q[RDOF_T3RF_QUEUE];
    int qh, qn;
    uint8_t ctl[RDOF_BURST_BYTES]; /* the controller's PDU burst, sent ctl_left more times */
    int ctl_left;
    bool close_after;  /* closes when ctl_left reaches 0 */
    /* RF: the inbound call being repeated */
    bool rf;
    int64_t rf_last;
    bool rf_regen_on, rf_new;
    rdof_voice_regen_t rf_regen;
    uint32_t rf_src, rf_dst;
    bool rf_group;
    /* the network's stream */
    bool net, net_started, net_regen_on;
    int net_held;
    rdof_voice_regen_t net_regen;
    /* counters */
    uint32_t rx_bursts, rx_pdus, tx_traffic, tx_idle, tx_control, net_bursts, net_dropped, overflows, lost;
    /* the Reverse Channel for the other slot's talker (361-1 5.1.5): RDOF_T3_RC_SEND | RC info, 0 none */
    int rc;
    bool after_term;   /* the last burst out was a terminator: no RC in the next */
    int64_t rc_k;      /* the slot of the last RC sent (360 ms apart) */
    uint32_t rc_sent;
    bool duplex;       /* a full-duplex call's party (rdof_t3rf_duplex) */
    uint8_t so;        /* the call's Service Options for an LC the layer makes (late entry, network voice without
                          one): 361-2 table 7.11, Emergency 80h, Broadcast 08h; the owner sets it, closing clears it */
} rdof_t3rf_ch_t;

/* ── the site ─────────────────────────────────────────────────────────────── */

typedef struct {
    /* The TSCC's outbound block for slot k (even): its data type and 10 octets (12 for RDOF_DT_RATE_12, a UDT's
     * appended block); false sends an idle burst. */
    bool (*tscc)(void *ctx, int64_t k, int *dt, uint8_t pdu[12]);
    /* Carrier c's CACH before slot k: *at (preset -1: the transmitter's own) and, at k % 4 == 0, *slc28 (preset -1:
     * keep) the short LC from k on: C_SYS_Parms on the TSCC's carrier, P_SYS_Parms or Act_Updt on the others. */
    void (*cach)(void *ctx, int carrier, int64_t k, int *at, int32_t *slc28);
    /* A control block heard with a good CRC on channel ch (the TSCC inbound, or a payload channel's: P_MAINT); on
     * the TSCC also a rate 1/2 block (RDOF_DT_RATE_12, 12 octets: a UDT upload's appended block). */
    void (*pdu)(void *ctx, int ch, int64_t k, int dt, const uint8_t *pdu, int rssi);
    /* A control block that failed its CRC on the TSCC inbound (counts a collision). */
    void (*bad)(void *ctx, int ch, int64_t k);
    /* Voice on payload channel ch's inbound: what (RDOF_T3RF_HEADER ...), with the addresses. */
    void (*activity)(void *ctx, int ch, int64_t k, int what, uint32_t src, uint32_t dst, bool group);
    /* A regenerated inbound burst of payload channel ch for the network, with its addresses (may be NULL). */
    void (*uplink)(void *ctx, int ch, const rdof_frame_t *f);
    void *ctx;
} rdof_t3rf_io_t;

typedef struct {
    rdof_t3rf_io_t io;
    int ncarriers, cc;
    bool tscc;         /* channel 0 is the TSCC */
    rdof_t3rf_ch_t ch[RDOF_T3RF_CHANNELS];
    uint32_t tscc_sent, tscc_idle, rx_tscc, rx_tscc_bad, rx_ignored;
} rdof_t3rf_t;

/* ncarriers 1..RDOF_T3RF_CARRIERS; tscc: carrier 0's TS1 is the TSCC. False when ncarriers is out of range. */
bool rdof_t3rf_init(rdof_t3rf_t *t, const rdof_t3rf_io_t *io, int ncarriers, int cc, bool tscc);
/* Channel ch's state, NULL for none (or the TSCC). */
rdof_t3rf_ch_t *rdof_t3rf_channel(rdof_t3rf_t *t, int ch);

/* The controller's grant took payload channel ch for a call: open it (a fresh one: warmup idles first; already open:
 * the new call's addresses, the channel carries on). */
bool rdof_t3rf_open(rdof_t3rf_t *t, int ch, uint32_t src, uint32_t dst, bool group, int warmup);
/* Payload channel ch is a party's of a full-duplex call (361-4 6.6.9.4): its carrier in offset timing (the
 * inbound in the carrier's other inbound slot), its inbound repeated on the other channel's outbound. Closing the
 * channel ends it. */
bool rdof_t3rf_duplex(rdof_t3rf_t *t, int ch, bool on);
/* Closed at once: what's queued is dropped, the channel goes idle. */
void rdof_t3rf_close(rdof_t3rf_t *t, int ch);
/* The controller's PDU (dt, 10 octets) on channel ch's outbound, ahead of any voice, repeats times (P_CLEAR: 4);
 * close_after: the channel closes once it has gone out (queued voice is dropped then). False: no such channel, or a
 * data type without a mask. */
bool rdof_t3rf_control(rdof_t3rf_t *t, int ch, int dt, const uint8_t pdu[10], int repeats, bool close_after);

/* The Reverse Channel PDU channel ch's outbound carries once for its carrier's other slot (rdof_t3_ch_out_t.rc: 0
 * changes nothing, -1 drops one not yet sent): in place of a voice burst F's embedded LC or of an idle burst's sync (an embedded / dedicated outbound RC), never
 * in a header or a control block, not right after a terminator, 360 ms apart (361-1 5.1.5.1, 6.4.2). Channel 0 may
 * be the TSCC: its RC goes in a slot the controller left without a block (an idle burst). */
void rdof_t3rf_rc(rdof_t3rf_t *t, int ch, int rc);
/* The RC info in a burst's embedded field (an outbound RC burst: EMB PI = 1, LCSS single fragment), or -1. */
int rdof_t3rf_rc_read(const uint8_t burst[RDOF_BURST_BYTES]);
/* A data burst the site sends (a header or block; control as MMDVM's DMR_DATA, the burst with any sync) on open
 * channel ch's outbound, regenerated with the BS sync and the colour code. False: closed, not data, or no room. */
bool rdof_t3rf_data(rdof_t3rf_t *t, int ch, uint8_t control, const uint8_t burst[RDOF_BURST_BYTES]);
/* Room on open channel ch's outbound queue, in bursts (0: closed or full). */
int rdof_t3rf_room(const rdof_t3rf_t *t, int ch);

/* A burst heard on carrier c under outbound slot k (TS1 even): the TSCC's inbound to pdu / bad, a payload channel's
 * to that channel (repeated on its outbound, voice to activity and uplink). rssi in dBm (0: unknown). */
void rdof_t3rf_rx(rdof_t3rf_t *t, int c, int64_t k, const rdof_burst_t *b, int rssi);
/* A network burst for payload channel ch (f->dt, f->n, f->burst as a box sends it on mqtt-net): regenerated and
 * queued behind a prebuffer, or dropped while RF holds the channel. False: dropped (closed channel, RF, full). */
bool rdof_t3rf_net(rdof_t3rf_t *t, int ch, const rdof_frame_t *f);
/* The network's stream on ch ended without a terminator: what's held plays out. */
void rdof_t3rf_net_end(rdof_t3rf_t *t, int ch);
/* Timers at slot k: RF calls lost (activity LOST). Call once a slot or so. */
void rdof_t3rf_tick(rdof_t3rf_t *t, int64_t k);

/* Payload channel ch's next outbound burst, for slot k: the controller's PDU, warm-up idle, queued traffic, idle. */
rdof_t3rf_out_t rdof_t3rf_ch_next(rdof_t3rf_t *t, int ch, int64_t k, uint8_t *control,
                                      uint8_t out[RDOF_BURST_BYTES]);
/* Carrier c's slot k: the burst (the TSCC's block, or the payload channel's), *control its MMDVM control byte, *at
 * and *slc28 from io.cach. The SDR modem's slot source (rdof_sdrmodem_source_t) for the TSCC's carrier is this. */
bool rdof_t3rf_slot(rdof_t3rf_t *t, int c, int64_t k, uint8_t burst[RDOF_BURST_BYTES], uint8_t *control, int *at,
                     int32_t *slc28);
/* Carrier c carries something: a payload channel open, or a PDU, warm-up or traffic still to send. */
bool rdof_t3rf_carrier_busy(const rdof_t3rf_t *t, int c);

/* ── a payload carrier on a split TX half (rdof/split.h) ──────────────────── */

struct rdof_split;
typedef struct {
    int carrier;
    bool keyed;                /* DMR_START 01 sent */
    int64_t next_k;            /* the next slot to write */
    int32_t slc_sent;          /* the short LC last sent (DMR_SHORTLC), -1 none */
    bool scheduled;            /* the last pump used TX_SCHED (else the FIFO) */
    uint32_t sched_bursts, fifo_bursts, short_lcs, keyups;
} rdof_t3rf_split_t;

void rdof_t3rf_split_init(rdof_t3rf_split_t *o, int carrier);
/* Carrier o->carrier's bursts to split s for the slots up to k_now + ahead (k_now: the slot on air now), keyed while
 * the carrier is busy (DMR_START 01 / 00 upstream), the short LC at each multiple of 4 when it changes (DMR_SHORTLC).
 * TX_SCHED when the split's scheduled stream is up (rdof_split_sched_ok: each traffic or control burst for its slot,
 * idle slots left to the half), else the FIFO (DMR_DATA1 / DMR_DATA2, every slot in order, the half's prebuffer
 * after it). ahead: 4 for the scheduled stream (past the half's guard), 1 for the FIFO; 0: by the mode. */
void rdof_t3rf_split_pump(rdof_t3rf_t *t, rdof_t3rf_split_t *o, struct rdof_split *s, int64_t k_now, int ahead,
                           double now);

/* ── layouts ──────────────────────────────────────────────────────────────── */

/* The Rust's (rust/src/t3rf.rs) size and alignment of each struct above it mirrors, i from 0 until one with a NULL
 * name: rdof_t3rf_tests compares them with the C's. */
typedef struct {
    const char *name;
    size_t size, align;
} rdof_t3rf_layout_t;
const rdof_t3rf_layout_t *rdof_t3rf_layout(size_t i);

#endif /* RDOF_FEATURE_REPEATER */

#ifdef __cplusplus
}
#endif

#endif
