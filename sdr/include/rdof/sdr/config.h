// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * An SDR board's settings: a plain SDR config in JSON (docs/spec/fw/config.md), resolved:
 * frequencies in MHz or Hz, channel defaults, peer_id_base, duplicate
 * frequencies refused, rssi_offset from the gain. Or a box config (docs/spec/fw/config.md;
 * settings with "radios"): its SDR section (the one "run" names, else the
 * first) resolved with the box's channel plan,
 * mqtt (app "sdr"), hblink and location.
 *
 * Board-only keys (in a box config, in the SDR section): usb {transfers,
 * transfer_kb}, dsp_cores (1 or 2), spike (run the USB throughput test at boot), selftest (run
 * the CPU self-test at boot), sim (no dongle: the self-test clip in a loop), idle_gate (false: run every
 * channel's FM stage and sync search even when it holds only noise), learn_ppm (false: don't learn the
 * tuner's crystal error from the AFC), sim_ppm (with sim: a simulated tuner this many ppm fast).
 * device.settings.biastee turns on the bias tee; device.settings.iq_bits (8 or 16) picks the LR2021's sample width. device.type picks the front end (rtlsdr, lr2021, sim;
 * rdof/sdr/frontend.h). Without a name the
 * board is "sdr". The board's wifi and ntp keys
 * aren't read here: its Wi-Fi and clock components keep those.
 * JSON with // and block comments and trailing commas is taken.
 */
#ifndef RDOF_SDR_CONFIG_H
#define RDOF_SDR_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/hblink.h"
#include "rdof/slotgrid.h"
#include "rdof/sdr/features.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_SDR_MAX_CHANNELS 16

typedef struct {
    char id[24], name[48];
    double freq;
    int cc;          /* -1: none (any) */
    int slot;
    long tg;         /* -1: none */
    bool hblink;
    long peer_id;    /* -1: none */
} rdof_sdr_cfg_channel_t;

typedef struct {
    char name[32];
    /* device */
    double sample_rate;
    double center;   /* 0: auto */
    bool has_gain;   /* false: the tuner's own AGC */
    double gain;
    bool agc;        /* our clip-driven control (rdof/sdr/agc.h) */
    double agc_cfg[6]; /* min, clip, headroom_db, hold_s, relax_s, settle_s */
    double ppm;
    bool biastee;
    int iq_bits;     /* device.settings.iq_bits, 8 or 16: the front end's sample width (LR2021); 0 its choice */
    int frontend;    /* rdof_fe_type_t: device.type (rtlsdr by default; sim with "sim": true) */
    /* outputs */
    bool has_mqtt;
    char mqtt_host[64], mqtt_user[32], mqtt_pass[64], mqtt_root[16], mqtt_app[16];
    int mqtt_port;
    bool mqtt_tls;
    bool hb_enabled;
    char hb_host[64], hb_callsign[12];
    int hb_port;
    bool has_location;
    rdof_location_t location;
    bool has_rssi_offset;
    double rssi_offset;
    bool afc;
    double status_seconds;
    int usb_transfers, usb_transfer_kb;
    int dsp_cores;   /* 1, or 2 to split the channel demods over both cores */
    bool spike, selftest;
    bool sim;        /* no dongle: the self-test clip, looped in real time, stands in for the RTL-SDR */
    bool idle_gate;  /* skip the FM stage and search on channels holding only noise (default on) */
    bool learn_ppm;  /* learn the tuner's crystal error from the AFC and keep it (default on) */
    double sim_ppm;  /* with sim: the simulated tuner's crystal error, for trying learn_ppm */
    rdof_slot_timing_t slot_timing; /* slot_timing (a box's: its own over the box's; docs/spec/slot-timing.md) */
    int nch;
    rdof_sdr_cfg_channel_t ch[RDOF_SDR_MAX_CHANNELS];
} rdof_sdr_config_t;

/* Parses and resolves. false with error set when the config is refused. */
bool rdof_sdr_config_parse(const char *text, size_t len, rdof_sdr_config_t *cfg, char *error, size_t cap);
/* The RSSI offset in dB: the setting, else -gain - 25 (gain 30 when it's auto). */
double rdof_sdr_config_rssi_offset(const rdof_sdr_config_t *cfg);
/* One line per group, for the web page and the golden tests. */
size_t rdof_sdr_config_describe(const rdof_sdr_config_t *cfg, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
