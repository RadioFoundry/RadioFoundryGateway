// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Each of the board's roamers or repeaters (up to RDOF_MAX_RADIOS, one task each): start the modem, build the
 * radio from the settings, then poll the transceiver or repeater every 20 ms until the modem fails, and start
 * over (1 s, doubling to 30 s).
 */
#include <math.h>
#include <string.h>

#include "app.h"
#include "heard.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "radio";


__attribute__((unused)) static void lock_fn(void *ctx)
{
    xSemaphoreTake((SemaphoreHandle_t)((roam_t *)ctx)->lock, portMAX_DELAY);
}

__attribute__((unused)) static void unlock_fn(void *ctx)
{
    xSemaphoreGive((SemaphoreHandle_t)((roam_t *)ctx)->lock);
}

__attribute__((unused)) static double clock_fn(void *ctx)
{
    return now_mono();
}

__attribute__((unused)) static void hear_fn(void *ctx, int slot, uint8_t control, const uint8_t *burst, double dbm, int cc, uint32_t freq)
{
    rx_hear(ctx, slot, control, burst, dbm, cc, freq);
}

__attribute__((unused)) static void on_tx(void *ctx, const char *action, const rdof_tx_call_info_t *call, double duration)
{
    roam_t *r = ctx;
    snprintf(r->st.sending, sizeof r->st.sending, "%s%lu>%lu", call->group ? "T" : "P", (unsigned long)call->src,
             (unsigned long)call->dst);
    r->st.on_air = !strcmp(action, "start");
    heard_tx(r->cfg->mode == RDOF_CFG_ROAMER ? "roamer" : "repeater", r->cfg->name, action, call, duration);
    roam_tx_event(r, action, call, duration);
}

__attribute__((unused)) static void on_sent(void *ctx, const rdof_tx_call_info_t *call, const rdof_net_burst_t *b)
{
    roam_tx_burst(ctx, call, b);
}

__attribute__((unused)) static void send_fn(void *ctx, const uint8_t *data, size_t len)
{
    roam_hb_send(ctx, data, len);
}

__attribute__((unused)) static void on_key(void *ctx, bool keyed)
{
    roam_t *r = ctx;
    r->st.on_air = keyed;
    roam_publish_status(r);
}

__attribute__((unused)) static void *big_alloc(size_t n)
{
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : calloc(1, n);
}

void radio_channel_now(roam_t *r, uint32_t *freq, int *cc)
{
    *freq = 0;
    *cc = 0;
#if RDOF_FEATURE_TX
    if (r->cfg->mode == RDOF_CFG_ROAMER && r->txvr)
        rdof_txvr_channel_now(r->txvr, freq, cc);
#endif
}

/* Config.build_radio: the kind (auto: from the modem's description), modem settings, tuner, RSSI map. */
static bool build_radio(roam_t *r)
{
    const rdof_config_t *c = r->cfg;
    rdof_radio_kind_t kind;
    const char *t = c->radio_type;
    if (!strcmp(t, "auto")) {
        kind = rdof_radio_detect(r->modem.version);
        if (kind == RDOF_RADIO_UNKNOWN) {
            snprintf(r->st.radio_error, sizeof r->st.radio_error, "can't tell what '%.40s' is: set the radio's type",
                     r->modem.version);
            return false;
        }
    } else {
        kind = rdof_radio_kind_of(t); /* config.c accepts only these, "auto" aside */
        if (kind == RDOF_RADIO_UNKNOWN)
            kind = RDOF_RADIO_MMDVM;
    }
    rdof_radio_init(&r->radio, &r->modem, kind);
    r->radio.config = c->modem;
    r->radio.rx_offset_hz = c->rx_offset_hz;
    r->radio.tx_offset_hz = c->tx_offset_hz;
    uint32_t fixed = c->mode == RDOF_CFG_REPEATER ? c->tx_freq : 0;
    if (c->tuner == RDOF_TUNER_MODEM || (c->tuner == RDOF_TUNER_NONE && kind != RDOF_RADIO_MMDVM)) {
        r->radio.tuner = RDOF_TUNER_MODEM;
    } else if (c->tuner == RDOF_TUNER_FIXED || fixed) {
        r->radio.tuner = RDOF_TUNER_FIXED;
        r->radio.fixed_hz = c->tuner_freq ? c->tuner_freq : fixed;
        if (!r->radio.fixed_hz) {
            snprintf(r->st.radio_error, sizeof r->st.radio_error, "radio.tuner fixed needs its freq");
            return false;
        }
    } else {
        snprintf(r->st.radio_error, sizeof r->st.radio_error,
                 "an MMDVM board needs radio.tuner: fixed (rigctld and command tuners need a computer)");
        return false;
    }
    if (c->rssi.n) {
        r->radio.rssi_map = c->rssi;
        r->radio.hotspot_rssi = false;
    }
    if (c->headers >= 0)
        r->radio.headers = c->headers;
    if (c->terminators >= 0)
        r->radio.terminators = c->terminators;
    r->radio.settle_ms = c->settle_ms;
    r->st.radio_kind = rdof_radio_kind_name(kind);
    if (c->mode == RDOF_CFG_REPEATER) {
        /* RepeaterConfig.build_radio */
        if (c->sfr) {
            int in, out;
            if (kind != RDOF_RADIO_HOTSPOT || !rdof_sfr_slots(r->modem.version, &in, &out)) {
                snprintf(r->st.radio_error, sizeof r->st.radio_error,
                         "'%.40s' can't be a single frequency repeater: it needs a radio in hotspot mode with SFR in "
                         "its firmware",
                         r->modem.version);
                return false;
            }
            if (in != c->inbound_slot) {
                snprintf(r->st.radio_error, sizeof r->st.radio_error,
                         "the radio repeats TS%d on TS%d; the settings say TS%d: set inbound_slot to match", in, out,
                         c->inbound_slot);
                return false;
            }
            char framing[16];
            rdof_sfr_framing(r->modem.version, framing, sizeof framing);
            if (strcmp(framing, "TS"))
                ESP_LOGW(TAG, "The radio repeats with %s framing, which doesn't mark the slot: set TS on the radio",
                         framing[0] ? framing : "unknown");
        } else if (c->dcdm) {
            if (!rdof_dcdm_capable(r->modem.version)) {
                snprintf(r->st.radio_error, sizeof r->st.radio_error,
                         "'%.40s' can't do dual capacity direct mode: DCDM isn't in its description", r->modem.version);
                return false;
            }
        } else if (kind == RDOF_RADIO_HOTSPOT) {
            snprintf(r->st.radio_error, sizeof r->st.radio_error,
                     "a radio in hotspot mode is simplex only: it can't be a repeater (but see mode: sfr)");
            return false;
        }
    }
    return true;
}

static void modem_rx(void *ctx, int slot, uint8_t control, const uint8_t *burst, int rssi)
{
    roam_t *r = ctx;
    const rdof_config_t *c = r->cfg;
    r->st.bursts_heard++;
#if RDOF_FEATURE_REPEATER
    if (c->mode == RDOF_CFG_REPEATER) {
        rdof_repeater_heard(r->rpt, slot, control, burst, rssi);
        return;
    }
#endif
#if RDOF_FEATURE_TX
    rdof_txvr_heard(r->txvr, slot, control, burst, rssi);
#else
    /* receive only: everything heard is passed on */
    int dbm;
    rx_hear(r, slot, control, burst, rdof_radio_rssi(&r->radio, rssi, c->idle.freq, &dbm) ? dbm : NAN, c->idle.cc,
            c->idle.freq);
#endif
}

static void modem_lost(void *ctx, int slot)
{
    roam_t *r = ctx;
#if RDOF_FEATURE_REPEATER
    if (r->cfg->mode == RDOF_CFG_REPEATER)
        rdof_repeater_lost(r->rpt, slot);
#endif
}

static void modem_nak(void *ctx, uint8_t command, int err)
{
    roam_t *r = ctx;
#if RDOF_FEATURE_REPEATER
    if (r->cfg->mode == RDOF_CFG_REPEATER) {
        rdof_repeater_nak(r->rpt, command, err);
        return;
    }
#endif
    ESP_LOGW(TAG, "modem refused command 0x%02x (error %d)", command, err);
}

#if RDOF_FEATURE_TX
static void retune(void *ctx, uint32_t freq, int cc)
{
    roam_t *r = ctx;
    ESP_LOGI(TAG, "tuned to %.5f MHz CC%d", freq / 1e6, cc);
    roam_hb_send_config(r, false); /* tell HBlink4 where we are now */
    roam_publish_status(r);
}

static void scan(void *ctx, bool holding)
{
    roam_t *r = ctx;
    ESP_LOGI(TAG, "scan: %s", holding ? "holding" : "resuming");
    roam_hb_send_config(r, false);
    roam_publish_status(r);
}

static void setup_txvr(roam_t *r)
{
    const rdof_config_t *c = r->cfg;
    rdof_txvr_t *t = r->txvr = big_alloc(sizeof *r->txvr);
    rdof_txvr_init(t, &r->radio, c->peer_id);
    for (int i = 0; i < c->nchannels && i < RDOF_TXVR_MAX_CHANNELS; i++) {
        t->channels[i].freq = c->channels[i].freq;
        t->channels[i].cc = c->channels[i].cc;
        t->channels[i].power = c->channels[i].power;
        t->nchannels = i + 1;
    }
    t->listen_s = c->listen_s;
    t->clear_s = c->clear_s;
    t->prebuffer = c->prebuffer;
    t->jb.adaptive = c->jitter_auto;
    t->jb.min = rdof_jb_bursts(c->jitter_min_ms);
    t->jb.max = rdof_jb_bursts(c->jitter_max_ms);
    t->csbk_preambles = c->csbk_preambles;
    t->answer_s = c->answer_s;
    t->answer_max_s = c->answer_max_s;
    for (int i = 0; i < c->ncovered && i < RDOF_TXVR_MAX_COVERED; i++)
        t->covered[t->ncovered++] = c->covered[i];
    t->has_idle = true;
    t->idle.freq = c->idle.freq;
    t->idle.cc = c->idle.cc;
    t->idle.power = c->idle.power;
    t->idle_after_s = c->idle_after_s;
    snprintf(t->cwid, sizeof t->cwid, "%s", c->cwid);
    t->cwid_every_s = c->cwid_every_s;
#if CONFIG_RDOF_SCAN
    if (c->has_scan) {
        t->has_scan = true;
        rdof_scan_defaults(&t->scan);
        for (int i = 0; i < c->nscan && i < RDOF_TXVR_MAX_CHANNELS; i++) {
            t->scan.channels[i].freq = c->scan_channels[i].freq;
            t->scan.channels[i].cc = c->scan_channels[i].cc;
            t->scan.channels[i].power = c->scan_channels[i].power;
            t->scan.nchannels = i + 1;
        }
        t->scan.dwell_s = c->dwell_s;
        t->scan.hang_s = c->hang_s_scan;
        t->scan.carrier_s = c->carrier_s;
    }
#else
    if (c->has_scan)
        ESP_LOGW(TAG, "scan is set but this build has no scanning (CONFIG_RDOF_SCAN): staying on the idle channel");
#endif
    t->send = send_fn;
    t->on_retune = retune;
    t->on_scan = scan;
    t->on_tx = on_tx;
    t->on_sent = on_sent;
    t->lock = lock_fn;
    t->unlock = unlock_fn;
    t->receiver.hear = hear_fn;
    t->receiver.ctx = r;
    t->ctx = r;
    t->clock = clock_fn;
}
#endif

static void radio_task(void *arg)
{
    roam_t *r = arg;
    const rdof_config_t *c = r->cfg;
    int backoff_ms = 1000;
    for (;;) {
        r->st.radio_ready = false;
        while (!modem_transport_ready(r))
            vTaskDelay(pdMS_TO_TICKS(200));
        if (!rdof_modem_start(&r->modem, 6)) {
            snprintf(r->st.radio_error, sizeof r->st.radio_error, "%s", r->modem.error);
            ESP_LOGW(TAG, "no answer from the modem: %s", r->modem.error);
            goto retry;
        }
        snprintf(r->st.modem_desc, sizeof r->st.modem_desc, "%s", r->modem.version);
        r->st.modem_protocol = r->modem.protocol;
        ESP_LOGI(TAG, "modem: %s (protocol %d)", r->modem.version[0] ? r->modem.version : "(no description)",
                 r->modem.protocol);
        if (!build_radio(r)) {
            ESP_LOGE(TAG, "%s", r->st.radio_error);
            goto retry;
        }
        r->st.radio_error[0] = 0;
        ESP_LOGI(TAG, "radio type %s, tuned by %s", r->st.radio_kind,
                 r->radio.tuner == RDOF_TUNER_MODEM ? "the modem" : "nothing (fixed)");
        bool ok = false;
#if RDOF_FEATURE_REPEATER
        if (c->mode == RDOF_CFG_REPEATER) {
            r->rpt->radio = &r->radio;
            ok = rdof_repeater_start(r->rpt);
            if (ok) {
                r->st.radio_ready = true;
                backoff_ms = 1000;
                roam_publish_status(r);
                TickType_t wake = xTaskGetTickCount();
                while (rdof_repeater_poll(r->rpt))
                    vTaskDelayUntil(&wake, pdMS_TO_TICKS(20));
                rdof_repeater_stop(r->rpt);
            }
        }
#endif
#if RDOF_FEATURE_TX
        if (c->mode == RDOF_CFG_ROAMER) {
            r->txvr->radio = &r->radio;
            ok = rdof_txvr_start(r->txvr);
            if (ok) {
                r->st.radio_ready = true;
                backoff_ms = 1000;
                roam_publish_status(r);
                TickType_t wake = xTaskGetTickCount();
                int64_t next_log = 0;
                while (rdof_txvr_poll(r->txvr)) {
                    r->st.radio_ready = r->txvr->ready;
                    if (esp_log_timestamp() >= next_log) {
                        ESP_LOGI(TAG, "%lu requests, %lu without a reply, %lu refused; %lu bursts heard; %s",
                                 (unsigned long)r->modem.requests, (unsigned long)r->modem.no_reply,
                                 (unsigned long)r->modem.refused, (unsigned long)r->st.bursts_heard,
                                 rdof_txvr_state_name(r->txvr));
                        next_log = esp_log_timestamp() + 60000;
                    }
                    vTaskDelayUntil(&wake, pdMS_TO_TICKS(20));
                }
            }
        }
#endif
#if !RDOF_FEATURE_TX
        /* Receive only: on the idle channel, everything heard passed on; a status read every 2 s keeps it in hotspot mode. */
        ok = rdof_modem_set_mode(&r->modem, RDOF_MODE_DMR) &&
             rdof_radio_tune(&r->radio, c->idle.freq, c->idle.cc, c->idle.power);
        if (ok) {
            r->st.radio_ready = true;
            backoff_ms = 1000;
            rdof_mmdvm_status_t st;
            while (rdof_modem_status(&r->modem, &st))
                vTaskDelay(pdMS_TO_TICKS(2000));
        }
#endif
        if (!ok)
            snprintf(r->st.radio_error, sizeof r->st.radio_error, "%s", r->modem.error);
        ESP_LOGW(TAG, "radio: %s", r->modem.error);
    retry:
        r->st.radio_ready = false;
        roam_publish_status(r);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        backoff_ms = backoff_ms < 30000 ? backoff_ms * 2 : 30000;
    }
}

void radio_start(roam_t *r)
{
    const rdof_config_t *c = r->cfg;
    r->lock = xSemaphoreCreateMutex();
    r->modem.cb_ctx = r;
    r->modem.on_rx = modem_rx;
    r->modem.on_lost = modem_lost;
    r->modem.on_nak = modem_nak;
#if RDOF_FEATURE_REPEATER
    if (c->mode == RDOF_CFG_REPEATER) {
        r->rpt = big_alloc(sizeof *r->rpt);
        rdof_repeater_init(r->rpt, &r->radio, c->peer_id, c->rx_freq, c->tx_freq, c->cc);
        r->rpt->power = c->power;
        r->rpt->enabled[1] = c->slot1;
        r->rpt->enabled[2] = c->slot2;
        r->rpt->inactivity_s = c->hang_s; /* T_MSInactiv */
        rdof_repeater_set_call_hang(r->rpt, c->call_hang_s);
        r->rpt->short_lc_ts1_low = c->short_lc_ts1_low;
        r->rpt->prebuffer = c->prebuffer > 1 ? c->prebuffer : 1;
        r->rpt->jb.adaptive = c->jitter_auto;
        r->rpt->jb.min = rdof_jb_bursts(c->jitter_min_ms);
        r->rpt->jb.max = rdof_jb_bursts(c->jitter_max_ms);
        snprintf(r->rpt->cwid, sizeof r->rpt->cwid, "%s", c->cwid);
        r->rpt->cwid_every_s = c->cwid_every_s;
        if (c->sfr)
            rdof_repeater_set_sfr(r->rpt, c->inbound_slot);
        else if (c->dcdm)
            rdof_repeater_set_dcdm(r->rpt);
        r->rpt->on_tx = on_tx;
        r->rpt->on_sent = on_sent;
        r->rpt->on_key = on_key;
        r->rpt->lock = lock_fn;
        r->rpt->unlock = unlock_fn;
        r->rpt->receiver.hear = hear_fn;
        r->rpt->receiver.ctx = r;
        r->rpt->ctx = r;
        r->rpt->clock = clock_fn;
    }
#else
    if (c->mode == RDOF_CFG_REPEATER)
        ESP_LOGE(TAG, "the settings are for a repeater, but this build has none (CONFIG_RDOF_FEATURE_REPEATER)");
#endif
#if RDOF_FEATURE_TX
    if (c->mode == RDOF_CFG_ROAMER)
        setup_txvr(r);
#endif
    roam_task(r, radio_task, "radio", 8192, 11);
}
