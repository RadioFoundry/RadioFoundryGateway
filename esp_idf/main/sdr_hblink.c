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

static const char *TAG = "hblink";

typedef struct {
    int channel;
    uint32_t peer_id;
    int sock;
    bool up;
    int64_t last_pong;
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
        up[n] = peers[i].up;
    }
    return n;
}

static size_t dmrc(const peer_t *p, uint8_t *out)
{
    const rdof_sdr_cfg_channel_t *ch = &g_scfg.ch[p->channel];
    char callsign[9];
    snprintf(callsign, sizeof callsign, "%.8s", ch->id); /* the channel id, 8 characters */
    uint32_t f = (uint32_t)ch->freq;
    return rdof_dmrc_packet(p->peer_id, callsign, f, f, ch->cc >= 0 ? ch->cc : 1, '3', 0, "radiofoundry-gateway",
                          "RadioFoundry_Gateway_SDR", g_scfg.has_location ? &g_scfg.location : NULL, NULL, NULL, out);
}

static int open_socket(void)
{
    return net_udp_connect(g_scfg.hb_host, g_scfg.hb_port);
}

static void peer_task(void *arg)
{
    const int64_t keepalive_us = (int64_t)(RDOF_HB_KEEPALIVE_SECONDS * 1e6);
    static uint8_t buf[1500];
    int64_t next_ka = 0;
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
            }
        }
        int64_t now = esp_timer_get_time();
        if (now >= next_ka) {
            next_ka = now + keepalive_us;
            int up = 0;
            for (int i = 0; i < npeers; i++) {
                peer_t *p = &peers[i];
                uint8_t pkt[RDOF_DMRC_MAX];
                peer_send(p, pkt, dmrc(p, pkt));
                if (p->up && now - p->last_pong > 3 * keepalive_us) {
                    ESP_LOGW(TAG, "peer %lu: no DMRP from master", (unsigned long)p->peer_id);
                    p->up = false;
                }
                up += p->up;
            }
            g_sstat.hb_up = up;
        }
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
                p->up = false;
                continue;
            }
            char kind = rdof_hb_kind(buf, (size_t)n);
            if (kind == 'P') {
                if (!p->up)
                    ESP_LOGI(TAG, "peer %lu (%s) registered with %s:%d", (unsigned long)p->peer_id,
                             g_scfg.ch[p->channel].id, g_scfg.hb_host, g_scfg.hb_port);
                p->up = true;
                p->last_pong = esp_timer_get_time();
            } else if (kind == 'N') {
                ESP_LOGW(TAG, "peer %lu refused by the master: %.6s", (unsigned long)p->peer_id, (const char *)buf);
                p->up = false;
            }
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
        p->peer_id = (uint32_t)ch->peer_id;
        p->sock = -1;
        p->lock = xSemaphoreCreateMutex();
        rdof_hb_sink_init(&p->sink, p->peer_id);
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
