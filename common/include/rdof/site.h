// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The settings, version 3 (docs/spec/fw/config-v3.md): devices, frequencies, apps, networks and the call policies
 * on each talk path. Its JSON Schema is config/rdof-config.schema.json.
 *
 *   "devices":     [{"kind": "sdr" | "modem" | "radio", "hears", "transmits", ...}]
 *   "frequencies": [438.0625, {"freq", "name", "cc", "mode", "ts", "tx", "slots", policy keys}, ...]
 *   "apps":        [{"type": "repeater" | "parrot" | "ipgw", ...}]
 *   "networks":    [{"type": "hblink" | "brandmeister" | "mqtt", ...}]
 *   "policies":    [{"name", "tg", "ptt", "ptt_hold_s", "pc", "apps", "net", "dir"}]
 *
 * rdof_site_parse reads it into rdof_site_t: every reference resolved, every talk path with its policy filled in
 * (the path's own keys, then its named policy, then defaults, then the table). The router (rdof/router.h) decides
 * where calls go from it.
 *
 * What runs on which device (the run units) and each part's own settings are rdof/siteplan.h's, on this parse.
 *
 * Portable C. rdof_site_t is large (about 40 KB): allocate it.
 */
#ifndef RDOF_SITE_H
#define RDOF_SITE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/idset.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_SITE_NAME 40
#define RDOF_SITE_MAX_FREQS 48
#define RDOF_SITE_MAX_DEVICES 16
#define RDOF_SITE_MAX_APPS 16
#define RDOF_SITE_MAX_NETS 4
#define RDOF_SITE_MAX_POLICIES 16
#define RDOF_SITE_MAX_PATHS 64

typedef enum { RDOF_FMODE_SIMPLEX = 0, RDOF_FMODE_SFR, RDOF_FMODE_DCDM } rdof_fmode_t;
typedef enum { RDOF_FLOW_RF_NET = 1, RDOF_FLOW_NET_RF = 2, RDOF_FLOW_BOTH = 3 } rdof_flow_t;
typedef enum { RDOF_HEARS_WINDOW = 0, RDOF_HEARS_IDLE, RDOF_HEARS_SCAN, RDOF_HEARS_NONE } rdof_hears_t;
typedef enum { RDOF_APP_REPEATER = 1, RDOF_APP_PARROT, RDOF_APP_IPGW } rdof_app_type_t;
typedef enum { RDOF_NET_HBLINK = 1, RDOF_NET_MQTT } rdof_net_type_t;
enum { RDOF_MQTT_FORMAT_DMRD = 0, RDOF_MQTT_FORMAT_V3 = 1 };
enum { RDOF_MQTT_TCP = 0, RDOF_MQTT_TLS = 1 };

/* A policy's keys, in this order (for src[] below). */
enum { RDOF_PK_TG, RDOF_PK_PTT, RDOF_PK_PTT_HOLD, RDOF_PK_PC, RDOF_PK_APPS, RDOF_PK_NET, RDOF_PK_DIR, RDOF_PK_N };
/* Where a path's key came from: its own keys, defaults, the table; >= 0: that named policy. */
enum { RDOF_SRC_OWN = -1, RDOF_SRC_DEFAULTS = -2, RDOF_SRC_TABLE = -3 };

typedef struct {
    rdof_idset_t tg, ptt, pc;
    double ptt_hold_s;
    uint32_t apps;  /* the apps it may reach: bit k for app k */
    int net;        /* the network: an index in net[] (-1: none, the site has no network) */
    uint8_t dir;    /* rdof_flow_t */
    int8_t src[RDOF_PK_N];
} rdof_policy_t;

typedef struct {
    char name[RDOF_SITE_NAME];
    uint32_t hz;
    int cc;          /* -1: any (a receive-only frequency without cc) */
    uint8_t mode;    /* rdof_fmode_t */
    uint8_t ts;      /* the slot a simplex transmission uses */
    bool tx;         /* something may transmit here */
    double power_dbm; /* at the antenna; NAN: the device's */
    uint32_t devices; /* the devices that may serve it (bit per device); 0: any */
    int app;         /* the app that takes it (a repeater's in or out), -1 none */
} rdof_freq_t;

typedef struct {
    char name[RDOF_SITE_NAME];
    int kind;        /* rdof_dev_kind_t (rdof/siteplan.h): 1 sdr, 2 modem, 3 radio */
    char driver[16];
    uint8_t hears;   /* rdof_hears_t */
    int idle;        /* RDOF_HEARS_IDLE / SCAN: the frequency it waits on, -1 none */
    uint64_t scan;   /* RDOF_HEARS_SCAN: the frequencies it scans (bit per frequency) */
    bool can_tx;     /* it can transmit: transmits isn't false, and (an SDR) its driver transmits */
    uint64_t transmits; /* the frequencies it may transmit on (bit per frequency) */
} rdof_device3_t;

typedef struct {
    char name[RDOF_SITE_NAME];
    uint8_t type;    /* rdof_app_type_t */
    int in, out;     /* a repeater: its frequencies */
    int path[2];     /* a repeater: its slots' paths */
    rdof_idset_t tg, pc; /* a parrot's, an IP gateway's: the IDs it answers */
} rdof_app3_t;

typedef struct {
    char name[RDOF_SITE_NAME];
    uint8_t type;    /* rdof_net_type_t */
    bool per_path;   /* hblink identity per_path: each path (a repeater: both its slots) its own peer */
    bool roaming;    /* hblink: the master may move a device (DMRT) */
    bool login;      /* type brandmeister or homebrew: an hblink network whose peers log in (RPTL,
                        docs/spec/fw/brandmeister.md) */
    bool homebrew;   /* a login written as type homebrew: any master that speaks the Homebrew repeater protocol
                        (a stock HBlink, TGIF, ...); the same login, named for what it is */
    bool report;     /* mqtt */
    bool calls;      /* mqtt: carries calls */
    uint8_t format;  /* mqtt: RDOF_MQTT_FORMAT_* (calls as DMRD, or the MQTT network's messages) */
    uint8_t bundle;  /* mqtt, format v3: voice items per message when sending (1-3; 1 is sent for now) */
    bool raw_tx;     /* mqtt, format v3: transmit raw items as heard */
    uint8_t transport; /* mqtt: RDOF_MQTT_TCP / RDOF_MQTT_TLS (tls true, or transport mqtt/tls) */
} rdof_net3_t;

typedef struct {
    char name[RDOF_SITE_NAME + 4];
    int freq;        /* the frequency it's heard on (a repeater: its in) */
    int tx_freq;     /* the frequency it transmits on (a repeater: its out), -1: receive only */
    int app;         /* the repeater it's a slot of, -1 none */
    uint8_t slot;    /* 1 or 2; 0: both (a simplex, SFR or receive-only frequency) */
    uint8_t tx_slot; /* the slot it transmits on: slot, else the frequency's ts */
    uint32_t peer_id; /* its HBlink peer: the site's id, or (identity per_path) its own */
    char peer_name[9]; /* identity per_path: that peer's name for the master (its frequency's or repeater's, 8 characters) */
    rdof_policy_t pol;
} rdof_path_t;

typedef struct {
    char name[32];
    uint32_t id;
    double pc_hold_s;
    int nfreq, ndev, napp, nnet, npol, npath;
    rdof_freq_t freq[RDOF_SITE_MAX_FREQS];
    rdof_device3_t dev[RDOF_SITE_MAX_DEVICES];
    rdof_app3_t app[RDOF_SITE_MAX_APPS];
    rdof_net3_t net[RDOF_SITE_MAX_NETS];
    char pol_name[RDOF_SITE_MAX_POLICIES][RDOF_SITE_NAME];
    rdof_path_t path[RDOF_SITE_MAX_PATHS];
} rdof_site_t;

/* Version 3 settings into s. false with err set: "path: message", path as frequencies[3].tg or apps.R1.in. */
bool rdof_site_parse(const char *text, size_t len, rdof_site_t *s, char *err, size_t cap);

/* By name; a frequency also by its frequency (439, "439.0"). -1: none. */
int rdof_site_freq(const rdof_site_t *s, const char *ref);
int rdof_site_freq_hz(const rdof_site_t *s, uint32_t hz);
int rdof_site_path(const rdof_site_t *s, const char *name);
int rdof_site_net(const rdof_site_t *s, const char *name);
int rdof_site_app(const rdof_site_t *s, const char *name);
/* The path a call heard at hz on slot (1, 2) belongs to: the frequency's, its slot's, or a repeater's slot when hz
   is a repeater's in. -1: none. */
int rdof_site_path_rx(const rdof_site_t *s, uint32_t hz, int slot);
/* The apps of a type the site has: the first, -1 none. */
int rdof_site_app_of(const rdof_site_t *s, int type);

/* The resolved view (docs/spec/fw/config-v3.md section 11) as JSON: the frequencies, the paths and their policies
   (each key's source), the reverse talkgroup view, the networks. Returns the length (out NUL terminated, cut at
   cap). */
size_t rdof_site_resolved(const rdof_site_t *s, char *out, size_t cap);
/* The same, indented for reading (the settings page's resolved view). */
size_t rdof_site_resolved_text(const rdof_site_t *s, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
