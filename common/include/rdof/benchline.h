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
#define RDOF_BL_OUT_TRIG 4194304 /* a trigger's at most (its whole capture where it fits) */
#define RDOF_BL_IQ_CHUNK 8192  /* samples a capture frame */

/* The engine's small buffers (request lines, a listener's frames): on an ESP32 in PSRAM when there is some, since
 * malloc keeps blocks this small in internal RAM, the scarce one. */
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define RDOF_BL_MALLOC(n) heap_caps_malloc_prefer((n), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT)
#define RDOF_BL_REALLOC(p, n) \
    heap_caps_realloc_prefer((p), (n), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT)
#else
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

typedef struct {
    /* What the bench is: kind ("rdofradio", "esp32s3-gateway"), name, version. */
    void (*device)(void *ctx, char *kind, size_t kcap, char *name, size_t ncap, char *version, size_t vcap);
    void (*window)(void *ctx, rdof_bl_window_t *w);
    /* The listed frequencies: how many it wrote. */
    int (*freqs)(void *ctx, rdof_bl_freq_t *out, int max);
    /* The wideband history, cs8: samples [sc, sc + n) into out (2n bytes). Returns how many it copied from sc on (0:
     * none yet). *oldest: the oldest sample kept; *next: the next one the receiver will write. NULL: no captures. */
    long (*iq_read)(void *ctx, uint64_t sc, int8_t *out, long n, uint64_t *oldest, uint64_t *next);
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

/* A wideband history the platform may use for iq_read: a ring of cs8 samples. */
typedef struct {
    int8_t *buf;         /* [n][2] */
    long n;
    uint64_t w;          /* samples written */
    float scale;         /* the samples are multiplied by it before they go to int8 (1 from init): a wider ADC's
                          * low bits, where a weak signal is; window's iq_scale_db is 20 log10 of it */
} rdof_bl_ring_t;

bool rdof_bl_ring_init(rdof_bl_ring_t *r, long n);
void rdof_bl_ring_free(rdof_bl_ring_t *r);
/* Float samples (full scale 1.0) in; the count that hit the int8 rails is added to *clipped. */
void rdof_bl_ring_put(rdof_bl_ring_t *r, const float *iq, long n, long *clipped);
long rdof_bl_ring_read(const rdof_bl_ring_t *r, uint64_t sc, int8_t *out, long n, uint64_t *oldest, uint64_t *next);

#ifdef __cplusplus
}
#endif

#endif
