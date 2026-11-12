// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* rdofradio --sim-ext: external MSs on the simulated air (rdofradio_simext.h). */
#include "rdofradio_simext.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "rdof/jsonp.h"
#include "rdof/pdu.h"
#include "rdof/t3rf.h"

#define CLIENTS 16
#define LINE 16384
#define QMAX 256
#define FATES 64
#define WAIT_MS 2000
#define HALF_SLOT_US 15000

typedef struct {
    int fd;
    char name[40];
    char buf[LINE];
    size_t len;
    int64_t sent_k, acked_k; /* the last slot sent, answered (-1 none) */
    struct {
        int64_t k;
        int ch;
        uint32_t id;
        const char *fate;
    } fates[FATES];
    int nfates;
} client_t;

/* a burst waiting for its slot */
typedef struct {
    client_t *cl;
    int64_t k;
    int ch, off_us;
    uint32_t id;
    bool corrupt;
    rdof_burst_t b;
} qent_t;

static struct {
    rdof_t3sim_t *sim;
    int lfd;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    client_t *cl[CLIENTS];
    qent_t q[QMAX];
    int nq;
    qent_t taken[RDOF_T3SIM_EXT]; /* the bursts given to the sim for this slot */
    double loss, err;
    uint32_t rng;
    bool verbose;
} X = {.lfd = -1};

static double rnd(void)
{
    X.rng ^= X.rng << 13, X.rng ^= X.rng >> 17, X.rng ^= X.rng << 5;
    return (X.rng >> 8) / 16777216.0;
}

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void hex(char *out, const uint8_t *d, int n)
{
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[2 * i] = h[d[i] >> 4];
        out[2 * i + 1] = h[d[i] & 15];
    }
    out[2 * n] = 0;
}

static bool unhex(const char *s, uint8_t *d, int n)
{
    if (!s || (int)strlen(s) != 2 * n)
        return false;
    for (int i = 0; i < 2 * n; i++) {
        const char c = s[i];
        const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0)
            return false;
        d[i / 2] = (uint8_t)(i & 1 ? d[i / 2] | v : v << 4);
    }
    return true;
}

/* what became of a burst: to the client after the slot, and the log */
static void fate(client_t *c, int64_t k, int ch, uint32_t id, int off_us, int dt, const char *f, const char *why)
{
    if (c->nfates < FATES) {
        c->fates[c->nfates].k = k;
        c->fates[c->nfates].ch = ch;
        c->fates[c->nfates].id = id;
        c->fates[c->nfates].fate = f;
        c->nfates++;
    }
    if (dt == RDOF_DT_VOICE && !X.verbose && !strcmp(f, "heard"))
        return;
    char what[24];
    if (dt == RDOF_DT_VOICE || dt == RDOF_DT_VOICE_SYNC)
        snprintf(what, sizeof what, "voice");
    else if (dt >= 0)
        snprintf(what, sizeof what, "dt %d", dt);
    else
        snprintf(what, sizeof what, "?");
    fprintf(stderr, "sim-ext: %s id %" PRIu32 " k %" PRId64 " carrier %d slot %d %s, %+d us: %s%s%s\n", c->name, id, k,
            ch / 2, ch % 2 + 1, what, off_us, f, why ? " " : "", why ? why : "");
}

static void drop(int i, const char *why)
{
    client_t *c = X.cl[i];
    fprintf(stderr, "sim-ext: %s left (%s)\n", c->name, why);
    close(c->fd);
    int n = 0;
    for (int e = 0; e < X.nq; e++)
        if (X.q[e].cl != c)
            X.q[n++] = X.q[e];
    X.nq = n;
    free(c);
    X.cl[i] = NULL;
}

/* ── the sim's hooks ──────────────────────────────────────────────────────── */

static int ext_in(void *ctx, int64_t k, rdof_t3sim_in_t *in, int max)
{
    (void)ctx;
    int n = 0, keep = 0;
    for (int e = 0; e < X.nq; e++) {
        qent_t *q = &X.q[e];
        if (q->k > k) {
            X.q[keep++] = *q;
            continue;
        }
        if (q->k < k)
            fate(q->cl, q->k, q->ch, q->id, q->off_us, q->b.dt, "late", "(sent after its slot)");
        else if (q->off_us > HALF_SLOT_US || q->off_us < -HALF_SLOT_US)
            fate(q->cl, q->k, q->ch, q->id, q->off_us, q->b.dt, "timing", "(more than half a slot off)");
        else if (X.loss > 0 && rnd() < X.loss)
            fate(q->cl, q->k, q->ch, q->id, q->off_us, q->b.dt, "loss", NULL);
        else if (n >= max)
            fate(q->cl, q->k, q->ch, q->id, q->off_us, q->b.dt, "bad", "(too many bursts in one slot)");
        else {
            q->corrupt = X.err > 0 && rnd() < X.err;
            if (q->corrupt)
                for (int j = 14; j < 20; j++)
                    q->b.burst[j] ^= 0xA5;
            X.taken[n] = *q;
            in[n].ch = q->ch;
            in[n].b = q->b;
            in[n].tag = &X.taken[n];
            n++;
        }
    }
    X.nq = keep;
    return n;
}

static void ext_out(void *ctx, int64_t k, const rdof_t3sim_out_t *out, int ncar, int at_next,
                    const rdof_t3sim_in_t *in, int nin)
{
    (void)ctx;
    for (int e = 0; e < nin; e++) {
        const qent_t *q = in[e].tag;
        const char *f = in[e].fate == RDOF_T3SIM_DROPPED    ? "no-slot"
                        : in[e].fate == RDOF_T3SIM_COLLIDED ? "collided"
                        : q->corrupt                           ? "corrupted"
                                                               : "heard";
        fate(q->cl, k, q->ch, q->id, q->off_us, q->b.dt, f,
             in[e].fate == RDOF_T3SIM_DROPPED ? "(that channel isn't on air in this slot)" : NULL);
    }
    /* the carriers, the same for every client */
    static char car[RDOF_T3_CARRIERS * 256];
    size_t m = 0;
    for (int c = 0; c < ncar; c++) {
        const rdof_t3sim_out_t *o = &out[c];
        char b[2 * RDOF_BURST_BYTES + 1], p[24], ch[8];
        int at = 0, tc = 0, lcss = 0;
        uint32_t p17 = 0;
        rdof_t3rf_cach_read(o->cach, &at, &tc, &lcss, &p17);
        hex(b, o->burst, RDOF_BURST_BYTES);
        if (o->pdu_ok) {
            p[0] = '"';
            hex(p + 1, o->pdu, 10);
            strcat(p, "\"");
        } else {
            strcpy(p, "null");
        }
        hex(ch, o->cach, 3);
        m += (size_t)snprintf(car + m, sizeof car - m,
                              "%s{\"c\":%d,\"ch\":%d,\"slot\":%d,\"aired\":%s,\"burst\":\"%s\",\"dt\":%d,\"cc\":%d,"
                              "\"pdu\":%s,\"cach\":\"%s\",\"at\":%d,\"tc\":%d,\"lcss\":%d,\"slc17\":%" PRIu32 "}",
                              c ? "," : "", c, 2 * c + (int)(k & 1), (int)(k & 1) + 1, o->aired ? "true" : "false",
                              o->aired ? b : "", o->aired ? o->dt : -1, o->aired ? o->cc : -1, p, ch, at, tc, lcss,
                              p17);
    }
    for (int i = 0; i < CLIENTS; i++) {
        client_t *c = X.cl[i];
        if (!c)
            continue;
        static char line[sizeof car + FATES * 96 + 128];
        size_t n = (size_t)snprintf(line, sizeof line, "{\"k\":%" PRId64 ",\"t_ms\":%" PRId64 ",\"at_next\":%d,\"out\":[%s],\"in\":[",
                                    k, k * 30, at_next > 0, car);
        for (int f = 0; f < c->nfates; f++)
            n += (size_t)snprintf(line + n, sizeof line - n, "%s{\"k\":%" PRId64 ",\"ch\":%d,\"id\":%" PRIu32 ",\"fate\":\"%s\"}",
                                  f ? "," : "", c->fates[f].k, c->fates[f].ch, c->fates[f].id, c->fates[f].fate);
        c->nfates = 0;
        n += (size_t)snprintf(line + n, sizeof line - n, "]}\n");
        size_t off = 0;
        while (off < n) {
            const ssize_t w = write(c->fd, line + off, n - off);
            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0)
                break;
            off += (size_t)w;
        }
        if (off < n) {
            drop(i, "write failed");
            continue;
        }
        c->sent_k = k;
    }
}

/* ── the clients' lines ───────────────────────────────────────────────────── */

static int jint(const rdof_jdoc_t *d, int obj, const char *key, int dflt)
{
    const rdof_jnode_t *n = rdof_jnode(d, rdof_jget(d, obj, key));
    return n && n->type == RDOF_JNUM ? (int)n->num : dflt;
}

static int64_t jll(const rdof_jdoc_t *d, int obj, const char *key, int64_t dflt)
{
    const rdof_jnode_t *n = rdof_jnode(d, rdof_jget(d, obj, key));
    return n && n->type == RDOF_JNUM ? (int64_t)n->num : dflt;
}

static const char *jstr(const rdof_jdoc_t *d, int obj, const char *key)
{
    const rdof_jnode_t *n = rdof_jnode(d, rdof_jget(d, obj, key));
    return n && n->type == RDOF_JSTR ? n->str : NULL;
}

/* one TX of a client's answer to slot base */
static void take_tx(client_t *c, const rdof_jdoc_t *d, int t, int64_t base)
{
    const rdof_t3unit_t *u = X.sim->u;
    int ch = jint(d, t, "ch", -1);
    if (ch < 0)
        ch = 2 * jint(d, t, "c", 0) + jint(d, t, "slot", 1) - 1;
    int64_t k = base + 1;
    if ((k & 1) != (ch & 1))
        k++;
    k = jll(d, t, "k", k);
    const uint32_t id = (uint32_t)jll(d, t, "id", 0);
    const int off = jint(d, t, "offset_us", 0), cc = jint(d, t, "cc", u->cfg.cc), n = jint(d, t, "n", 0);
    rdof_t3ms_tx_t tx;
    memset(&tx, 0, sizeof tx);
    tx.ch = (uint8_t)(ch < 0 ? 0 : ch);
    const char *voice = jstr(d, t, "voice"), *pdu = jstr(d, t, "pdu"), *burst = jstr(d, t, "burst");
    qent_t q = {.cl = c, .k = k, .ch = ch, .off_us = off, .id = id};
    bool ok = false;
    if (pdu) {
        tx.kind = 1;
        tx.dt = (uint8_t)jint(d, t, "dt", -1);
        ok = unhex(pdu, tx.pdu, 10) && rdof_t3sim_ms_burst(&tx, 0, cc, &q.b);
    } else if (voice) {
        tx.kind = 2;
        tx.what = !strcmp(voice, "header") ? RDOF_T3_VOICE_HEADER
                  : !strcmp(voice, "term") ? RDOF_T3_VOICE_TERM
                  : !strcmp(voice, "frame") ? RDOF_T3_VOICE_FRAME
                                            : 0;
        tx.src = (uint32_t)jll(d, t, "src", id);
        tx.dst = (uint32_t)jll(d, t, "dst", 0);
        const rdof_jnode_t *g = rdof_jnode(d, rdof_jget(d, t, "group"));
        tx.group = !g || g->type != RDOF_JBOOL || g->b;
        ok = tx.what && rdof_t3sim_ms_burst(&tx, n, cc, &q.b);
    } else if (burst && unhex(burst, q.b.burst, RDOF_BURST_BYTES)) {
        int dt = -1, rcc = -1;
        uint8_t p[10];
        if (rdof_t3rf_burst_pdu(q.b.burst, &dt, &rcc, p) != RDOF_T3RF_PDU_OK)
            dt = n ? RDOF_DT_VOICE : RDOF_DT_VOICE_SYNC;
        q.b.dt = jint(d, t, "dt", dt);
        q.b.n = n;
        q.b.cc = cc;
        q.b.family = RDOF_FAMILY_MS;
        ok = true;
    }
    if (!ok || ch < 0) {
        fate(c, k, ch < 0 ? 0 : ch, id, off, -1, "bad", "(no burst: pdu + dt, voice or burst)");
        return;
    }
    if (X.nq >= QMAX) {
        fate(c, k, ch, id, off, q.b.dt, "bad", "(the queue is full)");
        return;
    }
    X.q[X.nq++] = q;
}

static void take_line(client_t *c, const char *text, size_t len)
{
    static rdof_jnode_t nodes[1024];
    static char sbuf[LINE];
    rdof_jdoc_t d = {nodes, 1024, 0, sbuf, sizeof sbuf, 0, {0}};
    const int root = rdof_jparse(&d, text, len);
    if (root < 0 || rdof_jnode(&d, root)->type != RDOF_JOBJ) {
        fprintf(stderr, "sim-ext: %s: not a JSON object (%s)\n", c->name, root < 0 ? d.error : "");
        return;
    }
    const char *hello = jstr(&d, root, "hello");
    if (hello)
        snprintf(c->name, sizeof c->name, "%s", hello);
    const int64_t k = jll(&d, root, "k", c->sent_k);
    const int txs = rdof_jget(&d, root, "tx");
    const rdof_jnode_t *tn = rdof_jnode(&d, txs);
    if (tn && tn->type == RDOF_JARR)
        for (int i = 0; i < tn->count; i++)
            take_tx(c, &d, rdof_jat(&d, txs, i), k);
    if (rdof_jget(&d, root, "k") >= 0 && k > c->acked_k)
        c->acked_k = k;
}

/* read what's there; false: the client went */
static bool take_input(int i)
{
    client_t *c = X.cl[i];
    const ssize_t r = read(c->fd, c->buf + c->len, sizeof c->buf - c->len);
    if (r < 0 && (errno == EINTR || errno == EAGAIN))
        return true;
    if (r <= 0) {
        drop(i, r == 0 ? "closed" : strerror(errno));
        return false;
    }
    c->len += (size_t)r;
    char *nl;
    while ((nl = memchr(c->buf, '\n', c->len)) != NULL) {
        const size_t n = (size_t)(nl - c->buf);
        if (n)
            take_line(c, c->buf, n);
        memmove(c->buf, nl + 1, c->len - n - 1);
        c->len -= n + 1;
    }
    if (c->len == sizeof c->buf) {
        drop(i, "line too long");
        return false;
    }
    return true;
}

static void accept_new(void)
{
    for (;;) {
        const int fd = accept(X.lfd, NULL, NULL);
        if (fd < 0)
            return;
        int i = 0;
        while (i < CLIENTS && X.cl[i])
            i++;
        client_t *c = i < CLIENTS ? calloc(1, sizeof *c) : NULL;
        if (!c) {
            fprintf(stderr, "sim-ext: a client refused (%d at most)\n", CLIENTS);
            close(fd);
            continue;
        }
#ifdef SO_NOSIGPIPE
        const int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        c->fd = fd;
        c->sent_k = c->acked_k = -1;
        snprintf(c->name, sizeof c->name, "client %d", i);
        X.cl[i] = c;
        fprintf(stderr, "sim-ext: %s joined\n", c->name);
    }
}

/* ── the API ──────────────────────────────────────────────────────────────── */

bool rdofradio_simext_open(rdof_t3sim_t *sim, const char *path, double loss, double err_rate, uint32_t seed,
                           bool verbose, char *err, size_t cap)
{
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof a.sun_path) {
        snprintf(err, cap, "--sim-ext %s: the path is too long", path);
        return false;
    }
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    struct stat st;
    if (lstat(path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            snprintf(err, cap, "--sim-ext %s: exists and isn't a socket", path);
            return false;
        }
        unlink(path);
    }
    X.lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (X.lfd < 0 || bind(X.lfd, (struct sockaddr *)&a, sizeof a) || listen(X.lfd, 8)) {
        snprintf(err, cap, "--sim-ext %s: %s", path, strerror(errno));
        if (X.lfd >= 0)
            close(X.lfd);
        X.lfd = -1;
        return false;
    }
    fcntl(X.lfd, F_SETFL, fcntl(X.lfd, F_GETFL) | O_NONBLOCK);
    signal(SIGPIPE, SIG_IGN);
    snprintf(X.path, sizeof X.path, "%s", path);
    X.sim = sim;
    X.loss = loss;
    X.err = err_rate;
    X.rng = seed | 1;
    X.verbose = verbose;
    sim->ext_in = ext_in;
    sim->ext_out = ext_out;
    sim->ext_ctx = NULL;
    fprintf(stderr, "sim-ext: listening on %s\n", path);
    return true;
}

void rdofradio_simext_wait(int64_t k)
{
    if (X.lfd < 0)
        return;
    accept_new();
    const int64_t deadline = mono_ms() + WAIT_MS;
    for (;;) {
        struct pollfd p[CLIENTS];
        int idx[CLIENTS], np = 0;
        for (int i = 0; i < CLIENTS; i++)
            if (X.cl[i] && X.cl[i]->sent_k == k && X.cl[i]->acked_k < k) {
                p[np].fd = X.cl[i]->fd;
                p[np].events = POLLIN;
                idx[np++] = i;
            }
        if (!np)
            break;
        const int64_t left = deadline - mono_ms();
        if (left <= 0) {
            for (int j = 0; j < np; j++)
                drop(idx[j], "no answer in 2 s");
            break;
        }
        if (poll(p, (nfds_t)np, (int)left) <= 0)
            continue;
        for (int j = 0; j < np; j++)
            if (p[j].revents)
                take_input(idx[j]);
    }
    /* lines that came early (a client a slot ahead) */
    for (int i = 0; i < CLIENTS; i++)
        if (X.cl[i]) {
            struct pollfd p = {X.cl[i]->fd, POLLIN, 0};
            if (poll(&p, 1, 0) > 0)
                take_input(i);
        }
}

int rdofradio_simext_clients(void)
{
    int n = 0;
    for (int i = 0; i < CLIENTS; i++)
        n += X.cl[i] != NULL;
    return n;
}

void rdofradio_simext_close(void)
{
    for (int i = 0; i < CLIENTS; i++)
        if (X.cl[i])
            drop(i, "the site stopped");
    if (X.lfd >= 0) {
        close(X.lfd);
        unlink(X.path);
    }
    X.lfd = -1;
    if (X.sim)
        X.sim->ext_in = NULL, X.sim->ext_out = NULL;
}
