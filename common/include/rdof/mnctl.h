// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The MQTT network's control documents (docs/spec/fw/mqtt-net.md sections 3.3, 3.6, 6, 15-17): the JSON around the calls.
 *
 *   Registration records, {root}/node/{node}/ms/{id} (retained): a radio a trunked site serves.
 *   Node status, {root}/node/{node}/status (retained), with a trunked site's part.
 *   The trunk object of info.
 *   Provisioning from the manager, {root}/cfg/... (retained): net, node, tg, ms documents, each with a rev; held in
 *   rdof_mn_cfg_t, newest rev wins, cached in a file of lines so a box starts without the network.
 *   Commands, {root}/node/{node}/cmd, and their results on .../cmd/res.
 *   The router's where/{id}, as a box reads it.
 *
 * Portable C, no allocation, no locking. rdof_mn_cfg_t is about 90 KB: on a board, put it in PSRAM.
 */
#ifndef RDOF_MNCTL_H
#define RDOF_MNCTL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/idset.h"
#include "rdof/mqttnet.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── time ─────────────────────────────────────────────────────────────────── */

/* "2026-11-05T21:04:11.250Z" (or without the fraction) to ms since the Unix epoch. False: not that form. */
bool rdof_mn_iso_parse(const char *s, int64_t *ms);

/* ── topics ──────────────────────────────────────────────────────────────── */

/* {root}/node/{node}/ms/{id} */
size_t rdof_mn_ms_topic(char *out, size_t cap, const char *root, const char *node, uint32_t id);
/* A registration record's topic: its node and radio. False: not one. */
bool rdof_mn_ms_topic_parse(const char *topic, const char *root, char *node, size_t cap, uint32_t *id);
/* {root}/where/{id} and back */
size_t rdof_mn_where_topic(char *out, size_t cap, const char *root, uint32_t id);
bool rdof_mn_where_topic_parse(const char *topic, const char *root, uint32_t *id);

/* ── registration records ────────────────────────────────────────────────── */

#define RDOF_MN_REC_TG 8

typedef struct {
    uint32_t id;
    int64_t at_ms, exp_ms; /* registered or refreshed; expires */
    bool implicit;         /* "how": "call" (from a call request); else "register" */
    uint32_t tg[RDOF_MN_REC_TG];
    int ntg;
    uint32_t sub; /* the subscribed talkgroup, 0: none */
    int area;     /* the registration area (SYS_AREA); -1: not given */
} rdof_mn_record_t;

size_t rdof_mn_record_json(const rdof_mn_record_t *r, char *out, size_t cap);
/* False with err: not a record (an empty payload is a deletion: the caller checks for it first). */
bool rdof_mn_record_parse(const char *json, size_t len, rdof_mn_record_t *r, char *err, size_t cap);

/* ── where/{id}, from the router ─────────────────────────────────────────── */

typedef struct {
    char node[RDOF_MN_NODE_MAX + 1];
    int path; /* -1: not given */
    int64_t at_ms;
    bool reg; /* "source": "reg" (a registration record), else heard traffic */
} rdof_mn_where_t;

bool rdof_mn_where_parse(const char *json, size_t len, rdof_mn_where_t *w, char *err, size_t cap);

/* ── status and the trunk object of info ─────────────────────────────────── */

typedef struct {
    int chan, lcn;
    const char *use; /* "tscc", "call", "hang", "idle", "off" */
    uint32_t src, dst;
    bool group;
    int64_t since_ms; /* 0: not given */
} rdof_mn_chan_status_t;

typedef struct {
    bool connected;
    int regs, queue;
    const rdof_mn_chan_status_t *ch;
    int nch;
    uint32_t aloha, rand, collisions, grants, refused, queued;
} rdof_mn_trunk_status_t;

typedef struct {
    const char *node, *firmware;
    double wall, uptime_s;
    const rdof_mn_trunk_status_t *trunk; /* NULL: not a trunked site */
} rdof_mn_status_t;

size_t rdof_mn_status_json(const rdof_mn_status_t *s, char *out, size_t cap);

typedef struct {
    int chan;
    uint32_t tx_hz, rx_hz;
    int cc; /* -1: not given */
} rdof_mn_carrier_t;

typedef struct {
    const char *model; /* "tiny", "small", "large", "huge" */
    int net, site, area_bits;
    const char *par; /* "a", "b", "ab" */
    const rdof_mn_carrier_t *tscc;
    int ntscc;
    const rdof_mn_carrier_t *carriers;
    int ncarriers;
    const char *const *services;
    int nservices;
    bool reg; /* registration required */
} rdof_mn_trunk_info_t;

/* The object for info's "trunk" (rdof_mn_info_t.trunk). */
size_t rdof_mn_trunk_info_json(const rdof_mn_trunk_info_t *t, char *out, size_t cap);

/* ── provisioning: cfg/net, cfg/node/{node}, cfg/tg/{tg}, cfg/ms/{id} ─────── */

#define RDOF_MN_CFG_TIMERS 16
#define RDOF_MN_CFG_BROADCASTS 4
#define RDOF_MN_CFG_NEIGHBORS 32
#define RDOF_MN_CFG_TG 64
#define RDOF_MN_CFG_MS 512
#define RDOF_MN_CFG_NAME 32

/* rdof_mn_site_cfg_t.has bits: which values a document (or the settings) gave */
enum {
    RDOF_MN_HAS_MODEL = 1u << 0,
    RDOF_MN_HAS_NET = 1u << 1,
    RDOF_MN_HAS_SITE = 1u << 2,
    RDOF_MN_HAS_PAR = 1u << 3,
    RDOF_MN_HAS_AREA_BITS = 1u << 4,
    RDOF_MN_HAS_REGISTRATION = 1u << 5,
    RDOF_MN_HAS_MS = 1u << 6,
    RDOF_MN_HAS_REFUSE = 1u << 7,
    RDOF_MN_HAS_PRIVATE = 1u << 8,
    RDOF_MN_HAS_TG = 1u << 9,
    RDOF_MN_HAS_WIDE_TG = 1u << 10,
    RDOF_MN_HAS_STATIC_TG = 1u << 11,
    RDOF_MN_HAS_REQUIRE_REG = 1u << 12,
    RDOF_MN_HAS_FOACSU = 1u << 13,
    RDOF_MN_HAS_ENABLED = 1u << 14,
    RDOF_MN_HAS_NEIGHBORS = 1u << 15,
    RDOF_MN_HAS_NAME = 1u << 16,
};

enum { RDOF_MN_MODEL_TINY = 0, RDOF_MN_MODEL_SMALL, RDOF_MN_MODEL_LARGE, RDOF_MN_MODEL_HUGE };
enum { RDOF_MN_PAR_A = 1, RDOF_MN_PAR_B = 2, RDOF_MN_PAR_AB = 3 };

typedef struct {
    char name[20];
    double value;
} rdof_mn_timer_t;

/* What a trunked site runs with: the settings file's values, then cfg/net's, then cfg/node's, key by key. */
typedef struct {
    uint32_t has;
    char name[RDOF_MN_CFG_NAME]; /* a name for people (the manager, dashboards); the site doesn't act on it */
    int model, net, site, par, area_bits;
    bool listed;  /* registration "listed" (else "open") */
    bool refused; /* refuse_unlisted "refused" (else "denied") */
    bool require_registration, foacsu, enabled;
    rdof_idset_t ms, private_, tg, wide_tg, static_tg;
    int neighbors[RDOF_MN_CFG_NEIGHBORS];
    int nneighbors;
    rdof_mn_timer_t timers[RDOF_MN_CFG_TIMERS]; /* by name; a later layer's value replaces an earlier one's */
    int ntimers;
    /* the control channel's broadcast intervals (trunk.md 3.2: adjacent_s, chan_freq_s, local_time_s; 0 off), by name
       as the timers */
    rdof_mn_timer_t broadcast[RDOF_MN_CFG_BROADCASTS];
    int nbroadcast;
} rdof_mn_site_cfg_t;

typedef struct {
    uint32_t tg;
    long rev;
    char name[RDOF_MN_CFG_NAME];
    bool has_wide, wide;
    rdof_idset_t sites, ms; /* "*" when not given */
    int priority;
    double hang_s; /* 0: not given */
} rdof_mn_tg_cfg_t;

typedef struct {
    uint32_t id;
    long rev;
    char name[RDOF_MN_CFG_NAME];
    bool enabled, private_, wide_private, emergency; /* true when not given */
    int priority;
    uint32_t tg[RDOF_MN_REC_TG]; /* static talkgroups for this radio */
    int ntg;
} rdof_mn_ms_cfg_t;

typedef struct {
    char node[RDOF_MN_NODE_MAX + 1];
    long net_rev, node_rev; /* 0: no document */
    rdof_mn_site_cfg_t net, nodecfg;
    int ntg, nms;
    rdof_mn_tg_cfg_t tg[RDOF_MN_CFG_TG];
    rdof_mn_ms_cfg_t ms[RDOF_MN_CFG_MS];
    uint32_t changes; /* counts each document applied or deleted */
} rdof_mn_cfg_t;

typedef enum {
    RDOF_MN_CFG_NOT_CFG = 0, /* not a cfg topic under root, or another node's */
    RDOF_MN_CFG_APPLIED,
    RDOF_MN_CFG_DELETED,
    RDOF_MN_CFG_OLD,  /* its rev isn't newer than the one held */
    RDOF_MN_CFG_BAD,  /* malformed (err says why); nothing changed */
    RDOF_MN_CFG_FULL, /* no room for another tg or ms document */
} rdof_mn_cfg_result_t;

void rdof_mn_cfg_init(rdof_mn_cfg_t *c, const char *node);
/* {root}/cfg/net, {root}/cfg/node/{node}, {root}/cfg/tg/{id}, {root}/cfg/ms/{id} (kind "net", "node", "tg", "ms";
   node and id where they apply). */
size_t rdof_mn_cfg_topic(char *out, size_t cap, const char *root, const char *kind, const char *node, uint32_t id);
/* A retained message on a cfg topic. An empty payload deletes the document. note (may be NULL) gets the keys that
   were ignored ("ignored: frequencies, carriers"), "" when none. */
rdof_mn_cfg_result_t rdof_mn_cfg_message(rdof_mn_cfg_t *c, const char *root, const char *topic, const char *payload,
                                         size_t len, char *note, size_t cap);
/* The values that apply: base (the settings file's, may be NULL) under cfg/net under cfg/node/{node}. */
void rdof_mn_cfg_site(const rdof_mn_cfg_t *c, const rdof_mn_site_cfg_t *base, rdof_mn_site_cfg_t *out);
/* A timer's value in a resolved site config, or dflt. */
double rdof_mn_cfg_timer(const rdof_mn_site_cfg_t *s, const char *name, double dflt);
/* A broadcast interval (adjacent_s, chan_freq_s, local_time_s) in a resolved site config, or dflt. */
double rdof_mn_cfg_broadcast(const rdof_mn_site_cfg_t *s, const char *name, double dflt);
const rdof_mn_tg_cfg_t *rdof_mn_cfg_tg(const rdof_mn_cfg_t *c, uint32_t tg);
const rdof_mn_ms_cfg_t *rdof_mn_cfg_ms(const rdof_mn_cfg_t *c, uint32_t id);
/* A site-level document (cfg/net or cfg/node) from its JSON: what rdof_mn_cfg_message does for those two, for the
   settings file's trunk app too. False with err. */
bool rdof_mn_site_cfg_parse(const char *json, size_t len, rdof_mn_site_cfg_t *out, long *rev, char *note, size_t ncap,
                            char *err, size_t cap);

/* The cache: one line per document, "cfg/net\t{json}\n" (the topic under root, a tab, the document as it was
   applied). rdof_mn_cfg_cache writes them through line (each without its newline); rdof_mn_cfg_cache_line takes
   one back (the same rules as a message: an older rev than one held is ignored). */
void rdof_mn_cfg_cache(const rdof_mn_cfg_t *c, void (*line)(void *ctx, const char *text, size_t len), void *ctx);
rdof_mn_cfg_result_t rdof_mn_cfg_cache_line(rdof_mn_cfg_t *c, const char *text, size_t len);

/* ── commands ────────────────────────────────────────────────────────────── */

typedef enum {
    RDOF_MN_CMD_UNKNOWN = 0,
    RDOF_MN_CMD_DEREGISTER, /* ms */
    RDOF_MN_CMD_CLEAR,      /* chan, lcn */
    RDOF_MN_CMD_WITHDRAW,
    RDOF_MN_CMD_RESTORE,
    RDOF_MN_CMD_RESEND_RECORDS,
    RDOF_MN_CMD_STATUS,
} rdof_mn_cmd_kind_t;

typedef struct {
    char id[40];
    rdof_mn_cmd_kind_t cmd;
    char name[24];
    uint32_t ms;
    int chan, lcn;
} rdof_mn_cmd_t;

/* False with err when it can't be carried out (unknown command, a missing argument); c->id is filled whenever the
   message had one, so the result can name it. */
bool rdof_mn_cmd_parse(const char *json, size_t len, rdof_mn_cmd_t *c, char *err, size_t cap);
/* {"id":...,"ok":true} or {"id":...,"ok":false,"error":...} */
size_t rdof_mn_cmd_result_json(const char *id, bool ok, const char *error, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
