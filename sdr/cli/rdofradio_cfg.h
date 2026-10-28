// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * rdofradio's settings (--config FILE): the firmware's settings document (docs/spec/fw/config.md), read by the
 * firmware's parser (rdof_config_parse), and the part of it rdofradio applies. The radio is a repeater (type
 * repeater, or sdr_transceiver with duplex full) or a half duplex transceiver (sdr_transceiver, duplex half, on
 * its idle channel). From it: the name, peer_id, rx_freq / tx_freq, cc, parrot, and a repeater's slots, hang_s,
 * call_hang_s, short_lc_ts1_low, prebuffer and the jitter buffer, cwid and cwid_every_s. mode sfr and dcdm are
 * refused; every other key is kept in the file and listed as not used here. The SDR device stays on the command
 * line (--soapy, -r, --gain, --tx-gain ...). No I/O here: rdofradio reads and writes the file.
 */
#ifndef RDOFRADIO_CFG_H
#define RDOFRADIO_CFG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/config.h"
#include "rdof/repeater.h"

typedef struct {
    bool full;          /* a repeater (full duplex); false: a transceiver (half duplex) */
    char box[32];       /* the box's name */
    char name[24];      /* the radio's */
    uint32_t peer_id;
    uint32_t rx_hz, tx_hz;
    int cc;
    bool has_parrot;
    uint32_t parrot;    /* 0: everything */
    char unused[320];   /* the keys set that rdofradio doesn't apply, "hblink, mqtt", or "" */
    rdof_config_t cfg;  /* all of it, as the firmware resolves it */
} rdofradio_cfg_t;

/* Read a settings document. False: err says why (the parser's reason, or what rdofradio can't run). */
bool rdofradio_cfg_parse(const char *text, size_t len, rdofradio_cfg_t *out, char *err, size_t err_cap);
/* The settings document for what the command line set (shown read only on the page, a start for a file). */
size_t rdofradio_cfg_from_flags(char *out, size_t cap, bool full, uint32_t rx_hz, uint32_t tx_hz, int cc,
                                bool has_parrot, uint32_t parrot, uint32_t peer_id);
/* A repeater's settings past rdof_repeater_init's (slots, hangs, jitter buffer, CW ID), as the firmware sets them. */
void rdofradio_cfg_apply_repeater(const rdofradio_cfg_t *c, rdof_repeater_t *r);
/* /api/resolved: the radio as resolved, what's not used, and device (the command line's device flags). */
size_t rdofradio_cfg_describe(const rdofradio_cfg_t *c, const char *source, const char *device, char *out,
                              size_t cap);

#endif
