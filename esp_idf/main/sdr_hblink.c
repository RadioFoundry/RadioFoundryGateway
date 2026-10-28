// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * HBlink4: one receive-only peer per channel (hblink: true and a peer_id), each
 * with its own UDP socket, a DMRC every 10 s as the keepalive (DMRP back), and
 * the channel's DMRD from the tracker.
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
    int sock;
    rdof_hb_peer_t peer; /* the keepalive and the master's state (common/src/hbpeer.c) */
    rdof_hb_sink_t sink;
    SemaphoreHandle_t lock;
} peer_t;

static peer_t *peers; /* npeers of them, from the heap */
static int npeers;
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
    for (int c = 0; c < g_scfg.nch; c++) {
        const rdof_sdr_cfg_channel_t *ch = &g_scfg.ch[c];
        if (!ch->hblink || ch->peer_id < 0)
            continue;
        peer_t *p = &peers[npeers];
        p->channel = c;
        rdof_hb_peer_init(&p->peer, (uint32_t)ch->peer_id, RDOF_HB_KEEPALIVE_SECONDS);
        p->sock = -1;
        p->lock = xSemaphoreCreateMutex();
        rdof_hb_sink_init(&p->sink, p->peer.peer_id);
        p->sink.send = peer_send;
        p->sink.rand32 = peer_rand;
        p->sink.ctx = p;
        chan_peer[c] = npeers++;
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
