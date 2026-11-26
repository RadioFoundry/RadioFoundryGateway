// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#include "routed.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#if CONFIG_RDOF_IPGW

#include "app.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "lwip/raw.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"
#include "rdof_lwip_hooks.h"

static const char *TAG = "routed";

#define QUEUE_LEN 8

typedef struct {
    uint16_t len;
    uint8_t d[];
} lan_t;

static QueueHandle_t s_q;
static volatile uint32_t s_net, s_mask; /* 0: no owner, the hook takes nothing */
static volatile uint32_t s_dropped;
static const char *s_owner;
static rdof_ipgw_net_t s_radio_net;
static bool s_rip;
static int s_rip_sock = -1;
static double s_next_rip;
static struct raw_pcb *s_raw; /* lwIP's thread's own */

int rdof_ip4_input_hook(struct pbuf *p, struct netif *inp)
{
    const uint32_t mask = s_mask;
    if (!mask || p->len < 20)
        return 0;
    const uint8_t *h = p->payload;
    const uint32_t dst = (uint32_t)h[16] << 24 | (uint32_t)h[17] << 16 | (uint32_t)h[18] << 8 | h[19];
    if ((dst & mask) != s_net)
        return 0;
    lan_t *m = p->tot_len <= RDOF_IPGW_MAX_PACKET
                   ? heap_caps_malloc(sizeof *m + p->tot_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                   : NULL; /* past the longest a radio takes: the gateway would drop it anyway */
    if (m) {
        m->len = (uint16_t)pbuf_copy_partial(p, m->d, p->tot_len, 0);
        if (xQueueSend(s_q, &m, 0) != pdTRUE) {
            free(m);
            s_dropped++;
        }
    } else {
        s_dropped++;
    }
    pbuf_free(p);
    return 1;
}

static void raw_out(void *arg) /* on lwIP's thread */
{
    struct pbuf *p = arg;
    if (!s_raw && (s_raw = raw_new(255)) != NULL) /* 255: reserved, so it takes nothing in */
        raw_setflags(s_raw, RAW_FLAGS_HDRINCL);
    if (s_raw) {
        const uint8_t *h = p->payload;
        ip_addr_t dst;
        IP_ADDR4(&dst, h[16], h[17], h[18], h[19]);
        raw_sendto(s_raw, p, &dst);
    }
    pbuf_free(p);
}

bool routed_send(const uint8_t *data, size_t len)
{
    if (!s_mask || len < 20 || len > 0xFFFF)
        return false;
    struct pbuf *p = pbuf_alloc(PBUF_LINK, (u16_t)len, PBUF_RAM); /* room for the link header in front */
    if (!p)
        return false;
    memcpy(p->payload, data, len);
    if (tcpip_callback(raw_out, p) != ERR_OK) {
        pbuf_free(p);
        return false;
    }
    return true;
}

bool routed_lan(rdof_ipgw_net_t *out, uint32_t *ip_out)
{
    esp_netif_ip_info_t ip;
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!n || esp_netif_get_ip_info(n, &ip) != ESP_OK || !ip.ip.addr)
        return false;
    out->mask = ntohl(ip.netmask.addr);
    out->net = ntohl(ip.ip.addr) & out->mask;
    *ip_out = ntohl(ip.ip.addr);
    return true;
}

static void lock(bool on)
{
    static SemaphoreHandle_t m;
    static StaticSemaphore_t ms;
    if (!m)
        m = xSemaphoreCreateMutexStatic(&ms); /* first from app_main, before any owner */
    if (on)
        xSemaphoreTake(m, portMAX_DELAY);
    else
        xSemaphoreGive(m);
}

bool routed_open(const rdof_ipgw_net_t *net, const char *who, bool rip, char *why, size_t cap)
{
    lock(true);
    bool ok = false;
    if (s_owner) {
        snprintf(why, cap, "the %s routes radio_net", s_owner);
    } else if (!s_q && !(s_q = xQueueCreate(QUEUE_LEN, sizeof(lan_t *)))) {
        snprintf(why, cap, "no memory");
    } else {
        s_owner = who;
        s_radio_net = *net;
        s_rip = rip;
        s_next_rip = 0;
        if (rip && s_rip_sock < 0) {
            int s = socket(AF_INET, SOCK_DGRAM, 0);
            struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(RDOF_IPGW_RIP_PORT),
                                    .sin_addr.s_addr = htonl(INADDR_ANY)};
            int ttl = 1;
            if (s >= 0 && bind(s, (struct sockaddr *)&a, sizeof a) == 0 &&
                setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl) == 0) {
                s_rip_sock = s;
            } else {
                if (s >= 0)
                    close(s);
                ESP_LOGW(TAG, "no socket on port %d: no RIP", RDOF_IPGW_RIP_PORT);
            }
        }
        s_net = net->net;
        s_mask = net->mask; /* the hook takes from now on */
        ok = true;
        ESP_LOGI(TAG, "radio_net routed for the %s%s", who, rip ? ", announced by RIP" : "");
    }
    lock(false);
    return ok;
}

void routed_close(void)
{
    lock(true);
    s_mask = 0;
    s_owner = NULL;
    lan_t *m;
    while (s_q && xQueueReceive(s_q, &m, 0) == pdTRUE)
        free(m);
    if (s_rip_sock >= 0)
        close(s_rip_sock);
    s_rip_sock = -1;
    lock(false);
}

size_t routed_recv(uint8_t *buf, size_t cap)
{
    const double now = esp_timer_get_time() / 1e6;
    if (s_rip_sock >= 0 && now >= s_next_rip && g_board.wifi_up) {
        uint8_t m[24];
        size_t n = rdof_ipgw_rip(&(rdof_ipgw_config_t){.radio_net = s_radio_net}, m, sizeof m);
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(RDOF_IPGW_RIP_PORT),
                                .sin_addr.s_addr = htonl(RDOF_IPGW_RIP_GROUP)};
        if (n)
            sendto(s_rip_sock, m, n, 0, (struct sockaddr *)&a, sizeof a);
        s_next_rip = now + RDOF_IPGW_RIP_S;
    }
    lan_t *m;
    if (!s_q || xQueueReceive(s_q, &m, 0) != pdTRUE)
        return 0;
    size_t n = m->len <= cap ? m->len : 0;
    if (n)
        memcpy(buf, m->d, n);
    else
        s_dropped++;
    free(m);
    return n;
}

uint32_t routed_dropped(void) { return s_dropped; }

#endif /* CONFIG_RDOF_IPGW */
