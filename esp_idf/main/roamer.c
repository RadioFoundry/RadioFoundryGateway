// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The roamer or repeater role on an MMDVM modem
 * (an OpenGD77 or other radio_hotspot radio on USB or a UART, an MMDVM_HS HAT
 * on a UART), with its own call tracker, HBlink4 peer, MQTT connection, and
 * the OLED for the first. "run" may name up to RDOF_MAX_RADIOS of them, each a
 * roam_t of its own. Their memory (the transceiver or repeater, in PSRAM) and
 * tasks exist only once they're started.
 */
#include <string.h>

#include "app.h"
#include "mem.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "roamer";

roam_t *g_roam[RDOF_MAX_RADIOS];
int g_nroam;

/* Internal RAM a second radio needs beyond its PSRAM stacks: its MQTT client's task (6 KB), the UART or USB
 * driver's buffers, lwIP's sockets (docs/reference.md, "More than one radio"). Less free than this: it doesn't start. */
#define EXTRA_RADIO_INTERNAL (20 * 1024)

bool roamer_uses_usb(void)
{
    for (int k = 0; k < g_roles.nradios; k++)
        if (g_rcfg[k] && rdof_config_uses_usb(g_rcfg[k]))
            return true;
    return false;
}

static void role_name(int k, char *out, size_t cap) { snprintf(out, cap, "radio%d", (k & 7) + 1); }

/* The memory plan's claims of each radio (mem.c, docs/spec/fw/memory.md), role "radio1", "radio2": its roam_t
 * (internal for the first, PSRAM after), its tasks' stacks (the first's internal; the second's internal while the
 * spare above the margin holds them, else PSRAM), a UART driver's buffers kept as holds, its MQTT client, CDC-ACM
 * and SCTP as budgets. True if a radio is on USB. */
bool roamer_mem_claims(void)
{
    bool usb = false;
    for (int k = 0; k < g_roles.nradios; k++) {
        const rdof_config_t *c = g_rcfg[k];
        if (!c)
            continue;
        char role[12];
        role_name(k, role, sizeof role);
        /* an sdr_transceiver's like a second radio's: no UART or USB to keep up with, and the SDR beside it has
         * the internal RAM (the first's stacks there, 34 KB, left the W12 without Wi-Fi, 2026-10-24) */
        const bool on_sdr = !strcmp(c->modem_port, "sdr");
        const uint8_t stacks = k || on_sdr ? RDOF_MEM_INTERNAL | RDOF_MEM_FALLBACK : RDOF_MEM_INTERNAL;
        mem_add(role, "roam_t", sizeof(roam_t), 8, k || on_sdr ? RDOF_MEM_PSRAM : RDOF_MEM_INTERNAL, RDOF_MEM_ALLOC);
        mem_add_task(role, "radio", 8192, stacks);
        mem_add_task(role, "rx", 8192, stacks);
        mem_add_task(role, "modem_rx", 6144, stacks);
        mem_add_task(role, "roam_hblink", 4096, stacks);
        mem_add_task(role, "roam_status", 4096, stacks);
        rdof_cfg_port_t p[2];
        const int n = rdof_config_ports(c, p);
        int uarts = 0;
        for (int i = 0; i < n; i++) {
            if (!strcmp(p[i].modem_port, "uart")) {
                mem_add_task(role, "uart_rx", 3072, stacks);
                uarts++;
            } else if (!strcmp(p[i].modem_port, "usb")) {
                mem_add_task(role, "usb_open", 4096, stacks);
                mem_add(role, "cdc", 1536, 4, RDOF_MEM_DMA, RDOF_MEM_BUDGET);
                usb = true;
            } else if (!strcmp(p[i].modem_port, "sdr")) {
                mem_add_task(role, "sdrmodem", 4096, stacks);
            }
        }
        if (n > 1)
            mem_add_task(role, "split", 4096, stacks);
        if (uarts) { /* uart_driver_install's rings: internal, whatever the radio */
            mem_add_hold(role, "uart_rx_buf", 4096, uarts, RDOF_MEM_INTERNAL);
            mem_add_hold(role, "uart_tx_buf", 2048, uarts, RDOF_MEM_INTERNAL);
        }
        mem_add(role, "mqtt", MQTT_CLIENT_INTERNAL, 4, RDOF_MEM_INTERNAL, RDOF_MEM_BUDGET);
        if (!k) /* the first radio's small allocations: HBlink4's buffer, MQTT and receive state */
            mem_add(role, "heap", 4096, 4, RDOF_MEM_INTERNAL, RDOF_MEM_BUDGET);
        if (!strcmp(c->transport, "sctp")) /* the association and its task (tinysctp) */
            mem_add(role, "sctp", 6144 + 5120, 4, RDOF_MEM_INTERNAL, RDOF_MEM_BUDGET);
    }
    return usb;
}

/* A radio's task: on its planned stack, else as before (the first radio's internal, the rest in PSRAM). */
bool roam_task(roam_t *r, void (*fn)(void *), const char *name, uint32_t stack, int prio)
{
    char role[12];
    role_name(r->i, role, sizeof role);
    if (mem_task_planned(role, name, fn, r, prio, tskNO_AFFINITY, NULL))
        return true;
    if (!r->psram_stacks)
        return xTaskCreate(fn, name, stack, r, prio, NULL) == pdPASS;
    return xTaskCreatePinnedToCoreWithCaps(fn, name, stack, r, prio, NULL, tskNO_AFFINITY, MALLOC_CAP_SPIRAM) ==
           pdPASS;
}

static void start_one(int k)
{
    const rdof_config_t *c = g_rcfg[k];
    char role[12];
    role_name(k, role, sizeof role);
    const bool on_sdr = !strcmp(c->modem_port, "sdr"); /* as roamer_mem_claims plans it */
    roam_t *r = mem_take(role, "roam_t", NULL);
    if (!r && !k && !on_sdr)
        r = heap_caps_calloc(1, sizeof *r, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!r)
        r = heap_caps_calloc(1, sizeof *r, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!r) {
        ESP_LOGE(TAG, "no memory for %s", c->name);
        return;
    }
    r->i = k;
    r->cfg = c;
    r->psram_stacks = k > 0 || on_sdr;
    if (mem_role_state(role) < 0) {
        snprintf(r->st.radio_error, sizeof r->st.radio_error, "not started: memory plan: %.80s", mem_role_why(role));
        ESP_LOGE(TAG, "%s %s", c->name, r->st.radio_error);
        g_roam[g_nroam++] = r;
        return;
    }
    if (k && mem_role_state(role) == 0 && heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < EXTRA_RADIO_INTERNAL) {
        snprintf(r->st.radio_error, sizeof r->st.radio_error, "not started: %u KB of internal RAM free, it needs %u",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024), EXTRA_RADIO_INTERNAL / 1024);
        ESP_LOGE(TAG, "%s %s", c->name, r->st.radio_error);
        g_roam[g_nroam++] = r;
        return;
    }
    r->st.started = true;
    g_roam[g_nroam++] = r;
    ESP_LOGI(TAG, "%s %s, peer %lu, modem on %s", c->mode == RDOF_CFG_ROAMER ? "roamer" : "repeater", c->name,
             (unsigned long)c->peer_id, c->modem_port);
    modem_port_init(r);
    roam_rx_start(r);
    radio_start(r);
    roam_hb_start(r);
    roam_mqtt_start(r);
    if (!strcmp(c->modem_port, "sdr")) {
        sdr_modem_start(r);
    } else if (!strcmp(c->modem_port, "split")) {
        split_modem_start(r);
    } else if (!strcmp(c->modem_port, "uart")) {
#if CONFIG_RDOF_MODEM_UART
        uart_modem_start(r);
#else
        ESP_LOGE(TAG, "the settings say modem: uart, but this build has no UART modem (CONFIG_RDOF_MODEM_UART)");
#endif
    } else {
#if CONFIG_RDOF_MODEM_USB
        usb_modem_start(r);
#else
        ESP_LOGE(TAG, "the settings say modem: usb, but this build has no USB modem (CONFIG_RDOF_MODEM_USB)");
#endif
    }
}

void roamers_start(void)
{
    if (g_nroam)
        return;
    for (int k = 0; k < g_roles.nradios; k++)
        if (g_rcfg[k])
            start_one(k);
}

#if CONFIG_RDOF_HBLINK_SCTP
#define STATUS_SCTP 1024 /* room for the SCTP association's status */
#endif

static void one_json(rdof_json_t *j, roam_t *r, const char *key)
{
    const roam_status_t *s = &r->st;
    const rdof_config_t *c = r->cfg;
    rdof_json_open(j, key);
    rdof_json_str(j, "name", c->name);
    rdof_json_str(j, "mode", c->mode == RDOF_CFG_ROAMER ? "roamer"
                                : c->sfr                   ? "sfr repeater"
                                : c->dcdm                  ? "dcdm gateway"
                                                           : "repeater");
    rdof_json_bool(j, "started", s->started);
    rdof_json_str(j, "port", c->modem_port);
    split_status_json(r, j);
    rdof_json_bool(j, "modem_open", s->modem_open);
    rdof_json_bool(j, "radio_ready", s->radio_ready);
    rdof_json_str(j, "radio_error", s->radio_error);
    rdof_json_str(j, "modem", s->modem_desc);
    rdof_json_int(j, "protocol", s->modem_protocol);
    rdof_json_str(j, "radio", s->radio_kind ? s->radio_kind : "");
    rdof_json_int(j, "requests", r->modem.requests);
    rdof_json_int(j, "no_reply", r->modem.no_reply);
    rdof_json_int(j, "refused", r->modem.refused);
    rdof_json_int(j, "bursts_heard", s->bursts_heard);
    uint32_t freq = 0;
    int cc = 0;
    radio_channel_now(r, &freq, &cc);
#if RDOF_FEATURE_TX
    if (c->mode == RDOF_CFG_ROAMER && r->txvr) {
        rdof_json_str(j, "state", rdof_txvr_state_name(r->txvr));
        rdof_json_bool(j, "listening", rdof_txvr_listening(r->txvr));
        rdof_json_bool(j, "scanning", rdof_txvr_scanning(r->txvr));
        rdof_json_int(j, "status_misses", r->txvr->status_misses);
        rdof_json_int(j, "queue_dropped", r->txvr->call.dropped);
        rdof_jb_json(&r->txvr->jb, r->txvr->prebuffer, j, "jitter");
    }
#endif
#if RDOF_FEATURE_REPEATER
    if (c->mode == RDOF_CFG_REPEATER && r->rpt) {
        rdof_json_bool(j, "keyed", r->rpt->keyed);
        rdof_json_bool(j, "busy1", rdof_repeater_busy(r->rpt, 1));
        rdof_json_bool(j, "busy2", rdof_repeater_busy(r->rpt, 2));
        rdof_json_int(j, "dropped", r->rpt->dropped);
        rdof_jb_json(&r->rpt->jb, r->rpt->prebuffer, j, "jitter");
        freq = c->rx_freq;
        cc = c->cc;
    }
#endif
    rdof_json_int(j, "frequency", freq);
    rdof_json_int(j, "colorCode", cc);
    rdof_json_bool(j, "hblink", s->hblink_up);
    rdof_json_int(j, "hblink_tx", s->hblink_tx);
    rdof_json_int(j, "hblink_rx", s->hblink_rx);
#if CONFIG_RDOF_HBLINK_SCTP
    static char sctp[STATUS_SCTP];
    if (roam_hb_sctp_status(r, sctp, sizeof sctp))
        rdof_json_raw(j, "sctp", sctp);
#endif
    rdof_json_bool(j, "mqtt", s->mqtt_up);
    rdof_json_str(j, "heard", s->hearing ? s->heard : "");
    rdof_json_str(j, "sending", s->on_air ? s->sending : "");
    rdof_json_close(j);
}

void roamer_status_json(rdof_json_t *j)
{
    if (!g_nroam) {
        /* not started (both roles on USB, the other one plugged in): the first's settings */
        roam_t idle = {.cfg = g_rcfg[0]};
        if (idle.cfg)
            one_json(j, &idle, "roamer");
        return;
    }
    one_json(j, g_roam[0], "roamer");
    if (g_nroam > 1) {
        rdof_json_raw(j, "roamers", "[");
        j->first[j->depth - 1] = true; /* the list's items take their commas as an object's members would */
        for (int k = 0; k < g_nroam; k++)
            one_json(j, g_roam[k], NULL);
        if (j->len + 2 < j->cap) {
            j->buf[j->len++] = ']';
            j->buf[j->len] = 0;
        }
    }
}

size_t roamer_resolved(char *out, size_t cap)
{
    size_t n = 0;
    for (int k = 0; k < g_roles.nradios && g_rcfg[k] && n + 64 < cap; k++) {
        if (k)
            n += (size_t)snprintf(out + n, cap - n, "\n%s:\n", g_rcfg[k]->name);
        size_t m = rdof_config_describe(g_rcfg[k], out + n, cap - n);
        for (size_t i = n; i < n + m; i++)
            if (out[i] == ';')
                out[i] = '\n';
        n += m;
        if (n + 80 < cap)
            n += (size_t)snprintf(out + n, cap - n, "\nport: %s%s%s%s%s\n", g_rcfg[k]->modem_port,
                                  g_rcfg[k]->usb_serial[0] ? " serial " : "", g_rcfg[k]->usb_serial,
                                  g_rcfg[k]->usb_port[0] ? " at port " : "", g_rcfg[k]->usb_port);
    }
    return n < cap ? n : cap - 1;
}
