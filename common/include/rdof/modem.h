// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * One MMDVM modem as a client, with no threads of its own. The platform gives
 * it a port (write bytes, wait for a reply, a clock) and feeds it what the
 * port reads (rdof_modem_feed, from its reader task); requests then block in
 * the caller until the reply comes or the wait times out.
 *
 * Every request returns false when it fails; rdof_modem_t.error says why.
 */
#ifndef RDOF_MODEM_H
#define RDOF_MODEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/fec.h"
#include "rdof/mmdvm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Write a whole frame; false if the port is gone. */
    bool (*write)(void *ctx, const uint8_t *data, size_t len);
    /* Block until rdof_modem's reply_ready is set (signal it from on_reply) or timeout_ms passes. True if it came. */
    bool (*wait_reply)(void *ctx, int timeout_ms);
    /* Called from rdof_modem_feed when the awaited reply is in (e.g. give a semaphore). May be NULL. */
    void (*on_reply)(void *ctx);
    double (*clock)(void *ctx); /* monotonic seconds */
    void (*sleep_ms)(void *ctx, int ms);
    void *ctx;
} rdof_modem_port_t;

typedef struct rdof_modem {
    rdof_modem_port_t port;
    int protocol;
    char version[128];
    double last_rx;     /* clock() of the last DMR_DATA / DMR_LOST, -inf before any */
    char failed[64];    /* non-empty: the port failed; every request fails now */
    char error[96];     /* why the last request failed */
    int timeout_ms;
    uint32_t requests, no_reply, refused; /* diagnostics */
    /* the one outstanding request */
    volatile int waiting_for;
    volatile bool reply_ready;
    bool reply_ok;
    uint8_t reply[256];
    size_t reply_len;
    rdof_mmdvm_reader_t reader;
    /* What the modem hears (reader context): slot, control byte, 33 bytes, raw RSSI or -1. */
    void (*on_rx)(void *ctx, int slot, uint8_t control, const uint8_t *burst, int rssi);
    void (*on_lost)(void *ctx, int slot);
    /* An unanswered command (DMR data ...) NAKed. */
    void (*on_nak)(void *ctx, uint8_t command, int err);
    void *cb_ctx;
} rdof_modem_t;

void rdof_modem_init(rdof_modem_t *m, const rdof_modem_port_t *port);
/* Bytes the port read. */
void rdof_modem_feed(rdof_modem_t *m, const uint8_t *data, size_t len);
/* Mark the port failed (unplugged): requests fail from now on. why NULL: "". */
void rdof_modem_fail(rdof_modem_t *m, const char *why);

/* GET_VERSION, up to `tries` times: sets protocol and version. */
bool rdof_modem_start(rdof_modem_t *m, int tries);
bool rdof_modem_status(rdof_modem_t *m, rdof_mmdvm_status_t *out);
bool rdof_modem_configure(rdof_modem_t *m, const rdof_modem_config_t *c, int cc, bool duplex);
bool rdof_modem_set_freq(rdof_modem_t *m, uint32_t rx_hz, uint32_t tx_hz, int rf_level);
bool rdof_modem_set_mode(rdof_modem_t *m, int mode);
bool rdof_modem_send_cwid(rdof_modem_t *m, const char *text);
/* Not waited on. */
bool rdof_modem_write_dmr(rdof_modem_t *m, uint8_t control, const uint8_t burst[RDOF_BURST_BYTES], int slot);
bool rdof_modem_dmr_start(rdof_modem_t *m, bool on);
bool rdof_modem_dmr_abort(rdof_modem_t *m, int slot);
bool rdof_modem_dmr_short_lc(rdof_modem_t *m, const uint8_t lc9[9]);

/* ── the radio ───────────────────────────────────────────────────────────── */

typedef enum { RDOF_TUNER_MODEM = 0, RDOF_TUNER_FIXED, RDOF_TUNER_NONE } rdof_tuner_kind_t;

#define RDOF_RSSI_POINTS 16

typedef struct {
    int n; /* 0: no map */
    int raw[RDOF_RSSI_POINTS], dbm[RDOF_RSSI_POINTS];
} rdof_rssi_map_t;

typedef struct {
    rdof_modem_t *modem;
    rdof_radio_kind_t kind;
    rdof_modem_config_t config;
    rdof_tuner_kind_t tuner;
    int32_t rx_offset_hz, tx_offset_hz; /* ModemTuner */
    uint32_t fixed_hz;                  /* FixedTuner */
    rdof_rssi_map_t rssi_map;
    bool hotspot_rssi;                  /* no map given and a radio_hotspot: the AT1846S conversion */
    int headers, terminators;
    int settle_ms;
} rdof_radio_t;

/* Defaults for a kind (Profile: headers / terminators; the modem's own tuner, RSSI for an OpenGD77). */
void rdof_radio_init(rdof_radio_t *r, rdof_modem_t *modem, rdof_radio_kind_t kind);
/* Sort the map's points (call after filling rssi_map). */
void rdof_rssi_map_sort(rdof_rssi_map_t *map);
int rdof_rssi_map_dbm(const rdof_rssi_map_t *map, int raw);
bool rdof_radio_tune(rdof_radio_t *r, uint32_t freq, int cc, int power);
bool rdof_radio_tune_duplex(rdof_radio_t *r, uint32_t rx, uint32_t tx, int cc, int power);
/* Simplex on freq with RDOF_MMDVM_DCDM: both slots, direct mode (a modem that rdof_dcdm_capable says takes it). */
bool rdof_radio_tune_dcdm(rdof_radio_t *r, uint32_t freq, int cc, int power);
/* dBm for a raw RSSI (-1 = none); false when there's no conversion. */
bool rdof_radio_rssi(const rdof_radio_t *r, int raw, uint32_t freq, int *dbm);

#ifdef __cplusplus
}
#endif

#endif
