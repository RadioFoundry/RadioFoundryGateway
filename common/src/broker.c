// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The broker: devices' resources, their owners, and the settings check (rdof/broker.h). */
#include "rdof/broker.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/config.h"

const rdof_platform_t rdof_platform_board = {
    .name = "this board",
    .max_talkers = RDOF_MAX_RADIOS,
    .max_sdrs = 1,
    .path_ports = false,
    .uart_ports = true,
    .sdr_split_half = true, /* the board's SDR as a split's RX half (split_port.c) */
    .sdr_drivers_host = false,
};

const rdof_platform_t rdof_platform_host = {
    .name = "this host",
    .max_talkers = RDOF_SETUP_MAX_ROLES,
    .max_sdrs = RDOF_SETUP_MAX_DEVICES,
    .path_ports = true,
    .uart_ports = false,
    .sdr_split_half = true, /* rdofradio --split-tx: the split's TIME clock */
    .sdr_drivers_host = true,
};

void rdof_broker_default_caps(const rdof_device_t *d, rdof_dev_caps_t *c)
{
    memset(c, 0, sizeof *c);
    if (d->kind != RDOF_DEV_SDR) {
        c->rx = c->tx = true;
        c->full_duplex = d->kind == RDOF_DEV_MODEM && strcmp(d->driver, "radio_hotspot");
        return;
    }
    c->rx = true;
    c->rx_channels = RDOF_BROKER_MAX_CHANNELS;
    if (!strcmp(d->driver, "rtlsdr")) {
        c->freq_min = 24000000, c->freq_max = 1766000000;
        c->rx_rate_max = 3.2e6;
        return;
    }
    /* the rest may transmit; one carrier each until more are tried on the air (section 5) */
    c->tx = true;
    c->tx_carriers = 1;
    c->full_duplex = strcmp(d->driver, "lr2021") != 0; /* the LR2021: one chain, switched */
}

void rdof_broker_init(rdof_broker_t *b, const rdof_setup_t *setup, const rdof_platform_t *plat)
{
    memset(b, 0, sizeof *b);
    b->setup = setup;
    b->plat = *plat;
    for (int k = 0; k < setup->ndev; k++)
        rdof_broker_default_caps(&setup->dev[k], &b->dev[k].caps);
}

void rdof_broker_set_caps(rdof_broker_t *b, int dev, const rdof_dev_caps_t *caps)
{
    if (dev >= 0 && dev < b->setup->ndev)
        b->dev[dev].caps = *caps;
}

static const char *owner_name(const rdof_broker_t *b, int owner, char *buf, size_t cap)
{
    if (RDOF_OWNER_IS_ROLE(owner))
        return b->setup->role[owner - 1].name;
    snprintf(buf, cap, "bench session %d", owner - RDOF_OWNER_BENCH(0));
    return buf;
}

static bool on_bench(const rdof_setup_t *s, int dev)
{
    for (int k = 0; k < s->nbench; k++)
        if (s->bench[k] == dev)
            return true;
    return false;
}

/* A channel on an SDR for freq: shared with whoever hears it already. false: none left. */
static bool take_channel(rdof_broker_dev_t *d, uint32_t f)
{
    for (int k = 0; k < d->channels; k++)
        if (d->chan_freq[k] + 100 >= f && f + 100 >= d->chan_freq[k])
            return true;
    if (d->channels >= d->caps.rx_channels || d->channels >= RDOF_BROKER_MAX_CHANNELS)
        return false;
    d->chan_freq[d->channels++] = f;
    return true;
}

static int carriers_of(const rdof_broker_t *b, int dev)
{
    int n = b->setup->dev[dev].tx_carriers;
    return n < b->dev[dev].caps.tx_carriers ? n : b->dev[dev].caps.tx_carriers;
}

static int take_carrier(rdof_broker_t *b, int dev, uint32_t f, int owner)
{
    rdof_broker_dev_t *d = &b->dev[dev];
    for (int k = 0; k < carriers_of(b, dev); k++)
        if (!d->carrier_owner[k]) {
            d->carrier_owner[k] = owner;
            d->carrier_freq[k] = f;
            return k;
        }
    return -1;
}

static bool err(char *error, size_t cap, const char *fmt, ...)
{
    if (error && cap) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(error, cap, fmt, ap);
        va_end(ap);
    }
    return false;
}

/* The port checks that are the platform's. */
static bool port_ok(const rdof_broker_t *b, int dev, char *error, size_t cap)
{
    const rdof_device_t *d = &b->setup->dev[dev];
    if (d->kind == RDOF_DEV_SDR) {
        if (!b->plat.sdr_drivers_host && (!strcmp(d->driver, "soapy") || !strcmp(d->driver, "iio")))
            return err(error, cap, "devices.%s.driver: %s is the host tool's, not %s's", d->name, d->driver,
                       b->plat.name);
        return true;
    }
    if (d->port.kind == RDOF_PORT_PATH && !b->plat.path_ports)
        return err(error, cap, "devices.%s.port: a serial path is for a host; %s has uart and usb ports", d->name,
                   b->plat.name);
    if (d->port.kind == RDOF_PORT_UART && !b->plat.uart_ports)
        return err(error, cap, "devices.%s.port: %s has no UARTs of its own (give the serial device's path)",
                   d->name, b->plat.name);
    return true;
}

static int sdrs_in_use(const rdof_broker_t *b)
{
    int n = 0;
    for (int k = 0; k < b->setup->ndev; k++) {
        const rdof_broker_dev_t *d = &b->dev[k];
        bool used = d->channels > 0 || d->exclusive;
        for (int c = 0; c < RDOF_BROKER_MAX_CARRIERS; c++)
            used |= d->carrier_owner[c] != 0;
        n += b->setup->dev[k].kind == RDOF_DEV_SDR && used;
    }
    return n;
}

static bool claim_whole(rdof_broker_t *b, int dev, int owner, char *error, size_t cap)
{
    rdof_broker_dev_t *d = &b->dev[dev];
    char n1[32], n2[32];
    if (d->owner && d->owner != owner)
        return err(error, cap, "devices.%s: both %s and %s take it", b->setup->dev[dev].name,
                   owner_name(b, d->owner, n1, sizeof n1), owner_name(b, owner, n2, sizeof n2));
    d->owner = owner;
    return true;
}

bool rdof_broker_claim_role(rdof_broker_t *b, int role, char *error, size_t cap)
{
    const rdof_setup_t *s = b->setup;
    const rdof_role_t *r = &s->role[role];
    const int owner = RDOF_OWNER_ROLE(role);
    for (int k = 0; k < 2; k++) {
        int dev = k ? r->tx : r->rx;
        if (dev < 0 || (k && dev == r->rx))
            continue;
        if (!port_ok(b, dev, error, cap))
            return false;
        if (on_bench(s, dev) && s->dev[dev].kind != RDOF_DEV_SDR)
            return err(error, cap, "devices.%s: on the bench and %s takes it: one or the other", s->dev[dev].name,
                       r->name);
    }
    const rdof_device_t *rx = &s->dev[r->rx];
    if (r->split && rx->kind == RDOF_DEV_SDR && !b->plat.sdr_split_half)
        return err(error, cap, "roles.%s: an SDR as a split's RX half isn't done on %s yet (section 6)", r->name,
                   b->plat.name);
    /* receive */
    if (rx->kind == RDOF_DEV_SDR) {
        rdof_broker_dev_t *d = &b->dev[r->rx];
        if (!d->caps.rx)
            return err(error, cap, "devices.%s: it can't receive", rx->name);
        for (int i = 0; i < r->nfreqs; i++) {
            uint32_t f = r->freqs[i];
            if (d->caps.freq_max && (f < d->caps.freq_min || f > d->caps.freq_max))
                return err(error, cap, "roles.%s: devices.%s can't hear %.5f MHz", r->name, rx->name, f / 1e6);
            if (rx->rx_center && rx->rx_rate) {
                double off = (double)f - rx->rx_center;
                if (off < -rx->rx_rate / 2 || off > rx->rx_rate / 2)
                    return err(error, cap, "roles.%s: %.5f MHz is outside devices.%s's window (%.5f MHz, %.0f S/s)",
                               r->name, f / 1e6, rx->name, rx->rx_center / 1e6, rx->rx_rate);
            }
            if (!take_channel(d, f))
                return err(error, cap, "devices.%s: no receive channel left for %s's %.5f MHz (%d at most)",
                           rx->name, r->name, f / 1e6, d->caps.rx_channels);
        }
        if (sdrs_in_use(b) > b->plat.max_sdrs)
            return err(error, cap, "roles.%s: devices.%s would be SDR %d, and %s runs %d", r->name, rx->name,
                       sdrs_in_use(b), b->plat.name, b->plat.max_sdrs);
    } else if (!claim_whole(b, r->rx, owner, error, cap)) {
        return false;
    }
    if (r->type == RDOF_ROLE_MONITOR)
        return true;
    /* transmit */
    const rdof_device_t *tx = &s->dev[r->tx];
    if (tx->kind == RDOF_DEV_SDR) {
        rdof_broker_dev_t *d = &b->dev[r->tx];
        if (!d->caps.tx)
            return err(error, cap, "devices.%s: the %s can't transmit", tx->name, tx->driver);
        if (tx->tx_carriers > d->caps.tx_carriers)
            return err(error, cap, "devices.%s.tx.carriers: %d, and the %s sends %d (one carrier a transmitter for "
                                   "now)", tx->name, tx->tx_carriers, tx->driver, d->caps.tx_carriers);
        if (r->type == RDOF_ROLE_REPEATER && !d->caps.full_duplex)
            return err(error, cap, "roles.%s: devices.%s (%s) is half duplex: one chain, switched; a duplex "
                                   "repeater needs RX while it transmits", r->name, tx->name, tx->driver);
        if (r->type == RDOF_ROLE_DCDM && r->sdr_duplex_full && !d->caps.full_duplex)
            return err(error, cap, "roles.%s: duplex full, and devices.%s (%s) is half duplex", r->name, tx->name,
                       tx->driver);
        uint32_t f = r->tx_freq ? r->tx_freq : r->nfreqs ? r->freqs[0] : 0;
        if (d->caps.freq_max && f && (f < d->caps.freq_min || f > d->caps.freq_max))
            return err(error, cap, "roles.%s: the %s can't transmit on %.5f MHz", r->name, tx->driver, f / 1e6);
        if (take_carrier(b, r->tx, f, owner) < 0) {
            char names[96] = "";
            size_t o = 0;
            for (int k = 0; k < carriers_of(b, r->tx); k++)
                o += (size_t)snprintf(names + o, o < sizeof names ? sizeof names - o : 0, "%s%s", k ? ", " : "",
                                      RDOF_OWNER_IS_ROLE(d->carrier_owner[k])
                                          ? s->role[d->carrier_owner[k] - 1].name : "the bench");
            return err(error, cap, "devices.%s: carrier %d of %d (%s transmits on it already)", tx->name,
                       carriers_of(b, r->tx) + 1, carriers_of(b, r->tx), names);
        }
    } else if (r->tx != r->rx && !claim_whole(b, r->tx, owner, error, cap)) {
        return false;
    }
    return true;
}

/* Does SDR dev hear f now (a channel on it), or could its window take it? */
static int rx_fit(const rdof_broker_t *b, int dev, uint32_t f)
{
    const rdof_device_t *sd = &b->setup->dev[dev];
    const rdof_broker_dev_t *d = &b->dev[dev];
    if (sd->kind != RDOF_DEV_SDR || d->unavailable || !d->caps.rx)
        return 0;
    if (d->caps.freq_max && (f < d->caps.freq_min || f > d->caps.freq_max))
        return 0;
    for (int k = 0; k < d->channels; k++)
        if (d->chan_freq[k] + 100 >= f && f + 100 >= d->chan_freq[k])
            return 4; /* a shared channel on it already */
    if (d->channels >= d->caps.rx_channels)
        return 0;
    if (sd->rx_center && sd->rx_rate) {
        double off = (double)f - sd->rx_center;
        return off >= -sd->rx_rate / 2 && off <= sd->rx_rate / 2 ? 3 : 0; /* a free channel in its window */
    }
    if (d->channels > 0 || d->exclusive)
        return 2; /* running, its window may take it ("auto") */
    return 1;     /* idle: it may be tuned */
}

int rdof_broker_claim(rdof_broker_t *b, const rdof_claim_t *req, int owner, char *error, size_t cap)
{
    const rdof_setup_t *s = b->setup;
    int best = -1, best_score = 0;
    bool any = false;
    for (int k = 0; k < s->ndev; k++) {
        if (req->device >= 0 && k != req->device)
            continue;
        const rdof_device_t *d = &s->dev[k];
        const rdof_broker_dev_t *bd = &b->dev[k];
        int score = 0;
        if (req->dir == RDOF_DIR_RX) {
            if (d->kind == RDOF_DEV_SDR) {
                score = rx_fit(b, k, req->freq);
                if (req->exclusive && (bd->channels || (bd->exclusive && bd->exclusive != owner)))
                    score = score ? -1 : 0;
            } else if (bd->caps.rx) {
                score = bd->owner && bd->owner != owner ? -1 : 1;
            }
        } else {
            if (d->kind == RDOF_DEV_SDR && bd->caps.tx && d->tx) {
                bool free_ = false;
                for (int c = 0; c < carriers_of(b, k); c++)
                    free_ |= !bd->carrier_owner[c];
                score = free_ ? 3 : -1;
            } else if (d->kind == RDOF_DEV_MODEM) {
                score = bd->owner && bd->owner != owner ? -1 : 2;
            } else if (d->kind == RDOF_DEV_RADIO) {
                score = bd->owner && bd->owner != owner ? -1 : 1;
            }
            /* a bench session takes only the bench's devices, and an SDR's free carrier */
            if (!RDOF_OWNER_IS_ROLE(owner) && d->kind != RDOF_DEV_SDR && !on_bench(s, k))
                score = 0;
        }
        if (bd->unavailable)
            score = 0;
        any |= score != 0;
        if (score > best_score) {
            best_score = score;
            best = k;
        }
    }
    if (best < 0) {
        if (!any) {
            err(error, cap, "no device %s %.5f MHz", req->dir == RDOF_DIR_RX ? "hears" : "transmits on",
                req->freq / 1e6);
            return -2;
        }
        return -1;
    }
    rdof_broker_dev_t *d = &b->dev[best];
    if (s->dev[best].kind == RDOF_DEV_SDR) {
        if (req->dir == RDOF_DIR_TX)
            take_carrier(b, best, req->freq, owner);
        else if (req->exclusive)
            d->exclusive = owner;
        else
            take_channel(d, req->freq);
    } else {
        d->owner = owner;
    }
    return best;
}

void rdof_broker_release(rdof_broker_t *b, int owner)
{
    for (int k = 0; k < b->setup->ndev; k++) {
        rdof_broker_dev_t *d = &b->dev[k];
        if (d->owner == owner)
            d->owner = 0;
        if (d->exclusive == owner)
            d->exclusive = 0;
        for (int c = 0; c < RDOF_BROKER_MAX_CARRIERS; c++)
            if (d->carrier_owner[c] == owner)
                d->carrier_owner[c] = 0;
    }
}

bool rdof_broker_check(const rdof_setup_t *setup, const rdof_platform_t *plat, char *error, size_t cap)
{
    rdof_broker_t *b = malloc(sizeof *b);
    if (!b)
        return err(error, cap, "no memory");
    rdof_broker_init(b, setup, plat);
    bool ok = true;
    int talkers = 0;
    for (int k = 0; ok && k < setup->nrole; k++) {
        if (!setup->role[k].run)
            continue;
        if (setup->role[k].type != RDOF_ROLE_MONITOR && ++talkers > plat->max_talkers)
            ok = err(error, cap, "roles.%s: %s runs %d roamers or repeaters at most", setup->role[k].name,
                     plat->name, plat->max_talkers);
        else
            ok = rdof_broker_claim_role(b, k, error, cap);
    }
    free(b);
    return ok;
}

/* ── auto ──────────────────────────────────────────────────────────────────── */

bool rdof_broker_resolve_auto(rdof_setup_t *s, char *error, size_t cap)
{
    rdof_broker_t *b = malloc(sizeof *b);
    if (!b)
        return err(error, cap, "no memory");
    rdof_broker_init(b, s, &rdof_platform_host);
    /* the named devices first, so auto takes what's left */
    for (int k = 0; k < s->nrole; k++) {
        rdof_role_t *r = &s->role[k];
        if (r->rx >= 0 && s->dev[r->rx].kind != RDOF_DEV_SDR)
            b->dev[r->rx].owner = RDOF_OWNER_ROLE(k);
        if (r->tx >= 0 && s->dev[r->tx].kind != RDOF_DEV_SDR)
            b->dev[r->tx].owner = RDOF_OWNER_ROLE(k);
        for (int i = 0; r->rx >= 0 && s->dev[r->rx].kind == RDOF_DEV_SDR && i < r->nfreqs; i++)
            take_channel(&b->dev[r->rx], r->freqs[i]);
        if (r->tx >= 0 && r->type != RDOF_ROLE_MONITOR && s->dev[r->tx].kind == RDOF_DEV_SDR)
            take_carrier(b, r->tx, r->tx_freq, RDOF_OWNER_ROLE(k));
    }
    bool ok = true;
    for (int k = 0; ok && k < s->nrole; k++) {
        rdof_role_t *r = &s->role[k];
        bool rx_auto = r->rx == -2, tx_auto = r->tx == -2;
        if (!rx_auto && !tx_auto)
            continue;
        const int owner = RDOF_OWNER_ROLE(k);
        uint32_t f = r->nfreqs ? r->freqs[0] : 0;
        if (rx_auto && tx_auto && r->type != RDOF_ROLE_MONITOR) {
            /* "modem": auto, one device for both: an SDR that transmits, else a free modem */
            int pick = -1;
            for (int d = 0; d < s->ndev && pick < 0; d++)
                if (s->dev[d].kind == RDOF_DEV_SDR && s->dev[d].tx && rx_fit(b, d, f) > 0 &&
                    take_carrier(b, d, r->tx_freq ? r->tx_freq : f, owner) >= 0)
                    pick = d;
            for (int d = 0; d < s->ndev && pick < 0; d++)
                if (s->dev[d].kind == RDOF_DEV_MODEM && !b->dev[d].owner && !on_bench(s, d)) {
                    b->dev[d].owner = owner;
                    pick = d;
                }
            if (pick < 0) {
                ok = err(error, cap, "roles.%s.modem: auto, and no device is free for it (an SDR with tx, or a "
                                     "modem)", r->name);
                break;
            }
            r->rx = r->tx = pick;
            for (int i = 0; s->dev[pick].kind == RDOF_DEV_SDR && i < r->nfreqs; i++)
                take_channel(&b->dev[pick], r->freqs[i]);
            continue;
        }
        if (rx_auto) {
            int best = -1, score = 0;
            for (int d = 0; d < s->ndev; d++) {
                int sc = rx_fit(b, d, f);
                if (sc > score)
                    score = sc, best = d;
            }
            if (best < 0) {
                ok = err(error, cap, "roles.%s.rx: auto, and no SDR can hear %.5f MHz", r->name, f / 1e6);
                break;
            }
            r->rx = best;
            for (int i = 0; i < r->nfreqs; i++)
                take_channel(&b->dev[best], r->freqs[i]);
        }
        if (tx_auto) {
            rdof_claim_t q = {.dir = RDOF_DIR_TX, .device = -1, .freq = r->tx_freq ? r->tx_freq : f};
            int d = rdof_broker_claim(b, &q, owner, NULL, 0);
            for (int i = 0; d >= 0 && i < s->nbench; i++)
                if (s->bench[i] == d && s->dev[d].kind != RDOF_DEV_SDR)
                    d = -1;
            if (d < 0 || s->dev[d].kind == RDOF_DEV_RADIO) {
                ok = err(error, cap, "roles.%s.tx: auto, and no device is free to transmit on %.5f MHz", r->name,
                         q.freq / 1e6);
                break;
            }
            r->tx = d;
        }
    }
    free(b);
    return ok;
}
