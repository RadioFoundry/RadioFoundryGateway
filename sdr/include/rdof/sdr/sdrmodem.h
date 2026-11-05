// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The SDR as an MMDVM modem: what an sdr_transceiver's roamer or repeater
 * talks to (docs/reference.md, "Transmit"). Host frames in (the MMDVM serial
 * protocol, docs/spec/mmdvm-protocol.md in RadioFoundry, MIT), replies
 * and DMR_DATA out, as an MMDVM_HS on protocol 1 would send them. The bursts
 * it hears come from the SDR's receiver (one channel: the RX frequency); what
 * it sends goes through sdr's modulator, pulled by the front end.
 *
 * Simplex (half duplex, SET_CONFIG's simplex flag): bursts from DMR_DATA2,
 * one every 60 ms, each with its ramps; heard bursts that overlap one it sent
 * are dropped (the receiver is muted while it transmits); everything heard
 * goes up as DMR_DATA2; DMR_LOST2 when a call stops without a terminator. A data
 * packet's call ends at its last block (the header says how many): no carrier and
 * no DMR_LOST after it.
 *
 * Dual capacity direct mode (simplex with RDOF_MMDVM_DCDM; "DCDM" in the
 * description): two simplex calls on the one frequency, one per TDMA slot
 * (ETSI TS 102 361-1 TDMA direct mode). DMR_DATA1 bursts go out in TS1,
 * DMR_DATA2 in TS2, each slot one burst every 60 ms keyed on its own with its
 * ramps, the two slots 30 ms apart; the host frames them (the TS1 / TS2 direct
 * mode syncs, repeater.md R13.4). The slot timing is the channel's: taken
 * from the last TS1 / TS2 burst heard while it isn't sending, else its own
 * from its first burst. Heard bursts go up by their sync: TS1 as DMR_DATA1,
 * TS2 (and MS, plain direct mode) as DMR_DATA2, with DMR_LOST1 / LOST2; the
 * other slot is heard while it sends, its own bursts are muted. Both slots'
 * space in the status.
 *
 * Duplex (full duplex, only when the front end can): DMR_START keys a
 * continuous BS transmission, slot after slot, each with a CACH and the slot's
 * queued burst or an idle one. The CACH carries the short LC from DMR_SHORTLC
 * (the Null message until one comes), a quarter in each, starting on a slot
 * counter that's a multiple of 4; its AT bit is the inbound slot's busy state
 * (a call from its first burst to its terminator or DMR_LOST, 360 ms after a
 * data burst), as RadioFoundry's split-duplex.md 5.2 and 7 describe it. Heard bursts
 * go up on their own slot (DMR_DATA1 / DATA2), DMR_LOST1 / LOST2 likewise.
 *
 * Split duplex (RDOF_FEATURE_REPEATER): either half of RadioFoundry's split
 * duplex extension (vendor command 0xD6, " SPLIT1" in the description; RadioFoundry
 * docs/spec/split-duplex.md, MIT), so the SDR can be either half of
 * rdof/split.h's pair, with a radio or another SDR modem as the other. INFO
 * offers the RX role always and the TX role when the front end transmits
 * (io.tx_tune given). The
 * modem clock is the front end's sample counter (48 kS/s, in µs).
 *
 * Role 2, RX half: it never transmits (DMR_DATA NAKed), takes SET_CONFIG's
 * duplex flag and a TX frequency it can't send on, and labels each burst from
 * the epoch SET_EPOCH gives (TS1 in an even outbound slot after it, TS2 in an
 * odd one: aligned timing, a radio's TSx under outbound TSx); without a valid epoch (none, or older than 120 s) it goes up as
 * DMR_DATA2 and is counted.
 *
 * Role 1, TX half: the duplex downlink above on a slot clock that runs from
 * SET_ROLE 1 (restarted by SET_FREQ), keyed or not, so GET_EPOCH's slot start
 * holds for the role. It takes SET_CONFIG's duplex flag on a half duplex
 * front end and an RX frequency the receiver doesn't hear (it hears nothing in
 * this role). A slot starts sending once SET_ROLE's prebuffer is queued (or
 * the first burst has waited that long); the AT bits are SET_AT's mask (idle
 * 2 s after the last one), ORed in mode 0 with the outbound slots carrying
 * something; a key-up longer than max_key unkeys for the cool-down. SET_ROLE's
 * power cap is taken but not applied: the SDR sends at its tx_power_dbm, less
 * SET_FREQ's RF level (io.tx_level).
 *
 * Either role ends with SET_ROLE 0, a role change, or the lease (no 0xD6
 * request for lease_s), and drops what it had.
 *
 * UTC slot timing (slot_timing utc, docs/spec/slot-timing.md): with
 * rdof_sdrmodem_grid on and a map from the stream's samples to UTC (the
 * platform's sample clock, rdof/slotgrid.h), the duplex slot clock sits on the
 * UTC grid: slot k is grid slot n with TS1 on even n. Unkeyed (or the first
 * slot) it snaps there; keyed it is steered a sample a slot at most (a sample
 * slipped or inserted between slots: 21 µs at 48 kS/s), so a key-up never
 * jumps. GET_EPOCH's counter is then n mod 2^32. Heard bursts are labeled from
 * the grid (inbound TS1 in even n: aligned timing) instead of an epoch. Without a map (the
 * gate free, or no clock yet) the slot clock runs on as it is. The gate saying
 * stop unkeys it and refuses DMR data. The carrier is moved by the crystal
 * error the platform gives (rdof_sdrmodem_carrier_ppm).
 *
 * Also the draft UTC additions to RadioFoundry's split duplex extension (rdof/splitops.h)
 * as a TX half: INFO offers ALIGN and TX_SCHED; ALIGN snaps the slot clock to
 * the host's target unkeyed (keyed: the counter only, within 1 ms on parity);
 * stream mode 1 sends TX_SCHED's bursts in their counter's slot, idle between.
 *
 * Positions: the modulator's sample index (tx_fs) and the receiver's 48 kS/s
 * burst positions both count from the stream's start; rx_fs / tx_fs relates
 * them. Not thread safe: the platform serializes the calls.
 */
#ifndef RDOF_SDR_SDRMODEM_H
#define RDOF_SDR_SDRMODEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/mmdvm.h"
#include "rdof/sdr/demod.h"
#include "rdof/sdr/mod.h"
#include "rdof/sigmode.h"
#include "rdof/slotgrid.h"
#include "rdof/splitops.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_SDRMODEM_QUEUE 8      /* bursts a slot */
#define RDOF_SDRMODEM_SENT 32      /* bursts sent, remembered (rdof_sdrmodem_sent_bits) */
#define RDOF_SDRMODEM_SCHED 16     /* TX_SCHED bursts held */
#if RDOF_FEATURE_REPEATER
#define RDOF_SDRMODEM_DESC "RadioFoundry Gateway SDR modem (MMDVM_HS compatible) DCDM SPLIT1"
#else
#define RDOF_SDRMODEM_DESC "RadioFoundry Gateway SDR modem (MMDVM_HS compatible) DCDM"
#endif
#define RDOF_SDRMODEM_LOST_S 0.5   /* a call heard this long ago without a terminator is lost */
#define RDOF_SDRMODEM_NOT_HEARD (-2) /* rx_channel: no channel hears it, but tune anyway (a visit to transmit) */

typedef struct {
    /* A frame for the host. */
    void (*reply)(void *ctx, const uint8_t *frame, size_t len);
    /* The receiver channel for an RX frequency, or -1 if none hears it (SET_FREQ refused), or
     * RDOF_SDRMODEM_NOT_HEARD: none hears it but the modem may tune there to transmit (it hears nothing there). */
    int (*rx_channel)(void *ctx, uint32_t hz);
    /* Tune the transmitter: false if the front end can't; else where the modulator's carrier goes from its center
     * (Hz) in *offset_hz. */
    bool (*tx_tune)(void *ctx, uint32_t hz, double *offset_hz);
    void *ctx;
    /* Optional: the front end's 48 kS/s sample position now (the split's TIME), extrapolated from the last samples
     * read or sent. NULL: the latest position the modem has seen. */
    int64_t (*now48)(void *ctx);
    /* Optional: SET_FREQ's RF level (0..255) for the transmitter, after tx_tune. The host takes it as a level of
     * its device's map (rdof_txpower_sdr_map: 255 its tx.power_dbm). */
    void (*tx_level)(void *ctx, int level);
} rdof_sdrmodem_io_t;

typedef struct {
    uint8_t b[RDOF_BURST_BYTES];
} rdof_sdrmodem_burst_t;

typedef struct rdof_sdrmodem {
    rdof_sdrmodem_io_t io;
    bool can_full;              /* the front end has separate RX and TX chains */
    uint32_t rx_fs, tx_fs;
    rdof_mod_t mod;
    bool mod_ok;
    rdof_mmdvm_reader_t reader;
    /* what the host set */
    bool duplex;
    int mode, cc, channel;      /* channel -1: none tuned (or, as the TX half, none heard) */
    bool tuned;                 /* SET_FREQ taken */
    uint32_t rx_hz, tx_hz;
    /* transmit */
    rdof_sdrmodem_burst_t q[2][RDOF_SDRMODEM_QUEUE];
    int qh[2], qn[2];
    int64_t pos;                /* the next sample the front end pulls */
    int64_t next_start;         /* simplex: the earliest start of the next burst */
    bool dcdm;                  /* dual capacity direct mode (SET_CONFIG simplex with RDOF_MMDVM_DCDM) */
    bool dc_timed;              /* DCDM: dc_t0 holds a TS1 burst's start (modulator samples); TS2 a slot later */
    int64_t dc_t0;
    int64_t dc_seen;            /* DCDM: the latest burst start heard or queued on the timing */
    int64_t dc_last;            /* DCDM: the last burst queued (start); INT64_MIN none */
    int64_t dc_next[2];         /* DCDM: per slot, its next burst no sooner than this */
    uint32_t dc_timings;        /* DCDM: times the timing was taken from a burst heard */
    bool keyed, unkey;          /* duplex: DMR_START on; off asked */
    double inbound_off;         /* duplex: the last handheld burst's place against its inbound slot, in slots */
    int64_t tx_late48;          /* duplex: the transmit path's delay past what's assumed, at 48 kS/s, set by the platform
                                 * from what it measures (an SDR's buffers vary key-up to key-up); 0: none */
    int64_t base;               /* duplex: slot 0's start (its CACH's first symbol) */
    long slot_k;                /* duplex: the next slot to queue */
    bool ramp_up;               /* duplex: the next slot queued is the key-up's first */
    long key_k;                 /* duplex: the slot DMR_START's key-up started on, -1 none queued yet */
    uint32_t slot_skips;        /* split TX half: the slot clock fell behind mid key-up and jumped */
    long skip_from, skip_to;    /* the last jump: slots skip_from to skip_to - 1 never went out */
    uint32_t slc[4], slc_next[4]; /* duplex: the short LC's CACH payloads on air, and the next */
    int64_t sent48[RDOF_SDRMODEM_SENT]; /* 48 kS/s positions the receiver would report for bursts sent (the mute,
                                         * rdof_sdrmodem_sent_bits); duplex: idle bursts too */
    uint8_t sent_b[RDOF_SDRMODEM_SENT][RDOF_BURST_BYTES]; /* and the bursts */
    int nsent;
    /* receive */
    int64_t now48;              /* the latest position seen */
    int64_t last48[2];          /* per slot (simplex: [1]): the last burst heard in a call */
    int64_t data48[2];          /* per slot: busy until here after a data burst */
    bool in_call[2], voice[2];  /* per slot: a call heard (any burst but a terminator); a voice call */
    int data_left[2];           /* per slot: a data packet's blocks still to come (its last one ends the call) */
    uint32_t short_lcs, bad_short_lcs;
    /* split duplex */
    int role, lease_s;          /* 0 normal, 1 TX half, 2 RX half; the role's lease, s */
    int64_t role_at48;          /* the last 0xD6 request */
    bool has_epoch;
    uint32_t epoch_us, period_ns; /* SET_EPOCH's: an outbound TS1's start in the modem clock, the slot length */
    int64_t epoch_at48;
    int32_t residual_us;        /* the last burst's slot start against the epoch's prediction */
    uint32_t labeled[2], unlabeled, lease_expiries;
    /* split duplex TX half */
    bool clock_running;         /* the slot clock (base, slot_k) runs */
    int prebuffer, power_cap;   /* SET_ROLE's */
    int64_t max_key48, cooldown48;
    int64_t keyed_at48, cool_until48;
    int at_mask, at_mode;       /* SET_AT's */
    bool has_at;
    int64_t at_at48;
    bool streaming[2], tx_active[2], last_term[2]; /* per outbound slot */
    int64_t wait48[2];          /* when the first burst of a stream not started was queued */
    uint32_t underruns[2], cach_bursts;
    /* UTC slot timing */
    bool grid;                  /* slot_timing utc */
    int grid_offset_us;
    bool grid_ok;               /* a map: 48 kS/s position grid_i0 at UTC grid_t0 ns, grid_rate positions a second */
    int64_t grid_i0, grid_t0;
    double grid_rate;
    bool grid_placed;           /* slot k is UTC slot grid_n0 + k (grid_n0 a multiple of 4) */
    int64_t grid_n0;
    int64_t grid_err_ns;        /* the last slot's start against its target, before steering */
    uint32_t grid_slips, grid_snaps, grid_labeled;
    bool stopped;               /* the gate says stop */
    double carrier_ppm, tune_off_hz;
    int rf_level;               /* the last SET_FREQ's RF level (255 when it had none) */
    uint32_t counter_base;      /* slot k's counter is counter_base + k (GET_EPOCH, TX_SCHED) */
    /* split duplex TX half: ALIGN and TX_SCHED (rdof/splitops.h) */
    int stream_mode;
    bool aligned;               /* an ALIGN taken since the last retune */
    bool align_target;          /* slot align_k starts at modulator sample align_pos, slot_p samples a slot */
    double align_pos, slot_p;
    int64_t align_k;
    int32_t align_phase_us;
    int align_flags;
    struct {
        bool used;
        uint32_t counter;
        uint8_t control;
        uint8_t burst[RDOF_BURST_BYTES];
    } sched[RDOF_SDRMODEM_SCHED];
    int64_t sched_last48;       /* the last scheduled burst sent */
    uint32_t sched_sent, sched_late, sched_early_full, sched_replaced, sched_cooling;
    bool queue_keyed;           /* keyed by a queued burst, not DMR_START 01: unkeys 1 s after the last */
    /* counters */
    uint32_t tx_bursts, idle_bursts, rx_bursts, muted, lost, refused;
    char error[96];
    /* a signal mode's transmission (rdof_sdrmodem_signal) */
    bool sig_on;
    rdof_sig_src_t sig;
    double sig_off_hz;          /* the carrier from the stream's center (tx_tune's) */
    uint32_t sig_phase;         /* I/Q: the phase accumulator, 2^32 a turn */
    int64_t sig_done_at;        /* the first sample past the source's last keyed one, -1 not yet */
    int sig_d;                  /* the stream's samples a source sample (tx_fs / its rate) */
    int sig_k;                  /* the stream's samples into the source sample's interval (sig_pull) */
    float sig_h0, sig_h1, sig_e0, sig_e1; /* the interval's ends: frequency (I/Q: the phase step), envelope */
    rdof_cf_t sig_x0, sig_x1;   /* or I/Q */
} rdof_sdrmodem_t;

/* rx_fs: the receiver's wideband rate (a multiple of 48 kS/s); tx_fs: the modulator's; can_full: full duplex.
 * Returns 0 or -1 (the modulator wouldn't start). */
int rdof_sdrmodem_init(rdof_sdrmodem_t *m, const rdof_sdrmodem_io_t *io, uint32_t rx_fs, uint32_t tx_fs, bool can_full);
void rdof_sdrmodem_free(rdof_sdrmodem_t *m);
/* Bytes from the host. */
void rdof_sdrmodem_write(rdof_sdrmodem_t *m, const uint8_t *data, size_t len);
/* A burst the receiver heard on channel. */
void rdof_sdrmodem_heard(rdof_sdrmodem_t *m, int channel, const rdof_sdr_burst_t *b);
/* The next n transmit samples, from sample pos (pos jumps forward if the front end skipped some): I/Q at the
 * offset tx_tune gave, or the frequency (Hz) and envelope. Returns whether any of them is keyed. */
bool rdof_sdrmodem_pull_iq(rdof_sdrmodem_t *m, int64_t pos, rdof_cf_t *iq, int n);
bool rdof_sdrmodem_pull_freq(rdof_sdrmodem_t *m, int64_t pos, float *hz, float *env, int n);
/* A signal mode's transmission (rdof/sigmode.h) in place of DMR, for the bench: the transmitter tuned to hz (tx_tune),
 * then the pulls render src from the next one on. Refused (false, why) while DMR is keyed or queued, in duplex or a
 * split role, or where tx_tune can't reach hz. src NULL ends it (at once) and tunes back to SET_FREQ's frequency. */
bool rdof_sdrmodem_signal(rdof_sdrmodem_t *m, uint32_t hz, const rdof_sig_src_t *src, char *why, size_t cap);
/* The rate for a signal's source (its rate): tx_fs, or a fast stream's divided down to about 48 kS/s. The pulls spread
 * each of a slower source's samples over tx_fs / its rate of the stream's, linearly. */
double rdof_sdrmodem_signal_rate(const rdof_sdrmodem_t *m);
/* 0 none, 1 sending, 2 the source is done and a quarter second more has been pulled (what it keyed went out). */
int rdof_sdrmodem_signal_state(const rdof_sdrmodem_t *m);
/* A signal's transmission whose source renders I/Q only (no frequency): a front end that sends frequency pulls it
 * as I/Q (rdof_fe_caps_t.tx_polar). */
bool rdof_sdrmodem_signal_iq(const rdof_sdrmodem_t *m);
/* Transmitting or about to (anything queued). */
bool rdof_sdrmodem_transmitting(const rdof_sdrmodem_t *m);
/* The receiver's 48 kS/s position p: inside a span this modem keyed (half duplex: what the receiver heard there
 * is its own signal or nothing, so AFC and ppm learning skip it). */
bool rdof_sdrmodem_muted_at(const rdof_sdrmodem_t *m, int64_t p);
/* A burst b heard at 48 kS/s position p, on any channel, against those this modem sent within within48 of there
 * (the front end's delay past what's assumed: the Pluto's 53 ms): the bits it differs from the nearest by, -1 none
 * sent there. On a wideband receiver the transmitter's spurs land on other channels (half duplex, and a full duplex
 * repeater's outbound), where only the content tells: ours is within RDOF_SDRMODEM_SENT_BITS (two others differ by
 * about 100 of the 264). It remembers the last RDOF_SDRMODEM_SENT bursts: a duplex outbound's, one a slot, cover
 * about 1 s, past the transmit buffers and the receiver's delay. */
#define RDOF_SDRMODEM_SENT_BITS 24
int rdof_sdrmodem_sent_bits(const rdof_sdrmodem_t *m, int64_t p, int64_t within48, const uint8_t *b);
/* Split TX half: the AT bits on air now (bit 0 TS1, bit 1 TS2); otherwise rdof_sdrmodem_busy. */
int rdof_sdrmodem_at(const rdof_sdrmodem_t *m);
/* Split RX half: the slot (1, 2) a burst starting at 48 kS/s position pos48 is labeled with, 0 without a valid epoch
 * (counts nothing; for tests). */
int rdof_sdrmodem_label(rdof_sdrmodem_t *m, int64_t pos48);
/* Duplex repeater, keyed: the inbound slot (1, 2) of a handheld's burst by its timing; 0 for any other burst. */
int rdof_sdrmodem_inbound(rdof_sdrmodem_t *m, const rdof_sdr_burst_t *sb);
/* Duplex: the inbound slots busy now (bit 0 TS1, bit 1 TS2), what the CACH's AT bits say. */
int rdof_sdrmodem_busy(const rdof_sdrmodem_t *m);
/* Duplex: the 3 CACH bytes (24 bits, first on air in the top bit of byte 0) before outbound slot k (k even: TS1). */
void rdof_sdrmodem_cach(rdof_sdrmodem_t *m, long k, uint8_t out3[3]);

/* The host frames that make a split TX half (this modem with RDOF_FEATURE_REPEATER, or any SPLIT1 modem) a repeater's
 * downlink with no traffic, keyed: SET_ROLE 1 (lease_s, the key-up limit max_key_s rounded up to 10 s, prebuffer 1),
 * SET_CONFIG duplex on cc, SET_FREQ (RX = TX = hz, rf_level), SET_MODE DMR, DMR_START 01. Idle bursts with the CACH
 * on both slots from the next slot on, until DMR_START 00. Returns the bytes written to out (0: cap too small). */
size_t rdof_sdrmodem_outbound_frames(uint32_t hz, int cc, int rf_level, int lease_s, int max_key_s, uint8_t *out,
                                     size_t cap);
/* UTC slot timing: the mode (slot_timing utc) and the site's offset. */
void rdof_sdrmodem_grid(rdof_sdrmodem_t *m, bool on, int offset_us);
/* The map from 48 kS/s positions to UTC (ok false: none now, the slot clock runs on as it is): position i0 is UTC
 * t0_ns, rate positions a UTC second (48000 * (1 + the crystal's ppm)). */
void rdof_sdrmodem_grid_map(rdof_sdrmodem_t *m, bool ok, int64_t i0, int64_t t0_ns, double rate);
/* The gate: false stops transmitting (unkeyed, DMR data refused) until true again. */
void rdof_sdrmodem_set_transmit(rdof_sdrmodem_t *m, bool transmit);
/* The front end's crystal error the transmitter isn't corrected for (ppm, positive: fast; its LO and its sample
 * clock alike): the modulator asks for f / (1 + ppm), so DMR and a signal mode's carrier both come out on f. */
void rdof_sdrmodem_carrier_ppm(rdof_sdrmodem_t *m, double ppm);
/* A heard burst's UTC slot (inbound: TS1 in even n, aligned timing) from the map; false without one. */
bool rdof_sdrmodem_utc_slot(const rdof_sdrmodem_t *m, int64_t pos48, int64_t *n, int *ts, int64_t *utc_ns);

/* The end-to-end check (loopback.c; needs the receiver and RDOF_FEATURE_TX): a modem client, this modem, the
 * modulator's output mixed into the receiver's samples with another station's bursts and noise, the receiver's
 * bursts back to the modem. Half duplex (full false): our bursts heard back where they were sent and muted, the
 * other station's (in the gaps) up to the host as DMR_DATA2, duplex refused. Full: keyed by DMR_START on another
 * channel, idle bursts between ours, unkeyed. Returns the failures; report says what (or what passed). */
int rdof_sdrmodem_loopback(bool full, char *report, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
