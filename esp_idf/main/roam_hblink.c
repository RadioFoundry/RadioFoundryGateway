// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The HBlink4 peer, a roamer's or a repeater's: no login handshake; a DMRC
 * config packet every 5 s (roamer) or 10 s (repeater) is the keepalive, and the master answers
 * DMRP. A roamer's DMRC says where the radio is now and its idle channel;
 * DMRT tunes it for a call, and the call's DMRD follow. A repeater's DMRC has
 * its RX / TX frequencies and slots; DMRD go out on their slot.
 *
 * Over UDP, or with hblink transport "sctp" (CONFIG_RDOF_HBLINK_SCTP) over SCTP
 * in UDP to HBlink4's sctp_encap_port (components/tinysctp), as the
 * MMDVMHost fork's SCTP: one stream, sctp_heartbeat_ms, sctp_max_retransmit,
 * and DMRD given up after sctp_ttl_ms (PR-SCTP) when it's set. `lock` is
 * never held while calling tinysctp, whose task delivers what arrives.
 */
#include <string.h>

#include "app.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#if CONFIG_RDOF_HBLINK_SCTP
#include "tinysctp_esp.h"
#endif

static const char *TAG = "hblink";

struct roam_hb {
    int sock;
    SemaphoreHandle_t lock;
    uint8_t last_config[RDOF_DMRC_MAX];
    size_t last_config_len;
    int64_t last_pong;
#if CONFIG_RDOF_HBLINK_SCTP
    bool use_sctp;
    tsctp_esp_t *assoc; /* started once, never stopped: it reconnects by itself */
    volatile bool assoc_up;
#endif
};

#if CONFIG_RDOF_HBLINK_SCTP
#define USE_SCTP(r) ((r)->hb->use_sctp)

static void sctp_send(roam_t *r, const uint8_t *data, size_t len)
{
    tsctp_sendinfo_t si = {0};
    if (len >= 4 && !memcmp(data, "DMRD", 4))
        si.lifetime_ms = (uint32_t)r->cfg->sctp_ttl_ms;
    if (r->hb->assoc && tsctp_esp_send(r->hb->assoc, data, len, &si) == TSCTP_OK)
        r->st.hblink_tx++;
}
#else
#define USE_SCTP(r) false
#define sctp_send(r, data, len) ((void)0)
#endif

void roam_hb_send(roam_t *r, const uint8_t *data, size_t len)
{
    if (!r->hb)
        return;
    if (USE_SCTP(r)) {
        sctp_send(r, data, len);
        return;
    }
    struct roam_hb *h = r->hb;
    xSemaphoreTake(h->lock, portMAX_DELAY);
    if (h->sock >= 0 && send(h->sock, data, len, 0) == (ssize_t)len)
        r->st.hblink_tx++;
    xSemaphoreGive(h->lock);
}

static size_t config_packet(roam_t *r, uint8_t *out)
{
    const rdof_config_t *c = r->cfg;
    const rdof_location_t *loc = c->has_location ? &c->location : NULL;
    if (c->mode == RDOF_CFG_REPEATER) {
        char slots = c->slot1 && c->slot2 ? '3' : c->slot1 ? '1' : c->slot2 ? '2' : '0';
        return rdof_dmrc_packet(c->peer_id, c->callsign, c->rx_freq, c->tx_freq, c->cc, slots, c->power,
                              c->sfr    ? "RadioFoundry Gateway SFR repeater"
                              : c->dcdm ? "RadioFoundry Gateway DCDM gateway"
                                        : "RadioFoundry Gateway repeater",
                              c->sfr    ? "radiofoundry_gateway sfr repeater"
                              : c->dcdm ? "radiofoundry_gateway dcdm"
                                        : "radiofoundry_gateway duplex repeater",
                              loc, NULL, NULL, out);
    }
    uint32_t freq, idle = c->idle.freq;
    int cc, idle_cc = c->idle.cc;
    radio_channel_now(r, &freq, &cc);
    return rdof_dmrc_packet(c->peer_id, c->callsign, freq, freq, cc, '4', 0, "RadioFoundry Gateway roaming",
                          "radiofoundry_gateway simplex", loc, &idle, &idle_cc, out);
}

void roam_hb_send_config(roam_t *r, bool force)
{
    uint8_t pkt[RDOF_DMRC_MAX];
    struct roam_hb *h = r->hb;
    if (!h)
        return;
    size_t n = config_packet(r, pkt);
    xSemaphoreTake(h->lock, portMAX_DELAY);
    bool changed = n != h->last_config_len || memcmp(pkt, h->last_config, n) != 0;
    if (force || changed) {
        if (!USE_SCTP(r) && h->sock >= 0 && send(h->sock, pkt, n, 0) == (ssize_t)n)
            r->st.hblink_tx++;
        memcpy(h->last_config, pkt, n);
        h->last_config_len = n;
    }
    xSemaphoreGive(h->lock);
    if (USE_SCTP(r) && (force || changed))
        sctp_send(r, pkt, n);
}

static int open_socket(roam_t *r)
{
    int s = net_udp_connect(r->cfg->master_host, r->cfg->master_port);
    if (s < 0) {
        ESP_LOGW(TAG, "can't resolve %s", r->cfg->master_host);
        return -1;
    }
    struct timeval tv = {.tv_sec = 0, .tv_usec = 500000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return s;
}

static void dispatch(roam_t *r, const uint8_t *d, size_t n)
{
    char kind = rdof_hb_kind(d, n);
#if RDOF_FEATURE_TX
    if (r->cfg->mode == RDOF_CFG_ROAMER && r->txvr) {
        if (kind == 'T') {
            rdof_tune_t t;
            if (rdof_parse_dmrt(d, n, &t))
                rdof_txvr_on_tune(r->txvr, &t);
        } else if (kind == 'D') {
            rdof_net_burst_t b;
            if (rdof_parse_dmrd(d, n, &b))
                rdof_txvr_on_burst(r->txvr, &b);
        }
        return;
    }
#endif
#if RDOF_FEATURE_REPEATER
    if (r->cfg->mode == RDOF_CFG_REPEATER && r->rpt && kind == 'D') {
        rdof_net_burst_t b;
        if (rdof_parse_dmrd(d, n, &b))
            rdof_repeater_on_burst(r->rpt, &b);
        return;
    }
#endif
#if !RDOF_FEATURE_TX
    if (kind == 'T') { /* receive only: tunes refused */
        rdof_tune_t t;
        if (rdof_parse_dmrt(d, n, &t)) {
            uint8_t k[RDOF_DMRK_LEN];
            rdof_dmrk_packet(r->cfg->peer_id, t.stream_id, RDOF_DMRK_RADIO_ERROR, t.freq, t.cc, k);
            roam_hb_send(r, k, sizeof k);
        }
    }
#endif
}

/* A packet from the master. */
static void received(roam_t *r, const uint8_t *buf, size_t n)
{
    r->st.hblink_rx++;
    char kind = rdof_hb_kind(buf, n);
    if (kind == 'P') {
        if (!r->st.hblink_up) {
            ESP_LOGI(TAG, "peer %lu (%s) registered with %s:%d", (unsigned long)r->cfg->peer_id, r->cfg->name,
                     r->cfg->master_host, r->cfg->master_port);
            r->st.hblink_up = true;
            roam_publish_status(r);
        }
        r->hb->last_pong = esp_timer_get_time();
    } else if (kind == 'N') {
        ESP_LOGW(TAG, "peer %lu refused by the master: %.6s", (unsigned long)r->cfg->peer_id, (const char *)buf);
        r->st.hblink_up = false;
    } else {
        dispatch(r, buf, n);
    }
}

/* DMRC as the keepalive; the master gone quiet after three. */
static void keepalive(roam_t *r, int64_t now, int64_t *next_ka, int64_t keepalive_us)
{
    if (now < *next_ka)
        return;
    roam_hb_send_config(r, true);
    *next_ka = now + keepalive_us;
    if (r->st.hblink_up && now - r->hb->last_pong > 3 * keepalive_us) {
        ESP_LOGW(TAG, "peer %lu: no DMRP from the master", (unsigned long)r->cfg->peer_id);
        r->st.hblink_up = false;
        roam_publish_status(r);
    }
}

#if CONFIG_RDOF_HBLINK_SCTP
static void sctp_message(void *user, const tsctp_msg_t *m)
{
    received(user, m->data, m->len);
}

static void sctp_event(void *user, tsctp_event_t ev, tsctp_reason_t why)
{
    roam_t *r = user;
    if (ev == TSCTP_EV_DOWN) {
        ESP_LOGW(TAG, "SCTP association with %s down: %s", r->cfg->master_host, tsctp_reason_name(why));
        r->st.hblink_up = false;
    } else {
        ESP_LOGI(TAG, "SCTP association with %s %s", r->cfg->master_host, ev == TSCTP_EV_UP ? "up" : "restarted");
    }
    r->hb->assoc_up = ev != TSCTP_EV_DOWN;
}

static void sctp_task(roam_t *r, int64_t keepalive_us)
{
    struct roam_hb *h = r->hb;
    int backoff_ms = 1000;
    while (!h->assoc) {
        if (g_board.wifi_up && r->cfg->master_host[0]) {
            tsctp_esp_config_t c;
            tsctp_esp_config_defaults(&c);
            c.host = r->cfg->master_host;
            c.encap_port = (uint16_t)r->cfg->sctp_encap_port;
            c.task_name = "hblink-sctp";
            c.task_stack = 6144; /* what arrives is handled in its task: the transceiver, MQTT status */
            c.sctp.peer_port = (uint16_t)r->cfg->master_port;
            c.sctp.streams_out = c.sctp.streams_in = 1; /* as MMDVMHost */
            c.sctp.hb_interval_ms = (uint32_t)r->cfg->sctp_heartbeat_ms;
            c.sctp.max_retrans = (uint8_t)r->cfg->sctp_max_retransmit;
            c.sctp.rto_initial_ms = 1000;
            c.sctp.rto_min_ms = 300;
            c.sctp.rto_max_ms = 3000;
            c.sctp.sack_delay_ms = 50;
            c.sctp.sndbuf = c.sctp.rcvbuf = 16384;
            c.sctp.max_message = 2048;
            c.sctp.reconnect_min_ms = 1000;
            c.sctp.reconnect_max_ms = 10000;
            c.sctp.on_message = sctp_message;
            c.sctp.on_event = sctp_event;
            c.sctp.user = r;
            h->assoc = tsctp_esp_start(&c);
            if (h->assoc)
                break;
            ESP_LOGW(TAG, "peer %lu: can't reach %s (SCTP over UDP %d); retrying in %d s",
                     (unsigned long)r->cfg->peer_id, r->cfg->master_host, r->cfg->sctp_encap_port, backoff_ms / 1000);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms = backoff_ms < 30000 ? backoff_ms * 2 : 30000;
        } else {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    int64_t next_ka = 0;
    bool was_up = false;
    for (;;) {
        bool up = h->assoc_up;
        if (up && !was_up)
            next_ka = 0; /* DMRC as soon as the association is up */
        was_up = up;
        if (up)
            keepalive(r, esp_timer_get_time(), &next_ka, keepalive_us);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

size_t roam_hb_sctp_status(roam_t *r, char *buf, size_t cap)
{
    return r->hb && r->hb->use_sctp && r->hb->assoc ? tsctp_esp_status_json(r->hb->assoc, buf, cap) : 0;
}
#endif

static void peer_task(void *arg)
{
    roam_t *r = arg;
    struct roam_hb *h = r->hb;
    const int64_t keepalive_us = (int64_t)((r->cfg->mode == RDOF_CFG_REPEATER ? RDOF_HB_KEEPALIVE_SECONDS
                                                                            : RDOF_HB_ROAMING_KEEPALIVE_SECONDS) * 1e6);
    int backoff_ms = 1000;
    uint8_t *buf = r->i ? heap_caps_malloc(1500, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (!buf)
        buf = malloc(1500);
#if CONFIG_RDOF_HBLINK_SCTP
    if (h->use_sctp)
        sctp_task(r, keepalive_us);
#endif
    for (;;) {
        if (!g_board.wifi_up || !r->cfg->master_host[0]) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        int s = open_socket(r);
        if (s < 0) {
            ESP_LOGW(TAG, "peer %lu: can't reach %s:%d; retrying in %d s", (unsigned long)r->cfg->peer_id,
                     r->cfg->master_host, r->cfg->master_port, backoff_ms / 1000);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms = backoff_ms < 30000 ? backoff_ms * 2 : 30000;
            continue;
        }
        xSemaphoreTake(h->lock, portMAX_DELAY);
        h->sock = s;
        xSemaphoreGive(h->lock);
        backoff_ms = 1000;
        int64_t next_ka = 0;
        h->last_pong = 0;
        while (g_board.wifi_up) {
            keepalive(r, esp_timer_get_time(), &next_ka, keepalive_us);
            ssize_t n = recv(s, buf, 1500, 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                ESP_LOGW(TAG, "recv: errno %d", errno);
                break;
            }
            received(r, buf, (size_t)n);
        }
        xSemaphoreTake(h->lock, portMAX_DELAY);
        h->sock = -1;
        xSemaphoreGive(h->lock);
        close(s);
        r->st.hblink_up = false;
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
    }
}

void roam_hb_start(roam_t *r)
{
    struct roam_hb *h = calloc(1, sizeof *h);
    h->sock = -1;
    h->lock = xSemaphoreCreateMutex();
    r->hb = h;
#if CONFIG_RDOF_HBLINK_SCTP
    h->use_sctp = !strcmp(r->cfg->transport, "sctp");
    if (h->use_sctp)
        ESP_LOGI(TAG, "HBlink4 over SCTP: %s:%d in UDP %d", r->cfg->master_host, r->cfg->master_port,
                 r->cfg->sctp_encap_port);
    else
#endif
    if (strcmp(r->cfg->transport, "udp"))
        ESP_LOGW(TAG, "hblink transport %s isn't on this board: UDP", r->cfg->transport);
    roam_task(r, peer_task, "roam_hblink", 4096, 8);
}
