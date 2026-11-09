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
 * no roamer owns it, for the length of one WS /tx job. A bench outbound job
 * borrows the front end from a roamer: the roamer's modem is parked (its
 * writes still taken, nothing sent or heard) and comes back after the job.
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
#include "rdof/features.h"
#include "rdof/sdr/sdrmodem.h"
#include "rdof/txpower.h"

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
static rdof_sdrmodem_t *parked; /* the roamer's modem while the bench borrows the front end */
static double cap_dbm = NAN;    /* a routed bench job's power on the roamer's modem, at most (NAN: none) */
static double level_dbm = NAN;  /* the roamer's last SET_FREQ RF level, in dBm (NAN: its device's power) */

#define TAKE() xSemaphoreTake(lock, portMAX_DELAY)
#define GIVE() xSemaphoreGive(lock)

#if CONFIG_RDOF_BENCH_TX && RDOF_FEATURE_DATA_BLOCKS
/* The bench's confirmed-data responder (rdof/mssim.h; bench_tx.c arms it) on the bench's modem, under the lock: fed
 * the bursts heard on its channel, polled from the front end's pulls. The LR2021 doesn't hear itself: no echo. */
#include "esp_attr.h"
#include "rdof/mssim.h"
#define MS_LEAD48 1440 /* answers go to the modulator this far ahead of it */
EXT_RAM_BSS_ATTR static rdof_mssim_t ms;
static int ms_ch = -1;
static bool ms_send(void *ctx, int64_t at48, const uint8_t burst[33], int64_t *placed48)
{
    return mdm && rdof_sdrmodem_place48(mdm, at48, burst, placed48);
}
#define MS_POLL()                                                                                                     \
    do {                                                                                                              \
        if (ms.armed && mdm && bench_reply)                                                                           \
            rdof_mssim_poll(&ms, rdof_sdrmodem_tx_frontier48(mdm), MS_LEAD48);                                        \
    } while (0)
#define MS_HEARD(c, b)                                                                                                \
    do {                                                                                                              \
        if (ms.armed && (c) == ms_ch && bench_reply)                                                                  \
            rdof_mssim_heard(&ms, &(b)->b, (b)->position);                                                            \
    } while (0)
#else
#define MS_POLL() ((void)0)
#define MS_HEARD(c, b) ((void)0)
#endif

void sdr_tx_heard(void *ctx, int channel, const rdof_sdr_burst_t *b, double t)
{
    if (!mdm)
        return;
    TAKE();
    if (mdm) {
        rdof_sdrmodem_heard(mdm, channel, b);
        MS_HEARD(channel, b);
    }
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
    MS_POLL();
    bool r = mdm && rdof_sdrmodem_pull_iq(mdm, pos, iq, n);
    GIVE();
    return r;
}

static bool pull_freq(void *ctx, int64_t pos, float *hz, float *env, int n)
{
    TAKE();
    if (mdm)
        slot_timing_apply(mdm);
    MS_POLL();
    bool r = mdm && rdof_sdrmodem_pull_freq(mdm, pos, hz, env, n);
    GIVE();
    return r;
}

static int try_pull_freq(void *ctx, int64_t pos, float *hz, float *env, int n)
{
    if (xSemaphoreTake(lock, 0) != pdTRUE)
        return -1; /* the radio task's turn (a reply into the roamer): the LR2021's reader doesn't wait for it */
    if (mdm)
        slot_timing_apply(mdm);
    MS_POLL();
    bool r = mdm && rdof_sdrmodem_pull_freq(mdm, pos, hz, env, n);
    GIVE();
    return r;
}

static int try_pull_iq(void *ctx, int64_t pos, rdof_cf_t *iq, int n)
{
    if (xSemaphoreTake(lock, 0) != pdTRUE)
        return -1;
    MS_POLL();
    bool r = mdm && rdof_sdrmodem_pull_iq(mdm, pos, iq, n);
    GIVE();
    return r;
}

/* A bench signal that renders I/Q only (AM) is on: set with the signal under the lock, read without it (the
 * LR2021's reader asks each turn). */
static volatile bool sig_iq;
static bool iq_now(void *ctx) { return sig_iq; }

static const rdof_fe_tx_source_t source = {pull_iq, pull_freq, NULL, try_pull_freq, iq_now, try_pull_iq};

/* A frequency no receive channel hears is still one the modem may transmit on (a bench job's, a bench.tx band's):
 * it hears nothing there. A -1 would NAK the SET_FREQ ("isn't one of the SDR's channels"). */
static int io_channel(void *ctx, uint32_t hz)
{
    const int c = sdr_rx_channel(hz);
    return c >= 0 ? c : RDOF_SDRMODEM_NOT_HEARD;
}

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
    m->tx_late48 = llround(c->tx_late_us * 48 / 1000);
    TAKE();
    mdm = m;
    GIVE();
    fe.ops->tx_set(fe.ctx, &source, power_dbm);
    return NULL;
}

#if CONFIG_RDOF_ROAMER
/* The roamer's power at the antenna: its tune's (the frequency's power_dbm as SET_FREQ's level), at most its
 * device's, under a routed bench job's cap. */
static double roamer_dbm(void)
{
    double d = owner ? owner->cfg->tx_power_dbm : 0;
    if (!isnan(level_dbm) && level_dbm < d)
        d = level_dbm;
    return isnan(cap_dbm) || d <= cap_dbm ? d : cap_dbm;
}
#endif

void sdr_modem_tx_cap(double dbm)
{
    cap_dbm = dbm;
#if CONFIG_RDOF_ROAMER
    TAKE();
    const bool roamers = owner && mdm && !parked && !bench_reply; /* the roamer's modem is on the front end */
    GIVE();
    if (roamers)
        fe.ops->tx_set(fe.ctx, &source, roamer_dbm());
#endif
}

/* The roamer's modem back on the front end after a borrow: what it had queued dropped, unkeyed, on its frequency and
 * power again. */
static void unpark(void)
{
    if (!parked)
        return;
    TAKE();
    rdof_sdrmodem_t *m = parked;
    parked = NULL;
    m->keyed = m->unkey = false;
    m->qn[0] = m->qn[1] = 0;
    m->slot_k = -1;
    double off;
    if (m->tx_hz && !io_tx_tune(NULL, m->tx_hz, &off))
        ESP_LOGW(TAG, "the roamer's %.5f MHz didn't tune again", m->tx_hz / 1e6);
    mdm = m;
    GIVE();
#if CONFIG_RDOF_ROAMER
    fe.ops->tx_set(fe.ctx, &source, roamer_dbm());
#endif
}

void sdr_modem_retuned(double center_hz)
{
    if (!lock)
        return;
    TAKE();
    center = center_hz;
    rdof_sdrmodem_t *m = mdm;
    double off;
    if (m && m->tx_hz && !io_tx_tune(NULL, m->tx_hz, &off))
        ESP_LOGW(TAG, "%.5f MHz can't be sent from the receiver's %.5f MHz", m->tx_hz / 1e6, center_hz / 1e6);
    GIVE();
}

/* A bench job comes first: a roamer's modem is parked for it whether the job borrows or not, and a call it is
 * sending is cut (sdr_modem_bench_open). Only another bench job on the modem, a roamer still starting, or a front end
 * that can't, says no. */
const char *sdr_modem_bench_borrow_why(bool borrow)
{
    (void)borrow;
    if (bench_reply)
        return "the SDR's modem is in use by another bench job";
#if CONFIG_RDOF_ROAMER
    if (owner && !mdm && !parked) /* only while it starts (the seconds after boot): its modem_up would take over */
        return "the roamer's SDR modem isn't up yet";
#endif
    return frontend_why();
}

const char *sdr_modem_bench_why(void) { return sdr_modem_bench_borrow_why(false); }

static void bench_io_reply(void *ctx, const uint8_t *frame, size_t len)
{
    if (bench_reply)
        bench_reply(bench_ctx, frame, len);
}

const char *sdr_modem_bench_open(void (*reply)(void *ctx, const uint8_t *frame, size_t len), void *ctx,
                                 double power_dbm, bool borrow)
{
    if (!lock)
        lock = xSemaphoreCreateMutex();
    TAKE();
    const char *why = sdr_modem_bench_borrow_why(borrow);
    if (!why && mdm) { /* the roamer's: parked, the front end the bench's; a call it was sending cut */
        if (rdof_sdrmodem_transmitting(mdm))
            ESP_LOGI(TAG, "a bench job cuts the roamer's transmission");
        rdof_sdrmodem_preempt(mdm);
        parked = mdm;
        mdm = NULL;
    }
    GIVE();
    if (why)
        return why;
    if (parked)
        fe.ops->tx_set(fe.ctx, NULL, 0);
    bench_reply = reply;
    bench_ctx = ctx;
    const rdof_sdrmodem_io_t io = {bench_io_reply, io_channel, io_tx_tune, NULL, NULL, NULL}; /* power: the job's */
    if ((why = modem_up(&io, power_dbm)) != NULL) {
        bench_reply = NULL;
        unpark();
        return why;
    }
    ESP_LOGI(TAG, "the bench holds the SDR modem (%s, %.0f dBm%s)", fe.caps->kind, power_dbm,
             parked ? ", the roamer's parked" : "");
    return NULL;
}

bool sdr_modem_bench_slots(sdr_modem_slots_t *s)
{
    TAKE();
    const rdof_sdrmodem_t *m = bench_reply ? mdm : NULL;
    if (m)
        *s = (sdr_modem_slots_t){.keyed = m->keyed, .clock = m->slot_k >= 0, .base = m->base, .slot_k = m->slot_k,
                                 .key_k = m->key_k, .skips = m->slot_skips, .skip_from = m->skip_from,
                                 .skip_to = m->skip_to, .slot = (double)rdof_mod_slot_samples(&m->mod),
                                 .tx_fs = m->tx_fs};
    GIVE();
    return m != NULL;
}

const char *sdr_modem_bench_signal(uint32_t hz, const rdof_sig_src_t *src, char *why, size_t cap)
{
    const bool iq_only = src && src->iq && !src->freq;
    if (src && fe.caps->tx_mode == RDOF_FE_TX_FREQ && iq_only && !fe.caps->tx_polar) {
        snprintf(why, cap, "the %s sends frequency only: no amplitude (AM)", fe.caps->kind);
        return why;
    }
    if (src && fe.caps->tx_mode == RDOF_FE_TX_FREQ && !iq_only && fe.caps->tx_dev_max_hz > 0 &&
        src->peak_hz > fe.caps->tx_dev_max_hz) {
        snprintf(why, cap, "the %s sends at most %.0f Hz of deviation (this source peaks at %.0f Hz)", fe.caps->kind,
                 fe.caps->tx_dev_max_hz, src->peak_hz);
        return why;
    }
    TAKE();
    const char *no = !mdm || !bench_reply ? "the bench doesn't hold the SDR modem" : NULL;
    if (!no && !rdof_sdrmodem_signal(mdm, hz, src, why, cap))
        no = why;
    sig_iq = !no && mdm && rdof_sdrmodem_signal_iq(mdm);
    GIVE();
    if (no && no != why)
        snprintf(why, cap, "%s", no);
    return no ? why : NULL;
}

int sdr_modem_bench_signal_state(void)
{
    TAKE();
    const int s = mdm && bench_reply ? rdof_sdrmodem_signal_state(mdm) : -1;
    GIVE();
    return s;
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
#if CONFIG_RDOF_BENCH_TX && RDOF_FEATURE_DATA_BLOCKS
    rdof_mssim_disarm(&ms);
#endif
    GIVE();
    if (m) {
        rdof_sdrmodem_free(m);
        free(m);
    }
    const bool back = parked != NULL;
    unpark();
    ESP_LOGI(TAG, "the bench let the SDR modem go%s", back ? ": the roamer's back" : "");
}

#if CONFIG_RDOF_BENCH_TX && RDOF_FEATURE_DATA_BLOCKS
/* Armed on the bench's modem once it's set up (its frequency and mode): false, not yet. */
bool sdr_modem_bench_mssim_arm(const rdof_mssim_profile_t *p, int channel)
{
    TAKE();
    const bool ok = mdm && bench_reply && mdm->mod_ok;
    if (ok) {
        rdof_mssim_init(&ms, ms_send, NULL);
        ms_ch = channel;
        rdof_mssim_arm(&ms, p);
    }
    GIVE();
    return ok;
}

void sdr_modem_bench_mssim_disarm(void)
{
    if (!lock)
        return;
    TAKE();
    rdof_mssim_disarm(&ms);
    GIVE();
}

/* A copy for the status (the log's JSON written outside the lock) */
void sdr_modem_bench_mssim_copy(rdof_mssim_t *out)
{
    if (lock)
        TAKE();
    *out = ms;
    if (lock)
        GIVE();
}
#endif

#if CONFIG_RDOF_ROAMER

static void io_reply(void *ctx, const uint8_t *frame, size_t len) { modem_on_bytes(owner, frame, len); }

/* SET_FREQ's RF level: a level of the device's map (rdof_txpower_sdr_map: 255 its tx.power_dbm). Called in the
 * modem's write, the lock held. */
static void io_tx_level(void *ctx, int level)
{
    const rdof_txmap_t m = rdof_txpower_sdr_map(owner->cfg->tx_power_dbm);
    level_dbm = rdof_txpower_dbm(&m, level);
    if (!parked && !bench_reply)
        fe.ops->tx_set(fe.ctx, &source, roamer_dbm());
}

static bool sdr_write(void *ctx, const uint8_t *data, size_t len)
{
    TAKE();
    rdof_sdrmodem_write(parked ? parked : mdm, data, len);
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
        const rdof_sdrmodem_io_t io = {io_reply, io_channel, io_tx_tune, NULL, NULL, io_tx_level};
        if (r->cfg->slot_timing.utc) /* the repeater's own slot_timing over the SDR's */
            slot_timing_setup(&r->cfg->slot_timing);
        if ((why = modem_up(&io, roamer_dbm())) != NULL) {
            fail(r, why);
        } else {
            r->sdr = (modem_transport_t){"sdr", sdr_write, r};
            modem_transport_up(r, &r->sdr);
            ESP_LOGI(TAG, "%s: %s duplex on the %s, at most %.1f dBm at the antenna", r->cfg->name,
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
    level_dbm = NAN;
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
