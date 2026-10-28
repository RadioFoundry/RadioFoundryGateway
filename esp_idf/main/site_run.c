// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The call router on the board (rdof/siteroute.h), for a version 3 document: the site, parsed once at boot, and the
 * router's state, in PSRAM, under one lock (the receivers' tasks, the HBlink tasks and the radios' tasks all ask).
 *
 * One identity (hblink identity "box", the default): every path is the box's peer. The first roamer or repeater
 * with HBlink owns the socket (site_hb_owner); the others, and the SDR's monitored channels, send through it.
 * With identity per_path each role and channel is its own peer, as in version 2.
 */
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "rdof/siteroute.h"

static const char *TAG = "site";

static rdof_site_t *S;
static rdof_siteroute_t *SR;
static SemaphoreHandle_t lock;
static int hblink = -1, parrot = -1;
static roam_t *owner;

static void *ps_alloc(size_t n)
{
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : calloc(1, n);
}

static double mono(void) { return esp_timer_get_time() / 1e6; }

void site_load(const char *text, size_t len)
{
    if (!rdof_site_is_v3(text, len))
        return;
    char err[160];
    rdof_site_t *s = ps_alloc(sizeof *s);
    rdof_siteroute_t *r = ps_alloc(sizeof *r);
    if (!s || !r || !rdof_site_parse(text, len, s, err, sizeof err)) {
        ESP_LOGE(TAG, "version 3: %s; no call router", s && r ? err : "no memory");
        free(s);
        free(r);
        return;
    }
    rdof_siteroute_init(r, s);
    for (int k = 0; k < s->nnet; k++)
        if (s->net[k].type == RDOF_NET_HBLINK && hblink < 0)
            hblink = k;
    parrot = rdof_site_app_of(s, RDOF_APP_PARROT);
    lock = xSemaphoreCreateRecursiveMutex(); /* the transceiver answers a tune made here through site_tuned */
    S = s;
    SR = r;
    ESP_LOGI(TAG, "%s: %d frequencies, %d talk paths, %d apps, %d networks", s->name, s->nfreq, s->npath, s->napp,
             s->nnet);
}

bool site_on(void) { return SR != NULL; }

bool site_shared_identity(void) { return SR && hblink >= 0 && !S->net[hblink].per_path; }

uint32_t site_id(void) { return S ? S->id : 0; }

void site_add_role(roam_t *r)
{
    if (!SR)
        return;
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
#if RDOF_FEATURE_REPEATER
    if (r->cfg->mode == RDOF_CFG_REPEATER && r->rpt && (r->cfg->sfr || r->cfg->dcdm))
        rdof_siteroute_add_repeater_freq(SR, rdof_site_freq_hz(S, r->cfg->tx_freq), r->rpt);
    else if (r->cfg->mode == RDOF_CFG_REPEATER && r->rpt)
        for (int a = 0; a < S->napp; a++)
            if (S->app[a].type == RDOF_APP_REPEATER && S->freq[S->app[a].in].hz == r->cfg->rx_freq)
                rdof_siteroute_add_repeater(SR, a, r->rpt);
#endif
#if RDOF_FEATURE_TX
    if (r->cfg->mode == RDOF_CFG_ROAMER && r->txvr) {
        uint64_t freqs = 0; /* the site's frequencies among its channels */
        for (int i = 0; i < r->cfg->nchannels; i++) {
            const int f = rdof_site_freq_hz(S, r->cfg->channels[i].freq);
            if (f >= 0)
                freqs |= 1ull << f;
        }
        if (freqs)
            rdof_siteroute_add_txvr(SR, r->txvr, freqs);
    }
#endif
    xSemaphoreGiveRecursive(lock);
}

roam_t *site_hb_owner(roam_t *r)
{
    if (!site_shared_identity())
        return r;
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
    if (!owner)
        owner = r;
    roam_t *o = owner;
    xSemaphoreGiveRecursive(lock);
    return o;
}

roam_t *site_hb_peer(void) { return site_shared_identity() ? owner : NULL; }

bool site_rf(uint32_t hz, int slot, uint32_t src, uint32_t dst, bool group)
{
    if (!SR)
        return true;
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
    const int net = rdof_siteroute_rf(SR, hz, slot, src, dst, group, mono());
    const char *why = SR->why;
    xSemaphoreGiveRecursive(lock);
    if (net < 0 && why)
        ESP_LOGD(TAG, "%.5f MHz slot %d, %lu > %lu: %s", hz / 1e6, slot, (unsigned long)src, (unsigned long)dst, why);
    return net >= 0 && net == hblink;
}

bool site_reaches_parrot(uint32_t hz, int slot)
{
    return !SR || rdof_siteroute_reaches(SR, hz, slot, parrot);
}

bool site_dmrd(uint32_t peer, const uint8_t *d, size_t n)
{
    if (!SR || hblink < 0)
        return false;
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
    const bool taken = rdof_siteroute_dmrd(SR, hblink, peer, d, n, mono());
    xSemaphoreGiveRecursive(lock);
    return taken;
}

bool site_tuned(const uint8_t sid[4])
{
    if (!SR)
        return false;
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
    const bool t = rdof_siteroute_tuned(SR, sid);
    xSemaphoreGiveRecursive(lock);
    return t;
}

size_t site_resolved(char *out, size_t cap)
{
    return S ? rdof_site_resolved_text(S, out, cap) : 0;
}
