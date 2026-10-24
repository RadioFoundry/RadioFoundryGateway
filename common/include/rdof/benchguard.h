// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The guard on a bench radio's link (docs/spec/bench-api.md section 8): the bytes between a host tool and a radio
 * wired to the bench go through it both ways, and it keeps the radio at its lowest power and keyed only on the
 * frequencies the session's lease declared. A port of the authors' bench daemon's GuardedLink (MIT; PROVENANCE.md) as a
 * state machine: nothing here blocks, the caller feeds bytes and ticks it.
 *
 * RadioFoundry debug link (A5 5A, cmd, seq, u16 length, payload, CRC-16/CCITT-FALSE over cmd..payload; an answer is
 * cmd | 0x80, an error 0xFF): a power step up or an APRS power above the lowest is refused; a PTT is held while the
 * guard checks the radio (its power, stepped down if need be, and the channel's TX frequency) and passed only on a
 * declared frequency; while the tool uses the link the guard checks every RDOF_GUARD_CHECK_S, and a transmission on an
 * undeclared frequency is a violation (its PTT released). The guard's own requests use a sequence number the tool
 * isn't using; their answers never reach the tool.
 *
 * MMDVM (E0, length, cmd, payload; only with RDOF_GUARD_MMDVM): SET_FREQ to an undeclared TX frequency is refused and
 * its power byte is forced to 0; TX data (DMR data, DMR start, CW ID, the other modes' data) is refused until a
 * SET_FREQ through the guard named a declared TX frequency. A refusal is answered with a NAK.
 *
 * Everything else passes unchanged. Portable C, no allocation.
 */
#ifndef RDOF_BENCHGUARD_H
#define RDOF_BENCHGUARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_GUARD_MAX_TX 4
#define RDOF_GUARD_MAX_FRAME 1024 /* a RadioFoundry debug link payload */
#define RDOF_GUARD_CHECK_S 2.0
#define RDOF_GUARD_LINK_IDLE_S 5.0 /* no debug link frame from the tool this long: it speaks something else */
#define RDOF_GUARD_ANSWER_S 1.0
#define RDOF_GUARD_HELD 2048      /* tool bytes held while the guard checks the radio */
#define RDOF_GUARD_TUNE_MAX 6     /* RADIO requests in a tune */

typedef enum { RDOF_GUARD_ODR = 1, RDOF_GUARD_MMDVM = 2 } rdof_guard_proto_t;

typedef struct {
    void (*to_radio)(void *ctx, const uint8_t *b, size_t n);
    void (*to_tool)(void *ctx, const uint8_t *b, size_t n);
    /* refused, tx_violation, power_step, aprs_power_lowered, check_failed; detail is a sentence */
    void (*event)(void *ctx, const char *kind, const char *detail);
    void *ctx;
} rdof_guard_io_t;

typedef struct {
    uint8_t buf[6 + RDOF_GUARD_MAX_FRAME + 2];
    size_t n;
} rdof_guard_split_t;

typedef struct {
    long checks, power_steps, refused, violations, held_dropped;
} rdof_guard_stats_t;

/* A tune (POST /radios/{name}/tune) on a RadioFoundry radio: the VFO on the frequency (simplex), the mode, and for DMR the
 * color code, slot and talk target. */
typedef struct {
    uint32_t freq_hz;
    bool fm;
    int cc;   /* 0-15, -1: keep */
    int slot; /* 1 or 2, 0: keep */
    long tg;  /* a talkgroup, 0: keep; private: a private call to tg */
    bool private_call;
} rdof_guard_tune_t;

typedef enum { RDOF_TUNE_NONE, RDOF_TUNE_RUNNING, RDOF_TUNE_OK, RDOF_TUNE_FAILED } rdof_tune_state_t;

typedef struct {
    rdof_guard_proto_t proto;
    rdof_guard_io_t io;
    uint32_t tx_hz[RDOF_GUARD_MAX_TX]; /* declared TX frequencies; none: receive only */
    int ntx;
    bool lowest;
    rdof_guard_split_t tool, radio;
    /* the tool's debug link request in flight */
    bool outstanding;
    uint8_t out_seq;
    double out_t;
    uint8_t last_seq;
    double last_link, last_check;
    /* the guard's check of the radio */
    int step;
    uint8_t gseq, gcmd;
    double deadline, resume;
    int tries;
    char why[12];
    bool held_ptt; /* a tool PTT waits on the check */
    uint8_t ptt_seq;
    uint8_t ptt_raw[16];
    size_t ptt_n;
    uint8_t held[RDOF_GUARD_HELD]; /* tool bytes that came during the check */
    size_t nheld;
    uint8_t again[RDOF_GUARD_HELD]; /* the held bytes being replayed, through their own splitter */
    rdof_guard_split_t replay;
    /* the radio's view */
    int power, power_count;
    bool transmitting;
    uint32_t view_tx_hz;
    /* MMDVM */
    bool mmdvm_tx_ok;
    uint32_t mmdvm_tx_hz; /* the last SET_FREQ's TX frequency */
    /* a tune: its requests (op and arguments, RADIO's payload), the one in flight, and how it ended */
    uint8_t tune_req[RDOF_GUARD_TUNE_MAX][8];
    uint8_t tune_len[RDOF_GUARD_TUNE_MAX];
    int tune_n, tune_at, tune_busy;
    uint32_t tune_hz;
    rdof_tune_state_t tune;
    char tune_error[96];
    uint32_t view_rx_hz;
    rdof_guard_stats_t stats;
} rdof_guard_t;

void rdof_guard_init(rdof_guard_t *g, rdof_guard_proto_t proto, const rdof_guard_io_t *io, const uint32_t *tx_hz, int ntx);
/* Bytes from the tool, bytes from the radio, and time passing (call it every 20 ms or so); now in seconds. */
void rdof_guard_tool(rdof_guard_t *g, const uint8_t *b, size_t n, double now);
void rdof_guard_radio(rdof_guard_t *g, const uint8_t *b, size_t n, double now);
void rdof_guard_tick(rdof_guard_t *g, double now);
/* Check the radio now (a RadioFoundry link's start): its power down to the lowest. */
void rdof_guard_check(rdof_guard_t *g, double now);
/* The lease's TX frequencies changed (none: it ended). RadioFoundry: the radio is checked at once, so a transmission the new
 * list doesn't cover is a violation; MMDVM: TX data goes on only if the last SET_FREQ is still declared. */
void rdof_guard_set_tx(rdof_guard_t *g, const uint32_t *tx_hz, int ntx, double now);
/* The tool is gone (its socket closed): nothing more transmits. RadioFoundry: a PTT release; MMDVM: the modem to idle.
 * Bytes held for the tool are dropped. */
void rdof_guard_end(rdof_guard_t *g, double now);
bool rdof_guard_busy(const rdof_guard_t *g);
/* RadioFoundry: tune the radio (rdof_guard_tune_t), then check it as a link's start does (power to the lowest) and read back
 * where it is. Refused (false, why in tune_error) while the guard is busy. Tick it until rdof_guard_tune_state() isn't
 * RDOF_TUNE_RUNNING; RDOF_TUNE_FAILED has its reason in tune_error. */
bool rdof_guard_tune(rdof_guard_t *g, const rdof_guard_tune_t *t, double now);
rdof_tune_state_t rdof_guard_tune_state(const rdof_guard_t *g);

uint16_t rdof_guard_crc16(const uint8_t *b, size_t n);
/* A RadioFoundry debug link frame; returns its length (8 + n). */
size_t rdof_guard_frame(uint8_t cmd, uint8_t seq, const uint8_t *payload, size_t n, uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif
