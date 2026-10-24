// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * A split duplex repeater's modem (radio type split; docs/spec/repeater.md):
 * two radios with RadioFoundry's split duplex extension, a TX
 * half and an RX half, as one duplex MMDVM modem. rdof_split_t (common
 * rdof/split.h) is the transport under the repeater's rdof_modem, as
 * uart_modem.c is for an MMDVM_HS HAT, so radio_task.c and repeater.c run
 * unchanged.
 *
 * Each half is a shell roam_t with its own settings (the half's port) that the
 * usual transport (usb_modem.c, uart_modem.c) runs; modem_port.c hands a
 * shell's bytes and its transport going up or down to the hooks here. Bytes
 * from the halves go through a stream buffer each to this file's task, which
 * feeds the split, ticks it every 20 ms and writes what it has for the halves
 * and the repeater. (The USB data callback must not wait on the split's lock:
 * the lock's holder may be waiting for that same USB task to finish a write.)
 *
 * The board's SDR modem as a half (modem sdr, either half: sdr's
 * sdrmodem.c takes both roles) is refused for now: the split's TIME needs the
 * front end's sample clock against esp_timer to well under a slot, and the
 * receive and transmit paths don't stamp their samples yet (REPEATER.md,
 * "Split duplex").
 *
 * slot_timing utc: once a second the gate follows netclock_quality and the
 * split gets netclock's UTC against now_mono, so it ALIGNs a TX half that
 * offers it on the UTC grid (docs/spec/slot-timing.md); the gate's stop
 * unkeys it.
 */
#include <math.h>
#include <string.h>

#include "app.h"

#if CONFIG_RDOF_ROAMER && RDOF_FEATURE_REPEATER

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "netclock.h"
#include "rdof/slotgrid.h"
#include "rdof/split.h"

static const char *TAG = "split";

#define RETRY_S 5.0 /* after the split failed, with both halves still there */

typedef struct {
    roam_t *parent;
    roam_t *half[2];
    rdof_config_t *hcfg[2];
    rdof_split_t *s;
    SemaphoreHandle_t lock;
    StreamBufferHandle_t in[2];
    const modem_transport_t *volatile t[2];
    volatile bool changed; /* a half came or went */
    bool up;               /* the repeater's transport is up */
    double failed_at;
    modem_transport_t ours;
    uint8_t buf[512]; /* flush's, under the lock */
    rdof_slot_gate_t gate; /* slot_timing utc */
    double gate_at;
} split_port_t;

static split_port_t *ports[RDOF_MAX_RADIOS];

static void *big(size_t n)
{
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : calloc(1, n);
}

static split_port_t *shell_of(roam_t *r, int *which)
{
    for (int k = 0; k < RDOF_MAX_RADIOS; k++)
        for (int i = 0; ports[k] && i < 2; i++)
            if (ports[k]->half[i] == r) {
                *which = i;
                return ports[k];
            }
    return NULL;
}

static void log_fn(void *ctx, const char *msg) { ESP_LOGI(TAG, "%s: %s", ((split_port_t *)ctx)->parent->cfg->name, msg); }

/* What the split has for the halves and the repeater. Under the lock. */
static void flush(split_port_t *p)
{
    uint8_t *buf = p->buf;
    const size_t cap = sizeof p->buf;
    size_t n;
    for (int i = 0; i < 2; i++) {
        const modem_transport_t *t = p->t[i];
        while ((n = rdof_split_take(p->s, i, buf, cap)) > 0)
            if (t)
                t->write(t->ctx, buf, n);
    }
    while ((n = rdof_split_take(p->s, -1, buf, cap)) > 0)
        if (p->up)
            modem_on_bytes(p->parent, buf, n);
}

static bool our_write(void *ctx, const uint8_t *data, size_t len)
{
    split_port_t *p = ctx;
    xSemaphoreTake(p->lock, portMAX_DELAY);
    bool ok = p->up;
    if (ok) {
        rdof_split_upstream(p->s, data, len, now_mono());
        flush(p);
    }
    xSemaphoreGive(p->lock);
    return ok;
}

static void params_of(const rdof_config_t *c, rdof_split_params_t *sp)
{
    rdof_split_params_default(sp);
    sp->power_cap = c->split_power_cap;
    sp->prebuffer = c->split_prebuffer;
    sp->lease_s = c->split_lease_s;
    sp->cooldown_s = c->split_cooldown_s;
    sp->max_key_s = c->split_max_key_s;
}

/* Under the lock: the split started again over both halves, and the repeater's transport up. */
static void start(split_port_t *p)
{
    rdof_split_params_t sp;
    params_of(p->parent->cfg, &sp);
    rdof_split_init(p->s, now_mono(), &sp);
    p->gate_at = 0; /* the UTC setting again at the first tick */
    p->s->log = log_fn;
    p->s->log_ctx = p;
    p->up = true;
    flush(p);
    modem_transport_up(p->parent, &p->ours);
}

static void stop(split_port_t *p, const char *why)
{
    if (!p->up)
        return;
    p->up = false;
    p->failed_at = now_mono();
    ESP_LOGW(TAG, "%s: %s", p->parent->cfg->name, why);
    modem_transport_down(p->parent);
    snprintf(p->parent->st.radio_error, sizeof p->parent->st.radio_error, "split: %s", why);
}

static void split_task(void *arg)
{
    split_port_t *p = arg;
    uint8_t buf[256];
    for (;;) {
        size_t n[2];
        n[0] = xStreamBufferReceive(p->in[0], buf, sizeof buf / 2, pdMS_TO_TICKS(10));
        n[1] = xStreamBufferReceive(p->in[1], buf + sizeof buf / 2, sizeof buf / 2, 0);
        xSemaphoreTake(p->lock, portMAX_DELAY);
        const double now = now_mono();
        if (p->changed) {
            p->changed = false;
            if (p->up && (!p->t[0] || !p->t[1]))
                stop(p, !p->t[0] ? "the TX half went away" : "the RX half went away");
        }
        if (!p->up && p->t[0] && p->t[1] && now - p->failed_at >= (p->failed_at ? RETRY_S : 0)) {
            xStreamBufferReset(p->in[0]);
            xStreamBufferReset(p->in[1]);
            n[0] = n[1] = 0;
            start(p);
        }
        if (p->up) {
            for (int i = 0; i < 2; i++)
                if (n[i])
                    rdof_split_from_half(p->s, i, buf + i * (sizeof buf / 2), n[i], now);
            const rdof_slot_timing_t *st = &p->parent->cfg->slot_timing;
            if (st->utc && now - p->gate_at >= 1.0) {
                p->gate_at = now;
                netclock_source_t src = NETCLOCK_SRC_NONE;
                double err = NAN;
                const int64_t t_us = esp_timer_get_time();
                const bool synced = netclock_quality(&src, &err);
                rdof_slot_gate_update(&p->gate, now, synced, netclock_source_name(src), synced ? err : NAN);
                const double utc_minus_host = (double)(netclock_utc_ns(t_us, NULL) / 1000) - (double)t_us;
                rdof_split_set_utc(p->s, rdof_slot_gate_on_grid(&p->gate), utc_minus_host, isfinite(err) ? err : 0,
                                 st->offset_us, rdof_slot_gate_may_transmit(&p->gate), now);
            }
            rdof_split_tick(p->s, now);
            flush(p);
            if (p->s->failed[0])
                stop(p, p->s->failed);
        }
        xSemaphoreGive(p->lock);
    }
}

bool split_half_bytes(roam_t *r, const uint8_t *data, size_t len)
{
    int i;
    split_port_t *p = shell_of(r, &i);
    if (!p)
        return false;
    if (xStreamBufferSend(p->in[i], data, len, 0) != len)
        ESP_LOGW(TAG, "%s half: receive buffer full, bytes dropped", i ? "RX" : "TX");
    return true;
}

bool split_half_up(roam_t *r, const modem_transport_t *t)
{
    int i;
    split_port_t *p = shell_of(r, &i);
    if (!p)
        return false;
    p->t[i] = t;
    p->changed = true;
    ESP_LOGI(TAG, "%s: the %s half on %s", p->parent->cfg->name, i ? "RX" : "TX", t->name);
    return true;
}

bool split_half_down(roam_t *r)
{
    int i;
    split_port_t *p = shell_of(r, &i);
    if (!p)
        return false;
    p->t[i] = NULL;
    p->changed = true;
    return true;
}

void split_modem_start(roam_t *r)
{
    const rdof_config_t *c = r->cfg;
    for (int i = 0; i < 2; i++)
        if (!strcmp(c->split[i].modem_port, "sdr")) {
            snprintf(r->st.radio_error, sizeof r->st.radio_error,
                     "the SDR as a split's %s half isn't on the board yet: its sample clock against esp_timer isn't "
                     "stamped (REPEATER.md, \"Split duplex\")", i ? "RX" : "TX");
            ESP_LOGE(TAG, "%s: %s", c->name, r->st.radio_error);
            return;
        }
    split_port_t *p = big(sizeof *p);
    rdof_split_t *s = big(sizeof *s);
    if (!p || !s) {
        snprintf(r->st.radio_error, sizeof r->st.radio_error, "no memory for the split");
        return;
    }
    p->parent = r;
    p->s = s;
    rdof_slot_gate_init(&p->gate, &c->slot_timing);
    p->lock = xSemaphoreCreateMutex();
    p->ours = (modem_transport_t){"split", our_write, p};
    for (int i = 0; i < 2; i++) {
        p->in[i] = xStreamBufferCreate(2048, 1);
        p->hcfg[i] = big(sizeof *p->hcfg[i]);
        p->half[i] = big(sizeof *p->half[i]);
        if (!p->in[i] || !p->hcfg[i] || !p->half[i]) {
            snprintf(r->st.radio_error, sizeof r->st.radio_error, "no memory for the split's halves");
            return;
        }
        /* the half's settings: the repeater's, with the half's port */
        rdof_config_t *h = p->hcfg[i];
        *h = *c;
        const rdof_cfg_port_t *port = &c->split[i];
        snprintf(h->modem_port, sizeof h->modem_port, "%s", port->modem_port);
        h->uart_tx = port->uart_tx, h->uart_rx = port->uart_rx, h->uart_num = port->uart_num;
        h->uart_speed = port->uart_speed;
        snprintf(h->usb_serial, sizeof h->usb_serial, "%s", port->usb_serial);
        snprintf(h->usb_port, sizeof h->usb_port, "%s", port->usb_port);
        roam_t *sh = p->half[i];
        sh->i = r->i; /* a USB half takes the repeater's USB device slot (one USB half at most) */
        sh->cfg = h;
        sh->psram_stacks = r->psram_stacks;
    }
    ports[r->i] = p;
    roam_task(r, split_task, "split", 4096, 12);
    for (int i = 0; i < 2; i++) {
        if (!strcmp(c->split[i].modem_port, "uart")) {
#if CONFIG_RDOF_MODEM_UART
            uart_modem_start(p->half[i]);
#else
            ESP_LOGE(TAG, "the %s half is on a UART, but this build has no UART modem (CONFIG_RDOF_MODEM_UART)",
                     i ? "RX" : "TX");
#endif
        } else {
#if CONFIG_RDOF_MODEM_USB
            usb_modem_start(p->half[i]);
#else
            ESP_LOGE(TAG, "the %s half is on USB, but this build has no USB modem (CONFIG_RDOF_MODEM_USB)",
                     i ? "RX" : "TX");
#endif
        }
    }
    ESP_LOGI(TAG, "%s: TX half on %s, RX half on %s", c->name, c->split[0].modem_port, c->split[1].modem_port);
}

void split_status_json(roam_t *r, rdof_json_t *j)
{
    split_port_t *p = r->i < RDOF_MAX_RADIOS ? ports[r->i] : NULL;
    if (!p || p->parent != r)
        return;
    xSemaphoreTake(p->lock, portMAX_DELAY);
    rdof_json_open(j, "split");
    rdof_json_bool(j, "ready", p->up && p->s->ready);
    rdof_json_bool(j, "epoch", p->up && p->s->epoch_ok);
    rdof_json_str(j, "tx", p->s->halves[0].description);
    rdof_json_str(j, "rx", p->s->halves[1].description);
    rdof_json_int(j, "dropped", p->s->dropped);
    rdof_json_str(j, "failed", p->s->failed);
    if (p->parent->cfg->slot_timing.utc) {
        rdof_json_open(j, "slot_timing");
        rdof_json_str(j, "state", rdof_gate_state_name(p->gate.state));
        rdof_json_str(j, "source", p->gate.source);
        if (isfinite(p->gate.err_us))
            rdof_json_float3(j, "err_us", p->gate.err_us);
        rdof_json_float3(j, "holdover_s", rdof_slot_gate_holdover_age(&p->gate, now_mono()));
        rdof_json_str(j, "why", p->gate.why);
        rdof_json_str(j, "align", rdof_align_state_name(p->s->align_state));
        rdof_json_int(j, "phase_us", p->s->align_err_us);
        rdof_json_int(j, "align_flags", p->s->align_flags);
        rdof_json_close(j);
    }
    rdof_json_close(j);
    xSemaphoreGive(p->lock);
}

#elif CONFIG_RDOF_ROAMER

#include "esp_log.h"

void split_modem_start(roam_t *r)
{
    snprintf(r->st.radio_error, sizeof r->st.radio_error, "radio type split needs the repeater "
             "(CONFIG_RDOF_FEATURE_REPEATER)");
    ESP_LOGE("split", "%s: %s", r->cfg->name, r->st.radio_error);
}
bool split_half_bytes(roam_t *r, const uint8_t *data, size_t len) { return false; }
bool split_half_up(roam_t *r, const modem_transport_t *t) { return false; }
bool split_half_down(roam_t *r) { return false; }
void split_status_json(roam_t *r, rdof_json_t *j) {}

#endif
