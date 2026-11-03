// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The call router on the board (rdof/siteroute.h): the site (a copy of the settings' plan's, made at boot) and the
 * router's state, in PSRAM, under one lock (the receivers' tasks, the HBlink tasks and the radios' tasks all ask).
 *
 * One identity (hblink identity "box", the default): every path is the box's peer. The first roamer or repeater
 * with HBlink owns the socket (site_hb_owner); the others, and the SDR's monitored channels, send through it.
 * With identity per_path each unit and channel is its own peer.
 *
 * The box's parrot (docs/spec/fw/transmitters.md §5): with a parrot app and a transceiver in the router's pool, one
 * parrot hears every frequency (the SDR receiver's channels, a modem roamer's channel) and plays back on the path a
 * call was heard on, through the pool. A roamer then has no parrot of its own; a repeater keeps its own.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "rdof/bits.h"
#include "rdof/parrot.h"
#include "rdof/siteroute.h"

static const char *TAG = "site";

static rdof_site_t *S;
static rdof_siteroute_t *SR;
static SemaphoreHandle_t lock;
static int hblink = -1, parrot = -1;
static roam_t *owner;
#if RDOF_FEATURE_TX
static rdof_parrot_t *P;
static SemaphoreHandle_t plock; /* taken before lock, never while holding it */
static int ppath = -1;           /* the path the replay goes out on */
#endif

static void *ps_alloc(size_t n)
{
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : calloc(1, n);
}

static double mono(void) { return esp_timer_get_time() / 1e6; }

void site_load(const rdof_site_t *site)
{
    rdof_site_t *s = ps_alloc(sizeof *s);
    rdof_siteroute_t *r = ps_alloc(sizeof *r);
    if (!s || !r) {
        ESP_LOGE(TAG, "no memory for the call router");
        free(s);
        free(r);
        return;
    }
    *s = *site;
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

#if RDOF_FEATURE_TX
/* The box parrot's replay: the pool's transceiver, on the path the call was heard on. Called under plock. */
static void parrot_start(void *ctx, const uint8_t sid[4], int slot, uint32_t src, uint32_t dst, bool group)
{
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
    ppath = rdof_siteroute_path_at(SR, P->hz, slot);
    xSemaphoreGiveRecursive(lock);
    ESP_LOGI(TAG, "parrot: sending back %.5f MHz slot %d %lu > %s%lu, %d bursts", P->hz / 1e6, slot,
             (unsigned long)src, group ? "TG " : "", (unsigned long)dst, P->n);
}

static void parrot_send(void *ctx, const rdof_net_burst_t *b)
{
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
    rdof_siteroute_app_burst(SR, ppath, b, false, 255, mono());
    xSemaphoreGiveRecursive(lock);
}

static bool parrot_ready(void *ctx)
{
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
    const bool r = rdof_siteroute_app_ready(SR, rdof_siteroute_path_at(SR, P->hz, P->slot), mono());
    xSemaphoreGiveRecursive(lock);
    return r;
}

/* Kept when the path it's heard on reaches the parrot and isn't a repeater's (a repeater has its own parrot). */
static bool parrot_admit(void *ctx, int slot, uint32_t src, uint32_t dst, bool group)
{
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
    const int k = rdof_siteroute_path_at(SR, P->hz, slot);
    const bool r = k >= 0 && S->path[k].app < 0 && rdof_siteroute_reaches(SR, P->hz, slot, parrot);
    xSemaphoreGiveRecursive(lock);
    return r;
}

/* With the parrot id of the roamer whose transceiver joined the pool (its config's). */
static void parrot_setup(const roam_t *r)
{
    if (P || parrot < 0 || !r->cfg->has_parrot)
        return;
    rdof_parrot_t *p = ps_alloc(sizeof *p);
    if (!p) {
        ESP_LOGE(TAG, "parrot: no room for it (%u bytes)", (unsigned)sizeof *p);
        return;
    }
    rdof_parrot_init(p, r->cfg->parrot);
    p->start = parrot_start;
    p->send = parrot_send;
    p->ready = parrot_ready;
    p->admit = parrot_admit;
    plock = xSemaphoreCreateMutex();
    P = p;
    ESP_LOGI(TAG, "parrot: on, every frequency, %lu", (unsigned long)p->id);
}

bool site_parrot_on(void) { return P != NULL; }

void site_parrot_heard(uint32_t hz, int slot, uint8_t control, const uint8_t *burst)
{
    if (!P)
        return;
    xSemaphoreTake(plock, portMAX_DELAY);
    rdof_parrot_heard_at(P, hz, slot, control, burst, mono());
    xSemaphoreGive(plock);
}

void site_parrot_lost(uint32_t hz, int slot)
{
    if (!P)
        return;
    xSemaphoreTake(plock, portMAX_DELAY);
    rdof_parrot_lost_at(P, hz, slot, mono());
    xSemaphoreGive(plock);
}

void site_parrot_poll(void)
{
    if (!P)
        return;
    xSemaphoreTake(plock, portMAX_DELAY);
    rdof_parrot_poll(P, mono());
    xSemaphoreGive(plock);
}

bool site_pool_on(void) { return SR && SR->pool.n > 0; }

int site_app_path(uint32_t hz, int slot)
{
    if (!SR)
        return -1;
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
    const int k = rdof_siteroute_path_at(SR, hz, slot);
    xSemaphoreGiveRecursive(lock);
    return k;
}

bool site_app_burst(int path, const rdof_net_burst_t *b, bool take_hold, int power_max)
{
    if (!SR)
        return false;
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
    const bool sent = rdof_siteroute_app_burst(SR, path, b, take_hold, power_max, mono());
    xSemaphoreGiveRecursive(lock);
    return sent;
}

bool site_app_holds(const uint8_t sid[4])
{
    if (!SR)
        return false;
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
    const bool held = rdof_siteroute_holds(SR, sid);
    xSemaphoreGiveRecursive(lock);
    return held;
}

bool site_parrot_stream(const uint8_t sid[4])
{
    return P && (rdof_be32(sid) & 0xFFFF0000u) == RDOF_PARROT_STREAM;
}
#endif

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
#if RDOF_FEATURE_TX
    if (r->cfg->mode == RDOF_CFG_ROAMER && r->txvr)
        parrot_setup(r);
#endif
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

void site_status_json(rdof_json_t *j)
{
    if (!SR)
        return;
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
    const double now = mono();
    rdof_json_open(j, "router");
    rdof_json_int(j, "no_txvr", SR->no_txvr);
    rdof_json_int(j, "app_no_txvr", SR->app_no_txvr);
    rdof_json_int(j, "preempted", SR->pool.preempted);
    rdof_json_int(j, "held_taken", SR->pool.held_taken);
    rdof_json_open(j, "transmitters");
    for (int i = 0; i < SR->pool.n; i++) {
        const char k[2] = {(char)('0' + i), 0};
        const rdof_txp_state_t st = rdof_txpool_state(&SR->pool, i, now);
        const rdof_txp_tx_t *t = &SR->pool.tx[i];
        rdof_json_open(j, k);
        rdof_json_str(j, "state", rdof_txpool_state_name(st));
        if (st != RDOF_TXP_FREE && t->on.path >= 0 && t->on.path < S->npath) {
            char sid[9];
            snprintf(sid, sizeof sid, "%08lx", (unsigned long)t->on.stream);
            rdof_json_str(j, "path", S->path[t->on.path].name);
            rdof_json_str(j, "stream_id", sid);
        }
        rdof_json_close(j);
    }
    rdof_json_close(j);
#if RDOF_FEATURE_TX
    if (P) {
        rdof_json_open(j, "parrot");
        rdof_json_str(j, "state", P->state == RDOF_PARROT_RECORDING ? "recording"
                                  : P->state == RDOF_PARROT_WAITING ? "waiting"
                                  : P->state == RDOF_PARROT_REPLAYING ? "replaying" : "idle");
        rdof_json_int(j, "hz", P->hz);
        rdof_json_int(j, "gave_up", P->gave_up);
        rdof_json_close(j);
    }
#endif
    rdof_json_close(j);
    xSemaphoreGiveRecursive(lock);
}
