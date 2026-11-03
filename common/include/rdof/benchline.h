// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The bench's line protocol (docs/spec/bench-api.md section 14): the bench daemon's protocol, one JSON request line a
 * connection and JSON lines back, served by the gateway. This is the engine: connections, sessions, claims, the modem
 * queue, the burst fan-out and the capture streams. It owns no socket and no thread: the server (benchnet.h, or a
 * test) hands it each connection's bytes and sends what it gives back; the receiver feeds it bursts; the platform
 * answers its questions through rdof_bl_platform_t. Every call takes the engine's lock, so the server, the receiver
 * and the modem's thread may call it from their own threads.
 *
 * Portable C (POSIX threads for the lock).
 */
#ifndef RDOF_BENCHLINE_H
#define RDOF_BENCHLINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/sigmode.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_BL_PORT 7373
#define RDOF_BL_CONNS 8
#define RDOF_BL_SESSIONS 8
#define RDOF_BL_FREQS 24       /* frequencies the bench lists: receiver channels and TX */
#define RDOF_BL_CLAIMS 8       /* frequencies one session may claim */
#define RDOF_BL_LINE 4096      /* a request line, at first */
#define RDOF_BL_LINE_MAX 196608 /* a request line at most (a run's job carries its AMBE file) */
#define RDOF_BL_BURSTQ 256     /* bursts between the receiver and the connections */
#define RDOF_BL_OUT 16384      /* a connection's output buffer */
#define RDOF_BL_OUT_IQ 262144  /* a capture's */
#define RDOF_BL_IQ_CHUNK 8192  /* samples a capture frame */

/* The engine's small buffers (request lines, a listener's frames): on an ESP32 in PSRAM when there is some, since
 * malloc keeps blocks this small in internal RAM, the scarce one. */
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define RDOF_BL_MALLOC(n) heap_caps_malloc_prefer((n), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT)
#define RDOF_BL_REALLOC(p, n) \
    heap_caps_realloc_prefer((p), (n), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT)
#define RDOF_BL_OUT_TRIG 1048576 /* a trigger's output at most (its whole capture where it fits): 5 s of cs16 */
#else
#define RDOF_BL_OUT_TRIG 4194304
#define RDOF_BL_MALLOC(n) malloc(n)
#define RDOF_BL_REALLOC(p, n) realloc((p), (n))
#endif

enum {
    RDOF_BL_CAP_RX = 1,     /* in the receiver's window */
    RDOF_BL_CAP_DMR = 2,    /* a receiver channel decodes DMR there */
    RDOF_BL_CAP_IQ = 4,     /* in the I/Q captures */
    RDOF_BL_CAP_TX = 8,     /* the bench may transmit there (bench.tx) */
};

/* A frequency the bench lists. Receiver channels come first, in the receiver's order (ch is their index). */
typedef struct {
    double freq_hz;
    int ch;              /* the receiver channel, -1: none (a TX-only frequency) */
    char id[16];         /* the channel's name, "" none */
    unsigned caps;
} rdof_bl_freq_t;

typedef struct {
    bool running;        /* the receiver runs */
    double center_hz, fs, lo_hz, hi_hz, gain_db, dc_guard_hz, ppm;
    double iq_scale_db;  /* the captures' cs8 full scale under the ADC's (the ring's scale, in dB; 0: the same) */
} rdof_bl_window_t;

#define RDOF_BL_VOICE_SYNC 0xF0 /* RDOF_DT_VOICE_SYNC */
#define RDOF_BL_VOICE 0xF1      /* RDOF_DT_VOICE */

/* A burst the receiver heard (the platform's burst, in plain terms). */
typedef struct {
    int ch;              /* receiver channel */
    double t;            /* UTC seconds at the burst's start */
    int64_t pos48;       /* the channel's 48 kS/s sample at its start */
    float cfo_hz, dev_hz, level_dbfs;
    uint8_t dt;          /* data type 0-11, RDOF_BL_VOICE_SYNC (A), RDOF_BL_VOICE (B-F), as rdof_burst_t */
    int8_t n, slot, family, cc, sync_errors;
    uint8_t raw[33];
} rdof_bl_burst_t;

/* A duplex repeater job's setup (bench_repeater). */
typedef struct {
    uint32_t rx_hz, tx_hz;
    int cc, power;
    double hang_s, call_hang_s; /* T_MSInactiv, T_CallHt */
    unsigned slots;             /* bit 1: slot 1, bit 2: slot 2 */
} rdof_bl_rpt_cfg_t;

enum { RDOF_BL_RPT_HEARD = 1, RDOF_BL_RPT_LOST, RDOF_BL_RPT_KEY };

/* An outbound job's setup (bench_tx outbound): a BS downlink keyed for seconds. */
typedef struct {
    uint32_t freq_hz;
    int cc, power;
    double seconds;
} rdof_bl_ob_cfg_t;

/* An outbound job's slot clock, as the platform has it now. Slot k (the modem's count) starts its CACH at TX sample
 * base + k * slot; even k is TS1. Each slot's burst is queued ahead of its air time: next is the slot to queue. */
typedef struct {
    bool keyed;                 /* the key-up's first slot is queued */
    long first;                 /* the key-up's first slot, -1 none yet */
    long next;
    int64_t base;               /* TX samples at rate */
    double slot, rate;          /* samples a slot (1440 at 48 kS/s), TX samples a second */
    int sync_off;               /* from a slot's start to its burst's sync center, samples (CACH and 65.5 symbols) */
    long skips;                 /* the slot clock jumped (it fell behind): last_from to last_to was the last */
    long last_from, last_to;
    bool on_air;                /* the transmitter is sending the key-up: map_pos and map_t hold */
    int64_t map_pos;            /* TX sample map_pos goes out at UTC map_t */
    double map_t;
    double ppm;                 /* the transmitter's crystal error, not in the map (0 unknown) */
} rdof_bl_ob_state_t;

/* What a repeater job's repeater heard or did. */
typedef struct {
    int kind;
    double t;                   /* UTC, 0: when the engine takes it */
    int slot, rssi;
    uint8_t control;            /* heard: the MMDVM control byte */
    uint8_t burst[33];
    bool keyed;                 /* key */
} rdof_bl_rpt_ev_t;

typedef struct {
    /* What the bench is: kind ("rdofradio", "esp32s3-gateway"), name, version. */
    void (*device)(void *ctx, char *kind, size_t kcap, char *name, size_t ncap, char *version, size_t vcap);
    void (*window)(void *ctx, rdof_bl_window_t *w);
    /* The listed frequencies: how many it wrote. */
    int (*freqs)(void *ctx, rdof_bl_freq_t *out, int max);
    /* The wideband history, cs8: samples [sc, sc + n) into out (2n bytes). Returns how many it copied from sc on (0:
     * none yet). *oldest: the oldest sample kept; *next: the next one the receiver will write. NULL: no captures. */
    long (*iq_read)(void *ctx, uint64_t sc, int8_t *out, long n, uint64_t *oldest, uint64_t *next);
    /* How many of the history's samples [sc, sc + n) are filler (optional): zeros the receiver put where the front
     * end lost samples, so its count kept real time (rdof_bl_fills_t). A capture counts them in gap_samples. */
    long (*iq_filled)(void *ctx, uint64_t sc, long n);
    /* A capture is open: keep the history warm (optional). */
    void (*iq_warm)(void *ctx);
    /* UTC now, and of a wideband sample (optional: then the server's clock). */
    double (*utc)(void *ctx);
    double (*sc_utc)(void *ctx, uint64_t sc);
    /* The receiver's counters for status (optional): seconds running, overflows, ADC clipping (%), chunks the DSP
     * skipped, bursts the decoder dropped. */
    void (*rx_stats)(void *ctx, double *seconds, long *overflows, double *clip_pct, long *dsp_skipped, long *dropped);
    /* Whether the bench can transmit at all (its build, its front end, bench.tx): NULL yes, else why not. */
    const char *(*can_tx)(void *ctx);
    void *ctx;
    /* Transmit jobs (NULL: none), called from the engine's thread with its lock held. tx_open: take the transmitter
     * for a job on freq_hz at the job's power (bench_tx --power, 0-255: the platform says in why, a note, how it
     * applied it; why "" none) (setting it up may take a while); false with why when it can't. tx_space: bursts it
     * takes now (0: not yet), -1 when it failed (why). tx_push: a burst (its MMDVM DMR_DATA control byte).
     * tx_unkey: the key-up ends after what was pushed. tx_idle: everything pushed went out and it's unkeyed.
     * tx_close: the job is over (or cancelled): give the transmitter back. */
    bool (*tx_open)(void *ctx, uint32_t freq_hz, int cc, int power, char *why, size_t cap);
    int (*tx_space)(void *ctx, char *why, size_t cap);
    void (*tx_push)(void *ctx, uint8_t control, const uint8_t burst[33]);
    void (*tx_unkey)(void *ctx);
    bool (*tx_idle)(void *ctx);
    void (*tx_close)(void *ctx);
    /* Duplex repeater jobs (bench_repeater; NULL: none), as the transmit jobs: rpt_open takes the transmitter for a
     * repeater on cfg (setting it up may take a while); false with why when it can't. rpt_state: 1 it repeats, 0 not
     * yet, -1 it failed (why). rpt_event: the next thing it heard or did (false: none). rpt_wake: a burst into its
     * receive path as if heard (an idle beacon's BS_Dwn_Act). rpt_close: the job is over: the transmitter back. */
    bool (*rpt_open)(void *ctx, const rdof_bl_rpt_cfg_t *cfg, char *why, size_t cap);
    int (*rpt_state)(void *ctx, char *why, size_t cap);
    bool (*rpt_event)(void *ctx, rdof_bl_rpt_ev_t *ev);
    void (*rpt_wake)(void *ctx, uint8_t control, const uint8_t burst[33]);
    void (*rpt_close)(void *ctx);
    /* Optional, for transmit jobs: NULL when the path sends a burst family (rdof_family_t) as built, else why not (a
     * path that frames the bursts again). Asked before anything keys. NULL hook: every family goes as built. */
    const char *(*tx_why_family)(void *ctx, int family);
    /* Outbound jobs (bench_tx outbound; NULL: none), as the repeater jobs: ob_open takes the SDR modem itself (never
     * through a router) and keys a BS downlink on cfg; false with why. ob_state: 1 keyed, 0 not yet, -1 failed (why),
     * 2 unkeyed after ob_unkey; *s the slot clock. ob_unkey: the key-up ends after the slot being queued. ob_close:
     * the modem back to whoever had it. */
    bool (*ob_open)(void *ctx, const rdof_bl_ob_cfg_t *cfg, char *why, size_t cap);
    int (*ob_state)(void *ctx, rdof_bl_ob_state_t *s, char *why, size_t cap);
    void (*ob_unkey)(void *ctx);
    void (*ob_close)(void *ctx);
    /* Why an outbound can't run here (NULL: it can), in place of can_tx (NULL hook: can_tx's answer): it takes the
     * modem from whatever has it */
    const char *(*ob_can)(void *ctx);
    /* Signal modes (rdof/sigmode.h): the platform's list (NULL-terminated; NULL: none). A mode with a receiver runs
     * on a channel cut from the wideband history (iq_read), one with a transmitter through sig_open. */
    const rdof_sig_mode_t *const *modes;
    /* A signal mode's transmission (NULL: none), as the transmit jobs: sig_open takes the transmitter and keys what
     * src renders on freq_hz at power (as tx_open's; a note in why) for about seconds: 1; 0 with why when not yet (a
     * roamer is transmitting: asked again each tick, for up to 10 s); -1 with why when it can't (a duplex repeater
     * runs, a key-up that long, the source's peak deviation past the front end's). src is the transmitter's thread's
     * until sig_close.
     * sig_state: 1 sending, 2 the source is done and what it sent went out, -1 failed (why). sig_close: unkey and give
     * the transmitter back; src is never called again after it returns. */
    int (*sig_open)(void *ctx, uint32_t freq_hz, int power, double seconds, const rdof_sig_src_t *src, char *why,
                    size_t cap);
    int (*sig_state)(void *ctx, char *why, size_t cap);
    void (*sig_close)(void *ctx);
    /* The rate the transmitter pulls a source at, samples a second (NULL: 48000). */
    double (*sig_rate)(void *ctx);
} rdof_bl_platform_t;

typedef struct rdof_bl rdof_bl_t;

/* A new engine (malloc'd; NULL when there's no memory). */
rdof_bl_t *rdof_bl_new(const rdof_bl_platform_t *p);
void rdof_bl_free(rdof_bl_t *bl);

/* The server's side: a connection came (its slot, or -1 when all are in use: answer `busy` with
 * rdof_bl_refusal and close), its bytes, its end. */
int rdof_bl_open(rdof_bl_t *bl, double now);
const char *rdof_bl_refusal(void);
void rdof_bl_input(rdof_bl_t *bl, int c, const char *data, size_t n, double now);
void rdof_bl_closed(rdof_bl_t *bl, int c, double now);
/* How many bytes c's request takes now (the server reads no more than this; 0: read later). After the request line
 * a tx op's stream (a mode's PCM) follows, taken as the mode has room; any other request's are dropped. */
size_t rdof_bl_input_room(rdof_bl_t *bl, int c);
/* What to send on c now (copied into buf, up to cap): its length. */
size_t rdof_bl_output(rdof_bl_t *bl, int c, uint8_t *buf, size_t cap);
/* Its request is over and everything it had went out: close it. */
bool rdof_bl_finished(rdof_bl_t *bl, int c);
/* Queues, timeouts, the burst fan-out and the capture streams: often (every 10-20 ms). */
void rdof_bl_tick(rdof_bl_t *bl, double now);

/* The receiver's side: a burst heard (from any thread; it's queued). */
void rdof_bl_burst(rdof_bl_t *bl, const rdof_bl_burst_t *b);
/* Whether anyone wants bursts now (the receiver may skip the copy when not). */
bool rdof_bl_wants_bursts(const rdof_bl_t *bl);
/* Whether a capture is open (the receiver keeps the history then). */
bool rdof_bl_wants_iq(const rdof_bl_t *bl);

/* I/Q stream formats (bench-api.md section 6.2's fmt byte). */
enum { RDOF_BL_IQ_CS8 = 1, RDOF_BL_IQ_CU8 = 2, RDOF_BL_IQ_CS16 = 3, RDOF_BL_IQ_CF32 = 4 };

/* n float I/Q samples (full scale 1.0) into fmt at out, the 8-bit ones (cs8, cu8) k times full scale: a front end's
 * low bits, where a weak signal is (an LR2021's channel noise is about 1.4 of cs16's 32767: at k 1 cs8 is all
 * zeros). Returns how many values clipped. */
int rdof_bl_iq_convert(const float *iq, int n, int fmt, float k, uint8_t *out);
/* lrintf's rounding (to nearest, ties to even) for |x| < 2^22, without its call (a library call on the ESP32-S3):
 * adding 1.5 * 2^23 rounds x to a whole number, which the sum's low bits then hold. A union, not memcpy: at -Os
 * memcpy stays a call. */
static inline int rdof_bl_round(float x)
{
    const union {
        float f;
        int32_t i;
    } t = {.f = x + 12582912.0f};
    return (int)(t.i - 0x4B400000);
}
/* fmt's full scale under the ADC's, in dB (20 log10 k for 8-bit, 0 else): a level less it is dBFS. */
double rdof_bl_iq_scale_db(int fmt, float k);

/* Where a history holds filler, for iq_filled: the last RDOF_BL_FILLS runs of it, by sample. A run that follows the
 * last one merges with it; with no room the oldest goes (the captures read near the newest). The caller locks. */
#define RDOF_BL_FILLS 128 /* a 2 s history losing a bit of every 20 ms chunk has about 100 */

typedef struct {
    uint64_t from[RDOF_BL_FILLS];
    uint32_t n[RDOF_BL_FILLS];
    int head, count;
} rdof_bl_fills_t;

void rdof_bl_fills_reset(rdof_bl_fills_t *f);
/* Samples [sc, sc + n) are filler. */
void rdof_bl_fills_note(rdof_bl_fills_t *f, uint64_t sc, long n);
/* How many of [sc, sc + n) are. */
long rdof_bl_fills_count(const rdof_bl_fills_t *f, uint64_t sc, long n);

/* A wideband history the platform may use for iq_read: a ring of cs8 samples. */
typedef struct {
    int8_t *buf;         /* [n][2] */
    long n;
    uint64_t w;          /* samples written */
    float scale;         /* the samples are multiplied by it before they go to int8 (1 from init): a wider ADC's
                          * low bits, where a weak signal is; window's iq_scale_db is 20 log10 of it */
    rdof_bl_fills_t fills; /* where it holds filler (rdof_bl_ring_mark_filler) */
} rdof_bl_ring_t;

bool rdof_bl_ring_init(rdof_bl_ring_t *r, long n);
void rdof_bl_ring_free(rdof_bl_ring_t *r);
/* Float samples (full scale 1.0) in; the count that hit the int8 rails is added to *clipped. */
void rdof_bl_ring_put(rdof_bl_ring_t *r, const float *iq, long n, long *clipped);
/* The last n samples put are filler (zeros where the receiver lost samples). */
void rdof_bl_ring_mark_filler(rdof_bl_ring_t *r, long n);
long rdof_bl_ring_read(const rdof_bl_ring_t *r, uint64_t sc, int8_t *out, long n, uint64_t *oldest, uint64_t *next);
/* As iq_filled: how many of [sc, sc + n) are filler. */
long rdof_bl_ring_filled(const rdof_bl_ring_t *r, uint64_t sc, long n);

#ifdef __cplusplus
}
#endif

#endif
