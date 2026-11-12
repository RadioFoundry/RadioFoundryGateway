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
#include <math.h>
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
#include "rdof/mqttnet.h"
#include "rdof/parrot.h"
#include "rdof/siteroute.h"

static const char *TAG = "site";

static rdof_site_t *S;
static rdof_siteroute_t *SR;
static SemaphoreHandle_t lock;
static int hblink = -1, parrot = -1, mqttnet = -1;
static char warning[160]; /* a setting this build doesn't carry (site_load), for the log and the page */
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
    for (int k = 0; k < s->nnet; k++)
        if (s->net[k].type == RDOF_NET_MQTT && s->net[k].calls && s->net[k].format == RDOF_MQTT_FORMAT_V3 &&
            mqttnet < 0)
            mqttnet = k;
    /* settings that pass the check and that this build doesn't carry: said here and on the page, where they used
     * to drop every call with nothing logged */
    for (int k = 0; k < s->nnet; k++) {
        if (s->net[k].type != RDOF_NET_MQTT || !s->net[k].calls)
            continue;
        if (s->net[k].format != RDOF_MQTT_FORMAT_V3)
            snprintf(warning, sizeof warning, "the mqtt network's calls in the dmrd format aren't carried by a board "
                                              "(rdofradio's are): set format v3");
#if !CONFIG_RDOF_ROAMER
        else
            snprintf(warning, sizeof warning, "the mqtt network's calls (format v3) need an image with the "
                                              "transceiver built in: this one is the receiver only");
#endif
    }
    if (warning[0])
        ESP_LOGE(TAG, "settings: %s. No call goes to or from that network", warning);
    parrot = rdof_site_app_of(s, RDOF_APP_PARROT);
    lock = xSemaphoreCreateRecursiveMutex(); /* the transceiver answers a tune made here through site_tuned */
    S = s;
    SR = r;
    ESP_LOGI(TAG, "%s: %d frequencies, %d talk paths, %d apps, %d networks", s->name, s->nfreq, s->npath, s->napp,
             s->nnet);
}

bool site_on(void) { return SR != NULL; }

const char *site_warning(void) { return warning; }

bool site_shared_identity(void) { return SR && hblink >= 0 && !S->net[hblink].per_path; }

/* With a site: whether a unit whose own path is on the HBlink network or not (on_net) is to run an HBlink peer. No
   HBlink network: none. One identity: the box's peer carries every path. A peer a path (identity per_path, a
   BrandMeister login): only the unit whose path is on that network, or a unit not on it would log in beside them
   with the box's ID. */
bool site_hb_wanted(bool on_net)
{
    if (!SR)
        return true; /* no site: the role's own settings */
    if (hblink < 0)
        return false;
    return on_net || !(S->net[hblink].per_path || S->net[hblink].login);
}

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
    rdof_siteroute_app_burst(SR, ppath, b, false, INFINITY, mono());
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

bool site_app_burst(int path, const rdof_net_burst_t *b, bool take_hold, double cap_dbm)
{
    if (!SR)
        return false;
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
    const bool sent = rdof_siteroute_app_burst(SR, path, b, take_hold, cap_dbm, mono());
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

bool site_app_refused(const uint8_t sid[4])
{
    if (!SR)
        return false;
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
    const bool refused = rdof_siteroute_refused(SR, sid);
    xSemaphoreGiveRecursive(lock);
    return refused;
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
            rdof_siteroute_add_txvr(SR, r->txvr, freqs, &r->cfg->tx_map);
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

int site_rf_net(uint32_t hz, int slot, uint32_t src, uint32_t dst, bool group)
{
    if (!SR)
        return -1;
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
    const int net = rdof_siteroute_rf(SR, hz, slot, src, dst, group, mono());
    const char *why = SR->why;
    xSemaphoreGiveRecursive(lock);
    if (net < 0 && why)
        ESP_LOGD(TAG, "%.5f MHz slot %d, %lu > %lu: %s", hz / 1e6, slot, (unsigned long)src, (unsigned long)dst, why);
    return net;
}

bool site_rf(uint32_t hz, int slot, uint32_t src, uint32_t dst, bool group)
{
    if (!SR)
        return true;
    const int net = site_rf_net(hz, slot, src, dst, group);
    return net >= 0 && net == hblink;
}

/* ── the MQTT network (mqttnet_task.c): the router's side, under its lock ── */

const rdof_site_t *site_get(void) { return S; }

int site_mn_net(void) { return SR ? mqttnet : -1; }

int site_mn_path_at(uint32_t hz, int slot)
{
    if (!SR || mqttnet < 0)
        return -1;
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
    const int k = rdof_siteroute_path_at(SR, hz, slot);
    xSemaphoreGiveRecursive(lock);
    return k >= 0 && S->path[k].pol.net == mqttnet ? k : -1;
}

bool site_mn_burst(uint64_t only, bool emergency, const rdof_net_burst_t *b)
{
    if (!SR || mqttnet < 0)
        return false;
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
    const uint64_t went = rdof_siteroute_burst_on(SR, mqttnet, only, emergency, b, mono());
    xSemaphoreGiveRecursive(lock);
    return went != 0;
}

int site_heard_path(uint32_t id)
{
    if (!SR)
        return -1;
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
    const int k = rdof_router_heard(&SR->router, id, mono());
    xSemaphoreGiveRecursive(lock);
    return k;
}

/* the parrot's state and counts, in its app's object; under plock (site_apps_json) */
static void apps_extra(void *ctx, int a, rdof_json_t *j)
{
    static const char *pstate[] = {"idle", "recording", "waiting", "replaying"};
    if (S->app[a].type != RDOF_APP_PARROT || !P)
        return;
    rdof_json_str(j, "state", pstate[P->state & 3]);
    rdof_json_int(j, "replayed", P->replayed);
    rdof_json_int(j, "ignored", P->ignored);
    rdof_json_int(j, "cut", P->cut);
    if (P->state & 3) {
        rdof_json_int(j, "src", P->src);
        rdof_json_int(j, "dst", P->dst);
    }
}

size_t site_apps_json(char *out, size_t cap)
{
    if (!SR || !S->napp) {
        if (cap)
            out[0] = 0;
        return 0;
    }
    if (P)
        xSemaphoreTake(plock, portMAX_DELAY); /* the parrot's, before lock (the order above) */
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
    const size_t n = rdof_router_apps_json(&SR->router, mono(), apps_extra, NULL, out, cap);
    xSemaphoreGiveRecursive(lock);
    if (P)
        xSemaphoreGive(plock);
    return n;
}

int site_mn_sub_tgs(uint32_t *tgs, int max, bool *all)
{
    *all = false;
    if (!SR || mqttnet < 0)
        return 0;
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
    const int n = rdof_mn_sub_tgs(S, mqttnet, &SR->router, mono(), tgs, max, all);
    xSemaphoreGiveRecursive(lock);
    return n;
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
