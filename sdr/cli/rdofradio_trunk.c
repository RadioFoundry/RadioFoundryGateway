// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* rdofradio's trunked site (rdofradio_trunk.h). */
#include "rdofradio_trunk.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "rdof/mnctl.h"
#include "rdof/mqttnet.h"
#include "rdof/sdr/sdrmodem.h"
#include "rdof/t3.h"
#include "rdof/trunksim.h"
#include "rdof/trunkunit.h"
#ifdef RDOF_MQTT
#include "rdofradio_mqtt.h"
#endif

#ifndef RDOF_VERSION
#define RDOF_VERSION "dev"
#endif

static struct {
    bool on;
    const rdofradio_cfg_t *c;
    rdofradio_trunk_opts_t o;
    pthread_mutex_t mu;      /* the trunk unit: the modem's slot source and the main loop */
    rdof_trunkunit_t *u;
    void *site_mem;
    char node[RDOF_MN_NODE_MAX + 1];
    char online[128], info[128];
    bool net, up;
    uint32_t rng;
    double last_sim_status;
#ifdef RDOF_MQTT
    rdofmq_t *mq;
#endif
} T = {.mu = PTHREAD_MUTEX_INITIALIZER};

/* messages from libmosquitto's thread, taken by the loop */
#define INQ 1024
static struct {
    char *topic;
    void *data;
    size_t len;
} inq[INQ];
static int inq_head, inq_tail;
static uint32_t inq_dropped;
static pthread_mutex_t inq_mu = PTHREAD_MUTEX_INITIALIZER;

static int64_t wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void tsay(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "trunk: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ── the trunk unit's io ──────────────────────────────────────────────────── */

static void io_publish(void *ctx, const char *topic, const void *data, size_t len, int qos, bool retain)
{
#ifdef RDOF_MQTT
    if (T.mq)
        rdofmq_publish(T.mq, topic, data, (int)len, qos, retain);
#endif
}

static void io_subscribe(void *ctx, const char *const *f, int n)
{
#ifdef RDOF_MQTT
    if (T.mq)
        rdofmq_subscribe(T.mq, f, n);
#endif
}

static void io_log(void *ctx, const char *text)
{
    if (T.o.verbose)
        tsay("%s", text);
}

static uint32_t io_rand(void *ctx)
{
    T.rng ^= T.rng << 13, T.rng ^= T.rng >> 17, T.rng ^= T.rng << 5;
    return T.rng;
}

static void cache_path(char *out, size_t cap) { snprintf(out, cap, "%s/cfg-cache.json", T.o.state_dir); }

static void cache_line(void *ctx, const char *text, size_t len)
{
    fwrite(text, 1, len, ctx);
    fputc('\n', ctx);
}

/* provisioning changed: the cache rewritten (a temporary file, then renamed) */
static void io_cache(void *ctx)
{
    if (!T.o.state_dir)
        return;
    char path[512], tmp[520];
    cache_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        tsay("%s: %s", tmp, strerror(errno));
        return;
    }
    rdof_mn_cfg_cache(&T.u->mc, cache_line, f);
    if (fclose(f) || rename(tmp, path))
        tsay("%s: %s", path, strerror(errno));
}

static void cache_load(void)
{
    if (!T.o.state_dir)
        return;
    char path[512];
    cache_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    static char line[16384];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = 0;
        if (len && rdof_mn_cfg_cache_line(&T.u->mc, line, len) == RDOF_MN_CFG_APPLIED)
            n++;
    }
    fclose(f);
    if (n) {
        tsay("%d provisioning documents from %s", n, path);
        rdof_trunkunit_apply(T.u);
    }
}

/* ── the network ──────────────────────────────────────────────────────────── */

#ifdef RDOF_MQTT
static void on_message(void *ctx, const char *topic, const void *data, int len)
{
    char *t = strdup(topic);
    void *d = malloc(len > 0 ? (size_t)len : 1);
    if (!t || !d) {
        free(t), free(d);
        return;
    }
    if (len > 0)
        memcpy(d, data, (size_t)len);
    pthread_mutex_lock(&inq_mu);
    if ((inq_head + 1) % INQ != inq_tail) {
        inq[inq_head].topic = t, inq[inq_head].data = d, inq[inq_head].len = len > 0 ? (size_t)len : 0;
        inq_head = (inq_head + 1) % INQ;
        t = NULL, d = NULL;
    } else {
        inq_dropped++;
    }
    pthread_mutex_unlock(&inq_mu);
    free(t), free(d);
}

static void mq_say(const char *line) { tsay("mqtt: %s", line); }
#endif

/* online and info (retained), after each connect */
static void publish_node(void)
{
#ifdef RDOF_MQTT
    if (!T.mq)
        return;
    rdofmq_publish(T.mq, T.online, "1", -1, 1, true);
    static char trunk[1024], buf[4096];
    rdof_trunkunit_trunk_json(T.u, trunk, sizeof trunk);
    const rdofradio_trunk_cfg_t *t = &T.c->t;
    rdof_mn_info_t in = {.node = T.node, .name = T.c->site.name, .firmware = "rdofradio " RDOF_VERSION,
                         .transport = t->tls ? "mqtt/tls" : "mqtt", .bundle = 1,
                         .trunk_paths = t->path >= 0 && t->path < 64 ? 1ull << t->path : 0, .trunk = trunk};
    if (rdof_mn_info_json(&T.c->site, t->net, &in, buf, sizeof buf))
        rdofmq_publish(T.mq, T.info, buf, -1, 1, true);
#endif
}

static bool start_common(const rdofradio_cfg_t *c, const rdofradio_trunk_opts_t *o, char *err, size_t cap)
{
    T.c = c;
    T.o = *o;
    T.rng = (uint32_t)wall_ms() ^ ((uint32_t)getpid() << 16);
    if (!T.rng)
        T.rng = 1;
    const rdofradio_trunk_cfg_t *t = &c->t;
    snprintf(T.node, sizeof T.node, "%s", c->site.name);
    T.net = t->net >= 0;
    if (T.net && !rdof_mn_node_ok(T.node)) {
        tsay("the box's name \"%s\" can't be a node (letters, digits, . _ -, at most 39): the site runs alone",
             T.node);
        T.net = false;
    }
#ifndef RDOF_MQTT
    if (T.net)
        tsay("rdofradio was built without libmosquitto (RDOF_MQTT): the site runs alone");
    T.net = false;
#endif
    T.u = calloc(1, sizeof *T.u);
    T.site_mem = aligned_alloc(8, (rdof_t3_site_size() + 7) / 8 * 8);
    if (!T.u || !T.site_mem) {
        snprintf(err, cap, "no memory");
        return false;
    }
    const rdof_trunkunit_io_t io = {io_publish, io_subscribe, io_log, io_cache, io_rand, NULL};
    if (!rdof_trunkunit_init(T.u, T.site_mem, &t->cfg, &t->pol, &io, T.net ? t->root : NULL, T.net ? T.node : NULL,
                             t->path, "rdofradio " RDOF_VERSION, err, cap))
        return false;
    T.u->now_ms = wall_ms();
    cache_load();
#ifdef RDOF_MQTT
    if (T.net) {
        static char id[64];
        rdof_mn_node_topic(T.online, sizeof T.online, t->root, T.node, "online");
        rdof_mn_node_topic(T.info, sizeof T.info, t->root, T.node, "info");
        snprintf(id, sizeof id, "rdof-trunk-%s", T.node);
        rdofmq_opts_t mo = {.host = t->host, .user = t->user, .pass = t->pass, .port = t->port, .tls = t->tls,
                            .client_id = id, .will_topic = T.online, .will_msg = "0", .queue_max = 1000,
                            .say = mq_say, .on_message = on_message, .no_local = true, .nodelay = true};
        T.mq = rdofmq_start(&mo);
        if (!T.mq) {
            snprintf(err, cap, "mqtt: libmosquitto refused the options");
            return false;
        }
        tsay("node %s on %s:%d, %s/", T.node, t->host, t->port, t->root);
    }
#endif
    T.on = true;
    tsay("%s: site %u, %d carrier%s, TSCC chan %u (tx %.5f, rx %.5f MHz), cc %d", t->name, (unsigned)t->cfg.site,
         t->ncar, t->ncar == 1 ? "" : "s", (unsigned)t->cfg.carriers[0].chan, t->car[0].tx_hz / 1e6,
         t->car[0].rx_hz / 1e6, t->cfg.cc);
    return true;
}

/* messages in, the link's changes, timers; T.mu held */
static void poll_locked(int64_t now_ms)
{
    T.u->now_ms = now_ms;
    for (;;) {
        pthread_mutex_lock(&inq_mu);
        const bool any = inq_tail != inq_head;
        char *topic = NULL;
        void *data = NULL;
        size_t len = 0;
        if (any) {
            topic = inq[inq_tail].topic, data = inq[inq_tail].data, len = inq[inq_tail].len;
            inq_tail = (inq_tail + 1) % INQ;
        }
        pthread_mutex_unlock(&inq_mu);
        if (!any)
            break;
        rdof_trunkunit_message(T.u, topic, data, len);
        free(topic);
        free(data);
    }
#ifdef RDOF_MQTT
    if (T.mq) {
        if (rdofmq_connected(T.mq)) {
            T.up = true;
            publish_node();
            rdof_trunkunit_link(T.u, true);
        } else if (T.up && !rdofmq_up(T.mq)) {
            T.up = false;
            rdof_trunkunit_link(T.u, false);
        }
    }
#endif
    rdof_trunkunit_tick(T.u, now_ms);
}

static void stop_common(void)
{
#ifdef RDOF_MQTT
    if (T.mq) {
        rdofmq_stop(T.mq); /* online 0 */
        T.mq = NULL;
    }
#endif
    T.on = false;
}

/* ── --sim-air ────────────────────────────────────────────────────────────── */

typedef struct {
    int64_t at_ms;
    uint32_t id, arg, talk_ms;
    uint8_t what;
} act_t;

#define ACTS 256

static int act_cmp(const void *a, const void *b)
{
    const int64_t x = ((const act_t *)a)->at_ms, y = ((const act_t *)b)->at_ms;
    return x < y ? -1 : x > y;
}

/* The script's lines into acts (times from t0). Returns how many, -1 a bad line (said). */
static int script_read(const char *path, int64_t t0, act_t *acts, int max)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        tsay("%s: %s", path, strerror(errno));
        return -1;
    }
    char line[256];
    int n = 0, ln = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        char *h = strchr(line, '#');
        if (h)
            *h = 0;
        double at;
        unsigned long id, a1 = 0, a2 = 0;
        char what[24];
        const int got = sscanf(line, "%lf %lu %23s %lu %lu", &at, &id, what, &a1, &a2);
        if (got <= 0)
            continue;
        act_t x = {.at_ms = t0 + (int64_t)(at * 1000), .id = (uint32_t)id};
        if (got < 3 || at < 0 || !id)
            goto bad;
        if (!strcmp(what, "register"))
            x.what = RDOF_T3MS_DO_REGISTER, x.arg = (uint32_t)a1;
        else if (!strcmp(what, "deregister"))
            x.what = RDOF_T3MS_DO_DEREGISTER;
        else if (!strcmp(what, "group") && got >= 4)
            x.what = RDOF_T3MS_DO_GROUP, x.arg = (uint32_t)a1, x.talk_ms = got >= 5 ? (uint32_t)a2 : 2000;
        else if (!strcmp(what, "private") && got >= 4)
            x.what = RDOF_T3MS_DO_PRIVATE, x.arg = (uint32_t)a1, x.talk_ms = got >= 5 ? (uint32_t)a2 : 2000;
        else if (!strcmp(what, "cancel"))
            x.what = RDOF_T3MS_DO_CANCEL;
        else
            goto bad;
        if (n == max) {
            tsay("%s: more than %d actions", path, max);
            fclose(f);
            return -1;
        }
        acts[n++] = x;
        continue;
    bad:
        tsay("%s:%d: AT_S ID register [TG] | deregister | group TG [TALK_MS] | private ID [TALK_MS] | cancel", path, ln);
        fclose(f);
        return -1;
    }
    fclose(f);
    qsort(acts, (size_t)n, sizeof *acts, act_cmp);
    return n;
}

static const char *const ms_state[] = {"idle", "access", "waiting", "payload"};

int rdofradio_trunk_sim(const rdofradio_cfg_t *c, const rdofradio_trunk_opts_t *o, volatile sig_atomic_t *stop)
{
    char err[200];
    if (!start_common(c, o, err, sizeof err)) {
        tsay("%s", err);
        return 1;
    }
    const int64_t t0 = wall_ms();
    static act_t acts[ACTS];
    int nacts = 0;
    if (o->sim_script && (nacts = script_read(o->sim_script, t0, acts, ACTS)) < 0)
        return 2;
    /* the MSs: the N, then the script's others */
    static uint32_t ids[RDOF_TRUNKSIM_MS];
    int n = o->sim_n < RDOF_TRUNKSIM_MS ? o->sim_n : RDOF_TRUNKSIM_MS;
    for (int i = 0; i < n; i++)
        ids[i] = o->sim_id + (uint32_t)i;
    for (int a = 0; a < nacts; a++) {
        bool known = false;
        for (int i = 0; i < n && !known; i++)
            known = ids[i] == acts[a].id;
        if (!known && n < RDOF_TRUNKSIM_MS)
            ids[n++] = acts[a].id;
    }
    static rdof_t3ms_t *ms[RDOF_TRUNKSIM_MS];
    const rdof_t3_cfg_t *cfg = &T.u->cfg;
    uint16_t chans[RDOF_T3_CARRIERS];
    for (int k = 0; k < cfg->ncarriers; k++)
        chans[k] = cfg->carriers[k].chan;
    const uint32_t tg[1] = {o->sim_tg};
    for (int i = 0; i < n; i++) {
        ms[i] = malloc(rdof_t3ms_size());
        if (!ms[i]) {
            tsay("no memory");
            return 1;
        }
        rdof_t3ms_init(ms[i], ids[i], tg, 1, chans, cfg->ncarriers, T.rng ^ (ids[i] * 2654435761u));
        if (i < o->sim_n) /* staggered: 300 ms apart from half a second in */
            rdof_t3ms_do(ms[i], RDOF_T3MS_DO_REGISTER, o->sim_tg, 0, 0, t0 + 500 + 300 * i);
    }
    static rdof_trunksim_t sim;
    rdof_trunksim_init(&sim, T.u, n, ms, ids);
    tsay("--sim-air: %d MSs (%u..%u%s), talkgroup %u", n, (unsigned)o->sim_id, (unsigned)(o->sim_id + o->sim_n - 1),
         n > o->sim_n ? " and the script's" : "", (unsigned)o->sim_tg);

    int64_t k = t0 / 30, next_talk = o->sim_talk_s > 0 ? t0 + (int64_t)(o->sim_talk_s * 1000) : -1;
    int64_t last_poll = 0, last_status = t0;
    int ai = 0, talker = 0;
    uint32_t skipped = 0;
    rdof_trunkunit_run(T.u, k, t0);
    while (!*stop && (o->seconds <= 0 || wall_ms() - t0 < (int64_t)(o->seconds * 1000))) {
        const int64_t now = wall_ms();
        const int64_t k_now = now / 30;
        if (k_now - k > 50) { /* stalled (a suspended process): the air jumps ahead */
            skipped += (uint32_t)(k_now - k);
            k = k_now - 1;
        }
        for (; ai < nacts && acts[ai].at_ms <= now; ai++) {
            const int i = rdof_trunksim_find(&sim, acts[ai].id);
            if (i >= 0)
                rdof_t3ms_do(ms[i], acts[ai].what, acts[ai].arg, acts[ai].talk_ms, 0, now);
        }
        if (next_talk >= 0 && now >= next_talk && o->sim_n > 0) {
            rdof_t3ms_do(ms[talker], RDOF_T3MS_DO_GROUP, o->sim_tg, 2000, 0, now);
            talker = (talker + 1) % o->sim_n;
            next_talk = now + (int64_t)(o->sim_talk_s * 1000);
        }
        pthread_mutex_lock(&T.mu);
        while (k < k_now) {
            k++;
            rdof_trunksim_slot(&sim, k, k * 30);
        }
        if (now - last_poll >= 20) {
            last_poll = now;
            poll_locked(now);
        }
        pthread_mutex_unlock(&T.mu);
        if (now - last_status >= 10000) {
            last_status = now;
            int reg = 0;
            for (int i = 0; i < n; i++) {
                rdof_t3ms_status_t st;
                rdof_t3ms_status(ms[i], &st);
                reg += st.registered;
            }
            tsay("%d of %d MSs registered; out %u PDUs, in %u PDUs %u voice, %u collisions; net in %u, setups %u/%u%s",
                 reg, n, (unsigned)sim.out_pdus, (unsigned)sim.in_pdus, (unsigned)sim.in_voice,
                 (unsigned)sim.collisions, (unsigned)T.u->net_in, (unsigned)T.u->setups_out,
                 (unsigned)T.u->setups_in, skipped ? "; slots skipped" : "");
            if (o->verbose)
                for (int i = 0; i < n; i++) {
                    rdof_t3ms_status_t st;
                    rdof_t3ms_status(ms[i], &st);
                    tsay("  %u: %s on ch %d", (unsigned)ids[i],
                         st.state < (int)(sizeof ms_state / sizeof *ms_state) ? ms_state[st.state] : "?", st.ch);
                }
        }
        /* to the next slot's start */
        const int64_t wait = (k_now + 1) * 30 - wall_ms();
        if (wait > 0) {
            struct timespec ts = {0, (long)(wait > 20 ? 20 : wait) * 1000000L};
            nanosleep(&ts, NULL);
        }
    }
    stop_common();
    for (int i = 0; i < n; i++)
        free(ms[i]);
    return 0;
}

/* ── on a device ──────────────────────────────────────────────────────────── */

static rdof_sdrmodem_t *M;

static bool source_slot(void *ctx, int64_t k, uint8_t burst[RDOF_BURST_BYTES], int *at, int32_t *slc28)
{
    uint8_t control;
    pthread_mutex_lock(&T.mu);
    T.u->now_ms = wall_ms();
    const bool ok = rdof_trunkunit_slot(T.u, 0, k, burst, &control, at, slc28);
    pthread_mutex_unlock(&T.mu);
    return ok;
}

bool rdofradio_trunk_start(const rdofradio_cfg_t *c, const rdofradio_trunk_opts_t *o, rdof_sdrmodem_t *m, char *err,
                           size_t cap)
{
    if (c->t.ncar > 1) {
        snprintf(err, cap, "%s: %d carriers: rdofradio runs one (the SDR's) for now; a split's TX half for the others "
                 "isn't wired", c->t.name, c->t.ncar);
        return false;
    }
    if (!c->t.car[0].tx_sdr) {
        snprintf(err, cap, "%s: the TSCC's carrier is sent by the SDR here", c->t.name);
        return false;
    }
    if (!start_common(c, o, err, cap))
        return false;
    M = m;
    const rdof_sdrmodem_source_t src = {source_slot, NULL};
    rdof_sdrmodem_set_source(m, &src);
    if (!rdof_sdrmodem_hold_key(m, true)) {
        snprintf(err, cap, "the modem won't hold its key (not duplex?)");
        return false;
    }
    return true;
}

bool rdofradio_trunk_on(void) { return T.on; }

void rdofradio_trunk_heard(int c, int64_t k, const rdof_burst_t *b)
{
    if (!T.on || k < 0)
        return;
    pthread_mutex_lock(&T.mu);
    rdof_trunkunit_rx(T.u, c, k, b, 0);
    pthread_mutex_unlock(&T.mu);
}

void rdofradio_trunk_poll(void)
{
    if (!T.on)
        return;
    pthread_mutex_lock(&T.mu);
    poll_locked(wall_ms());
    pthread_mutex_unlock(&T.mu);
}

void rdofradio_trunk_stop(void)
{
    if (!T.on)
        return;
    if (M) {
        rdof_sdrmodem_set_source(M, NULL);
        rdof_sdrmodem_hold_key(M, false);
    }
    stop_common();
}
