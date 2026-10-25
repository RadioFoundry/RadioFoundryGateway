// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * An sdr_transceiver's modem (docs/reference.md, "Transmit"): the SDR's front end
 * as the radio, through sdr's emulated MMDVM modem (rdof/sdr/sdrmodem.h).
 * The roamer or repeater talks to it as it would to an MMDVM_HS on a UART:
 * this is one more transport under its rdof_modem. The modem hears through the
 * SDR's receiver (sdr_tx_heard, from the DSP tasks) and transmits through the
 * front end, which pulls the modulator's samples (tx_set). One lock covers
 * the modem: the roamer's writes, the DSP's bursts and pulls.
 *
 * The bench API's transmit (bench_tx.c, via=modem) opens the same modem when
 * no roamer owns it, for the length of one WS /tx job.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "app.h"

#if CONFIG_RDOF_SDR

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rdof/sdr/sdrmodem.h"

static rdof_sdrmodem_t *volatile mdm;
static SemaphoreHandle_t lock;
static const char *TAG = "sdrmodem";
static rdof_fe_t fe;
static double rate, center;
#if CONFIG_RDOF_ROAMER
static roam_t *owner;
#endif
/* the bench's job (bench_tx.c), while it holds the modem */
static void (*bench_reply)(void *ctx, const uint8_t *frame, size_t len);
static void *bench_ctx;

#define TAKE() xSemaphoreTake(lock, portMAX_DELAY)
#define GIVE() xSemaphoreGive(lock)

void sdr_tx_heard(void *ctx, int channel, const rdof_sdr_burst_t *b, double t)
{
    if (!mdm)
        return;
    TAKE();
    if (mdm)
        rdof_sdrmodem_heard(mdm, channel, b);
    GIVE();
}

bool sdr_tx_muted(void *ctx, int64_t position)
{
    if (!mdm)
        return false;
    TAKE();
    bool r = mdm && rdof_sdrmodem_muted_at(mdm, position);
    GIVE();
    return r;
}

static bool pull_iq(void *ctx, int64_t pos, rdof_cf_t *iq, int n)
{
    TAKE();
    if (mdm)
        slot_timing_apply(mdm);
    bool r = mdm && rdof_sdrmodem_pull_iq(mdm, pos, iq, n);
    GIVE();
    return r;
}

static bool pull_freq(void *ctx, int64_t pos, float *hz, float *env, int n)
{
    TAKE();
    if (mdm)
        slot_timing_apply(mdm);
    bool r = mdm && rdof_sdrmodem_pull_freq(mdm, pos, hz, env, n);
    GIVE();
    return r;
}

static const rdof_fe_tx_source_t source = {pull_iq, pull_freq, NULL};

static int io_channel(void *ctx, uint32_t hz) { return sdr_rx_channel(hz); }

static bool io_tx_tune(void *ctx, uint32_t hz, double *off)
{
    const rdof_fe_caps_t *c = fe.caps;
    if (hz < c->tx_freq_min || hz > c->tx_freq_max)
        return false;
    if (c->tx_mode == RDOF_FE_TX_IQ) { /* the carrier inside the stream's passband */
        *off = (double)hz - center;
        return fabs(*off) <= rate * c->usable / 2;
    }
    *off = 0; /* a chip that tunes its own transmitter: the samples are the deviation only */
    return c->tx_mode == RDOF_FE_TX_FREQ && fe.ops->tx_tune && fe.ops->tx_tune(fe.ctx, hz) == 0;
}

/* Why the front end can't be this modem's transmitter (NULL: it can). Fills fe, rate, center. */
static const char *frontend_why(void)
{
    if (!sdr_tx_frontend(&fe, &rate, &center))
        return "the SDR isn't receiving yet";
    const rdof_fe_caps_t *c = fe.caps;
    const bool modulates = c->tx_mode == RDOF_FE_TX_IQ || (c->tx_mode == RDOF_FE_TX_FREQ && fe.ops->tx_tune);
    if (c->duplex == RDOF_FE_RX_ONLY || !modulates || !fe.ops->tx_set)
        return "the SDR's front end can't transmit";
    return NULL;
}

/* A new modem on the front end, transmitting from now on: NULL, or why not. */
static const char *modem_up(const rdof_sdrmodem_io_t *io, double power_dbm)
{
    const rdof_fe_caps_t *c = fe.caps;
    rdof_sdrmodem_t *m = heap_caps_calloc(1, sizeof *m, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!m)
        m = heap_caps_calloc(1, sizeof *m, MALLOC_CAP_8BIT);
    const uint32_t tx_fs = (uint32_t)(c->tx_rate > 0 ? c->tx_rate : rate);
    static char why[96];
    if (!m || rdof_sdrmodem_init(m, io, (uint32_t)rate, tx_fs, c->duplex == RDOF_FE_FULL_DUPLEX)) {
        snprintf(why, sizeof why, "%s", m ? m->error : "no memory for the SDR modem");
        free(m);
        return why;
    }
    TAKE();
    mdm = m;
    GIVE();
    fe.ops->tx_set(fe.ctx, &source, power_dbm);
    return NULL;
}

const char *sdr_modem_bench_why(void)
{
#if CONFIG_RDOF_ROAMER
    if (owner)
        return "the SDR's modem belongs to a roamer (an sdr_transceiver)";
#endif
    if (bench_reply)
        return "the SDR's modem is in use by another bench job";
    return frontend_why();
}

static void bench_io_reply(void *ctx, const uint8_t *frame, size_t len)
{
    if (bench_reply)
        bench_reply(bench_ctx, frame, len);
}

const char *sdr_modem_bench_open(void (*reply)(void *ctx, const uint8_t *frame, size_t len), void *ctx,
                                 double power_dbm)
{
    if (!lock)
        lock = xSemaphoreCreateMutex();
    const char *why = sdr_modem_bench_why();
    if (why)
        return why;
    bench_reply = reply;
    bench_ctx = ctx;
    const rdof_sdrmodem_io_t io = {bench_io_reply, io_channel, io_tx_tune, NULL, NULL};
    if ((why = modem_up(&io, power_dbm)) != NULL) {
        bench_reply = NULL;
        return why;
    }
    ESP_LOGI(TAG, "the bench holds the SDR modem (%s, %.0f dBm)", fe.caps->kind, power_dbm);
    return NULL;
}

void sdr_modem_bench_write(const uint8_t *data, size_t len)
{
    TAKE();
    if (mdm && bench_reply)
        rdof_sdrmodem_write(mdm, data, len);
    GIVE();
}

void sdr_modem_bench_close(void)
{
    if (!bench_reply)
        return;
    fe.ops->tx_set(fe.ctx, NULL, 0);
    TAKE();
    rdof_sdrmodem_t *m = mdm;
    mdm = NULL;
    bench_reply = NULL;
    GIVE();
    if (m) {
        rdof_sdrmodem_free(m);
        free(m);
    }
    ESP_LOGI(TAG, "the bench let the SDR modem go");
}

#if CONFIG_RDOF_ROAMER

static void io_reply(void *ctx, const uint8_t *frame, size_t len) { modem_on_bytes(owner, frame, len); }

static bool sdr_write(void *ctx, const uint8_t *data, size_t len)
{
    TAKE();
    rdof_sdrmodem_write(mdm, data, len);
    GIVE();
    return true;
}

static void fail(roam_t *r, const char *why)
{
    snprintf(r->st.radio_error, sizeof r->st.radio_error, "%s", why);
    ESP_LOGE(TAG, "%s: %s", r->cfg->name, why);
}

static void start_task(void *arg)
{
    roam_t *r = arg;
    while (!sdr_tx_frontend(&fe, &rate, &center) || bench_reply)
        vTaskDelay(pdMS_TO_TICKS(200));
    const char *why = frontend_why();
    const rdof_fe_caps_t *c = fe.caps;
    if (why) {
        fail(r, "the SDR's front end can't transmit (yet): this sdr_transceiver only listens");
    } else if (r->cfg->sdr_duplex == RDOF_SDR_DUPLEX_FULL && c->duplex != RDOF_FE_FULL_DUPLEX) {
        fail(r, "duplex: full, but the SDR's front end is half duplex");
    } else {
        const rdof_sdrmodem_io_t io = {io_reply, io_channel, io_tx_tune, NULL, NULL};
        if (r->cfg->slot_timing.utc) /* the repeater's own slot_timing over the SDR's */
            slot_timing_setup(&r->cfg->slot_timing);
        if ((why = modem_up(&io, r->cfg->tx_power_dbm)) != NULL) {
            fail(r, why);
        } else {
            r->sdr = (modem_transport_t){"sdr", sdr_write, r};
            modem_transport_up(r, &r->sdr);
            ESP_LOGI(TAG, "%s: %s duplex on the %s, %.0f dBm", r->cfg->name,
                     r->cfg->sdr_duplex == RDOF_SDR_DUPLEX_FULL ? "full" : "half", c->kind, r->cfg->tx_power_dbm);
        }
    }
    vTaskDelete(NULL);
}

void sdr_modem_start(roam_t *r)
{
    if (owner) {
        fail(r, "the SDR's modem is taken by another radio");
        return;
    }
    owner = r;
    if (!lock)
        lock = xSemaphoreCreateMutex();
    roam_task(r, start_task, "sdrmodem", 4096, 5);
}

#endif /* CONFIG_RDOF_ROAMER */

#else /* no SDR in this build */

#if CONFIG_RDOF_ROAMER
#include "esp_log.h"
void sdr_modem_start(roam_t *r)
{
    snprintf(r->st.radio_error, sizeof r->st.radio_error, "an sdr_transceiver needs the SDR role (CONFIG_RDOF_SDR)");
    ESP_LOGE("sdrmodem", "%s: %s", r->cfg->name, r->st.radio_error);
}
#endif

#endif
