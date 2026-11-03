// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The SDR's settings, from the settings (version 3; docs/spec/fw/config-v3.md section 12; rdof/siteplan.h): the
 * SDR device's driver, rx keys and own keys, the frequencies its receiver hears (and the ones it hears for the
 * units on it), the networks (mqtt app "sdr", hblink), the top level's callsign, location and slot_timing.
 *
 * The device's own keys: driver (rtlsdr, lr2021, sx1255, sim; rdof/sdr/frontend.h), rx {rate, center_hz, gain_db,
 * agc, ppm, biastee, iq_bits}, usb {transfers, transfer_kb}, dsp_cores (1 or 2), spike (the USB throughput test at
 * boot), selftest (the CPU self-test at boot), sim (no dongle: the self-test clip in a loop), idle_gate (false: run
 * every channel's FM stage and sync search even when it holds only noise), learn_ppm (false: don't learn the
 * tuner's crystal error from the AFC), sim_ppm (with sim: a simulated tuner this many ppm fast), rssi_offset, afc,
 * status_seconds. The wifi and ntp keys aren't read here: the board's Wi-Fi and clock components keep those.
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
    bool has_gain;   /* false: the tuner's own AGC (the LR2021 has a fixed step unless gain_db is "auto") */
    double gain;
    bool agc;        /* our clip-driven control (rdof/sdr/agc.h) */
    double agc_cfg[6]; /* min, clip, headroom_db, hold_s, relax_s, settle_s */
    double ppm;
    bool biastee;
    int iq_bits;     /* rx.iq_bits, 8 or 16: the front end's sample width (LR2021); 0 its choice */
    int frontend;    /* rdof_fe_type_t: driver (rtlsdr by default; sim with "sim": true) */
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
    rdof_slot_timing_t slot_timing; /* the top level's slot_timing (docs/spec/slot-timing.md) */
    int nch;
    rdof_sdr_cfg_channel_t ch[RDOF_SDR_MAX_CHANNELS];
} rdof_sdr_config_t;

/* The SDR called device (a unit's SDR, or an SDR device a unit uses; NULL: the first a unit uses): false with error
   set ("devices.sdr0.rx.rate: ...", the place in the settings). rdof_sdr_config_parse is the first. */
bool rdof_sdr_config_parse(const char *text, size_t len, rdof_sdr_config_t *cfg, char *error, size_t cap);
bool rdof_sdr_config_parse_device(const char *text, size_t len, const char *device, rdof_sdr_config_t *cfg,
                                  char *error, size_t cap);
/* The same from a plan (rdof/siteplan.h). */
struct rdof_plan;
bool rdof_plan_sdr(const struct rdof_plan *p, const char *name, rdof_sdr_config_t *cfg, char *error, size_t cap);
/* The RSSI offset in dB, added to a burst's dBFS: the setting, else by the driver. The LR2021: the calibration
   less its gain step's dB (rdof/sdr/frontend.h; the default step's when it's auto). The others: -gain - 25 (gain 30
   when it's auto). */
double rdof_sdr_config_rssi_offset(const rdof_sdr_config_t *cfg);
/* One line per group, for the web page and the golden tests. */
size_t rdof_sdr_config_describe(const rdof_sdr_config_t *cfg, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
