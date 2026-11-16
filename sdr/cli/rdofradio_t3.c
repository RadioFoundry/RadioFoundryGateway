// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* rdofradio's trunked site (rdofradio_t3.h). */
#include "rdofradio_t3.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "rdof/duplex.h"
#include "rdof/mmdvm.h"
#include "rdof/mnctl.h"
#include "rdof/mqttnet.h"
#include "rdof/pdu.h"
#include "rdof/sdr/sdrmodem.h"
#include "rdof/sdr/t3split.h"
#include "rdof/split.h"
#include "rdof/splitops.h"
#include "rdof/t3.h"
#include "rdof/t3hat.h"
#include "rdof/t3rf.h"
#include "rdof/t3sim.h"
#include "rdof/t3unit.h"
#ifdef RDOF_MQTT
#include "rdofradio_ipgw.h"
#include "rdofradio_mqtt.h"
#endif
#include "rdofradio_serial.h"
#include "rdofradio_simext.h"
#include "rdofradio_utc.h"
#ifdef RDOF_SIM_SPLIT
#include "fake_radio.h"
#endif

#ifndef RDOF_VERSION
#define RDOF_VERSION "dev"
#endif

static struct {
    bool on;
    const rdofradio_cfg_t *c;
    rdofradio_t3_opts_t o;
    pthread_mutex_t mu;      /* the Tier III unit: the modem's slot source and the main loop */
    rdof_t3unit_t *u;
    void *site_mem;
    char node[RDOF_MN_NODE_MAX + 1];
    char online[128], info[128];
    bool net, up;
    uint32_t rng;
    double last_sim_status;
#ifdef RDOF_MQTT
    rdofmq_t *mq;
#endif
    /* standby (tier3.md 6.2): the active one's beat out on hold, a standby's heard on standby */
    char hold_topic[128], sb_topic[128];
    int64_t hold_next, sb_last;
    bool sb_seen, fenced;
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

/* The standby pair's beats and timeouts: a clock step (NTP, netclock) mustn't look like a beat gone. */
static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void tsay(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "tier3: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ── the Tier III unit's io ──────────────────────────────────────────────────── */

static void io_publish(void *ctx, const char *topic, const void *data, size_t len, int qos, bool retain)
{
#ifdef RDOF_MQTT
    if (T.mq)
        rdofmq_publish(T.mq, topic, data, (int)len, qos, retain);
#endif
}

static char air_topic[160]; /* the air MSs' commands (--air-ms), "" none */

static void io_subscribe(void *ctx, const char *const *f, int n)
{
#ifdef RDOF_MQTT
    if (!T.mq)
        return;
    const char *all[RDOFMQ_SUBS];
    int m = 0;
    for (int i = 0; i < n && m < RDOFMQ_SUBS - 1; i++)
        all[m++] = f[i];
    if (air_topic[0])
        all[m++] = air_topic;
    if (T.sb_topic[0] && m < RDOFMQ_SUBS - 1)
        all[m++] = T.sb_topic, all[m++] = T.hold_topic;
    rdofmq_subscribe(T.mq, all, m);
#endif
}

/* the site's own lines (C_RAND timing, C_MOVE, commands, data): a few a minute, so always; -v adds the bursts */
static void io_log(void *ctx, const char *text) { tsay("%s", strncmp(text, "tier3: ", 7) ? text : text + 7); }

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
    const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600); /* it holds the MSs' authentication keys */
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!f) {
        if (fd >= 0)
            close(fd);
        tsay("%s: %s", tmp, strerror(errno));
        return;
    }
    rdof_mn_cfg_cache(rdof_t3unit_mc(T.u), cache_line, f);
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
        if (len && rdof_mn_cfg_cache_line(rdof_t3unit_mc(T.u), line, len) == RDOF_MN_CFG_APPLIED)
            n++;
    }
    fclose(f);
    if (n) {
        tsay("%d provisioning documents from %s", n, path);
        rdof_t3unit_apply(T.u);
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
    rdof_t3unit_trunk_json(T.u, trunk, sizeof trunk);
    const rdofradio_t3_cfg_t *t = &T.c->t;
    rdof_mn_info_t in = {.node = T.node, .name = T.c->site.name, .firmware = "rdofradio " RDOF_VERSION,
                         .transport = t->tls ? "mqtt/tls" : "mqtt", .bundle = 1,
                         .trunk_paths = t->path >= 0 && t->path < 64 ? 1ull << t->path : 0, .trunk = trunk};
    if (rdof_mn_info_json(&T.c->site, t->net, &in, buf, sizeof buf))
        rdofmq_publish(T.mq, T.info, buf, -1, 1, true);
#endif
}

/* Packet data the site is a party to (tier3.md 3.16) goes to the IP gateway app, when the document has one; what
 * the gateway sends goes down as the site's own data. Both under T.mu: io_data is the unit's, the gateway's poll
 * runs in poll_locked. */
static void io_data(void *ctx, int ch, int dt, const uint8_t *burst, uint32_t src, uint32_t dst)
{
    if (!rdofgw_on())
        return;
    if (T.o.verbose) { /* each burst as heard: a message the gateway calls bad can be decoded again offline */
        char hex[2 * RDOF_BURST_BYTES + 1];
        for (int i = 0; i < RDOF_BURST_BYTES; i++)
            snprintf(hex + 2 * i, 3, "%02x", burst[i]);
        tsay("ip gateway: ch %d dt %d %u -> %u: %s", ch, dt, src, dst, hex);
    }
    rdofgw_heard(0, (ch & 1) + 1, 0, -1, dt, burst, (double)wall_ms() / 1000.0);
}
/* The gateway's packets go down one at a time (a reply can come while the one before waits for its MS): the rest
 * wait here, in order, for the one going down. Its ACKs don't wait: they go on the open channel they answer. */
#define GW_QUEUE 8
static rdof_ipgw_dmr_t gw_q[GW_QUEUE];
static int gw_qn;
static bool t3_gw_send(void *ctx, const rdof_ipgw_dmr_t *p)
{
    /* an ACK goes now, on the channel it answers: a packet waiting for the MS's return mustn't hold it */
    if (p->response)
        return rdof_t3unit_data_tx(T.u, p->src, p->dst, false, true, p->nbursts, p->control, &p->bursts[0][0]);
    if (!gw_qn && !rdof_t3unit_data_busy(T.u))
        return rdof_t3unit_data_tx(T.u, p->src, p->dst, false, p->response, p->nbursts, p->control, &p->bursts[0][0]);
    if (gw_qn == GW_QUEUE) {
        tsay("ip gateway: %d packets waiting: %u -> %u dropped", GW_QUEUE, (unsigned)p->src, (unsigned)p->dst);
        return false;
    }
    gw_q[gw_qn++] = *p;
    return true;
}
static void gw_drain(void)
{
    while (gw_qn && !rdof_t3unit_data_busy(T.u)) {
        const rdof_ipgw_dmr_t *p = &gw_q[0];
        /* a refusal is logged */
        rdof_t3unit_data_tx(T.u, p->src, p->dst, false, p->response, p->nbursts, p->control, &p->bursts[0][0]);
        memmove(&gw_q[0], &gw_q[1], (size_t)--gw_qn * sizeof gw_q[0]);
    }
}

static bool start_common(const rdofradio_cfg_t *c, const rdofradio_t3_opts_t *o, char *err, size_t cap)
{
    T.c = c;
    T.o = *o;
    T.rng = (uint32_t)wall_ms() ^ ((uint32_t)getpid() << 16);
    FILE *ur = fopen("/dev/urandom", "rb"); /* the authentication challenges start from it (rdof_t3_entropy) */
    if (ur) {
        uint32_t v = 0;
        if (fread(&v, sizeof v, 1, ur) == 1)
            T.rng ^= v;
        fclose(ur);
    }
    if (!T.rng)
        T.rng = 1;
    const rdofradio_t3_cfg_t *t = &c->t;
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
    T.u = calloc(1, rdof_t3unit_size());
    T.site_mem = aligned_alloc(8, (rdof_t3_site_size() + 7) / 8 * 8);
    if (!T.u || !T.site_mem) {
        snprintf(err, cap, "no memory");
        return false;
    }
    const rdof_t3unit_io_t io = {io_publish, io_subscribe, io_log, io_cache, io_rand, NULL, io_data};
    if (!rdof_t3unit_init(T.u, T.site_mem, &t->cfg, &t->pol, &io, T.net ? t->root : NULL, T.net ? T.node : NULL,
                             t->path, "rdofradio " RDOF_VERSION, err, cap))
        return false;
    rdof_t3unit_set_now(T.u, wall_ms());
    cache_load();
    if (c->has_ipgw && c->ipgw.on && !rdofgw_on() && !rdofgw_start(&c->ipgw, t3_gw_send, NULL))
        tsay("ip gateway: no memory: packet data to the site goes unanswered");
#ifdef RDOF_MQTT
    if (T.net) {
        static char id[64];
        rdof_mn_node_topic(T.online, sizeof T.online, t->root, T.node, "online");
        rdof_mn_node_topic(T.info, sizeof T.info, t->root, T.node, "info");
        snprintf(id, sizeof id, "rdof-tier3-%s", T.node);
        if (t->standby) {
            rdof_mn_node_topic(T.hold_topic, sizeof T.hold_topic, t->root, T.node, "hold");
            rdof_mn_node_topic(T.sb_topic, sizeof T.sb_topic, t->root, T.node, "standby");
        }
        /* with a standby: the broker sends the will 1.5 keepalives after the last packet, past fence_s */
        rdofmq_opts_t mo = {.host = t->host, .user = t->user, .pass = t->pass, .port = t->port, .tls = t->tls,
                            .client_id = id, .will_topic = T.online, .will_msg = "0", .queue_max = 1000,
                            .say = mq_say, .on_message = on_message, .no_local = true, .nodelay = true,
                            .keepalive_s = t->standby ? (int)t->fence_s + 1 : 0};
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
static bool air_command(const char *topic, const void *data, size_t len);

/* The active box of a standby pair (tier3.md 6.2): a standby's beat or its will on standby; another box on the air
   on hold (the lower ID stays). True: the message was the pair's. */
#define RDOF_T3_BEAT_MS 500
static bool standby_message(const char *topic, const void *data, size_t len)
{
#ifndef RDOF_MQTT
    (void)topic, (void)data, (void)len;
    return false;
#else
    if (!T.sb_topic[0])
        return false;
    char m[32];
    snprintf(m, sizeof m, "%.*s", (int)(len < sizeof m ? len : sizeof m - 1), (const char *)data);
    if (!strcmp(topic, T.sb_topic)) {
        if (m[0] == '-') { /* its will: gone, no takeover to make way for */
            if (T.sb_seen)
                tsay("standby: %s is gone", m + 2);
            T.sb_seen = false;
        } else {
            if (!T.sb_seen)
                tsay("standby: %s is there: off the air after %.0f s without its beat", m, T.c->t.fence_s);
            T.sb_seen = true, T.sb_last = mono_ms();
        }
        return true;
    }
    if (!strcmp(topic, T.hold_topic)) {
        if (!strncmp(m, "on ", 3) && strcmp(m + 3, rdofmq_hwid()) < 0 && !T.fenced) {
            tsay("standby: %s is on the air too: off the air", m + 3);
            T.fenced = true;
            raise(SIGTERM);
        }
        return true;
    }
    return false;
#endif
}

/* the active box's beat; off the air (the run stops) once a standby's beat is gone fence_s without its will */
static void standby_tick(void)
{
#ifdef RDOF_MQTT
    if (!T.sb_topic[0] || !T.mq)
        return;
    const int64_t now_ms = mono_ms();
    if (now_ms >= T.hold_next) {
        char m[16];
        snprintf(m, sizeof m, "on %s", rdofmq_hwid());
        rdofmq_publish(T.mq, T.hold_topic, m, -1, 0, false);
        T.hold_next = now_ms + RDOF_T3_BEAT_MS;
    }
    if (T.sb_seen && !T.fenced && now_ms - T.sb_last > (int64_t)(T.c->t.fence_s * 1000)) {
        tsay("standby: no beat from the standby or the broker for %.0f s: off the air, the standby takes over",
             T.c->t.fence_s);
        T.fenced = true;
        raise(SIGTERM);
    }
#endif
}

bool rdofradio_t3_fenced(void) { return T.fenced; }

#ifdef RDOF_MQTT
/* The standby's view, from libmosquitto's thread. */
static struct {
    pthread_mutex_t mu;
    char hold[128], sb[128], online[128];
    int64_t on_ms;     /* another box's "on" beat, last */
    int64_t claim_ms;  /* a claim by a lower ID, last */
    int64_t echo_ms;   /* our own beat back from the broker, last: the path is there */
    int64_t will_ms;   /* online "0" (the active one's will), last; 0 none since the last "on" */
} SB = {.mu = PTHREAD_MUTEX_INITIALIZER};

static void sb_message(void *ctx, const char *topic, const void *data, int len)
{
    (void)ctx;
    char m[32];
    snprintf(m, sizeof m, "%.*s", len < 0 ? 0 : len < (int)sizeof m ? len : (int)sizeof m - 1, (const char *)data);
    const int64_t now = mono_ms();
    const char *hw = rdofmq_hwid();
    pthread_mutex_lock(&SB.mu);
    if (!strcmp(topic, SB.sb) && !strcmp(m, hw))
        SB.echo_ms = now;
    else if (!strcmp(topic, SB.hold) && !strncmp(m, "on ", 3) && strcmp(m + 3, hw))
        SB.on_ms = now, SB.will_ms = 0;
    else if (!strcmp(topic, SB.hold) && !strncmp(m, "claim ", 6) && strcmp(m + 6, hw) < 0)
        SB.claim_ms = now;
    else if (!strcmp(topic, SB.online) && !strcmp(m, "0"))
        SB.will_ms = now;
    else if (!strcmp(topic, SB.online) && !strcmp(m, "1"))
        SB.will_ms = 0;
    pthread_mutex_unlock(&SB.mu);
}
#endif

bool rdofradio_t3_standby(const rdofradio_cfg_t *c, volatile sig_atomic_t *stop)
{
    const rdofradio_t3_cfg_t *t = &c->t;
    if (!t->standby)
        return true;
#ifndef RDOF_MQTT
    tsay("standby: rdofradio was built without libmosquitto (RDOF_MQTT): on the air at once");
    return true;
#else
    if (t->net < 0 || !rdof_mn_node_ok(c->site.name)) {
        tsay("standby: no mqtt network for the node: on the air at once");
        return true;
    }
    const char *hw = rdofmq_hwid();
    rdof_mn_node_topic(SB.hold, sizeof SB.hold, t->root, c->site.name, "hold");
    rdof_mn_node_topic(SB.sb, sizeof SB.sb, t->root, c->site.name, "standby");
    rdof_mn_node_topic(SB.online, sizeof SB.online, t->root, c->site.name, "online");
    char id[64], will[16];
    snprintf(id, sizeof id, "rdof-standby-%s-%s", c->site.name, hw);
    snprintf(will, sizeof will, "- %s", hw);
    /* keepalive 5 (libmosquitto's least): a standby gone without a word is gone (its will) 7.5 s on, inside the
       active one's fence_s (at least 9);
       no no_local: our own beat back is the broker's path proven */
    const char *subs[] = {SB.hold, SB.sb, SB.online};
    rdofmq_opts_t mo = {.host = t->host, .user = t->user, .pass = t->pass, .port = t->port, .tls = t->tls,
                        .client_id = id, .will_topic = SB.sb, .will_msg = will, .queue_max = 100, .say = mq_say,
                        .on_message = sb_message, .nodelay = true, .keepalive_s = 5};
    rdofmq_t *q = rdofmq_start(&mo);
    if (!q) {
        tsay("standby: mqtt: libmosquitto refused the options: on the air at once");
        return true;
    }
    rdofmq_subscribe(q, subs, 3);
    tsay("standby: %s waits as %s on %s:%d: on the air %.0f s after the node's beat stops (1 s after its will)",
         c->site.name, hw, t->host, t->port, t->takeover_s);
    int64_t quiet = mono_ms(), claim = 0, next = 0;
    bool won = false;
    while (!*stop && !won) {
        const int64_t now = mono_ms();
        if (now >= next) {
            char m[16];
            if (claim)
                snprintf(m, sizeof m, "claim %s", hw);
            rdofmq_publish(q, claim ? SB.hold : SB.sb, claim ? m : hw, -1, 0, false);
            if (claim) /* the standby's beat goes on while it claims */
                rdofmq_publish(q, SB.sb, hw, -1, 0, false);
            next = now + (claim ? 250 : RDOF_T3_BEAT_MS);
        }
        pthread_mutex_lock(&SB.mu);
        const int64_t on = SB.on_ms, lower = SB.claim_ms, echo = SB.echo_ms, will = SB.will_ms;
        pthread_mutex_unlock(&SB.mu);
        const bool path = rdofmq_up(q) && now - echo < 1500;
        if (!path || on > quiet)
            quiet = path ? on : now;
        if (claim && (on >= claim || lower >= claim)) {
            tsay("standby: %s is on the air: waiting again", on >= claim ? "another box" : "a lower ID");
            claim = 0, quiet = now;
        } else if (claim && now - claim >= 1500) {
            won = true;
        } else if (!claim && path &&
                   (now - quiet >= (int64_t)(t->takeover_s * 1000) || (will > quiet && now - will >= 1000))) {
            tsay("standby: %s: claiming the node", now - quiet >= (int64_t)(t->takeover_s * 1000)
                                                      ? "the node's beat stopped" : "the node's will");
            claim = now, next = 0;
        }
        usleep(50000);
    }
    rdofmq_stop(q); /* its will: no longer a standby */
    if (won)
        tsay("standby: on the air");
    return won;
#endif
}

static void poll_locked(int64_t now_ms)
{
    rdof_t3unit_set_now(T.u, now_ms);
    rdofgw_poll((double)now_ms / 1000.0, rdof_data_sync(RDOF_BS_VOICE)); /* the site regenerates it anyway */
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
        if (!standby_message(topic, data, len) && !air_command(topic, data, len))
            rdof_t3unit_message(T.u, topic, data, len);
        free(topic);
        free(data);
    }
#ifdef RDOF_MQTT
    if (T.mq) {
        if (rdofmq_connected(T.mq)) {
            T.up = true;
            publish_node();
            rdof_t3unit_link(T.u, true);
        } else if (T.up && !rdofmq_up(T.mq)) {
            T.up = false;
            rdof_t3unit_link(T.u, false);
        }
    }
#endif
    standby_tick();
    rdof_t3unit_tick(T.u, now_ms);
    gw_drain();
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
    uint32_t ip; /* data: the datagram's destination when not the ID's (@A.B.C.D), 0 the ID's */
    uint8_t what, flags, len;
    uint16_t fill; /* data: this many generated characters in place of the text (*N) */
    char text[64]; /* sdm: the message (a UDT carries up to 46 octets); data: the UDP payload */
} act_t;

#define ACT_SDM 0xF0       /* rdof_t3ms_sdm (ISO 8-bit text), not an rdof_t3ms_do */
#define ACT_SETSTATUS 0xF1 /* rdof_t3ms_set_status */
#define ACT_DATA 0xF2      /* rdof_t3ms_data: the text, UTF-16BE, in a UDP/IPv4 datagram (port 5016 to 5016) */
#define ACT_AMBE 0xF3      /* the MSs' voice from a file of AMBE+2 frames (rdof_t3sim_ambe), "off" the near-silence */
#define ACT_UPOLL 0xF4     /* rdof_t3ms_upoll (ISO 8-bit), arg the MS, len the blocks */
#define ACT_SETPOLL 0xF5   /* rdof_t3ms_set_poll (ISO 8-bit text) */
#define ACT_IPADV 0xF6     /* rdof_t3ms_ip_advice: arg the address, flags 1 deleting it */
#define ACT_SETSUPP 0xF7   /* rdof_t3ms_set_supp (text, binary format) */
#define ACT_MISSACK 0xF8   /* rdof_t3ms_miss_acks: arg how many */
#define ACT_POWER 0xF9     /* an air MS switched off (arg 0: it neither hears nor sends) or on (1) */
#define AIR_RADIO_NET 0x0C000000u /* 12.0.0.0/8: a radio's address, its id in the low 24 bits */
#define AIR_FILL_MAX ((RDOF_IPGW_MAX_PACKET - 28) / 2) /* data *N: characters at most, one UDP/IPv4 datagram */
#define ACT_HELP                                                                                                       \
    "ID register [TG] | deregister | group TG [TALK_MS] | egroup TG [TALK_MS] | private ID [TALK_MS] | status ID N | gstatus TG N | "        \
    "sdm ID TEXT | gsdm TG TEXT | upoll ID [BLOCKS] | setpoll TEXT | setsupp [TEXT] | "                               \
    "ipadv [A.B.C.D] [del] | data ID [@A.B.C.D] TEXT | "                                                                         \
    "cdata ID TEXT | gdata TG TEXT "                                                                                  \
    "(data34, cdata1 ...: rate 3/4, 1; "                                                                              \
    "TEXT *N: N generated characters) | "                                                                             \
    "interrupt ID [TALK_MS] | ginterrupt TG [TALK_MS] | als ID | alsend ID | "                                        \
    "setstatus N | missack N | off | on | cancel | ambe FILE|off (any MS: all MSs' voice, raw 9-octet AMBE+2 frames)"

#define ACTS 256

/* data, cdata, gdata with a rate after them (none, 34, 1): its RDOF_T3MS_F_RATE* flag (0 rate 1/2); -1 not one */
static int act_data_rate(const char *what)
{
    const char *r = what + (what[0] == 'c' || what[0] == 'g');
    if (strncmp(r, "data", 4))
        return -1;
    r += 4;
    return !*r ? 0 : !strcmp(r, "34") ? RDOF_T3MS_F_RATE34 : !strcmp(r, "1") ? RDOF_T3MS_F_RATE1 : -1;
}

static int act_cmp(const void *a, const void *b)
{
    const int64_t x = ((const act_t *)a)->at_ms, y = ((const act_t *)b)->at_ms;
    return x < y ? -1 : x > y;
}

/* ACT_HELP (a # starts a comment; status N 0-127, 126 the emergency alarm; sdm up to 46 characters; data, cdata
 * (confirmed) and gdata up to 64 characters, or *N for N generated ones (up to AIR_FILL_MAX: a message of up to 127
 * blocks), a packet data call carrying them as a text message, UTF-16BE on UDP/IPv4 port 5016 (361-3 5.7.1), from
 * 12.x.y.z to 12.x.y.z, to a talkgroup 225.x.y.z, or (@A.B.C.D after the ID) to that address, a host through the
 * site's IP gateway, in rate 1/2 blocks; data34, cdata34, gdata34 in rate 3/4 blocks, data1, cdata1, gdata1 in rate
 * 1): 1 an action in *x (at_ms 0), 0 a blank line, -1 a bad one. */
static int act_parse(const char *text, act_t *x)
{
    char line[256];
    snprintf(line, sizeof line, "%s", text);
    char *h = strchr(line, '#');
    if (h)
        *h = 0;
    unsigned long id, a1 = 0, a2 = 0;
    char what[24];
    const int got = sscanf(line, "%lu %23s %lu %lu", &id, what, &a1, &a2);
    if (got <= 0)
        return 0;
    *x = (act_t){.id = (uint32_t)id};
    if (got < 2 || !id)
        return -1;
    if (!strcmp(what, "register"))
        x->what = RDOF_T3MS_DO_REGISTER, x->arg = (uint32_t)a1;
    else if (!strcmp(what, "deregister"))
        x->what = RDOF_T3MS_DO_DEREGISTER;
    else if ((!strcmp(what, "group") || !strcmp(what, "egroup")) && got >= 3)
        x->what = RDOF_T3MS_DO_GROUP, x->arg = (uint32_t)a1, x->talk_ms = got >= 4 ? (uint32_t)a2 : 2000,
        x->flags = what[0] == 'e' ? RDOF_T3MS_F_EMERGENCY : 0;
    else if (!strcmp(what, "private") && got >= 3)
        x->what = RDOF_T3MS_DO_PRIVATE, x->arg = (uint32_t)a1, x->talk_ms = got >= 4 ? (uint32_t)a2 : 2000;
    else if (!strcmp(what, "als") && got >= 3)
        x->what = RDOF_T3MS_DO_PRIVATE, x->arg = (uint32_t)a1, x->flags = RDOF_T3MS_F_ALS;
    else if ((!strcmp(what, "interrupt") || !strcmp(what, "ginterrupt")) && got >= 3)
        x->what = RDOF_T3MS_DO_INTERRUPT, x->arg = (uint32_t)a1, x->talk_ms = got >= 4 ? (uint32_t)a2 : 2000,
        x->flags = what[0] == 'g' ? RDOF_T3MS_F_TO_GROUP : 0;
    else if (!strcmp(what, "alsend") && got >= 3)
        x->what = RDOF_T3MS_DO_INTERRUPT, x->arg = (uint32_t)a1, x->flags = RDOF_T3MS_F_ALS;
    else if ((!strcmp(what, "status") || !strcmp(what, "gstatus")) && got >= 4 && a2 <= 127)
        x->what = RDOF_T3MS_DO_STATUS, x->arg = (uint32_t)a1, x->talk_ms = (uint32_t)a2,
        x->flags = what[0] == 'g' ? RDOF_T3MS_F_TO_GROUP : 0;
    else if ((!strcmp(what, "sdm") || !strcmp(what, "gsdm") || act_data_rate(what) >= 0) && got >= 3) {
        const bool sdm = strstr(what, "sdm") != NULL;
        /* the text: after the destination */
        const char *t = strstr(line, what) + strlen(what);
        t += strspn(t, " \t");
        t += strspn(t, "0123456789");
        t += strspn(t, " \t");
        unsigned b[4];
        int used = 0;
        if (!sdm && t[0] == '@') { /* the datagram's destination: a host through the site's IP gateway, say */
            if (sscanf(t, "@%u.%u.%u.%u%n", &b[0], &b[1], &b[2], &b[3], &used) != 4 ||
                (b[0] | b[1] | b[2] | b[3]) > 255)
                return -1;
            x->ip = b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
            t += used;
            t += strspn(t, " \t");
        }
        size_t n = strcspn(t, "\r\n");
        if (!sdm && t[0] == '*') {
            const unsigned long f = strtoul(t + 1, NULL, 10);
            if (f < 1 || f > AIR_FILL_MAX)
                return -1;
            x->fill = (uint16_t)f, n = 0;
        } else if (!n || n > (sdm ? 46 : sizeof x->text))
            return -1;
        memcpy(x->text, t, n);
        x->what = sdm ? ACT_SDM : ACT_DATA, x->arg = (uint32_t)a1, x->len = (uint8_t)n,
        x->flags = (what[0] == 'g' ? RDOF_T3MS_F_GROUP : 0) | (what[0] == 'c' ? RDOF_T3MS_F_CONFIRMED : 0) |
                   (sdm ? 0 : (uint8_t)act_data_rate(what));
    } else if (!strcmp(what, "missack") && got >= 3 && a1 <= 255)
        x->what = ACT_MISSACK, x->arg = (uint32_t)a1;
    else if (!strcmp(what, "upoll") && got >= 3 && (got < 4 || (a2 >= 1 && a2 <= 4)))
        x->what = ACT_UPOLL, x->arg = (uint32_t)a1, x->len = got >= 4 ? (uint8_t)a2 : 4;
    else if ((!strcmp(what, "setpoll") || !strcmp(what, "setsupp")) && got >= 2) {
        const char *t = strstr(line, what) + strlen(what);
        t += strspn(t, " \t");
        const size_t n = strcspn(t, "\r\n");
        if (n > 46)
            return -1;
        memcpy(x->text, t, n);
        x->what = what[3] == 'p' ? ACT_SETPOLL : ACT_SETSUPP, x->len = (uint8_t)n;
    } else if (!strcmp(what, "ipadv")) {
        /* the address (default the MS's own, 12.x.y.z), "del" deleting it */
        const char *t = strstr(line, what) + strlen(what);
        unsigned b[4];
        int used = 0;
        x->what = ACT_IPADV;
        if (sscanf(t, " %u.%u.%u.%u%n", &b[0], &b[1], &b[2], &b[3], &used) == 4) {
            if (b[0] > 255 || b[1] > 255 || b[2] > 255 || b[3] > 255)
                return -1;
            x->arg = b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
            t += used;
        } else
            x->arg = AIR_RADIO_NET | (x->id & 0xFFFFFF);
        t += strspn(t, " \t");
        if (!strncmp(t, "del", 3))
            x->flags = 1;
        else if (*t && *t != '\r' && *t != '\n')
            return -1;
    } else if (!strcmp(what, "setstatus") && got >= 3 && a1 <= 127)
        x->what = ACT_SETSTATUS, x->arg = (uint32_t)a1;
    else if (!strcmp(what, "cancel"))
        x->what = RDOF_T3MS_DO_CANCEL;
    else if (!strcmp(what, "off") || !strcmp(what, "on"))
        x->what = ACT_POWER, x->arg = what[1] == 'n';
    else if (!strcmp(what, "ambe") && got >= 2) {
        const char *t = strstr(line, what) + strlen(what);
        t += strspn(t, " \t");
        const size_t n = strcspn(t, " \t\r\n");
        if (!n || n >= sizeof x->text)
            return -1;
        memcpy(x->text, t, n);
        x->text[n] = 0;
        x->what = ACT_AMBE;
    } else
        return -1;
    return 1;
}


/* UTF-8 text (n octets) as UTF-16BE, the BMP only (anything else U+FFFD): the octets written */
static size_t utf16be(const char *s, size_t n, uint8_t *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 2 <= cap;) {
        const uint8_t c = (uint8_t)s[i];
        uint32_t u = 0xFFFD;
        int len = 1;
        if (c < 0x80)
            u = c;
        else if ((c & 0xE0) == 0xC0 && i + 1 < n)
            u = (c & 0x1Fu) << 6 | (s[i + 1] & 0x3Fu), len = 2;
        else if ((c & 0xF0) == 0xE0 && i + 2 < n)
            u = (c & 0x0Fu) << 12 | (s[i + 1] & 0x3Fu) << 6 | (s[i + 2] & 0x3Fu), len = 3;
        else if ((c & 0xF8) == 0xF0)
            len = 4;
        i += (size_t)len;
        out[o++] = (uint8_t)(u >> 8);
        out[o++] = (uint8_t)u;
    }
    return o;
}

/* An action on MS ms: false when its queue is full. */
/* ambe FILE: the simulated MSs' voice from the file's frames (a recording to test a radio's decoding of speech) */
static void act_ambe(const char *path)
{
    static uint8_t frames[4096 * 9];
    if (!strcmp(path, "off")) {
        rdof_t3sim_ambe(NULL, 0);
        fprintf(stderr, "tier3: air MSs: near-silence voice\n");
        return;
    }
    rdof_t3sim_ambe(NULL, 0); /* not read while the file overwrites it */
    FILE *f = fopen(path, "rb");
    const size_t n = f ? fread(frames, 9, sizeof frames / 9, f) : 0;
    if (f)
        fclose(f);
    rdof_t3sim_ambe(frames, n);
    fprintf(stderr, "tier3: air MSs: voice from %s, %zu frames%s\n", path, n, n ? "" : " (none: near-silence)");
}

static bool act_run(rdof_t3ms_t *ms, const act_t *a, int64_t now)
{
    if (a->what == ACT_AMBE) {
        act_ambe(a->text);
        return true;
    }
    if (a->what == ACT_SETSTATUS) {
        rdof_t3ms_set_status(ms, (uint8_t)a->arg);
        return true;
    }
    if (a->what == ACT_SETPOLL)
        return rdof_t3ms_set_poll(ms, 4 /* ISO 8-bit */, (const uint8_t *)a->text, a->len);
    if (a->what == ACT_SETSUPP)
        return rdof_t3ms_set_supp(ms, (const uint8_t *)a->text, a->len);
    if (a->what == ACT_MISSACK) {
        rdof_t3ms_miss_acks(ms, (uint8_t)a->arg);
        return true;
    }
    if (a->what == ACT_POWER) {
        tsay("%u %s: --air-ms only", (unsigned)a->id, a->arg ? "on" : "off");
        return true;
    }
    if (a->what == ACT_UPOLL)
        return rdof_t3ms_upoll(ms, a->arg, 4, a->len, now);
    if (a->what == ACT_IPADV)
        return rdof_t3ms_ip_advice(ms, a->arg, !(a->flags & 1), now);
    /* a status sent is the one it answers a poll with after (as a radio does) */
    if (a->what == RDOF_T3MS_DO_STATUS && a->talk_ms < 127)
        rdof_t3ms_set_status(ms, (uint8_t)a->talk_ms);
    if (a->what == ACT_SDM)
        return rdof_t3ms_sdm(ms, a->arg, a->flags, 4 /* ISO 8-bit */, (const uint8_t *)a->text, a->len, now);
    if (a->what == ACT_DATA) {
        const rdof_ipgw_net_t net = {AIR_RADIO_NET, 0xFF000000u};
        const uint32_t dip = a->ip                              ? a->ip
                             : a->flags & RDOF_T3MS_F_GROUP ? RDOF_IPGW_GROUP_NET | (a->arg & 0xFFFFFF)
                                                            : rdof_ipgw_radio_ip(&net, a->arg);
        static uint16_t ip_id;
        static char fill[AIR_FILL_MAX];
        static uint8_t u16[2 * AIR_FILL_MAX], d[28 + sizeof u16];
        for (size_t k = 0; k < a->fill; k++)
            fill[k] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"[k % 36];
        const size_t m = a->fill ? utf16be(fill, a->fill, u16, sizeof u16) : utf16be(a->text, a->len, u16, sizeof u16);
        const size_t n = rdof_ipgw_udp_datagram(rdof_ipgw_radio_ip(&net, a->id), 5016, dip, 5016, ++ip_id, u16, m, d,
                                                sizeof d);
        return n && rdof_t3ms_data(ms, a->arg, a->flags, d, n, now);
    }
    return rdof_t3ms_do(ms, a->what, a->arg, a->talk_ms, a->flags, now);
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
        double at;
        int used = 0;
        act_t x;
        if (sscanf(line, " %lf%n", &at, &used) < 1) {
            char *h = strchr(line, '#');
            if (h)
                *h = 0;
            if (strspn(line, " \t\r\n") == strlen(line))
                continue;
            goto bad;
        }
        const int r = act_parse(line + used, &x);
        if (r == 0)
            continue;
        if (r < 0 || at < 0)
            goto bad;
        x.at_ms = t0 + (int64_t)(at * 1000);
        if (n == max) {
            tsay("%s: more than %d actions", path, max);
            fclose(f);
            return -1;
        }
        acts[n++] = x;
        continue;
    bad:
        tsay("%s:%d: AT_S " ACT_HELP, path, ln);
        fclose(f);
        return -1;
    }
    fclose(f);
    qsort(acts, (size_t)n, sizeof *acts, act_cmp);
    return n;
}

static const char *const ms_state[] = {"idle", "access", "waiting", "payload"};

int rdofradio_t3_sim(const rdofradio_cfg_t *c, const rdofradio_t3_opts_t *o, volatile sig_atomic_t *stop)
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
    static uint32_t ids[RDOF_T3SIM_MS];
    int n = o->sim_n < RDOF_T3SIM_MS ? o->sim_n : RDOF_T3SIM_MS;
    for (int i = 0; i < n; i++)
        ids[i] = o->sim_id + (uint32_t)i;
    for (int a = 0; a < nacts; a++) {
        bool known = false;
        for (int i = 0; i < n && !known; i++)
            known = ids[i] == acts[a].id;
        if (!known && n < RDOF_T3SIM_MS)
            ids[n++] = acts[a].id;
    }
    static rdof_t3ms_t *ms[RDOF_T3SIM_MS];
    const rdof_t3_cfg_t *cfg = rdof_t3unit_cfg(T.u);
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
    static rdof_t3sim_t sim;
    rdof_t3sim_init(&sim, T.u, n, ms, ids);
    const bool ext = o->sim_ext != NULL;
    if (ext && !rdofradio_simext_open(&sim, o->sim_ext, o->sim_ext_loss, o->sim_ext_err, T.rng, o->verbose, err,
                                      sizeof err)) {
        tsay("%s", err);
        stop_common();
        return 1;
    }
    tsay("--sim-air: %d MSs (%u..%u%s), talkgroup %u", n, (unsigned)o->sim_id, (unsigned)(o->sim_id + o->sim_n - 1),
         n > o->sim_n ? " and the script's" : "", (unsigned)o->sim_tg);

    int64_t k = t0 / 30, next_talk = o->sim_talk_s > 0 ? t0 + (int64_t)(o->sim_talk_s * 1000) : -1;
    int64_t last_poll = 0, last_status = t0, ahead_ms = 0;
    int ai = 0, talker = 0;
    uint32_t skipped = 0;
    rdof_t3unit_run(T.u, k, t0);
    while (!*stop && (o->seconds <= 0 || (ext ? k * 30 : wall_ms()) - t0 < (int64_t)(o->seconds * 1000))) {
        /* with external MSs the slots go one at a time, each answered: their time is the slot's, never skipped.
           --sim-fast while a client is there: unpaced, the slot clock running ahead of the wall clock (ahead_ms) */
        const int64_t wall = wall_ms(), now = ext ? (k + 1) * 30 : wall;
        if (ext && o->sim_fast && rdofradio_simext_clients() > 0) {
            if (now - wall > ahead_ms)
                ahead_ms = now - wall;
        } else if (ext && now > wall + ahead_ms) {
            struct timespec ts = {0, (long)(now - wall - ahead_ms > 20 ? 20 : now - wall - ahead_ms) * 1000000L};
            nanosleep(&ts, NULL);
            rdofradio_simext_wait(k); /* clients joining between slots */
            continue;
        }
        const int64_t k_now = now / 30;
        if (!ext && k_now - k > 50) { /* stalled (a suspended process): the air jumps ahead */
            skipped += (uint32_t)(k_now - k);
            k = k_now - 1;
        }
        for (; ai < nacts && acts[ai].at_ms <= now; ai++) {
            const int i = rdof_t3sim_find(&sim, acts[ai].id);
            if (i >= 0)
                act_run(ms[i], &acts[ai], now);
        }
        if (next_talk >= 0 && now >= next_talk && o->sim_n > 0) {
            rdof_t3ms_do(ms[talker], RDOF_T3MS_DO_GROUP, o->sim_tg, 2000, 0, now);
            talker = (talker + 1) % o->sim_n;
            next_talk = now + (int64_t)(o->sim_talk_s * 1000);
        }
        pthread_mutex_lock(&T.mu);
        while (k < k_now) {
            k++;
            rdof_t3sim_slot(&sim, k, k * 30);
        }
        if (now - last_poll >= 20) {
            last_poll = now;
            poll_locked(now);
        }
        pthread_mutex_unlock(&T.mu);
        if (ext)
            rdofradio_simext_wait(k);
        if (now - last_status >= 10000) {
            last_status = now;
            int reg = 0;
            for (int i = 0; i < n; i++) {
                rdof_t3ms_status_t st;
                rdof_t3ms_status(ms[i], &st);
                reg += st.registered;
            }
            char extra[96] = "";
            if (ext)
                snprintf(extra, sizeof extra, "; external: %d clients, %u bursts (%u on no slot)",
                         rdofradio_simext_clients(), (unsigned)sim.ext_in_n, (unsigned)sim.ext_drop);
            tsay("%d of %d MSs registered; out %u PDUs, in %u PDUs %u voice, %u collisions; net in %u, setups %u/%u%s%s",
                 reg, n, (unsigned)sim.out_pdus, (unsigned)sim.in_pdus, (unsigned)sim.in_voice,
                 (unsigned)sim.collisions, (unsigned)rdof_t3unit_counts(T.u)->net_in, (unsigned)rdof_t3unit_counts(T.u)->setups_out,
                 (unsigned)rdof_t3unit_counts(T.u)->setups_in, extra, skipped ? "; slots skipped" : "");
            if (o->verbose)
                for (int i = 0; i < n; i++) {
                    rdof_t3ms_status_t st;
                    rdof_t3ms_status(ms[i], &st);
                    tsay("  %u: %s on ch %d", (unsigned)ids[i],
                         st.state < (int)(sizeof ms_state / sizeof *ms_state) ? ms_state[st.state] : "?", st.ch);
                }
        }
        /* to the next slot's start */
        const int64_t wait = ext ? 0 : (k_now + 1) * 30 - wall_ms();
        if (wait > 0) {
            struct timespec ts = {0, (long)(wait > 20 ? 20 : wait) * 1000000L};
            nanosleep(&ts, NULL);
        }
    }
    if (ext)
        rdofradio_simext_close();
    stop_common();
    for (int i = 0; i < n; i++)
        free(ms[i]);
    return 0;
}

/* ── on a device ──────────────────────────────────────────────────────────── */

/* --air-ms: simulated MSs (t3ms) on the device's site. Each slot they hear what carrier 1 sends (its outbound,
 * decoded as a radio would) and what they send goes to the site as if heard on carrier 1's inbound, beside the real
 * radios on the air: a radio can register next to them, call them, hear their calls on the payload channel. Driven by
 * --sim-script and --sim-talk as --sim-air, and by MQTT: ROOT/node/NODE/air-ms, a script line without its time ("ID
 * private 3105738 5000"). Carriers 2 to 4 too when the SDR sends them (their outbound heard, their inbound sent; not
 * a split's beside them), and every carrier when all are splits (each slot as its pump sent it to the half). */
static struct {
    int n;
    rdof_t3ms_t *ms[RDOF_T3SIM_MS];
    uint32_t id[RDOF_T3SIM_MS];
    int voice_n[RDOF_T3SIM_MS];
    rdof_t3ms_status_t last[RDOF_T3SIM_MS];
    uint32_t sd_seen[RDOF_T3SIM_MS], pd_sum[RDOF_T3SIM_MS], supp_seen[RDOF_T3SIM_MS]; /* what air_inbox said */
    act_t acts[ACTS];
    int nacts, ai, talker;
    int64_t next_talk;
    uint32_t in, out, bad;
    uint64_t off; /* bit i: ms[i] switched off ("ID off"): it misses what goes out meanwhile */
    int ncar; /* the carriers they're on: 1, and those the SDR sends; every carrier when all are splits */
} AM;
static int nsdr; /* the carriers after carrier 1 the SDR sends (CX[0..nsdr-1]: carriers 2 to nsdr + 1) */
static bool all_split; /* every carrier on a split TX half, carrier 1's too: the SDR receives only */

static int air_find(uint32_t id)
{
    for (int i = 0; i < AM.n; i++)
        if (AM.id[i] == id)
            return i;
    return -1;
}

/* The air MSs' authentication keys, from the cfg/ms documents (tier3.md 3.14); again when provisioning changes. */
static uint32_t air_cfg_changes;

static void air_keys(void)
{
    const rdof_mn_cfg_t *mc = rdof_t3unit_mc(T.u);
    air_cfg_changes = mc->changes;
    for (int i = 0; i < AM.n; i++) {
        const rdof_mn_ms_cfg_t *m = rdof_mn_cfg_ms(mc, AM.id[i]);
        rdof_t3ms_key(AM.ms[i], m && m->has_key ? m->key : NULL, m ? m->psn : 0, m && m->has_psn);
    }
}

static bool air_start(const rdofradio_t3_opts_t *o, char *err, size_t cap)
{
    if (o->air_ms <= 0 && !o->sim_script)
        return true;
    const int64_t t0 = wall_ms();
    if (o->sim_script && (AM.nacts = script_read(o->sim_script, t0, AM.acts, ACTS)) < 0) {
        snprintf(err, cap, "%s: a bad script", o->sim_script);
        return false;
    }
    int n = o->air_ms < RDOF_T3SIM_MS ? o->air_ms : RDOF_T3SIM_MS;
    for (int i = 0; i < n; i++)
        AM.id[i] = o->sim_id + (uint32_t)i;
    for (int a = 0; a < AM.nacts && n < RDOF_T3SIM_MS; a++) {
        bool known = false;
        for (int i = 0; i < n && !known; i++)
            known = AM.id[i] == AM.acts[a].id;
        if (!known)
            AM.id[n++] = AM.acts[a].id;
    }
    const rdof_t3_cfg_t *cfg = rdof_t3unit_cfg(T.u);
    uint16_t chans[RDOF_T3_CARRIERS];
    for (int k = 0; k < cfg->ncarriers; k++)
        chans[k] = cfg->carriers[k].chan;
    const uint32_t tg[1] = {o->sim_tg};
    AM.ncar = all_split ? rdof_t3unit_cfg(T.u)->ncarriers : 1 + nsdr;
    for (int i = 0; i < n; i++) {
        AM.ms[i] = malloc(rdof_t3ms_size());
        if (!AM.ms[i]) {
            snprintf(err, cap, "no memory");
            return false;
        }
        rdof_t3ms_init(AM.ms[i], AM.id[i], tg, 1, chans, AM.ncar, T.rng ^ (AM.id[i] * 2654435761u));
        char reply[24]; /* a UDT short data poll's answer until the script sets one */
        const int rn = snprintf(reply, sizeof reply, "air MS %u", (unsigned)AM.id[i]);
        rdof_t3ms_set_poll(AM.ms[i], 4, (const uint8_t *)reply, (size_t)rn);
        if (i < o->air_ms)
            rdof_t3ms_do(AM.ms[i], RDOF_T3MS_DO_REGISTER, o->sim_tg, 0, 0, t0 + 3000 + 700 * i);
        rdof_t3ms_status(AM.ms[i], &AM.last[i]);
    }
    AM.next_talk = o->sim_talk_s > 0 ? t0 + (int64_t)(o->sim_talk_s * 1000) : -1;
    AM.n = n;
    air_keys();
#ifdef RDOF_MQTT
    if (T.net)
        rdof_mn_node_topic(air_topic, sizeof air_topic, T.c->t.root, T.node, "air-ms");
#endif
    tsay("air MSs: %d (%u..%u), talkgroup %u, on carrier%s %d%s%s", n, (unsigned)AM.id[0], (unsigned)AM.id[n - 1],
         (unsigned)o->sim_tg, AM.ncar > 1 ? "s 1 to" : "", AM.ncar, air_topic[0] ? "; commands on " : "", air_topic);
    return true;
}

static void air_do(const act_t *a, int64_t now)
{
    const int i = air_find(a->id);
    if (i < 0) {
        tsay("air MSs: no %u here", (unsigned)a->id);
        return;
    }
    if (a->what == ACT_POWER) { /* off, it misses what goes out meanwhile; on, it carries on where it was */
        AM.off = a->arg ? AM.off & ~(1ull << i) : AM.off | 1ull << i;
        tsay("air MS %u: switched %s", (unsigned)a->id, a->arg ? "on" : "off");
        return;
    }
    if (!act_run(AM.ms[i], a, now))
        tsay("air MS %u: its queue is full", (unsigned)a->id);
}

/* T.mu held */
static bool air_command(const char *topic, const void *data, size_t len)
{
    if (!air_topic[0] || strcmp(topic, air_topic))
        return false;
    char line[200];
    snprintf(line, sizeof line, "%.*s", (int)(len < sizeof line ? len : sizeof line - 1), (const char *)data);
    act_t a;
    const int r = act_parse(line, &a);
    if (r > 0)
        air_do(&a, wall_ms());
    else if (r < 0)
        tsay("air MSs: \"%s\": " ACT_HELP, line);
    return true;
}

static const char *const air_state[] = {"idle", "access", "waiting", "payload"};

/* n octets as text, the unprintable ones as \xHH */
static void air_text(const uint8_t *d, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 5 < cap; i++)
        o += (size_t)snprintf(out + o, cap - o, d[i] >= 32 && d[i] < 127 && d[i] != '\\' ? "%c" : "\\x%02x", d[i]);
    out[o] = 0;
}

/* what MS i took since the last look, said: short data, and packet data (a UDP/IPv4 datagram's payload) */
static void air_inbox(int i)
{
    static uint8_t d[RDOF_IPGW_MAX_BURSTS * 24]; /* a message of up to 127 rate 1 blocks */
    char t[400];
    size_t n = 0;
    uint32_t src = 0;
    uint8_t fmt = 0;
    const uint32_t count = rdof_t3ms_sd_inbox(AM.ms[i], d, sizeof d, &n, &src, &fmt);
    if (count != AM.sd_seen[i]) {
        AM.sd_seen[i] = count;
        air_text(d, n, t, sizeof t);
        tsay("air MS %u: short data from %u, format %u, %zu octets: \"%s\"", (unsigned)AM.id[i], (unsigned)src,
             fmt, n, t);
    }
    const uint32_t sn = rdof_t3ms_supp_inbox(AM.ms[i], d, sizeof d, &n);
    if (sn != AM.supp_seen[i]) {
        AM.supp_seen[i] = sn;
        while (n > 0 && d[n - 1] == 0)
            n--; /* the blocks' zero padding */
        air_text(d, n, t, sizeof t);
        tsay("air MS %u: supplementary data, %zu octets: \"%s\"", (unsigned)AM.id[i], n, t);
    }
    const int pn = rdof_t3ms_data_inbox(AM.ms[i], d, sizeof d, &src);
    if (pn < 0)
        return;
    uint32_t sum = 2166136261u ^ src; /* FNV-1a: a new message (one identical to the last goes unsaid) */
    for (int k = 0; k < pn && k < (int)sizeof d; k++)
        sum = (sum ^ d[k]) * 16777619u;
    if (sum == AM.pd_sum[i])
        return;
    AM.pd_sum[i] = sum;
    const size_t m = (size_t)pn < sizeof d ? (size_t)pn : sizeof d;
    if (m >= 28 && d[0] == 0x45 && d[9] == 17) { /* IPv4 with no options, UDP */
        const size_t u = m - 28;
        bool text16 = (d[22] << 8 | d[23]) == 5016 && u % 2 == 0; /* a text message, UTF-16BE (361-3 5.7.1) */
        for (size_t k = 0; text16 && k < u; k += 2)
            text16 = d[28 + k] == 0 && d[29 + k] >= 32 && d[29 + k] < 127; /* shown as text when it's ASCII */
        if (text16) {
            uint8_t a[80];
            size_t na = 0;
            for (size_t k = 0; k < u && na < sizeof a; k += 2)
                a[na++] = d[29 + k];
            air_text(a, na, t, sizeof t);
            /* a long one: whether it is the *N pattern, whole */
            bool pat = u / 2 > sizeof a;
            for (size_t k = 0; pat && k < u / 2; k++)
                pat = d[29 + 2 * k] == "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"[k % 36];
            tsay("air MS %u: packet data from %u, UDP %u.%u.%u.%u:%u to %u.%u.%u.%u:%u, %zu octets, UTF-16BE text: "
                 "\"%s\"%s",
                 (unsigned)AM.id[i], (unsigned)src, d[12], d[13], d[14], d[15], d[20] << 8 | d[21], d[16], d[17],
                 d[18], d[19], d[22] << 8 | d[23], u, t,
                 u / 2 > sizeof a ? pat ? " (the first 80; all of it the *N pattern)" : " (the first 80)" : "");
            return;
        }
        air_text(d + 28, m - 28, t, sizeof t);
        tsay("air MS %u: packet data from %u, UDP %u.%u.%u.%u:%u to %u.%u.%u.%u:%u, %zu octets: \"%s\"",
             (unsigned)AM.id[i], (unsigned)src, d[12], d[13], d[14], d[15], d[20] << 8 | d[21], d[16], d[17], d[18],
             d[19], d[22] << 8 | d[23], m - 28, t);
    } else {
        air_text(d, m, t, sizeof t);
        tsay("air MS %u: packet data from %u, %d octets: \"%s\"", (unsigned)AM.id[i], (unsigned)src, pn, t);
    }
}

/* trunk slot k's inbound: each MS's burst for it, the first on each carrier heard (the others collide with it); on
 * carrier 1, and carrier 2 when the SDR sends it. Once a slot, with carrier 1's. T.mu held. */
static void hat_air_in(int64_t k, const rdof_burst_t *b);
static bool sim_air_hear(int c, int64_t k, const rdof_burst_t *b);

static void air_in(int64_t k)
{
    if (!AM.n)
        return;
    if (rdof_t3unit_mc(T.u)->changes != air_cfg_changes)
        air_keys();
    const int64_t now = wall_ms();
    for (; AM.ai < AM.nacts && AM.acts[AM.ai].at_ms <= now; AM.ai++)
        air_do(&AM.acts[AM.ai], now);
    if (AM.next_talk >= 0 && now >= AM.next_talk && T.o.air_ms > 0) {
        rdof_t3ms_do(AM.ms[AM.talker], RDOF_T3MS_DO_GROUP, T.o.sim_tg, 2000, 0, now);
        AM.talker = (AM.talker + 1) % T.o.air_ms;
        AM.next_talk = now + (int64_t)(T.o.sim_talk_s * 1000);
    }
    const int cc = rdof_t3unit_cfg(T.u)->cc;
    bool sent[RDOF_T3_CARRIERS] = {false};
    for (int i = 0; i < AM.n; i++) {
        rdof_t3ms_tx_t tx;
        if (AM.off >> i & 1 || !rdof_t3ms_tx(AM.ms[i], k, now, &tx))
            continue;
        rdof_burst_t b;
        const int n = AM.voice_n[i];
        AM.voice_n[i] = tx.kind != 1 && tx.what == RDOF_T3_VOICE_FRAME ? (n + 1) % 6 : 0;
        const int c = tx.ch >> 1;
        if (c >= AM.ncar || sent[c] || !rdof_t3sim_ms_burst(&tx, n, cc, &b))
            continue;
        b.slot = (tx.ch & 1) + 1;
        if (!sim_air_hear(c, k, &b)) /* a simulated RX half hears it (a carrier on two radios), or straight in */
            rdof_t3unit_rx(T.u, c, k, &b, -60);
        if (c == 0)
            hat_air_in(k, &b);
        sent[c] = true;
        AM.in++;
    }
}

/* trunk slot k of carrier c: its outbound burst (aired: one was sent), heard by the MSs on its channels. T.mu held. */
static void air_out(int c, int64_t k, const uint8_t burst[RDOF_BURST_BYTES], bool aired)
{
    if (!AM.n || c >= AM.ncar)
        return;
    /* as a radio decodes it: a PDU, or a data block (12 octets: UDT and packet data; 18, 24) */
    uint8_t pdu[24] = {0};
    int dt = -1, rcc = -1;
    uint8_t mdt = 0;
    if (aired) {
        const rdof_trunk_pdu_t r = rdof_t3rf_burst_block(burst, &dt, &rcc, pdu);
        if (r == RDOF_T3RF_PDU_OK)
            mdt = (uint8_t)dt, AM.out++;
        else if (r == RDOF_T3RF_PDU_BAD)
            AM.bad++;
    }
    const bool at = c == 0 && rdof_t3unit_at(T.u, k) > 0;
    /* a Reverse Channel PDU in slot k is for the talker on the carrier's other slot (361-1 5.1.5) */
    const int rc = aired ? rdof_t3rf_rc_read(burst) : -1;
    for (int i = 0; i < AM.n; i++) {
        rdof_t3ms_status_t st;
        if (AM.off >> i & 1)
            continue;
        if (rc >= 0)
            rdof_t3ms_rc(AM.ms[i], 2 * c + (int)((k & 1) ^ 1), rc);
        rdof_t3ms_status(AM.ms[i], &st);
        if (st.ch >> 1 == c)
            rdof_t3ms_slc(AM.ms[i], c, rdof_t3unit_slc(T.u, c, k)); /* a composite TSCC's P_SYS_Parms */
        if ((st.ch & 1) == (k & 1) && st.ch >> 1 == c)
            rdof_t3ms_rx(AM.ms[i], st.ch, k, mdt, pdu, st.ch == 0 && at);
        rdof_t3ms_status(AM.ms[i], &st);
        rdof_t3ms_status_t *l = &AM.last[i];
        if (st.state != l->state || st.registered != l->registered || st.calls_done != l->calls_done ||
            st.calls_failed != l->calls_failed)
            tsay("air MS %u: %s%s on ch %d; calls %u done, %u failed (reason 0x%02x); heard %u -> %u", (unsigned)AM.id[i],
                 st.state < 4 ? air_state[st.state] : "?", st.registered ? ", registered" : "", st.ch,
                 (unsigned)st.calls_done, (unsigned)st.calls_failed, st.last_reason, (unsigned)st.heard_src,
                 (unsigned)st.heard_dst);
        *l = st;
        air_inbox(i);
    }
}

static void air_stop(void)
{
    for (int i = 0; i < AM.n; i++)
        free(AM.ms[i]);
    AM.n = 0;
    air_topic[0] = 0;
}

static rdof_sdrmodem_t *M;
static rdofradio_t3_dev_t DEV;

/* A modem's slots on the trunk's (UTC) slots. On the host's slot grid (rdofradio_utc.h) the modem's slot k is the UTC
 * slot itself; off it (no map within GRID_WAIT_S) the modem's own count moved by an even d, placed once against the
 * host's clock (which its sample clock then drifts from), again when the modem starts over. */
#define GRID_WAIT_S 10.0
#define CLOCK_WAIT_S 30.0 /* for the host's clock to settle (rdofradio_utc_clock_settled) */
typedef struct {
    bool placed, on_grid;
    int64_t d;      /* trunk k = modem k + d (even: the modem's TS1 is the trunk's LCN 0) */
    int64_t last_k; /* the modem's last slot asked */
} slotmap_t;

/* carrier 1 */
static struct {
    slotmap_t s;
    int chan;         /* the receiver's channel on carrier 1's inbound */
    bool keyed;       /* the key-up held */
    double key_by;    /* hold the key-up by then, on the grid or not */
} A = {.chan = -1};

/* carriers 2 to 4 when the SDR sends them: each its own modem, summed into the transmit stream (rdofradio_t3_mix) */
#define MIX_N 2048 /* samples a pull */
#define CX_N (RDOF_T3_CARRIERS - 1)
typedef struct {
    rdof_sdrmodem_t m;
    int c; /* the carrier: 2 to 4 (the controller's carrier c - 1) */
    bool on;
    slotmap_t s;
    int chan;            /* the receiver's channel on its inbound */
    double lo;           /* the TX LO it was tuned against */
    bool lo_moved;       /* a bench job has the LO elsewhere: unsent */
    uint32_t heard;      /* bursts heard on its inbound, given to the site */
    double pull_s;       /* time in its pulls and the sum */
    uint64_t pulls, samples;
    rdof_cf_t scratch[MIX_N];
} cx_t;
static cx_t CX[CX_N];

/* Modem m's slot k (the source's k) on the trunk's slots, for carrier c (1 to 4). Off the grid carriers 2 to 4 take
 * carrier 1's d when their modems' slots start on the same sample (keyed together), so all count alike. */
static int64_t trunk_k(slotmap_t *p, const rdof_sdrmodem_t *m, int64_t k, int c)
{
    const bool on_grid = m->grid_placed;
    if (on_grid) {
        if (!p->placed || !p->on_grid)
            tsay("carrier %d: on the slot grid (modem slot %lld is the UTC slot)", c, (long long)k);
        p->d = 0;
        p->placed = true;
    } else if (!p->placed || p->on_grid || k < p->last_k) { /* the first, off the grid, or the modem started again */
        if (c >= 2 && A.s.placed && !A.s.on_grid && M && m->base == M->base) {
            p->d = A.s.d;
        } else {
            const int64_t want = wall_ms() / 30 + 2;
            p->d = want - k;
            p->d += p->d & 1;
        }
        p->placed = true;
        tsay("carrier %d: off the slot grid: modem slot %lld is trunk slot %lld (the SDR's clock drifts from it)", c,
             (long long)k, (long long)(k + p->d));
    }
    p->on_grid = on_grid;
    p->last_k = k;
    return k + p->d;
}

static bool source_slot(void *ctx, int64_t k, uint8_t burst[RDOF_BURST_BYTES], int *at, int32_t *slc28)
{
    uint8_t control;
    pthread_mutex_lock(&T.mu);
    rdof_t3unit_set_now(T.u, wall_ms());
    const int64_t tk = trunk_k(&A.s, M, k, 1);
    const bool ok = rdof_t3unit_slot(T.u, 0, tk, burst, &control, at, slc28);
    air_in(tk);
    air_out(0, tk, burst, ok);
    pthread_mutex_unlock(&T.mu);
    return ok;
}

/* carrier x->c's slot source: the controller's carrier x->c - 1 (its two payload channels) */
static bool source_slotx(void *ctx, int64_t k, uint8_t burst[RDOF_BURST_BYTES], int *at, int32_t *slc28)
{
    cx_t *x = ctx;
    uint8_t control;
    pthread_mutex_lock(&T.mu);
    rdof_t3unit_set_now(T.u, wall_ms());
    const int64_t tk = trunk_k(&x->s, &x->m, k, x->c);
    const bool ok = rdof_t3unit_slot(T.u, x->c - 1, tk, burst, &control, at, slc28);
    air_out(x->c - 1, tk, burst, ok);
    pthread_mutex_unlock(&T.mu);
    return ok;
}

static void c2_reply(void *ctx, const uint8_t *frame, size_t len) { (void)ctx, (void)frame, (void)len; }
static int c2_channel(void *ctx, uint32_t hz)
{
    const int c = DEV.channel(hz);
    return c >= 0 ? c : RDOF_SDRMODEM_NOT_HEARD;
}
static bool c2_tune(void *ctx, uint32_t hz, double *off)
{
    double reach;
    return DEV.tx_offset(hz, off, &reach);
}

static void c2_stop(void)
{
    for (int i = 0; i < CX_N; i++) {
        if (!CX[i].on)
            continue;
        rdof_sdrmodem_set_source(&CX[i].m, NULL);
        rdof_sdrmodem_free(&CX[i].m);
        CX[i].on = false;
    }
    nsdr = 0;
}

/* Every carrier's out within the transmit stream's reach of the one TX LO (rdofradio puts it in the middle) */
static bool c2_fits(const rdofradio_t3_cfg_t *t, char *err, size_t cap)
{
    const double lo = DEV.tx_lo();
    uint32_t lo_hz = t->car[0].tx_hz, hi_hz = t->car[0].tx_hz;
    for (int c = 1; c < t->ncar; c++) {
        lo_hz = t->car[c].tx_hz < lo_hz ? t->car[c].tx_hz : lo_hz;
        hi_hz = t->car[c].tx_hz > hi_hz ? t->car[c].tx_hz : hi_hz;
    }
    for (int c = 0; c < t->ncar; c++) {
        double off, reach;
        if (DEV.tx_offset(t->car[c].tx_hz, &off, &reach))
            continue;
        snprintf(err, cap, "%d carriers out from %.5f to %.5f MHz, %.1f kHz: the TX LO on %.5f MHz reaches %.1f kHz "
                           "either side at %.0f kS/s (the transmit stream's rate), carrier %d is %.1f kHz off it",
                 t->ncar, lo_hz / 1e6, hi_hz / 1e6, ((double)hi_hz - (double)lo_hz) / 1e3, lo / 1e6, reach / 1e3,
                 DEV.tx_fs / 1e3, c + 1, fabs(off) / 1e3);
        return false;
    }
    return true;
}

/* carrier c's modem (c 2 to 4, in CX[c - 2]): duplex, tuned (in and out), DMR, its slot source; keyed with carrier 1
   (key_carrier1) */
static bool c2_start(const rdofradio_t3_cfg_t *t, int c, const rdof_sdrmodem_t *m1, char *err, size_t cap)
{
    cx_t *x = &CX[c - 2];
    if (!DEV.tx_offset || !DEV.tx_lo || !DEV.tx_fs) {
        snprintf(err, cap, "carrier %d: this device can't send more than one carrier", c);
        return false;
    }
    const uint32_t rx = t->car[c - 1].rx_hz, tx = t->car[c - 1].tx_hz;
    memset(x, 0, sizeof *x);
    x->c = c;
    x->chan = DEV.channel(rx);
    if (x->chan < 0) {
        snprintf(err, cap, "carrier %d: the receiver has no channel on its inbound %.5f MHz", c, rx / 1e6);
        return false;
    }
    const rdof_sdrmodem_io_t io = {c2_reply, c2_channel, c2_tune, NULL, NULL, NULL};
    if (rdof_sdrmodem_init(&x->m, &io, DEV.rx_fs, DEV.tx_fs, true)) {
        snprintf(err, cap, "carrier %d: modem: %s", c, x->m.error);
        return false;
    }
    rdof_sdrmodem_carrier_ppm(&x->m, m1->carrier_ppm);
    /* as the repeater sets carrier 1's: duplex (SET_CONFIG without the simplex flag) on the site's CC, both
       frequencies, DMR */
    uint8_t config[23] = {0}, freq[14], f[64];
    config[1] = RDOF_DMR_ENABLED;
    config[3] = RDOF_MODE_DMR;
    config[6] = (uint8_t)(t->cfg.cc & 0x0F);
    const uint8_t mode = RDOF_MODE_DMR;
    rdof_sdrmodem_write(&x->m, f, rdof_mmdvm_frame(RDOF_MMDVM_SET_CONFIG, config, sizeof config, f, sizeof f));
    rdof_sdrmodem_write(&x->m, f,
                        rdof_mmdvm_frame(RDOF_MMDVM_SET_FREQ, freq, rdof_mmdvm_set_freq_payload(rx, tx, 255, freq), f,
                                         sizeof f));
    rdof_sdrmodem_write(&x->m, f, rdof_mmdvm_frame(RDOF_MMDVM_SET_MODE, &mode, 1, f, sizeof f));
    if (!x->m.duplex || !x->m.tuned || x->m.mode != RDOF_MODE_DMR) {
        snprintf(err, cap, "carrier %d: its modem refused the settings%s%s", c, x->m.error[0] ? ": " : "", x->m.error);
        rdof_sdrmodem_free(&x->m);
        return false;
    }
    const rdof_sdrmodem_source_t src = {source_slotx, x};
    rdof_sdrmodem_set_source(&x->m, &src);
    x->lo = DEV.tx_lo();
    x->on = true;
    nsdr = c - 1;
    double off, reach;
    DEV.tx_offset(tx, &off, &reach);
    tsay("carrier %d: sent by the SDR (tx %.5f, rx %.5f MHz), %+.1f kHz from the TX LO %.5f MHz, %.0f kS/s; each "
         "carrier at 1/%d amplitude",
         c, tx / 1e6, rx / 1e6, off / 1e3, x->lo / 1e6, DEV.tx_fs / 1e3, t->ncar);
    return true;
}

/* Carriers on split TX halves (rdof/sdr/t3split.h): carrier 2 of two beside the SDR's carrier 1, or every carrier
 * (the TSCC's too: its half takes the CACH with each burst), the SDR's receiver their RX half; or every carrier on two
 * radios, a split TX half and a split RX half, and no SDR (rdofradio_t3_radios). Each radio its port and a reader
 * thread. With RDOF_SIM_SPLIT (the host tests' build) the port "sim-split" (and any name it starts) is a simulated
 * radio (host-tests/fake_radio.h) on a socket pair: a TX half, or an RX half with RX_META, which hears the air MSs. */
#define BQ 64
#define SIM_AIR 64
typedef struct sp sp_t;
typedef struct {
    sp_t *p;
    bool rx; /* the RX half's radio */
    bool open;
    int fd;
    pthread_t thread;
    volatile bool quit, gone;
    pthread_mutex_t mu;
    struct {
        uint8_t b[256];
        int n;
        double t; /* when it came: the split times the halves' replies by it (its clock sync), the slots by it */
    } q[BQ];
    int head, tail;
    uint32_t dropped;
#ifdef RDOF_SIM_SPLIT
    bool sim;
    int sim_fd;
    pthread_t sim_thread;
    volatile bool sim_quit;
    fake_radio_t *fake;
    double sim_clock;
    struct {
        int64_t k;
        uint8_t control, burst[RDOF_BURST_BYTES];
    } air[SIM_AIR]; /* the air MSs' bursts for a simulated RX half, heard once their slot is over (under mu) */
    int air_head, air_tail;
#endif
} pt_t;
struct sp {
    bool on, up;
    int c; /* the carrier, 0 to 3 */
    int chan;
    bool radios; /* its in on a radio (the RX half), no SDR */
    pt_t tx, rx;
    rdof_t3split_t ts;
    int64_t heard_k; /* the SDR: the UTC slot of the last burst heard on its inbound */
    int align_said;  /* the TX half's alignment as last said */
    uint32_t by_meta, by_parity; /* the RX half's bursts placed by RX_META's slot number, by their label's parity */
};
static sp_t SP[RDOF_T3_CARRIERS];
static double utc_minus_host;     /* UTC - the monotonic clock, µs */
static double next_utc;           /* when to measure it again */
static int64_t air_k;             /* all split: the last slot the air MSs had */

static double utc_minus_host_us(void)
{
    struct timespec w;
    const double h0 = DEV.now();
    clock_gettime(CLOCK_REALTIME, &w);
    const double h1 = DEV.now();
    return ((double)w.tv_sec * 1e6 + w.tv_nsec / 1e3) - 1e6 * (h0 + h1) / 2;
}

static int64_t utc_slot_at(double t) { return (int64_t)floor((t * 1e6 + utc_minus_host) / 30000.0); }
static int64_t utc_slot_now(void) { return utc_slot_at(DEV.now()); }

static void *b_reader(void *arg)
{
    pt_t *x = arg;
    while (!x->quit) {
        struct pollfd pf = {x->fd, POLLIN, 0};
        if (poll(&pf, 1, 100) <= 0)
            continue;
        uint8_t b[256];
        const int n = rdofradio_serial_read(x->fd, b, sizeof b);
        const double t = DEV.now();
        if (n < 0 || (pf.revents & (POLLHUP | POLLERR | POLLNVAL))) {
            x->gone = true;
            break;
        }
        if (n == 0)
            continue;
        pthread_mutex_lock(&x->mu);
        if (x->head - x->tail < BQ) {
            const int i = x->head++ % BQ;
            x->q[i].n = n;
            x->q[i].t = t;
            memcpy(x->q[i].b, b, (size_t)n);
        } else {
            x->dropped += (uint32_t)n;
        }
        pthread_mutex_unlock(&x->mu);
    }
    return NULL;
}

static void pt_write(pt_t *x, const uint8_t *d, size_t n)
{
    if (!rdofradio_serial_write(x->fd, d, n) && !x->gone) {
        x->gone = true;
        tsay("carrier %d: the %s half's port write failed", x->p->c + 1, x->rx ? "RX" : "TX");
    }
}
static void b_to_radio(void *ctx, const uint8_t *d, size_t n) { pt_write(&((sp_t *)ctx)->tx, d, n); }
static void b_to_rx_radio(void *ctx, const uint8_t *d, size_t n) { pt_write(&((sp_t *)ctx)->rx, d, n); }
static int b_channel(void *ctx, uint32_t hz) { return DEV.channel(hz); }
static int64_t b_now48(void *ctx) { return DEV.now48(); }

/* what its RX half heard, to the site, in its UTC slot: by RX_META's slot number when the RX half sends it; else a
 * radio's by its label's parity in the slot just over when it came (t), the SDR's by the last burst heard there.
 * T.mu held. */
static void b_take(sp_t *p, double t)
{
    int slot, meta, rssi;
    uint8_t control, burst[RDOF_BURST_BYTES];
    uint32_t counter;
    const int64_t k_at = utc_slot_at(t);
    while (rdof_t3split_take_at(&p->ts, &slot, &control, burst, &rssi, &meta, &counter)) {
        int64_t k;
        if ((meta & RDOF_SPLIT_META_COUNTER) && (meta & RDOF_SPLIT_META_UTC)) {
            k = rdof_t3split_unwrap(counter, k_at);
            p->by_meta++;
        } else if (p->radios) {
            k = rdof_t3split_parity_slot(k_at - 1, slot);
            p->by_parity++;
        } else { /* the slot of that parity nearest the last burst heard there */
            k = p->heard_k;
            if ((k & 1) != slot - 1)
                k += 1;
        }
        rdof_burst_t b;
        rdof_t3split_burst(&p->ts, slot, control, burst, &b);
        rdof_t3unit_rx(T.u, p->c, k, &b, rssi);
    }
}

/* the split's bytes from its radios and its timers; T.mu held */
static void b_service(sp_t *p, double now)
{
    for (int h = 0; h < 2; h++) {
        pt_t *x = h ? &p->rx : &p->tx;
        while (x->open) {
            uint8_t b[256];
            int n = 0;
            double t = now;
            pthread_mutex_lock(&x->mu);
            if (x->tail != x->head) {
                const int i = x->tail++ % BQ;
                n = x->q[i].n;
                t = x->q[i].t;
                memcpy(b, x->q[i].b, (size_t)n);
            }
            pthread_mutex_unlock(&x->mu);
            if (!n)
                break;
            if (x->rx) {
                rdof_t3split_from_rx_radio(&p->ts, b, (size_t)n, t);
                b_take(p, t);
            } else {
                rdof_t3split_from_radio(&p->ts, b, (size_t)n, t);
            }
        }
    }
    rdof_t3split_tick(&p->ts, now);
}

#ifdef RDOF_SIM_SPLIT
#define SIM_REPORT_MS 8 /* a simulated RX half reports a burst this long after its slot ends */

/* the simulated radio: what the host writes in, its replies and its air timing out, on its own clock; an RX half
 * hears the air MSs' bursts */
static void *sim_split_thread(void *arg)
{
    pt_t *x = arg;
    while (!x->sim_quit) {
        struct pollfd pf = {x->sim_fd, POLLIN, 0};
        const int r = poll(&pf, 1, 5);
        x->sim_clock = DEV.now();
        uint8_t b[512];
        if (r > 0 && (pf.revents & POLLIN)) {
            const ssize_t n = read(x->sim_fd, b, sizeof b);
            if (n <= 0)
                break;
            fake_write(x->fake, b, (size_t)n);
        }
        fake_air(x->fake);
        if (x->fake->non_air > 48) { /* what went out: the newest kept */
            memmove(x->fake->on_air, x->fake->on_air + 32, 16 * sizeof x->fake->on_air[0]);
            x->fake->non_air = 16;
        }
        if (x->rx) {
            const double utc_ms = (x->sim_clock * 1e6 + utc_minus_host) / 1000.0;
            pthread_mutex_lock(&x->mu);
            while (x->air_tail != x->air_head &&
                   utc_ms >= (double)((x->air[x->air_tail % SIM_AIR].k + 1) * 30 + SIM_REPORT_MS)) {
                const int i = x->air_tail++ % SIM_AIR;
                const double start = ((double)x->air[i].k * 30000.0 - utc_minus_host) / 1e6;
                fake_hear_at(x->fake, x->air[i].control, x->air[i].burst, -70, start);
            }
            pthread_mutex_unlock(&x->mu);
        }
        size_t m;
        while ((m = fake_take(x->fake, b, sizeof b)) > 0)
            if (write(x->sim_fd, b, m) != (ssize_t)m)
                break;
    }
    return NULL;
}

static int sim_split_open(pt_t *x, const char *port, char *err, size_t cap)
{
    int sv[2];
    x->fake = calloc(1, sizeof *x->fake);
    if (!x->fake || socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) {
        free(x->fake);
        snprintf(err, cap, "sim-split: no socket pair");
        return -1;
    }
    const int c = x->p->c;
    fake_init(x->fake, "odr_split");
    if (x->rx) { /* an RX half with RX_META, as RadioFoundry's (split-duplex.md 3.10); without, "...-nometa" */
        x->fake->roles = 2;
        if (!strstr(port, "nometa"))
            x->fake->split_caps |= RDOF_OPS_RX_META;
    } else {
        x->fake->split_caps |= RDOF_OPS_CACH; /* as RadioFoundry's (radiofoundry-handoff.md 1.4) */
    }
    x->fake->clock = &x->sim_clock;
    x->fake->clock_offset_us = 1234567.0 * (c + 1) + (x->rx ? 777777.0 : 0); /* each its own modem clock */
    x->fake->clock_rate = ((c & 1) ^ x->rx ? -6e-6 : 9e-6);
    x->sim_clock = DEV.now();
    x->sim_fd = sv[1];
    x->sim_quit = false;
    if (pthread_create(&x->sim_thread, NULL, sim_split_thread, x)) {
        close(sv[0]), close(sv[1]);
        free(x->fake);
        snprintf(err, cap, "sim-split: no thread");
        return -1;
    }
    x->sim = true;
    return sv[0];
}

/* an air MS's burst on carrier c in slot k, to its simulated RX half; false: carrier c has none */
static bool sim_air_hear(int c, int64_t k, const rdof_burst_t *b)
{
    pt_t *x = &SP[c].rx;
    if (!all_split || !SP[c].up || !x->sim)
        return false;
    pthread_mutex_lock(&x->mu);
    if (x->air_head - x->air_tail < SIM_AIR) {
        const int i = x->air_head++ % SIM_AIR;
        x->air[i].k = k;
        x->air[i].control = b->dt == RDOF_DT_VOICE_SYNC ? RDOF_SYNC_AUDIO
                            : b->dt == RDOF_DT_VOICE    ? (uint8_t)b->n
                                                        : (uint8_t)(RDOF_SYNC_DATA | b->dt);
        memcpy(x->air[i].burst, b->burst, RDOF_BURST_BYTES);
    }
    pthread_mutex_unlock(&x->mu);
    return true;
}
#else
static bool sim_air_hear(int c, int64_t k, const rdof_burst_t *b) { return false; }
#endif

static void pt_close(pt_t *x)
{
    if (!x->open)
        return;
    x->quit = true;
    pthread_join(x->thread, NULL);
    close(x->fd);
#ifdef RDOF_SIM_SPLIT
    if (x->sim) {
        x->sim_quit = true;
        pthread_join(x->sim_thread, NULL);
        close(x->sim_fd);
        free(x->fake);
        x->sim = false;
    }
#endif
    pthread_mutex_destroy(&x->mu);
    x->open = false;
}

/* a radio's port and its reader */
static bool pt_open(sp_t *p, pt_t *x, bool rx, const char *port, long baud, char *err, size_t cap)
{
    x->p = p, x->rx = rx, x->fd = -1;
#ifdef RDOF_SIM_SPLIT
    if (!strncmp(port, "sim-split", 9))
        x->fd = sim_split_open(x, port, err, cap);
    else
#endif
        x->fd = rdofradio_serial_open(port, baud ? (int)baud : 115200, err, cap);
    if (x->fd < 0)
        return false;
    pthread_mutex_init(&x->mu, NULL);
    x->open = true; /* pt_close from here */
    if (pthread_create(&x->thread, NULL, b_reader, x)) {
        x->thread = pthread_self();
        x->quit = true;
        snprintf(err, cap, "carrier %d: the port reader: no thread", p->c + 1);
        x->open = false;
        close(x->fd);
        pthread_mutex_destroy(&x->mu);
        return false;
    }
    return true;
}

static void b_stop(sp_t *p)
{
    if (!p->on)
        return;
    pt_close(&p->tx);
    pt_close(&p->rx);
    rdof_t3split_free(&p->ts);
    p->on = p->up = false;
}

static void splits_stop(void)
{
    for (int c = 0; c < RDOF_T3_CARRIERS; c++)
        b_stop(&SP[c]);
}

/* carrier c's split (0-based): its ports, the bring-up and its settings (the receiver isn't running yet: no RX half
 * on the SDR) */
static bool b_start(const rdofradio_t3_cfg_t *t, int c, char *err, size_t cap)
{
    sp_t *p = &SP[c];
    const uint32_t rx = t->car[c].rx_hz, tx = t->car[c].tx_hz;
    const char *port = t->car[c].port, *rport = t->car[c].rx_port;
    memset(p, 0, sizeof *p);
    p->c = c, p->align_said = -1, p->chan = -1;
    p->radios = !t->car[c].rx_sdr && rport[0];
    if (!p->radios && (p->chan = DEV.channel(rx)) < 0) {
        snprintf(err, cap, "carrier %d: the receiver has no channel on its inbound %.5f MHz", c + 1, rx / 1e6);
        return false;
    }
    if (!pt_open(p, &p->tx, false, port, t->car[c].baud, err, cap))
        return false;
    if (p->radios && !pt_open(p, &p->rx, true, rport, t->car[c].rx_baud, err, cap)) {
        pt_close(&p->tx);
        return false;
    }
    p->on = true; /* b_stop from here */
    const rdof_t3split_io_t io = {b_to_radio, b_channel, b_now48, p, b_to_rx_radio};
    const bool ok = p->radios ? rdof_t3split_init_radios(&p->ts, &io, c, rx, tx, t->cfg.cc, DEV.now())
                              : rdof_t3split_init(&p->ts, &io, DEV.rx_fs, c, rx, tx, t->cfg.cc, DEV.now());
    if (!ok) {
        snprintf(err, cap, "carrier %d: %s", c + 1, p->ts.error);
        b_stop(p);
        return false;
    }
    const double until = DEV.now() + 8.0;
    while (p->ts.state != RDOF_T3SPLIT_UP && p->ts.state != RDOF_T3SPLIT_FAILED && !p->tx.gone && !p->rx.gone &&
           DEV.now() < until) {
        usleep(10000);
        b_service(p, DEV.now());
    }
    if (p->ts.state != RDOF_T3SPLIT_UP) {
        snprintf(err, cap, "carrier %d (%s%s%s): %s", c + 1, port, p->radios ? ", " : "", p->radios ? rport : "",
                 p->tx.gone || p->rx.gone             ? "the port went away"
                 : p->ts.state == RDOF_T3SPLIT_FAILED ? p->ts.error
                 : p->radios                          ? "no split TX and RX halves answered in 8 s"
                                                      : "no split TX half answered in 8 s");
        b_stop(p);
        return false;
    }
    /* the TSCC's carrier: the AT and short LC slot by slot, in the CACH sent with each burst */
    if (c == 0 && !rdof_split_has_cach(rdof_t3split_split(&p->ts))) {
        snprintf(err, cap, "carrier 1 (%s): its split TX half doesn't take the CACH with each burst (RDOF_OPS_CACH): "
                           "the TSCC sets its AT and short LC slot by slot", port);
        b_stop(p);
        return false;
    }
    if (!next_utc) {
        utc_minus_host = utc_minus_host_us();
        next_utc = DEV.now() + 60;
    }
    rdof_t3split_utc(&p->ts, utc_minus_host, 1000, DEV.now());
    p->up = true;
    if (p->radios)
        tsay("carrier %d: split TX half on %s, RX half on %s (tx %.5f, rx %.5f MHz%s; each burst's slot %s), aligning "
             "to UTC", c + 1, port, rport, tx / 1e6, rx / 1e6, c == 0 ? ", the CACH with each burst" : "",
             rdof_t3split_split(&p->ts)->rx_meta_on ? "from RX_META" : "by its parity (no RX_META)");
    else
        tsay("carrier %d: split TX half on %s (tx %.5f, rx %.5f MHz%s), aligning to UTC", c + 1, port, tx / 1e6,
             rx / 1e6, c == 0 ? ", the CACH with each burst" : "");
    return true;
}

/* the splits' bytes, timers, UTC, what their RX halves heard, their pumps; T.mu held */
static void splits_poll(double now)
{
    const bool remeasure = next_utc && now >= next_utc;
    if (remeasure) { /* the host's clock follows NTP: the TX halves follow it */
        utc_minus_host = utc_minus_host_us();
        next_utc = now + 60;
    }
    const int64_t k_now = utc_slot_now();
    for (int c = 0; c < RDOF_T3_CARRIERS; c++) {
        sp_t *p = &SP[c];
        if (!p->up)
            continue;
        if (remeasure)
            rdof_t3split_utc(&p->ts, utc_minus_host, 1000, now);
        b_service(p, now);
        b_take(p, now);
        const rdof_split_t *sp = rdof_t3split_split(&p->ts);
        if ((int)sp->align_state != p->align_said) {
            p->align_said = (int)sp->align_state;
            tsay("carrier %d: the TX half's slot clock: %s", c + 1, rdof_align_state_name(sp->align_state));
        }
        if (p->ts.state == RDOF_T3SPLIT_FAILED || p->tx.gone || p->rx.gone) {
            tsay("carrier %d: %s; its channels go unsent", c + 1,
                 p->tx.gone || p->rx.gone ? "a port went away" : p->ts.error);
            p->up = false;
        } else {
            rdof_t3unit_split_pump(T.u, &p->ts.o, rdof_t3split_split(&p->ts), k_now, now);
        }
    }
    /* every carrier a split: the air MSs hear each slot as its pump sent it, up to now (no SDR slot source) */
    if (!all_split || !AM.n)
        return;
    if (!air_k || air_k < k_now - 8)
        air_k = k_now - 1;
    while (air_k < k_now) {
        const int64_t k = ++air_k;
        rdof_t3unit_set_now(T.u, wall_ms());
        air_in(k);
        for (int c = 0; c < AM.ncar; c++) {
            uint8_t burst[RDOF_BURST_BYTES] = {0};
            const int r = SP[c].up ? rdof_t3rf_split_sent(&SP[c].ts.o, k, burst) : -1;
            air_out(c, k, burst, r == 1);
        }
    }
}

/* ── carrier 1 on a duplex MMDVM modem (--tier3 with --mmdvm; docs/spec/fw/tier3-hat.md) ─────────────────────── */

#define HAT_POLL_S 0.25 /* GET_STATUS this often: the modem's watchdog drops it to IDLE after about 1 s without */
#define HAT_OFF_LINES 8 /* lines on a timeslot's bursts moving off or back on their slot, per report at most */
#define HAT_REPORT_S 10.0
static struct {
    bool on, paused;
    rdof_t3hat_t *h;
    rdofradio_t3_hat_t io;
    pthread_t thread;
    volatile bool quit;
    int out_chan, in_chan, cc;
    int64_t k_next;       /* the next trunk slot it may place on */
    double next_poll;
    uint8_t slc[9];       /* the last DMR_SHORTLC sent */
    bool slc_sent;
    uint32_t keeps, slcs, heard_out, heard_in, in_unplaced, airs_at[2];
    uint32_t at_same, at_diff; /* the AT heard before a slot of ours against the controller's (after the slot before) */
    int state, flags;          /* the last GET_STATUS: the modem's state (-1 not known) and flags */
    double next_report;
    rdof_t3hat_stats_t last;
    double key_t;              /* the last write to key it */
    int64_t cy_m;              /* the short LC cycle being heard: its LCSS 1 CACH's modem slot, its rows so far */
    int cy_n;
    uint32_t cy_rows[4];
    uint8_t null9[9];          /* the Null short LC */
    struct {                   /* the lead sweep (rdofradio_t3_hat_t) */
        int state;             /* 0 not yet, 1 running, 2 done */
        double lead, t_step, t_win;
        uint32_t aborts;       /* the flushes at the step's start */
    } sw;
    int off[2];                /* where each timeslot's last burst aired (slots off), for a line when it changes */
    int off_lines;             /* those lines since the last report (at most HAT_OFF_LINES) */
} H = {.out_chan = -1, .in_chan = -1, .state = -1};

static bool hat_frame(uint8_t cmd, const uint8_t *p, size_t n)
{
    uint8_t f[64];
    const size_t len = rdof_mmdvm_frame(cmd, p, n, f, sizeof f);
    return len && H.io.write(f, len);
}

/* slc28 as DMR_SHORTLC's 9 bytes: its four CACH rows joined */
static void hat_slc9(uint32_t slc28, uint8_t out[9])
{
    static const int at[4] = {0, 0, 0, 0};
    uint8_t c[4][3];
    uint32_t rows[4];
    rdof_t3rf_cach4(slc28, at, 0, c);
    for (int q = 0; q < 4; q++) {
        int a, slot, lcss;
        rdof_t3rf_cach_read(c[q], &a, &slot, &lcss, &rows[q]);
    }
    rdof_short_lc_join(rows, out);
}

/* trunk slot k to the modem, lead s before its air time; T.mu held */
static void hat_write(int64_t k, double now, double lead)
{
    uint8_t burst[RDOF_BURST_BYTES], control = 0;
    int at = -1;
    int32_t slc28 = -1;
    rdof_t3unit_set_now(T.u, wall_ms());
    const bool ok = rdof_t3unit_slot(T.u, 0, k, burst, &control, &at, &slc28);
    air_in(k);
    air_out(0, k, burst, ok);
    if (!ok) { /* nothing on it: an idle burst, so the modem keeps both slots as the host's */
        rdof_duplex_idle(H.cc, burst);
        control = RDOF_SYNC_DATA | 9;
    }
    if (H.io.mark && (control & 0x4F) == (RDOF_SYNC_DATA | 9)) /* each idle its own: which slot it aired on is heard */
        rdof_duplex_idle_mark(H.cc, (uint32_t)k * 0x9E3779B1u, burst);
    if (rdof_t3hat_lc_start(H.h, k)) {
        /* the short LC for the modem's 4 CACH from k, latched as it builds the first: before slot k's burst. Its
         * cycle starts before TS1 (k % 4 == 0) or TS2 (k % 4 == 1): the controller's cycle from k & ~3 */
        int64_t slc = rdof_t3unit_slc(T.u, 0, k - (k & 3));
        if (slc < 0)
            slc = slc28;
        if (slc >= 0) {
            uint8_t s9[9];
            hat_slc9((uint32_t)slc, s9);
            rdof_t3hat_slc_wrote(H.h, k, s9);
            if (!H.slc_sent || memcmp(s9, H.slc, 9)) {
                hat_frame(RDOF_MMDVM_DMR_SHORTLC, s9, 9);
                memcpy(H.slc, s9, 9);
                H.slc_sent = true;
                H.slcs++;
            }
        }
    }
    if (rdof_t3hat_abort(H.h, k)) { /* its timeslot ran behind: the write after this empties its queue first */
        const uint8_t ts = (uint8_t)((k & 1) + 1);
        hat_frame(RDOF_MMDVM_DMR_ABORT, &ts, 1);
    }
    uint8_t p[1 + RDOF_BURST_BYTES];
    p[0] = control;
    memcpy(p + 1, burst, RDOF_BURST_BYTES);
    hat_frame((k & 1) ? RDOF_MMDVM_DMR_DATA2 : RDOF_MMDVM_DMR_DATA1, p, sizeof p);
    rdof_t3hat_wrote(H.h, k, burst, now, lead);
}

static void hat_events(void)
{
    int kind, ts;
    int64_t k, m;
    double v[4];
    while (rdof_t3hat_event(H.h, &kind, &ts, &k, &m, v)) {
        if (kind == RDOF_T3HAT_GRID) {
            char ku[64] = "";
            if (!isnan(v[2]) && H.key_t > 0 && v[2] > H.key_t && v[2] - H.key_t < 2.0)
                snprintf(ku, sizeof ku, "; its first slot aired %.1f ms after the keying write", (v[2] - H.key_t) * 1e3);
            tsay("carrier 1 (modem): its slot grid from what the receiver hears%s%s",
                 H.last.grids ? " (again: it keyed up anew)" : "", ku);
        }
        else if (kind == RDOF_T3HAT_PLACE)
            tsay("carrier 1 (modem): trunk slot %lld is its slot %lld; its short LC cycle from k %% 4 == %.0f", (long long)k,
                 (long long)m, v[1]);
        else if (kind == RDOF_T3HAT_REKEY)
            tsay("carrier 1 (modem): its status says unkeyed: placed again when it keys");
        else if (kind == RDOF_T3HAT_ABORT && T.o.verbose)
            tsay("carrier 1 (modem): TS%d flushed before slot %lld (%s)", ts, (long long)k,
                 v[0] == 0 ? "aired late" : v[0] == 1 ? "its queue too deep" : v[0] == 2 ? "NAKed" : v[0] == 3 ? "placed" : "asked");
        else if (kind == RDOF_T3HAT_SKIP && v[0] == 1 && H.off_lines < HAT_OFF_LINES) {
            H.off_lines++;
            tsay("carrier 1 (modem): TS%d slot %lld left unwritten, for the modem to fill: it aired early", ts,
                 (long long)k);
        }
        else if (kind == RDOF_T3HAT_AIR) {
            if ((int)v[0] != H.off[ts - 1] && H.off_lines < HAT_OFF_LINES) {
                H.off_lines++;
                if (v[0] == 0)
                    tsay("carrier 1 (modem): TS%d on its slot again from slot %lld", ts, (long long)k);
                else
                    tsay("carrier 1 (modem): TS%d airs %+.0f slots off from slot %lld (written %.1f ms ahead)", ts, v[0],
                         (long long)k, v[2] * 1e3);
            }
            H.off[ts - 1] = (int)v[0];
            if (v[0] != 0 && T.o.verbose)
                tsay("carrier 1 (modem): slot %lld aired %+.0f slots off (written %.1f ms ahead)", (long long)k, v[0],
                     v[2] * 1e3);
            if (ts == 2 && v[0] == 0)
                H.airs_at[v[3] > 0]++;
            const int want = rdof_t3unit_at(T.u, k - 1);
            if (v[0] == 0 && want >= 0 && v[3] >= 0) {
                if ((want > 0) == (v[3] > 0))
                    H.at_same++;
                else
                    H.at_diff++;
            }
        }
        H.last.grids += kind == RDOF_T3HAT_GRID;
    }
}

static void hat_detail(const rdof_t3hat_stats_t *s, char *out, size_t cap)
{
    size_t n = 0;
    for (int ts = 0; ts < 2; ts++) {
        const uint32_t *e = s->e_n[ts], *dq = s->depth_n[ts];
        char d[32] = "", sp[24] = "";
        if (e[2])
            snprintf(d, sizeof d, ", write to air %.1f-%.1f ms", s->ts_delay_min[ts] * 1e3, s->ts_delay_max[ts] * 1e3);
        if (dq[0] + dq[1] + dq[2] + dq[3])
            snprintf(sp, sizeof sp, " (space %d-%d)", s->space_min[ts], s->space_max[ts]);
        n += (size_t)snprintf(out + n, n < cap ? cap - n : 0,
                              "%sTS%d aired -4..+4: %u %u %u %u %u, its own %u%s; AT 0/1 %u/%u; depth 0..3+ %u %u %u %u%s",
                              ts ? "; " : "", ts + 1, e[0], e[1], e[2], e[3], e[4], s->fills_ts[ts], d,
                              s->at_n[ts][0], s->at_n[ts][1], dq[0], dq[1], dq[2], dq[3], sp);
    }
    snprintf(out + n, n < cap ? cap - n : 0, "; short LC cycles %u as written, %u a cycle late, %u Null, %u else; %u alike",
             s->slc_on, s->slc_late, s->slc_null, s->slc_other, s->ambiguous);
}

/* a lead sweep step's start; T.mu held */
static void hat_sweep_step(double now)
{
    rdof_t3hat_set_leads(H.h, H.sw.lead, H.sw.lead, H.sw.lead - 0.01);
    if (!H.io.sweep_keep)
        rdof_t3hat_flush(H.h); /* each step from empty queues */
    rdof_t3hat_stats_t s;
    rdof_t3hat_stats(H.h, &s);
    H.sw.aborts = s.aborts;
    H.sw.t_step = now;
    H.sw.t_win = now + 0.5; /* the step's first half second settling */
}

/* the lead sweep (--hat-sweep): a line per step; T.mu held */
static void hat_sweep(double now)
{
    if (H.io.sweep_s <= 0 || H.sw.state == 2)
        return;
    const double dir = H.io.sweep_to_s >= H.io.sweep_from_s ? 1 : -1;
    if (H.sw.state == 0) {
        H.sw.state = 1;
        H.sw.lead = H.io.sweep_from_s;
        rdof_t3hat_set_catchup(H.h, false);
        hat_sweep_step(now);
        tsay("carrier 1 (modem): lead sweep %.1f to %.1f ms by %.1f, %.1f s each, no catch-up%s", H.io.sweep_from_s * 1e3,
             H.io.sweep_to_s * 1e3, H.io.sweep_step_s * 1e3, H.io.sweep_s,
             H.io.sweep_keep ? ", no flush" : ", both queues flushed each step");
        return;
    }
    if (H.sw.t_win > 0 && now >= H.sw.t_win) {
        rdof_t3hat_window(H.h);
        H.sw.t_win = 0;
    }
    if (now < H.sw.t_step + H.io.sweep_s)
        return;
    rdof_t3hat_stats_t s;
    rdof_t3hat_stats(H.h, &s);
    char d[512];
    hat_detail(&s, d, sizeof d);
    tsay("carrier 1 (modem): lead %.1f ms (%u flushed): %s", H.sw.lead * 1e3, s.aborts - H.sw.aborts, d);
    H.sw.lead += dir * H.io.sweep_step_s;
    if ((H.sw.lead - H.io.sweep_to_s) * dir > 1e-6) {
        H.sw.state = 2;
        rdof_t3hat_set_leads(H.h, H.io.lead_idle_s, H.io.lead_busy_s, H.io.lead_idle_s - 0.01);
        rdof_t3hat_set_catchup(H.h, true);
        rdof_t3hat_flush(H.h);
        tsay("carrier 1 (modem): lead sweep done: leads %.1f / %.1f ms again", H.io.lead_idle_s * 1e3,
             H.io.lead_busy_s * 1e3);
        return;
    }
    hat_sweep_step(now);
}

static void *hat_writer(void *arg)
{
    (void)arg;
    while (!H.quit) {
        const struct timespec ts = {0, 1000000L};
        nanosleep(&ts, NULL);
        pthread_mutex_lock(&T.mu);
        if (H.paused || !T.on) {
            pthread_mutex_unlock(&T.mu);
            continue;
        }
        const double now = DEV.now();
        rdof_t3hat_stats_t st;
        rdof_t3hat_stats(H.h, &st);
        if (!st.placed) { /* the trunk's slots follow the wall clock until placed (then each in turn) */
            int64_t w = wall_ms() / 30 + 2;
            w += w & 1;
            if (w > H.k_next)
                H.k_next = w;
        }
        if (st.placed)
            hat_sweep(now);
        if (now >= H.next_poll) { /* the modem's watchdog; its free space per timeslot */
            hat_frame(RDOF_MMDVM_GET_STATUS, NULL, 0);
            H.next_poll = now + HAT_POLL_S;
        }
        int64_t k;
        double air;
        if (!rdof_t3hat_next(H.h, now, H.k_next, &k, &air)) {
            if (rdof_t3hat_keep(H.h, now)) { /* a write keys it (DMR_START 1 would hold its queues 20 slots) */
                uint8_t p[1 + RDOF_BURST_BYTES];
                p[0] = RDOF_SYNC_DATA | 9;
                rdof_duplex_idle(H.cc, p + 1);
                hat_frame(RDOF_MMDVM_DMR_DATA1, p, sizeof p);
                H.keeps++;
                H.key_t = now;
            }
        } else {
            /* written early, the slot's burst sets the AT of the CACH after slot k - 2: the controller's */
            const int at = rdof_t3unit_at(T.u, k - 2);
            const double lead = rdof_t3hat_lead(H.h, at > 0);
            if (now >= air - lead) {
                hat_write(k, now, air - now);
                H.k_next = k + 1;
            }
        }
        hat_events();
        pthread_mutex_unlock(&T.mu);
    }
    return NULL;
}

/* a whole short LC cycle heard (its LCSS 1 CACH before modem slot m); T.mu held */
static void hat_slc_heard(int64_t m, const uint8_t s9[9])
{
    static const uint8_t zero[9];
    rdof_t3hat_slc_heard(H.h, m, s9, !memcmp(s9, H.null9, 9) || !memcmp(s9, zero, 9));
}

/* an outbound burst heard on carrier 1's out: its CACH's row of the short LC (row: -1 none), or (the simulated modem)
 * the cycle's short LC and which row; T.mu held */
static void hat_heard_out(int64_t pos48, int slot, int at, int lcss, int64_t row, const uint8_t *slc9, int frag,
                          const uint8_t burst[RDOF_BURST_BYTES])
{
    int64_t m = INT64_MIN;
    rdof_t3hat_heard(H.h, pos48, slot, at, lcss, burst, &m);
    H.heard_out++;
    if (m == INT64_MIN || lcss < 0)
        return;
    if (slc9) {
        if (frag == 3)
            hat_slc_heard(m - 3, slc9);
        return;
    }
    if (row < 0)
        return;
    const int i = lcss == 1 ? 0 : (int)(m - H.cy_m);
    const int want = i == 0 ? 1 : i == 3 ? 2 : 3;
    if (i < 0 || i > 3 || lcss != want || (i > 0 && H.cy_n != i)) {
        H.cy_n = 0;
        return;
    }
    if (i == 0)
        H.cy_m = m;
    H.cy_rows[i] = (uint32_t)row;
    H.cy_n = i + 1;
    if (H.cy_n == 4) {
        uint8_t s9[9];
        rdof_short_lc_join(H.cy_rows, s9);
        hat_slc_heard(H.cy_m, s9);
        H.cy_n = 0;
    }
}

void rdofradio_t3_hat_heard(int64_t pos48, int slot, int at, int lcss, const uint8_t burst[RDOF_BURST_BYTES],
                            const uint8_t slc9[9], int frag)
{
    if (!H.on)
        return;
    pthread_mutex_lock(&T.mu);
    hat_heard_out(pos48, slot, at, lcss, -1, slc9, frag, burst);
    pthread_mutex_unlock(&T.mu);
}

/* what the receiver heard on carrier 1's out or in; T.mu not held */
static void hat_rx(int c, const rdof_sdr_burst_t *b)
{
    if (c == H.out_chan) {
        int at = -1, slot = b->b.slot, lcss = -1;
        uint32_t pl;
        if (b->has_cach && !rdof_t3rf_cach_read(b->cach, &at, &slot, &lcss, &pl))
            at = -1, slot = b->b.slot, lcss = -1;
        pthread_mutex_lock(&T.mu);
        hat_heard_out(b->position, slot, at, lcss, lcss >= 0 ? (int64_t)pl : -1, NULL, 0, b->b.burst);
        pthread_mutex_unlock(&T.mu);
    }
    if (c == H.in_chan) {
        pthread_mutex_lock(&T.mu);
        const int64_t k = rdof_t3hat_inbound_k(H.h, b->position);
        if (k >= 0) {
            rdof_t3unit_rx(T.u, 0, k, &b->b, 0);
            rdof_t3hat_inbound(H.h, k, b->b.burst);
            H.heard_in++;
            if (T.o.verbose)
                tsay("carrier 1 (modem): heard %s dt %d on TS%d, trunk slot %lld", rdof_family_name(b->b.family),
                     b->b.dt, b->b.slot, (long long)k);
        } else {
            H.in_unplaced++;
        }
        pthread_mutex_unlock(&T.mu);
    }
}

/* A GET_STATUS reply (the modem's protocol, its payload): v1.5.2's state at 1, flags at 2, TS1 and TS2 free space
 * at 4 and 5 (bursts, its free bytes / 35). */
void rdofradio_t3_hat_status(int protocol, const uint8_t *p, size_t len)
{
    rdof_mmdvm_status_t st;
    if (!H.on || !rdof_mmdvm_parse_status(protocol, p, len, &st))
        return;
    const int state = protocol == 1 ? p[1] : -1;
    const int flags = protocol == 1 ? p[2] : st.transmitting;
    pthread_mutex_lock(&T.mu);
    if (!H.paused) {
        if (state == 0 && H.state != 0)
            tsay("carrier 1 (modem): its status says IDLE (its watchdog: GET_STATUS late?)");
        H.state = state, H.flags = flags;
        rdof_t3hat_status(H.h, state, flags, st.space1, st.space);
    }
    pthread_mutex_unlock(&T.mu);
}

/* a command NAKed: a DMR_DATA's, its queue full */
void rdofradio_t3_hat_nak(uint8_t cmd, int err)
{
    if (!H.on || (cmd != RDOF_MMDVM_DMR_DATA1 && cmd != RDOF_MMDVM_DMR_DATA2))
        return;
    (void)err;
    pthread_mutex_lock(&T.mu);
    rdof_t3hat_nak(H.h, cmd == RDOF_MMDVM_DMR_DATA1 ? 1 : 2);
    pthread_mutex_unlock(&T.mu);
}

/* a burst the modem itself heard on the inbound (DMR_DATA1/2 from it, slot its tag), at now */
void rdofradio_t3_hat_modem_rx(int slot, const uint8_t burst[RDOF_BURST_BYTES], double now)
{
    if (!H.on)
        return;
    pthread_mutex_lock(&T.mu);
    if (!H.paused) {
        const int64_t k = rdof_t3hat_modem_rx(H.h, now, slot, burst);
        if (T.o.verbose)
            tsay("carrier 1 (modem): it heard TS%d inbound, the receiver's slot %lld", slot, (long long)k);
    }
    pthread_mutex_unlock(&T.mu);
}

/* the status part of the reports: since l (or the run's, l NULL) */
static void hat_report_status(const rdof_t3hat_stats_t *s, const rdof_t3hat_stats_t *l)
{
    static const rdof_t3hat_stats_t zero;
    if (!l)
        l = &zero;
    const uint32_t rx = s->rx_same + s->rx_other - l->rx_same - l->rx_other;
    char d[48] = "";
    if (rx)
        snprintf(d, sizeof d, ", %+.1f to %+.1f ms after its end", s->rx_delay_min * 1e3, s->rx_delay_max * 1e3);
    tsay("carrier 1 (modem): status %u (%u IDLE, %u unkeyed, %u overflow), its queues %u / %u deep at most; %u "
         "flushed, %u NAKed; it heard %u inbound: %u in the receiver's timeslot, %u in the other, %u unmatched%s",
         s->polls - l->polls, s->idle - l->idle, s->unkeyed - l->unkeyed, s->overflows - l->overflows,
         s->depth_max[0], s->depth_max[1], s->aborts - l->aborts, s->naks - l->naks, s->rx_n - l->rx_n,
         s->rx_same - l->rx_same, s->rx_other - l->rx_other, s->rx_unmatched - l->rx_unmatched, d);
}

/* the main loop's; T.mu held */
static void hat_poll(double now)
{
    rdof_t3hat_clock(H.h, now, DEV.now48());
    if (now < H.next_report)
        return;
    H.next_report = now + HAT_REPORT_S;
    rdof_t3hat_stats_t s;
    rdof_t3hat_stats(H.h, &s);
    const rdof_t3hat_stats_t *l = &H.last;
    tsay("carrier 1 (modem): %u written, %u on their slot, %u late, %u early, %u lost, %u skipped, %u its own; "
         "%u heard out, %u in; write to air %.1f-%.1f ms; its clock %+.2f ppm; AT busy %u of %u, as the controller's "
         "%u of %u",
         s.written - l->written, s.on_time - l->on_time, s.late - l->late, s.early - l->early, s.lost - l->lost,
         s.skipped - l->skipped, s.fills - l->fills, H.heard_out, H.heard_in,
         s.on_time > l->on_time ? s.delay_min * 1e3 : 0.0, s.on_time > l->on_time ? s.delay_max * 1e3 : 0.0, s.ppm,
         H.airs_at[1], H.airs_at[0] + H.airs_at[1], H.at_same, H.at_same + H.at_diff);
    hat_report_status(&s, l);
    if (H.sw.state != 1) { /* the sweep has its own windows */
        char d[512];
        hat_detail(&s, d, sizeof d);
        tsay("carrier 1 (modem): %s", d);
        rdof_t3hat_window(H.h);
    }
    H.off_lines = 0;
    const uint32_t grids = H.last.grids;
    H.last = s;
    H.last.grids = grids;
    H.heard_out = H.heard_in = 0;
    H.airs_at[0] = H.airs_at[1] = 0;
    H.at_same = H.at_diff = 0;
}

void rdofradio_t3_hat_pause(bool pause)
{
    if (!H.on)
        return;
    pthread_mutex_lock(&T.mu);
    if (pause != H.paused) {
        H.paused = pause;
        rdof_t3hat_reset(H.h);
        H.slc_sent = false;
        H.state = -1;
        H.next_poll = 0;
        tsay("carrier 1 (modem): %s", pause ? "a bench job has the modem: paused" : "the modem is back: on again");
    }
    pthread_mutex_unlock(&T.mu);
}

bool rdofradio_t3_start_hat(const rdofradio_cfg_t *c, const rdofradio_t3_opts_t *o, const rdofradio_t3_dev_t *dev,
                            const rdofradio_t3_hat_t *hat, char *err, size_t cap)
{
    const rdofradio_t3_cfg_t *t = &c->t;
    if (t->ncar != 1) {
        snprintf(err, cap, "%s: %d carriers: on a modem the site runs one", t->name, t->ncar);
        return false;
    }
    DEV = *dev;
    H.io = *hat;
    H.cc = t->cfg.cc;
    H.out_chan = DEV.channel(t->car[0].tx_hz);
    H.in_chan = DEV.channel(t->car[0].rx_hz);
    H.h = aligned_alloc(16, (rdof_t3hat_size() + 15) / 16 * 16);
    if (!H.h) {
        snprintf(err, cap, "no memory");
        return false;
    }
    rdof_t3hat_init(H.h, hat->lead_idle_s, hat->lead_busy_s);
    hat_slc9(0, H.null9);
    H.sw.state = 0;
    H.cy_n = 0;
    H.key_t = 0;
    H.off[0] = H.off[1] = 0;
    H.off_lines = 0;
    rdof_t3hat_set_leads(H.h, hat->lead_idle_s, hat->lead_busy_s, hat->lead_idle_s - 0.01);
    if (!start_common(c, o, err, cap))
        return false;
    if (!air_start(o, err, cap)) {
        stop_common();
        return false;
    }
    H.k_next = 0;
    H.next_poll = 0;
    H.quit = false;
    H.paused = false;
    H.next_report = DEV.now() + HAT_REPORT_S;
    H.on = true;
    if (pthread_create(&H.thread, NULL, hat_writer, NULL)) {
        H.on = false;
        air_stop();
        stop_common();
        snprintf(err, cap, "the modem's writer: no thread");
        return false;
    }
    tsay("carrier 1 on the modem (tx %.5f, rx %.5f MHz): the receiver %s its out, %s its in; leads %.1f / %.1f ms",
         t->car[0].tx_hz / 1e6, t->car[0].rx_hz / 1e6, H.out_chan >= 0 ? "hears" : "doesn't hear",
         H.in_chan >= 0 ? "hears" : "doesn't hear", hat->lead_idle_s * 1e3, hat->lead_busy_s * 1e3);
    return true;
}

static void hat_stop(void)
{
    if (!H.on)
        return;
    H.quit = true;
    pthread_join(H.thread, NULL);
    rdof_t3hat_stats_t s;
    rdof_t3hat_stats(H.h, &s);
    tsay("carrier 1 (modem): %u written, %u on their slot, %u late, %u early, %u lost, %u skipped, %u its own, "
         "%u grids, %u short LCs; its clock %+.2f ppm", s.written, s.on_time, s.late, s.early, s.lost, s.skipped,
         s.fills, s.grids, H.slcs, s.ppm);
    hat_report_status(&s, NULL);
    H.on = false;
    free(H.h);
    H.h = NULL;
}

/* --mmdvm sim-hat: the stock modem simulated (rdof/t3hat.h) on a socket; what it sends heard as the receiver would */
static struct {
    bool on;
    volatile bool quit;
    int fd;
    pthread_t thread;
    pthread_mutex_t mu; /* the sim: its thread, and hat_air_in's (the writer's) */
    rdof_t3hatsim_t *s;
    double rx_delay;
    uint32_t rng;
} HS = {.fd = -1, .rng = 1, .mu = PTHREAD_MUTEX_INITIALIZER};

/* an inbound burst of an air MS under trunk slot k (T.mu held): for the modem's own report to match, and with the
 * simulated modem, heard by it as an MS's burst under the slot */
static void hat_air_in(int64_t k, const rdof_burst_t *b)
{
    if (!H.on)
        return;
    rdof_t3hat_inbound(H.h, k, b->burst);
    const double air = rdof_t3hat_air(H.h, k);
    if (!HS.on || isnan(air))
        return;
    const uint8_t control = b->dt == RDOF_DT_VOICE_SYNC ? RDOF_SYNC_AUDIO
                            : b->dt == RDOF_DT_VOICE    ? (uint8_t)b->n
                                                        : (uint8_t)(RDOF_SYNC_DATA | (b->dt & 0x0F));
    pthread_mutex_lock(&HS.mu);
    /* it starts 1.25 ms before slot k's outbound burst; its sync ends 156 bits in */
    rdof_t3hatsim_hear(HS.s, air - 0.00125 + 156.0 / 9600.0, control, b->burst);
    pthread_mutex_unlock(&HS.mu);
}

static double mono_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void *hat_sim_thread(void *arg)
{
    (void)arg;
    while (!HS.quit) {
        struct pollfd pf = {HS.fd, POLLIN, 0};
        const int r = poll(&pf, 1, 1);
        const double now = mono_s();
        if (r > 0 && (pf.revents & POLLIN)) {
            uint8_t b[512];
            const ssize_t n = read(HS.fd, b, sizeof b);
            if (n <= 0)
                break;
            pthread_mutex_lock(&HS.mu);
            rdof_t3hatsim_input(HS.s, now, b, (size_t)n);
            pthread_mutex_unlock(&HS.mu);
        }
        pthread_mutex_lock(&HS.mu);
        rdof_t3hatsim_run(HS.s, now);
        uint8_t out[1024];
        const size_t n = rdof_t3hatsim_output(HS.s, out, sizeof out);
        pthread_mutex_unlock(&HS.mu);
        if (n && write(HS.fd, out, n) != (ssize_t)n)
            break;
        int64_t m;
        int slot, at, lcss, frag;
        double t;
        uint8_t burst[RDOF_BURST_BYTES], slc9[9];
        bool host;
        for (;;) {
            pthread_mutex_lock(&HS.mu);
            const bool got =
                rdof_t3hatsim_aired(HS.s, now - HS.rx_delay, &m, &slot, &t, burst, &host, &at, &lcss, slc9, &frag);
            pthread_mutex_unlock(&HS.mu);
            if (!got)
                break;
            /* a receiver's position wanders by a sample or so: whole samples alone would alias with the slot */
            HS.rng = HS.rng * 1103515245u + 12345u;
            const double dither = (double)(HS.rng >> 8 & 0xffff) / 65536.0 - 0.5;
            rdofradio_t3_hat_heard((int64_t)llround(t * 48000.0 + dither), slot, at, lcss, burst, slc9, frag);
        }
    }
    return NULL;
}

int rdofradio_t3_hat_sim(double ppm, double fetch_s, char *err, size_t cap)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) {
        snprintf(err, cap, "sim-hat: %s", strerror(errno));
        return -1;
    }
    HS.s = aligned_alloc(16, (rdof_t3hatsim_size() + 15) / 16 * 16);
    if (!HS.s) {
        close(sv[0]), close(sv[1]);
        snprintf(err, cap, "sim-hat: no memory");
        return -1;
    }
    rdof_t3hatsim_init(HS.s, ppm, fetch_s, 0);
    HS.fd = sv[1];
    HS.rx_delay = 0.004;
    HS.quit = false;
    if (pthread_create(&HS.thread, NULL, hat_sim_thread, NULL)) {
        close(sv[0]), close(sv[1]);
        free(HS.s);
        snprintf(err, cap, "sim-hat: no thread");
        return -1;
    }
    HS.on = true;
    return sv[0];
}

void rdofradio_t3_hat_sim_stop(void)
{
    if (!HS.on)
        return;
    HS.quit = true;
    pthread_join(HS.thread, NULL);
    close(HS.fd);
    uint32_t c[5];
    rdof_t3hatsim_counts(HS.s, c);
    fprintf(stderr, "sim-hat: %u key-ups, %u unkeys, %u watchdog drops, %u short LC cycles Null while written, %u NAKs\n",
            c[0], c[1], c[2], c[3], c[4]);
    free(HS.s);
    HS.on = false;
}

bool rdofradio_t3_start(const rdofradio_cfg_t *c, const rdofradio_t3_opts_t *o, rdof_sdrmodem_t *m,
                           const rdofradio_t3_dev_t *dev, char *err, size_t cap)
{
    const rdofradio_t3_cfg_t *t = &c->t;
    all_split = !t->car[0].tx_sdr;
    for (int k = 0; all_split && k < t->ncar; k++)
        if (t->car[k].tx_sdr || !t->car[k].port[0] || (!t->car[k].rx_sdr && !t->car[k].rx_port[0])) {
            snprintf(err, cap, "%s: carrier 1 on a split TX half: every carrier is (carrier %d isn't: a radio with a "
                               "port path, port: {\"path\": ...}), each heard by the SDR or its own radio with a port "
                               "path", t->name, k + 1);
            return false;
        }
    for (int k = 1; !all_split && k < t->ncar; k++) {
        if (t->car[k].tx_sdr && t->car[k].tx_dev != t->car[0].tx_dev) {
            snprintf(err, cap, "%s: carrier %d's out is on another SDR: rdofradio sends every carrier from the one it "
                               "runs", t->name, k + 1);
            return false;
        }
        if (t->ncar > 2 && !t->car[k].tx_sdr) {
            snprintf(err, cap, "%s: %d carriers: past two, every carrier is the SDR's (carrier %d isn't; a split TX "
                               "half can be carrier 2 of two)", t->name, t->ncar, k + 1);
            return false;
        }
    }
    if (!all_split && t->ncar == 2 && !t->car[1].tx_sdr && !t->car[1].port[0]) {
        snprintf(err, cap, "%s: carrier 2 is the SDR's (its out among the SDR's transmits) or a split TX half: a radio "
                           "with a port path (port: {\"path\": ...})", t->name);
        return false;
    }
    DEV = *dev;
    A.chan = DEV.channel(t->car[0].rx_hz);
    next_utc = 0, air_k = 0;
    for (int k = 0; all_split && k < t->ncar; k++)
        if (!b_start(t, k, err, cap)) {
            splits_stop();
            return false;
        }
    if (!all_split && t->ncar >= 2 && t->car[1].tx_sdr) {
        if (!DEV.tx_lo || !c2_fits(t, err, cap))
            return false;
        for (int k = 2; k <= t->ncar; k++)
            if (!c2_start(t, k, m, err, cap)) {
                c2_stop();
                return false;
            }
    }
    if (!all_split && t->ncar == 2 && !t->car[1].tx_sdr && !b_start(t, 1, err, cap))
        return false;
    if (!start_common(c, o, err, cap)) {
        splits_stop();
        c2_stop();
        return false;
    }
    if (!air_start(o, err, cap)) {
        splits_stop();
        c2_stop();
        stop_common();
        return false;
    }
    if (all_split) { /* the SDR receives only: its modem stays unkeyed, the splits' pumps send every slot */
        M = NULL;
        if (t->car[0].rx_sdr)
            tsay("%s: every carrier on a split TX half; the SDR receives only", t->name);
        return true;
    }
    M = m;
    const rdof_sdrmodem_source_t src = {source_slot, NULL};
    rdof_sdrmodem_set_source(m, &src);
    /* keyed once the slot grid has a map (the modem snaps to it at key-up; a held key-up is only steered), else
     * after GRID_WAIT_S on its own count */
    A.keyed = false;
    A.key_by = DEV.now() + (rdofradio_utc_enabled() ? GRID_WAIT_S : 0);
    return true;
}

/* the main loop's, before the modem's pull; carrier 2's modem with it, so both snap to the grid at one key-up */
static bool key_carrier1(void)
{
    if (A.keyed || !M)
        return true;
    int64_t i0, t0;
    double rate;
    const bool mapped = rdofradio_utc_map48(&i0, &t0, &rate);
    if (!mapped && DEV.now() < A.key_by)
        return true;
    if (rdofradio_utc_enabled() && !rdofradio_utc_clock_settled()) {
        if (DEV.now() < A.key_by - GRID_WAIT_S + CLOCK_WAIT_S)
            return true;
        tsay("carrier 1: the host's clock hasn't settled after %.0f s: keyed anyway", CLOCK_WAIT_S);
    }
    if (!mapped && rdofradio_utc_enabled()) {
        const char *why = "";
        rdofradio_utc_state(&why, NULL);
        tsay("carrier 1: no slot grid after %.0f s (%s): keyed on the SDR's own clock", GRID_WAIT_S, why);
    }
    A.keyed = true;
    for (int i = 0; i < nsdr; i++)
        if (CX[i].on && !rdof_sdrmodem_hold_key(&CX[i].m, true))
            tsay("carrier %d: the modem won't hold its key", CX[i].c);
    return rdof_sdrmodem_hold_key(M, true);
}

bool rdofradio_t3_on(void) { return T.on; }

static int no_channel(uint32_t hz) { return -1; }
static int64_t no_now48(void) { return 0; }

int rdofradio_t3_radios(const rdofradio_cfg_t *c, const rdofradio_t3_opts_t *o, volatile sig_atomic_t *stop)
{
    const rdofradio_t3_dev_t dev = {.channel = no_channel, .now48 = no_now48, .now = mono_s, .rx_fs = 48000};
    char err[300];
    if (!rdofradio_t3_start(c, o, NULL, &dev, err, sizeof err)) {
        tsay("%s", err);
        return 1;
    }
    tsay("%s: every carrier on two radios, a split TX half and a split RX half; no SDR", c->t.name);
    const double t0 = mono_s();
    while (!*stop && (o->seconds <= 0 || mono_s() - t0 < o->seconds)) {
        usleep(5000);
        rdofradio_t3_poll();
    }
    rdofradio_t3_stop();
    return 0;
}

void rdofradio_t3_heard(int c, const rdof_sdr_burst_t *b)
{
    if (!T.on || c < 0)
        return;
    if (H.on) {
        hat_rx(c, b);
        return;
    }
    if (c == A.chan) {
        int64_t k;
        const bool in = M && rdof_sdrmodem_inbound_k(M, b, &k);
        pthread_mutex_lock(&T.mu);
        if (in && A.s.placed)
            rdof_t3unit_rx(T.u, 0, k + A.s.d, &b->b, 0);
        pthread_mutex_unlock(&T.mu);
    }
    for (int i = 0; i < nsdr; i++) {
        cx_t *x = &CX[i];
        if (!x->on || c != x->chan)
            continue;
        rdof_sdrmodem_heard(&x->m, c, b); /* its inbound's busy state: the AT in its CACH */
        int64_t k;
        const bool in = rdof_sdrmodem_inbound_k(&x->m, b, &k);
        pthread_mutex_lock(&T.mu);
        if (in && x->s.placed) {
            rdof_t3unit_rx(T.u, x->c - 1, k + x->s.d, &b->b, 0);
            x->heard++;
            if (T.o.verbose)
                tsay("carrier %d: heard %s dt %d on TS%d, trunk slot %lld", x->c, rdof_family_name(b->b.family),
                     b->b.dt, b->b.slot, (long long)(k + x->s.d));
        }
        pthread_mutex_unlock(&T.mu);
    }
    for (int j = 0; j < RDOF_T3_CARRIERS; j++) {
        sp_t *p = &SP[j];
        if (!p->up || c != p->chan)
            continue;
        pthread_mutex_lock(&T.mu);
        /* its UTC slot from its position: the burst starts on the slot's start */
        const double t = DEV.now() - (double)(DEV.now48() - b->position) / 48000.0;
        p->heard_k = (int64_t)floor((t * 1e6 + utc_minus_host) / 30000.0 + 0.25);
        rdof_t3split_heard(&p->ts, c, b);
        b_take(p, DEV.now());
        pthread_mutex_unlock(&T.mu);
    }
}

void rdofradio_t3_poll(void)
{
    if (!T.on)
        return;
    if (!key_carrier1()) {
        tsay("carrier 1: the modem won't hold its key (not duplex?)");
        A.keyed = true;
    }
    pthread_mutex_lock(&T.mu);
    poll_locked(wall_ms());
    if (H.on)
        hat_poll(DEV.now());
    splits_poll(DEV.now());
    pthread_mutex_unlock(&T.mu);
}

bool rdofradio_t3_mix(int64_t pos, rdof_cf_t *iq, int n)
{
    if (!nsdr || !T.on || !M)
        return false;
    bool keyed = false;
    for (int j = 0; j < nsdr; j++) {
        cx_t *x = &CX[j];
        if (!x->on)
            continue;
        const bool moved = DEV.tx_lo() != x->lo;
        if (moved != x->lo_moved)
            tsay("carrier %d: %s", x->c,
                 moved ? "the TX LO moved (a bench job): unsent until it's back" : "the TX LO is back: sent");
        x->lo_moved = moved;
        x->m.tx_late48 = M->tx_late48; /* the transmit path's delay is the stream's: carrier 1's measure */
        if (x->m.carrier_ppm != M->carrier_ppm)
            rdof_sdrmodem_carrier_ppm(&x->m, M->carrier_ppm);
        rdofradio_utc_apply(&x->m);
        const double t0 = DEV.now();
        const float w = 1.0f / (float)(j + 2); /* carriers 1 to j + 1 in iq already */
        for (int i = 0; i < n; i += MIX_N) {
            const int k = n - i < MIX_N ? n - i : MIX_N;
            if (moved)
                rdof_sdrmodem_pull_iq(&x->m, pos + i, x->scratch, k); /* its clock runs on */
            else
                keyed |= rdof_sdrmodem_pull_mixw(&x->m, pos + i, iq + i, x->scratch, k, w);
        }
        x->pull_s += DEV.now() - t0;
        x->pulls++;
        x->samples += (uint64_t)n;
    }
    return keyed;
}

void rdofradio_t3_carrier2(char *out, size_t cap, uint32_t *sent, uint32_t *heard)
{
    const rdofradio_t3_cfg_t *t = T.c ? &T.c->t : NULL;
    *sent = *heard = 0;
    if (!T.on || !t || t->ncar < 2) {
        snprintf(out, cap, "%s", "");
        return;
    }
    if (nsdr == 1) {
        snprintf(out, cap, "carrier 2 by the SDR: out %.5f, in %.5f MHz", t->car[1].tx_hz / 1e6, t->car[1].rx_hz / 1e6);
    } else if (nsdr > 1) {
        int n = snprintf(out, cap, "carriers 2 to %d by the SDR: out", nsdr + 1);
        for (int j = 0; j < nsdr && n > 0 && (size_t)n < cap; j++)
            n += snprintf(out + n, cap - (size_t)n, "%s %.5f", j ? "," : "", t->car[j + 1].tx_hz / 1e6);
        if (n > 0 && (size_t)n < cap)
            snprintf(out + n, cap - (size_t)n, " MHz");
    } else {
        int n = 0;
        for (int j = all_split ? 0 : 1; j < t->ncar && n >= 0 && (size_t)n < cap; j++)
            n += snprintf(out + n, cap - (size_t)n, "%scarrier %d by a split TX half on %s: out %.5f, in %.5f MHz%s",
                          n ? "; " : "", j + 1, t->car[j].port, t->car[j].tx_hz / 1e6, t->car[j].rx_hz / 1e6,
                          SP[j].up ? "" : " (down)");
    }
    for (int j = 0; j < nsdr; j++)
        *sent += CX[j].m.src_bursts, *heard += CX[j].heard;
}

void rdofradio_t3_stop(void)
{
    if (!T.on)
        return;
    if (M) {
        rdof_sdrmodem_set_source(M, NULL);
        rdof_sdrmodem_hold_key(M, false);
    }
    for (int j = 0; j < nsdr; j++) {
        cx_t *x = &CX[j];
        if (!x->on)
            continue;
        tsay("carrier %d: %u bursts from the site, %u idle, %u heard; its pulls %.1f us a ms of stream (%.0f us each, "
             "%llu)", x->c, (unsigned)x->m.src_bursts, (unsigned)x->m.idle_bursts, (unsigned)x->heard,
             x->samples ? 1e6 * x->pull_s / ((double)x->samples / DEV.tx_fs * 1e3) : 0.0,
             x->pulls ? 1e6 * x->pull_s / (double)x->pulls : 0.0, (unsigned long long)x->pulls);
        rdof_sdrmodem_hold_key(&x->m, false);
    }
    c2_stop();
    hat_stop();
    for (int j = 0; j < RDOF_T3_CARRIERS; j++)
        if (SP[j].on)
            tsay("carrier %d: split: %u scheduled (%u with the CACH), %u by the FIFO, %u key-ups; its RX half's bursts "
                 "%u by RX_META's slot, %u by parity", j + 1, (unsigned)SP[j].ts.o.sched_bursts,
                 (unsigned)SP[j].ts.o.cach_bursts, (unsigned)SP[j].ts.o.fifo_bursts, (unsigned)SP[j].ts.o.keyups,
                 (unsigned)SP[j].by_meta, (unsigned)SP[j].by_parity);
    splits_stop();
    stop_common();
    air_stop();
}
