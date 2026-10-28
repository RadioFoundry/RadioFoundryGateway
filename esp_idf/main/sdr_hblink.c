// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * HBlink4: one receive-only peer per channel (hblink: true and a peer_id), each
 * with its own UDP socket, a DMRC every 10 s as the keepalive (DMRP back), and
 * the channel's DMRD from the tracker.
 *
 * Version 3 with one identity (site_run.c): every channel's DMRD goes through the box's peer, a roamer's or
 * repeater's when one runs, else one peer here (the box's id, the first channel's DMRC). Each stream goes up only
 * when its path's policy sends it to HBlink (site_rf); what the master sends a peer here goes to the call router.
 */
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "mem.h"

#if CONFIG_RDOF_SDR_HBLINK

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "rdof/hblink.h"
#include "rdof/hbpeer.h"

static const char *TAG = "hblink";

typedef struct {
    int channel;
    bool via;  /* one identity: no socket of its own, sends through the box's peer */
    int sock;
    rdof_hb_peer_t peer; /* the keepalive and the master's state (common/src/hbpeer.c) */
    rdof_hb_sink_t sink;
    SemaphoreHandle_t lock;
} peer_t;

static peer_t *peers; /* npeers of them, from the heap */
static int npeers;
static peer_t *box_peer; /* one identity, no roamer or repeater: the box's peer, here */
static int chan_peer[RDOF_SDR_MAX_CHANNELS] = {-1};

static void peer_send(void *ctx, const uint8_t *data, size_t len)
{
    peer_t *p = ctx;
    xSemaphoreTake(p->lock, portMAX_DELAY);
    if (p->sock >= 0)
        send(p->sock, data, len, 0);
    xSemaphoreGive(p->lock);
}

static uint32_t peer_rand(void *ctx) { return esp_random(); }

/* one identity: through the roamer's or repeater's peer, else the box's here */
static void via_send(void *ctx, const uint8_t *data, size_t len)
{
#if CONFIG_RDOF_ROAMER
    roam_t *o = site_hb_peer();
    if (o) {
        roam_hb_send(o, data, len);
        return;
    }
#endif
    if (box_peer)
        peer_send(box_peer, data, len);
}

static bool chan_admit(void *ctx, int slot, uint32_t src, uint32_t dst, bool group)
{
    return site_rf((uint32_t)g_scfg.ch[((peer_t *)ctx)->channel].freq, slot, src, dst, group);
}

static void on_frame(void *ctx, const char *ch, const rdof_frame_t *f) { rdof_hb_sink_on_frame(&((peer_t *)ctx)->sink, f); }

bool sdr_hb_sink(int c, rdof_sink_t *out)
{
    if (c < 0 || c >= g_scfg.nch || chan_peer[c] < 0)
        return false;
    rdof_sink_t s = {.on_frame = on_frame, .ctx = &peers[chan_peer[c]]};
    *out = s;
    return true;
}

int sdr_hb_status(const char **ids, bool *up, int max)
{
    int n = 0;
    for (int i = 0; i < npeers && n < max; i++, n++) {
        ids[n] = g_scfg.ch[peers[i].channel].id;
        up[n] = peers[i].peer.up;
    }
    return n;
}

static size_t dmrc(const peer_t *p, uint8_t *out)
{
    const rdof_sdr_cfg_channel_t *ch = &g_scfg.ch[p->channel];
    return rdof_hb_channel_dmrc(p->peer.peer_id, ch->id, (uint32_t)ch->freq, ch->cc,
                                g_scfg.has_location ? &g_scfg.location : NULL, out);
}

static int open_socket(void)
{
    return net_udp_connect(g_scfg.hb_host, g_scfg.hb_port);
}

static void peer_task(void *arg)
{
    static uint8_t buf[1500];
    for (;;) {
        if (!g_board.wifi_up) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        for (int i = 0; i < npeers; i++) {
            peer_t *p = &peers[i];
            if (p->via)
                continue;
            if (p->sock < 0) {
                int s = open_socket();
                xSemaphoreTake(p->lock, portMAX_DELAY);
                p->sock = s;
                xSemaphoreGive(p->lock);
                if (s >= 0)
                    rdof_hb_peer_link_up(&p->peer);
            }
        }
        const double now = esp_timer_get_time() / 1e6;
        int up = 0;
        for (int i = 0; i < npeers; i++) {
            peer_t *p = &peers[i];
            rdof_hbp_event_t ev;
            if (p->via)
                continue;
            if (p->sock >= 0 && rdof_hb_peer_tick(&p->peer, now, &ev)) {
                uint8_t pkt[RDOF_DMRC_MAX];
                const size_t n = dmrc(p, pkt);
                rdof_hb_peer_set_config(&p->peer, pkt, n, true, now);
                peer_send(p, pkt, n);
                if (ev == RDOF_HBP_QUIET)
                    ESP_LOGW(TAG, "peer %lu: no DMRP from master", (unsigned long)p->peer.peer_id);
            }
            up += p->peer.up;
        }
        g_sstat.hb_up = up;
        fd_set fds;
        FD_ZERO(&fds);
        int maxfd = -1;
        for (int i = 0; i < npeers; i++)
            if (peers[i].sock >= 0) {
                FD_SET(peers[i].sock, &fds);
                maxfd = peers[i].sock > maxfd ? peers[i].sock : maxfd;
            }
        struct timeval tv = {.tv_sec = 0, .tv_usec = 500000};
        if (maxfd < 0 || select(maxfd + 1, &fds, NULL, NULL, &tv) <= 0)
            continue;
        for (int i = 0; i < npeers; i++) {
            peer_t *p = &peers[i];
            if (p->sock < 0 || !FD_ISSET(p->sock, &fds))
                continue;
            ssize_t n = recv(p->sock, buf, sizeof buf, 0);
            if (n <= 0) {
                xSemaphoreTake(p->lock, portMAX_DELAY);
                close(p->sock);
                p->sock = -1;
                xSemaphoreGive(p->lock);
                rdof_hb_peer_link_down(&p->peer);
                continue;
            }
            const rdof_hbp_event_t ev = rdof_hb_peer_received(&p->peer, buf, (size_t)n, esp_timer_get_time() / 1e6);
            if (ev == RDOF_HBP_REGISTERED)
                ESP_LOGI(TAG, "peer %lu (%s) registered with %s:%d", (unsigned long)p->peer.peer_id,
                         g_scfg.ch[p->channel].id, g_scfg.hb_host, g_scfg.hb_port);
            else if (ev == RDOF_HBP_REFUSED)
                ESP_LOGW(TAG, "peer %lu refused by the master: %.6s", (unsigned long)p->peer.peer_id,
                         (const char *)buf);
            else if (ev == RDOF_HBP_PACKET)
                site_dmrd(p->peer.peer_id, buf, (size_t)n); /* version 3: the call router's, else nothing sends */
        }
    }
}

void sdr_hb_start(void)
{
    for (int c = 0; c < RDOF_SDR_MAX_CHANNELS; c++)
        chan_peer[c] = -1;
    if (!g_scfg.hb_enabled)
        return;
    peers = calloc(RDOF_SDR_MAX_CHANNELS, sizeof *peers);
    if (!peers)
        return;
    const bool shared = site_shared_identity();
    for (int c = 0; c < g_scfg.nch; c++) {
        const rdof_sdr_cfg_channel_t *ch = &g_scfg.ch[c];
        const bool via = shared && !ch->hblink;
        if (!via && (!ch->hblink || ch->peer_id < 0))
            continue;
        peer_t *p = &peers[npeers];
        p->channel = c;
        p->via = via;
        const uint32_t id = via ? site_id() : (uint32_t)ch->peer_id;
        rdof_hb_peer_init(&p->peer, id, RDOF_HB_KEEPALIVE_SECONDS);
        p->sock = -1;
        p->lock = xSemaphoreCreateMutex();
        rdof_hb_sink_init(&p->sink, id);
        p->sink.send = via ? via_send : peer_send;
        p->sink.rand32 = peer_rand;
        p->sink.admit = chan_admit;
        p->sink.ctx = p;
        chan_peer[c] = npeers++;
    }
    /* one identity and no roamer or repeater: the box's peer is the first channel's, with a socket */
    if (shared && !g_roles.has_radio)
        for (int i = 0; i < npeers && !box_peer; i++)
            if (peers[i].via) {
                peers[i].via = false;
                box_peer = &peers[i];
            }
    g_sstat.hb_peers = npeers;
    if (npeers) {
        ESP_LOGI(TAG, "%d peers for %s:%d", npeers, g_scfg.hb_host, g_scfg.hb_port);
        mem_task("sdr_net", "sdr_hblink", peer_task, 4096, NULL, 8, 0, NULL);
    }
}

#else

bool sdr_hb_sink(int c, rdof_sink_t *out) { return false; }
int sdr_hb_status(const char **ids, bool *up, int max) { return 0; }
void sdr_hb_start(void) {}

#endif
