// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* rdofradio's trunked site (rdofradio_t3.h). */
#include "rdofradio_t3.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
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
#include "rdof/t3.h"
#include "rdof/t3hat.h"
#include "rdof/t3sim.h"
#include "rdof/t3unit.h"
#ifdef RDOF_MQTT
#include "rdofradio_ipgw.h"
#include "rdofradio_mqtt.h"
#endif
#include "rdofradio_serial.h"
#include "rdofradio_simext.h"
#include "rdofradio_utc.h"

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
    rdofmq_subscribe(T.mq, all, m);
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
    if (rdofgw_on())
        rdofgw_heard(0, (ch & 1) + 1, 0, -1, dt, burst, (double)wall_ms() / 1000.0);
}
static bool t3_gw_send(void *ctx, const rdof_ipgw_dmr_t *p)
{
    return rdof_t3unit_data_tx(T.u, p->src, p->dst, false, p->nbursts, p->control, &p->bursts[0][0]);
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
static bool air_command(const char *topic, const void *data, size_t len);

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
        if (!air_command(topic, data, len))
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
    rdof_t3unit_tick(T.u, now_ms);
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
    uint8_t what, flags, len;
    char text[64]; /* sdm: the message (a UDT carries up to 46 octets); data: the UDP payload */
} act_t;

#define ACT_SDM 0xF0       /* rdof_t3ms_sdm (ISO 8-bit text), not an rdof_t3ms_do */
#define ACT_SETSTATUS 0xF1 /* rdof_t3ms_set_status */
#define ACT_DATA 0xF2      /* rdof_t3ms_data: the text, UTF-16BE, in a UDP/IPv4 datagram (port 5016 to 5016) */
#define AIR_TEXT16 44     /* rate 1/2 data's characters at most: 28 + 2 x 44 octets fill 12 confirmed blocks */
#define ACT_HELP                                                                                                       \
    "ID register [TG] | deregister | group TG [TALK_MS] | private ID [TALK_MS] | status ID N | gstatus TG N | "        \
    "sdm ID TEXT | gsdm TG TEXT | data ID TEXT | cdata ID TEXT | gdata TG TEXT (data34, cdata1 ...: rate 3/4, 1) | "  \
    "setstatus N | cancel"

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
 * (confirmed) and gdata up to 44 characters (64 at rate 3/4 or 1), a packet data call carrying them as a text message, UTF-16BE on
 * UDP/IPv4 port 5016 (361-3 5.7.1), from 12.x.y.z to 12.x.y.z or, to a talkgroup, 225.x.y.z, in rate 1/2 blocks;
 * data34, cdata34, gdata34 in rate 3/4 blocks, data1, cdata1, gdata1 in rate 1): 1 an action in *x (at_ms 0), 0 a
 * blank line, -1 a bad one. */
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
    else if (!strcmp(what, "group") && got >= 3)
        x->what = RDOF_T3MS_DO_GROUP, x->arg = (uint32_t)a1, x->talk_ms = got >= 4 ? (uint32_t)a2 : 2000;
    else if (!strcmp(what, "private") && got >= 3)
        x->what = RDOF_T3MS_DO_PRIVATE, x->arg = (uint32_t)a1, x->talk_ms = got >= 4 ? (uint32_t)a2 : 2000;
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
        size_t n = strcspn(t, "\r\n");
        if (!n || n > (sdm ? 46 : act_data_rate(what) ? sizeof x->text : AIR_TEXT16))
            return -1;
        memcpy(x->text, t, n);
        x->what = sdm ? ACT_SDM : ACT_DATA, x->arg = (uint32_t)a1, x->len = (uint8_t)n,
        x->flags = (what[0] == 'g' ? RDOF_T3MS_F_GROUP : 0) | (what[0] == 'c' ? RDOF_T3MS_F_CONFIRMED : 0) |
                   (sdm ? 0 : (uint8_t)act_data_rate(what));
    } else if (!strcmp(what, "setstatus") && got >= 3 && a1 <= 127)
        x->what = ACT_SETSTATUS, x->arg = (uint32_t)a1;
    else if (!strcmp(what, "cancel"))
        x->what = RDOF_T3MS_DO_CANCEL;
    else
        return -1;
    return 1;
}

#define AIR_RADIO_NET 0x0C000000u /* 12.0.0.0/8: a radio's address, its id in the low 24 bits */

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
static bool act_run(rdof_t3ms_t *ms, const act_t *a, int64_t now)
{
    if (a->what == ACT_SETSTATUS) {
        rdof_t3ms_set_status(ms, (uint8_t)a->arg);
        return true;
    }
    /* a status sent is the one it answers a poll with after (as a radio does) */
    if (a->what == RDOF_T3MS_DO_STATUS && a->talk_ms < 127)
        rdof_t3ms_set_status(ms, (uint8_t)a->talk_ms);
    if (a->what == ACT_SDM)
        return rdof_t3ms_sdm(ms, a->arg, a->flags, 4 /* ISO 8-bit */, (const uint8_t *)a->text, a->len, now);
    if (a->what == ACT_DATA) {
        const rdof_ipgw_net_t net = {AIR_RADIO_NET, 0xFF000000u};
        const uint32_t dip = a->flags & RDOF_T3MS_F_GROUP ? RDOF_IPGW_GROUP_NET | (a->arg & 0xFFFFFF)
                                                          : rdof_ipgw_radio_ip(&net, a->arg);
        static uint16_t ip_id;
        uint8_t u16[2 * sizeof a->text], d[28 + sizeof u16];
        const size_t m = utf16be(a->text, a->len, u16, sizeof u16);
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
 * private 3105738 5000"). Carrier 2 too when the SDR sends it (its outbound heard, its inbound sent; not a split's). */
static struct {
    int n;
    rdof_t3ms_t *ms[RDOF_T3SIM_MS];
    uint32_t id[RDOF_T3SIM_MS];
    int voice_n[RDOF_T3SIM_MS];
    rdof_t3ms_status_t last[RDOF_T3SIM_MS];
    uint32_t sd_seen[RDOF_T3SIM_MS], pd_sum[RDOF_T3SIM_MS]; /* what air_inbox said last */
    act_t acts[ACTS];
    int nacts, ai, talker;
    int64_t next_talk;
    uint32_t in, out, bad;
    int ncar; /* the carriers they're on: 1, 2 when the SDR sends carrier 2 */
} AM;
static bool c2_sdr; /* carrier 2 is sent by the SDR (C2) */

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
    AM.ncar = c2_sdr ? 2 : 1;
    for (int i = 0; i < n; i++) {
        AM.ms[i] = malloc(rdof_t3ms_size());
        if (!AM.ms[i]) {
            snprintf(err, cap, "no memory");
            return false;
        }
        rdof_t3ms_init(AM.ms[i], AM.id[i], tg, 1, chans, AM.ncar, T.rng ^ (AM.id[i] * 2654435761u));
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
    tsay("air MSs: %d (%u..%u), talkgroup %u, on carrier%s%s%s", n, (unsigned)AM.id[0], (unsigned)AM.id[n - 1],
         (unsigned)o->sim_tg, AM.ncar > 1 ? "s 1 and 2" : " 1", air_topic[0] ? "; commands on " : "", air_topic);
    return true;
}

static void air_do(const act_t *a, int64_t now)
{
    const int i = air_find(a->id);
    if (i < 0) {
        tsay("air MSs: no %u here", (unsigned)a->id);
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
    uint8_t d[300];
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
            tsay("air MS %u: packet data from %u, UDP %u.%u.%u.%u:%u to %u.%u.%u.%u:%u, %zu octets, UTF-16BE text: "
                 "\"%s\"",
                 (unsigned)AM.id[i], (unsigned)src, d[12], d[13], d[14], d[15], d[20] << 8 | d[21], d[16], d[17],
                 d[18], d[19], d[22] << 8 | d[23], u, t);
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
    bool sent[2] = {false, false};
    for (int i = 0; i < AM.n; i++) {
        rdof_t3ms_tx_t tx;
        if (!rdof_t3ms_tx(AM.ms[i], k, now, &tx))
            continue;
        rdof_burst_t b;
        const int n = AM.voice_n[i];
        AM.voice_n[i] = tx.kind != 1 && tx.what == RDOF_T3_VOICE_FRAME ? (n + 1) % 6 : 0;
        const int c = tx.ch >> 1;
        if (c >= AM.ncar || sent[c] || !rdof_t3sim_ms_burst(&tx, n, cc, &b))
            continue;
        b.slot = (tx.ch & 1) + 1;
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
    for (int i = 0; i < AM.n; i++) {
        rdof_t3ms_status_t st;
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

/* carrier 2 when the SDR sends it: its own modem, summed into the transmit stream (rdofradio_t3_mix) */
#define MIX_N 2048 /* samples a pull */
static struct {
    rdof_sdrmodem_t m;
    bool on;
    slotmap_t s;
    int chan;            /* the receiver's channel on its inbound */
    double lo;           /* the TX LO it was tuned against */
    bool lo_moved;       /* a bench job has the LO elsewhere: unsent */
    uint32_t heard;      /* bursts heard on its inbound, given to the site */
    double pull_s;       /* time in its pulls and the sum */
    uint64_t pulls, samples;
    rdof_cf_t scratch[MIX_N];
} C2 = {.chan = -1};

/* Modem m's slot k (the source's k) on the trunk's slots, for carrier c (1, 2). Off the grid carrier 2 takes carrier
 * 1's d when the two modems' slots start on the same sample (keyed together), so both count alike. */
static int64_t trunk_k(slotmap_t *p, const rdof_sdrmodem_t *m, int64_t k, int c)
{
    const bool on_grid = m->grid_placed;
    if (on_grid) {
        if (!p->placed || !p->on_grid)
            tsay("carrier %d: on the slot grid (modem slot %lld is the UTC slot)", c, (long long)k);
        p->d = 0;
        p->placed = true;
    } else if (!p->placed || p->on_grid || k < p->last_k) { /* the first, off the grid, or the modem started again */
        if (c == 2 && A.s.placed && !A.s.on_grid && M && m->base == M->base) {
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

/* carrier 2's slot source: the controller's carrier 2 (its two payload channels) */
static bool source_slot2(void *ctx, int64_t k, uint8_t burst[RDOF_BURST_BYTES], int *at, int32_t *slc28)
{
    uint8_t control;
    pthread_mutex_lock(&T.mu);
    rdof_t3unit_set_now(T.u, wall_ms());
    const int64_t tk = trunk_k(&C2.s, &C2.m, k, 2);
    const bool ok = rdof_t3unit_slot(T.u, 1, tk, burst, &control, at, slc28);
    air_out(1, tk, burst, ok);
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
    if (!C2.on)
        return;
    rdof_sdrmodem_set_source(&C2.m, NULL);
    rdof_sdrmodem_free(&C2.m);
    C2.on = c2_sdr = false;
}

/* Both carriers' outs within the transmit stream's reach of the one TX LO (rdofradio puts it between them) */
static bool c2_fits(const rdofradio_t3_cfg_t *t, char *err, size_t cap)
{
    const double lo = DEV.tx_lo();
    for (int c = 0; c < 2; c++) {
        double off, reach;
        if (DEV.tx_offset(t->car[c].tx_hz, &off, &reach))
            continue;
        snprintf(err, cap, "carriers 1 and 2 out on %.5f and %.5f MHz, %.1f kHz apart: the TX LO on %.5f MHz reaches "
                           "%.1f kHz either side at %.0f kS/s (the transmit stream's rate), carrier %d is %.1f kHz off it",
                 t->car[0].tx_hz / 1e6, t->car[1].tx_hz / 1e6,
                 fabs((double)t->car[0].tx_hz - (double)t->car[1].tx_hz) / 1e3, lo / 1e6, reach / 1e3,
                 DEV.tx_fs / 1e3, c + 1, fabs(off) / 1e3);
        return false;
    }
    return true;
}

/* carrier 2's modem: duplex, tuned (in and out), DMR, its slot source; keyed with carrier 1 (key_carriers) */
static bool c2_start(const rdofradio_t3_cfg_t *t, const rdof_sdrmodem_t *m1, char *err, size_t cap)
{
    if (!DEV.tx_offset || !DEV.tx_lo || !DEV.tx_fs) {
        snprintf(err, cap, "carrier 2: this device can't send a second carrier");
        return false;
    }
    if (!c2_fits(t, err, cap))
        return false;
    const uint32_t rx = t->car[1].rx_hz, tx = t->car[1].tx_hz;
    C2.chan = DEV.channel(rx);
    if (C2.chan < 0) {
        snprintf(err, cap, "carrier 2: the receiver has no channel on its inbound %.5f MHz", rx / 1e6);
        return false;
    }
    const rdof_sdrmodem_io_t io = {c2_reply, c2_channel, c2_tune, NULL, NULL, NULL};
    if (rdof_sdrmodem_init(&C2.m, &io, DEV.rx_fs, DEV.tx_fs, true)) {
        snprintf(err, cap, "carrier 2: modem: %s", C2.m.error);
        return false;
    }
    rdof_sdrmodem_carrier_ppm(&C2.m, m1->carrier_ppm);
    /* as the repeater sets carrier 1's: duplex (SET_CONFIG without the simplex flag) on the site's CC, both
       frequencies, DMR */
    uint8_t config[23] = {0}, freq[14], f[64];
    config[1] = RDOF_DMR_ENABLED;
    config[3] = RDOF_MODE_DMR;
    config[6] = (uint8_t)(t->cfg.cc & 0x0F);
    const uint8_t mode = RDOF_MODE_DMR;
    rdof_sdrmodem_write(&C2.m, f, rdof_mmdvm_frame(RDOF_MMDVM_SET_CONFIG, config, sizeof config, f, sizeof f));
    rdof_sdrmodem_write(&C2.m, f,
                        rdof_mmdvm_frame(RDOF_MMDVM_SET_FREQ, freq, rdof_mmdvm_set_freq_payload(rx, tx, 255, freq), f,
                                         sizeof f));
    rdof_sdrmodem_write(&C2.m, f, rdof_mmdvm_frame(RDOF_MMDVM_SET_MODE, &mode, 1, f, sizeof f));
    if (!C2.m.duplex || !C2.m.tuned || C2.m.mode != RDOF_MODE_DMR) {
        snprintf(err, cap, "carrier 2: its modem refused the settings%s%s", C2.m.error[0] ? ": " : "", C2.m.error);
        rdof_sdrmodem_free(&C2.m);
        return false;
    }
    const rdof_sdrmodem_source_t src = {source_slot2, NULL};
    rdof_sdrmodem_set_source(&C2.m, &src);
    C2.lo = DEV.tx_lo();
    C2.s = (slotmap_t){0};
    C2.on = c2_sdr = true;
    double off, reach;
    DEV.tx_offset(tx, &off, &reach);
    tsay("carrier 2: sent by the SDR (tx %.5f, rx %.5f MHz), %+.1f kHz from the TX LO %.5f MHz, %.0f kS/s; each "
         "carrier at half amplitude",
         tx / 1e6, rx / 1e6, off / 1e3, C2.lo / 1e6, DEV.tx_fs / 1e3);
    return true;
}

/* carrier 2: its split TX half */
#define BQ 64
static struct {
    bool on, up;
    int fd, chan;
    pthread_t thread;
    volatile bool quit, gone;
    pthread_mutex_t mu;
    struct {
        uint8_t b[256];
        int n;
    } q[BQ];
    int head, tail;
    uint32_t dropped;
    rdof_t3split_t ts;
    double utc_minus_host_us; /* UTC - the monotonic clock, µs */
    int64_t heard_k;          /* the UTC slot of the last burst heard on its inbound */
    double next_utc;          /* when to measure UTC - host again */
} BC = {.fd = -1, .chan = -1, .mu = PTHREAD_MUTEX_INITIALIZER};

static double utc_minus_host_us(void)
{
    struct timespec w;
    const double h0 = DEV.now();
    clock_gettime(CLOCK_REALTIME, &w);
    const double h1 = DEV.now();
    return ((double)w.tv_sec * 1e6 + w.tv_nsec / 1e3) - 1e6 * (h0 + h1) / 2;
}

static int64_t utc_slot_now(void) { return (int64_t)floor((DEV.now() * 1e6 + BC.utc_minus_host_us) / 30000.0); }


static void *b_reader(void *arg)
{
    while (!BC.quit) {
        struct pollfd pf = {BC.fd, POLLIN, 0};
        if (poll(&pf, 1, 100) <= 0)
            continue;
        uint8_t b[256];
        const int n = rdofradio_serial_read(BC.fd, b, sizeof b);
        if (n < 0 || (pf.revents & (POLLHUP | POLLERR | POLLNVAL))) {
            BC.gone = true;
            break;
        }
        if (n == 0)
            continue;
        pthread_mutex_lock(&BC.mu);
        if (BC.head - BC.tail < BQ) {
            const int i = BC.head++ % BQ;
            BC.q[i].n = n;
            memcpy(BC.q[i].b, b, (size_t)n);
        } else {
            BC.dropped += (uint32_t)n;
        }
        pthread_mutex_unlock(&BC.mu);
    }
    return NULL;
}

static void b_to_radio(void *ctx, const uint8_t *d, size_t n)
{
    if (!rdofradio_serial_write(BC.fd, d, n) && !BC.gone) {
        BC.gone = true;
        tsay("carrier 2: the port write failed");
    }
}
static int b_channel(void *ctx, uint32_t hz) { return DEV.channel(hz); }
static int64_t b_now48(void *ctx) { return DEV.now48(); }

/* the split's bytes from the radio, its timers, what its RX half heard; T.mu held */
static void b_take(void)
{
    int slot;
    uint8_t control, burst[RDOF_BURST_BYTES];
    while (rdof_t3split_take(&BC.ts, &slot, &control, burst, NULL)) {
        /* the slot of that parity nearest the last burst heard there */
        int64_t k = BC.heard_k;
        if ((k & 1) != slot - 1)
            k += 1;
        rdof_burst_t b;
        rdof_t3split_burst(&BC.ts, slot, control, burst, &b);
        rdof_t3unit_rx(T.u, 1, k, &b, 0);
    }
}

static void b_service(double now)
{
    for (;;) {
        uint8_t b[256];
        int n = 0;
        pthread_mutex_lock(&BC.mu);
        if (BC.tail != BC.head) {
            const int i = BC.tail++ % BQ;
            n = BC.q[i].n;
            memcpy(b, BC.q[i].b, (size_t)n);
        }
        pthread_mutex_unlock(&BC.mu);
        if (!n)
            break;
        rdof_t3split_from_radio(&BC.ts, b, (size_t)n, now);
    }
    rdof_t3split_tick(&BC.ts, now);
}

static void b_stop(void)
{
    if (!BC.on)
        return;
    BC.quit = true;
    pthread_join(BC.thread, NULL);
    close(BC.fd);
    rdof_t3split_free(&BC.ts);
    BC.on = false;
}

/* carrier 2's split: its port, the bring-up and its settings (the receiver isn't running yet: no RX half) */
static bool b_start(const rdofradio_t3_cfg_t *t, char *err, size_t cap)
{
    const uint32_t rx = t->car[1].rx_hz, tx = t->car[1].tx_hz;
    BC.chan = DEV.channel(rx);
    if (BC.chan < 0) {
        snprintf(err, cap, "carrier 2: the receiver has no channel on its inbound %.5f MHz", rx / 1e6);
        return false;
    }
    BC.fd = rdofradio_serial_open(t->car[1].port, t->car[1].baud ? (int)t->car[1].baud : 115200, err, cap);
    if (BC.fd < 0)
        return false;
    if (pthread_create(&BC.thread, NULL, b_reader, NULL)) {
        close(BC.fd);
        snprintf(err, cap, "carrier 2: the port reader: no thread");
        return false;
    }
    const rdof_t3split_io_t io = {b_to_radio, b_channel, b_now48, NULL};
    BC.on = true;
    if (!rdof_t3split_init(&BC.ts, &io, DEV.rx_fs, 1, rx, tx, t->cfg.cc, DEV.now())) {
        snprintf(err, cap, "carrier 2: %s", BC.ts.error);
        b_stop();
        return false;
    }
    const double until = DEV.now() + 8.0;
    while (BC.ts.state != RDOF_T3SPLIT_UP && BC.ts.state != RDOF_T3SPLIT_FAILED && !BC.gone &&
           DEV.now() < until) {
        usleep(10000);
        b_service(DEV.now());
    }
    if (BC.ts.state != RDOF_T3SPLIT_UP) {
        snprintf(err, cap, "carrier 2 (%s): %s", t->car[1].port,
                 BC.gone ? "the port went away"
                 : BC.ts.state == RDOF_T3SPLIT_FAILED ? BC.ts.error
                                                         : "no split TX half answered in 8 s");
        b_stop();
        return false;
    }
    BC.utc_minus_host_us = utc_minus_host_us();
    rdof_t3split_utc(&BC.ts, BC.utc_minus_host_us, 1000, DEV.now());
    BC.next_utc = DEV.now() + 60;
    BC.up = true;
    tsay("carrier 2: split TX half on %s (tx %.5f, rx %.5f MHz), aligning to UTC", t->car[1].port, tx / 1e6,
         rx / 1e6);
    return true;
}

/* ── carrier 1 on a duplex MMDVM modem (--tier3 with --mmdvm; docs/spec/fw/tier3-hat.md) ─────────────────────── */

#define HAT_POLL_S 0.25 /* GET_STATUS this often: the modem's watchdog drops it to IDLE after about 1 s without */
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
    if (rdof_t3hat_lc_start(H.h, k)) {
        /* the short LC for the modem's 4 CACH from k, latched as it builds the first: before slot k's burst. Its
         * cycle starts before TS1 (k % 4 == 0) or TS2 (k % 4 == 1): the controller's cycle from k & ~3 */
        int64_t slc = rdof_t3unit_slc(T.u, 0, k - (k & 3));
        if (slc < 0)
            slc = slc28;
        if (slc >= 0) {
            uint8_t s9[9];
            hat_slc9((uint32_t)slc, s9);
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
        if (kind == RDOF_T3HAT_GRID)
            tsay("carrier 1 (modem): its slot grid from what the receiver hears%s", H.last.grids ? " (again: it keyed up anew)" : "");
        else if (kind == RDOF_T3HAT_PLACE)
            tsay("carrier 1 (modem): trunk slot %lld is its slot %lld; its short LC cycle from k %% 4 == %.0f", (long long)k,
                 (long long)m, v[1]);
        else if (kind == RDOF_T3HAT_REKEY)
            tsay("carrier 1 (modem): its status says unkeyed: placed again when it keys");
        else if (kind == RDOF_T3HAT_ABORT && T.o.verbose)
            tsay("carrier 1 (modem): TS%d flushed before slot %lld (%s)", ts, (long long)k,
                 v[0] == 0 ? "aired late" : v[0] == 1 ? "its queue too deep" : "NAKed");
        else if (kind == RDOF_T3HAT_AIR) {
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

/* an outbound burst heard on carrier 1's out; T.mu held */
static void hat_heard_out(int64_t pos48, int slot, int at, int lcss, const uint8_t burst[RDOF_BURST_BYTES])
{
    rdof_t3hat_heard(H.h, pos48, slot, at, lcss, burst, NULL);
    H.heard_out++;
}

void rdofradio_t3_hat_heard(int64_t pos48, int slot, int at, int lcss, const uint8_t burst[RDOF_BURST_BYTES])
{
    if (!H.on)
        return;
    pthread_mutex_lock(&T.mu);
    hat_heard_out(pos48, slot, at, lcss, burst);
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
        hat_heard_out(b->position, slot, at, lcss, b->b.burst);
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
            rdofradio_t3_hat_heard((int64_t)llround(t * 48000.0 + dither), slot, at, lcss, burst);
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
    if (t->ncar > 2) {
        snprintf(err, cap, "%s: %d carriers: rdofradio runs two (the SDR's, and one more the SDR or a split TX half "
                           "sends)", t->name, t->ncar);
        return false;
    }
    if (!t->car[0].tx_sdr) {
        snprintf(err, cap, "%s: the TSCC's carrier is sent by the SDR here", t->name);
        return false;
    }
    if (t->ncar == 2 && t->car[1].tx_sdr && t->car[1].tx_dev != t->car[0].tx_dev) {
        snprintf(err, cap, "%s: carrier 2's out is on another SDR: rdofradio sends both carriers from the one it runs",
                 t->name);
        return false;
    }
    if (t->ncar == 2 && !t->car[1].tx_sdr && !t->car[1].port[0]) {
        snprintf(err, cap, "%s: carrier 2 is the SDR's (its out among the SDR's transmits) or a split TX half: a radio "
                           "with a port path (port: {\"path\": ...})", t->name);
        return false;
    }
    DEV = *dev;
    A.chan = DEV.channel(t->car[0].rx_hz);
    if (t->ncar == 2 && t->car[1].tx_sdr && !c2_start(t, m, err, cap))
        return false;
    if (t->ncar == 2 && !t->car[1].tx_sdr && !b_start(t, err, cap))
        return false;
    if (!start_common(c, o, err, cap)) {
        b_stop();
        c2_stop();
        return false;
    }
    if (!air_start(o, err, cap)) {
        b_stop();
        c2_stop();
        stop_common();
        return false;
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
    if (C2.on && !rdof_sdrmodem_hold_key(&C2.m, true))
        tsay("carrier 2: the modem won't hold its key");
    return rdof_sdrmodem_hold_key(M, true);
}

bool rdofradio_t3_on(void) { return T.on; }

void rdofradio_t3_heard(int c, const rdof_sdr_burst_t *b)
{
    if (!T.on || c < 0)
        return;
    if (H.on) {
        hat_rx(c, b);
        return;
    }
    if (c == A.chan) {
        const int64_t k = M ? rdof_sdrmodem_inbound_k(M, b) : -1;
        pthread_mutex_lock(&T.mu);
        if (k >= 0 && A.s.placed)
            rdof_t3unit_rx(T.u, 0, k + A.s.d, &b->b, 0);
        pthread_mutex_unlock(&T.mu);
    }
    if (C2.on && c == C2.chan) {
        rdof_sdrmodem_heard(&C2.m, c, b); /* its inbound's busy state: the AT in carrier 2's CACH */
        const int64_t k = rdof_sdrmodem_inbound_k(&C2.m, b);
        pthread_mutex_lock(&T.mu);
        if (k >= 0 && C2.s.placed) {
            rdof_t3unit_rx(T.u, 1, k + C2.s.d, &b->b, 0);
            C2.heard++;
            if (T.o.verbose)
                tsay("carrier 2: heard %s dt %d on TS%d, trunk slot %lld", rdof_family_name(b->b.family), b->b.dt,
                     b->b.slot, (long long)(k + C2.s.d));
        }
        pthread_mutex_unlock(&T.mu);
    }
    if (BC.up && c == BC.chan) {
        pthread_mutex_lock(&T.mu);
        /* its UTC slot from its position: the burst starts on the slot's start */
        const double t = DEV.now() - (double)(DEV.now48() - b->position) / 48000.0;
        BC.heard_k = (int64_t)floor((t * 1e6 + BC.utc_minus_host_us) / 30000.0 + 0.25);
        rdof_t3split_heard(&BC.ts, c, b);
        b_take();
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
    if (BC.up) {
        const double now = DEV.now();
        if (now >= BC.next_utc) { /* the host's clock follows NTP: the TX half follows it */
            BC.utc_minus_host_us = utc_minus_host_us();
            rdof_t3split_utc(&BC.ts, BC.utc_minus_host_us, 1000, now);
            BC.next_utc = now + 60;
        }
        b_service(now);
        b_take();
        if (BC.ts.state == RDOF_T3SPLIT_FAILED || BC.gone) {
            tsay("carrier 2: %s; its payload channels go unsent", BC.gone ? "the port went away" : BC.ts.error);
            BC.up = false;
        } else {
            rdof_t3unit_split_pump(T.u, &BC.ts.o, rdof_t3split_split(&BC.ts), utc_slot_now(), now);
        }
    }
    pthread_mutex_unlock(&T.mu);
}

bool rdofradio_t3_mix(int64_t pos, rdof_cf_t *iq, int n)
{
    if (!C2.on || !T.on || !M)
        return false;
    const bool moved = DEV.tx_lo() != C2.lo;
    if (moved != C2.lo_moved)
        tsay("carrier 2: %s", moved ? "the TX LO moved (a bench job): unsent until it's back" : "the TX LO is back: sent");
    C2.lo_moved = moved;
    C2.m.tx_late48 = M->tx_late48; /* the transmit path's delay is the stream's: carrier 1's measure */
    if (C2.m.carrier_ppm != M->carrier_ppm)
        rdof_sdrmodem_carrier_ppm(&C2.m, M->carrier_ppm);
    rdofradio_utc_apply(&C2.m);
    const double t0 = DEV.now();
    bool keyed = false;
    for (int i = 0; i < n; i += MIX_N) {
        const int k = n - i < MIX_N ? n - i : MIX_N;
        if (moved)
            rdof_sdrmodem_pull_iq(&C2.m, pos + i, C2.scratch, k); /* its clock runs on */
        else
            keyed |= rdof_sdrmodem_pull_mix(&C2.m, pos + i, iq + i, C2.scratch, k);
    }
    C2.pull_s += DEV.now() - t0;
    C2.pulls++;
    C2.samples += (uint64_t)n;
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
    if (C2.on) {
        snprintf(out, cap, "carrier 2 by the SDR: out %.5f, in %.5f MHz", t->car[1].tx_hz / 1e6, t->car[1].rx_hz / 1e6);
        *sent = C2.m.src_bursts, *heard = C2.heard;
    } else {
        snprintf(out, cap, "carrier 2 by a split TX half on %s: out %.5f, in %.5f MHz%s", t->car[1].port,
                 t->car[1].tx_hz / 1e6, t->car[1].rx_hz / 1e6, BC.up ? "" : " (down)");
    }
}

void rdofradio_t3_stop(void)
{
    if (!T.on)
        return;
    if (M) {
        rdof_sdrmodem_set_source(M, NULL);
        rdof_sdrmodem_hold_key(M, false);
    }
    if (C2.on) {
        tsay("carrier 2: %u bursts from the site, %u idle, %u heard; its pulls %.1f us a ms of stream (%.0f us each, "
             "%llu)", (unsigned)C2.m.src_bursts, (unsigned)C2.m.idle_bursts, (unsigned)C2.heard,
             C2.samples ? 1e6 * C2.pull_s / ((double)C2.samples / DEV.tx_fs * 1e3) : 0.0,
             C2.pulls ? 1e6 * C2.pull_s / (double)C2.pulls : 0.0, (unsigned long long)C2.pulls);
        rdof_sdrmodem_hold_key(&C2.m, false);
        c2_stop();
    }
    hat_stop();
    b_stop();
    stop_common();
    air_stop();
}
