// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The bench's line protocol engine (rdof/benchline.h, docs/spec/bench-api.md section 14).
 */
#if !defined(ESP_PLATFORM) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE /* glibc: gmtime_r with -std=c11 */
#endif
#include "rdof/benchline.h"

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rdof/benchjob.h"
#include "rdof/jsonp.h"
#include "rdof/layer2.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define NODES 384
#define WAIT_MAX_S 900.0       /* a wait with no wait_max */
#define UNTIL_END_S 0.3        /* bursts --until-end: quiet this long after a burst ends the watch */
#define SLOT_MS 30.0
#define SYNC_CENTER_48 655     /* a burst's sync center past its start, 48 kS/s samples (symbol 65.5) */

enum { C_FREE = 0, C_LINE, C_ACTIVE, C_DONE };
enum { OP_NONE = 0, OP_SESSION, OP_CLAIM, OP_HOLD, OP_RUN, OP_BURSTS, OP_LISTEN, OP_CAPTURE, OP_TRIGGER, OP_TX, OP_RX };
enum { W_NONE = 0, W_MODEM, W_FREQ };
/* a run's job */
enum { J_WAIT = 0, J_OPEN, J_SEND, J_DRAIN, J_GAP, J_LISTEN, J_RPT_SETUP, J_RPT_RUN, J_OB_SETUP, J_OB_RUN, J_OB_DRAIN };

typedef struct {
    rdof_bj_job_t job;
    uint8_t ambe[RDOF_BL_LINE_MAX / 2];
    rdof_bj_burst_t *b;       /* the key-up's bursts */
    int nb, bi, maxb, part, nparts;
    int state;
    double until, started, max_s;
    bool tx_open, listening;
    rdof_bj_listener_t lis;
    /* bench_repeater */
    bool rpt_open, keyed;
    rdof_bj_rpt_t rpt;
    int beacons;
    double ends;
    /* bench_tx outbound */
    bool ob_open;
    rdof_bl_ob_state_t ob;
    long ob_rep, ob_last, ob_skips, ob_n[3]; /* the next slot to report; the slot to unkey after; skips told */
    int64_t ob_pos;           /* the map, latched once on the air: TX sample ob_pos at UTC ob_t */
    double ob_t;
    long id;
    char note[200], done[200];
    char esc[7000];           /* an out line, as a JSON string */
} job_t;

/* a trigger: a channel cut from the wideband history (mixed down, filtered, decimated to about 48 kS/s), a level
 * trigger on it, then the pre-roll and what follows, cut again from the history */
#define TRIG_HIST 300          /* the floor's histogram: 0.5 dB bins from -150 dB */

typedef struct {
    int d, ntaps;             /* decimation, filter length (odd) */
    float *taps;
    float *dl;                /* the delay line, [2 * ntaps][2]: each sample twice, so a window is contiguous */
    float *y;                 /* a chunk's outputs, [n][2] */
    int di;
    double step;              /* the mixer's radians a sample */
    uint64_t sc, next_out;    /* the next input to take; the input that completes the next output */
    double rate, bw, thr, min_ms, pre_s, post_s, max_s, floor_s, timeout, started, last_tick;
    bool cs16, until_end, fired;
    int have_floor;           /* 0 learning, 1 learned, 2 given */
    double floor_db, level_db;
    int hist[TRIG_HIST];
    long floor_blocks;
    int bsize, bn;            /* outputs in a 1 ms block, so far */
    double bp;
    uint64_t block_sc;
    int run;                  /* blocks over the threshold in a row */
    uint64_t run_sc;
    double run_p;
    uint64_t trig_sc, out_sc0, end_sc;
    long sent, gaps, clipped, inputs;
    long filled;              /* wideband filler in what it sent, [out_sc0, end_sc) */
} trig_t;

typedef struct {
    int state, op;
    char *line;
    size_t ln, lcap;
    bool overlong;
    uint8_t *out;
    size_t ocap, oh, ol;      /* the output: ol bytes from oh */
    double opened;
    int sess;                 /* the session this request is for, -1 none; for OP_SESSION the one it holds */
    /* a wait (claim, hold, run) */
    int wait;
    long seq;
    double wait_since, wait_max, expect_s;
    double freqs[RDOF_BL_CLAIMS];
    int nfreqs;
    bool queued_told;
    /* OP_SESSION */
    double ends;              /* max_s runs out, 0: never */
    /* bursts, listen */
    int ch;
    double until;             /* 0: no limit */
    long max, sent, dropped;
    bool until_end, idle;
    int cc;                   /* -1 any */
    double first_t, prev_t, last_t, last_mono;
    rdof_bj_emb_t emb;         /* dmr-bursts: the embedded LC so far */
    double cfo_sum, cfo_min, cfo_max, lvl_sum, lvl_max;
    int kinds[18];
    int syncs[9];
    /* capture */
    uint64_t sc, sc0, sc_end;
    uint32_t iseq;
    long gaps, clipped, sent_iq; /* gaps: samples skipped (overwritten before they went) or filler */
    bool gapped;              /* samples were skipped since the last frame: the next one says so (flag 1) */
    double freq_hz, pre_s, gain;
    double fs;                /* bursts: the wideband rate, for a record's sc */
    /* run */
    job_t *job;
    trig_t *trig;
    struct sigtx *stx;        /* tx */
    struct sigrx *srx;        /* rx */
} conn_t;

typedef struct {
    bool used;
    char id[12];
    char label[48];
    int conn;                 /* the session request's connection */
    double since;
    double expect_s;
    double claims[RDOF_BL_CLAIMS];
    int nclaims;
    int hold;                 /* the modem hold's id, 0 none */
    double hold_since, hold_expect;
} session_t;

struct rdof_bl {
    rdof_bl_platform_t p;
    pthread_mutex_t mu;
    conn_t c[RDOF_BL_CONNS];
    session_t s[RDOF_BL_SESSIONS];
    unsigned next_id;
    long next_seq;
    int holder;               /* the modem: a session's hold (its index), -1 none */
    rdof_bl_freq_t f[RDOF_BL_FREQS];
    int nf;
    /* the burst queue */
    rdof_bl_burst_t q[RDOF_BL_BURSTQ];
    int qh, qn;
    long qdrops;
    volatile int n_bursts, n_iq;
    /* the request parser */
    int tx_conn;              /* the run transmitting now, -1 none */
    rdof_jnode_t nodes[NODES];
    char *strbuf;
    rdof_jdoc_t doc;
    int8_t iq[RDOF_BL_IQ_CHUNK * 2];
    char tmp[8192];
    char bdec[1200], bline[2048]; /* a burst record (under the lock: off the server thread's stack) */
};

static void lock(rdof_bl_t *bl) { pthread_mutex_lock(&bl->mu); }
static void unlock(rdof_bl_t *bl) { pthread_mutex_unlock(&bl->mu); }

/* ── output ───────────────────────────────────────────────────────────────── */

static size_t room(const conn_t *c) { return c->ocap - c->ol; }

static bool put_bytes(conn_t *c, const void *b, size_t n)
{
    if (n > room(c))
        return false;
    if (c->oh + c->ol + n > c->ocap) { /* to the front */
        memmove(c->out, c->out + c->oh, c->ol);
        c->oh = 0;
    }
    memcpy(c->out + c->oh + c->ol, b, n);
    c->ol += n;
    return true;
}

/* A line (the newline added); false when it didn't fit. */
static bool emit(rdof_bl_t *bl, conn_t *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(bl->tmp, sizeof bl->tmp - 1, fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)sizeof bl->tmp - 1)
        return false;
    bl->tmp[n++] = '\n';
    return put_bytes(c, bl->tmp, (size_t)n);
}

/* A JSON string's text (quotes added) into out. */
static void jstr(const char *s, char *out, size_t cap)
{
    size_t k = 0;
    if (cap < 3) {
        if (cap)
            out[0] = 0;
        return;
    }
    out[k++] = '"';
    for (; *s && k + 7 < cap; s++) {
        const unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\')
            out[k++] = '\\', out[k++] = (char)ch;
        else if (ch < 0x20)
            k += (size_t)snprintf(out + k, cap - k, "\\u%04x", ch);
        else
            out[k++] = (char)ch;
    }
    out[k++] = '"';
    out[k] = 0;
}

static void fail(rdof_bl_t *bl, conn_t *c, const char *code, const char *fmt, ...)
{
    char msg[256], q[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    jstr(msg, q, sizeof q);
    emit(bl, c, "{\"error\":%s,\"code\":\"%s\"}", q, code);
    c->state = C_DONE;
}

/* ── frequencies ──────────────────────────────────────────────────────────── */

static void refresh_freqs(rdof_bl_t *bl)
{
    bl->nf = bl->p.freqs ? bl->p.freqs(bl->p.ctx, bl->f, RDOF_BL_FREQS) : 0;
    if (bl->nf < 0)
        bl->nf = 0;
}

static int freq_index(const rdof_bl_t *bl, double mhz)
{
    for (int i = 0; i < bl->nf; i++)
        if (fabs(bl->f[i].freq_hz - mhz * 1e6) < 100.0)
            return i;
    return -1;
}

static unsigned cap_bit(const char *s)
{
    if (!strcmp(s, "rx"))
        return RDOF_BL_CAP_RX;
    if (!strcmp(s, "dmr"))
        return RDOF_BL_CAP_DMR;
    if (!strcmp(s, "iq"))
        return RDOF_BL_CAP_IQ;
    if (!strcmp(s, "tx") || !strcmp(s, "sdr_tx") || !strcmp(s, "mmdvm_tx"))
        return RDOF_BL_CAP_TX;
    return 0;
}

/* The caps as the daemon names them, into out. */
static void caps_json(unsigned caps, char *out, size_t cap)
{
    size_t k = (size_t)snprintf(out, cap, "[");
    static const char *const names[] = {"rx", "dmr", "iq", "mmdvm_tx"};
    bool first = true;
    for (int i = 0; i < 4 && k < cap; i++)
        if (caps & (1u << i)) {
            k += (size_t)snprintf(out + k, cap - k, "%s\"%s\"", first ? "" : ",", names[i]);
            first = false;
            if (i == 3 && k < cap)
                k += (size_t)snprintf(out + k, cap - k, ",\"sdr_tx\"");
        }
    if (k < cap)
        snprintf(out + k, cap - k, "]");
}

/* The session that claimed freq (MHz), -1 none. */
static int claimed_by(const rdof_bl_t *bl, double mhz)
{
    for (int s = 0; s < RDOF_BL_SESSIONS; s++)
        if (bl->s[s].used)
            for (int k = 0; k < bl->s[s].nclaims; k++)
                if (fabs(bl->s[s].claims[k] - mhz) < 1e-4)
                    return s;
    return -1;
}

/* ── sessions ─────────────────────────────────────────────────────────────── */

static int session_find(const rdof_bl_t *bl, const char *id)
{
    if (!id || !id[0])
        return -1;
    for (int s = 0; s < RDOF_BL_SESSIONS; s++)
        if (bl->s[s].used && !strcmp(bl->s[s].id, id))
            return s;
    return -1;
}

static void session_end(rdof_bl_t *bl, int s, const char *why)
{
    session_t *ss = &bl->s[s];
    if (!ss->used)
        return;
    if (bl->holder == s)
        bl->holder = -1;
    for (int i = 0; i < RDOF_BL_CONNS; i++) { /* its waits and streams go with it */
        conn_t *c = &bl->c[i];
        if (c->state == C_ACTIVE && c->sess == s && c->op != OP_SESSION)
            fail(bl, c, "refused", "the session %s ended (%s)", ss->id, why);
    }
    const int sc = ss->conn;
    memset(ss, 0, sizeof *ss);
    if (sc >= 0 && bl->c[sc].state == C_ACTIVE && bl->c[sc].op == OP_SESSION) {
        emit(bl, &bl->c[sc], "{\"ended\":\"%s\"}", why);
        bl->c[sc].state = C_DONE;
    }
}

/* ── the engine ───────────────────────────────────────────────────────────── */

rdof_bl_t *rdof_bl_new(const rdof_bl_platform_t *p)
{
    rdof_bl_t *bl = calloc(1, sizeof *bl);
    if (!bl)
        return NULL;
    bl->p = *p;
    pthread_mutex_init(&bl->mu, NULL);
    bl->holder = -1;
    bl->tx_conn = -1;
    bl->next_id = 1;
    bl->strbuf = malloc(RDOF_BL_LINE_MAX + 64);
    if (!bl->strbuf) {
        pthread_mutex_destroy(&bl->mu);
        free(bl);
        return NULL;
    }
    for (int s = 0; s < RDOF_BL_SESSIONS; s++)
        bl->s[s].conn = -1;
    return bl;
}

static void trig_free(trig_t *t)
{
    if (t) {
        free(t->taps);
        free(t->dl);
        free(t->y);
        free(t);
    }
}

void rdof_bl_free(rdof_bl_t *bl)
{
    if (!bl)
        return;
    for (int i = 0; i < RDOF_BL_CONNS; i++) {
        free(bl->c[i].out);
        free(bl->c[i].line);
        trig_free(bl->c[i].trig);
    }
    free(bl->strbuf);
    pthread_mutex_destroy(&bl->mu);
    free(bl);
}

const char *rdof_bl_refusal(void)
{
    return "{\"error\":\"the bench has all its connections in use\",\"code\":\"busy\"}\n";
}

int rdof_bl_open(rdof_bl_t *bl, double now)
{
    lock(bl);
    int got = -1;
    for (int i = 0; i < RDOF_BL_CONNS && got < 0; i++) {
        conn_t *c = &bl->c[i];
        if (c->state != C_FREE)
            continue;
        uint8_t *out = c->out;
        size_t ocap = c->ocap;
        char *line = c->line;
        size_t lcap = c->lcap;
        memset(c, 0, sizeof *c);
        c->out = out, c->ocap = ocap;
        c->line = line, c->lcap = lcap;
        if (!c->out || c->ocap != RDOF_BL_OUT) {
            free(c->out);
            c->out = malloc(RDOF_BL_OUT);
            c->ocap = c->out ? RDOF_BL_OUT : 0;
        }
        if (!c->out)
            break;
        c->state = C_LINE;
        c->opened = now;
        c->sess = -1;
        c->ch = -1;
        got = i;
    }
    unlock(bl);
    return got;
}

static void request(rdof_bl_t *bl, int ci, double now);
static void job_end(rdof_bl_t *bl, conn_t *c);
static void stx_input(rdof_bl_t *bl, conn_t *c, const uint8_t *b, size_t n);
static size_t stx_room(const conn_t *c);
static void sig_end(rdof_bl_t *bl, conn_t *c);
static double trig_utc(const rdof_bl_t *bl, uint64_t sc, double now, uint64_t next, double fs);

void rdof_bl_input(rdof_bl_t *bl, int ci, const char *data, size_t n, double now)
{
    if (ci < 0 || ci >= RDOF_BL_CONNS)
        return;
    lock(bl);
    conn_t *c = &bl->c[ci];
    size_t i = 0;
    for (; i < n && c->state == C_LINE; i++) {
        if (data[i] == '\n') {
            c->line[c->ln] = 0;
            if (c->overlong)
                fail(bl, c, "bad_request", "the request is longer than %d bytes", RDOF_BL_LINE_MAX - 1);
            else
                request(bl, ci, now);
        } else if (c->ln + 1 < c->lcap) {
            c->line[c->ln++] = data[i];
        } else if (c->lcap < RDOF_BL_LINE_MAX) {
            const size_t nc = c->lcap ? (c->lcap * 4 > RDOF_BL_LINE_MAX ? RDOF_BL_LINE_MAX : c->lcap * 4) : RDOF_BL_LINE;
            char *p = RDOF_BL_REALLOC(c->line, nc);
            if (!p) {
                c->overlong = true;
                continue;
            }
            c->line = p, c->lcap = nc;
            c->line[c->ln++] = data[i];
        } else {
            c->overlong = true;
        }
    }
    /* After the request only a tx op's stream counts (the rest is dropped); closing is what counts. */
    if (i < n && c->stx)
        stx_input(bl, c, (const uint8_t *)data + i, n - i);
    unlock(bl);
}

size_t rdof_bl_input_room(rdof_bl_t *bl, int ci)
{
    if (ci < 0 || ci >= RDOF_BL_CONNS)
        return 0;
    lock(bl);
    const size_t n = stx_room(&bl->c[ci]);
    unlock(bl);
    return n;
}

void rdof_bl_closed(rdof_bl_t *bl, int ci, double now)
{
    (void)now;
    if (ci < 0 || ci >= RDOF_BL_CONNS)
        return;
    lock(bl);
    conn_t *c = &bl->c[ci];
    if (c->op == OP_SESSION && c->sess >= 0 && bl->s[c->sess].conn == ci) {
        bl->s[c->sess].conn = -1;
        session_end(bl, c->sess, "closed");
    }
    if (c->op == OP_BURSTS || c->op == OP_LISTEN)
        bl->n_bursts--;
    if (c->op == OP_CAPTURE || c->op == OP_TRIGGER)
        bl->n_iq--;
    if (c->job)
        job_end(bl, c); /* a transmission unkeys */
    if (c->op == OP_RX)
        bl->n_iq--;
    sig_end(bl, c);
    trig_free(c->trig);
    c->trig = NULL;
    c->state = C_FREE;
    c->op = OP_NONE;
    c->ol = c->oh = 0;
    if (c->ocap > RDOF_BL_OUT) { /* a capture's big buffer goes back */
        free(c->out);
        c->out = NULL, c->ocap = 0;
    }
    unlock(bl);
}

size_t rdof_bl_output(rdof_bl_t *bl, int ci, uint8_t *buf, size_t cap)
{
    if (ci < 0 || ci >= RDOF_BL_CONNS)
        return 0;
    lock(bl);
    conn_t *c = &bl->c[ci];
    size_t n = c->ol < cap ? c->ol : cap;
    memcpy(buf, c->out + c->oh, n);
    c->oh += n, c->ol -= n;
    if (!c->ol)
        c->oh = 0;
    unlock(bl);
    return n;
}

bool rdof_bl_finished(rdof_bl_t *bl, int ci)
{
    if (ci < 0 || ci >= RDOF_BL_CONNS)
        return true;
    lock(bl);
    const bool done = bl->c[ci].state == C_DONE && bl->c[ci].ol == 0;
    unlock(bl);
    return done;
}

bool rdof_bl_wants_bursts(const rdof_bl_t *bl) { return bl && bl->n_bursts > 0; }
bool rdof_bl_wants_iq(const rdof_bl_t *bl) { return bl && bl->n_iq > 0; }

void rdof_bl_burst(rdof_bl_t *bl, const rdof_bl_burst_t *b)
{
    if (!bl || bl->n_bursts <= 0)
        return;
    lock(bl);
    if (bl->qn == RDOF_BL_BURSTQ) {
        bl->qdrops++;
    } else {
        bl->q[(bl->qh + bl->qn) % RDOF_BL_BURSTQ] = *b;
        bl->qn++;
    }
    unlock(bl);
}

/* ── requests ─────────────────────────────────────────────────────────────── */

static int jget(rdof_bl_t *bl, int root, const char *key) { return rdof_jget(&bl->doc, root, key); }

static double jnum(rdof_bl_t *bl, int root, const char *key, double dflt)
{
    const rdof_jnode_t *n = rdof_jnode(&bl->doc, jget(bl, root, key));
    return n && n->type == RDOF_JNUM ? n->num : dflt;
}

static bool jbool(rdof_bl_t *bl, int root, const char *key)
{
    const rdof_jnode_t *n = rdof_jnode(&bl->doc, jget(bl, root, key));
    return n && ((n->type == RDOF_JBOOL && n->b) || (n->type == RDOF_JNUM && n->num != 0));
}

static const char *jstrv(rdof_bl_t *bl, int root, const char *key)
{
    const rdof_jnode_t *n = rdof_jnode(&bl->doc, jget(bl, root, key));
    return n && n->type == RDOF_JSTR ? n->str : NULL;
}

/* The request's session ("session" may be absent: -1; an unknown one fails the request). */
static bool req_session(rdof_bl_t *bl, conn_t *c, int root, bool required)
{
    const char *id = jstrv(bl, root, "session");
    c->sess = session_find(bl, id);
    if (c->sess < 0 && (required || (id && id[0]))) {
        if (id && id[0])
            fail(bl, c, "bad_request", "no session %s here", id);
        else
            fail(bl, c, "bad_request", "this needs a session");
        return false;
    }
    return true;
}

/* The platform's window, with what it doesn't know yet (NAN) as 0: JSON has no NaN. */
static void get_window(rdof_bl_t *bl, rdof_bl_window_t *w)
{
    memset(w, 0, sizeof *w);
    if (bl->p.window)
        bl->p.window(bl->p.ctx, w);
    double *v[] = {&w->center_hz, &w->fs, &w->lo_hz, &w->hi_hz, &w->gain_db, &w->dc_guard_hz, &w->ppm, &w->iq_scale_db};
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++)
        if (!isfinite(*v[i]))
            *v[i] = 0;
}

static void put_window(rdof_bl_t *bl, char *out, size_t cap)
{
    rdof_bl_window_t w;
    get_window(bl, &w);
    if (!w.running || w.fs <= 0)
        snprintf(out, cap, "{\"enabled\":false}");
    else
        snprintf(out, cap,
                 "{\"enabled\":true,\"lo\":%.6f,\"hi\":%.6f,\"center\":%.6f,\"fs\":%.1f,\"gain\":%.1f,"
                 "\"dc_guard_khz\":%.1f,\"ppm\":%.3f}",
                 w.lo_hz / 1e6, w.hi_hz / 1e6, w.center_hz / 1e6, w.fs, w.gain_db, w.dc_guard_hz / 1e3, w.ppm);
}

static double expect_left(const rdof_bl_t *bl, double now)
{
    if (bl->holder < 0)
        return 0;
    const session_t *s = &bl->s[bl->holder];
    const double left = s->hold_expect > 0 ? s->hold_expect - (now - s->hold_since) : 30.0;
    return left > 0 ? left : 0;
}

static void op_info(rdof_bl_t *bl, conn_t *c, double now)
{
    char win[320], dev[200], kind[32] = "gateway", name[48] = "", ver[32] = "", qn[64];
    if (bl->p.device)
        bl->p.device(bl->p.ctx, kind, sizeof kind, name, sizeof name, ver, sizeof ver);
    jstr(name, qn, sizeof qn);
    snprintf(dev, sizeof dev, "{\"kind\":\"%s\",\"name\":%s,\"version\":\"%s\"}", kind, qn, ver);
    put_window(bl, win, sizeof win);
    rdof_bl_window_t w;
    get_window(bl, &w);
    /* built piece by piece: the channel list can be long */
    char line[RDOF_BL_OUT / 2];
    size_t k = (size_t)snprintf(line, sizeof line, "{\"window\":%s,\"sdr\":\"%s\",\"device\":%s,\"channels\":[", win,
                                w.running ? "shared" : "stopped", dev);
    for (int i = 0; i < bl->nf && k < sizeof line; i++) {
        const rdof_bl_freq_t *f = &bl->f[i];
        char caps[96], held[96] = "null", qid[40];
        caps_json(f->caps, caps, sizeof caps);
        const int s = claimed_by(bl, f->freq_hz / 1e6);
        if (s >= 0) {
            char ql[64];
            jstr(bl->s[s].label, ql, sizeof ql);
            snprintf(held, sizeof held, "{\"session\":\"%s\",\"label\":%s}", bl->s[s].id, ql);
        }
        jstr(f->id, qid, sizeof qid);
        char ch[12] = "null";
        if (f->ch >= 0)
            snprintf(ch, sizeof ch, "%d", f->ch);
        k += (size_t)snprintf(line + k, sizeof line - k,
                              "%s{\"freq\":%.6f,\"ch\":%s,\"id\":%s,\"in_window\":%s,\"listed\":true,\"caps\":%s,"
                              "\"claimed_by\":%s}",
                              i ? "," : "", f->freq_hz / 1e6, ch, qid, (f->caps & RDOF_BL_CAP_RX) ? "true" : "false",
                              caps, held);
    }
    const char *notx = bl->p.can_tx ? bl->p.can_tx(bl->p.ctx) : "no transmitter";
    char held[160] = "[]", wq[16];
    int waiting = 0;
    for (int i = 0; i < RDOF_BL_CONNS; i++)
        if (bl->c[i].state == C_ACTIVE && bl->c[i].wait == W_MODEM)
            waiting++;
    if (bl->holder >= 0) {
        char ql[64];
        jstr(bl->s[bl->holder].label, ql, sizeof ql);
        snprintf(held, sizeof held, "[{\"session\":\"%s\",\"label\":%s}]", bl->s[bl->holder].id, ql);
    }
    snprintf(wq, sizeof wq, "%d", waiting);
    const double eta = expect_left(bl, now);
    char why[160] = "null";
    if (notx)
        jstr(notx, why, sizeof why);
    char stx[160] = "[]", srx[160] = "[]";
    if (bl->p.sig_open && !rdof_sig_names_json(bl->p.modes, true, stx, sizeof stx))
        strcpy(stx, "[]");
    if (bl->p.iq_read && !rdof_sig_names_json(bl->p.modes, false, srx, sizeof srx))
        strcpy(srx, "[]");
    if (k < sizeof line)
        k += (size_t)snprintf(line + k, sizeof line - k,
                              "],\"stations\":[{\"name\":\"modem\",\"kind\":\"sdrmodem\",\"model\":\"the gateway's SDR "
                              "modem\",\"port\":\"\",\"present\":%s,\"tx\":%s,\"why_not\":%s,\"modes\":[\"dmr\"],"
                              "\"bands\":[],\"held_by\":%s,\"waiting\":%s}],"
                              "\"modem\":{\"held_by\":%s,\"waiting\":%s,\"next_job_eta\":{\"eta_s\":%.1f,"
                              "\"worst_s\":%.1f}},\"signal_modes\":{\"tx\":%s,\"rx\":%s},\"default_caps\":[]}",
                              notx ? "false" : "true", notx ? "false" : "true", why, held, wq, held, wq, eta, eta, stx,
                              srx);
    if (k >= sizeof line) {
        fail(bl, c, "internal", "info doesn't fit");
        return;
    }
    emit(bl, c, "%s", line);
    c->state = C_DONE;
}

static void op_status(rdof_bl_t *bl, conn_t *c, double now)
{
    double secs = 0, clip = 0;
    long ovf = 0, skip = 0, drop = 0;
    if (bl->p.rx_stats)
        bl->p.rx_stats(bl->p.ctx, &secs, &ovf, &clip, &skip, &drop);
    if (!isfinite(secs))
        secs = 0;
    if (!isfinite(clip))
        clip = 0;
    int captures = 0;
    for (int i = 0; i < RDOF_BL_CONNS; i++)
        if (bl->c[i].state == C_ACTIVE && bl->c[i].op == OP_CAPTURE)
            captures++;
    rdof_bl_window_t w;
    get_window(bl, &w);
    char line[RDOF_BL_OUT / 2];
    size_t k = (size_t)snprintf(line, sizeof line,
                                "{\"sdr\":\"%s\",\"receiver\":{\"seconds\":%.1f,\"overflows\":%ld,\"clip_pct\":%.2f,"
                                "\"dsp_skipped\":%ld,\"decode_dropped\":%ld,\"captures\":%d,\"queue_drops\":%ld},"
                                "\"held\":[",
                                w.running ? "shared" : "stopped", secs, ovf, clip, skip, drop + bl->qdrops, captures,
                                bl->qdrops);
    bool first = true;
    if (bl->holder >= 0) {
        const session_t *s = &bl->s[bl->holder];
        char ql[64];
        jstr(s->label, ql, sizeof ql);
        k += (size_t)snprintf(line + k, sizeof line - k,
                              "{\"id\":%d,\"kind\":\"modem\",\"session\":\"%s\",\"label\":%s,\"since_s\":%.1f,"
                              "\"remaining_s\":%.1f}",
                              s->hold, s->id, ql, now - s->hold_since, expect_left(bl, now));
        first = false;
    }
    for (int s = 0; s < RDOF_BL_SESSIONS && k < sizeof line; s++)
        for (int f = 0; bl->s[s].used && f < bl->s[s].nclaims && k < sizeof line; f++) {
            char ql[64];
            jstr(bl->s[s].label, ql, sizeof ql);
            k += (size_t)snprintf(line + k, sizeof line - k,
                                  "%s{\"id\":0,\"kind\":\"claim\",\"session\":\"%s\",\"label\":%s,\"freq\":%.6f,"
                                  "\"since_s\":%.1f}",
                                  first ? "" : ",", bl->s[s].id, ql, bl->s[s].claims[f], now - bl->s[s].since);
            first = false;
        }
    if (k < sizeof line)
        k += (size_t)snprintf(line + k, sizeof line - k, "],\"waiting\":[");
    first = true;
    int pos = 0;
    for (int i = 0; i < RDOF_BL_CONNS && k < sizeof line; i++) {
        const conn_t *w2 = &bl->c[i];
        if (w2->state != C_ACTIVE || w2->wait == W_NONE)
            continue;
        const session_t *s = w2->sess >= 0 ? &bl->s[w2->sess] : NULL;
        char ql[64];
        jstr(s ? s->label : "", ql, sizeof ql);
        k += (size_t)snprintf(line + k, sizeof line - k,
                              "%s{\"id\":%ld,\"kind\":\"%s\",\"session\":\"%s\",\"label\":%s,\"position\":%d,"
                              "\"since_s\":%.1f}",
                              first ? "" : ",", w2->seq, w2->wait == W_MODEM ? "modem" : "claim", s ? s->id : "", ql,
                              ++pos, now - w2->wait_since);
        first = false;
    }
    if (k < sizeof line)
        k += (size_t)snprintf(line + k, sizeof line - k, "],\"sessions\":[");
    first = true;
    for (int s = 0; s < RDOF_BL_SESSIONS && k < sizeof line; s++) {
        if (!bl->s[s].used)
            continue;
        char ql[64], claims[160] = "";
        size_t m = 0;
        for (int f = 0; f < bl->s[s].nclaims && m < sizeof claims; f++)
            m += (size_t)snprintf(claims + m, sizeof claims - m, "%s\"%.4f\"", f ? "," : "", bl->s[s].claims[f]);
        jstr(bl->s[s].label, ql, sizeof ql);
        k += (size_t)snprintf(line + k, sizeof line - k, "%s{\"session\":\"%s\",\"label\":%s,\"since_s\":%.1f,"
                                                         "\"claims\":[%s]}",
                              first ? "" : ",", bl->s[s].id, ql, now - bl->s[s].since, claims);
        first = false;
    }
    if (k < sizeof line)
        k += (size_t)snprintf(line + k, sizeof line - k, "]}");
    if (k >= sizeof line) {
        fail(bl, c, "internal", "status doesn't fit");
        return;
    }
    emit(bl, c, "%s", line);
    c->state = C_DONE;
}

static void op_session(rdof_bl_t *bl, conn_t *c, int ci, int root, double now)
{
    int s = 0;
    while (s < RDOF_BL_SESSIONS && bl->s[s].used)
        s++;
    if (s == RDOF_BL_SESSIONS) {
        fail(bl, c, "busy", "the bench has %d sessions open", RDOF_BL_SESSIONS);
        return;
    }
    session_t *ss = &bl->s[s];
    memset(ss, 0, sizeof *ss);
    ss->used = true;
    snprintf(ss->id, sizeof ss->id, "s%u", bl->next_id++);
    const char *label = jstrv(bl, root, "label");
    snprintf(ss->label, sizeof ss->label, "%s", label ? label : "");
    ss->conn = ci;
    ss->since = now;
    ss->expect_s = jnum(bl, root, "expect_s", 0);
    c->op = OP_SESSION;
    c->sess = s;
    const double max_s = jnum(bl, root, "max_s", 0);
    c->ends = max_s > 0 ? now + max_s : 0;
    char ql[64];
    jstr(ss->label, ql, sizeof ql);
    emit(bl, c, "{\"session\":\"%s\",\"label\":%s}", ss->id, ql);
    c->state = C_ACTIVE;
}

/* The frequencies of a claim or run: into c->freqs; false (failed) when one isn't listed or lacks a cap. */
static bool take_freqs(rdof_bl_t *bl, conn_t *c, int arr, unsigned need)
{
    const rdof_jnode_t *a = rdof_jnode(&bl->doc, arr);
    c->nfreqs = 0;
    if (!a || a->type != RDOF_JARR || !a->count) {
        fail(bl, c, "bad_request", "no frequencies");
        return false;
    }
    for (int i = 0; i < a->count; i++) {
        const rdof_jnode_t *f = rdof_jnode(&bl->doc, rdof_jat(&bl->doc, arr, i));
        if (!f || f->type != RDOF_JNUM) {
            fail(bl, c, "bad_request", "a frequency isn't a number");
            return false;
        }
        const int k = freq_index(bl, f->num);
        if (k < 0) {
            fail(bl, c, "refused", "%.4f MHz isn't one of this bench's frequencies", f->num);
            return false;
        }
        if ((bl->f[k].caps & need) != need) {
            fail(bl, c, "refused", "%.4f MHz lacks a cap asked for", f->num);
            return false;
        }
        if (c->nfreqs == RDOF_BL_CLAIMS) {
            fail(bl, c, "bad_request", "more than %d frequencies", RDOF_BL_CLAIMS);
            return false;
        }
        c->freqs[c->nfreqs++] = f->num;
    }
    return true;
}

static void wait_start(rdof_bl_t *bl, conn_t *c, int kind, int root, double now)
{
    c->wait = kind;
    c->seq = ++bl->next_seq;
    c->wait_since = now;
    c->wait_max = jnum(bl, root, "wait_max", WAIT_MAX_S);
    if (c->wait_max <= 0)
        c->wait_max = WAIT_MAX_S;
    c->expect_s = jnum(bl, root, "expect_s", 0);
    c->state = C_ACTIVE;
}

static void op_claim(rdof_bl_t *bl, conn_t *c, int root, double now)
{
    if (!req_session(bl, c, root, true))
        return;
    unsigned need = 0;
    const int caps = jget(bl, root, "caps");
    const rdof_jnode_t *cn = rdof_jnode(&bl->doc, caps);
    for (int i = 0; cn && cn->type == RDOF_JARR && i < cn->count; i++) {
        const rdof_jnode_t *s = rdof_jnode(&bl->doc, rdof_jat(&bl->doc, caps, i));
        const unsigned b = s && s->type == RDOF_JSTR ? cap_bit(s->str) : 0;
        if (!b) {
            fail(bl, c, "refused", "this bench has no cap %s", s && s->type == RDOF_JSTR ? s->str : "?");
            return;
        }
        need |= b;
    }
    if (!take_freqs(bl, c, jget(bl, root, "freqs"), need))
        return;
    c->op = OP_CLAIM;
    wait_start(bl, c, W_FREQ, root, now);
}

static void op_release(rdof_bl_t *bl, conn_t *c, int root)
{
    if (!req_session(bl, c, root, true))
        return;
    session_t *s = &bl->s[c->sess];
    const int arr = jget(bl, root, "freqs");
    const rdof_jnode_t *a = rdof_jnode(&bl->doc, arr);
    char out[256];
    size_t k = (size_t)snprintf(out, sizeof out, "{\"released\":[");
    bool first = true;
    for (int i = 0; i < s->nclaims;) {
        bool drop = !a || a->type != RDOF_JARR || !a->count;
        for (int j = 0; !drop && j < a->count; j++) {
            const rdof_jnode_t *f = rdof_jnode(&bl->doc, rdof_jat(&bl->doc, arr, j));
            drop = f && f->type == RDOF_JNUM && fabs(f->num - s->claims[i]) < 1e-4;
        }
        if (!drop) {
            i++;
            continue;
        }
        if (k < sizeof out)
            k += (size_t)snprintf(out + k, sizeof out - k, "%s%.6f", first ? "" : ",", s->claims[i]);
        first = false;
        s->claims[i] = s->claims[--s->nclaims];
    }
    const double hold = jnum(bl, root, "hold", 0);
    bool released_hold = false;
    if ((hold > 0 && (int)hold == s->hold) || ((!a || a->type != RDOF_JARR || !a->count) && hold <= 0 && s->hold)) {
        if (bl->holder == c->sess)
            bl->holder = -1;
        s->hold = 0;
        released_hold = true;
    }
    if (k < sizeof out)
        snprintf(out + k, sizeof out - k, "],\"hold\":%s}", released_hold ? "true" : "false");
    emit(bl, c, "%s", out);
    c->state = C_DONE;
}

static void op_hold(rdof_bl_t *bl, conn_t *c, int root, double now)
{
    if (!req_session(bl, c, root, true))
        return;
    const char *st = jstrv(bl, root, "station");
    if (st && st[0] && strcmp(st, "modem")) {
        fail(bl, c, "refused", "this bench has no station %s (only its modem)", st);
        return;
    }
    const char *notx = bl->p.can_tx ? bl->p.can_tx(bl->p.ctx) : "no transmitter";
    if (notx) {
        fail(bl, c, "unavailable", "the modem can't transmit: %s", notx);
        return;
    }
    if (bl->s[c->sess].hold) { /* already its own */
        emit(bl, c, "{\"held\":%d}", bl->s[c->sess].hold);
        c->state = C_DONE;
        return;
    }
    c->op = OP_HOLD;
    wait_start(bl, c, W_MODEM, root, now);
}

static void op_bursts(rdof_bl_t *bl, conn_t *c, int root, double now, bool listen)
{
    if (!req_session(bl, c, root, false))
        return;
    const double mhz = jnum(bl, root, "freq", NAN);
    const int k = isfinite(mhz) ? freq_index(bl, mhz) : -1;
    if (k < 0 || bl->f[k].ch < 0 || !(bl->f[k].caps & RDOF_BL_CAP_DMR)) {
        fail(bl, c, "refused", "no receiver channel decodes DMR at %.4f MHz on this bench", mhz);
        return;
    }
    c->op = listen ? OP_LISTEN : OP_BURSTS;
    c->ch = bl->f[k].ch;
    c->freq_hz = bl->f[k].freq_hz;
    const double secs = jnum(bl, root, "seconds", listen ? 10.0 : 10.0);
    c->until = secs > 0 ? now + secs : 0;
    c->max = (long)jnum(bl, root, "max_bursts", 0);
    c->until_end = jbool(bl, root, "until_end");
    c->cc = (int)jnum(bl, root, "cc", -1);
    c->first_t = c->prev_t = c->last_t = NAN;
    memset(&c->emb, 0, sizeof c->emb);
    c->cfo_min = INFINITY, c->cfo_max = -INFINITY, c->lvl_max = -INFINITY;
    rdof_bl_window_t w;
    get_window(bl, &w);
    c->fs = w.fs;
    c->state = C_ACTIVE;
    bl->n_bursts++;
    if (!listen)
        emit(bl, c, "{\"ready\":%.3f}", bl->p.utc ? bl->p.utc(bl->p.ctx) : now);
}

/* A capture's output buffer, `want` bytes if it can have them, else RDOF_BL_OUT_IQ: the lines so far go along. */
static bool big_out(conn_t *c, size_t want)
{
    size_t n = want > RDOF_BL_OUT_IQ ? want : RDOF_BL_OUT_IQ;
    uint8_t *big = RDOF_BL_MALLOC(n);
    if (!big && n > RDOF_BL_OUT_IQ)
        big = RDOF_BL_MALLOC(n = RDOF_BL_OUT_IQ);
    if (!big)
        return false;
    memcpy(big, c->out + c->oh, c->ol);
    free(c->out);
    c->out = big, c->ocap = n, c->oh = 0;
    return true;
}

static void op_capture(rdof_bl_t *bl, conn_t *c, int root, double now)
{
    if (!req_session(bl, c, root, false))
        return;
    if (!bl->p.iq_read) {
        fail(bl, c, "unsupported", "this bench keeps no I/Q");
        return;
    }
    rdof_bl_window_t w;
    get_window(bl, &w);
    const double mhz = jnum(bl, root, "freq", NAN), secs = jnum(bl, root, "seconds", 0);
    if (!w.running || w.fs <= 0) {
        fail(bl, c, "unavailable", "the receiver isn't running");
        return;
    }
    if (!isfinite(mhz) || mhz * 1e6 < w.lo_hz || mhz * 1e6 > w.hi_hz) {
        fail(bl, c, "refused", "%.4f MHz is outside the receiver's window (%.4f-%.4f MHz)", mhz, w.lo_hz / 1e6,
             w.hi_hz / 1e6);
        return;
    }
    if (!(secs > 0) || secs > 600) {
        fail(bl, c, "bad_request", "seconds is 0-600");
        return;
    }
    if (!big_out(c, 0)) {
        fail(bl, c, "unavailable", "no memory for a capture");
        return;
    }
    if (bl->p.iq_warm) /* a history kept only while warm starts now */
        bl->p.iq_warm(bl->p.ctx);
    uint64_t oldest = 0, next = 0;
    int8_t probe[2];
    bl->p.iq_read(bl->p.ctx, UINT64_MAX, probe, 0, &oldest, &next);
    double pre = jnum(bl, root, "pre_s", 2.0);
    if (pre < 0)
        pre = 0;
    /* at most half of what the history holds: the other half is room for the reader to be behind (a W12's 2 s
     * ring, read from its oldest sample over Wi-Fi, lost more than half of a 10 s capture) */
    uint64_t pre_n = (uint64_t)(pre * w.fs);
    if (pre_n > (next - oldest) / 2)
        pre_n = (next - oldest) / 2;
    c->op = OP_CAPTURE;
    c->sc0 = c->sc = next - pre_n;
    c->sc_end = next + (uint64_t)(secs * w.fs);
    c->freq_hz = mhz * 1e6;
    c->pre_s = (double)pre_n / w.fs;
    c->gain = jnum(bl, root, "gain", w.gain_db);
    c->state = C_ACTIVE;
    bl->n_iq++;
    if (bl->p.iq_warm)
        bl->p.iq_warm(bl->p.ctx);
    const double t0 = bl->p.sc_utc ? bl->p.sc_utc(bl->p.ctx, c->sc0) : now - c->pre_s;
    emit(bl, c, "{\"ready\":%.6f,\"shared\":true,\"center\":%.6f,\"pre_s\":%.3f,\"fs\":%.1f,\"sc0\":%llu}", t0,
         w.center_hz / 1e6, c->pre_s, w.fs, (unsigned long long)c->sc0);
}

/* ── trigger ──────────────────────────────────────────────────────────────── */

/* Start (or restart) the channel so its first output is centered on wideband sample `first`: the filter fills from
 * (ntaps - 1) / 2 before it. */
static void trig_restart(trig_t *t, uint64_t first)
{
    const int g = (t->ntaps - 1) / 2;
    memset(t->dl, 0, sizeof(float) * 4 * (size_t)t->ntaps);
    t->di = 0;
    t->sc = first >= (uint64_t)g ? first - (uint64_t)g : 0;
    t->next_out = first + (uint64_t)g;
    t->bn = 0, t->bp = 0, t->run = 0;
}

static void op_trigger(rdof_bl_t *bl, conn_t *c, int root, double now)
{
    if (!req_session(bl, c, root, false))
        return;
    if (!bl->p.iq_read) {
        fail(bl, c, "unsupported", "this bench keeps no I/Q");
        return;
    }
    rdof_bl_window_t w;
    get_window(bl, &w);
    const double mhz = jnum(bl, root, "freq", NAN);
    if (!w.running || w.fs <= 0) {
        fail(bl, c, "unavailable", "the receiver isn't running");
        return;
    }
    if (!isfinite(mhz) || mhz * 1e6 < w.lo_hz || mhz * 1e6 > w.hi_hz) {
        fail(bl, c, "refused", "%.4f MHz is outside the receiver's window (%.4f-%.4f MHz)", mhz, w.lo_hz / 1e6,
             w.hi_hz / 1e6);
        return;
    }
    const char *fmt = jstrv(bl, root, "fmt");
    if (fmt && strcmp(fmt, "cf32") && strcmp(fmt, "cs16")) {
        fail(bl, c, "bad_request", "fmt is cf32 or cs16");
        return;
    }
    int d = (int)lround(w.fs / 48000.0);
    if (d < 1)
        d = 1;
    const double rate = w.fs / d;
    const double bw = jnum(bl, root, "bw", 12500), thr = jnum(bl, root, "threshold_db", 10),
                 min_ms = jnum(bl, root, "min_ms", 10), pre = jnum(bl, root, "pre_s", 1),
                 post = jnum(bl, root, "post_s", 2), max_s = jnum(bl, root, "max_s", 30),
                 floor_s = jnum(bl, root, "floor_s", 1), timeout = jnum(bl, root, "timeout_s", 60),
                 floor_db = jnum(bl, root, "floor_db", NAN);
    if (!(bw > 0 && bw <= 0.9 * rate) || !(thr > 0) || !(min_ms >= 0) || !(pre >= 0 && pre <= 2) ||
        !(post >= 0 && post <= 120) || !(max_s > 0 && max_s <= 120) || !(floor_s > 0 && floor_s <= 10) ||
        !(timeout > 0 && timeout <= 3600)) {
        fail(bl, c, "bad_request", "bw 0-%.0f Hz, threshold_db > 0, pre_s 0-2, post_s 0-120, max_s 0-120, "
             "floor_s 0-10, timeout_s 0-3600", 0.9 * rate);
        return;
    }
    trig_t *t = calloc(1, sizeof *t);
    if (t) {
        t->d = d;
        t->ntaps = 8 * d + 1;
        t->taps = RDOF_BL_MALLOC(sizeof(float) * (size_t)t->ntaps);
        t->dl = RDOF_BL_MALLOC(sizeof(float) * 4 * (size_t)t->ntaps);
        t->y = RDOF_BL_MALLOC(sizeof(float) * 2 * (size_t)(RDOF_BL_IQ_CHUNK / d + 2));
    }
    /* room for the whole capture where there's memory for it: a slow link (an ESP32's Wi-Fi) sends it late, not
     * with gaps from a history that moved on */
    const double whole_s = pre + post + (jbool(bl, root, "until_end") ? 2 : 0) + 0.5;
    const double whole = whole_s * rate * (fmt && !strcmp(fmt, "cs16") ? 4 : 8);
    if (!t || !t->taps || !t->dl || !t->y || !big_out(c, whole < RDOF_BL_OUT_TRIG ? (size_t)whole : RDOF_BL_OUT_TRIG)) {
        trig_free(t);
        fail(bl, c, "unavailable", "no memory for a trigger");
        return;
    }
    /* the low-pass: a Hamming-windowed sinc, cut off at bw / 2, unity gain */
    const double fc = bw / 2 / w.fs, mid = (t->ntaps - 1) / 2.0;
    double sum = 0;
    for (int i = 0; i < t->ntaps; i++) {
        const double x = i - mid;
        const double h = (x == 0 ? 2 * fc : sin(2 * M_PI * fc * x) / (M_PI * x)) *
                         (0.54 - 0.46 * cos(2 * M_PI * i / (t->ntaps - 1)));
        t->taps[i] = (float)h;
        sum += h;
    }
    for (int i = 0; i < t->ntaps; i++)
        t->taps[i] = (float)(t->taps[i] / sum);
    t->step = -2 * M_PI * (mhz * 1e6 - w.center_hz) / w.fs;
    t->rate = rate, t->bw = bw, t->thr = thr, t->min_ms = min_ms, t->pre_s = pre, t->post_s = post;
    t->max_s = max_s, t->floor_s = floor_s, t->timeout = timeout, t->started = now;
    t->cs16 = fmt && !strcmp(fmt, "cs16");
    t->until_end = jbool(bl, root, "until_end");
    t->bsize = (int)lround(rate / 1000);
    if (t->bsize < 1)
        t->bsize = 1;
    if (isfinite(floor_db))
        t->floor_db = floor_db, t->have_floor = 2;
    c->trig = t;
    c->op = OP_TRIGGER;
    c->freq_hz = mhz * 1e6;
    c->gain = w.gain_db;
    c->state = C_ACTIVE;
    bl->n_iq++;
    if (bl->p.iq_warm)
        bl->p.iq_warm(bl->p.ctx);
    /* the floor from the history when there is some (a history kept only while warm has none yet) */
    uint64_t oldest = 0, next = 0;
    int8_t probe[2];
    bl->p.iq_read(bl->p.ctx, UINT64_MAX, probe, 0, &oldest, &next);
    uint64_t back = t->have_floor ? 0 : (uint64_t)(floor_s * w.fs) + (uint64_t)t->ntaps;
    if (back > next - oldest)
        back = next - oldest;
    trig_restart(t, next - back + (uint64_t)t->ntaps / 2);
    if (t->have_floor)
        emit(bl, c, "{\"armed\":{\"floor_db\":%.1f,\"rate\":%.1f}}", t->floor_db, t->rate);
}

/* ── run: the bench tools' jobs (section 14.3) ────────────────────────────── */

static double utc_now(const rdof_bl_t *bl, double now) { return bl->p.utc ? bl->p.utc(bl->p.ctx) : now; }

/* A line of the tool's log (Python logging's "%(asctime)s %(message)s") on err. */
static void run_log(rdof_bl_t *bl, conn_t *c, double now, const char *fmt, ...)
{
    char msg[300], line[340];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    const double t = utc_now(bl, now);
    const time_t s = (time_t)t;
    struct tm tm;
    gmtime_r(&s, &tm);
    char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    snprintf(line, sizeof line, "%s,%03d %s", ts, (int)((t - (double)s) * 1000) % 1000, msg);
    jstr(line, c->job->esc, sizeof c->job->esc);
    emit(bl, c, "{\"err\":%s}", c->job->esc);
}

static void run_exit(rdof_bl_t *bl, conn_t *c, int code, const char *killed)
{
    if (killed) {
        char q[200];
        jstr(killed, q, sizeof q);
        emit(bl, c, "{\"exit\":%d,\"killed\":%s}", code, q);
    } else {
        emit(bl, c, "{\"exit\":%d}", code);
    }
    c->state = C_DONE;
}

typedef struct {
    rdof_bl_t *bl;
    conn_t *c;
} run_ctx_t;

/* A record of the listener: an out line. */
static void run_out(void *ctx, const char *line)
{
    run_ctx_t *r = ctx;
    jstr(line, r->c->job->esc, sizeof r->c->job->esc);
    emit(r->bl, r->c, "{\"out\":%s}", r->c->job->esc);
}

/* A file the job writes (--ambe-out): its bytes go to the client, the output buffer grown to hold them. */
static void run_file(void *ctx, const char *name, const uint8_t *b, size_t n)
{
    run_ctx_t *r = ctx;
    conn_t *c = r->c;
    char q[160], hdr[220];
    jstr(name, q, sizeof q);
    const int hn = snprintf(hdr, sizeof hdr, "{\"file\":%s,\"bytes\":%zu}\n", q, n);
    if (room(c) < (size_t)hn + n) {
        const size_t need = c->ol + (size_t)hn + n + RDOF_BL_OUT;
        uint8_t *p = malloc(need);
        if (!p) {
            run_log(r->bl, c, 0, "%s: no memory for %zu bytes", name, n);
            return;
        }
        memcpy(p, c->out + c->oh, c->ol);
        free(c->out);
        c->out = p, c->ocap = need, c->oh = 0;
    }
    put_bytes(c, hdr, (size_t)hn);
    put_bytes(c, b, n);
}

static void job_end(rdof_bl_t *bl, conn_t *c)
{
    job_t *J = c->job;
    if (!J)
        return;
    if (J->tx_open && bl->p.tx_close)
        bl->p.tx_close(bl->p.ctx);
    if (J->rpt_open && bl->p.rpt_close)
        bl->p.rpt_close(bl->p.ctx);
    if (J->ob_open && bl->p.ob_close)
        bl->p.ob_close(bl->p.ctx);
    if (bl->tx_conn >= 0 && &bl->c[bl->tx_conn] == c)
        bl->tx_conn = -1;
    if (J->listening)
        bl->n_bursts--;
    rdof_bj_listen_free(&J->lis);
    free(J->b);
    free(J);
    c->job = NULL;
}

static void run_listen(rdof_bl_t *bl, conn_t *c, double now, double secs)
{
    job_t *J = c->job;
    rdof_bj_listen_init(&J->lis, J->job.json, J->job.ambe_out[0] != 0);
    J->listening = true;
    bl->n_bursts++;
    J->until = now + secs;
    J->state = J_LISTEN;
}

/* A bench_repeater job: it waits for the modem, as a transmit job, with both frequencies claimed. */
static void rpt_run(rdof_bl_t *bl, conn_t *c, job_t *J, int root, double now)
{
    const rdof_bj_job_t *j = &J->job;
    const char *no = !bl->p.rpt_open ? "this bench runs no duplex repeater jobs"
                   : bl->p.can_tx ? bl->p.can_tx(bl->p.ctx) : NULL;
    if (no) {
        free(J);
        fail(bl, c, "unsupported", "%s", no);
        return;
    }
    const double rx = j->rx_mhz, tx = j->tx_mhz;
    const int kr = freq_index(bl, rx), kt = freq_index(bl, tx);
    if (kr < 0 || bl->f[kr].ch < 0 || !(bl->f[kr].caps & RDOF_BL_CAP_DMR)) {
        free(J);
        fail(bl, c, "refused", "no receiver channel decodes DMR at %.4f MHz (the repeater's input) on this bench", rx);
        return;
    }
    if (kt < 0 || !(bl->f[kt].caps & RDOF_BL_CAP_TX)) {
        free(J);
        fail(bl, c, "refused", "%.4f MHz isn't a transmit frequency of this bench", tx);
        return;
    }
    c->job = J;
    c->op = OP_RUN;
    c->ch = bl->f[kr].ch;
    J->max_s = jnum(bl, root, "max_s", 0);
    J->id = ++bl->next_seq;
    c->nfreqs = 2;
    c->freqs[0] = j->rx_mhz, c->freqs[1] = j->tx_mhz;
    wait_start(bl, c, W_MODEM, root, now);
    if (c->expect_s <= 0)
        c->expect_s = j->seconds + 3.0;
}

/* A bench_tx outbound job: it waits for the modem, as a transmit job, its frequency claimed. */
static void ob_run(rdof_bl_t *bl, conn_t *c, job_t *J, int root, double now)
{
    const rdof_bj_job_t *j = &J->job;
    const char *no = !bl->p.ob_open ? "this bench runs no outbound jobs"
                   : bl->p.ob_can ? bl->p.ob_can(bl->p.ctx)
                   : bl->p.can_tx ? bl->p.can_tx(bl->p.ctx) : NULL;
    if (no) {
        free(J);
        fail(bl, c, "unsupported", "%s", no);
        return;
    }
    const double mhz = j->freq_mhz;
    const int k = freq_index(bl, mhz);
    if (k < 0 || !(bl->f[k].caps & RDOF_BL_CAP_TX)) {
        free(J);
        fail(bl, c, "refused", "%.4f MHz isn't a transmit frequency of this bench", mhz);
        return;
    }
    c->job = J;
    c->op = OP_RUN;
    c->ch = bl->f[k].ch;
    J->max_s = jnum(bl, root, "max_s", 0);
    J->id = ++bl->next_seq;
    c->nfreqs = 1;
    c->freqs[0] = j->freq_mhz;
    wait_start(bl, c, W_MODEM, root, now);
    if (c->expect_s <= 0)
        c->expect_s = j->seconds + 3.0;
}

/* Why a transmit job's burst family can't go as built on the bench's path now (NULL: it can). */
/* A job whose bursts must go out as it built them (tx_open's as_built). */
static bool as_built(const rdof_bj_job_t *j)
{
    if (j->cmd != RDOF_BJ_VOICE && j->cmd != RDOF_BJ_CSBK && j->cmd != RDOF_BJ_DATA)
        return false;
    return j->family != RDOF_FAMILY_MS ||
           (j->cmd == RDOF_BJ_VOICE && (j->headers >= 0 || j->terminators >= 0 || j->late >= 0));
}

static const char *family_why(rdof_bl_t *bl, const rdof_bj_job_t *j)
{
    if (j->cmd != RDOF_BJ_VOICE && j->cmd != RDOF_BJ_CSBK && j->cmd != RDOF_BJ_DATA)
        return NULL;
    if (as_built(j))
        return NULL; /* not through the router: the family goes as asked */
    return bl->p.tx_why_family ? bl->p.tx_why_family(bl->p.ctx, j->family) : NULL;
}

static void op_run(rdof_bl_t *bl, conn_t *c, int ci, int root, double now)
{
    (void)ci;
    if (!req_session(bl, c, root, false))
        return;
    const int jo = jget(bl, root, "job");
    const rdof_jnode_t *jn = rdof_jnode(&bl->doc, jo);
    if (!jn || jn->type != RDOF_JOBJ) {
        fail(bl, c, "unsupported", "the gateway runs the bench tools' jobs (bench_tx.py), not programs");
        return;
    }
    job_t *J = calloc(1, sizeof *J);
    if (!J) {
        fail(bl, c, "busy", "no memory for the job");
        return;
    }
    char why[200];
    if (!rdof_bj_parse(&bl->doc, jo, &J->job, J->ambe, sizeof J->ambe, why, sizeof why)) {
        free(J);
        fail(bl, c, "unsupported", "%s", why);
        return;
    }
    const rdof_bj_job_t *j = &J->job;
    if (j->cmd == RDOF_BJ_REPEATER) {
        rpt_run(bl, c, J, root, now);
        return;
    }
    if (j->cmd == RDOF_BJ_OUTBOUND) {
        ob_run(bl, c, J, root, now);
        return;
    }
    const char *fam = family_why(bl, j);
    if (fam) {
        free(J);
        fail(bl, c, "unsupported", "--family: %s", fam);
        return;
    }
    const double mhz = j->freq_mhz;
    const bool tx = j->cmd != RDOF_BJ_LISTEN;
    const bool hears = !tx || j->listen_after > 0;
    const int k = freq_index(bl, j->freq_mhz);
    if (hears && (k < 0 || bl->f[k].ch < 0 || !(bl->f[k].caps & RDOF_BL_CAP_DMR))) {
        free(J);
        fail(bl, c, "refused", "no receiver channel decodes DMR at %.4f MHz on this bench", mhz);
        return;
    }
    if (tx) {
        const char *no = !bl->p.tx_open ? "this bench runs no transmit jobs" : bl->p.can_tx ? bl->p.can_tx(bl->p.ctx) : NULL;
        if (no) {
            free(J);
            fail(bl, c, "unsupported", "%s", no);
            return;
        }
        if (k < 0 || !(bl->f[k].caps & RDOF_BL_CAP_TX)) {
            free(J);
            fail(bl, c, "refused", "%.4f MHz isn't a transmit frequency of this bench", mhz);
            return;
        }
        J->maxb = rdof_bj_max_bursts(j);
        J->b = J->maxb > 0 ? malloc(sizeof *J->b * (size_t)J->maxb) : NULL;
        if (!J->b) {
            free(J);
            fail(bl, c, "busy", "no memory for the job's bursts");
            return;
        }
        J->nparts = rdof_bj_parts(j);
    }
    c->job = J;
    c->op = OP_RUN;
    c->ch = k >= 0 ? bl->f[k].ch : -1;
    J->max_s = jnum(bl, root, "max_s", 0);
    J->id = ++bl->next_seq;
    if (tx) {
        c->nfreqs = 1;
        c->freqs[0] = j->freq_mhz;
        wait_start(bl, c, W_MODEM, root, now);
        if (c->expect_s <= 0)
            c->expect_s = J->nparts * (rdof_bj_gap(j) + 0.06 * J->maxb) + 1.0;
        return;
    }
    c->state = C_ACTIVE;
    J->started = now;
    emit(bl, c, "{\"started\":%ld}", J->id);
    run_log(bl, c, now, "listening on %.4f MHz cc %d for %.0f s", j->freq_mhz, j->cc, j->seconds);
    run_listen(bl, c, now, j->seconds);
}

/* The modem is the job's: key up. */
static void run_granted(rdof_bl_t *bl, conn_t *c, int ci, double now)
{
    job_t *J = c->job;
    bl->tx_conn = ci;
    J->started = now;
    J->state = J_OPEN;
    emit(bl, c, "{\"started\":%ld}", J->id);
}

static void run_heard(rdof_bl_t *bl, conn_t *c, const rdof_bl_burst_t *b)
{
    run_ctx_t r = {bl, c};
    rdof_bj_listen_burst(&c->job->lis, b, run_out, &r);
}

/* The next key-up's bursts: false when the job ended (an error). */
static bool run_build(rdof_bl_t *bl, conn_t *c, double now)
{
    job_t *J = c->job;
    char err[200];
    J->note[0] = J->done[0] = 0;
    J->nb = rdof_bj_build(&J->job, J->part, J->b, J->maxb, J->note, J->done, sizeof J->note, err, sizeof err);
    if (J->nb < 0) {
        run_log(bl, c, now, "%s", err);
        run_exit(bl, c, 1, NULL);
        return false;
    }
    if (J->note[0])
        run_log(bl, c, now, "%s", J->note);
    J->bi = 0;
    J->state = J_SEND;
    return true;
}

/* A repeater record: an out line with --json, else the tool's log line. */
static void rpt_out(void *ctx, const char *line)
{
    run_ctx_t *r = ctx;
    if (r->c->job->job.json)
        run_out(ctx, line);
    else
        run_log(r->bl, r->c, 0, "%s", line);
}

static void tick_rpt(rdof_bl_t *bl, conn_t *c, double now)
{
    job_t *J = c->job;
    const rdof_bj_job_t *j = &J->job;
    run_ctx_t rc = {bl, c};
    char why[200] = "", body[96];
    switch (J->state) {
    case J_OPEN: {
        const rdof_bl_rpt_cfg_t cfg = {(uint32_t)lround(j->rx_mhz * 1e6), (uint32_t)lround(j->tx_mhz * 1e6), j->cc,
                                       j->power, j->hang, j->call_hang, j->slots};
        if (!bl->p.rpt_open(bl->p.ctx, &cfg, why, sizeof why)) {
            run_log(bl, c, now, "can't run the repeater: %s", why[0] ? why : "the transmitter is busy");
            run_exit(bl, c, 1, NULL);
            return;
        }
        J->rpt_open = true;
        if (why[0])
            run_log(bl, c, now, "%s", why);
        J->until = now + 15.0;
        J->state = J_RPT_SETUP;
        break;
    }
    case J_RPT_SETUP: {
        const int st = bl->p.rpt_state(bl->p.ctx, why, sizeof why);
        if (st < 0 || (st == 0 && now > J->until)) {
            run_log(bl, c, now, "the repeater didn't start: %s", st < 0 ? why : "no answer in 15 s");
            run_exit(bl, c, 1, NULL);
            return;
        }
        if (st == 0)
            break;
        run_log(bl, c, now, "rfg repeater RX %.4f TX %.4f MHz cc %d", j->rx_mhz, j->tx_mhz, j->cc);
        if (j->beacon)
            run_log(bl, c, now, "beacon every %.1f s for %.1f s, first after %.1f s", j->beacon, j->beacon_len,
                    j->beacon_phase);
        J->started = now;
        J->ends = now + j->seconds;
        J->state = J_RPT_RUN;
    } /* fall through */
    case J_RPT_RUN: {
        rdof_bl_rpt_ev_t ev;
        while (bl->p.rpt_event(bl->p.ctx, &ev)) {
            const double t = ev.t > 0 ? ev.t : utc_now(bl, now);
            if (ev.kind == RDOF_BL_RPT_HEARD) {
                rdof_bj_rpt_heard(&J->rpt, t, ev.slot, ev.control, ev.burst, ev.rssi, rpt_out, &rc);
            } else if (ev.kind == RDOF_BL_RPT_LOST) {
                snprintf(body, sizeof body, ", \"lost\": %d", ev.slot);
                rdof_bj_rpt_record(t, body, rpt_out, &rc);
            } else if (ev.kind == RDOF_BL_RPT_KEY) {
                J->keyed = ev.keyed;
                snprintf(body, sizeof body, ", \"key\": %s", ev.keyed ? "true" : "false");
                rdof_bj_rpt_record(t, body, rpt_out, &rc);
            }
        }
        const int st = bl->p.rpt_state(bl->p.ctx, why, sizeof why);
        if (st < 0) {
            run_log(bl, c, now, "the repeater stopped: %s", why);
            run_exit(bl, c, 1, NULL);
            return;
        }
        if (j->beacon && now >= J->started + j->beacon_phase + J->beacons * j->beacon && now < J->ends) {
            /* an idle beacon: a local BS_Dwn_Act, as if a radio had asked; not while keyed (traffic) */
            J->beacons++;
            if (J->keyed) {
                snprintf(body, sizeof body, ", \"beacon\": %d, \"skipped\": \"keyed\"", J->beacons);
            } else {
                uint8_t b[33];
                const uint8_t control = rdof_bj_rpt_wake_burst(j->cc, b);
                bl->p.rpt_wake(bl->p.ctx, control, b);
                snprintf(body, sizeof body, ", \"beacon\": %d, \"len_s\": %g", J->beacons, j->beacon_len);
            }
            rdof_bj_rpt_record(utc_now(bl, now), body, rpt_out, &rc);
        }
        if (now < J->ends)
            break;
        bl->p.rpt_close(bl->p.ctx);
        J->rpt_open = false;
        bl->tx_conn = -1;
        rdof_bj_rpt_summary(&J->rpt, utc_now(bl, now), rpt_out, &rc);
        run_exit(bl, c, 0, NULL);
        break;
    }
    }
}

/* ── bench_tx outbound ── */

#define OB_SETUP_S 5.0         /* from the modem's setup to the first slot on the air, at most */
#define OB_DRAIN_S 2.0         /* from the unkey to the modem saying so, at most */

/* Slot k's burst: its sync center as a TX sample, and that sample's UTC from the latched map. */
static int64_t ob_sc(const job_t *J, long k) { return J->ob.base + (int64_t)llround(k * J->ob.slot) + J->ob.sync_off; }
static double ob_utc(const job_t *J, int64_t sc) { return J->ob_t + (double)(sc - J->ob_pos) / J->ob.rate; }

/* The records of the slots queued since the last tick: one a burst (in the job's --slots), a skip where the slot
 * clock jumped. As many as the connection has room for; the rest next time. */
static void ob_report(rdof_bl_t *bl, conn_t *c)
{
    job_t *J = c->job;
    const rdof_bj_job_t *j = &J->job;
    run_ctx_t rc = {bl, c};
    char line[256];
    while (J->ob_rep < J->ob.next && room(c) > 1024) {
        const long k = J->ob_rep;
        if (J->ob.skips > J->ob_skips && k >= J->ob.last_from && J->ob.last_to > k) {
            const int64_t sc = ob_sc(J, J->ob.last_to);
            snprintf(line, sizeof line,
                     "{\"t\": %.6f, \"skip\": {\"from_n\": %ld, \"to_n\": %ld, \"slots\": %ld, \"sc\": %lld}}",
                     ob_utc(J, sc), k - J->ob.first, J->ob.last_to - J->ob.first, J->ob.last_to - k, (long long)sc);
            run_out(&rc, line);
            J->ob_skips = J->ob.skips;
            J->ob_rep = J->ob.last_to;
            continue;
        }
        const int ts = (int)(k & 1) + 1;
        J->ob_n[ts]++;
        J->ob_rep++;
        if (!(j->slots & (1u << ts)))
            continue;
        const int64_t sc = ob_sc(J, k);
        snprintf(line, sizeof line,
                 "{\"t\": %.6f, \"sc\": %lld, \"n\": %ld, \"slot\": %d, \"sync\": \"BS_DATA\", \"kind\": \"IDLE\", "
                 "\"cc\": %d}",
                 ob_utc(J, sc), (long long)sc, k - J->ob.first, ts, j->cc);
        run_out(&rc, line);
    }
    if (J->ob.skips > J->ob_skips && J->ob_rep >= J->ob.last_to)
        J->ob_skips = J->ob.skips; /* a jump before the key-up's first slot: nothing went out there */
}

static void tick_ob(rdof_bl_t *bl, conn_t *c, double now)
{
    job_t *J = c->job;
    const rdof_bj_job_t *j = &J->job;
    run_ctx_t rc = {bl, c};
    char why[200] = "", line[512];
    if (J->state == J_OPEN) {
        const rdof_bl_ob_cfg_t cfg = {(uint32_t)lround(j->freq_mhz * 1e6), j->cc, j->power, j->seconds};
        if (!bl->p.ob_open(bl->p.ctx, &cfg, why, sizeof why)) {
            run_log(bl, c, now, "can't transmit: %s", why[0] ? why : "the transmitter is busy");
            run_exit(bl, c, 1, NULL);
            return;
        }
        J->ob_open = true;
        if (why[0])
            run_log(bl, c, now, "%s", why);
        J->until = now + OB_SETUP_S;
        J->state = J_OB_SETUP;
        return;
    }
    const int st = bl->p.ob_state(bl->p.ctx, &J->ob, why, sizeof why);
    if (st < 0) {
        run_log(bl, c, now, "the outbound stopped: %s", why[0] ? why : "the modem failed");
        run_exit(bl, c, 1, NULL);
        return;
    }
    if (J->state == J_OB_SETUP) {
        if (st < 1 || !J->ob.keyed || !J->ob.on_air || J->ob.first < 0) {
            if (now > J->until) {
                run_log(bl, c, now, "the outbound didn't key up in %.0f s", OB_SETUP_S);
                run_exit(bl, c, 1, NULL);
            }
            return;
        }
        J->ob_pos = J->ob.map_pos;
        J->ob_t = J->ob.map_t;
        J->ob_rep = J->ob.first;
        J->ob_skips = J->ob.skips;
        const long n = lround(j->seconds * 1000.0 / SLOT_MS);
        J->ob_last = J->ob.first + (n > 1 ? n : 1) - 1;
        const int64_t sc0 = ob_sc(J, J->ob.first);
        run_log(bl, c, now, "outbound on %.4f MHz cc %d: BS idle bursts with the CACH on both slots, %ld slots", j->freq_mhz,
                j->cc, J->ob_last - J->ob.first + 1);
        snprintf(line, sizeof line,
                 "{\"t\": %.6f, \"start\": {\"freq\": %.4f, \"cc\": %d, \"slots\": %ld, \"first_slot\": %d, "
                 "\"rate\": %.1f, \"period_samples\": %.1f, \"period_ms\": %.3f, \"sync_off\": %d, \"sc0\": %lld, "
                 "\"t0\": %.6f, \"ppm\": %.2f}}",
                 utc_now(bl, now), j->freq_mhz, j->cc, J->ob_last - J->ob.first + 1, (int)(J->ob.first & 1) + 1,
                 J->ob.rate, J->ob.slot, 1e3 * J->ob.slot / J->ob.rate, J->ob.sync_off, (long long)sc0,
                 ob_utc(J, sc0), J->ob.ppm);
        run_out(&rc, line);
        J->state = J_OB_RUN;
    }
    if (J->state == J_OB_RUN && J->ob.next >= J->ob_last) {
        bl->p.ob_unkey(bl->p.ctx); /* the slot queued next is the last */
        J->until = now + OB_DRAIN_S;
        J->state = J_OB_DRAIN;
    }
    ob_report(bl, c);
    if (J->state != J_OB_DRAIN || J->ob_rep < J->ob.next || (st != 2 && now < J->until))
        return;
    if (st != 2)
        run_log(bl, c, now, "the modem didn't say it unkeyed in %.0f s", OB_DRAIN_S);
    bl->p.ob_close(bl->p.ctx);
    J->ob_open = false;
    bl->tx_conn = -1;
    const long sent = J->ob_rep - J->ob.first;
    run_log(bl, c, now, "sent %ld slots (%ld TS1, %ld TS2), %ld skips", sent, J->ob_n[1], J->ob_n[2], J->ob.skips);
    snprintf(line, sizeof line,
             "{\"t\": %.6f, \"summary\": {\"slots\": %ld, \"ts1\": %ld, \"ts2\": %ld, \"skips\": %ld, "
             "\"seconds\": %.3f, \"sc0\": %lld, \"t0\": %.6f, \"period_samples\": %.1f, \"rate\": %.1f}}",
             utc_now(bl, now), sent, J->ob_n[1], J->ob_n[2], J->ob.skips, sent * SLOT_MS / 1000.0,
             (long long)ob_sc(J, J->ob.first), ob_utc(J, ob_sc(J, J->ob.first)), J->ob.slot, J->ob.rate);
    run_out(&rc, line);
    run_exit(bl, c, 0, NULL);
}

static void tick_run(rdof_bl_t *bl, conn_t *c, int ci, double now)
{
    (void)ci;
    job_t *J = c->job;
    const rdof_bj_job_t *j = &J->job;
    char why[200] = "";
    if (J->max_s > 0 && now - J->started > J->max_s) {
        char k[80];
        snprintf(k, sizeof k, "the job ran past max_s %.0f s", J->max_s);
        run_exit(bl, c, -9, k);
        return;
    }
    if (j->cmd == RDOF_BJ_REPEATER) {
        tick_rpt(bl, c, now);
        return;
    }
    if (j->cmd == RDOF_BJ_OUTBOUND) {
        tick_ob(bl, c, now);
        return;
    }
    switch (J->state) {
    case J_OPEN: {
        static const char *const CMDS[] = {"", "voice", "csbk", "data", "listen"};
        const char *fam = family_why(bl, j); /* the path may have changed while the job waited */
        if (fam) {
            run_log(bl, c, now, "can't transmit: --family: %s", fam);
            run_exit(bl, c, 1, NULL);
            return;
        }
        if (!bl->p.tx_open(bl->p.ctx, (uint32_t)lround(j->freq_mhz * 1e6), j->cc, j->power, as_built(j), why,
                           sizeof why)) {
            run_log(bl, c, now, "can't transmit: %s", why[0] ? why : "the transmitter is busy");
            run_exit(bl, c, 1, NULL);
            return;
        }
        J->tx_open = true;
        if (why[0])
            run_log(bl, c, now, "%s", why);
        run_log(bl, c, now, "%s on %.4f MHz cc %d", CMDS[j->cmd], j->freq_mhz, j->cc);
        J->part = 0;
        run_build(bl, c, now);
        break;
    }
    case J_SEND: {
        int n = bl->p.tx_space(bl->p.ctx, why, sizeof why);
        if (n < 0) {
            run_log(bl, c, now, "transmit failed: %s", why);
            run_exit(bl, c, 1, NULL);
            return;
        }
        for (; n > 0 && J->bi < J->nb; n--, J->bi++)
            bl->p.tx_push(bl->p.ctx, J->b[J->bi].control, J->b[J->bi].burst);
        if (J->bi == J->nb) {
            bl->p.tx_unkey(bl->p.ctx);
            J->state = J_DRAIN;
        }
        break;
    }
    case J_DRAIN:
        if (!bl->p.tx_idle(bl->p.ctx))
            break;
        run_log(bl, c, now, "sent %d bursts", J->nb);
        if (J->done[0])
            run_log(bl, c, now, "%s", J->done);
        if (++J->part < J->nparts) {
            J->until = now + rdof_bj_gap(j);
            J->state = J_GAP;
            break;
        }
        bl->p.tx_close(bl->p.ctx);
        J->tx_open = false;
        bl->tx_conn = -1;
        if (j->listen_after > 0) {
            run_log(bl, c, now, "listening on %.4f MHz for %.0f s", j->freq_mhz, j->listen_after);
            run_listen(bl, c, now, j->listen_after);
        } else {
            run_exit(bl, c, 0, NULL);
        }
        break;
    case J_GAP:
        if (now >= J->until)
            run_build(bl, c, now);
        break;
    case J_LISTEN:
        if (now >= J->until) {
            run_ctx_t r = {bl, c};
            rdof_bj_listen_summary(&J->lis, j->ambe_out, run_out, run_file, &r);
            run_exit(bl, c, 0, NULL);
        }
        break;
    }
}

/* ── signal modes: tx and rx (section 14.6) ───────────────────────────────── */

#define STX_IN 2048            /* a tx stream's bytes the engine holds for the mode */
#define STX_TAIL_S 10.0        /* max_s past the mode's own length, by default */
#define SRX_REC 8192
#define SRX_FLOOR_S 0.3        /* the floor learned before an rx starts (no squelch_dbfs given) */

typedef struct sigtx {
    const rdof_sig_mode_t *m;
    void *tx;
    rdof_sig_src_t src;
    uint32_t freq_hz;
    int power;
    double seconds, started, max_s;
    double retry_until;       /* sig_open said not yet: asked again each tick until then (0: not waiting) */
    bool open;
    long id;
    uint8_t in[STX_IN];
    size_t inn;
    uint64_t taken;           /* stream bytes the mode took */
    char why[200];
} sigtx_t;

/* A channel cut from the wideband history: mixed down, a Hamming-windowed sinc low-pass, decimated to about
 * 48 kS/s (the trigger's, on its own). */
typedef struct {
    int d, ntaps;
    float *taps, *dl;         /* dl: each sample twice, so a window is contiguous */
    int di;
    double step;
    uint64_t sc, next_out;    /* the next input to take; the input that completes the next output */
} chan_t;

typedef struct sigrx {
    const rdof_sig_mode_t *m;
    void *rx;
    chan_t ch;
    float *y;                 /* a chunk's channel samples, [n][2] */
    float *out[RDOF_SIG_STREAMS];
    int16_t *pcm;
    rdof_sig_stream_t st[RDOF_SIG_STREAMS];
    int nst;
    unsigned long seq;
    double rate, bw, seconds, over_db, floor_db, squelch, last_tick;
    bool learning;
    int hist[TRIG_HIST];
    long blocks, need_blocks;
    int bsize, bn;
    double bp;
    uint64_t start_sc, end_sc;
    long gaps, filled, samples;
    char *rec;                /* a record or the summary, SRX_REC (emit's is bl->tmp) */
} sigrx_t;

static void chan_free(chan_t *h)
{
    free(h->taps), free(h->dl);
    h->taps = h->dl = NULL;
}

static void chan_restart(chan_t *h, uint64_t first)
{
    const int g = (h->ntaps - 1) / 2;
    memset(h->dl, 0, sizeof(float) * 4 * (size_t)h->ntaps);
    h->di = 0;
    h->sc = first >= (uint64_t)g ? first - (uint64_t)g : 0;
    h->next_out = first + (uint64_t)g;
}

static bool chan_init(chan_t *h, double fs, double offset_hz, double bw)
{
    h->d = (int)lround(fs / 48000.0);
    if (h->d < 1)
        h->d = 1;
    h->ntaps = 8 * h->d + 1;
    h->taps = RDOF_BL_MALLOC(sizeof(float) * (size_t)h->ntaps);
    h->dl = RDOF_BL_MALLOC(sizeof(float) * 4 * (size_t)h->ntaps);
    if (!h->taps || !h->dl) {
        chan_free(h);
        return false;
    }
    const double fc = bw / 2 / fs, mid = (h->ntaps - 1) / 2.0;
    double sum = 0;
    for (int i = 0; i < h->ntaps; i++) {
        const double x = i - mid;
        const double v = (x == 0 ? 2 * fc : sin(2 * M_PI * fc * x) / (M_PI * x)) *
                         (0.54 - 0.46 * cos(2 * M_PI * i / (h->ntaps - 1)));
        h->taps[i] = (float)v;
        sum += v;
    }
    for (int i = 0; i < h->ntaps; i++)
        h->taps[i] = (float)(h->taps[i] / sum);
    h->step = -2 * M_PI * offset_hz / fs;
    return true;
}

/* got history samples from h->sc in: the outputs into y ([n][2]), how many; *first: the first one's center. */
static int chan_run(chan_t *h, const int8_t *in, long got, float *y, uint64_t *first)
{
    const uint64_t g = (uint64_t)(h->ntaps - 1) / 2;
    const double ph = fmod(h->step * (double)h->sc, 2 * M_PI); /* in float after (an ESP32's FPU has no double) */
    float pr = (float)cos(ph), pq = (float)sin(ph);
    const float rr = (float)cos(h->step), rq = (float)sin(h->step), k127 = 1.0f / 127;
    int n = 0;
    for (long i = 0; i < got; i++) {
        const float xi = in[2 * i] * k127, xq = in[2 * i + 1] * k127;
        float *a = h->dl + 2 * h->di, *b = a + 2 * h->ntaps;
        a[0] = b[0] = xi * pr - xq * pq;
        a[1] = b[1] = xi * pq + xq * pr;
        const float npr = pr * rr - pq * rq;
        pq = pr * rq + pq * rr, pr = npr;
        if (++h->di == h->ntaps)
            h->di = 0;
        if (h->sc + (uint64_t)i != h->next_out)
            continue;
        if (!n)
            *first = h->next_out - g;
        h->next_out += (uint64_t)h->d;
        const float *x = h->dl + 2 * h->di;
        float yi = 0, yq = 0;
        for (int k = 0; k < h->ntaps; k++)
            yi += h->taps[k] * x[2 * k], yq += h->taps[k] * x[2 * k + 1];
        y[2 * n] = yi, y[2 * n + 1] = yq, n++;
    }
    h->sc += (uint64_t)got;
    return n;
}

static void sig_end(rdof_bl_t *bl, conn_t *c)
{
    sigtx_t *t = c->stx;
    if (t) {
        if (t->open && bl->p.sig_close)
            bl->p.sig_close(bl->p.ctx); /* the source is never pulled again */
        if (bl->tx_conn >= 0 && &bl->c[bl->tx_conn] == c)
            bl->tx_conn = -1;
        if (t->tx)
            t->m->tx_free(t->tx);
        free(t);
        c->stx = NULL;
    }
    sigrx_t *r = c->srx;
    if (r) {
        if (r->rx)
            r->m->rx_free(r->rx);
        chan_free(&r->ch);
        free(r->y), free(r->pcm), free(r->rec);
        for (int k = 0; k < RDOF_SIG_STREAMS; k++)
            free(r->out[k]);
        free(r);
        c->srx = NULL;
    }
}

/* The mode's name from the request, found in the platform's list: NULL after a refusal. */
static const rdof_sig_mode_t *sig_mode(rdof_bl_t *bl, conn_t *c, int root, bool tx)
{
    const char *name = jstrv(bl, root, "mode");
    const rdof_sig_mode_t *m = name ? rdof_sig_find(bl->p.modes, name) : NULL;
    if (m && (tx ? m->tx_new != NULL : m->rx_new != NULL))
        return m;
    char list[160];
    if (!rdof_sig_names_json(bl->p.modes, tx, list, sizeof list))
        strcpy(list, "[]");
    if (!name)
        fail(bl, c, "bad_request", "no mode (this bench %s %s)", tx ? "sends" : "receives", list);
    else
        fail(bl, c, "unsupported", "this bench doesn't %s %s (it does %s)", tx ? "send" : "receive", name, list);
    return NULL;
}

static size_t stx_room(const conn_t *c)
{
    const sigtx_t *t = c->stx;
    if (c->state == C_LINE || !t || !t->m->tx_in_want || !t->m->tx_in_want(t->tx))
        return 1024; /* the request line, or bytes dropped (a close still counts) */
    return STX_IN - t->inn;
}

/* What the engine holds goes to the mode, as it has room. */
static void stx_feed(sigtx_t *t)
{
    if (!t->inn || !t->m->tx_in)
        return;
    size_t room = t->m->tx_in_room(t->tx);
    if (room > t->inn)
        room = t->inn;
    const size_t n = room ? t->m->tx_in(t->tx, t->in, room) : 0;
    memmove(t->in, t->in + n, t->inn - n);
    t->inn -= n;
    t->taken += n;
}

static void stx_input(rdof_bl_t *bl, conn_t *c, const uint8_t *b, size_t n)
{
    (void)bl;
    sigtx_t *t = c->stx;
    if (!t->m->tx_in_want || !t->m->tx_in_want(t->tx))
        return;
    while (n) {
        size_t k = STX_IN - t->inn;
        k = k < n ? k : n;
        memcpy(t->in + t->inn, b, k);
        t->inn += k, b += k, n -= k;
        stx_feed(t);
        if (t->inn == STX_IN)
            break; /* more than the server was told it could send: dropped */
    }
}

/* {"op":"tx","mode":M,"freq":MHz,"power":0-255, the mode's parameters}: the stream follows the line. */
static void op_tx(rdof_bl_t *bl, conn_t *c, int root, double now)
{
    if (!req_session(bl, c, root, false))
        return;
    const rdof_sig_mode_t *m = sig_mode(bl, c, root, true);
    if (!m)
        return;
    const char *no = !bl->p.sig_open ? "this bench transmits no signal modes"
                   : bl->p.can_tx ? bl->p.can_tx(bl->p.ctx) : NULL;
    if (no) {
        fail(bl, c, "unsupported", "%s", no);
        return;
    }
    const double mhz = jnum(bl, root, "freq", NAN);
    const int k = isfinite(mhz) ? freq_index(bl, mhz) : -1;
    if (k < 0 || !(bl->f[k].caps & RDOF_BL_CAP_TX)) {
        fail(bl, c, "refused", "%.4f MHz isn't a transmit frequency of this bench", mhz);
        return;
    }
    sigtx_t *t = calloc(1, sizeof *t);
    if (!t) {
        fail(bl, c, "busy", "no memory for the job");
        return;
    }
    char why[200] = "";
    const double fs = bl->p.sig_rate ? bl->p.sig_rate(bl->p.ctx) : 48000.0; /* the transmitter's */
    t->tx = m->tx_new(&bl->doc, root, fs, &t->seconds, why, sizeof why);
    if (!t->tx) {
        free(t);
        fail(bl, c, "bad_request", "%s", why[0] ? why : "the mode refused its parameters");
        return;
    }
    t->m = m;
    t->src = m->tx_src(t->tx);
    t->src.rate = fs;
    t->freq_hz = (uint32_t)lround(mhz * 1e6);
    t->power = (int)jnum(bl, root, "power", 0);
    t->max_s = jnum(bl, root, "max_s", t->seconds + STX_TAIL_S);
    t->id = ++bl->next_seq;
    c->stx = t;
    c->op = OP_TX;
    c->nfreqs = 1;
    c->freqs[0] = mhz;
    const uint64_t want = m->tx_in_want ? m->tx_in_want(t->tx) : 0;
    emit(bl, c, "{\"tx\":{\"mode\":\"%s\",\"freq\":%.6f,\"seconds\":%.3f,\"input_bytes\":%llu}}", m->name, mhz,
         t->seconds, (unsigned long long)want);
    wait_start(bl, c, W_MODEM, root, now);
    if (c->expect_s <= 0)
        c->expect_s = t->seconds + 3.0;
}

static void stx_open(rdof_bl_t *bl, conn_t *c, double now);

static void stx_granted(rdof_bl_t *bl, conn_t *c, int ci, double now)
{
    sigtx_t *t = c->stx;
    bl->tx_conn = ci;
    t->started = now;
    stx_feed(t); /* the stream so far is in before the key-up: a prebuffer */
    t->retry_until = 0;
    stx_open(bl, c, now);
}

#define STX_RETRY_S 10.0

/* The source keyed (started), not yet (asked again from tick_tx) or refused. */
static void stx_open(rdof_bl_t *bl, conn_t *c, double now)
{
    sigtx_t *t = c->stx;
    t->why[0] = 0;
    const int r = bl->p.sig_open(bl->p.ctx, t->freq_hz, t->power, t->seconds, &t->src, t->why, sizeof t->why);
    if (r == 0 && (!t->retry_until || now < t->retry_until)) {
        if (!t->retry_until) {
            t->retry_until = now + STX_RETRY_S;
            emit(bl, c, "{\"note\":\"waiting to transmit: %s\"}", t->why[0] ? t->why : "the transmitter is busy");
        }
        return;
    }
    if (r <= 0) {
        fail(bl, c, r ? "unavailable" : "busy", "can't transmit: %s", t->why[0] ? t->why : "the transmitter is busy");
        return;
    }
    t->retry_until = 0;
    t->started = now;
    t->open = true;
    char q[220] = "null";
    if (t->why[0])
        jstr(t->why, q, sizeof q);
    emit(bl, c, "{\"started\":%ld,\"t\":%.6f,\"note\":%s}", t->id, utc_now(bl, now), q);
}

static void tick_tx(rdof_bl_t *bl, conn_t *c, double now)
{
    sigtx_t *t = c->stx;
    stx_feed(t);
    if (!t->open && t->retry_until && c->wait == W_NONE)
        stx_open(bl, c, now);
    if (!t->open || c->wait != W_NONE)
        return;
    char why[200] = "";
    const int st = bl->p.sig_state(bl->p.ctx, why, sizeof why);
    if (st < 0) {
        fail(bl, c, "internal", "transmit failed: %s", why);
        return;
    }
    const bool late = now - t->started > t->max_s;
    if (st != 2 && !late)
        return;
    bl->p.sig_close(bl->p.ctx);
    t->open = false;
    bl->tx_conn = -1;
    char rep[600] = "";
    if (t->m->tx_report && !t->m->tx_report(t->tx, rep, sizeof rep))
        rep[0] = 0;
    emit(bl, c, "{\"done\":{\"mode\":\"%s\",\"freq\":%.6f,\"on_air_s\":%.3f,\"input_bytes\":%llu,\"cut\":%s%s}}",
         t->m->name, t->freq_hz / 1e6, now - t->started, (unsigned long long)t->taken, late ? "true" : "false", rep);
    c->state = C_DONE;
}

/* {"op":"rx","mode":M,"freq":MHz,"seconds":S,"bw":Hz,"streams":"audio,disc", the mode's parameters} */
static void op_rx(rdof_bl_t *bl, conn_t *c, int root, double now)
{
    (void)now;
    if (!req_session(bl, c, root, false))
        return;
    const rdof_sig_mode_t *m = sig_mode(bl, c, root, false);
    if (!m)
        return;
    if (!bl->p.iq_read) {
        fail(bl, c, "unsupported", "this bench keeps no I/Q");
        return;
    }
    rdof_bl_window_t w;
    get_window(bl, &w);
    const double mhz = jnum(bl, root, "freq", NAN), secs = jnum(bl, root, "seconds", 10),
                 bw = jnum(bl, root, "bw", 16000), sq = jnum(bl, root, "squelch_dbfs", NAN),
                 over = jnum(bl, root, "squelch_over_db", 10);
    if (!w.running || w.fs <= 0) {
        fail(bl, c, "unavailable", "the receiver isn't running");
        return;
    }
    if (!isfinite(mhz) || mhz * 1e6 < w.lo_hz || mhz * 1e6 > w.hi_hz) {
        fail(bl, c, "refused", "%.4f MHz is outside the receiver's window (%.4f-%.4f MHz)", mhz, w.lo_hz / 1e6,
             w.hi_hz / 1e6);
        return;
    }
    if (!(secs > 0 && secs <= 3600) || !(bw >= 1000 && bw <= 40000) || !(over > 0 && over <= 60)) {
        fail(bl, c, "bad_request", "seconds 0-3600, bw 1000-40000 Hz, squelch_over_db 0-60");
        return;
    }
    sigrx_t *r = calloc(1, sizeof *r);
    if (!r || !chan_init(&r->ch, w.fs, mhz * 1e6 - w.center_hz, bw)) {
        free(r);
        fail(bl, c, "unavailable", "no memory for a channel");
        return;
    }
    r->m = m;
    r->rate = w.fs / r->ch.d;
    if (bw > 0.9 * r->rate) {
        const double most = 0.9 * r->rate;
        chan_free(&r->ch), free(r);
        fail(bl, c, "bad_request", "bw is at most %.0f Hz here", most);
        return;
    }
    char why[200] = "";
    /* made now so a bad parameter is refused at once; without squelch_dbfs made again once the floor is known */
    r->rx = m->rx_new(&bl->doc, root, r->rate, isfinite(sq) ? sq : -60, why, sizeof why);
    const int maxn = RDOF_BL_IQ_CHUNK / r->ch.d + 2;
    r->y = RDOF_BL_MALLOC(sizeof(float) * 2 * (size_t)maxn);
    r->pcm = RDOF_BL_MALLOC(sizeof(int16_t) * (size_t)maxn);
    r->rec = RDOF_BL_MALLOC(SRX_REC);
    bool ok = r->rx && r->y && r->pcm && r->rec;
    for (int k = 0; ok && k < RDOF_SIG_STREAMS; k++)
        ok = (r->out[k] = RDOF_BL_MALLOC(sizeof(float) * (size_t)maxn)) != NULL;
    if (!ok || !big_out(c, 0)) {
        c->srx = r;
        sig_end(bl, c);
        if (why[0])
            fail(bl, c, "bad_request", "%s", why);
        else
            fail(bl, c, "unavailable", "no memory for a receiver");
        return;
    }
    r->nst = m->rx_streams(r->rx, r->st, RDOF_SIG_STREAMS);
    r->bw = bw, r->seconds = secs, r->over_db = over;
    r->bsize = (int)lround(r->rate / 1000);
    if (r->bsize < 1)
        r->bsize = 1;
    r->learning = !isfinite(sq);
    r->squelch = sq;
    r->floor_db = NAN;
    r->need_blocks = (long)(SRX_FLOOR_S * 1000);
    c->srx = r;
    c->op = OP_RX;
    c->freq_hz = mhz * 1e6;
    c->state = C_ACTIVE;
    bl->n_iq++;
    if (bl->p.iq_warm)
        bl->p.iq_warm(bl->p.ctx);
    uint64_t oldest = 0, next = 0;
    int8_t probe[2];
    bl->p.iq_read(bl->p.ctx, UINT64_MAX, probe, 0, &oldest, &next);
    /* the floor from the history where it has some, then on from now */
    uint64_t back = r->learning ? (uint64_t)(SRX_FLOOR_S * w.fs) + (uint64_t)r->ch.ntaps : 0;
    if (back > next - oldest)
        back = next - oldest;
    chan_restart(&r->ch, next - back + (uint64_t)r->ch.ntaps / 2);
    if (!r->learning)
        r->start_sc = r->ch.next_out - (uint64_t)(r->ch.ntaps - 1) / 2;
}

/* The receiver starts: its header line (the floor learned, or squelch_dbfs given). False after a failure. */
static bool srx_start(rdof_bl_t *bl, conn_t *c, uint64_t first, double now, const rdof_bl_window_t *w)
{
    sigrx_t *r = c->srx;
    if (r->learning) { /* the floor: the median 1 ms block */
        long acc = 0;
        int m = 0;
        while (m < TRIG_HIST - 1 && (acc += r->hist[m]) <= r->blocks / 2)
            m++;
        r->floor_db = -150 + m * 0.5 + 0.25;
        r->squelch = r->floor_db + r->over_db;
        r->learning = false;
        /* the request again (the parser's document is the latest request's): the receiver with this squelch */
        bl->doc = (rdof_jdoc_t){.nodes = bl->nodes, .max = NODES, .buf = bl->strbuf, .cap = RDOF_BL_LINE_MAX + 64};
        const int root = rdof_jparse(&bl->doc, c->line, c->ln);
        char why[200] = "";
        r->m->rx_free(r->rx);
        r->rx = root >= 0 ? r->m->rx_new(&bl->doc, root, r->rate, r->squelch, why, sizeof why) : NULL;
        if (!r->rx) {
            fail(bl, c, "internal", "the receiver didn't start again: %s", why);
            return false;
        }
        r->nst = r->m->rx_streams(r->rx, r->st, RDOF_SIG_STREAMS);
    }
    r->start_sc = first;
    r->end_sc = first + (uint64_t)(r->seconds * w->fs);
    uint64_t oldest = 0, next = 0;
    int8_t probe[2];
    bl->p.iq_read(bl->p.ctx, UINT64_MAX, probe, 0, &oldest, &next);
    char st[RDOF_SIG_STREAMS * 120] = "", fl[24] = "null";
    size_t k = 0;
    for (int i = 0; i < r->nst && k < sizeof st; i++)
        k += (size_t)snprintf(st + k, sizeof st - k, "%s{\"name\":\"%s\",\"unit\":\"%s\",\"rate\":%.3f,\"full\":%.3f}",
                              i ? "," : "", r->st[i].name, r->st[i].unit, r->st[i].rate, r->st[i].full);
    if (isfinite(r->floor_db))
        snprintf(fl, sizeof fl, "%.1f", r->floor_db);
    emit(bl, c,
         "{\"rx\":{\"mode\":\"%s\",\"freq\":%.6f,\"rate\":%.3f,\"bw\":%.0f,\"t0\":%.6f,\"sc0\":%llu,\"seconds\":%.3f,"
         "\"floor_dbfs\":%s,\"squelch_dbfs\":%.1f,\"scale_db\":%.2f,\"streams\":[%s]}}",
         r->m->name, c->freq_hz / 1e6, r->rate, r->bw, trig_utc(bl, first, now, next, w->fs),
         (unsigned long long)first, r->seconds, fl, r->squelch, w->iq_scale_db, st);
    return true;
}

static void srx_records(rdof_bl_t *bl, conn_t *c)
{
    sigrx_t *r = c->srx;
    size_t n;
    while (room(c) > SRX_REC + 16 && (n = r->m->rx_record(r->rx, r->rec, SRX_REC - 16)) > 0) {
        r->rec[n] = 0;
        emit(bl, c, "{\"rec\":%s}", r->rec);
    }
}

static void tick_rx(rdof_bl_t *bl, conn_t *c, double now)
{
    sigrx_t *r = c->srx;
    rdof_bl_window_t w;
    get_window(bl, &w);
    const uint64_t g = (uint64_t)(r->ch.ntaps - 1) / 2, d = (uint64_t)r->ch.d;
    double dt = r->last_tick > 0 ? now - r->last_tick : 0.02;
    r->last_tick = now;
    dt = dt < 0.02 ? 0.02 : dt > 0.125 ? 0.125 : dt;
    long budget = (long)(2 * dt * w.fs) + RDOF_BL_IQ_CHUNK; /* the lock is held: at most this much a tick */
    srx_records(bl, c);
    while (budget > 0) {
        if (!r->learning && r->end_sc && r->ch.next_out - g >= r->end_sc) {
            srx_records(bl, c);
            if (room(c) < SRX_REC + 400)
                return;
            size_t n = r->m->rx_summary(r->rx, r->rec, SRX_REC - 400);
            if (!n)
                strcpy(r->rec, "null"), n = 4;
            r->rec[n] = 0;
            const long gaps = r->gaps + (r->filled + (long)d - 1) / (long)d;
            emit(bl, c, "{\"done\":{\"mode\":\"%s\",\"freq\":%.6f,\"seconds\":%.3f,\"samples\":%ld,\"gap_samples\":%ld,"
                        "\"summary\":%s}}",
                 r->m->name, c->freq_hz / 1e6, r->samples / r->rate, r->samples, gaps, r->rec);
            c->state = C_DONE;
            return;
        }
        const size_t frame = (size_t)r->nst * ((size_t)(RDOF_BL_IQ_CHUNK / r->ch.d + 2) * 2 + 120) + 200;
        if (room(c) < frame)
            return; /* the client is behind: later */
        uint64_t oldest = 0, next = 0;
        const long got = bl->p.iq_read(bl->p.ctx, r->ch.sc, bl->iq, RDOF_BL_IQ_CHUNK, &oldest, &next);
        if (got <= 0 && r->ch.sc < oldest) { /* overwritten before it went: on from the oldest, what's between lost */
            const uint64_t want = oldest + g, first = r->ch.next_out - g;
            const uint64_t k = want > first ? (want - first + d - 1) / d : 0;
            if (!r->learning && r->end_sc) {
                r->gaps += (long)k;
                emit(bl, c, "{\"gap\":{\"sc\":%llu,\"samples\":%llu}}", (unsigned long long)first,
                     (unsigned long long)k);
            }
            chan_restart(&r->ch, first + k * d);
            continue;
        }
        if (got <= 0)
            return; /* not written yet */
        budget -= got;
        const long fill = bl->p.iq_filled ? bl->p.iq_filled(bl->p.ctx, r->ch.sc, got) : 0;
        uint64_t first = 0;
        const int n = chan_run(&r->ch, bl->iq, got, r->y, &first);
        if (!n)
            continue;
        if (r->learning) {
            for (int i = 0; i < n && r->learning; i++) {
                r->bp += r->y[2 * i] * r->y[2 * i] + r->y[2 * i + 1] * r->y[2 * i + 1];
                if (++r->bn < r->bsize)
                    continue;
                const double db = 10 * log10(r->bp / r->bsize + 1e-30);
                r->bn = 0, r->bp = 0;
                const int b = (int)((db + 150) * 2);
                r->hist[b < 0 ? 0 : b >= TRIG_HIST ? TRIG_HIST - 1 : b]++;
                if (++r->blocks >= r->need_blocks && !srx_start(bl, c, first + (uint64_t)(i + 1) * d, now, &w))
                    return;
            }
            continue; /* the receiver takes the samples after the floor's */
        }
        if (!r->end_sc && !srx_start(bl, c, first, now, &w))
            return;
        int keep = n; /* nothing past the end */
        if (first + (uint64_t)(n - 1) * d >= r->end_sc)
            keep = first >= r->end_sc ? 0 : (int)((r->end_sc - first + d - 1) / d);
        if (fill > 0) {
            r->filled += fill;
            emit(bl, c, "{\"gap\":{\"sc\":%llu,\"filler\":%ld}}", (unsigned long long)first, fill);
        }
        if (!keep)
            continue;
        int nout[RDOF_SIG_STREAMS] = {0};
        r->m->rx_feed(r->rx, r->y, keep, r->out, nout);
        r->samples += keep;
        for (int s = 0; s < r->nst; s++) {
            if (nout[s] <= 0)
                continue;
            const float k = 32767.0f / (float)r->st[s].full;
            for (int i = 0; i < nout[s]; i++) {
                const float v = r->out[s][i] * k;
                r->pcm[i] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : rdof_bl_round(v));
            }
            char hdr[160];
            const int hn = snprintf(hdr, sizeof hdr, "{\"pcm\":{\"stream\":\"%s\",\"seq\":%lu,\"n\":%d},\"bytes\":%d}\n",
                                    r->st[s].name, r->seq++, nout[s], 2 * nout[s]);
            put_bytes(c, hdr, (size_t)hn);
            put_bytes(c, r->pcm, (size_t)(2 * nout[s]));
        }
        srx_records(bl, c);
    }
}

static void request(rdof_bl_t *bl, int ci, double now)
{
    conn_t *c = &bl->c[ci];
    bl->doc = (rdof_jdoc_t){.nodes = bl->nodes, .max = NODES, .buf = bl->strbuf, .cap = RDOF_BL_LINE_MAX + 64};
    const int root = rdof_jparse(&bl->doc, c->line, c->ln);
    const rdof_jnode_t *r = rdof_jnode(&bl->doc, root);
    if (!r || r->type != RDOF_JOBJ) {
        fail(bl, c, "bad_request", "the request isn't a JSON object: %s", bl->doc.error);
        return;
    }
    const char *op = jstrv(bl, root, "op");
    refresh_freqs(bl);
    c->state = C_DONE; /* each op below says otherwise */
    if (!op)
        fail(bl, c, "bad_request", "no op");
    else if (!strcmp(op, "info"))
        op_info(bl, c, now);
    else if (!strcmp(op, "status"))
        op_status(bl, c, now);
    else if (!strcmp(op, "session"))
        op_session(bl, c, ci, root, now);
    else if (!strcmp(op, "claim"))
        op_claim(bl, c, root, now);
    else if (!strcmp(op, "release"))
        op_release(bl, c, root);
    else if (!strcmp(op, "hold"))
        op_hold(bl, c, root, now);
    else if (!strcmp(op, "bursts"))
        op_bursts(bl, c, root, now, false);
    else if (!strcmp(op, "listen"))
        op_bursts(bl, c, root, now, true);
    else if (!strcmp(op, "capture"))
        op_capture(bl, c, root, now);
    else if (!strcmp(op, "run"))
        op_run(bl, c, ci, root, now);
    else if (!strcmp(op, "trigger"))
        op_trigger(bl, c, root, now);
    else if (!strcmp(op, "tx"))
        op_tx(bl, c, root, now);
    else if (!strcmp(op, "rx"))
        op_rx(bl, c, root, now);
    else if (!strcmp(op, "activity") || !strcmp(op, "burstlog") || !strcmp(op, "bridge"))
        fail(bl, c, "unsupported", "this bench has no %s yet", op);
    else
        fail(bl, c, "bad_request", "no op %s", op);
}

/* ── the tick ─────────────────────────────────────────────────────────────── */

/* The first waiter for the modem (the lowest seq), -1 none. */
static int modem_next(const rdof_bl_t *bl)
{
    int best = -1;
    for (int i = 0; i < RDOF_BL_CONNS; i++) {
        const conn_t *c = &bl->c[i];
        if (c->state == C_ACTIVE && c->wait == W_MODEM && (best < 0 || c->seq < bl->c[best].seq))
            best = i;
    }
    return best;
}

static void tell_queued(rdof_bl_t *bl, conn_t *c, double now, const char *kind, int blocker, int ahead)
{
    char blk[200] = "";
    if (blocker >= 0) {
        char ql[64];
        jstr(bl->s[blocker].label, ql, sizeof ql);
        snprintf(blk, sizeof blk, "{\"session\":\"%s\",\"label\":%s,\"kind\":\"%s\",\"since_s\":%.1f,"
                                  "\"remaining_s\":%.1f}",
                 bl->s[blocker].id, ql, kind, now - bl->s[blocker].since, expect_left(bl, now));
    }
    const double eta = expect_left(bl, now);
    emit(bl, c, "{\"state\":\"queued\",\"kind\":\"%s\",\"eta_s\":%.1f,\"worst_s\":%.1f,\"ahead\":%d,\"blockers\":[%s]}",
         kind, eta, c->wait_max, ahead, blk);
    c->queued_told = true;
}

static void granted(rdof_bl_t *bl, conn_t *c, double now, const char *kind)
{
    if (c->queued_told)
        emit(bl, c, "{\"state\":\"granted\",\"kind\":\"%s\",\"waited_s\":%.1f}", kind, now - c->wait_since);
    c->wait = W_NONE;
}

static void tick_waits(rdof_bl_t *bl, double now)
{
    for (int i = 0; i < RDOF_BL_CONNS; i++) {
        conn_t *c = &bl->c[i];
        if (c->state != C_ACTIVE || c->wait == W_NONE)
            continue;
        if (c->sess >= 0 && !bl->s[c->sess].used) {
            fail(bl, c, "refused", "the session ended");
            continue;
        }
        if (c->wait == W_FREQ) {
            int blocker = -1;
            for (int f = 0; f < c->nfreqs && blocker < 0; f++) {
                const int s = claimed_by(bl, c->freqs[f]);
                if (s >= 0 && s != c->sess)
                    blocker = s;
            }
            if (blocker < 0) {
                session_t *s = &bl->s[c->sess];
                for (int f = 0; f < c->nfreqs; f++)
                    if (claimed_by(bl, c->freqs[f]) != c->sess && s->nclaims < RDOF_BL_CLAIMS)
                        s->claims[s->nclaims++] = c->freqs[f];
                granted(bl, c, now, "channel");
                char list[256], win[320];
                size_t k = 0;
                for (int f = 0; f < c->nfreqs && k < sizeof list; f++)
                    k += (size_t)snprintf(list + k, sizeof list - k, "%s%.6f", f ? "," : "", c->freqs[f]);
                if (!c->nfreqs)
                    list[0] = 0;
                put_window(bl, win, sizeof win);
                emit(bl, c, "{\"claimed\":[%s],\"window\":%s}", list, win);
                c->state = C_DONE;
            } else if (!c->queued_told) {
                tell_queued(bl, c, now, "channel", blocker, 0);
            }
        } else if (c->wait == W_MODEM) {
            const bool mine = bl->holder >= 0 && bl->holder == c->sess;
            int blocker = -1;
            for (int f = 0; f < c->nfreqs && blocker < 0; f++) {
                const int s = claimed_by(bl, c->freqs[f]);
                if (s >= 0 && s != c->sess)
                    blocker = s;
            }
            if (((bl->holder < 0 && modem_next(bl) == i) || mine) && bl->tx_conn < 0 && blocker < 0) {
                granted(bl, c, now, "modem");
                if (c->op == OP_RUN)
                    run_granted(bl, c, i, now);
                if (c->op == OP_TX)
                    stx_granted(bl, c, i, now);
                if (c->op == OP_HOLD) {
                    session_t *s = &bl->s[c->sess];
                    s->hold = (int)(++bl->next_seq);
                    s->hold_since = now;
                    s->hold_expect = c->expect_s;
                    bl->holder = c->sess;
                    emit(bl, c, "{\"held\":%d}", s->hold);
                    c->state = C_DONE;
                }
            } else if (!c->queued_told) {
                int ahead = 0;
                for (int j = 0; j < RDOF_BL_CONNS; j++)
                    if (bl->c[j].state == C_ACTIVE && bl->c[j].wait == W_MODEM && bl->c[j].seq < c->seq)
                        ahead++;
                tell_queued(bl, c, now, "modem", bl->holder, ahead);
            }
        }
        if (c->state == C_ACTIVE && c->wait != W_NONE && now - c->wait_since > c->wait_max)
            fail(bl, c, "timeout", "waited %.0f s for the %s", now - c->wait_since,
                 c->wait == W_MODEM ? "modem" : "channel");
    }
}

static const char *kind_name(int dt, int n, char tmp[12])
{
    static const char *const names[] = {"PI_HDR", "VLC_HDR", "TERM_LC", "CSBK", "MBC_HDR", "MBC_CONT",
                                        "DATA_HDR", "R1/2", "R3/4", "IDLE", "R1", "USBD"};
    if (dt == RDOF_BL_VOICE_SYNC || dt == RDOF_BL_VOICE) {
        snprintf(tmp, 12, "VOICE_%c", 'A' + (n >= 0 && n < 6 ? n : 0));
        return tmp;
    }
    if (dt >= 0 && dt < 12)
        return names[dt];
    snprintf(tmp, 12, "dt%d", dt);
    return tmp;
}

/* 0-7 BS/MS/TS1/TS2 voice/data, 8 EMB */
static int sync_index(int family, int dt)
{
    if (dt == RDOF_BL_VOICE)
        return 8;
    if (family < 0 || family > 3)
        return -1;
    return family * 2 + (dt == RDOF_BL_VOICE_SYNC ? 0 : 1);
}

static const char *const SYNC_NAMES[9] = {"BS_VOICE", "BS_DATA", "MS_VOICE", "MS_DATA", "TS1_VOICE",
                                          "TS1_DATA", "TS2_VOICE", "TS2_DATA", "EMB"};

static void burst_out(rdof_bl_t *bl, conn_t *c, const rdof_bl_burst_t *b, double now)
{
    const bool voice = b->dt == RDOF_BL_VOICE_SYNC || b->dt == RDOF_BL_VOICE;
    if (c->cc >= 0 && voice && b->cc >= 0 && b->cc != c->cc)
        return;
    /* t and sc at the sync's center (bench-api.md 6.3): 65.5 symbols past the burst's start */
    const double t = b->t + SYNC_CENTER_48 / 48000.0;
    char sc[32] = "null";
    if (c->fs > 0)
        snprintf(sc, sizeof sc, "%lld", (long long)llround((double)(b->pos48 + SYNC_CENTER_48) * c->fs / 48000.0));
    if (isnan(c->first_t))
        c->first_t = t;
    const double rel = 1e3 * (t - c->first_t);
    const long gn = lround(rel / SLOT_MS);
    double gerr = rel - gn * SLOT_MS;
    if (fabs(gerr) < 0.0005)
        gerr = 0; /* not -0.000 */
    char kt[12], prev[24] = "null", hex[67];
    if (!isnan(c->prev_t))
        snprintf(prev, sizeof prev, "%.3f", 1e3 * (t - c->prev_t));
    for (int i = 0; i < 33; i++)
        snprintf(hex + 2 * i, 3, "%02x", b->raw[i]);
    const int si = sync_index(b->family, b->dt);
    const char *kind = kind_name(b->dt, b->n, kt);
    char *dec = bl->bdec, *line = bl->bline;
    rdof_bj_burst_decode(&c->emb, b, dec, sizeof bl->bdec);
    snprintf(line, sizeof bl->bline,
             "{\"burst\":{\"t\":%.6f,\"sc\":%s,\"t_rel_ms\":%.3f,\"dt_prev_ms\":%s,\"grid_n\":%ld,\"grid_err_ms\":%.3f,"
             "\"slot_ab\":\"%c\",\"sync\":\"%s\",\"sync_errors\":%d,\"slot\":%d,\"voice\":%s,\"kind\":\"%s\","
             "\"dt\":%d,\"n\":%d,\"cc\":%d,\"cfo_hz\":%.1f,\"level_dbfs\":%.1f,\"dev_hz\":%.1f,\"freq\":%.1f%s,"
             "\"raw_hex\":\"%s\"}}",
             t, sc, rel, prev, gn, gerr, gn % 2 == 0 ? 'A' : 'B', si >= 0 ? SYNC_NAMES[si] : "?",
             b->sync_errors, b->slot, voice ? "true" : "false", kind, b->dt, b->n, b->cc, b->cfo_hz,
             b->level_dbfs, b->dev_hz, c->freq_hz, dec, hex);
    if (!emit(bl, c, "%s", line)) {
        c->dropped++;
        return;
    }
    c->prev_t = c->last_t = t;
    c->last_mono = now;
    c->sent++;
    c->cfo_sum += b->cfo_hz, c->lvl_sum += b->level_dbfs;
    c->cfo_min = fmin(c->cfo_min, b->cfo_hz), c->cfo_max = fmax(c->cfo_max, b->cfo_hz);
    c->lvl_max = fmax(c->lvl_max, b->level_dbfs);
    if (voice)
        c->kinds[12 + (b->n >= 0 && b->n < 6 ? b->n : 0)]++;
    else if (b->dt < 12)
        c->kinds[b->dt]++;
    if (si >= 0)
        c->syncs[si]++;
}

static void bursts_done(rdof_bl_t *bl, conn_t *c, const char *reason, double now)
{
    if (c->op == OP_LISTEN) {
        c->state = C_DONE;
        return;
    }
    char kinds[400] = "", syncs[200] = "", kt[12];
    size_t k = 0, m = 0;
    for (int i = 0; i < 18; i++)
        if (c->kinds[i] && k < sizeof kinds)
            k += (size_t)snprintf(kinds + k, sizeof kinds - k, "%s\"%s\":%d", k ? "," : "",
                                  i < 12 ? kind_name(i, 0, kt) : kind_name(RDOF_BL_VOICE, i - 12, kt), c->kinds[i]);
    for (int i = 0; i < 9; i++)
        if (c->syncs[i] && m < sizeof syncs)
            m += (size_t)snprintf(syncs + m, sizeof syncs - m, "%s\"%s\":%d", m ? "," : "", SYNC_NAMES[i], c->syncs[i]);
    char more[400] = "";
    if (c->sent)
        snprintf(more, sizeof more,
                 ",\"first_t\":%.6f,\"span_ms\":%.3f,\"cfo_hz\":{\"mean\":%.1f,\"min\":%.1f,\"max\":%.1f},"
                 "\"level_dbfs\":{\"mean\":%.1f,\"max\":%.1f}",
                 c->first_t, 1e3 * (c->last_t - c->first_t), c->cfo_sum / c->sent, c->cfo_min, c->cfo_max,
                 c->lvl_sum / c->sent, c->lvl_max);
    emit(bl, c,
         "{\"done\":{\"bursts\":%ld,\"reason\":\"%s\",\"freq\":%.1f,\"kinds\":{%s},\"syncs\":{%s},"
         "\"seconds_watched\":%.2f,\"dropped\":%ld%s}}",
         c->sent, reason, c->freq_hz, kinds, syncs, now - c->opened, c->dropped, more);
    c->state = C_DONE;
}

static void tick_bursts(rdof_bl_t *bl, double now)
{
    while (bl->qn) {
        const rdof_bl_burst_t *b = &bl->q[bl->qh];
        for (int i = 0; i < RDOF_BL_CONNS; i++) {
            conn_t *c = &bl->c[i];
            if (c->state == C_ACTIVE && c->ch == b->ch &&
                (c->op == OP_BURSTS || (c->op == OP_LISTEN && b->dt != 9 /* idle: a repeater's hang */)))
                burst_out(bl, c, b, now);
            else if (c->state == C_ACTIVE && c->op == OP_RUN && c->job && c->job->listening && c->ch == b->ch &&
                     b->dt != 9)
                run_heard(bl, c, b);
        }
        bl->qh = (bl->qh + 1) % RDOF_BL_BURSTQ;
        bl->qn--;
    }
    for (int i = 0; i < RDOF_BL_CONNS; i++) {
        conn_t *c = &bl->c[i];
        if (c->state != C_ACTIVE || (c->op != OP_BURSTS && c->op != OP_LISTEN))
            continue;
        if (c->max && c->sent >= c->max)
            bursts_done(bl, c, "bursts", now);
        else if (c->until_end && c->sent && now - c->last_mono > UNTIL_END_S + 0.1)
            bursts_done(bl, c, "ended", now);
        else if (c->until && now >= c->until)
            bursts_done(bl, c, "time", now);
    }
}

static void tick_capture(rdof_bl_t *bl, conn_t *c, double now)
{
    rdof_bl_window_t w;
    get_window(bl, &w);
    for (;;) {
        if (c->sc >= c->sc_end) {
            const double secs = (double)(c->sc_end - c->sc0) / (w.fs > 0 ? w.fs : 1);
            const double t0 = bl->p.sc_utc ? bl->p.sc_utc(bl->p.ctx, c->sc0) : now - secs;
            emit(bl, c,
                 "{\"done\":{\"freq\":%.1f,\"center\":%.1f,\"fs\":%.1f,\"fmt\":\"cs8\",\"gain\":%.1f,\"ppm\":%.3f,"
                 "\"t0\":%.6f,\"sc0\":%llu,\"seconds\":%.3f,\"samples\":%lld,\"overflows\":%ld,\"gap_samples\":%ld,"
                 "\"clip_pct\":%.3f,\"pre_s\":%.3f,\"scale_db\":%.2f,\"source\":\"rfg\"}}",
                 c->freq_hz, w.center_hz, w.fs, c->gain, w.ppm, t0, (unsigned long long)c->sc0, secs,
                 (long long)(c->sc_end - c->sc0), c->gaps ? 1L : 0L, c->gaps,
                 c->sent_iq ? 100.0 * (double)c->clipped / (double)(2 * c->sent_iq) : 0.0, c->pre_s, w.iq_scale_db);
            c->state = C_DONE;
            return;
        }
        long want = (long)(c->sc_end - c->sc);
        if (want > RDOF_BL_IQ_CHUNK)
            want = RDOF_BL_IQ_CHUNK;
        if (room(c) < (size_t)(2 * want + 160))
            return; /* the client is behind: later */
        uint64_t oldest = 0, next = 0;
        long got = bl->p.iq_read(bl->p.ctx, c->sc, bl->iq, want, &oldest, &next);
        unsigned flags = 0;
        if (got <= 0 && c->sc < oldest) { /* overwritten before it went: skip ahead */
            const uint64_t skip = (oldest < c->sc_end ? oldest : c->sc_end) - c->sc;
            c->gaps += (long)skip;
            c->sc += skip;
            c->gapped = true;
            continue;
        }
        if (got <= 0)
            return; /* not written yet */
        /* filler in the history (the front end lost samples, zeros kept the count): a gap too */
        const long fill = bl->p.iq_filled ? bl->p.iq_filled(bl->p.ctx, c->sc, got) : 0;
        c->gaps += fill;
        if (fill > 0 || c->gapped)
            flags |= 1;
        c->gapped = false;
        for (long i = 0; i < 2 * got; i++)
            if (bl->iq[i] == 127 || bl->iq[i] == -128 || bl->iq[i] == -127)
                c->clipped++;
        char hdr[160];
        const int hn = snprintf(hdr, sizeof hdr, "{\"iq\":{\"seq\":%lu,\"n\":%ld,\"sc\":%llu,\"flags\":%u},\"bytes\":%ld}\n",
                                (unsigned long)c->iseq++, got, (unsigned long long)c->sc, flags, 2 * got);
        put_bytes(c, hdr, (size_t)hn);
        put_bytes(c, bl->iq, (size_t)(2 * got));
        c->sc += (uint64_t)got;
        c->sent_iq += got;
    }
}

static double trig_utc(const rdof_bl_t *bl, uint64_t sc, double now, uint64_t next, double fs)
{
    return bl->p.sc_utc ? bl->p.sc_utc(bl->p.ctx, sc) : now - ((double)next - (double)sc) / fs;
}

/* A 1 ms block's power is in: the floor, the rise, the end. True when it fired (the channel restarts). */
static bool trig_block(rdof_bl_t *bl, conn_t *c, double lin, const rdof_bl_window_t *w)
{
    trig_t *t = c->trig;
    const double db = 10 * log10(lin + 1e-30);
    if (!t->have_floor) { /* the floor: the median block */
        int k = (int)((db + 150) * 2);
        t->hist[k < 0 ? 0 : k >= TRIG_HIST ? TRIG_HIST - 1 : k]++;
        if (++t->floor_blocks >= t->floor_s * 1000) {
            long acc = 0;
            int m = 0;
            while (m < TRIG_HIST - 1 && (acc += t->hist[m]) <= t->floor_blocks / 2)
                m++;
            t->floor_db = -150 + m * 0.5 + 0.25;
            t->have_floor = 1;
            emit(bl, c, "{\"armed\":{\"floor_db\":%.1f,\"rate\":%.1f}}", t->floor_db, t->rate);
        }
        return false;
    }
    const bool over = db >= t->floor_db + t->thr;
    if (t->fired) {
        if (t->until_end && t->block_sc >= t->trig_sc && db >= t->floor_db + t->thr / 2) {
            const uint64_t cap = t->trig_sc + (uint64_t)(t->max_s * w->fs);
            uint64_t e = t->block_sc + (uint64_t)t->bsize * (uint64_t)t->d + (uint64_t)(t->post_s * w->fs);
            if (e > cap)
                e = cap;
            if (e > t->end_sc)
                t->end_sc = e;
        }
        return false;
    }
    if (!over) {
        t->run = 0;
        return false;
    }
    if (!t->run++)
        t->run_sc = t->block_sc, t->run_p = 0;
    t->run_p += lin;
    if (t->run < t->min_ms)
        return false;
    /* fired: back to pre_s before the rise, as far as the history goes */
    t->fired = true;
    t->trig_sc = t->run_sc;
    t->level_db = 10 * log10(t->run_p / t->run + 1e-30);
    uint64_t oldest = 0, next = 0;
    int8_t probe[2];
    bl->p.iq_read(bl->p.ctx, UINT64_MAX, probe, 0, &oldest, &next);
    const uint64_t pre = (uint64_t)(t->pre_s * w->fs), lo = oldest + (uint64_t)t->ntaps;
    t->out_sc0 = t->trig_sc > lo + pre ? t->trig_sc - pre : lo < t->trig_sc ? lo : t->trig_sc;
    t->end_sc = t->trig_sc + (uint64_t)(t->post_s * w->fs);
    trig_restart(t, t->out_sc0);
    return true;
}

static void tick_trigger(rdof_bl_t *bl, conn_t *c, double now)
{
    trig_t *t = c->trig;
    rdof_bl_window_t w;
    get_window(bl, &w);
    const uint64_t g = (uint64_t)(t->ntaps - 1) / 2, d = (uint64_t)t->d;
    /* twice the input since the last tick, so it catches up, at most 0.25 s of it: the lock is held */
    double dt = t->last_tick > 0 ? now - t->last_tick : 0.02;
    t->last_tick = now;
    dt = dt < 0.02 ? 0.02 : dt > 0.125 ? 0.125 : dt;
    long budget = (long)(2 * dt * w.fs) + RDOF_BL_IQ_CHUNK;
    while (budget > 0) {
        if (t->fired && t->next_out - g >= t->end_sc) {
            uint64_t oldest = 0, next = 0;
            int8_t probe[2];
            bl->p.iq_read(bl->p.ctx, UINT64_MAX, probe, 0, &oldest, &next);
            const long samples = (long)((t->end_sc - t->out_sc0 + d - 1) / d);
            const long gaps = t->gaps + (t->filled + (long)d - 1) / (long)d; /* filler, in the channel's samples */
            emit(bl, c,
                 "{\"done\":{\"freq\":%.1f,\"center\":%.1f,\"fs\":%.3f,\"rate\":%.3f,\"wide_fs\":%.1f,"
                 "\"fmt\":\"%s\",\"bw\":%.0f,\"gain\":%.1f,\"ppm\":%.3f,\"t0\":%.6f,\"trigger_time\":%.6f,"
                 "\"signal_start_sample\":%llu,\"trigger_sc\":%llu,\"level_db\":%.1f,\"floor_db\":%.1f,"
                 "\"snr_db\":%.1f,\"threshold_db\":%.1f,\"seconds\":%.3f,\"samples\":%ld,\"pre_s\":%.3f,"
                 "\"gap_samples\":%ld,\"overflows\":%ld,\"clip_pct\":%.3f,\"scale_db\":%.2f,\"source\":\"rfg\"}}",
                 c->freq_hz, w.center_hz, t->rate, t->rate, w.fs, t->cs16 ? "cs16" : "cf32", t->bw, c->gain, w.ppm,
                 trig_utc(bl, t->out_sc0, now, next, w.fs), trig_utc(bl, t->trig_sc, now, next, w.fs),
                 (unsigned long long)((t->trig_sc - t->out_sc0) / d), (unsigned long long)t->trig_sc, t->level_db,
                 t->floor_db, t->level_db - t->floor_db, t->thr, samples / t->rate, samples,
                 (double)(t->trig_sc - t->out_sc0) / w.fs, gaps, gaps ? 1L : 0L,
                 t->inputs ? 100.0 * (double)t->clipped / (double)(2 * t->inputs) : 0.0, w.iq_scale_db);
            c->state = C_DONE;
            return;
        }
        if (!t->fired && now - t->started > t->timeout) {
            if (t->have_floor)
                fail(bl, c, "no_signal", "nothing rose %.0f dB over the floor (%.1f dB) in %.0f s", t->thr,
                     t->floor_db, t->timeout);
            else
                fail(bl, c, "no_signal", "no samples to learn the floor from in %.0f s", t->timeout);
            return;
        }
        const size_t frame = (size_t)(RDOF_BL_IQ_CHUNK / t->d + 2) * 8 + 160;
        if (t->fired && room(c) < frame)
            return; /* the client is behind: later */
        uint64_t oldest = 0, next = 0;
        const long got = bl->p.iq_read(bl->p.ctx, t->sc, bl->iq, RDOF_BL_IQ_CHUNK, &oldest, &next);
        if (got <= 0 && t->sc < oldest) { /* overwritten before it went: on from the oldest, the outputs between lost */
            if (!t->fired) {
                trig_restart(t, oldest + g);
                continue;
            }
            const uint64_t want = oldest + g, first = t->next_out - g;
            const uint64_t k = want > first ? (want - first + d - 1) / d : 0;
            t->gaps += (long)k;
            trig_restart(t, first + k * d);
            continue;
        }
        if (got <= 0)
            return; /* not written yet */
        budget -= got;
        if (t->fired && bl->p.iq_filled) { /* filler in the inputs of what it sends: a gap there */
            const uint64_t a = t->sc > t->out_sc0 ? t->sc : t->out_sc0, e = t->sc + (uint64_t)got;
            const uint64_t b = e < t->end_sc ? e : t->end_sc;
            if (b > a)
                t->filled += bl->p.iq_filled(bl->p.ctx, a, (long)(b - a));
        }
        const long idx0 = t->fired ? (long)((t->next_out - g - t->out_sc0) / d) : 0;
        /* the mixer in float (an ESP32's FPU has no double), its phase exact again each chunk */
        const double ph = fmod(t->step * (double)t->sc, 2 * M_PI);
        float pr = (float)cos(ph), pq = (float)sin(ph);
        const float rr = (float)cos(t->step), rq = (float)sin(t->step);
        const float k127 = 1.0f / 127;
        int nout = 0;
        bool restarted = false;
        for (long i = 0; i < got; i++) {
            const int8_t xi8 = bl->iq[2 * i], xq8 = bl->iq[2 * i + 1];
            if (t->fired) {
                t->inputs++;
                t->clipped += (xi8 >= 127 || xi8 <= -127) + (xq8 >= 127 || xq8 <= -127);
            }
            const float xi = xi8 * k127, xq = xq8 * k127;
            float *a = t->dl + 2 * t->di, *b = a + 2 * t->ntaps;
            a[0] = b[0] = xi * pr - xq * pq;
            a[1] = b[1] = xi * pq + xq * pr;
            const float npr = pr * rr - pq * rq;
            pq = pr * rq + pq * rr, pr = npr;
            if (++t->di == t->ntaps)
                t->di = 0;
            if (t->sc + (uint64_t)i != t->next_out)
                continue;
            t->next_out += d;
            const float *x = t->dl + 2 * t->di; /* oldest to newest (the taps are symmetric) */
            float yi = 0, yq = 0;
            for (int k = 0; k < t->ntaps; k++)
                yi += t->taps[k] * x[2 * k], yq += t->taps[k] * x[2 * k + 1];
            const uint64_t center = t->sc + (uint64_t)i - g;
            if (t->fired && center < t->end_sc)
                t->y[2 * nout] = yi, t->y[2 * nout + 1] = yq, nout++;
            if (!t->bn++)
                t->block_sc = center;
            t->bp += yi * yi + yq * yq;
            if (t->bn == t->bsize) {
                const double lin = t->bp / t->bsize;
                t->bn = 0, t->bp = 0;
                if (trig_block(bl, c, lin, &w)) {
                    restarted = true;
                    break;
                }
            }
        }
        if (restarted)
            continue;
        t->sc += (uint64_t)got;
        if (!nout)
            continue;
        size_t nb = (size_t)nout * 8;
        if (t->cs16) {
            int16_t *o = (int16_t *)t->y;
            for (int k = 0; k < 2 * nout; k++) {
                const float v = t->y[k] * 32767.0f;
                o[k] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : lrintf(v));
            }
            nb = (size_t)nout * 4;
        }
        char hdr[160];
        const int hn = snprintf(hdr, sizeof hdr, "{\"iq\":{\"seq\":%lu,\"n\":%d,\"sc\":%ld,\"flags\":0},\"bytes\":%zu}\n",
                                (unsigned long)c->iseq++, nout, idx0, nb);
        put_bytes(c, hdr, (size_t)hn);
        put_bytes(c, t->y, nb);
        t->sent += nout;
    }
}

void rdof_bl_tick(rdof_bl_t *bl, double now)
{
    lock(bl);
    tick_waits(bl, now);
    tick_bursts(bl, now);
    for (int i = 0; i < RDOF_BL_CONNS; i++) {
        conn_t *c = &bl->c[i];
        if (c->state == C_ACTIVE && c->op == OP_CAPTURE)
            tick_capture(bl, c, now);
        else if (c->state == C_ACTIVE && c->op == OP_TRIGGER)
            tick_trigger(bl, c, now);
        else if (c->state == C_ACTIVE && c->op == OP_RUN && c->job && c->wait == W_NONE)
            tick_run(bl, c, i, now);
        else if (c->state == C_ACTIVE && c->op == OP_TX && c->stx)
            tick_tx(bl, c, now);
        else if (c->state == C_ACTIVE && c->op == OP_RX && c->srx)
            tick_rx(bl, c, now);
        else if (c->state == C_ACTIVE && c->op == OP_SESSION && c->ends > 0 && now >= c->ends && c->sess >= 0)
            session_end(bl, c->sess, "max_s");
        else if (c->state == C_LINE && now - c->opened > 30.0)
            fail(bl, c, "bad_request", "no request in 30 s");
        if (c->state == C_DONE && (c->op == OP_BURSTS || c->op == OP_LISTEN || c->op == OP_CAPTURE ||
                                   c->op == OP_TRIGGER || c->op == OP_RX)) {
            if (c->op == OP_CAPTURE || c->op == OP_TRIGGER || c->op == OP_RX)
                bl->n_iq--;
            else
                bl->n_bursts--;
            c->op = OP_NONE; /* counted out once */
        }
        if (c->state == C_DONE && c->job)
            job_end(bl, c);
        if (c->state == C_DONE && (c->stx || c->srx))
            sig_end(bl, c);
    }
    unlock(bl);
}

/* ── I/Q formats ──────────────────────────────────────────────────────────── */

int rdof_bl_iq_convert(const float *iq, int n, int fmt, float k, uint8_t *out)
{
    if (fmt == RDOF_BL_IQ_CF32) {
        memcpy(out, iq, (size_t)n * 8);
        return 0;
    }
    int clipped = 0;
    const float scale = fmt == RDOF_BL_IQ_CS16 ? 32767.0f : 127.0f, g = fmt == RDOF_BL_IQ_CS16 ? scale : scale * k;
    for (int i = 0; i < 2 * n; i++) {
        float x = iq[i] * g;
        if (x > scale) {
            x = scale;
            clipped++;
        } else if (x < -scale) {
            x = -scale;
            clipped++;
        }
        const int q = rdof_bl_round(x); /* lrintf cost a W12 a tenth of a core (lr2021-iq.md R2.37) */
        if (fmt == RDOF_BL_IQ_CS16) {
            out[2 * (size_t)i] = (uint8_t)(uint16_t)(int16_t)q; /* little-endian */
            out[2 * (size_t)i + 1] = (uint8_t)((uint16_t)(int16_t)q >> 8);
        } else
            out[i] = (uint8_t)(fmt == RDOF_BL_IQ_CU8 ? q + 128 : (int8_t)q);
    }
    return clipped;
}

double rdof_bl_iq_scale_db(int fmt, float k)
{
    return (fmt == RDOF_BL_IQ_CS8 || fmt == RDOF_BL_IQ_CU8) && k > 0 ? 20.0 * log10(k) : 0.0;
}

/* ── filler in a history ──────────────────────────────────────────────────── */

void rdof_bl_fills_reset(rdof_bl_fills_t *f) { memset(f, 0, sizeof *f); }

void rdof_bl_fills_note(rdof_bl_fills_t *f, uint64_t sc, long n)
{
    if (n <= 0)
        return;
    if (f->count) {
        const int last = (f->head + f->count - 1) % RDOF_BL_FILLS;
        if (f->from[last] + f->n[last] == sc && f->n[last] <= UINT32_MAX - (uint64_t)n) {
            f->n[last] += (uint32_t)n;
            return;
        }
    }
    if (f->count == RDOF_BL_FILLS) { /* no room: the oldest goes */
        f->head = (f->head + 1) % RDOF_BL_FILLS;
        f->count--;
    }
    const int i = (f->head + f->count) % RDOF_BL_FILLS;
    f->from[i] = sc;
    f->n[i] = (uint64_t)n > UINT32_MAX ? UINT32_MAX : (uint32_t)n; /* not (long)UINT32_MAX: -1 where long is 32 bits */
    f->count++;
}

long rdof_bl_fills_count(const rdof_bl_fills_t *f, uint64_t sc, long n)
{
    if (n <= 0)
        return 0;
    const uint64_t end = sc + (uint64_t)n;
    long k = 0;
    for (int j = 0; j < f->count; j++) {
        const int i = (f->head + j) % RDOF_BL_FILLS;
        const uint64_t a = f->from[i] > sc ? f->from[i] : sc, e = f->from[i] + f->n[i];
        const uint64_t b = e < end ? e : end;
        if (b > a)
            k += (long)(b - a);
    }
    return k;
}

/* ── the history ring ─────────────────────────────────────────────────────── */

bool rdof_bl_ring_init(rdof_bl_ring_t *r, long n)
{
    r->buf = malloc((size_t)n * 2);
    r->n = r->buf ? n : 0;
    r->w = 0;
    r->scale = 1.0f;
    rdof_bl_fills_reset(&r->fills);
    return r->buf != NULL;
}

void rdof_bl_ring_mark_filler(rdof_bl_ring_t *r, long n)
{
    if (n > 0 && (uint64_t)n <= r->w)
        rdof_bl_fills_note(&r->fills, r->w - (uint64_t)n, n);
}

long rdof_bl_ring_filled(const rdof_bl_ring_t *r, uint64_t sc, long n) { return rdof_bl_fills_count(&r->fills, sc, n); }

void rdof_bl_ring_free(rdof_bl_ring_t *r)
{
    free(r->buf);
    r->buf = NULL;
    r->n = 0;
}

static inline int8_t sat8(float x, float k)
{
    float v = x * k;
    v = v > 127.0f ? 127.0f : v < -128.0f ? -128.0f : v;
    /* to the nearest, ties to even, as lrintf without its call (a PlutoSDR spent a fifth of a core on it): adding
     * 1.5 * 2^23 leaves no fraction */
    return (int8_t)(int)((v + 12582912.0f) - 12582912.0f);
}

void rdof_bl_ring_put(rdof_bl_ring_t *r, const float *iq, long n, long *clipped)
{
    if (!r->buf)
        return;
    long k = (long)(r->w % (uint64_t)r->n);
    long clip = 0;
    const float g = 127.0f * r->scale;
    for (long i = 0; i < n;) { /* up to the ring's end, then from its start */
        const long m = n - i < r->n - k ? n - i : r->n - k;
        int8_t *o = r->buf + 2 * k;
        const float *x = iq + 2 * i;
        for (long j = 0; j < 2 * m; j++) {
            o[j] = sat8(x[j], g);
            clip += o[j] == 127 || o[j] == -128;
        }
        i += m, k += m;
        if (k == r->n)
            k = 0;
    }
    __atomic_store_n(&r->w, r->w + (uint64_t)n, __ATOMIC_RELEASE);
    if (clipped)
        *clipped += clip;
}

long rdof_bl_ring_read(const rdof_bl_ring_t *r, uint64_t sc, int8_t *out, long n, uint64_t *oldest, uint64_t *next)
{
    const uint64_t w = __atomic_load_n(&r->w, __ATOMIC_ACQUIRE);
    /* a margin of a tenth of the ring: the writer may be filling it while this copies */
    const uint64_t keep = r->n > 10 ? (uint64_t)(r->n - r->n / 10) : 0;
    const uint64_t old = w > keep ? w - keep : 0;
    if (oldest)
        *oldest = old;
    if (next)
        *next = w;
    if (!r->buf || n <= 0 || sc < old || sc >= w)
        return 0;
    if ((uint64_t)n > w - sc)
        n = (long)(w - sc);
    long k = (long)(sc % (uint64_t)r->n);
    for (long i = 0; i < n; i++) {
        out[2 * i] = r->buf[2 * k], out[2 * i + 1] = r->buf[2 * k + 1];
        if (++k == r->n)
            k = 0;
    }
    return n;
}
