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
#include "rdof/hbpeer.h"
#if CONFIG_RDOF_HBLINK_SCTP
#include "tinysctp_esp.h"
#endif

static const char *TAG = "hblink";

struct roam_hb {
    int sock;
    SemaphoreHandle_t lock; /* sock and peer */
    rdof_hb_peer_t peer;    /* the keepalive and the master's state (common/src/hbpeer.c) */
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
    if (len >= 13 && !memcmp(data, "DMRK", 4) && site_tuned(data + 8))
        return; /* a transceiver's answer to a tune the router made: the master sent no DMRT */
    if (!r->hb && r->hb_via)
        r = r->hb_via; /* one identity: through the owner's peer */
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

static double now_s(void) { return esp_timer_get_time() / 1e6; }

static size_t config_packet(roam_t *r, uint8_t *out)
{
    uint32_t freq = 0;
    int cc = 0;
    if (r->cfg->mode != RDOF_CFG_REPEATER)
        radio_channel_now(r, &freq, &cc);
    return rdof_hb_role_dmrc(r->cfg, freq, cc, out);
}

void roam_hb_send_config(roam_t *r, bool force)
{
    uint8_t pkt[RDOF_DMRC_MAX];
    struct roam_hb *h = r->hb;
    if (!h)
        return;
    size_t n = config_packet(r, pkt);
    xSemaphoreTake(h->lock, portMAX_DELAY);
    const bool send_now = rdof_hb_peer_set_config(&h->peer, pkt, n, force, now_s());
    if (send_now && !USE_SCTP(r) && h->sock >= 0 && send(h->sock, pkt, n, 0) == (ssize_t)n)
        r->st.hblink_tx++;
    xSemaphoreGive(h->lock);
    if (USE_SCTP(r) && send_now)
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

/* A packet from the master. */
static void received(roam_t *r, const uint8_t *buf, size_t n)
{
    struct roam_hb *h = r->hb;
    r->st.hblink_rx++;
    xSemaphoreTake(h->lock, portMAX_DELAY);
    const rdof_hbp_event_t ev = rdof_hb_peer_received(&h->peer, buf, n, now_s());
    xSemaphoreGive(h->lock);
    if (ev == RDOF_HBP_REGISTERED) {
        ESP_LOGI(TAG, "peer %lu (%s) registered with %s:%d", (unsigned long)r->cfg->peer_id, r->cfg->name,
                 r->cfg->master_host, r->cfg->master_port);
        r->st.hblink_up = true;
        roam_publish_status(r);
    } else if (ev == RDOF_HBP_REFUSED) {
        ESP_LOGW(TAG, "peer %lu refused by the master: %.6s", (unsigned long)r->cfg->peer_id, (const char *)buf);
        r->st.hblink_up = false;
    } else if (ev == RDOF_HBP_PACKET) {
        if (site_dmrd(r->cfg->peer_id, buf, n))
            return; /* version 3: to the paths the call policies pick, any role's */
        rdof_repeater_t *rpt = NULL;
        rdof_txvr_t *txvr = NULL;
#if RDOF_FEATURE_REPEATER
        rpt = r->cfg->mode == RDOF_CFG_REPEATER ? r->rpt : NULL;
#endif
#if RDOF_FEATURE_TX
        txvr = r->cfg->mode == RDOF_CFG_ROAMER ? r->txvr : NULL;
#endif
        uint8_t k[RDOF_DMRK_LEN];
        if (rdof_hb_dispatch(buf, n, rpt, txvr, r->cfg->peer_id, k))
            roam_hb_send(r, k, sizeof k);
    }
}

/* DMRC as the keepalive; the master gone quiet after three. */
static void keepalive(roam_t *r)
{
    struct roam_hb *h = r->hb;
    rdof_hbp_event_t ev;
    xSemaphoreTake(h->lock, portMAX_DELAY);
    const bool due = rdof_hb_peer_tick(&h->peer, now_s(), &ev);
    xSemaphoreGive(h->lock);
    if (!due)
        return;
    roam_hb_send_config(r, true);
    if (ev == RDOF_HBP_QUIET) {
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

static void sctp_task(roam_t *r)
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
    bool was_up = false;
    for (;;) {
        bool up = h->assoc_up;
        if (up && !was_up) { /* DMRC as soon as the association is up */
            xSemaphoreTake(h->lock, portMAX_DELAY);
            rdof_hb_peer_link_up(&h->peer);
            xSemaphoreGive(h->lock);
        }
        was_up = up;
        if (up)
            keepalive(r);
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
    int backoff_ms = 1000;
    uint8_t *buf = r->i ? heap_caps_malloc(1500, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (!buf)
        buf = malloc(1500);
#if CONFIG_RDOF_HBLINK_SCTP
    if (h->use_sctp)
        sctp_task(r);
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
        rdof_hb_peer_link_up(&h->peer);
        xSemaphoreGive(h->lock);
        backoff_ms = 1000;
        while (g_board.wifi_up) {
            keepalive(r);
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
        rdof_hb_peer_link_down(&h->peer);
        xSemaphoreGive(h->lock);
        close(s);
        r->st.hblink_up = false;
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
    }
}

void roam_hb_start(roam_t *r)
{
    roam_t *o = site_hb_owner(r);
    if (o != r) { /* one identity: the box's peer is another role's */
        r->hb_via = o;
        ESP_LOGI(TAG, "%s: HBlink4 through %s's peer", r->cfg->name, o->cfg->name);
        return;
    }
    struct roam_hb *h = calloc(1, sizeof *h);
    h->sock = -1;
    h->lock = xSemaphoreCreateMutex();
    rdof_hb_peer_init(&h->peer, r->cfg->peer_id,
                      r->cfg->mode == RDOF_CFG_REPEATER ? RDOF_HB_KEEPALIVE_SECONDS : RDOF_HB_ROAMING_KEEPALIVE_SECONDS);
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
