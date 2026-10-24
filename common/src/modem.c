// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* One MMDVM modem as a client, and the radio on it (rdof/modem.h), without threads or tuners other than the modem's own and fixed. */
#include "rdof/modem.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static double now_s(rdof_modem_t *m)
{
    return m->port.clock ? m->port.clock(m->port.ctx) : 0.0;
}

void rdof_modem_init(rdof_modem_t *m, const rdof_modem_port_t *port)
{
    memset(m, 0, sizeof *m);
    m->port = *port;
    m->protocol = 1;
    m->last_rx = -INFINITY;
    m->timeout_ms = 1000;
    m->waiting_for = -1;
    rdof_mmdvm_reader_init(&m->reader);
}

void rdof_modem_fail(rdof_modem_t *m, const char *why)
{
    snprintf(m->failed, sizeof m->failed, "%s", why ? why : "");
    if (m->waiting_for >= 0) {
        m->reply_ok = false;
        m->reply_ready = true;
        if (m->port.on_reply)
            m->port.on_reply(m->port.ctx);
    }
}

static void on_frame(void *ctx, uint8_t cmd, const uint8_t *p, size_t len)
{
    rdof_modem_t *m = ctx;
    if (cmd == RDOF_MMDVM_DMR_DATA1 || cmd == RDOF_MMDVM_DMR_LOST1 || cmd == RDOF_MMDVM_DMR_DATA2 ||
        cmd == RDOF_MMDVM_DMR_LOST2) {
        m->last_rx = now_s(m);
        int slot = (cmd == RDOF_MMDVM_DMR_DATA1 || cmd == RDOF_MMDVM_DMR_LOST1) ? 1 : 2;
        if (cmd == RDOF_MMDVM_DMR_LOST1 || cmd == RDOF_MMDVM_DMR_LOST2) {
            if (m->on_lost)
                m->on_lost(m->cb_ctx, slot);
        } else if (len >= 34) {
            int rssi = len >= 36 ? (p[34] << 8) | p[35] : -1;
            if (m->on_rx)
                m->on_rx(m->cb_ctx, slot, p[0], p + 1, rssi);
        }
        return;
    }
    int key;
    bool ok = true;
    if (cmd == RDOF_MMDVM_ACK || cmd == RDOF_MMDVM_NAK) {
        if (len == 0)
            return;
        key = p[0];
        ok = cmd == RDOF_MMDVM_ACK;
        if (rdof_mmdvm_unanswered((uint8_t)key)) {
            if (!ok && m->on_nak)
                m->on_nak(m->cb_ctx, (uint8_t)key, len > 1 ? p[1] : 0);
            return;
        }
        p++;
        len--;
    } else {
        key = cmd;
    }
    if (key != m->waiting_for)
        return; /* nobody's waiting for it (a reply that came too late) */
    m->reply_len = len < sizeof m->reply ? len : sizeof m->reply;
    memcpy(m->reply, p, m->reply_len);
    m->reply_ok = ok;
    m->reply_ready = true;
    if (m->port.on_reply)
        m->port.on_reply(m->port.ctx);
}

void rdof_modem_feed(rdof_modem_t *m, const uint8_t *data, size_t len)
{
    rdof_mmdvm_reader_feed(&m->reader, data, len, on_frame, m);
}

static bool write_frame(rdof_modem_t *m, uint8_t cmd, const uint8_t *payload, size_t len)
{
    uint8_t buf[260];
    if (m->failed[0]) {
        snprintf(m->error, sizeof m->error, "%s", m->failed);
        return false;
    }
    size_t n = rdof_mmdvm_frame(cmd, payload, len, buf, sizeof buf);
    if (!n || !m->port.write || !m->port.write(m->port.ctx, buf, n)) {
        if (!m->failed[0])
            rdof_modem_fail(m, "port write failed");
        snprintf(m->error, sizeof m->error, "%s", m->failed);
        return false;
    }
    return true;
}

static bool request(rdof_modem_t *m, uint8_t cmd, const uint8_t *payload, size_t len)
{
    m->reply_ready = false;
    m->waiting_for = cmd;
    m->requests++;
    if (!write_frame(m, cmd, payload, len)) {
        m->waiting_for = -1;
        return false;
    }
    bool came = m->reply_ready || (m->port.wait_reply && m->port.wait_reply(m->port.ctx, m->timeout_ms));
    came = came || m->reply_ready;
    m->waiting_for = -1;
    if (m->failed[0]) {
        snprintf(m->error, sizeof m->error, "%s", m->failed);
        return false;
    }
    if (!came) {
        m->no_reply++;
        snprintf(m->error, sizeof m->error, "no reply to command 0x%02x", cmd);
        return false;
    }
    if (!m->reply_ok) {
        m->refused++;
        snprintf(m->error, sizeof m->error, "modem refused command 0x%02x (error %d)", cmd,
                 m->reply_len ? m->reply[0] : -1);
        return false;
    }
    return true;
}

bool rdof_modem_start(rdof_modem_t *m, int tries)
{
    for (int i = 0; i < tries; i++) {
        if (request(m, RDOF_MMDVM_GET_VERSION, NULL, 0)) {
            m->protocol = rdof_mmdvm_parse_version(m->reply, m->reply_len, m->version, sizeof m->version);
            return true;
        }
        if (m->failed[0])
            return false;
    }
    return false;
}

bool rdof_modem_status(rdof_modem_t *m, rdof_mmdvm_status_t *out)
{
    if (!request(m, RDOF_MMDVM_GET_STATUS, NULL, 0))
        return false;
    if (!rdof_mmdvm_parse_status(m->protocol, m->reply, m->reply_len, out)) {
        snprintf(m->error, sizeof m->error, "short status reply");
        return false;
    }
    return true;
}

/* SET_CONFIG with extra flags ORed into its flags byte (RDOF_MMDVM_DCDM). */
static bool configure(rdof_modem_t *m, const rdof_modem_config_t *c, int cc, bool duplex, uint8_t extra)
{
    uint8_t p[40];
    size_t n = m->protocol >= 2 ? rdof_mmdvm_config_v2(c, cc, RDOF_MODE_DMR, duplex, p)
                                : rdof_mmdvm_config_v1(c, cc, RDOF_MODE_DMR, duplex, p);
    p[0] |= extra;
    return request(m, RDOF_MMDVM_SET_CONFIG, p, n);
}

bool rdof_modem_configure(rdof_modem_t *m, const rdof_modem_config_t *c, int cc, bool duplex)
{
    return configure(m, c, cc, duplex, 0);
}

bool rdof_modem_set_freq(rdof_modem_t *m, uint32_t rx_hz, uint32_t tx_hz, int rf_level)
{
    uint8_t p[14];
    size_t n = rdof_mmdvm_set_freq_payload(rx_hz, tx_hz, rf_level, p);
    return request(m, RDOF_MMDVM_SET_FREQ, p, n);
}

bool rdof_modem_set_mode(rdof_modem_t *m, int mode)
{
    uint8_t p = (uint8_t)mode;
    return request(m, RDOF_MMDVM_SET_MODE, &p, 1);
}

bool rdof_modem_send_cwid(rdof_modem_t *m, const char *text)
{
    uint8_t p[40];
    size_t n = 0;
    for (; *text && n < sizeof p; text++)
        if ((unsigned char)*text < 0x80)
            p[n++] = (uint8_t)toupper((unsigned char)*text);
    return request(m, RDOF_MMDVM_SEND_CWID, p, n);
}

bool rdof_modem_write_dmr(rdof_modem_t *m, uint8_t control, const uint8_t burst[RDOF_BURST_BYTES], int slot)
{
    uint8_t p[34];
    p[0] = control;
    memcpy(p + 1, burst, RDOF_BURST_BYTES);
    return write_frame(m, slot == 1 ? RDOF_MMDVM_DMR_DATA1 : RDOF_MMDVM_DMR_DATA2, p, sizeof p);
}

bool rdof_modem_dmr_start(rdof_modem_t *m, bool on)
{
    uint8_t p = on ? 0x01 : 0x00;
    return write_frame(m, RDOF_MMDVM_DMR_START, &p, 1);
}

bool rdof_modem_dmr_abort(rdof_modem_t *m, int slot)
{
    uint8_t p = (uint8_t)slot;
    return write_frame(m, RDOF_MMDVM_DMR_ABORT, &p, 1);
}

bool rdof_modem_dmr_short_lc(rdof_modem_t *m, const uint8_t lc9[9])
{
    return write_frame(m, RDOF_MMDVM_DMR_SHORTLC, lc9, 9);
}

/* ── the radio ─────────────────────────────────────────────────────────────── */

void rdof_radio_init(rdof_radio_t *r, rdof_modem_t *modem, rdof_radio_kind_t kind)
{
    memset(r, 0, sizeof *r);
    r->modem = modem;
    r->kind = kind;
    rdof_modem_config_default(&r->config);
    r->tuner = kind == RDOF_RADIO_MMDVM ? RDOF_TUNER_FIXED : RDOF_TUNER_MODEM;
    r->hotspot_rssi = kind == RDOF_RADIO_HOTSPOT || kind == RDOF_RADIO_SPLIT; /* a split's RX half: a radio */
    r->headers = kind == RDOF_RADIO_HOTSPOT ? 1 : 8;
    r->terminators = kind == RDOF_RADIO_HOTSPOT ? 1 : 3;
}

/* The points in use: n, within the arrays. */
static int points(const rdof_rssi_map_t *map)
{
    return map->n < 0 ? 0 : map->n > RDOF_RSSI_POINTS ? RDOF_RSSI_POINTS : map->n;
}

void rdof_rssi_map_sort(rdof_rssi_map_t *map)
{
    for (int i = 1; i < points(map); i++)
        for (int j = i; j > 0 && (map->raw[j - 1] > map->raw[j] ||
                                  (map->raw[j - 1] == map->raw[j] && map->dbm[j - 1] > map->dbm[j]));
             j--) {
            int t = map->raw[j];
            map->raw[j] = map->raw[j - 1];
            map->raw[j - 1] = t;
            t = map->dbm[j];
            map->dbm[j] = map->dbm[j - 1];
            map->dbm[j - 1] = t;
        }
}

int rdof_rssi_map_dbm(const rdof_rssi_map_t *map, int raw)
{
    const int n = points(map);
    int i = 0;
    while (i < n && map->raw[i] < raw) /* bisect_left on (raw, -inf) */
        i++;
    if (i <= 0)
        return map->dbm[0];
    if (i >= n)
        return map->dbm[n - 1];
    /* in 64 bits and doubles: the differences of ints at their ends overflow int */
    const int64_t r0 = map->raw[i - 1], r1 = map->raw[i];
    const int d0 = map->dbm[i - 1], d1 = map->dbm[i];
    const int64_t span = r1 - r0 > 1 ? r1 - r0 : 1;
    return (int)rint(d0 + ((double)d1 - d0) * (double)(raw - r0) / (double)span);
}

bool rdof_radio_rssi(const rdof_radio_t *r, int raw, uint32_t freq, int *dbm)
{
    if (raw < 0)
        return false;
    if (r->rssi_map.n) {
        *dbm = rdof_rssi_map_dbm(&r->rssi_map, raw);
        return true;
    }
    if (r->hotspot_rssi) {
        *dbm = rdof_radio_hotspot_dbm(raw, freq);
        return true;
    }
    return false;
}

static bool tune_split(rdof_radio_t *r, uint32_t rx, uint32_t tx, int power)
{
    if (r->tuner == RDOF_TUNER_MODEM)
        return rdof_modem_set_freq(r->modem, (uint32_t)((int64_t)rx + r->rx_offset_hz),
                                 (uint32_t)((int64_t)tx + r->tx_offset_hz), power);
    if (r->tuner == RDOF_TUNER_FIXED) {
        int64_t d = (int64_t)tx - r->fixed_hz;
        if (d > 100 || d < -100) {
            snprintf(r->modem->error, sizeof r->modem->error, "this radio is fixed on %.5f MHz", r->fixed_hz / 1e6);
            return false;
        }
        return true;
    }
    snprintf(r->modem->error, sizeof r->modem->error, "no tuner");
    return false;
}

bool rdof_radio_tune(rdof_radio_t *r, uint32_t freq, int cc, int power)
{
    if (!tune_split(r, freq, freq, power))
        return false;
    if (r->settle_ms && r->modem->port.sleep_ms)
        r->modem->port.sleep_ms(r->modem->port.ctx, r->settle_ms);
    return rdof_modem_configure(r->modem, &r->config, cc, false) && rdof_modem_set_mode(r->modem, RDOF_MODE_DMR);
}

bool rdof_radio_tune_duplex(rdof_radio_t *r, uint32_t rx, uint32_t tx, int cc, int power)
{
    if (!tune_split(r, rx, tx, power))
        return false;
    if (r->settle_ms && r->modem->port.sleep_ms)
        r->modem->port.sleep_ms(r->modem->port.ctx, r->settle_ms);
    return rdof_modem_configure(r->modem, &r->config, cc, true) && rdof_modem_set_mode(r->modem, RDOF_MODE_DMR);
}

bool rdof_radio_tune_dcdm(rdof_radio_t *r, uint32_t freq, int cc, int power)
{
    if (!tune_split(r, freq, freq, power))
        return false;
    if (r->settle_ms && r->modem->port.sleep_ms)
        r->modem->port.sleep_ms(r->modem->port.ctx, r->settle_ms);
    return configure(r->modem, &r->config, cc, false, RDOF_MMDVM_DCDM) && rdof_modem_set_mode(r->modem, RDOF_MODE_DMR);
}
