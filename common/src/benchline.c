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

#define NODES 384
#define WAIT_MAX_S 900.0       /* a wait with no wait_max */
#define UNTIL_END_S 0.3        /* bursts --until-end: quiet this long after a burst ends the watch */
#define SLOT_MS 30.0

enum { C_FREE = 0, C_LINE, C_ACTIVE, C_DONE };
enum { OP_NONE = 0, OP_SESSION, OP_CLAIM, OP_HOLD, OP_RUN, OP_BURSTS, OP_LISTEN, OP_CAPTURE };
enum { W_NONE = 0, W_MODEM, W_FREQ };
/* a run's job */
enum { J_WAIT = 0, J_OPEN, J_SEND, J_DRAIN, J_GAP, J_LISTEN };

typedef struct {
    rdof_bj_job_t job;
    uint8_t ambe[RDOF_BL_LINE_MAX / 2];
    rdof_bj_burst_t *b;       /* the key-up's bursts */
    int nb, bi, maxb, part, nparts;
    int state;
    double until, started, max_s;
    bool tx_open, listening;
    rdof_bj_listener_t lis;
    long id;
    char note[200], done[200];
    char esc[7000];           /* an out line, as a JSON string */
} job_t;

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
    long gaps, clipped, sent_iq;
    double freq_hz, pre_s, gain;
    /* run */
    job_t *job;
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

void rdof_bl_free(rdof_bl_t *bl)
{
    if (!bl)
        return;
    for (int i = 0; i < RDOF_BL_CONNS; i++) {
        free(bl->c[i].out);
        free(bl->c[i].line);
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

void rdof_bl_input(rdof_bl_t *bl, int ci, const char *data, size_t n, double now)
{
    if (ci < 0 || ci >= RDOF_BL_CONNS)
        return;
    lock(bl);
    conn_t *c = &bl->c[ci];
    for (size_t i = 0; i < n && c->state == C_LINE; i++) {
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
    /* After the request, the client sends nothing the engine reads; closing is what counts. */
    unlock(bl);
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
    if (c->op == OP_CAPTURE)
        bl->n_iq--;
    if (c->job)
        job_end(bl, c); /* a transmission unkeys */
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

static void put_window(rdof_bl_t *bl, char *out, size_t cap)
{
    rdof_bl_window_t w = {0};
    if (bl->p.window)
        bl->p.window(bl->p.ctx, &w);
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
    rdof_bl_window_t w = {0};
    if (bl->p.window)
        bl->p.window(bl->p.ctx, &w);
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
    if (k < sizeof line)
        k += (size_t)snprintf(line + k, sizeof line - k,
                              "],\"stations\":[{\"name\":\"modem\",\"kind\":\"sdrmodem\",\"model\":\"the gateway's SDR "
                              "modem\",\"port\":\"\",\"present\":%s,\"tx\":%s,\"why_not\":%s,\"modes\":[\"dmr\"],"
                              "\"bands\":[],\"held_by\":%s,\"waiting\":%s}],"
                              "\"modem\":{\"held_by\":%s,\"waiting\":%s,\"next_job_eta\":{\"eta_s\":%.1f,"
                              "\"worst_s\":%.1f}},\"default_caps\":[]}",
                              notx ? "false" : "true", notx ? "false" : "true", why, held, wq, held, wq, eta, eta);
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
    int captures = 0;
    for (int i = 0; i < RDOF_BL_CONNS; i++)
        if (bl->c[i].state == C_ACTIVE && bl->c[i].op == OP_CAPTURE)
            captures++;
    rdof_bl_window_t w = {0};
    if (bl->p.window)
        bl->p.window(bl->p.ctx, &w);
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
    c->state = C_ACTIVE;
    bl->n_bursts++;
    if (!listen)
        emit(bl, c, "{\"ready\":%.3f}", bl->p.utc ? bl->p.utc(bl->p.ctx) : now);
}

static void op_capture(rdof_bl_t *bl, conn_t *c, int root, double now)
{
    if (!req_session(bl, c, root, false))
        return;
    if (!bl->p.iq_read) {
        fail(bl, c, "unsupported", "this bench keeps no I/Q");
        return;
    }
    rdof_bl_window_t w = {0};
    if (bl->p.window)
        bl->p.window(bl->p.ctx, &w);
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
    uint8_t *big = malloc(RDOF_BL_OUT_IQ);
    if (!big) {
        fail(bl, c, "unavailable", "no memory for a capture");
        return;
    }
    /* the lines so far go along into the big buffer */
    memcpy(big, c->out + c->oh, c->ol);
    free(c->out);
    c->out = big, c->ocap = RDOF_BL_OUT_IQ, c->oh = 0;
    uint64_t oldest = 0, next = 0;
    int8_t probe[2];
    bl->p.iq_read(bl->p.ctx, UINT64_MAX, probe, 0, &oldest, &next);
    double pre = jnum(bl, root, "pre_s", 2.0);
    if (pre < 0)
        pre = 0;
    uint64_t pre_n = (uint64_t)(pre * w.fs);
    if (pre_n > next - oldest)
        pre_n = next - oldest;
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
    emit(bl, c, "{\"ready\":%.6f,\"shared\":true,\"center\":%.6f,\"pre_s\":%.3f,\"fs\":%.1f}", t0, w.center_hz / 1e6,
         c->pre_s, w.fs);
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
    switch (J->state) {
    case J_OPEN: {
        static const char *const CMDS[] = {"", "voice", "csbk", "data", "listen"};
        if (!bl->p.tx_open(bl->p.ctx, (uint32_t)lround(j->freq_mhz * 1e6), j->cc, j->power, why, sizeof why)) {
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
    else if (!strcmp(op, "trigger") || !strcmp(op, "activity") || !strcmp(op, "burstlog") ||
             !strcmp(op, "bridge"))
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
    if (isnan(c->first_t))
        c->first_t = b->t;
    const double rel = 1e3 * (b->t - c->first_t);
    const long gn = lround(rel / SLOT_MS);
    double gerr = rel - gn * SLOT_MS;
    if (fabs(gerr) < 0.0005)
        gerr = 0; /* not -0.000 */
    char kt[12], prev[24] = "null", hex[67];
    if (!isnan(c->prev_t))
        snprintf(prev, sizeof prev, "%.3f", 1e3 * (b->t - c->prev_t));
    for (int i = 0; i < 33; i++)
        snprintf(hex + 2 * i, 3, "%02x", b->raw[i]);
    const int si = sync_index(b->family, b->dt);
    const char *kind = kind_name(b->dt, b->n, kt);
    char dec[1200], line[2048];
    rdof_bj_burst_decode(&c->emb, b, dec, sizeof dec);
    snprintf(line, sizeof line,
             "{\"burst\":{\"t\":%.6f,\"t_rel_ms\":%.3f,\"dt_prev_ms\":%s,\"grid_n\":%ld,\"grid_err_ms\":%.3f,"
             "\"slot_ab\":\"%c\",\"sync\":\"%s\",\"sync_errors\":%d,\"slot\":%d,\"voice\":%s,\"kind\":\"%s\","
             "\"dt\":%d,\"n\":%d,\"cc\":%d,\"cfo_hz\":%.1f,\"level_dbfs\":%.1f,\"dev_hz\":%.1f,\"freq\":%.1f%s,"
             "\"raw_hex\":\"%s\"}}",
             b->t, rel, prev, gn, gerr, gn % 2 == 0 ? 'A' : 'B', si >= 0 ? SYNC_NAMES[si] : "?",
             b->sync_errors, b->slot, voice ? "true" : "false", kind, b->dt, b->n, b->cc, b->cfo_hz,
             b->level_dbfs, b->dev_hz, c->freq_hz, dec, hex);
    if (!emit(bl, c, "%s", line)) {
        c->dropped++;
        return;
    }
    c->prev_t = c->last_t = b->t;
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
            if (c->state == C_ACTIVE && (c->op == OP_BURSTS || c->op == OP_LISTEN) && c->ch == b->ch &&
                b->dt != 9 /* idle */)
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
    rdof_bl_window_t w = {0};
    if (bl->p.window)
        bl->p.window(bl->p.ctx, &w);
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
            continue;
        }
        if (got <= 0)
            return; /* not written yet */
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

void rdof_bl_tick(rdof_bl_t *bl, double now)
{
    lock(bl);
    tick_waits(bl, now);
    tick_bursts(bl, now);
    for (int i = 0; i < RDOF_BL_CONNS; i++) {
        conn_t *c = &bl->c[i];
        if (c->state == C_ACTIVE && c->op == OP_CAPTURE)
            tick_capture(bl, c, now);
        else if (c->state == C_ACTIVE && c->op == OP_RUN && c->job && c->wait == W_NONE)
            tick_run(bl, c, i, now);
        else if (c->state == C_ACTIVE && c->op == OP_SESSION && c->ends > 0 && now >= c->ends && c->sess >= 0)
            session_end(bl, c->sess, "max_s");
        else if (c->state == C_LINE && now - c->opened > 30.0)
            fail(bl, c, "bad_request", "no request in 30 s");
        if (c->state == C_DONE && (c->op == OP_BURSTS || c->op == OP_LISTEN || c->op == OP_CAPTURE)) {
            if (c->op == OP_CAPTURE)
                bl->n_iq--;
            else
                bl->n_bursts--;
            c->op = OP_NONE; /* counted out once */
        }
        if (c->state == C_DONE && c->job)
            job_end(bl, c);
    }
    unlock(bl);
}

/* ── the history ring ─────────────────────────────────────────────────────── */

bool rdof_bl_ring_init(rdof_bl_ring_t *r, long n)
{
    r->buf = malloc((size_t)n * 2);
    r->n = r->buf ? n : 0;
    r->w = 0;
    r->scale = 1.0f;
    return r->buf != NULL;
}

void rdof_bl_ring_free(rdof_bl_ring_t *r)
{
    free(r->buf);
    r->buf = NULL;
    r->n = 0;
}

static inline int8_t sat8(float x, float k)
{
    const float v = x * k;
    return v >= 127.0f ? 127 : v <= -128.0f ? -128 : (int8_t)lrintf(v);
}

void rdof_bl_ring_put(rdof_bl_ring_t *r, const float *iq, long n, long *clipped)
{
    if (!r->buf)
        return;
    long k = (long)(r->w % (uint64_t)r->n);
    long clip = 0;
    const float g = 127.0f * r->scale;
    for (long i = 0; i < n; i++) {
        const int8_t a = sat8(iq[2 * i], g), b = sat8(iq[2 * i + 1], g);
        clip += (a == 127 || a == -128) + (b == 127 || b == -128);
        r->buf[2 * k] = a, r->buf[2 * k + 1] = b;
        if (++k == r->n)
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
