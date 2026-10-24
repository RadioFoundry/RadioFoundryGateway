// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The IP gateway on the board (docs/spec/fw/ipgw.md): rdof_ipgw on a task of its own. The receivers' trackers
 * (the SDR's channels, each roamer's or repeater's radio) hand it their data bursts through a queue; each flow
 * has a UDP socket of its own on an ephemeral port; replies go out through a roamer (as a network call to its
 * transceiver) or a repeater (as network bursts on the slot the radio was heard on).
 */
#include <string.h>

#include "app.h"
#include "mem.h"

#if CONFIG_RDOF_IPGW

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "fcntl.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "rdof/ipgw.h"
#include "rdof/ipgwtx.h"
#include "rdof/layer2.h"
#include "rdof/mmdvm.h"
#include "rdof/pdu.h"

static const char *TAG = "ipgw";

#define QUEUE_LEN 64
#define MAX_SOURCES (24 + RDOF_MAX_RADIOS) /* the SDR's channels and taps, and the radios */
#define RESOLVE_EVERY_S 30.0
#define TICK_EVERY_S 0.25

typedef struct {
    int16_t source;
    uint8_t slot, dt;
    int8_t cc;
    uint32_t freq;
    uint8_t burst[RDOF_BURST_BYTES];
} heard_t;

typedef struct {
    int source;
    uint32_t freq;
    int cc;
    const double *freq_now; /* a roamer's tracker follows its radio around */
} source_t;

static rdof_ipgw_t *s_gw;
static QueueHandle_t s_queue;
static int s_sock[RDOF_IPGW_MAX_FLOWS];
static int s_ttl[RDOF_IPGW_MAX_FLOWS]; /* the IP_TTL each socket has */
static uint16_t s_port[RDOF_IPGW_MAX_FLOWS]; /* the local port each is bound to */
static int s_icmp = -1, s_icmp_ttl;    /* NAT's ICMP: one raw socket (a raw PCB; lwIP always has LWIP_RAW) */
static source_t s_sources[MAX_SOURCES];
static volatile uint32_t s_queue_full;
static char s_error[96];

static double mono(void)
{
    return esp_timer_get_time() / 1e6;
}

/* ── in: the trackers' sinks (any task; the SDR's on core 1) ───────────────── */

static void on_frame(void *ctx, const char *channel, const rdof_frame_t *f)
{
    const source_t *s = ctx;
    if (f->dt != RDOF_DT_DATA_HEADER && f->dt != RDOF_DT_RATE_12 && f->dt != RDOF_DT_RATE_34 &&
        f->dt != RDOF_DT_RATE_1)
        return;
    heard_t h = {.source = (int16_t)s->source, .slot = (uint8_t)f->slot, .dt = (uint8_t)f->dt, .cc = (int8_t)s->cc};
    h.freq = s->freq_now && *s->freq_now > 0 ? (uint32_t)*s->freq_now : s->freq;
    memcpy(h.burst, f->burst, RDOF_BURST_BYTES);
    if (xQueueSend(s_queue, &h, 0) != pdTRUE)
        s_queue_full++;
}

bool ipgw_sink(int source, uint32_t freq, int cc, const double *freq_now, rdof_sink_t *out)
{
    if (!s_gw || !s_queue)
        return false;
    source_t *s = NULL;
    for (int i = 0; i < MAX_SOURCES && !s; i++)
        if (s_sources[i].source == source || s_sources[i].source < 0)
            s = &s_sources[i];
    if (!s)
        return false;
    *s = (source_t){.source = source, .freq = freq, .cc = cc >= 0 && cc <= 15 ? cc : -1, .freq_now = freq_now};
    *out = (rdof_sink_t){.on_frame = on_frame, .ctx = s};
    return true;
}

/* ── the flows' sockets ────────────────────────────────────────────────────── */

static bool open_flow(void *ctx, int flow)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        ESP_LOGW(TAG, "flow %d: no socket (lwIP has %d)", flow, CONFIG_LWIP_MAX_SOCKETS);
        return false;
    }
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = 0, .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
        close(s);
        return false;
    }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    socklen_t al = sizeof a;
    s_port[flow] = getsockname(s, (struct sockaddr *)&a, &al) == 0 ? ntohs(a.sin_port) : 0;
    s_sock[flow] = s;
    s_ttl[flow] = 0;
    return true;
}

static void close_flow(void *ctx, int flow)
{
    if (s_sock[flow] >= 0)
        close(s_sock[flow]);
    s_sock[flow] = -1;
}

static bool send_udp(void *ctx, int flow, uint32_t ip, uint16_t port, const uint8_t *data, size_t len, int ttl)
{
    if (s_sock[flow] < 0)
        return false;
    /* the radio's TTL less one, as a router would; a whole datagram (0) goes in UDP with the stack's own */
    int want = ttl > 0 ? ttl : 64;
    if (want != s_ttl[flow] && setsockopt(s_sock[flow], IPPROTO_IP, IP_TTL, &want, sizeof want) == 0)
        s_ttl[flow] = want;
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(ip)};
    return sendto(s_sock[flow], data, len, 0, (struct sockaddr *)&a, sizeof a) == (ssize_t)len;
}

static uint16_t local_port(void *ctx, int flow)
{
    return s_port[flow];
}

static bool send_icmp(void *ctx, uint32_t ip, const uint8_t *m, size_t len, int ttl)
{
    if (s_icmp < 0)
        return false;
    if (ttl != s_icmp_ttl && setsockopt(s_icmp, IPPROTO_IP, IP_TTL, &ttl, sizeof ttl) == 0)
        s_icmp_ttl = ttl;
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(ip)};
    return sendto(s_icmp, m, len, 0, (struct sockaddr *)&a, sizeof a) == (ssize_t)len;
}

/* ── out: a roamer or repeater transmits ───────────────────────────────────── */

#if CONFIG_RDOF_ROAMER

/* The radio that transmits: ip_gateway.tx by name, else the first that runs. */
static roam_t *tx_radio(void)
{
    for (int k = 0; k < g_nroam; k++) {
        roam_t *r = g_roam[k];
        if (r && r->cfg && (!s_gw->cfg.tx[0] || !strcmp(r->cfg->name, s_gw->cfg.tx)))
            return r;
    }
    return NULL;
}

static rdof_ipgw_tx_t *s_tx; /* a packet going on air (rdof/ipgwtx.h) */

static bool send_dmr(void *ctx, const rdof_ipgw_dmr_t *p)
{
    roam_t *r = tx_radio();
    if (!r || !r->st.radio_ready || !s_tx)
        return false;
    uint32_t rnd = esp_random();
    uint8_t sid[4] = {(uint8_t)(rnd >> 24), (uint8_t)(rnd >> 16), (uint8_t)(rnd >> 8), (uint8_t)rnd};
#if RDOF_FEATURE_TX
    if (r->cfg->mode == RDOF_CFG_ROAMER && r->txvr)
        return rdof_ipgw_tx_txvr(s_tx, r->txvr, p, sid);
#endif
#if RDOF_FEATURE_REPEATER
    if (r->cfg->mode == RDOF_CFG_REPEATER && r->rpt)
        return rdof_ipgw_tx_repeater(s_tx, r->rpt, p, sid);
#endif
    return false;
}

/* The rest of a packet a repeater couldn't take at once, or that waits for its slot. */
static void send_more(void)
{
#if RDOF_FEATURE_REPEATER
    roam_t *r = tx_radio();
    if (s_tx && s_tx->busy && r && r->cfg->mode == RDOF_CFG_REPEATER && r->rpt)
        rdof_ipgw_tx_more(s_tx, r->rpt);
    if (s_tx)
        s_gw->n.tx_busy = s_tx->gave_up;
#endif
}

/* MS data sync for a simplex roamer, BS for a repeater. */
static uint64_t tx_sync(void)
{
    roam_t *r = tx_radio();
    return r && r->cfg->mode == RDOF_CFG_REPEATER ? rdof_data_sync(RDOF_BS_VOICE) : rdof_data_sync(RDOF_MS_VOICE);
}

#else

static bool send_dmr(void *ctx, const rdof_ipgw_dmr_t *p)
{
    return false; /* a build without the roamer: nothing transmits */
}

static void send_more(void) {}

static uint64_t tx_sync(void)
{
    return rdof_data_sync(RDOF_MS_VOICE);
}

#endif

/* ── the task ──────────────────────────────────────────────────────────────── */

static void resolve(void)
{
    for (int i = 0; i < s_gw->cfg.nroutes; i++) {
        rdof_ipgw_route_t *r = &s_gw->cfg.routes[i];
        if (r->ip)
            continue;
        struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM}, *res = NULL;
        if (getaddrinfo(r->host, NULL, &hints, &res) == 0 && res) {
            r->ip = ntohl(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr);
            ESP_LOGI(TAG, "route %s: %s is %u.%u.%u.%u", r->name, r->host, (unsigned)(r->ip >> 24),
                     (unsigned)(r->ip >> 16 & 255), (unsigned)(r->ip >> 8 & 255), (unsigned)(r->ip & 255));
        }
        if (res)
            freeaddrinfo(res);
    }
}

static void ipgw_task(void *arg)
{
    static uint8_t buf[RDOF_IPGW_MAX_PACKET + 64];
    double next_tick = 0, next_resolve = 0;
    for (;;) {
        bool any = false;
        int top = -1;
        fd_set rs;
        FD_ZERO(&rs);
        for (int i = 0; i < RDOF_IPGW_MAX_FLOWS; i++)
            if (s_sock[i] >= 0) {
                FD_SET(s_sock[i], &rs);
                top = s_sock[i] > top ? s_sock[i] : top;
                any = true;
            }
        if (s_icmp >= 0) {
            FD_SET(s_icmp, &rs);
            top = s_icmp > top ? s_icmp : top;
            any = true;
        }
        heard_t h;
        /* wait on the queue while there are no sockets, else on the sockets a little */
        BaseType_t got = xQueueReceive(s_queue, &h, any ? 0 : pdMS_TO_TICKS(20));
        s_gw->sync48 = tx_sync();
        while (got == pdTRUE) {
            rdof_ipgw_where_t w = {.source = h.source, .slot = h.slot, .freq = h.freq, .cc = h.cc};
            rdof_ipgw_burst(s_gw, &w, h.dt, h.burst, mono());
            got = xQueueReceive(s_queue, &h, 0);
        }
        if (any) {
            struct timeval tv = {.tv_sec = 0, .tv_usec = 10000};
            int ready = select(top + 1, &rs, NULL, NULL, &tv);
            if (ready > 0 && s_icmp >= 0 && FD_ISSET(s_icmp, &rs)) {
                int n; /* a raw socket hands over the IPv4 header too */
                while ((n = recv(s_icmp, buf, sizeof buf, 0)) > 0)
                    rdof_ipgw_icmp_from_net(s_gw, buf, (size_t)n, mono());
            }
            if (ready > 0)
                for (int i = 0; i < RDOF_IPGW_MAX_FLOWS; i++) {
                    if (s_sock[i] < 0 || !FD_ISSET(s_sock[i], &rs))
                        continue;
                    struct sockaddr_in from;
                    socklen_t fl = sizeof from;
                    int n;
                    while ((n = recvfrom(s_sock[i], buf, sizeof buf, 0, (struct sockaddr *)&from, &fl)) >= 0) {
                        rdof_ipgw_from_net(s_gw, i, ntohl(from.sin_addr.s_addr), ntohs(from.sin_port), buf, (size_t)n,
                                           mono());
                        if (s_sock[i] < 0)
                            break;
                        fl = sizeof from;
                    }
                }
        }
        send_more();
        double t = mono();
        if (t >= next_tick) {
            rdof_ipgw_tick(s_gw, t);
            next_tick = t + TICK_EVERY_S;
        }
        if (t >= next_resolve && g_board.wifi_up) {
            resolve();
            next_resolve = t + RESOLVE_EVERY_S;
        }
    }
}

/* The settings' ip_gateway, parsed once (for the memory plan, then the start): NULL if it's off or doesn't parse. */
static rdof_ipgw_config_t *s_cfg;
static bool s_prepared;

static rdof_ipgw_config_t *prepare(void)
{
    if (s_prepared)
        return s_cfg;
    s_prepared = true;
    char *text = settings_text();
    if (!text)
        return NULL;
    rdof_ipgw_config_t *cfg = heap_caps_malloc(sizeof *cfg, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cfg)
        cfg = malloc(sizeof *cfg);
    bool ok = cfg && rdof_ipgw_settings_parse(text, strlen(text), cfg, s_error, sizeof s_error);
    free(text);
    if (!ok || !cfg->on) {
        if (cfg && !ok)
            ESP_LOGE(TAG, "%s", s_error);
        free(cfg);
        return NULL;
    }
    if (cfg->max_flows > CONFIG_RDOF_IPGW_MAX_SOCKETS) {
        ESP_LOGW(TAG, "max_flows %d: this build has %d sockets for it", cfg->max_flows, CONFIG_RDOF_IPGW_MAX_SOCKETS);
        cfg->max_flows = CONFIG_RDOF_IPGW_MAX_SOCKETS;
    }
    return s_cfg = cfg;
}

#define QUEUE_MAX (4 * QUEUE_LEN)
#define FLOW_INTERNAL 512 /* lwIP's share of a flow's UDP socket: the netconn, its PCB and mailbox */

/* The memory plan's "ipgw" (mem.c): its task's stack, the heard queue (QUEUE_LEN bursts, deeper while the spare
 * above the margin allows: a board without an SDR has more), and its sockets as a budget. */
void ipgw_mem_claims(void)
{
    const rdof_ipgw_config_t *cfg = prepare();
    if (!cfg)
        return;
    mem_add_task("ipgw", "ipgw", 6144, RDOF_MEM_INTERNAL);
    mem_elastic(mem_add("ipgw", "queue", QUEUE_LEN * sizeof(heard_t), 4, RDOF_MEM_PSRAM, RDOF_MEM_ALLOC),
                QUEUE_MAX * sizeof(heard_t), sizeof(heard_t));
    mem_add("ipgw", "sockets", (size_t)(cfg->max_flows + 1) * FLOW_INTERNAL, 4, RDOF_MEM_INTERNAL, RDOF_MEM_BUDGET);
}

void ipgw_start(void)
{
    for (int i = 0; i < RDOF_IPGW_MAX_FLOWS; i++)
        s_sock[i] = -1;
    for (int i = 0; i < MAX_SOURCES; i++)
        s_sources[i].source = -1;
    rdof_ipgw_config_t *cfg = prepare();
    if (!cfg)
        return;
    s_cfg = NULL;
    rdof_ipgw_t *g = heap_caps_calloc(1, sizeof *g, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g)
        g = calloc(1, sizeof *g);
    static StaticQueue_t qs;
    size_t qbytes = 0;
    uint8_t *qmem = mem_take("ipgw", "queue", &qbytes);
    const int depth = qmem ? (int)(qbytes / sizeof(heard_t)) : QUEUE_LEN;
    s_queue = qmem ? xQueueCreateStatic(depth, sizeof(heard_t), qmem, &qs) : xQueueCreate(QUEUE_LEN, sizeof(heard_t));
    if (!g || !s_queue) {
        ESP_LOGE(TAG, "no memory");
        free(g);
        free(cfg);
        return;
    }
    rdof_ipgw_init(g, cfg);
    g->open = open_flow;
    g->close = close_flow;
    g->send_udp = send_udp;
    g->send_dmr = send_dmr;
    g->local_port = local_port;
    if (cfg->nat) {
        s_icmp = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
        if (s_icmp >= 0) {
            fcntl(s_icmp, F_SETFL, fcntl(s_icmp, F_GETFL, 0) | O_NONBLOCK);
            g->send_icmp = send_icmp;
        } else {
            ESP_LOGW(TAG, "no ICMP socket: NAT is UDP only");
        }
    }
    char d[320];
    rdof_ipgw_describe(cfg, d, sizeof d);
    free(cfg);
    ESP_LOGI(TAG, "%s", d);
#if CONFIG_RDOF_ROAMER
    s_tx = heap_caps_calloc(1, sizeof *s_tx, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_tx)
        s_tx = calloc(1, sizeof *s_tx);
#endif
    s_gw = g;
    ESP_LOGI(TAG, "heard queue: %d bursts%s", depth, qmem ? " (planned)" : "");
    mem_task("ipgw", "ipgw", ipgw_task, 6144, NULL, 6, 0, NULL);
}

void ipgw_status_json(rdof_json_t *j)
{
    if (!s_gw)
        return;
    s_gw->n.queue_full = s_queue_full;
    rdof_ipgw_status_json(s_gw, j);
}

#endif /* CONFIG_RDOF_IPGW */
