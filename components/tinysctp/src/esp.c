// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* tinysctp's task on ESP-IDF. See tinysctp_esp.h. */
#include "tinysctp_esp.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "tinysctp_udp.h"

static const char *TAG = "tinysctp";

struct tsctp_esp {
    tsctp_t *s;
    tsctp_udp_t u;
    SemaphoreHandle_t lock, done;
    volatile bool stop;
};

#define LOCK(h) xSemaphoreTakeRecursive((h)->lock, portMAX_DELAY)
#define UNLOCK(h) xSemaphoreGiveRecursive((h)->lock)

void tsctp_esp_config_defaults(tsctp_esp_config_t *c)
{
    memset(c, 0, sizeof *c);
    tsctp_config_defaults(&c->sctp);
    c->encap_port = 9899;
    c->task_name = "tinysctp";
    c->task_stack = CONFIG_TINYSCTP_TASK_STACK;
    c->task_priority = CONFIG_TINYSCTP_TASK_PRIORITY;
}

static void run(void *arg)
{
    tsctp_esp_t *h = arg;
    while (!h->stop) {
        LOCK(h);
        uint64_t now = tsctp_udp_now_ms(), next = tsctp_poll(h->s, now);
        UNLOCK(h);
        uint32_t wait = next <= now ? 0 : next - now < 100 ? (uint32_t)(next - now) : 100;
        bool ready = tsctp_udp_wait(&h->u, wait);
        LOCK(h);
        if (ready)
            tsctp_udp_drain(&h->u, h->s);
        tsctp_poll(h->s, tsctp_udp_now_ms());
        UNLOCK(h);
    }
    LOCK(h);
    tsctp_abort(h->s, tsctp_udp_now_ms());
    UNLOCK(h);
    xSemaphoreGive(h->done);
    vTaskDelete(NULL);
}

tsctp_esp_t *tsctp_esp_start(const tsctp_esp_config_t *cfg)
{
    tsctp_esp_t *h = calloc(1, sizeof *h);
    if (!h)
        return NULL;
    if (tsctp_udp_open(&h->u, cfg->host, cfg->encap_port, cfg->local_udp_port) != 0) {
        ESP_LOGW(TAG, "can't open UDP to %s:%u", cfg->host ? cfg->host : "(listen)", cfg->encap_port);
        free(h);
        return NULL;
    }
    tsctp_config_t sc = cfg->sctp;
    sc.output = tsctp_udp_output;
    sc.output_ctx = &h->u;
    sc.random = tsctp_udp_random;
    h->lock = xSemaphoreCreateRecursiveMutex();
    h->done = xSemaphoreCreateBinary();
    h->s = tsctp_new(&sc, tsctp_udp_now_ms());
    if (!h->lock || !h->done || !h->s)
        goto fail;
    if (cfg->host)
        tsctp_connect(h->s, tsctp_udp_now_ms());
    else
        tsctp_listen(h->s);
    if (xTaskCreate(run, cfg->task_name ? cfg->task_name : "tinysctp", cfg->task_stack, h, cfg->task_priority,
                    NULL) != pdPASS)
        goto fail;
    return h;
fail:
    tsctp_free(h->s);
    tsctp_udp_close(&h->u);
    if (h->lock)
        vSemaphoreDelete(h->lock);
    if (h->done)
        vSemaphoreDelete(h->done);
    free(h);
    return NULL;
}

void tsctp_esp_stop(tsctp_esp_t *h)
{
    if (!h)
        return;
    h->stop = true;
    xSemaphoreTake(h->done, portMAX_DELAY);
    tsctp_free(h->s);
    tsctp_udp_close(&h->u);
    vSemaphoreDelete(h->lock);
    vSemaphoreDelete(h->done);
    free(h);
}

int tsctp_esp_send(tsctp_esp_t *h, const void *data, size_t len, const tsctp_sendinfo_t *info)
{
    LOCK(h);
    int r = tsctp_send(h->s, data, len, info, tsctp_udp_now_ms());
    UNLOCK(h);
    return r;
}

int tsctp_esp_shutdown(tsctp_esp_t *h)
{
    LOCK(h);
    int r = tsctp_shutdown(h->s, tsctp_udp_now_ms());
    UNLOCK(h);
    return r;
}

tsctp_state_t tsctp_esp_state(tsctp_esp_t *h)
{
    LOCK(h);
    tsctp_state_t st = tsctp_state(h->s);
    UNLOCK(h);
    return st;
}

void tsctp_esp_stats(tsctp_esp_t *h, tsctp_stats_t *out)
{
    LOCK(h);
    tsctp_get_stats(h->s, out);
    UNLOCK(h);
}

size_t tsctp_esp_status_json(tsctp_esp_t *h, char *buf, size_t cap)
{
    LOCK(h);
    size_t n = tsctp_status_json(h->s, buf, cap);
    UNLOCK(h);
    return n;
}
